/*
 * ThreadStorm - sync_lab.h
 *
 * Isolated synchronization experiments:
 *   1. Race Condition Arena  – counter without vs. with mutex
 *   2. Deadlock Prevention Lab – safe lock-ordering demonstration
 *
 * These experiments run in temporary threads and do NOT touch the
 * production server's shared state.
 */

#ifndef SYNC_LAB_H
#define SYNC_LAB_H

#include "common.h"

/* ── Race Condition Arena ─────────────────────────────────────────────── */

typedef struct {
    int       thread_count;
    int       iterations_per_thread;
    long long expected_value;

    long long unsafe_result;
    long long duration_unsafe_ms;

    long long safe_result;
    long long duration_safe_ms;

    bool      race_detected;   /* unsafe_result != expected_value */
    char      explanation[512];
} RaceResult;

/* Run the race experiment synchronously.
   thread_count: 1–SYNC_MAX_THREADS
   iterations:   1–SYNC_MAX_ITERATIONS */
void sync_run_race(int thread_count, int iterations, RaceResult *out);

/* Serialise RaceResult to JSON into buf (caller supplies ≥2048 bytes). */
void sync_race_to_json(const RaceResult *r, char *buf, int buf_size);


/* ── Deadlock Handling & Prevention Lab (CO3) ────────────────────────── */

typedef struct {
    char      scenario[64];    /* "detection_and_prevention" | "safe_ordered" */
    char      result[64];      /* "completed" | "timeout" */
    long long duration_ms;

    /* Unsafe scenario with dynamic detection & recovery */
    int       deadlocks_detected;
    int       recoveries_performed;
    long long duration_unsafe_ms;
    int       counter_unsafe;

    /* Safe scenario with linear ordering prevention */
    long long duration_safe_ms;
    int       counter_safe;

    int       expected_value;
    char      explanation[1536];
} DeadlockResult;

/* Demonstrate Deadlock Detection & Recovery (Unsafe ordering with
   try-lock backoff) and Deadlock Prevention (Strict Linear Lock Ordering).
   thread_count: 2–4 threads. */
void sync_run_deadlock_demo(int thread_count, DeadlockResult *out);

/* Serialise DeadlockResult to JSON. */
void sync_deadlock_to_json(const DeadlockResult *r, char *buf, int buf_size);

#endif /* SYNC_LAB_H */

