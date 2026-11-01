/*
 * ThreadStorm - benchmark.c
 *
 * Issues N HTTP GET requests against the running server using
 * either a single thread (sequential) or multiple threads (concurrent).
 *
 * Latency samples are collected, sorted, and used for percentile
 * calculations when enough samples are available.
 */

#include "benchmark.h"
#include <math.h>

/* ── Per-thread benchmark argument ───────────────────────────────────── */
typedef struct {
    const BenchParams *params;
    int    start_req;          /* first request index for this thread */
    int    req_count;          /* how many requests this thread sends */
    long long *latencies;      /* caller-supplied latency array slice */
    volatile LONG *completed;   /* shared counter (Interlocked) */
    volatile LONG *failed;      /* shared counter (Interlocked) */
} BenchThreadArg;

static DWORD WINAPI bench_thread_fn(LPVOID arg) {
    BenchThreadArg *bta = (BenchThreadArg *)arg;
    const BenchParams *p = bta->params;

    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);

    for (int i = 0; i < bta->req_count; i++) {
        long long t0 = ts_now_ms();

        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (s == INVALID_SOCKET) {
            InterlockedIncrement(bta->failed);
            continue;
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family      = AF_INET;
        addr.sin_port        = htons((u_short)p->target_port);
        addr.sin_addr.s_addr = inet_addr(p->target_host);

        if (connect(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            closesocket(s);
            InterlockedIncrement(bta->failed);
            continue;
        }

        /* Set short timeouts for benchmark connections */
        DWORD toms = 3000;
        setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (char *)&toms, sizeof(toms));
        setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (char *)&toms, sizeof(toms));

        char req[256];
        int req_len = snprintf(req, sizeof(req),
            "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
            p->target_uri, p->target_host);

        if (send(s, req, req_len, 0) != req_len) {
            closesocket(s);
            InterlockedIncrement(bta->failed);
            continue;
        }

        /* Read response (drain it) */
        char rbuf[4096];
        int  total_recv = 0;
        bool got_data   = false;
        while (total_recv < 65536) {
            int r = recv(s, rbuf, sizeof(rbuf), 0);
            if (r <= 0) break;
            total_recv += r;
            got_data = true;
        }

        closesocket(s);

        long long lat = ts_now_ms() - t0;
        bta->latencies[i] = lat;

        if (got_data) {
            InterlockedIncrement(bta->completed);
        } else {
            InterlockedIncrement(bta->failed);
        }
    }

    WSACleanup();
    return 0;
}

/* ── Comparison function for qsort ──────────────────────────────────── */
static int compare_ll(const void *a, const void *b) {
    long long x = *(const long long *)a;
    long long y = *(const long long *)b;
    return (x > y) - (x < y);
}

/* ── Main benchmark entry point ──────────────────────────────────────── */
void bench_run(const BenchParams *params, BenchResult *out) {
    memset(out, 0, sizeof(*out));
    out->min_latency_ms = LLONG_MAX;

    int total = params->total_requests;
    int threads = params->thread_count;
    if (threads < 1) threads = 1;
    if (total  < 1) total  = 1;

    /* Allocate latency array */
    long long *latencies = (long long *)calloc(total, sizeof(long long));
    if (!latencies) {
        ts_strlcpy(out->note, "Out of memory", sizeof(out->note));
        return;
    }

    volatile LONG completed = 0;
    volatile LONG failed    = 0;

    HANDLE *handles = (HANDLE *)calloc(threads, sizeof(HANDLE));
    BenchThreadArg *args = (BenchThreadArg *)calloc(threads, sizeof(BenchThreadArg));

    if (!handles || !args) {
        free(latencies);
        free(handles);
        free(args);
        ts_strlcpy(out->note, "Out of memory", sizeof(out->note));
        return;
    }

    /* Distribute requests across threads */
    int base     = total / threads;
    int leftover = total % threads;

    long long t_start = ts_now_ms();
    int offset = 0;

    for (int i = 0; i < threads; i++) {
        int count       = base + (i < leftover ? 1 : 0);
        args[i].params     = params;
        args[i].start_req  = offset;
        args[i].req_count  = count;
        args[i].latencies  = latencies + offset;
        args[i].completed  = &completed;
        args[i].failed     = &failed;
        handles[i] = CreateThread(NULL, 0, bench_thread_fn, &args[i], 0, NULL);
        offset += count;
    }

    WaitForMultipleObjects(threads, handles, TRUE, 60000);  /* 60-second timeout */
    long long t_end = ts_now_ms();

    for (int i = 0; i < threads; i++) CloseHandle(handles[i]);
    free(handles);
    free(args);

    out->completed        = completed;
    out->failed           = failed;
    out->total_duration_ms = t_end - t_start;

    if (out->total_duration_ms > 0) {
        out->rps = (double)out->completed * 1000.0
                   / (double)out->total_duration_ms;
    }

    /* Compute latency statistics from collected samples */
    int sample_count = 0;
    long long sum    = 0;
    for (int i = 0; i < total; i++) {
        if (latencies[i] > 0) {
            if (latencies[i] < out->min_latency_ms)
                out->min_latency_ms = latencies[i];
            if (latencies[i] > out->max_latency_ms)
                out->max_latency_ms = latencies[i];
            sum += latencies[i];
            sample_count++;
        }
    }

    if (sample_count > 0) {
        out->avg_latency_ms = (double)sum / (double)sample_count;

        /* Sort for percentiles */
        qsort(latencies, total, sizeof(long long), compare_ll);

        /* Skip leading zeroes (failed requests) */
        int first_valid = total - sample_count;
        if (sample_count >= 2) {
            out->p50_latency_ms = (double)latencies[first_valid + sample_count / 2];
            out->p95_latency_ms = (double)latencies[first_valid + (int)(sample_count * 0.95)];
            out->p99_latency_ms = (double)latencies[first_valid + (int)(sample_count * 0.99)];
        }
    }

    if (out->min_latency_ms == LLONG_MAX) out->min_latency_ms = 0;

    snprintf(out->note, sizeof(out->note),
        "%d threads, %d requests total. "
        "Results depend on hardware, OS scheduling, and server load.",
        threads, total);

    free(latencies);
}

/* ── Serialise to JSON ────────────────────────────────────────────────── */
void bench_result_to_json(const BenchResult *r, char *buf, int buf_size) {
    snprintf(buf, buf_size,
        "{"
        "\"completed\":%d,"
        "\"failed\":%d,"
        "\"total_duration_ms\":%lld,"
        "\"rps\":%.2f,"
        "\"avg_latency_ms\":%.2f,"
        "\"min_latency_ms\":%lld,"
        "\"max_latency_ms\":%lld,"
        "\"p50_latency_ms\":%.2f,"
        "\"p95_latency_ms\":%.2f,"
        "\"p99_latency_ms\":%.2f,"
        "\"note\":\"%s\""
        "}",
        r->completed, r->failed,
        r->total_duration_ms,
        r->rps,
        r->avg_latency_ms,
        r->min_latency_ms, r->max_latency_ms,
        r->p50_latency_ms, r->p95_latency_ms, r->p99_latency_ms,
        r->note
    );
}

/* ── Serialise to CSV ─────────────────────────────────────────────────── */
void bench_result_to_csv(const BenchResult *r, char *buf, int buf_size) {
    snprintf(buf, buf_size,
        "metric,value\r\n"
        "completed,%d\r\n"
        "failed,%d\r\n"
        "total_duration_ms,%lld\r\n"
        "rps,%.2f\r\n"
        "avg_latency_ms,%.2f\r\n"
        "min_latency_ms,%lld\r\n"
        "max_latency_ms,%lld\r\n"
        "p50_latency_ms,%.2f\r\n"
        "p95_latency_ms,%.2f\r\n"
        "p99_latency_ms,%.2f\r\n",
        r->completed, r->failed,
        r->total_duration_ms,
        r->rps,
        r->avg_latency_ms,
        r->min_latency_ms, r->max_latency_ms,
        r->p50_latency_ms, r->p95_latency_ms, r->p99_latency_ms
    );
}

