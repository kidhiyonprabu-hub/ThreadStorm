/*
 * ThreadStorm - Real-Time Multithreaded Web Server & Synchronization Intelligence Platform
 * common.h - Shared types, constants, and platform definitions
 *
 * This header is included by every module. It defines constants and
 * Windows-specific setup needed before any other system header.
 */

#ifndef COMMON_H
#define COMMON_H

/* ── Windows platform setup ─────────────────────────────────────────── */
#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600   /* Vista+ for condition variables */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

/* ── Standard library ────────────────────────────────────────────────── */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <stdbool.h>

/* ── Compiler / linkage ──────────────────────────────────────────────── */
#pragma comment(lib, "ws2_32.lib")

/* ── Server configuration constants ─────────────────────────────────── */
#define SERVER_PORT           8080
#define SERVER_HOST           "127.0.0.1"
#define SERVER_NAME           "ThreadStorm/1.0"

/* ── Thread-pool limits ──────────────────────────────────────────────── */
#define POOL_MIN_WORKERS      2
#define POOL_MAX_WORKERS      32
#define POOL_DEFAULT_WORKERS  4

/* ── Request queue ───────────────────────────────────────────────────── */
#define QUEUE_DEFAULT_CAPACITY  64
#define QUEUE_MAX_CAPACITY     256

/* ── HTTP / network ──────────────────────────────────────────────────── */
#define HTTP_MAX_REQUEST_SIZE   (8  * 1024)   /* 8 KB  */
#define HTTP_MAX_HEADER_SIZE    (4  * 1024)   /* 4 KB  */
#define HTTP_MAX_URI_LEN        512
#define HTTP_MAX_METHOD_LEN     16
#define HTTP_SEND_TIMEOUT_MS    5000
#define HTTP_RECV_TIMEOUT_MS    5000
#define HTTP_BACKLOG            64

/* ── Static file serving ─────────────────────────────────────────────── */
#define PUBLIC_DIR              "public"
#define DEFAULT_PAGE            "index.html"
#define MAX_PATH_LEN            512

/* ── Metrics & monitoring ────────────────────────────────────────────── */
#define METRICS_HISTORY_LEN     60    /* seconds of RPS history */
#define MAX_RECENT_EVENTS       100

/* ── Synchronization lab ─────────────────────────────────────────────── */
#define SYNC_MAX_THREADS        16
#define SYNC_MAX_ITERATIONS     1000000

/* ── Benchmark ───────────────────────────────────────────────────────── */
#define BENCH_MAX_REQUESTS      2000
#define BENCH_MAX_THREADS       32

/* ── Adaptive pool ───────────────────────────────────────────────────── */
#define ADAPTIVE_COOLDOWN_MS    2000
#define ADAPTIVE_SCALE_STEP     1

/* ── Logging ─────────────────────────────────────────────────────────── */
#define LOG_FILE                "logs/threadstorm.log"

/* ── Utility macros ──────────────────────────────────────────────────── */
#define TS_MIN(a, b)   ((a) < (b) ? (a) : (b))
#define TS_MAX(a, b)   ((a) > (b) ? (a) : (b))
#define TS_CLAMP(v, lo, hi) (TS_MAX((lo), TS_MIN((hi), (v))))
#define TS_UNUSED(x) ((void)(x))

/* ── Logging helper ──────────────────────────────────────────────────── */
void ts_log(const char *level, const char *fmt, ...);
#define LOG_INFO(...)   ts_log("INFO ", __VA_ARGS__)
#define LOG_WARN(...)   ts_log("WARN ", __VA_ARGS__)
#define LOG_ERROR(...)  ts_log("ERROR", __VA_ARGS__)
#define LOG_DEBUG(...)  ts_log("DEBUG", __VA_ARGS__)

/* ── Timestamp helper ────────────────────────────────────────────────── */
/* Returns milliseconds since process start (monotonic-ish via QPC) */
long long ts_now_ms(void);

/* ── System resource metrics (CO3: Memory & CPU Telemetry) ───────────── */
void ts_get_memory_usage(size_t *working_set_kb, size_t *peak_working_set_kb, size_t *private_kb);
double ts_get_cpu_usage(void);

/* ── Safe string copy ────────────────────────────────────────────────── */
static inline void ts_strlcpy(char *dst, const char *src, size_t n) {
    if (n == 0) return;
    strncpy(dst, src, n - 1);
    dst[n - 1] = '\0';
}

#endif /* COMMON_H */

