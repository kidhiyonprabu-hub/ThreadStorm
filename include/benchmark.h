/*
 * ThreadStorm - benchmark.h
 *
 * Sequential vs. multithreaded benchmark:
 * Issues N HTTP GET requests and measures throughput and latency.
 */

#ifndef BENCHMARK_H
#define BENCHMARK_H

#include "common.h"

/* ── Benchmark parameters ─────────────────────────────────────────────── */
typedef struct {
    int   total_requests;   /* 1 – BENCH_MAX_REQUESTS */
    int   thread_count;     /* 1 = sequential, >1 = concurrent */
    char  target_host[64];
    int   target_port;
    char  target_uri[HTTP_MAX_URI_LEN];
} BenchParams;

/* ── Benchmark result ─────────────────────────────────────────────────── */
typedef struct {
    int       completed;
    int       failed;
    long long total_duration_ms;
    double    rps;
    double    avg_latency_ms;
    double    p50_latency_ms;
    double    p95_latency_ms;
    double    p99_latency_ms;
    long long min_latency_ms;
    long long max_latency_ms;
    char      note[256];
} BenchResult;

/* ── Public API ──────────────────────────────────────────────────────── */

/* Run benchmark synchronously.  May take several seconds. */
void bench_run(const BenchParams *params, BenchResult *out);

/* Serialise result to JSON (caller supplies ≥1024 bytes). */
void bench_result_to_json(const BenchResult *r, char *buf, int buf_size);

/* Serialise result to CSV. */
void bench_result_to_csv(const BenchResult *r, char *buf, int buf_size);

#endif /* BENCHMARK_H */

