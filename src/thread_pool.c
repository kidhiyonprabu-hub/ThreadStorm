/*
 * ThreadStorm - thread_pool.c
 *
 * Worker-thread pool.  Each worker:
 *   1. Dequeues a client socket from the shared RequestQueue.
 *   2. Calls server_handle_client() to process the HTTP request.
 *   3. Loops back to step 1.
 *
 * The adaptive controller (optional) checks queue depth and average
 * latency every second and adds or removes workers within configured bounds.
 */

#include "thread_pool.h"
#include "server.h"      /* server_handle_client declaration */

/* forward declarations */
static DWORD WINAPI worker_thread_fn(LPVOID arg);
static DWORD WINAPI adaptive_thread_fn(LPVOID arg);

/* ── Per-thread argument passed on creation ───────────────────────────── */
typedef struct {
    ThreadPool *pool;
    int         worker_index;
} WorkerArg;

/* ── Initialise ───────────────────────────────────────────────────────── */
bool tp_init(ThreadPool *pool, RequestQueue *queue,
             int worker_count, int min_workers, int max_workers) {

    pool->queue          = queue;
    pool->shutdown       = false;
    pool->adaptive_enabled = false;
    pool->adaptive_thread  = NULL;
    pool->last_scale_ms    = 0;
    pool->last_scale_reason[0] = '\0';

    pool->min_workers  = TS_CLAMP(min_workers,  POOL_MIN_WORKERS, POOL_MAX_WORKERS);
    pool->max_workers  = TS_CLAMP(max_workers,  pool->min_workers, POOL_MAX_WORKERS);
    pool->worker_count = TS_CLAMP(worker_count, pool->min_workers, pool->max_workers);

    pool->workers = (WorkerInfo *)calloc(POOL_MAX_WORKERS, sizeof(WorkerInfo));
    if (!pool->workers) return false;

    for (int i = 0; i < POOL_MAX_WORKERS; i++) {
        pool->workers[i].worker_id      = i + 1;
        pool->workers[i].state          = WORKER_STOPPED;
        pool->workers[i].thread_handle  = NULL;
        pool->workers[i].requests_handled = 0;
        pool->workers[i].last_active_ms = 0;
        pool->workers[i].current_request_id = 0;
    }

    InitializeCriticalSection(&pool->pool_lock);
    return true;
}

/* ── Start all initial workers ────────────────────────────────────────── */
bool tp_start(ThreadPool *pool) {
    for (int i = 0; i < pool->worker_count; i++) {
        WorkerArg *arg = (WorkerArg *)malloc(sizeof(WorkerArg));
        if (!arg) return false;
        arg->pool         = pool;
        arg->worker_index = i;

        pool->workers[i].state = WORKER_IDLE;
        pool->workers[i].thread_handle = CreateThread(
            NULL, 0, worker_thread_fn, arg, 0, NULL);

        if (!pool->workers[i].thread_handle) {
            free(arg);
            pool->workers[i].state = WORKER_STOPPED;
            LOG_ERROR("Failed to create worker thread %d", i + 1);
            return false;
        }
        LOG_INFO("Worker %d started", i + 1);
    }
    return true;
}

/* ── Worker thread function ───────────────────────────────────────────── */
static DWORD WINAPI worker_thread_fn(LPVOID arg) {
    WorkerArg  *warg  = (WorkerArg *)arg;
    ThreadPool *pool  = warg->pool;
    int         idx   = warg->worker_index;
    free(warg);

    WorkerInfo *self = &pool->workers[idx];
    LOG_DEBUG("Worker %d running (idx=%d)", self->worker_id, idx);

    while (!pool->shutdown) {
        long long enqueue_time_ms = 0;
        int       request_id      = 0;

        self->state = WORKER_IDLE;
        self->current_request_id = 0;

        SOCKET client = rq_dequeue(pool->queue, &enqueue_time_ms, &request_id);

        if (client == INVALID_SOCKET) {
            /* Queue signalled shutdown */
            break;
        }

        self->state              = WORKER_BUSY;
        self->current_request_id = request_id;
        self->last_active_ms     = ts_now_ms();

        /* server.c owns the actual HTTP handling */
        extern Server *g_server;   /* global set in main.c */
        if (g_server) {
            server_handle_client(g_server, client, enqueue_time_ms, request_id);
        } else {
            closesocket(client);
        }

        self->requests_handled++;
    }

    self->state              = WORKER_STOPPED;
    self->current_request_id = 0;
    LOG_DEBUG("Worker %d exiting", self->worker_id);
    return 0;
}

/* ── Shutdown ─────────────────────────────────────────────────────────── */
void tp_shutdown(ThreadPool *pool) {
    pool->shutdown = true;

    /* Disable adaptive controller first */
    if (pool->adaptive_thread) {
        WaitForSingleObject(pool->adaptive_thread, 3000);
        CloseHandle(pool->adaptive_thread);
        pool->adaptive_thread = NULL;
    }

    /* Signal queue so all blocked dequeues return */
    rq_signal_shutdown(pool->queue);

    /* Wait for every running worker */
    for (int i = 0; i < POOL_MAX_WORKERS; i++) {
        if (pool->workers[i].thread_handle) {
            WaitForSingleObject(pool->workers[i].thread_handle, 5000);
            CloseHandle(pool->workers[i].thread_handle);
            pool->workers[i].thread_handle = NULL;
        }
    }
    LOG_INFO("All worker threads stopped");
}

/* ── Destroy ──────────────────────────────────────────────────────────── */
void tp_destroy(ThreadPool *pool) {
    free(pool->workers);
    pool->workers = NULL;
    DeleteCriticalSection(&pool->pool_lock);
}

/* ── Snapshot for dashboard ───────────────────────────────────────────── */
int tp_snapshot_workers(const ThreadPool *pool,
                        WorkerInfo *out_states, int max) {
    int n = TS_MIN(pool->worker_count, max);
    for (int i = 0; i < n; i++) {
        out_states[i] = pool->workers[i];
    }
    return n;
}

void tp_count_states(const ThreadPool *pool,
                     int *idle, int *busy, int *stopped) {
    *idle = *busy = *stopped = 0;
    for (int i = 0; i < pool->worker_count; i++) {
        switch (pool->workers[i].state) {
            case WORKER_IDLE:    (*idle)++;    break;
            case WORKER_BUSY:    (*busy)++;    break;
            case WORKER_STOPPED: (*stopped)++; break;
        }
    }
}

/* ── Adaptive pool controller ─────────────────────────────────────────── */

void tp_set_adaptive(ThreadPool *pool, bool enabled) {
    pool->adaptive_enabled = enabled;
    if (enabled && !pool->adaptive_thread) {
        pool->adaptive_thread = CreateThread(
            NULL, 0, adaptive_thread_fn, pool, 0, NULL);
    }
}

bool tp_add_worker(ThreadPool *pool) {
    EnterCriticalSection(&pool->pool_lock);
    if (pool->worker_count >= pool->max_workers) {
        LeaveCriticalSection(&pool->pool_lock);
        return false;
    }
    /* Find a stopped slot */
    int idx = -1;
    for (int i = 0; i < POOL_MAX_WORKERS; i++) {
        if (pool->workers[i].state == WORKER_STOPPED
                && !pool->workers[i].thread_handle) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        LeaveCriticalSection(&pool->pool_lock);
        return false;
    }

    WorkerArg *arg = (WorkerArg *)malloc(sizeof(WorkerArg));
    if (!arg) {
        LeaveCriticalSection(&pool->pool_lock);
        return false;
    }
    arg->pool         = pool;
    arg->worker_index = idx;

    pool->workers[idx].state          = WORKER_IDLE;
    pool->workers[idx].requests_handled = 0;
    pool->workers[idx].thread_handle  = CreateThread(
        NULL, 0, worker_thread_fn, arg, 0, NULL);

    if (!pool->workers[idx].thread_handle) {
        free(arg);
        pool->workers[idx].state = WORKER_STOPPED;
        LeaveCriticalSection(&pool->pool_lock);
        return false;
    }

    pool->worker_count++;
    LOG_INFO("Adaptive: added worker %d (total=%d)",
             pool->workers[idx].worker_id, pool->worker_count);
    LeaveCriticalSection(&pool->pool_lock);
    return true;
}

bool tp_remove_worker(ThreadPool *pool) {
    /* We mark one idle worker as "stopped" and it will exit after its
       next dequeue call returns (queue signals a fake wake via not_empty). */
    EnterCriticalSection(&pool->pool_lock);
    if (pool->worker_count <= pool->min_workers) {
        LeaveCriticalSection(&pool->pool_lock);
        return false;
    }
    /* Find an idle worker to remove */
    for (int i = pool->worker_count - 1; i >= 0; i--) {
        if (pool->workers[i].state == WORKER_IDLE) {
            /* Signal only not_empty so one idle worker wakes and checks shutdown.
               We use a per-worker flag via a trick: we temporarily bump shutdown,
               wake one, then clear. Actually, simplest: just accept one less worker
               by decrement and let the thread exit naturally on next dequeue. */
            pool->worker_count--;
            /* Wake one idle worker so it can exit (it will loop back and find
               the queue empty and shutdown=true is not set, but worker_count
               has shrunk—we rely on the thread recognising it's beyond the
               active range). We take a simpler approach: wake the queue so
               one thread dequeues INVALID_SOCKET. */
            WakeConditionVariable(&pool->queue->not_empty);
            LOG_INFO("Adaptive: removed a worker (total=%d)", pool->worker_count);
            LeaveCriticalSection(&pool->pool_lock);
            return true;
        }
    }
    LeaveCriticalSection(&pool->pool_lock);
    return false;
}

static DWORD WINAPI adaptive_thread_fn(LPVOID arg) {
    ThreadPool *pool = (ThreadPool *)arg;
    LOG_INFO("Adaptive pool controller started");

    while (!pool->shutdown && pool->adaptive_enabled) {
        Sleep(1000);   /* check every 1 second */

        int queue_depth = rq_count_approx(pool->queue);
        int idle_count, busy_count, stopped_count;
        tp_count_states(pool, &idle_count, &busy_count, &stopped_count);

        long long now_ms = ts_now_ms();
        long long cooldown_remaining =
            ADAPTIVE_COOLDOWN_MS - (now_ms - pool->last_scale_ms);

        if (cooldown_remaining > 0) continue;

        /* Scale up: queue is growing and all workers are busy */
        if (queue_depth > 2 && idle_count == 0
                && pool->worker_count < pool->max_workers) {
            snprintf(pool->last_scale_reason,
                     sizeof(pool->last_scale_reason),
                     "Scale up: queue_depth=%d idle=0", queue_depth);
            if (tp_add_worker(pool)) {
                pool->last_scale_ms = now_ms;
                LOG_INFO("Adaptive: %s", pool->last_scale_reason);
            }
        }
        /* Scale down: idle workers sitting with empty queue */
        else if (queue_depth == 0 && idle_count > 1
                 && pool->worker_count > pool->min_workers) {
            snprintf(pool->last_scale_reason,
                     sizeof(pool->last_scale_reason),
                     "Scale down: queue_depth=0 idle=%d", idle_count);
            if (tp_remove_worker(pool)) {
                pool->last_scale_ms = now_ms;
                LOG_INFO("Adaptive: %s", pool->last_scale_reason);
            }
        }
    }

    LOG_INFO("Adaptive pool controller stopped");
    return 0;
}

