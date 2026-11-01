/*
 * ThreadStorm - server.c
 *
 * HTTP server: Winsock lifecycle, accept loop, request parsing,
 * static-file serving, and internal API endpoints.
 *
 * Internal API endpoints (all return JSON):
 *   GET /api/metrics          – server + pool metrics snapshot
 *   GET /api/events           – recent request events
 *   GET /api/security         – security events
 *   GET /api/rps              – RPS history array
 *   GET /api/workers          – per-worker state array
 *   POST /api/sync/race       – run race condition experiment
 *   POST /api/sync/deadlock   – run deadlock prevention demo
 *   POST /api/bench/run       – run benchmark
 *   POST /api/adaptive/toggle – toggle adaptive pool
 */

#include "server.h"
#include "sync_lab.h"
#include "benchmark.h"

/* ── Global server pointer (used by worker threads) ──────────────────── */
Server *g_server = NULL;

/* ── Default config ────────────────────────────────────────────────────── */
void server_default_config(ServerConfig *cfg) {
    cfg->port           = SERVER_PORT;
    cfg->worker_count   = POOL_DEFAULT_WORKERS;
    cfg->queue_capacity = QUEUE_DEFAULT_CAPACITY;
    cfg->adaptive_pool  = false;
    ts_strlcpy(cfg->public_dir, PUBLIC_DIR, sizeof(cfg->public_dir));
}

/* ── MIME types ─────────────────────────────────────────────────────────── */
const char *mime_from_ext(const char *path) {
    const char *ext = strrchr(path, '.');
    if (!ext) return "application/octet-stream";
    if (_stricmp(ext, ".html") == 0) return "text/html; charset=utf-8";
    if (_stricmp(ext, ".css")  == 0) return "text/css; charset=utf-8";
    if (_stricmp(ext, ".js")   == 0) return "application/javascript; charset=utf-8";
    if (_stricmp(ext, ".json") == 0) return "application/json";
    if (_stricmp(ext, ".png")  == 0) return "image/png";
    if (_stricmp(ext, ".jpg")  == 0 || _stricmp(ext, ".jpeg") == 0)
        return "image/jpeg";
    if (_stricmp(ext, ".svg")  == 0) return "image/svg+xml";
    if (_stricmp(ext, ".ico")  == 0) return "image/x-icon";
    if (_stricmp(ext, ".txt")  == 0) return "text/plain; charset=utf-8";
    if (_stricmp(ext, ".csv")  == 0) return "text/csv; charset=utf-8";
    return "application/octet-stream";
}

/* ── Path safety check ──────────────────────────────────────────────────── */
bool path_is_safe(const char *uri, char *resolved_path, int resolved_len,
                  const char *public_dir) {
    /* Reject if URI contains ".." sequences */
    if (strstr(uri, "..")) return false;
    /* Reject backslashes (Windows traversal) */
    if (strchr(uri, '\\')) return false;
    /* Reject NULL bytes */
    for (int i = 0; uri[i]; i++) {
        if ((unsigned char)uri[i] < 0x20 && uri[i] != '\0') return false;
    }

    /* Strip query string */
    char clean_uri[HTTP_MAX_URI_LEN];
    ts_strlcpy(clean_uri, uri, sizeof(clean_uri));
    char *q = strchr(clean_uri, '?');
    if (q) *q = '\0';

    /* Build candidate path: public_dir + clean_uri */
    char candidate[MAX_PATH_LEN];
    snprintf(candidate, sizeof(candidate), "%s%s", public_dir, clean_uri);

    /* Normalise separators */
    for (char *p = candidate; *p; p++) {
        if (*p == '/') *p = '\\';
    }

    /* GetFullPathName to resolve any remaining relative segments */
    char full[MAX_PATH_LEN];
    DWORD ret = GetFullPathNameA(candidate, sizeof(full), full, NULL);
    if (ret == 0 || ret >= sizeof(full)) return false;

    /* GetFullPathName for the public dir itself */
    char pub_full[MAX_PATH_LEN];
    ret = GetFullPathNameA(public_dir, sizeof(pub_full), pub_full, NULL);
    if (ret == 0) return false;

    /* Ensure resolved path starts with public_dir */
    size_t pub_len = strlen(pub_full);
    if (_strnicmp(full, pub_full, pub_len) != 0) return false;

    ts_strlcpy(resolved_path, full, resolved_len);
    return true;
}

/* ── HTTP response helpers ──────────────────────────────────────────────── */
void http_send_response(SOCKET sock, int status_code,
                        const char *content_type,
                        const char *body, int body_len) {
    const char *reason = "OK";
    switch (status_code) {
        case 200: reason = "OK";                    break;
        case 204: reason = "No Content";            break;
        case 301: reason = "Moved Permanently";     break;
        case 400: reason = "Bad Request";           break;
        case 403: reason = "Forbidden";             break;
        case 404: reason = "Not Found";             break;
        case 405: reason = "Method Not Allowed";    break;
        case 413: reason = "Payload Too Large";     break;
        case 500: reason = "Internal Server Error"; break;
        case 503: reason = "Service Unavailable";   break;
        default:  reason = "Unknown";               break;
    }

    char header[512];
    int hlen = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\n"
        "Server: %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %d\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Connection: close\r\n"
        "\r\n",
        status_code, reason, SERVER_NAME, content_type, body_len);

    send(sock, header, hlen, 0);
    if (body && body_len > 0) {
        int sent = 0;
        while (sent < body_len) {
            int r = send(sock, body + sent, body_len - sent, 0);
            if (r <= 0) break;
            sent += r;
        }
    }
}

void http_send_json(SOCKET sock, const char *json_body) {
    http_send_response(sock, 200, "application/json",
                       json_body, (int)strlen(json_body));
}

void http_send_error(SOCKET sock, int status_code, const char *message) {
    char body[256];
    snprintf(body, sizeof(body),
             "{\"error\":%d,\"message\":\"%s\"}", status_code, message);
    http_send_response(sock, status_code, "application/json",
                       body, (int)strlen(body));
}

/* ── Request parsing ────────────────────────────────────────────────────── */
static bool parse_request(const char *raw, int raw_len, HttpRequest *req) {
    memset(req, 0, sizeof(*req));

    /* Find the end of the request line */
    const char *eol = strstr(raw, "\r\n");
    if (!eol) return false;

    /* Method */
    const char *p = raw;
    int i = 0;
    while (*p && *p != ' ' && i < HTTP_MAX_METHOD_LEN - 1)
        req->method[i++] = *p++;
    req->method[i] = '\0';
    if (*p != ' ') return false;
    p++;

    /* URI */
    i = 0;
    while (*p && *p != ' ' && i < HTTP_MAX_URI_LEN - 1)
        req->uri[i++] = *p++;
    req->uri[i] = '\0';
    if (*p != ' ') return false;
    p++;

    /* HTTP version */
    i = 0;
    while (*p && *p != '\r' && i < 15)
        req->version[i++] = *p++;
    req->version[i] = '\0';

    return true;
}

/* ── Static file serving ────────────────────────────────────────────────── */
static void serve_static_file(Server *srv, SOCKET sock,
                               const char *uri, int request_id,
                               long long *status_out) {
    char resolved[MAX_PATH_LEN];
    char serve_uri[HTTP_MAX_URI_LEN];

    /* Map "/" to "/index.html" */
    if (strcmp(uri, "/") == 0) {
        snprintf(serve_uri, sizeof(serve_uri), "/%s", DEFAULT_PAGE);
    } else {
        ts_strlcpy(serve_uri, uri, sizeof(serve_uri));
    }

    /* Strip query string for file lookup */
    char *q = strchr(serve_uri, '?');
    if (q) *q = '\0';

    if (!path_is_safe(serve_uri, resolved, sizeof(resolved),
                       srv->config.public_dir)) {
        char detail[256];
        snprintf(detail, sizeof(detail), "Traversal attempt: %s", uri);
        metrics_security_event(&srv->metrics, "TRAVERSAL", detail);
        LOG_WARN("Rejected path: %s", uri);
        http_send_error(sock, 403, "Forbidden");
        *status_out = 403;
        return;
    }

    FILE *fp = fopen(resolved, "rb");
    if (!fp) {
        http_send_error(sock, 404, "Not Found");
        *status_out = 404;
        return;
    }

    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    /* Reject unreasonably large files (> 4 MB) */
    if (fsize > 4 * 1024 * 1024) {
        fclose(fp);
        http_send_error(sock, 403, "File too large");
        *status_out = 403;
        return;
    }

    char *content = (char *)malloc(fsize + 1);
    if (!content) {
        fclose(fp);
        http_send_error(sock, 500, "Out of memory");
        *status_out = 500;
        return;
    }

    size_t read_bytes = fread(content, 1, fsize, fp);
    fclose(fp);

    const char *mime = mime_from_ext(resolved);
    http_send_response(sock, 200, mime, content, (int)read_bytes);
    free(content);
    *status_out = 200;
}

/* ── Internal API handler ────────────────────────────────────────────────── */
static void handle_api(Server *srv, SOCKET sock,
                        const char *method, const char *uri,
                        const char *body, int body_len,
                        long long *status_out) {
    char buf[16384];

    /* ── GET /api/metrics ── */
    if (strcmp(uri, "/api/metrics") == 0) {
        metrics_to_json(&srv->metrics, &srv->pool, &srv->queue,
                        buf, sizeof(buf));
        http_send_json(sock, buf);
        *status_out = 200;

    /* ── GET /api/events ── */
    } else if (strcmp(uri, "/api/events") == 0) {
        metrics_events_to_json(&srv->metrics, buf, sizeof(buf));
        http_send_json(sock, buf);
        *status_out = 200;

    /* ── GET /api/security ── */
    } else if (strcmp(uri, "/api/security") == 0) {
        metrics_security_to_json(&srv->metrics, buf, sizeof(buf));
        http_send_json(sock, buf);
        *status_out = 200;

    /* ── GET /api/rps ── */
    } else if (strcmp(uri, "/api/rps") == 0) {
        metrics_rps_to_json(&srv->metrics, buf, sizeof(buf));
        http_send_json(sock, buf);
        *status_out = 200;

    /* ── GET /api/workers ── */
    } else if (strcmp(uri, "/api/workers") == 0) {
        WorkerInfo snap[POOL_MAX_WORKERS];
        int n = tp_snapshot_workers(&srv->pool, snap, POOL_MAX_WORKERS);
        int w = snprintf(buf, sizeof(buf), "[");
        for (int i = 0; i < n && w < (int)sizeof(buf) - 100; i++) {
            const char *state_str =
                snap[i].state == WORKER_BUSY  ? "busy"  :
                snap[i].state == WORKER_IDLE  ? "idle"  : "stopped";
            w += snprintf(buf + w, sizeof(buf) - w,
                "%s{\"id\":%d,\"state\":\"%s\","
                "\"requests\":%d,\"current_req\":%d}",
                i > 0 ? "," : "",
                snap[i].worker_id, state_str,
                snap[i].requests_handled,
                snap[i].current_request_id);
        }
        if (w < (int)sizeof(buf) - 2) { buf[w++] = ']'; buf[w] = '\0'; }
        http_send_json(sock, buf);
        *status_out = 200;

    /* ── POST /api/sync/race ── */
    } else if (strcmp(uri, "/api/sync/race") == 0
               && strcmp(method, "POST") == 0) {
        int threads = 4, iters = 100000;
        /* Simple JSON parse for {"threads":N,"iterations":M} */
        if (body && body_len > 0) {
            const char *t = strstr(body, "\"threads\"");
            if (t) sscanf(t + 10, ":%d", &threads);
            const char *it = strstr(body, "\"iterations\"");
            if (it) sscanf(it + 13, ":%d", &iters);
        }
        threads = TS_CLAMP(threads, 2, SYNC_MAX_THREADS);
        iters   = TS_CLAMP(iters,   1000, SYNC_MAX_ITERATIONS);

        RaceResult result;
        sync_run_race(threads, iters, &result);
        sync_race_to_json(&result, buf, sizeof(buf));
        http_send_json(sock, buf);
        *status_out = 200;

    /* ── POST /api/sync/deadlock ── */
    } else if (strcmp(uri, "/api/sync/deadlock") == 0
               && strcmp(method, "POST") == 0) {
        int threads = 3;
        if (body && body_len > 0) {
            const char *t = strstr(body, "\"threads\"");
            if (t) sscanf(t + 10, ":%d", &threads);
        }
        threads = TS_CLAMP(threads, 2, 4);

        DeadlockResult result;
        sync_run_deadlock_demo(threads, &result);
        sync_deadlock_to_json(&result, buf, sizeof(buf));
        http_send_json(sock, buf);
        *status_out = 200;

    /* ── POST /api/bench/run ── */
    } else if (strcmp(uri, "/api/bench/run") == 0
               && strcmp(method, "POST") == 0) {
        BenchParams params;
        params.total_requests = 100;
        params.thread_count   = 1;
        params.target_port    = srv->config.port;
        ts_strlcpy(params.target_host, SERVER_HOST, sizeof(params.target_host));
        ts_strlcpy(params.target_uri,  "/api/metrics", sizeof(params.target_uri));

        if (body && body_len > 0) {
            const char *r = strstr(body, "\"requests\"");
            if (r) sscanf(r + 11, ":%d", &params.total_requests);
            const char *t = strstr(body, "\"threads\"");
            if (t) sscanf(t + 10, ":%d", &params.thread_count);
        }
        params.total_requests = TS_CLAMP(params.total_requests, 1, BENCH_MAX_REQUESTS);
        params.thread_count   = TS_CLAMP(params.thread_count,   1, BENCH_MAX_THREADS);

        BenchResult result;
        bench_run(&params, &result);
        bench_result_to_json(&result, buf, sizeof(buf));
        http_send_json(sock, buf);
        *status_out = 200;

    /* ── POST /api/adaptive/toggle ── */
    } else if (strcmp(uri, "/api/adaptive/toggle") == 0
               && strcmp(method, "POST") == 0) {
        bool new_state = !srv->pool.adaptive_enabled;
        tp_set_adaptive(&srv->pool, new_state);
        snprintf(buf, sizeof(buf),
                 "{\"adaptive_enabled\":%s}",
                 new_state ? "true" : "false");
        http_send_json(sock, buf);
        *status_out = 200;

    /* ── Unknown API ── */
    } else {
        http_send_error(sock, 404, "Unknown API endpoint");
        *status_out = 404;
    }
}

/* ── Handle one client connection (called by worker threads) ─────────── */
void server_handle_client(Server *srv, SOCKET client_sock,
                           long long enqueue_time_ms, int request_id) {
    metrics_conn_open(&srv->metrics);
    long long start_ms = ts_now_ms();

    /* Set socket timeouts */
    DWORD timeout_ms = HTTP_RECV_TIMEOUT_MS;
    setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO,
               (char *)&timeout_ms, sizeof(timeout_ms));
    setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO,
               (char *)&timeout_ms, sizeof(timeout_ms));

    /* Read the request */
    char raw[HTTP_MAX_REQUEST_SIZE + 1];
    int  total = 0;
    int  headers_done = 0;

    while (total < HTTP_MAX_REQUEST_SIZE) {
        int r = recv(client_sock, raw + total, HTTP_MAX_REQUEST_SIZE - total, 0);
        if (r <= 0) goto cleanup;
        total += r;
        raw[total] = '\0';
        /* Check for end of headers */
        if (strstr(raw, "\r\n\r\n")) {
            headers_done = 1;
            break;
        }
    }

    if (!headers_done) {
        /* Oversized or malformed request */
        char detail[64];
        snprintf(detail, sizeof(detail), "No header end after %d bytes", total);
        metrics_security_event(&srv->metrics, "MALFORMED", detail);
        http_send_error(client_sock, 400, "Bad Request");
        goto cleanup;
    }

    /* Enforce hard request size limit */
    if (total >= HTTP_MAX_REQUEST_SIZE) {
        metrics_security_event(&srv->metrics, "TOO_LARGE",
                               "Request exceeded size limit");
        http_send_error(client_sock, 413, "Request Too Large");
        goto cleanup;
    }

    HttpRequest req;
    if (!parse_request(raw, total, &req)) {
        metrics_security_event(&srv->metrics, "MALFORMED", "Could not parse request line");
        http_send_error(client_sock, 400, "Bad Request");
        goto cleanup;
    }

    /* Only GET and POST are supported */
    if (strcmp(req.method, "GET")  != 0 &&
        strcmp(req.method, "POST") != 0 &&
        strcmp(req.method, "HEAD") != 0) {
        metrics_security_event(&srv->metrics, "BAD_METHOD", req.method);
        http_send_error(client_sock, 405, "Method Not Allowed");
        goto cleanup;
    }

    /* Find body (after \r\n\r\n) */
    const char *body_start = strstr(raw, "\r\n\r\n");
    int body_len = 0;
    if (body_start) {
        body_start += 4;
        body_len    = (int)(raw + total - body_start);
    }

    long long status_code = 200;

    /* Route the request */
    if (strncmp(req.uri, "/api/", 5) == 0) {
        handle_api(srv, client_sock, req.method, req.uri,
                   body_start, body_len, &status_code);
    } else {
        /* Serve static files for GET/HEAD only */
        if (strcmp(req.method, "POST") == 0) {
            http_send_error(client_sock, 405, "Method Not Allowed");
            status_code = 405;
        } else {
            serve_static_file(srv, client_sock, req.uri, request_id, &status_code);
        }
    }

    long long duration_ms = ts_now_ms() - start_ms;
    metrics_record_request(&srv->metrics, request_id,
                           0,   /* worker_id filled by caller in next version */
                           req.method, req.uri,
                           (int)status_code, duration_ms);

cleanup:
    closesocket(client_sock);
    metrics_conn_close(&srv->metrics);
}

/* ── Accept loop thread ──────────────────────────────────────────────────── */
static DWORD WINAPI accept_loop_fn(LPVOID arg) {
    Server *srv = (Server *)arg;
    LOG_INFO("Accept loop started on port %d", srv->config.port);

    while (srv->running) {
        SOCKET client = accept(srv->listen_sock, NULL, NULL);
        if (client == INVALID_SOCKET) {
            if (srv->running) {
                LOG_ERROR("accept() failed: %d", WSAGetLastError());
            }
            break;
        }

        if (!rq_enqueue(&srv->queue, client)) {
            /* enqueue already closed the socket and logged */
        }
    }
    LOG_INFO("Accept loop exited");
    return 0;
}

/* ── Start the server ─────────────────────────────────────────────────────── */
bool server_run(Server *srv) {
    g_server = srv;

    /* ── Winsock init ── */
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        LOG_ERROR("WSAStartup failed: %d", WSAGetLastError());
        return false;
    }

    /* ── Create listen socket ── */
    srv->listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (srv->listen_sock == INVALID_SOCKET) {
        LOG_ERROR("socket() failed: %d", WSAGetLastError());
        WSACleanup();
        return false;
    }

    /* Allow rapid restart */
    BOOL optval = TRUE;
    setsockopt(srv->listen_sock, SOL_SOCKET, SO_REUSEADDR,
               (char *)&optval, sizeof(optval));

    /* ── Bind to localhost ── */
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((u_short)srv->config.port);
    addr.sin_addr.s_addr = inet_addr(SERVER_HOST);

    if (bind(srv->listen_sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        LOG_ERROR("bind() failed: %d", WSAGetLastError());
        closesocket(srv->listen_sock);
        WSACleanup();
        return false;
    }

    if (listen(srv->listen_sock, HTTP_BACKLOG) != 0) {
        LOG_ERROR("listen() failed: %d", WSAGetLastError());
        closesocket(srv->listen_sock);
        WSACleanup();
        return false;
    }

    LOG_INFO("ThreadStorm listening on http://%s:%d", SERVER_HOST, srv->config.port);

    /* ── Initialise subsystems ── */
    metrics_init(&srv->metrics);
    InitializeCriticalSection(&srv->state_lock);

    if (!rq_init(&srv->queue, srv->config.queue_capacity)) {
        LOG_ERROR("Failed to initialise request queue");
        closesocket(srv->listen_sock);
        WSACleanup();
        return false;
    }

    if (!tp_init(&srv->pool, &srv->queue,
                 srv->config.worker_count,
                 POOL_MIN_WORKERS, POOL_MAX_WORKERS)) {
        LOG_ERROR("Failed to initialise thread pool");
        rq_destroy(&srv->queue);
        closesocket(srv->listen_sock);
        WSACleanup();
        return false;
    }

    if (!tp_start(&srv->pool)) {
        LOG_ERROR("Failed to start worker threads");
        tp_destroy(&srv->pool);
        rq_destroy(&srv->queue);
        closesocket(srv->listen_sock);
        WSACleanup();
        return false;
    }

    if (srv->config.adaptive_pool) {
        tp_set_adaptive(&srv->pool, true);
    }

    srv->running = true;

    /* Accept loop runs in this thread (blocks until shutdown) */
    accept_loop_fn(srv);

    /* ── Shutdown sequence ── */
    LOG_INFO("Shutting down ThreadStorm...");
    tp_shutdown(&srv->pool);
    tp_destroy(&srv->pool);
    rq_destroy(&srv->queue);
    metrics_destroy(&srv->metrics);
    DeleteCriticalSection(&srv->state_lock);
    closesocket(srv->listen_sock);
    WSACleanup();
    LOG_INFO("Shutdown complete");
    return true;
}

/* ── Signal shutdown ─────────────────────────────────────────────────────── */
void server_stop(Server *srv) {
    if (!srv || !srv->running) return;
    srv->running = false;
    /* Closing the listen socket causes accept() to return with an error */
    closesocket(srv->listen_sock);
    srv->listen_sock = INVALID_SOCKET;
    LOG_INFO("Server stop requested");
}

