/*
 * ThreadStorm - server.h
 *
 * HTTP server: socket lifecycle, request parsing, response sending,
 * and static-file serving.  All socket I/O happens here.
 */

#ifndef SERVER_H
#define SERVER_H

#include "common.h"
#include "request_queue.h"
#include "thread_pool.h"
#include "metrics.h"

/* ── Parsed HTTP request ──────────────────────────────────────────────── */
typedef struct {
    char method[HTTP_MAX_METHOD_LEN];
    char uri[HTTP_MAX_URI_LEN];
    char version[16];
    char host[128];
    /* raw header block and body omitted for simplicity */
    int  content_length;
    bool keep_alive;
} HttpRequest;

/* ── Server configuration ─────────────────────────────────────────────── */
typedef struct {
    int      port;
    int      worker_count;
    int      queue_capacity;
    bool     adaptive_pool;
    char     public_dir[MAX_PATH_LEN];   /* path to static files */
} ServerConfig;

/* ── Runtime server state ─────────────────────────────────────────────── */
typedef struct {
    ServerConfig  config;
    SOCKET        listen_sock;
    ThreadPool    pool;
    RequestQueue  queue;
    Metrics       metrics;
    volatile bool running;
    CRITICAL_SECTION state_lock;
} Server;

/* ── Public API ──────────────────────────────────────────────────────── */

/* Initialise a Server with default config values. */
void server_default_config(ServerConfig *cfg);

/* Start the server: bind socket, launch pool, enter accept loop.
   Blocks until server stops.  Returns false on fatal error. */
bool server_run(Server *srv);

/* Signal the server to stop (safe to call from a signal handler thread). */
void server_stop(Server *srv);

/* Handle one client connection.  Called by worker threads. */
void server_handle_client(Server *srv, SOCKET client_sock,
                          long long enqueue_time_ms, int request_id);

/* ── HTTP response helpers (used in server.c and by API endpoints) ─────── */
void http_send_response(SOCKET sock, int status_code,
                        const char *content_type,
                        const char *body, int body_len);

void http_send_json(SOCKET sock, const char *json_body);
void http_send_error(SOCKET sock, int status_code, const char *message);

/* Returns true if the given URI maps safely inside public_dir.
   Rejects ".." traversal, absolute paths, and dangerous characters. */
bool path_is_safe(const char *uri, char *resolved_path, int resolved_len,
                  const char *public_dir);

/* MIME type from file extension. */
const char *mime_from_ext(const char *path);

#endif /* SERVER_H */

