/*
 * ThreadStorm - sync_lab.c
 *
 * Race Condition Arena and Deadlock Prevention Lab.
 * These experiments run in isolated threads and do NOT use any global
 * server state.  They are driven by HTTP API calls from the dashboard.
 */

#include "sync_lab.h"

/* ═══════════════════════════════════════════════════════════════════════
 * PART 1 – Race Condition Arena
 * ═══════════════════════════════════════════════════════════════════════ */

/* Shared counter for the unsafe experiment */
static volatile long long g_unsafe_counter = 0;

/* Shared counter + mutex for the safe experiment */
static volatile long long g_safe_counter   = 0;
static CRITICAL_SECTION   g_safe_lock;

typedef struct {
    int  iterations;
    bool use_lock;
} CounterArg;

static DWORD WINAPI counter_thread(LPVOID arg) {
    CounterArg *ca = (CounterArg *)arg;

    if (ca->use_lock) {
        for (int i = 0; i < ca->iterations; i++) {
            EnterCriticalSection(&g_safe_lock);
            g_safe_counter++;
            LeaveCriticalSection(&g_safe_lock);
        }
    } else {
        for (int i = 0; i < ca->iterations; i++) {
            g_unsafe_counter++;   /* data race – intentional for demo */
        }
    }
    return 0;
}

static long long run_counter_test(int thread_count, int iterations, bool use_lock) {
    HANDLE  handles[SYNC_MAX_THREADS] = {0};
    CounterArg args[SYNC_MAX_THREADS];

    if (use_lock) g_safe_counter   = 0;
    else          g_unsafe_counter = 0;

    for (int i = 0; i < thread_count; i++) {
        args[i].iterations = iterations;
        args[i].use_lock   = use_lock;
        handles[i] = CreateThread(NULL, 0, counter_thread, &args[i], 0, NULL);
    }
    WaitForMultipleObjects(thread_count, handles, TRUE, INFINITE);
    for (int i = 0; i < thread_count; i++) CloseHandle(handles[i]);

    return use_lock ? g_safe_counter : g_unsafe_counter;
}

void sync_run_race(int thread_count, int iterations, RaceResult *out) {
    out->thread_count          = thread_count;
    out->iterations_per_thread = iterations;
    out->expected_value        = (long long)thread_count * (long long)iterations;

    InitializeCriticalSection(&g_safe_lock);

    /* Unsafe run */
    long long t0 = ts_now_ms();
    out->unsafe_result    = run_counter_test(thread_count, iterations, false);
    out->duration_unsafe_ms = ts_now_ms() - t0;

    /* Safe run */
    t0 = ts_now_ms();
    out->safe_result      = run_counter_test(thread_count, iterations, true);
    out->duration_safe_ms = ts_now_ms() - t0;

    DeleteCriticalSection(&g_safe_lock);

    out->race_detected = (out->unsafe_result != out->expected_value);

    snprintf(out->explanation, sizeof(out->explanation),
        "%d threads each incremented a counter %d times. "
        "Expected: %lld. "
        "Unsafe result: %lld (%s). "
        "Safe (mutex) result: %lld (%s). "
        "Race conditions are non-deterministic: the unsafe result "
        "may occasionally equal the expected value on lightly loaded systems.",
        thread_count, iterations,
        out->expected_value,
        out->unsafe_result,
        out->race_detected ? "DATA RACE DETECTED" : "no race this run",
        out->safe_result,
        out->safe_result == out->expected_value ? "CORRECT" : "WRONG – bug in safe path");
}

void sync_race_to_json(const RaceResult *r, char *buf, int buf_size) {
    snprintf(buf, buf_size,
        "{"
        "\"thread_count\":%d,"
        "\"iterations_per_thread\":%d,"
        "\"expected_value\":%lld,"
        "\"unsafe_result\":%lld,"
        "\"safe_result\":%lld,"
        "\"race_detected\":%s,"
        "\"duration_unsafe_ms\":%lld,"
        "\"duration_safe_ms\":%lld,"
        "\"explanation\":\"%s\""
        "}",
        r->thread_count,
        r->iterations_per_thread,
        r->expected_value,
        r->unsafe_result,
        r->safe_result,
        r->race_detected ? "true" : "false",
        r->duration_unsafe_ms,
        r->duration_safe_ms,
        r->explanation
    );
}

/* ═══════════════════════════════════════════════════════════════════════
 * PART 2 – Deadlock Handling & Prevention Lab (CO3)
 * ═══════════════════════════════════════════════════════════════════════
 *
 * Demonstrates:
 * 1. Deadlock Detection & Recovery:
 *    Threads request resources in opposing order (Thread 1: A→B, Thread 2: B→A).
 *    A deadlock cycle is detected via timed contention on the secondary lock.
 *    The thread breaks "Hold and Wait" by releasing its held lock, backing off,
 *    and retrying. This guarantees 100% completion with zero server freeze!
 *
 * 2. Deadlock Prevention:
 *    Strict linear lock hierarchy (All threads: A→B). Mathematically eliminates
 *    Coffman's Circular Wait condition. Zero contentions or backoffs needed.
 */

static CRITICAL_SECTION g_resource_A;
static CRITICAL_SECTION g_resource_B;
static volatile int     g_dl_safe_counter = 0;
static volatile int     g_dl_unsafe_counter = 0;
static volatile LONG    g_deadlocks_detected = 0;
static volatile LONG    g_recoveries_performed = 0;

typedef struct {
    int id;
    int iterations;
} DLArg;

/* Worker for Unsafe ordering with Detection & Recovery */
static DWORD WINAPI unsafe_detected_thread(LPVOID arg) {
    DLArg *da = (DLArg *)arg;
    bool order_AB = (da->id % 2 != 0); /* Odd threads: A then B; Even threads: B then A */

    for (int i = 0; i < da->iterations; i++) {
        bool acquired = false;

        while (!acquired) {
            if (order_AB) {
                EnterCriticalSection(&g_resource_A);
                /* Try to acquire B without indefinite freeze */
                bool got_B = false;
                for (int attempt = 0; attempt < 40; attempt++) {
                    if (TryEnterCriticalSection(&g_resource_B)) {
                        got_B = true;
                        break;
                    }
                    Sleep(0); /* Yield CPU slice */
                }

                if (got_B) {
                    /* Both locks held safely */
                    g_dl_unsafe_counter++;
                    LeaveCriticalSection(&g_resource_B);
                    LeaveCriticalSection(&g_resource_A);
                    acquired = true;
                } else {
                    /* Deadlock condition detected! Handle via Preemption / Backoff */
                    InterlockedIncrement(&g_deadlocks_detected);
                    LeaveCriticalSection(&g_resource_A); /* Break Hold & Wait */
                    InterlockedIncrement(&g_recoveries_performed);
                    Sleep(1 + (da->id % 3)); /* Backoff jitter */
                }
            } else {
                /* Even thread: B then A (Opposite order creating circular wait risk) */
                EnterCriticalSection(&g_resource_B);
                bool got_A = false;
                for (int attempt = 0; attempt < 40; attempt++) {
                    if (TryEnterCriticalSection(&g_resource_A)) {
                        got_A = true;
                        break;
                    }
                    Sleep(0);
                }

                if (got_A) {
                    g_dl_unsafe_counter++;
                    LeaveCriticalSection(&g_resource_A);
                    LeaveCriticalSection(&g_resource_B);
                    acquired = true;
                } else {
                    InterlockedIncrement(&g_deadlocks_detected);
                    LeaveCriticalSection(&g_resource_B); /* Break Hold & Wait */
                    InterlockedIncrement(&g_recoveries_performed);
                    Sleep(1 + ((da->id + 1) % 3));
                }
            }
        }
    }
    return 0;
}

/* Worker for Safe Linear Ordering (Deadlock Prevention) */
static DWORD WINAPI safe_ordered_thread(LPVOID arg) {
    DLArg *da = (DLArg *)arg;

    for (int i = 0; i < da->iterations; i++) {
        /* Always acquire A → B (consistent order = no circular wait) */
        EnterCriticalSection(&g_resource_A);
        EnterCriticalSection(&g_resource_B);

        g_dl_safe_counter++;

        LeaveCriticalSection(&g_resource_B);
        LeaveCriticalSection(&g_resource_A);
    }
    return 0;
}

void sync_run_deadlock_demo(int thread_count, DeadlockResult *out) {
    thread_count = TS_CLAMP(thread_count, 2, 4);
    memset(out, 0, sizeof(*out));
    ts_strlcpy(out->scenario, "detection_and_prevention", sizeof(out->scenario));

    int iters = 5000;
    out->expected_value = thread_count * iters;

    InitializeCriticalSection(&g_resource_A);
    InitializeCriticalSection(&g_resource_B);

    /* ── Phase 1: Unsafe Circular Wait with Detection & Recovery ── */
    g_dl_unsafe_counter = 0;
    g_deadlocks_detected = 0;
    g_recoveries_performed = 0;

    HANDLE handles[4];
    DLArg  args[4];

    long long t0 = ts_now_ms();
    for (int i = 0; i < thread_count; i++) {
        args[i].id         = i + 1;
        args[i].iterations = iters;
        handles[i] = CreateThread(NULL, 0, unsafe_detected_thread, &args[i], 0, NULL);
    }
    DWORD wait_unsafe = WaitForMultipleObjects(thread_count, handles, TRUE, 8000);
    out->duration_unsafe_ms = ts_now_ms() - t0;
    for (int i = 0; i < thread_count; i++) CloseHandle(handles[i]);

    out->deadlocks_detected   = (int)g_deadlocks_detected;
    out->recoveries_performed = (int)g_recoveries_performed;
    out->counter_unsafe       = g_dl_unsafe_counter;

    /* ── Phase 2: Safe Linear Lock Hierarchy (Prevention) ── */
    g_dl_safe_counter = 0;
    long long t1 = ts_now_ms();
    for (int i = 0; i < thread_count; i++) {
        args[i].id         = i + 1;
        args[i].iterations = iters;
        handles[i] = CreateThread(NULL, 0, safe_ordered_thread, &args[i], 0, NULL);
    }
    DWORD wait_safe = WaitForMultipleObjects(thread_count, handles, TRUE, 8000);
    out->duration_safe_ms = ts_now_ms() - t1;
    for (int i = 0; i < thread_count; i++) CloseHandle(handles[i]);

    out->counter_safe = g_dl_safe_counter;
    out->duration_ms  = out->duration_unsafe_ms + out->duration_safe_ms;

    DeleteCriticalSection(&g_resource_A);
    DeleteCriticalSection(&g_resource_B);

    if (wait_unsafe == WAIT_TIMEOUT || wait_safe == WAIT_TIMEOUT) {
        ts_strlcpy(out->result, "timeout", sizeof(out->result));
    } else {
        ts_strlcpy(out->result, "completed", sizeof(out->result));
    }

    snprintf(out->explanation, sizeof(out->explanation),
        "CO3 Deadlock Verification (%d threads, %d iterations/thread):\n\n"
        "1. DETECTION & RECOVERY (Unsafe Lock Order): Odd threads acquire A->B; "
        "Even threads acquire B->A. Interleaving triggered %d deadlock conditions. "
        "Each deadlock was detected via try-lock contention, recovered by releasing held locks "
        "(breaking Hold & Wait) and backing off (%d recoveries). Final counter: %d/%d in %lld ms.\n\n"
        "2. PREVENTION (Strict Lock Hierarchy): All threads strictly acquire Lock A before Lock B. "
        "Circular wait was mathematically eliminated. Zero deadlocks detected. Final counter: %d/%d in %lld ms.",
        thread_count, iters,
        out->deadlocks_detected, out->recoveries_performed,
        out->counter_unsafe, out->expected_value, out->duration_unsafe_ms,
        out->counter_safe, out->expected_value, out->duration_safe_ms
    );
}

void sync_deadlock_to_json(const DeadlockResult *r, char *buf, int buf_size) {
    /* Escape newlines in explanation */
    char esc[1800];
    int j = 0;
    for (int i = 0; r->explanation[i] && j < (int)sizeof(esc) - 3; i++) {
        if (r->explanation[i] == '\n') {
            esc[j++] = '\\'; esc[j++] = 'n';
        } else if (r->explanation[i] == '"') {
            esc[j++] = '\\'; esc[j++] = '"';
        } else {
            esc[j++] = r->explanation[i];
        }
    }
    esc[j] = '\0';

    snprintf(buf, buf_size,
        "{"
        "\"scenario\":\"%s\","
        "\"result\":\"%s\","
        "\"duration_ms\":%lld,"
        "\"duration_unsafe_ms\":%lld,"
        "\"duration_safe_ms\":%lld,"
        "\"deadlocks_detected\":%d,"
        "\"recoveries_performed\":%d,"
        "\"counter_unsafe\":%d,"
        "\"counter_safe\":%d,"
        "\"expected_value\":%d,"
        "\"explanation\":\"%s\""
        "}",
        r->scenario, r->result,
        r->duration_ms,
        r->duration_unsafe_ms,
        r->duration_safe_ms,
        r->deadlocks_detected,
        r->recoveries_performed,
        r->counter_unsafe,
        r->counter_safe,
        r->expected_value,
        esc
    );
}

