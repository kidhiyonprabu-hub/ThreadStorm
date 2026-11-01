/*
 * ThreadStorm - thread_pool.h
 *
 * Manages a fixed or adaptive pool of worker threads.
 * Each worker pulls client sockets from a RequestQueue and handles HTTP.
 */

#ifndef THREAD_POOL_H
#define THREAD_POOL_H

#include "common.h"
#include "request_queue.h"

/* ── Worker states (for the dashboard visualizer) ─────────────────────── */
typedef enum {
    WORKER_IDLE    = 0,   /* waiting for work */
    WORKER_BUSY    = 1,   /* currently handling a request */
    WORKER_STOPPED = 2    /* thread has exited */
} WorkerState;

/* ── Per-worker information ───────────────────────────────────────────── */
typedef struct {
    int           worker_id;
    HANDLE        thread_handle;
    volatile WorkerState state;
    volatile int  requests_handled;
    volatile long long last_active_ms;
    volatile int  current_request_id;   /* 0 if idle */
} WorkerInfo;

/* ── Thread pool ──────────────────────────────────────────────────────── */
typedef struct {
    WorkerInfo      *workers;           /* array of worker descriptors */
    int              worker_count;      /* current number of workers */
    int              max_workers;
    int              min_workers;

    RequestQueue    *queue;             /* shared request queue */

    volatile bool    shutdown;          /* set true to stop all workers */
    CRITICAL_SECTION pool_lock;         /* protects worker array mutations */

    /* adaptive pool support */
    bool             adaptive_enabled;
    HANDLE           adaptive_thread;
    volatile long long last_scale_ms;
    char             last_scale_reason[128];
} ThreadPool;

/* ── Public API ──────────────────────────────────────────────────────── */

/* Initialise pool.  Does NOT start workers yet. */
bool tp_init(ThreadPool *pool, RequestQueue *queue,
             int worker_count, int min_workers, int max_workers);

/* Start all worker threads. */
bool tp_start(ThreadPool *pool);

/* Stop all workers and wait for them to finish. */
void tp_shutdown(ThreadPool *pool);

/* Free resources.  Call after tp_shutdown. */
void tp_destroy(ThreadPool *pool);

/* Enable/disable adaptive scaling. */
void tp_set_adaptive(ThreadPool *pool, bool enabled);

/* Snapshot of worker states for dashboard (caller allocates out_states[max]).
   Returns actual count written. */
int  tp_snapshot_workers(const ThreadPool *pool, WorkerInfo *out_states, int max);

/* Count workers in each state. */
void tp_count_states(const ThreadPool *pool,
                     int *idle, int *busy, int *stopped);

/* Add/remove one worker (called by adaptive controller). */
bool tp_add_worker(ThreadPool *pool);
bool tp_remove_worker(ThreadPool *pool);

#endif /* THREAD_POOL_H */

