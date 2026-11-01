# ThreadStorm ⚡
### Real-Time Multithreaded Web Server & Synchronization Intelligence Platform

> **College Micro Project 03 — Systems & Concurrency Engineering**  
> **Target OS:** Windows (Win32 API & Winsock 2.2)  
> **Implementation Language:** C (C11 standard) & Vanilla HTML5/CSS3/JavaScript  
> **Status:** Verified & Fully Tested (26/26 Automated Tests Passing)

---

## 📖 Executive Summary

**ThreadStorm** is a high-performance, multithreaded HTTP/1.1 web server and concurrency observation platform built entirely from scratch in C for Microsoft Windows. Designed as a college micro-project and hackathon demonstration, it provides deep real-time visibility into operating system synchronization primitives, thread pool worker lifecycles, and network I/O behavior.

The frontend is a futuristic cloud-infrastructure dashboard communicating in real-time with native C telemetry endpoints, visualizing active worker thread states, request queues, latency percentiles, data races, and deadlock prevention algorithms without any third-party runtime frameworks (no Python, Node.js, Flask, or external web servers).

---

## 🏗️ Architecture & Core Components

```
                           +----------------------------+
                           |   Incoming HTTP Requests   |
                           +----------------------------+
                                         |
                                         v
                         +-------------------------------+
                         |   Master Accept Loop (TCP)    |
                         |   (Winsock socket/bind/listen)|
                         +-------------------------------+
                                         |
                                         v
                    +-----------------------------------------+
                    |    Thread-Safe Bounded Request Queue     |
                    |    (CRITICAL_SECTION + 2x CondVars)     |
                    +-----------------------------------------+
                         |               |               |
                         v               v               v
                   +-----------+   +-----------+   +-----------+
                   | Worker #1 |   | Worker #2 |   | Worker #N |
                   |  (Win32)  |   |  (Win32)  |   |  (Win32)  |
                   +-----------+   +-----------+   +-----------+
                         |               |               |
                         +---------------+---------------+
                                         |
                                         v
                         +-------------------------------+
                         | HTTP Parser & Safe Dispatcher |
                         +-------------------------------+
                                 /               \
                                v                 v
                    +--------------------+   +--------------------+
                    | Static File Engine |   | Internal REST APIs |
                    | (Path Traversal    |   | - /api/metrics     |
                    |  Protection)       |   | - /api/workers     |
                    |                    |   | - /api/events      |
                    +--------------------+   | - /api/sync/race   |
                                             | - /api/sync/deadlock
                                             +--------------------+
```

### 1. Winsock Network Layer
- **Winsock 2.2 Initialization (`WSAStartup`) & Teardown (`WSACleanup`)**: Sets up non-blocking sockets with `SO_REUSEADDR`, receive and send socket timeouts (`SO_RCVTIMEO`, `SO_SNDTIMEO`).
- **Bounded Request Queue (`request_queue.c`)**: Ring buffer guarded by `CRITICAL_SECTION`. Workers sleep on `not_empty` condition variable; listener signals when a connection is queued and applies back-pressure when full.

### 2. Native Win32 Worker Thread Pool (`thread_pool.c`)
- Pre-allocated worker threads using `CreateThread`.
- Workers transition through distinct observable states: `IDLE` (blocked on condition variable) &rarr; `BUSY` (processing HTTP request) &rarr; `STOPPED` (clean exit).
- **Adaptive Controller**: An autonomous monitor thread analyzes queue depth and average latency to dynamically adjust worker counts within configurable limits (`[2, 32]`) with cooldown anti-thrashing mechanisms.

### 3. Synchronization Lab (`sync_lab.c`)
- **Race Condition Arena**: Multi-threaded counter test demonstrating lost updates from unsynchronized writes (`x++`) versus atomic mutual exclusion (`EnterCriticalSection`).
- **Deadlock Prevention Lab**: Strict hierarchical lock ordering demonstration proving Coffman's circular wait condition can be broken mathematically.

### 4. Security & Hardening (`server.c`)
- Strict request size limit (8 KB) rejecting buffer exhaustion attacks (`413 Payload Too Large`).
- Path traversal defense: canonicalizes candidate file paths with `GetFullPathNameA()` and validates that the prefix strictly matches the `public/` folder, blocking `../` and backslash traversal exploits (`403 Forbidden`).
- HTTP method restriction: permits only `GET`, `POST`, and `HEAD`, returning `405 Method Not Allowed` for `DELETE`, `PUT`, etc.

---

## 📂 Project Structure

```
ThreadStorm/
├── bin/
│   ├── threadstorm.exe       # Main server executable
│   └── test_suite.exe        # Automated test harness
├── include/
│   ├── common.h              # Platform constants, logging, monotonic timer
│   ├── request_queue.h       # Bounded thread-safe queue headers
│   ├── thread_pool.h         # Thread pool & worker descriptors
│   ├── metrics.h             # Telemetry & ring buffer counters
│   ├── server.h              # Winsock lifecycle, HTTP parser & routes
│   ├── sync_lab.h            # Race arena & deadlock demo headers
│   └── benchmark.h           # Sequential vs concurrent benchmark headers
├── src/
│   ├── main.c                # Entry point, CLI arguments & SIGINT handler
│   ├── common.c              # Monotonic QPC timer & thread-safe logger
│   ├── request_queue.c       # Ring buffer with condition variables
│   ├── thread_pool.c         # Worker management & adaptive controller
│   ├── metrics.c             # Interlocked counters & JSON serializers
│   ├── server.c              # Winsock server, static files & REST APIs
│   ├── sync_lab.c            # Isolated concurrency experiments
│   └── benchmark.c           # High-resolution benchmark engine
├── public/
│   ├── index.html            # 10-section observability dashboard
│   ├── styles.css            # Futuristic dark navy/violet/cyan UI theme
│   └── app.js                # Vanilla JS live poller & canvas chart engine
├── tests/
│   └── test_suite.c          # 26 automated integration & security tests
├── docs/
│   ├── ARCHITECTURE.md       # Detailed technical design & sequence diagrams
│   ├── VIVA_GUIDE.md         # College viva Q&A and CO mapping
│   └── DEMO_SCRIPT.md        # Step-by-step evaluator demonstration flow
├── logs/
│   └── threadstorm.log       # Server audit & access log
├── Makefile                  # Build script for GCC (MSYS2/MinGW)
└── README.md                 # Project guide
```

---

## 🛠️ Build and Execution Instructions

### Prerequisites
- **Operating System:** Windows 10 or 11
- **Compiler:** GCC 14.x or MinGW-w64 (via MSYS2 or standalone MinGW)
- **Libraries:** Windows Sockets (`ws2_32`)

### 1. Build the Application
Open PowerShell in the `ThreadStorm` folder and run:

```powershell
# Set temporary directory and path to GCC if using MSYS2
$env:PATH = "C:\msys64\ucrt64\bin;C:\msys64\usr\bin;$env:PATH"
$env:TEMP = "$PWD\build_tmp"
$env:TMP = "$PWD\build_tmp"

# Build main server executable
gcc -Wall -Wextra -std=c11 -O2 -Iinclude -o bin/threadstorm.exe `
    src/main.c src/common.c src/server.c src/request_queue.c `
    src/thread_pool.c src/metrics.c src/sync_lab.c src/benchmark.c -lws2_32
```

*(Alternatively, use `make` if GNU Make is installed).*

### 2. Run the Server
Launch the server with default settings (Port 8080, 4 workers, queue capacity 64):

```powershell
.\bin\threadstorm.exe
```

Custom configuration flags:
```powershell
.\bin\threadstorm.exe -p 8080 -w 8 -q 128 -a
```
- `-p <port>` : Port to listen on (default: `8080`)
- `-w <workers>` : Pre-allocated worker thread count (default: `4`, range: `2-32`)
- `-q <capacity>`: Bounded queue size (default: `64`, max: `256`)
- `-a` : Enable autonomous adaptive pool controller

### 3. Open the Observability Dashboard
Navigate to:
```
http://127.0.0.1:8080/
```

### 4. Run the Automated Test Suite
To verify all 26 tests (connectivity, security, race conditions, deadlocks, metrics):

```powershell
# Build test suite
gcc -Wall -Wextra -std=c11 -O2 -Iinclude -o bin/test_suite.exe `
    tests/test_suite.c src/common.c src/server.c src/request_queue.c `
    src/thread_pool.c src/metrics.c src/sync_lab.c src/benchmark.c -lws2_32

# Execute test suite
.\bin\test_suite.exe
```

---

## 🧪 Test Suite Results

The automated integration harness validates all server subsystems against rigorous test criteria:

```
╔══════════════════════════════════════════════════╗
║      ThreadStorm Test Suite  v1.0                ║
╚══════════════════════════════════════════════════╝

[Starting in-process test server on port 18080...]
── 1. Basic Connectivity ──
  [PASS] Connected to server
  [PASS] GET /api/metrics returns 200
  [PASS] /api/metrics body has 'status' key
  [PASS] /api/metrics body has 'total_requests'

── 2. Static File Serving ──
  [PASS] Connected for /
  [PASS] GET / returns 200
  [PASS] Response body not empty
  [PASS] Missing file returns 404

── 3. Path Traversal Protection ──
  [PASS] Traversal attempt rejected with 403/400
  [PASS] Windows backslash traversal rejected

── 4. HTTP Method Restriction ──
  [PASS] DELETE method returns 405 Method Not Allowed

── 5. API Endpoints ──
  [PASS] GET /api/events returns 200
  [PASS] GET /api/security returns 200
  [PASS] GET /api/rps returns 200
  [PASS] GET /api/workers returns 200
  [PASS] Unknown API returns 404

── 6. Concurrent Requests ──
  [PASS] >= 15 of 20 concurrent connects succeed
  [PASS] >= 15 of 20 concurrent requests get 200

── 7. Sync Lab: Race Condition ──
  [PASS] POST /api/sync/race returns 200
  [PASS] Race result has 'expected_value'
  [PASS] Race result has 'safe_result'

── 8. Sync Lab: Deadlock Prevention ──
  [PASS] POST /api/sync/deadlock returns 200
  [PASS] Deadlock result has 'result'
  [PASS] Deadlock demo completed without hang

── 9. Metrics Accuracy ──
  [PASS] total_requests counter increments correctly

── 10. Oversized Request ──
  [PASS] Oversized/malformed request gets 400 or 413

══════════════════════════════════════════════════
  Results:  26 passed  |  0 failed
══════════════════════════════════════════════════
```

---

## 🎓 Course Outcomes (CO1–CO4) Mapping

| Outcome | Curriculum Competency | Demonstrated In ThreadStorm | Evaluation Metric |
| :--- | :--- | :--- | :--- |
| **CO1** | **OS Architecture & Hardware Interaction** | Winsock socket lifecycle, kernel socket backlog, context switching, CPU thread dispatching, QueryPerformanceCounter (QPC). | Directly measured via monotonic timing and live throughput. |
| **CO2** | **Scheduling & Concurrency Synchronization** | Producer-Consumer thread pool, bounded ring buffer, `CRITICAL_SECTION`, `CONDITION_VARIABLE` signaling, Race Condition Arena. | Measured via lost updates in Race Arena and worker state visualizer. |
| **CO3** | **Deadlock Handling & Memory Management** | Breaking Coffman's circular wait condition via strict total lock hierarchy, bounded allocations, zero-leak resource teardown. | Deadlock lab execution time and completion verification. |
| **CO4** | **Virtual Memory, Filesystem & Security** | Canonicalized path validation via `GetFullPathNameA`, bounded buffer protection, MIME resolution, directory traversal prevention. | Live security audit events and HTTP 403/413 blocks. |

---

## 📜 License & Academic Integrity
Developed as an educational systems programming micro project. Intended for demonstration of operating systems principles, systems architecture, and multithreading synchronization.

