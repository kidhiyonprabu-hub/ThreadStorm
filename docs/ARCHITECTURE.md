# ThreadStorm Architecture & Technical Specifications

This document outlines the internal architectural design, synchronization primitives, memory structures, and network lifecycles of ThreadStorm.

---

## 1. Concurrency Architecture

ThreadStorm utilizes a **Producer-Consumer Thread Pool Model** built on top of native Windows synchronization APIs.

```
+--------------------------------------------------------------------------+
|                            PROCESS ADDRESS SPACE                         |
|                                                                          |
|   +-----------------------+                                              |
|   |  Accept Thread (Main) |                                              |
|   |  - Winsock accept()   |                                              |
|   +-----------+-----------+                                              |
|               |                                                          |
|      Enqueues client socket (Holds queue lock)                           |
|               |                                                          |
|               v                                                          |
|   +------------------------------------------------------------------+   |
|   |                      BOUNDED REQUEST QUEUE                       |   |
|   |  - Ring Buffer: QueueSlot slots[capacity]                        |   |
|   |  - Guards: CRITICAL_SECTION lock                                 |   |
|   |  - Conditions: CONDITION_VARIABLE not_empty, not_full            |   |
|   +------------------------------------------------------------------+   |
|               |                           |                              |
|     WakeConditionVariable       WakeConditionVariable                    |
|               v                           v                              |
|   +-----------------------+   +-----------------------+                  |
|   |   Worker Thread #1    |   |   Worker Thread #2    |  ...             |
|   | - SleepConditionVariable  | - SleepConditionVariable                 |
|   | - Dequeues client sock|   | - Dequeues client sock|                  |
|   | - Serves HTTP request |   | - Serves HTTP request |                  |
|   | - Closes client sock  |   | - Closes client sock  |                  |
|   +-----------+-----------+   +-----------+-----------+                  |
|               |                           |                              |
|               +-------------+-------------+                              |
|                             |                                            |
|            Atomic Interlocked Increment / Metrics Lock                   |
|                             v                                            |
|   +------------------------------------------------------------------+   |
|   |                   SHARED METRICS & TELEMETRY                     |   |
|   |  - Total requests, success/error responses                       |   |
|   |  - Latency sum, min/max latency                                  |   |
|   |  - Rolling 60s RPS ring buffer                                   |   |
|   +------------------------------------------------------------------+   |
|                                                                          |
+--------------------------------------------------------------------------+
```

---

## 2. Request Processing Sequence Diagram

```
Client                   Accept Thread              Queue               Worker Thread
  |                            |                      |                       |
  |--- TCP Connect Request --->|                      |                       |
  |    (SYN -> SYN/ACK)        |                      |                       |
  |                            |-- Enqueue Socket --->|                       |
  |                            |   (Acquires Lock)    |                       |
  |                            |   (Inserts to tail)  |                       |
  |                            |   (Signals CV) ----->|                       |
  |                            |                      |-- Wake Worker ------->|
  |                            |                      |   (Acquires Lock)     |
  |                            |                      |   (Pulls from head)   |
  |                            |                      |<-- Return Socket -----|
  |                            |                      |                       |
  |--- HTTP GET /index.html ------------------------------------------------->|
  |                            |                      |   (Parses Method/URI) |
  |                            |                      |   (Validates Safety)  |
  |                            |                      |   (Reads file bytes)  |
  |<-- HTTP/1.1 200 OK -------------------------------------------------------|
  |<-- Content Payload -------------------------------------------------------|
  |                            |                      |   (Updates Metrics)   |
  |--- TCP FIN / Close ------------------------------------------------------>|
  |                            |                      |   (closesocket)       |
  |                            |                      |   (Re-enters CV wait) |
```

---

## 3. Synchronization Primitives Comparison

| Primitive | Used In ThreadStorm | Purpose & Mechanical Advantage |
| :--- | :--- | :--- |
| **`CRITICAL_SECTION`** | `request_queue.c`, `server.c`, `metrics.c` | User-mode fast path locking. Avoids transitioning into ring-0 kernel space when uncontended, providing sub-10ns lock acquisition. |
| **`CONDITION_VARIABLE`** | `request_queue.c` (`not_empty`, `not_full`) | Sleep/wake signaling. Allows workers to sleep with zero CPU burn and awaken atomically upon queue mutation via `WakeConditionVariable()`. |
| **`InterlockedIncrement` / `InterlockedDecrement`** | `metrics.c`, `benchmark.c` | Lock-free hardware atomic instructions (`LOCK XADD` on x86/x64). Completely eliminates lock contention for monotonic counters. |
| **Monotonic QPC (`QueryPerformanceCounter`)** | `common.c` | Microsecond-accurate latency measurements immune to system clock adjustments or daylight savings shifts. |

---

## 4. Memory Safety & Buffer Bounds

1. **Request Queue Ring Buffer**:
   - Pre-allocated on heap upon initialization.
   - Fixed capacity (`64` default, maximum `256`).
   - Head and tail pointers advance with modulo arithmetic (`(pos + 1) % capacity`).
   - Prevents dynamic heap churn and eliminates fragmentation.

2. **HTTP Header Buffer**:
   - Hard upper bound: `8192` bytes (8 KB).
   - If headers exceed 8 KB without `\r\n\r\n`, connection is immediately rejected with `413 Payload Too Large` to prevent Slowloris and memory exhaustion.

3. **Path Traversal Barrier**:
   - `path_is_safe()` strips query strings, checks for prohibited characters, converts path slashes to standard Windows format, and invokes `GetFullPathNameA()`.
   - The canonicalized path is validated to ensure it starts with the absolute directory prefix of `public/`.

