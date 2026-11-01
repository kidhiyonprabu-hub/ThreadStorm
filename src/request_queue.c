/*
 * ThreadStorm - request_queue.c
 *
 * Bounded ring-buffer queue protected by a CRITICAL_SECTION
 * and two CONDITION_VARIABLEs (Vista+ API, no extra libraries needed).
 *
 * Key design points:
 *   • Enqueue blocks (with timeout) if full → prevents unbounded memory use.
 *   • Dequeue blocks until an item is ready or shutdown is set.
 *   • Both functions check the shutdown flag to exit cleanly.
 */

#include "request_queue.h"

bool rq_init(RequestQueue *q, int capacity) {
    if (capacity < 1 || capacity > QUEUE_MAX_CAPACITY) {
        capacity = QUEUE_DEFAULT_CAPACITY;
    }

    q->slots = (QueueSlot *)calloc(capacity, sizeof(QueueSlot));
    if (!q->slots) return false;

    q->capacity        = capacity;
    q->head            = 0;
    q->tail            = 0;
    q->count           = 0;
    q->shutdown        = false;
    q->total_enqueued  = 0;
    q->total_dequeued  = 0;
    q->total_dropped   = 0;
    q->next_request_id = 1;

    InitializeCriticalSection(&q->lock);
    InitializeConditionVariable(&q->not_empty);
    InitializeConditionVariable(&q->not_full);

    return true;
}

bool rq_enqueue(RequestQueue *q, SOCKET client_sock) {
    EnterCriticalSection(&q->lock);

    /*
     * If the queue is full, wait up to 1000 ms for a slot to open.
     * This applies back-pressure to the accept loop so we don't drop
     * connections silently.
     */
    while (q->count >= q->capacity && !q->shutdown) {
        if (!SleepConditionVariableCS(&q->not_full, &q->lock, 1000)) {
            /* Timed out or spurious wake — give up and drop this socket. */
            q->total_dropped++;
            LeaveCriticalSection(&q->lock);
            closesocket(client_sock);
            LOG_WARN("Queue full – connection dropped (capacity=%d)", q->capacity);
            return false;
        }
    }

    if (q->shutdown) {
        LeaveCriticalSection(&q->lock);
        closesocket(client_sock);
        return false;
    }

    /* Write into the ring buffer */
    int rid = q->next_request_id++;
    q->slots[q->tail].client_sock     = client_sock;
    q->slots[q->tail].enqueue_time_ms = ts_now_ms();
    q->slots[q->tail].request_id      = rid;
    q->tail = (q->tail + 1) % q->capacity;
    q->count++;
    q->total_enqueued++;

    /* Wake one waiting worker */
    WakeConditionVariable(&q->not_empty);

    LeaveCriticalSection(&q->lock);
    return true;
}

SOCKET rq_dequeue(RequestQueue *q, long long *enqueue_time_ms_out,
                  int *request_id_out) {
    EnterCriticalSection(&q->lock);

    /* Wait until there is work or we are told to stop */
    while (q->count == 0 && !q->shutdown) {
        SleepConditionVariableCS(&q->not_empty, &q->lock, INFINITE);
    }

    if (q->count == 0) {
        /* Shutdown with empty queue */
        LeaveCriticalSection(&q->lock);
        if (enqueue_time_ms_out) *enqueue_time_ms_out = 0;
        if (request_id_out)      *request_id_out      = 0;
        return INVALID_SOCKET;
    }

    QueueSlot slot = q->slots[q->head];
    q->head  = (q->head + 1) % q->capacity;
    q->count--;
    q->total_dequeued++;

    /* Wake a waiting enqueuer */
    WakeConditionVariable(&q->not_full);

    LeaveCriticalSection(&q->lock);

    if (enqueue_time_ms_out) *enqueue_time_ms_out = slot.enqueue_time_ms;
    if (request_id_out)      *request_id_out      = slot.request_id;
    return slot.client_sock;
}

void rq_signal_shutdown(RequestQueue *q) {
    EnterCriticalSection(&q->lock);
    q->shutdown = true;
    /* Wake ALL waiters so they can check the flag and exit */
    WakeAllConditionVariable(&q->not_empty);
    WakeAllConditionVariable(&q->not_full);
    LeaveCriticalSection(&q->lock);
}

void rq_destroy(RequestQueue *q) {
    free(q->slots);
    q->slots = NULL;
    DeleteCriticalSection(&q->lock);
    /* Condition variables don't need explicit destruction on Windows */
}

