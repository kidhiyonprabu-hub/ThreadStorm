# ThreadStorm — Evaluation & Demonstration Script

Follow this step-by-step walkthrough to demonstrate the full capabilities of ThreadStorm to college evaluators, viva examiners, or hackathon judges.

---

## 🚀 Pre-Demo Checklist
- [x] Windows PC or Laptop with Command Prompt / PowerShell.
- [x] Compiled `bin/threadstorm.exe` and `bin/test_suite.exe`.
- [x] Modern browser (Chrome, Edge, Firefox, Brave) ready to open `http://127.0.0.1:8080/`.

---

## 🎬 Demonstration Steps

### Step 1: Automated Test Validation (Prove Code Works 100%)
Open PowerShell in the project directory and run:
```powershell
.\bin\test_suite.exe
```
**What to highlight to the evaluator:**
- Point out the **26 passed | 0 failed** output.
- Highlight that tests verify:
  1. Socket connectivity and Winsock lifecycle
  2. Static file serving (HTML, CSS, JS) and 404 handling
  3. Directory traversal blocking (`/../../../etc/passwd` & backslashes)
  4. HTTP method restrictions (DELETE blocked with 405)
  5. 20 concurrent requests handled in parallel
  6. Race condition arena and Deadlock prevention completion
  7. High-resolution latency and metrics tracking

---

### Step 2: Start the C Server
Start the server in a PowerShell window:
```powershell
.\bin\threadstorm.exe -p 8080 -w 4 -q 64
```
**What to point out:**
- The console shows:
  - Port `8080` bound to `127.0.0.1`.
  - 4 Win32 worker threads spawned and in `IDLE` state waiting on condition variables.
  - Bounded ring buffer initialized with capacity `64`.
  - Accept loop started.

---

### Step 3: Open the Observability Dashboard
In your browser, open:
```
http://127.0.0.1:8080/
```
**What to show:**
- Dark navy, futuristic violet and cyan observability UI.
- Top status indicator shows **`ONLINE (C SERVER)`** with live pulsing badge.
- Uptime counting up via monotonic timer (`QueryPerformanceCounter`).
- Total HTTP requests counter incrementing automatically as the dashboard polls metrics.

---

### Step 4: Thread Pool Visualizer (Tab 3)
Click on **`🧵 3. Thread Pool Visualizer`**.
**What to show:**
- 4 worker cards representing OS worker threads.
- Note that workers display **`IDLE (Waiting)`** when no load is active.
- Explain that workers consume 0% CPU while blocked on `SleepConditionVariableCS()`.

---

### Step 5: Concurrency Experiments (Tab 5: Synchronization Lab)
Click on **`🔬 5. Synchronization Lab`**.

1. **Race Condition Arena**:
   - Set Threads = `4`, Iterations = `100,000`.
   - Click **`Run Race Arena Experiment`**.
   - Expected value = `400,000`.
   - Unsafe (No Lock) will show a missing count (e.g. `241,520`) with a bright red **`DATA RACE CONFIRMED`** badge.
   - Safe (Mutex) will show exactly `400,000` with 100% precision.
   - **Explain:** Explain lost updates during the READ-MODIFY-WRITE cycle on multi-core processors.

2. **Deadlock Prevention Lab**:
   - Click **`Run Deadlock Prevention Demo`**.
   - Watch the demo execute 3 concurrent threads acquiring two resources 10,000 times in under 5 ms without any hang.
   - **Explain:** Explain how strict hierarchical lock ordering (`Lock A` before `Lock B`) mathematically eliminates Coffman's circular wait condition.

---

### Step 6: Native C Load Testing (Tab 6: Load Testing)
Click on **`🚀 6. Load Testing`**.
- Set Total Requests = `250`, Concurrency = `8 Threads`.
- Click **`Launch C Multithreaded Load Test`**.
- Watch the test complete in ~200–400 ms.
- Observe:
  - Throughput (e.g., `800+ req/sec`).
  - Average latency (e.g., `1.2 ms`).
  - Latency percentiles: `p50`, `p95`, `p99` tail latency.

---

### Step 7: Sequential vs Multithreaded Performance (Tab 7)
Click on **`⚖️ 7. Performance Comparison`**.
- Click **`Run Full Comparison Suite`**.
- The engine executes 100 requests with 1 thread, then 100 requests with 4 concurrent threads.
- Point out the **Throughput Factor Speedup** (e.g., `2.5x – 4.0x Speedup`).
- **Explain:** Explain how multi-worker concurrency overcomes head-of-line blocking in network servers.

---

### Step 8: Security Exploit Simulator (Tab 8: Security Events)
Click on **`🛡️ 8. Security Events`**.
- Click **`Simulate Path Traversal`**.
- Immediately point out the new entry in the Live Security Audit Log:
  - Attack: `TRAVERSAL`
  - Detail: `/../../../etc/passwd`
  - Defense: `BLOCKED & REJECTED (HTTP 403 Forbidden)`
- Click **`Simulate Unsupported Method`** and observe the `BAD_METHOD` event recorded.

---

### Step 9: CO1–CO4 Learning Center & Data Export (Tab 10)
Click on **`🎓 10. CO1–CO4 Learning Center`**.
- Show the faculty evaluator the full curriculum mapping table for CO1, CO2, CO3, and CO4.
- In the top header, click **`Export JSON`** or **`Export CSV`** to demonstrate that test results and audit logs can be exported directly for grading and laboratory reports.

---

### Step 10: Graceful Shutdown
Switch to the PowerShell server window and press **`Ctrl + C`**.
**What to point out:**
- The server captures `CTRL_C_EVENT`.
- Unblocks the accept loop by closing the listen socket.
- Signals condition variables (`WakeAllConditionVariable`).
- Joins all 4 worker threads cleanly with `WaitForSingleObject`.
- Cleans up Winsock resources with `WSACleanup()`.
- Exits with return code 0 and zero dangling handles.

