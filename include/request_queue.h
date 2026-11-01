/*
 * ThreadStorm - request_queue.h
 *
 * Bounded, thread-safe FIFO queue of accepted client sockets.
 * Workers dequeue one socket at a time; the listener enqueues.
 *
 * Synchronization:
 *   - CRITICAL_SECTION  protects head, tail, count, and the slot array.
 *   - CONDITION_VARIABLE not_empty — workers wait here when queue is empty.
 *   - CONDITION_VARIABLE not_full  — listener waits here when queue is full.
 */

#ifndef REQUEST_QUEUE_H
#define REQUEST_QUEUE_H

#include "common.h"

/* ── One slot in the ring-buffer ──────────────────────────────────────── */
typedef struct {
    SOCKET  client_sock;
    long long enqueue_time_ms;   /* for latency tracking */
    int       request_id;
} QueueSlot;

/* ── The queue itself ─────────────────────────────────────────────────── */
typedef struct RequestQueue {
    QueueSlot        *slots;          /* ring buffer (heap-allocated) */
    int               capacity;       /* maximum number of slots */
    int               head;           /* dequeue index */
    int               tail;           /* enqueue index */
    volatile int      count;          /* current item count   (atomic read OK) */
    volatile bool     shutdown;       /* set true to unblock all waiters */

    CRITICAL_SECTION  lock;
    CONDITION_VARIABLE not_empty;
    CONDITION_VARIABLE not_full;

    /* statistics (protected by lock) */
    int               total_enqueued;
    int               total_dequeued;
    int               total_dropped;     /* dropped when full and no-wait */
    int               next_request_id;
} RequestQueue;

/* ── Public API ──────────────────────────────────────────────────────── */

/* Allocate and initialise the queue with the given capacity.
   Returns true on success. */
bool  rq_init(RequestQueue *q, int capacity);

/* Enqueue a client socket.
   Blocks (up to ~1 s) if queue is full, then returns false.
   Returns false immediately if shutdown is set.
   Returns true on success. */
bool  rq_enqueue(RequestQueue *q, SOCKET client_sock);

/* Dequeue a client socket.
   Blocks until an item is available or shutdown is set.
   Returns INVALID_SOCKET when shut down with no item available. */
SOCKET rq_dequeue(RequestQueue *q, long long *enqueue_time_ms_out,
                  int *request_id_out);

/* Signal all waiters to wake and check the shutdown flag. */
void  rq_signal_shutdown(RequestQueue *q);

/* Free resources.  Call after all threads have exited. */
void  rq_destroy(RequestQueue *q);

/* Read current count without locking (approximate, for display). */
static inline int rq_count_approx(const RequestQueue *q) {
    return q->count;
}

#endif /* REQUEST_QUEUE_H */

