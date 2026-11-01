# ThreadStorm — Viva Examination & Technical Q&A Guide

This guide prepares students for college viva evaluations, lab examinations, and technical presentations on Operating Systems, Systems Programming, and Concurrency.

---

## 🎯 High-Probability Viva Questions & Model Answers

### Q1: What is the Producer-Consumer pattern, and how is it used in ThreadStorm?
**Answer:**  
The Producer-Consumer pattern decouples tasks that produce data from tasks that process data using a synchronized shared buffer. In ThreadStorm:
- The **Producer** is the master accept thread running `accept()`, which accepts incoming TCP connections and wraps client sockets into queue slots.
- The **Consumer** is the pool of worker threads running `worker_thread_fn()`, which pull sockets from the bounded ring buffer and handle HTTP transactions.
- A `CRITICAL_SECTION` guarantees mutual exclusion during queue access, while `CONDITION_VARIABLE`s manage buffer state signaling (`not_empty` and `not_full`).

---

### Q2: Why is a thread pool more efficient than creating a thread per connection?
**Answer:**  
Creating a thread per connection has three fatal drawbacks under high load:
1. **Memory Overhead:** Every thread requires a stack allocation (typically 1 MB default on Windows). 1,000 concurrent connections would consume ~1 GB of virtual memory just for stacks.
2. **Kernel Latency:** Thread creation and destruction requires Windows kernel transitions, object allocation, and teardown taking 1–2 milliseconds per request.
3. **Context Switching Thrashing:** When active threads drastically exceed physical CPU core count, the OS scheduler spends excessive CPU cycles swapping register states and invalidating CPU L1/L2 caches rather than executing request logic.  
A thread pool pre-allocates a fixed number of threads matching hardware parallelism, reusing them continuously with microsecond overhead.

---

### Q3: What is the difference between a Windows `CRITICAL_SECTION` and a `Mutex`?
**Answer:**  
- A **`CRITICAL_SECTION`** is a lightweight user-mode synchronization primitive. It uses atomic assembly operations in user space for uncontended acquisition and only invokes kernel wait functions if contention occurs. It cannot be shared across different processes.
- A **`Mutex`** (`CreateMutex`) is a kernel-mode object. Every acquire (`WaitForSingleObject`) and release (`ReleaseMutex`) requires a context switch into ring-0 kernel space, making it significantly slower (~50x) than an uncontended critical section. However, mutexes can be named and shared across different processes.  
ThreadStorm uses `CRITICAL_SECTION` because worker threads reside in the same process address space and need maximum throughput.

---

### Q4: How does ThreadStorm avoid spinning / busy-waiting?
**Answer:**  
Busy-waiting (e.g. `while(queue->count == 0) {}`) consumes 100% CPU cycles on all cores while waiting for network input.  
ThreadStorm uses Windows Condition Variables:
```c
SleepConditionVariableCS(&q->not_empty, &q->lock, INFINITE);
```
This function atomically releases the queue's `CRITICAL_SECTION` and puts the calling thread into a non-runnable kernel sleep state. The thread consumes zero CPU cycles until the accept thread calls `WakeConditionVariable(&q->not_empty)`.

---

### Q5: How does the Race Condition Arena prove non-determinism?
**Answer:**  
In the unsafe mode, $N$ threads increment a shared counter: `counter++`. In x86/x64 assembly, `counter++` is not atomic; it expands to:
1. `MOV RAX, [counter]` (read from memory into CPU register)
2. `INC RAX` (increment register)
3. `MOV [counter], RAX` (write back to memory)

When two threads read `counter = 10` simultaneously, both increment to 11 and write back 11. Two increments occurred, but the counter only advanced by 1 (a lost update). Because thread preemption and core scheduling are non-deterministic, the magnitude of lost updates varies on every run.

---

### Q6: What are the four Coffman conditions for Deadlock, and how does ThreadStorm prevent it?
**Answer:**  
Deadlock can occur if and only if all four conditions hold:
1. **Mutual Exclusion:** Resources cannot be shared simultaneously.
2. **Hold and Wait:** A process holds one resource while requesting another.
3. **No Preemption:** Resources cannot be forcibly revoked from holding threads.
4. **Circular Wait:** Thread 1 waits for a resource held by Thread 2, which waits for a resource held by Thread 1.

ThreadStorm eliminates condition #4 (**Circular Wait**) by enforcing a **Global Linear Lock Order**:
- Resource A has rank 1; Resource B has rank 2.
- Any thread requiring both resources must acquire Lock A before Lock B.
- Because no thread is allowed to request A while holding B, a circular cycle in the Resource Allocation Graph is mathematically impossible.

---

### Q7: How does ThreadStorm prevent Directory Traversal attacks?
**Answer:**  
Attackers attempt to access arbitrary host files using payloads like `/../../../Windows/system.ini`. ThreadStorm defends against this via:
1. **Character validation:** Rejection of non-printable characters and malformed URI tokens.
2. **Canonicalization:** Invokes Windows API `GetFullPathNameA()` to collapse all relative segments (`..` and `.`) into an absolute path.
3. **Prefix Boundary Check:** Verifies that the resolved path strictly begins with the absolute directory path of `public/`. If the resolved prefix does not match, it returns `403 Forbidden` and logs a security event.

---

### Q8: What is back-pressure and how does the bounded queue implement it?
**Answer:**  
If clients submit requests faster than worker threads can process them, an unbounded queue would allocate memory until the process exhausts virtual address space and crashes (Out-Of-Memory).  
ThreadStorm uses a **bounded queue** (default 64 slots). When the queue is full:
- The accept thread blocks on `SleepConditionVariableCS(&not_full, &lock, 1000)` waiting for a worker to dequeue.
- If the queue remains full after the timeout, ThreadStorm drops the incoming client socket (`closesocket`), preventing memory saturation and logging a queue saturation warning.

