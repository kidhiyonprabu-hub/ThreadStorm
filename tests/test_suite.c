/*
 * ThreadStorm - tests/test_suite.c
 *
 * Automated test suite. Starts the C server in a background thread,
 * fires HTTP requests against it, and validates responses.
 */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "common.h"
#include "server.h"

#define TEST_HOST   "127.0.0.1"
#define TEST_PORT   18080       /* dedicated test port */
#define PASS        "\033[32mPASS\033[0m"
#define FAIL        "\033[31mFAIL\033[0m"

static int g_pass = 0, g_fail = 0;

#define ASSERT(cond, name) do { \
    if (cond) { printf("  [%s] %s\n", PASS, name); g_pass++; } \
    else       { printf("  [%s] %s\n", FAIL, name); g_fail++; } \
} while(0)

/* ── Low-level HTTP request helper ───────────────────────────────────── */
typedef struct {
    int  status_code;
    char body[8192];
    int  body_len;
    bool ok;
} HttpResponse;

static HttpResponse http_get(const char *path) {
    HttpResponse res = {0};

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return res;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(TEST_PORT);
    addr.sin_addr.s_addr = inet_addr(TEST_HOST);

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        printf("connect failed: %d\n", WSAGetLastError());
        closesocket(s); return res;
    }

    DWORD toms = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&toms, sizeof(toms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&toms, sizeof(toms));

    char req[512];
    int rl = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
        path, TEST_HOST);
    send(s, req, rl, 0);

    char buf[8192];
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        int r = recv(s, buf + total, sizeof(buf) - 1 - total, 0);
        if (r <= 0) break;
        total += r;
    }
    closesocket(s);
    buf[total] = '\0';

    int code = 0;
    if (sscanf(buf, "HTTP/%*s %d", &code) == 1) {
        res.status_code = code;
        res.ok = true;
    }

    const char *body = strstr(buf, "\r\n\r\n");
    if (body) {
        body += 4;
        res.body_len = (int)(buf + total - body);
        if (res.body_len > 0 && res.body_len < (int)sizeof(res.body)) {
            memcpy(res.body, body, res.body_len);
            res.body[res.body_len] = '\0';
        }
    }
    return res;
}

static HttpResponse http_post(const char *path, const char *json_body) {
    HttpResponse res = {0};

    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return res;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(TEST_PORT);
    addr.sin_addr.s_addr = inet_addr(TEST_HOST);

    if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        printf("connect failed: %d\n", WSAGetLastError());
        closesocket(s); return res;
    }

    DWORD toms = 10000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&toms, sizeof(toms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&toms, sizeof(toms));

    int body_len = json_body ? (int)strlen(json_body) : 0;
    char req[1024];
    int rl = snprintf(req, sizeof(req),
        "POST %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, TEST_HOST, body_len);
    send(s, req, rl, 0);
    if (body_len > 0) send(s, json_body, body_len, 0);

    char buf[8192];
    int total = 0;
    while (total < (int)sizeof(buf) - 1) {
        int r = recv(s, buf + total, sizeof(buf) - 1 - total, 0);
        if (r <= 0) break;
        total += r;
    }
    closesocket(s);
    buf[total] = '\0';

    int code = 0;
    if (sscanf(buf, "HTTP/%*s %d", &code) == 1) {
        res.status_code = code;
        res.ok = true;
    }
    const char *body = strstr(buf, "\r\n\r\n");
    if (body) {
        body += 4;
        res.body_len = (int)(buf + total - body);
        if (res.body_len > 0 && res.body_len < (int)sizeof(res.body)) {
            memcpy(res.body, body, res.body_len);
            res.body[res.body_len] = '\0';
        }
    }
    return res;
}

/* ── Test server in background thread ─────────────────────────────────── */
static Server g_test_srv;
static HANDLE g_test_srv_thread = NULL;

static DWORD WINAPI server_thread_proc(LPVOID param) {
    TS_UNUSED(param);
    server_run(&g_test_srv);
    return 0;
}

static bool start_test_server(void) {
    memset(&g_test_srv, 0, sizeof(g_test_srv));
    server_default_config(&g_test_srv.config);
    g_test_srv.config.port           = TEST_PORT;
    g_test_srv.config.worker_count   = 4;
    g_test_srv.config.queue_capacity = 32;

    g_test_srv_thread = CreateThread(NULL, 0, server_thread_proc, NULL, 0, NULL);
    if (!g_test_srv_thread) return false;

    /* Wait up to 2 seconds for server to be ready */
    for (int i = 0; i < 20; i++) {
        Sleep(100);
        HttpResponse r = http_get("/api/metrics");
        if (r.ok && r.status_code == 200) {
            return true;
        }
    }
    return false;
}

static void stop_test_server(void) {
    server_stop(&g_test_srv);
    if (g_test_srv_thread) {
        WaitForSingleObject(g_test_srv_thread, 3000);
        CloseHandle(g_test_srv_thread);
        g_test_srv_thread = NULL;
    }
}

/* ══════════════════════════════════════════════════════════════════════
 *  TEST CASES
 * ══════════════════════════════════════════════════════════════════════ */

static void test_basic_connectivity(void) {
    printf("\n── 1. Basic Connectivity ──\n");
    HttpResponse r = http_get("/api/metrics");
    ASSERT(r.ok,                 "Connected to server");
    ASSERT(r.status_code == 200, "GET /api/metrics returns 200");
    ASSERT(strstr(r.body, "\"status\""), "/api/metrics body has 'status' key");
    ASSERT(strstr(r.body, "\"total_requests\""),
           "/api/metrics body has 'total_requests'");
}

static void test_static_file_serving(void) {
    printf("\n── 2. Static File Serving ──\n");
    HttpResponse r = http_get("/");
    ASSERT(r.ok,                 "Connected for /");
    ASSERT(r.status_code == 200, "GET / returns 200");
    ASSERT(r.body_len > 0,       "Response body not empty");

    /* 404 for missing file */
    r = http_get("/does_not_exist_xyz.txt");
    ASSERT(r.status_code == 404, "Missing file returns 404");
}

static void test_path_traversal_protection(void) {
    printf("\n── 3. Path Traversal Protection ──\n");
    HttpResponse r = http_get("/../../../etc/passwd");
    ASSERT(r.status_code == 403 || r.status_code == 400,
           "Traversal attempt rejected with 403/400");

    r = http_get("/..\\..\\windows\\system32\\cmd.exe");
    ASSERT(r.status_code == 403 || r.status_code == 400,
           "Windows backslash traversal rejected");
}

static void test_method_restriction(void) {
    printf("\n── 4. HTTP Method Restriction ──\n");
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(TEST_PORT);
    addr.sin_addr.s_addr = inet_addr(TEST_HOST);
    connect(s, (struct sockaddr *)&addr, sizeof(addr));

    DWORD toms = 3000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&toms, sizeof(toms));

    const char *req = "DELETE / HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    send(s, req, (int)strlen(req), 0);
    char buf[512]; int n = recv(s, buf, sizeof(buf)-1, 0);
    closesocket(s);
    if (n > 0) { buf[n] = '\0'; }
    int code = 0;
    sscanf(buf, "HTTP/%*s %d", &code);
    ASSERT(code == 405, "DELETE method returns 405 Method Not Allowed");
}

static void test_api_endpoints(void) {
    printf("\n── 5. API Endpoints ──\n");
    HttpResponse r;

    r = http_get("/api/events");
    ASSERT(r.status_code == 200, "GET /api/events returns 200");

    r = http_get("/api/security");
    ASSERT(r.status_code == 200, "GET /api/security returns 200");

    r = http_get("/api/rps");
    ASSERT(r.status_code == 200, "GET /api/rps returns 200");

    r = http_get("/api/workers");
    ASSERT(r.status_code == 200, "GET /api/workers returns 200");

    r = http_get("/api/nonexistent");
    ASSERT(r.status_code == 404, "Unknown API returns 404");
}

static void test_concurrent_requests(void) {
    printf("\n── 6. Concurrent Requests ──\n");

    SOCKET sockets[20];
    int connected = 0;
    for (int i = 0; i < 20; i++) {
        sockets[i] = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons(TEST_PORT);
        addr.sin_addr.s_addr = inet_addr(TEST_HOST);
        if (connect(sockets[i], (struct sockaddr *)&addr, sizeof(addr)) == 0)
            connected++;
    }
    ASSERT(connected >= 15, ">= 15 of 20 concurrent connects succeed");

    int ok_responses = 0;
    for (int i = 0; i < 20; i++) {
        if (sockets[i] == INVALID_SOCKET) continue;
        const char *req = "GET /api/metrics HTTP/1.1\r\nHost:127.0.0.1\r\n\r\n";
        send(sockets[i], req, (int)strlen(req), 0);
        char buf[1024]; int r = recv(sockets[i], buf, sizeof(buf)-1, 0);
        if (r > 4) { buf[r] = '\0'; if (strstr(buf, "200")) ok_responses++; }
        closesocket(sockets[i]);
    }
    ASSERT(ok_responses >= 15, ">= 15 of 20 concurrent requests get 200");
}

static void test_sync_race(void) {
    printf("\n── 7. Sync Lab: Race Condition ──\n");
    HttpResponse r = http_post("/api/sync/race",
                               "{\"threads\":4,\"iterations\":50000}");
    ASSERT(r.status_code == 200, "POST /api/sync/race returns 200");
    ASSERT(strstr(r.body, "\"expected_value\""),
           "Race result has 'expected_value'");
    ASSERT(strstr(r.body, "\"safe_result\""),
           "Race result has 'safe_result'");
}

static void test_sync_deadlock(void) {
    printf("\n── 8. Sync Lab: Deadlock Handling & Prevention (CO3) ──\n");
    HttpResponse r = http_post("/api/sync/deadlock", "{\"threads\":3}");
    ASSERT(r.status_code == 200, "POST /api/sync/deadlock returns 200");
    ASSERT(strstr(r.body, "\"result\""), "Deadlock result has 'result'");
    ASSERT(strstr(r.body, "completed"), "Deadlock demo completed without hang");
    ASSERT(strstr(r.body, "\"deadlocks_detected\""), "Deadlock JSON reports 'deadlocks_detected'");
    ASSERT(strstr(r.body, "\"recoveries_performed\""), "Deadlock JSON reports 'recoveries_performed'");
    ASSERT(strstr(r.body, "\"counter_unsafe\""), "Deadlock JSON reports 'counter_unsafe'");
    ASSERT(strstr(r.body, "\"counter_safe\""), "Deadlock JSON reports 'counter_safe'");
}

static void test_co3_memory_telemetry_and_stability(void) {
    printf("\n── 11. Memory Management Telemetry & Stability (CO3) ──\n");
    HttpResponse r1 = http_get("/api/metrics");
    ASSERT(r1.ok && r1.status_code == 200, "GET /api/metrics successful");
    ASSERT(strstr(r1.body, "\"ram_working_set_kb\""), "Metrics exposes 'ram_working_set_kb'");
    ASSERT(strstr(r1.body, "\"ram_peak_kb\""), "Metrics exposes 'ram_peak_kb'");
    ASSERT(strstr(r1.body, "\"queue_memory_bytes\""), "Metrics exposes 'queue_memory_bytes'");
    ASSERT(strstr(r1.body, "\"cpu_usage_pct\""), "Metrics exposes 'cpu_usage_pct'");

    long long ws_before = 0;
    const char *pws = strstr(r1.body, "\"ram_working_set_kb\":");
    if (pws) sscanf(pws + 21, "%lld", &ws_before);
    ASSERT(ws_before > 0, "Working set size is greater than 0 KB");

    /* Burst 50 HTTP requests to evaluate memory stability */
    for (int i = 0; i < 50; i++) {
        HttpResponse rb = http_get("/api/metrics");
        ASSERT(rb.ok, "Burst request completed");
        if (!rb.ok) break;
    }

    HttpResponse r2 = http_get("/api/metrics");
    long long ws_after = 0;
    pws = strstr(r2.body, "\"ram_working_set_kb\":");
    if (pws) sscanf(pws + 21, "%lld", &ws_after);

    /* Bounded allocation check: Memory should not grow uncontrollably (< 64 MB growth) */
    long long diff_kb = ws_after > ws_before ? (ws_after - ws_before) : (ws_before - ws_after);
    ASSERT(diff_kb < 65536, "Memory remains bounded with zero run-away leak under burst load");
}

static void test_metrics_accuracy(void) {
    printf("\n── 9. Metrics Accuracy ──\n");
    HttpResponse r1 = http_get("/api/metrics");
    long total_before = 0;
    if (r1.ok) {
        const char *p = strstr(r1.body, "\"total_requests\":");
        if (p) sscanf(p + 17, "%ld", &total_before);
    }

    for (int i = 0; i < 5; i++) http_get("/api/metrics");

    HttpResponse r2 = http_get("/api/metrics");
    long total_after = 0;
    if (r2.ok) {
        const char *p = strstr(r2.body, "\"total_requests\":");
        if (p) sscanf(p + 17, "%ld", &total_after);
    }

    ASSERT(total_after >= total_before + 5,
           "total_requests counter increments correctly");
}

static void test_oversized_request(void) {
    printf("\n── 10. Oversized Request ──\n");
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons(TEST_PORT);
    addr.sin_addr.s_addr = inet_addr(TEST_HOST);
    connect(s, (struct sockaddr *)&addr, sizeof(addr));

    DWORD toms = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&toms, sizeof(toms));

    char *big = (char *)malloc(9000);
    memset(big, 'A', 9000);
    memcpy(big, "GET / HTTP/1.1\r\nX-Junk: ", 24);
    send(s, big, 9000, 0);
    free(big);

    char buf[512]; int n = recv(s, buf, sizeof(buf)-1, 0);
    closesocket(s);
    int code = 0;
    if (n > 0) { buf[n] = '\0'; sscanf(buf, "HTTP/%*s %d", &code); }
    ASSERT(code == 400 || code == 413,
           "Oversized/malformed request gets 400 or 413");
}

/* ── Main ─────────────────────────────────────────────────────────────── */
int main(void) {
    printf("╔══════════════════════════════════════════════════╗\n");
    printf("║      ThreadStorm Test Suite  v1.0                ║\n");
    printf("╚══════════════════════════════════════════════════╝\n");

    WSADATA wsa;
    WSAStartup(MAKEWORD(2,2), &wsa);

    printf("\n[Starting in-process test server on port %d...]\n", TEST_PORT);
    if (!start_test_server()) {
        fprintf(stderr, "Cannot start server – aborting tests.\n");
        WSACleanup();
        return 1;
    }

    test_basic_connectivity();
    test_static_file_serving();
    test_path_traversal_protection();
    test_method_restriction();
    test_api_endpoints();
    test_concurrent_requests();
    test_sync_race();
    test_sync_deadlock();
    test_metrics_accuracy();
    test_oversized_request();
    test_co3_memory_telemetry_and_stability();

    stop_test_server();

    printf("\n══════════════════════════════════════════════════\n");
    printf("  Results:  %d passed  |  %d failed\n", g_pass, g_fail);
    printf("══════════════════════════════════════════════════\n\n");

    WSACleanup();
    return g_fail > 0 ? 1 : 0;
}
