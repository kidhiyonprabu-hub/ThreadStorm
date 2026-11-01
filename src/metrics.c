/*
 * ThreadStorm - metrics.c
 *
 * All counters that multiple threads increment use InterlockedIncrement /
 * InterlockedDecrement so no lock is needed for the common case.
 * Composite reads (e.g. building a JSON snapshot) take metrics_lock briefly.
 */

#include "metrics.h"
#include "request_queue.h"
#include "thread_pool.h"

/* ── Init / Destroy ────────────────────────────────────────────────────── */
void metrics_init(Metrics *m) {
    memset(m, 0, sizeof(*m));
    m->start_time_ms    = ts_now_ms();
    m->min_latency_ms   = LLONG_MAX;
    InitializeCriticalSection(&m->metrics_lock);
    InitializeCriticalSection(&m->sec_lock);

    /* Initialise RPS history */
    m->rps.current_slot  = 0;
    m->rps.slot_start_ms = ts_now_ms();
}

void metrics_destroy(Metrics *m) {
    DeleteCriticalSection(&m->metrics_lock);
    DeleteCriticalSection(&m->sec_lock);
}

/* ── RPS helper ────────────────────────────────────────────────────────── */
static void rps_record(RpsHistory *rps) {
    long long now_ms  = ts_now_ms();
    long long elapsed = now_ms - rps->slot_start_ms;

    if (elapsed >= 1000) {
        /* Advance to next slot (wrap around) */
        int next = (rps->current_slot + 1) % METRICS_HISTORY_LEN;
        InterlockedExchange(&rps->slots[next], 0);   /* clear new slot */
        rps->current_slot  = next;
        rps->slot_start_ms = now_ms;
    }

    InterlockedIncrement(&rps->slots[rps->current_slot]);
}

/* ── Record a completed request ───────────────────────────────────────── */
void metrics_record_request(Metrics *m,
                             int request_id, int worker_id,
                             const char *method, const char *uri,
                             int status_code, long long duration_ms) {
    InterlockedIncrement(&m->total_requests);

    if (status_code >= 200 && status_code < 300) {
        InterlockedIncrement(&m->success_responses);
        InterlockedIncrement(&m->status.count_2xx);
    } else if (status_code >= 300 && status_code < 400) {
        InterlockedIncrement(&m->status.count_3xx);
    } else if (status_code >= 400 && status_code < 500) {
        InterlockedIncrement(&m->error_responses);
        InterlockedIncrement(&m->status.count_4xx);
    } else if (status_code >= 500) {
        InterlockedIncrement(&m->error_responses);
        InterlockedIncrement(&m->status.count_5xx);
    }

    rps_record(&m->rps);

    EnterCriticalSection(&m->metrics_lock);

    m->total_latency_ms += duration_ms;
    if (duration_ms < m->min_latency_ms) m->min_latency_ms = duration_ms;
    if (duration_ms > m->max_latency_ms) m->max_latency_ms = duration_ms;

    /* Write event into ring buffer */
    int slot = m->event_head % MAX_RECENT_EVENTS;
    RequestEvent *ev = &m->recent_events[slot];
    ev->request_id   = request_id;
    ev->status_code  = status_code;
    ev->duration_ms  = duration_ms;
    ev->timestamp_ms = ts_now_ms();
    ev->worker_id    = worker_id;
    ts_strlcpy(ev->method, method ? method : "?", sizeof(ev->method));
    ts_strlcpy(ev->uri,    uri    ? uri    : "/", sizeof(ev->uri));

    m->event_head++;
    if (m->event_count < MAX_RECENT_EVENTS) m->event_count++;

    LeaveCriticalSection(&m->metrics_lock);
}

/* ── Connection tracking ─────────────────────────────────────────────── */
void metrics_conn_open(Metrics *m) {
    InterlockedIncrement(&m->active_connections);
}
void metrics_conn_close(Metrics *m) {
    if (m->active_connections > 0)
        InterlockedDecrement(&m->active_connections);
}

/* ── Security events ─────────────────────────────────────────────────── */
void metrics_security_event(Metrics *m, const char *type, const char *detail) {
    EnterCriticalSection(&m->sec_lock);

    int slot = m->sec_head % MAX_SECURITY_EVENTS;
    SecurityEvent *ev = &m->sec_events[slot];
    ts_strlcpy(ev->type,   type,   sizeof(ev->type));
    ts_strlcpy(ev->detail, detail, sizeof(ev->detail));
    ev->timestamp_ms = ts_now_ms();

    m->sec_head++;
    if (m->sec_count < MAX_SECURITY_EVENTS) m->sec_count++;

    LeaveCriticalSection(&m->sec_lock);
}

/* ── JSON helpers ─────────────────────────────────────────────────────── */

/* Escape a string for embedding in JSON (very small subset). */
static void json_escape(const char *src, char *dst, int dst_size) {
    int j = 0;
    for (int i = 0; src[i] && j < dst_size - 2; i++) {
        unsigned char c = (unsigned char)src[i];
        if (c == '"' || c == '\\') {
            if (j + 2 >= dst_size - 1) break;
            dst[j++] = '\\';
            dst[j++] = (char)c;
        } else if (c < 0x20) {
            /* Replace control characters */
            if (j + 2 >= dst_size - 1) break;
            dst[j++] = '?';
        } else {
            dst[j++] = (char)c;
        }
    }
    dst[j] = '\0';
}

void metrics_to_json(Metrics *m, const void *pool_ptr,
                     const RequestQueue *queue, char *buf, int buf_size) {
    const ThreadPool *pool = (const ThreadPool *)pool_ptr;

    long long uptime_ms = ts_now_ms() - m->start_time_ms;

    int idle = 0, busy = 0, stopped = 0;
    int worker_count = 0;
    if (pool) {
        tp_count_states(pool, &idle, &busy, &stopped);
        worker_count = pool->worker_count;
    }

    int queue_depth    = queue ? rq_count_approx(queue)  : 0;
    int queue_capacity = queue ? queue->capacity : 0;

    long long total_req = m->total_requests;
    double avg_lat = (total_req > 0)
        ? (double)m->total_latency_ms / (double)total_req : 0.0;

    /* Compute current RPS from last two slots */
    int rps_val = 0;
    {
        int cur  = m->rps.current_slot;
        int prev = (cur + METRICS_HISTORY_LEN - 1) % METRICS_HISTORY_LEN;
        rps_val  = (int)(m->rps.slots[cur] + m->rps.slots[prev]);
        /* crude 1-second average: average of current + previous slot */
        if (rps_val > 0) rps_val = (rps_val + 1) / 2;
    }

    /* Adaptive pool info */
    char adaptive_reason[128] = "N/A";
    bool adaptive_on = false;
    if (pool) {
        adaptive_on = pool->adaptive_enabled;
        if (pool->last_scale_reason[0])
            ts_strlcpy(adaptive_reason, pool->last_scale_reason,
                       sizeof(adaptive_reason));
    }

    /* CO3 Memory & CPU Telemetry */
    size_t ram_ws_kb = 0, ram_peak_kb = 0, ram_priv_kb = 0;
    ts_get_memory_usage(&ram_ws_kb, &ram_peak_kb, &ram_priv_kb);
    double cpu_pct = ts_get_cpu_usage();
    size_t queue_mem_bytes = queue ? (sizeof(RequestQueue) + (size_t)queue->capacity * sizeof(QueueSlot)) : 0;

    snprintf(buf, buf_size,
        "{"
        "\"status\":\"running\","
        "\"uptime_ms\":%lld,"
        "\"total_requests\":%ld,"
        "\"success_responses\":%ld,"
        "\"error_responses\":%ld,"
        "\"active_connections\":%ld,"
        "\"rps\":%d,"
        "\"avg_latency_ms\":%.2f,"
        "\"min_latency_ms\":%lld,"
        "\"max_latency_ms\":%lld,"
        "\"worker_count\":%d,"
        "\"workers_idle\":%d,"
        "\"workers_busy\":%d,"
        "\"queue_depth\":%d,"
        "\"queue_capacity\":%d,"
        "\"status_2xx\":%ld,"
        "\"status_3xx\":%ld,"
        "\"status_4xx\":%ld,"
        "\"status_5xx\":%ld,"
        "\"adaptive_enabled\":%s,"
        "\"adaptive_last_reason\":\"%s\","
        "\"ram_working_set_kb\":%zu,"
        "\"ram_peak_kb\":%zu,"
        "\"ram_private_kb\":%zu,"
        "\"queue_memory_bytes\":%zu,"
        "\"cpu_usage_pct\":%.1f"
        "}",
        uptime_ms,
        (long)m->total_requests,
        (long)m->success_responses,
        (long)m->error_responses,
        (long)m->active_connections,
        rps_val,
        avg_lat,
        (m->min_latency_ms == LLONG_MAX ? 0 : m->min_latency_ms),
        m->max_latency_ms,
        worker_count,
        idle, busy,
        queue_depth, queue_capacity,
        (long)m->status.count_2xx,
        (long)m->status.count_3xx,
        (long)m->status.count_4xx,
        (long)m->status.count_5xx,
        adaptive_on ? "true" : "false",
        adaptive_reason,
        ram_ws_kb,
        ram_peak_kb,
        ram_priv_kb,
        queue_mem_bytes,
        cpu_pct
    );
}

void metrics_events_to_json(Metrics *m, char *buf, int buf_size) {
    EnterCriticalSection(&m->metrics_lock);

    int written = snprintf(buf, buf_size, "[");
    int count   = TS_MIN(m->event_count, MAX_RECENT_EVENTS);
    /* Output newest first */
    int start   = (m->event_head - 1 + MAX_RECENT_EVENTS) % MAX_RECENT_EVENTS;
    int output  = TS_MIN(count, 50);   /* cap at 50 for JSON size */

    for (int i = 0; i < output && written < buf_size - 200; i++) {
        int idx = (start - i + MAX_RECENT_EVENTS) % MAX_RECENT_EVENTS;
        if (i >= m->event_count) break;
        RequestEvent *ev = &m->recent_events[idx];
        char esc_uri[HTTP_MAX_URI_LEN * 2];
        json_escape(ev->uri, esc_uri, sizeof(esc_uri));
        written += snprintf(buf + written, buf_size - written,
            "%s{\"id\":%d,\"method\":\"%s\",\"uri\":\"%s\","
            "\"status\":%d,\"duration_ms\":%lld,"
            "\"timestamp_ms\":%lld,\"worker_id\":%d}",
            i > 0 ? "," : "",
            ev->request_id, ev->method, esc_uri,
            ev->status_code, ev->duration_ms,
            ev->timestamp_ms, ev->worker_id);
    }
    if (written < buf_size - 2) {
        buf[written++] = ']';
        buf[written]   = '\0';
    } else {
        buf[buf_size - 2] = ']';
        buf[buf_size - 1] = '\0';
    }

    LeaveCriticalSection(&m->metrics_lock);
}

void metrics_security_to_json(Metrics *m, char *buf, int buf_size) {
    EnterCriticalSection(&m->sec_lock);

    int written = snprintf(buf, buf_size, "[");
    int count   = TS_MIN(m->sec_count, MAX_SECURITY_EVENTS);
    int start   = (m->sec_head - 1 + MAX_SECURITY_EVENTS) % MAX_SECURITY_EVENTS;
    int output  = TS_MIN(count, 30);

    for (int i = 0; i < output && written < buf_size - 200; i++) {
        int idx = (start - i + MAX_SECURITY_EVENTS) % MAX_SECURITY_EVENTS;
        if (i >= m->sec_count) break;
        SecurityEvent *ev = &m->sec_events[idx];
        char esc_detail[256];
        json_escape(ev->detail, esc_detail, sizeof(esc_detail));
        written += snprintf(buf + written, buf_size - written,
            "%s{\"type\":\"%s\",\"detail\":\"%s\",\"timestamp_ms\":%lld}",
            i > 0 ? "," : "",
            ev->type, esc_detail, ev->timestamp_ms);
    }
    if (written < buf_size - 2) {
        buf[written++] = ']';
        buf[written]   = '\0';
    } else {
        buf[buf_size - 2] = ']';
        buf[buf_size - 1] = '\0';
    }

    LeaveCriticalSection(&m->sec_lock);
}

void metrics_rps_to_json(Metrics *m, char *buf, int buf_size) {
    int written = snprintf(buf, buf_size, "[");
    int cur     = m->rps.current_slot;
    for (int i = 0; i < METRICS_HISTORY_LEN && written < buf_size - 20; i++) {
        int idx = (cur - METRICS_HISTORY_LEN + 1 + i
                   + METRICS_HISTORY_LEN) % METRICS_HISTORY_LEN;
        written += snprintf(buf + written, buf_size - written,
                            "%s%ld", i > 0 ? "," : "",
                            (long)m->rps.slots[idx]);
    }
    if (written < buf_size - 2) {
        buf[written++] = ']';
        buf[written]   = '\0';
    } else {
        buf[buf_size - 2] = ']';
        buf[buf_size - 1] = '\0';
    }
}

