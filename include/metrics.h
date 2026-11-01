/*
 * ThreadStorm - metrics.h
 *
 * Collects and exposes server statistics.
 * All counters are updated from multiple threads, so every write/read
 * uses either Interlocked operations or the metrics_lock critical section.
 */

#ifndef METRICS_H
#define METRICS_H

#include "common.h"

/* Forward declare RequestQueue */
typedef struct RequestQueue RequestQueue;


/* ── A single recent request event ───────────────────────────────────── */
typedef struct {
    int       request_id;
    char      method[HTTP_MAX_METHOD_LEN];
    char      uri[HTTP_MAX_URI_LEN];
    int       status_code;
    long long duration_ms;   /* end-to-end latency */
    long long timestamp_ms;  /* when the request completed */
    int       worker_id;
} RequestEvent;

/* ── HTTP status-code bucket ──────────────────────────────────────────── */
typedef struct {
    volatile LONG count_2xx;
    volatile LONG count_3xx;
    volatile LONG count_4xx;
    volatile LONG count_5xx;
} StatusCounts;

/* ── Security event log entry ─────────────────────────────────────────── */
typedef struct {
    char      type[32];        /* e.g. "TRAVERSAL", "BAD_METHOD", "TOO_LARGE" */
    char      detail[128];
    long long timestamp_ms;
} SecurityEvent;

#define MAX_SECURITY_EVENTS 200

/* ── RPS ring buffer (one slot per second) ────────────────────────────── */
typedef struct {
    volatile LONG  slots[METRICS_HISTORY_LEN];  /* requests completed per slot */
    volatile int   current_slot;                 /* index being written */
    long long      slot_start_ms;                /* when current_slot began */
} RpsHistory;

/* ── Main metrics structure ───────────────────────────────────────────── */
typedef struct {
    /* counters (Interlocked – safe without holding lock) */
    volatile LONG  total_requests;
    volatile LONG  success_responses;   /* 2xx */
    volatile LONG  error_responses;     /* 4xx + 5xx */
    volatile LONG  active_connections;

    /* latency (protected by metrics_lock) */
    long long      total_latency_ms;
    long long      min_latency_ms;
    long long      max_latency_ms;

    /* server lifecycle */
    long long      start_time_ms;

    /* status code distribution */
    StatusCounts   status;

    /* recent requests ring buffer (protected by metrics_lock) */
    RequestEvent   recent_events[MAX_RECENT_EVENTS];
    int            event_head;      /* next write position */
    int            event_count;     /* items currently stored */

    /* security events (protected by sec_lock) */
    SecurityEvent  sec_events[MAX_SECURITY_EVENTS];
    int            sec_head;
    int            sec_count;

    /* RPS history */
    RpsHistory     rps;

    /* locks */
    CRITICAL_SECTION metrics_lock;
    CRITICAL_SECTION sec_lock;
} Metrics;

/* ── Public API ──────────────────────────────────────────────────────── */
void metrics_init(Metrics *m);
void metrics_destroy(Metrics *m);

/* Called when a request completes. */
void metrics_record_request(Metrics *m,
                            int request_id, int worker_id,
                            const char *method, const char *uri,
                            int status_code, long long duration_ms);

/* Called when a connection opens/closes. */
void metrics_conn_open(Metrics *m);
void metrics_conn_close(Metrics *m);

/* Record a security event. */
void metrics_security_event(Metrics *m, const char *type, const char *detail);

/* Fill a JSON string with a snapshot of current metrics.
   buf must be at least 4096 bytes. */
void metrics_to_json(Metrics *m, const void *pool,
                     const RequestQueue *queue, char *buf, int buf_size);

/* Fill a JSON string with recent request events. */
void metrics_events_to_json(Metrics *m, char *buf, int buf_size);

/* Fill a JSON string with security events. */
void metrics_security_to_json(Metrics *m, char *buf, int buf_size);

/* Fill a JSON string with RPS history array (last N seconds). */
void metrics_rps_to_json(Metrics *m, char *buf, int buf_size);

#endif /* METRICS_H */

