/**
 * ThreadStorm - app.js
 * Real-Time Multithreaded Web Server & Synchronization Intelligence Platform
 * Vanilla JavaScript Frontend Engine
 */

// Application State
const state = {
  activeTab: 'overview',
  pollIntervalMs: 1000,
  pollTimer: null,
  isPolling: true,
  serverOnline: false,
  metrics: null,
  workers: [],
  events: [],
  securityEvents: [],
  rpsHistory: [],
  comparisonData: {
    sequential: null,
    concurrent: null
  }
};

// Canvas Chart Helpers
class MiniChart {
  static drawLineChart(canvasId, dataPoints, strokeColor, fillColor) {
    const canvas = document.getElementById(canvasId);
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    const width = canvas.width = canvas.parentElement.clientWidth;
    const height = canvas.height = canvas.parentElement.clientHeight;

    ctx.clearRect(0, 0, width, height);

    if (!dataPoints || dataPoints.length === 0) {
      ctx.fillStyle = '#64748b';
      ctx.font = '12px Inter, sans-serif';
      ctx.textAlign = 'center';
      ctx.fillText('No data available', width / 2, height / 2);
      return;
    }

    const padding = 25;
    const chartW = width - padding * 2;
    const chartH = height - padding * 2;
    const maxVal = Math.max(...dataPoints, 5);

    // Grid lines
    ctx.strokeStyle = 'rgba(255, 255, 255, 0.05)';
    ctx.lineWidth = 1;
    for (let i = 0; i <= 4; i++) {
      const y = padding + (chartH / 4) * i;
      ctx.beginPath();
      ctx.moveTo(padding, y);
      ctx.lineTo(width - padding, y);
      ctx.stroke();

      const label = Math.round(maxVal - (maxVal / 4) * i);
      ctx.fillStyle = '#64748b';
      ctx.font = '10px monospace';
      ctx.textAlign = 'right';
      ctx.fillText(label.toString(), padding - 5, y + 3);
    }

    // Step calculation
    const stepX = chartW / (dataPoints.length - 1 || 1);

    // Path
    ctx.beginPath();
    ctx.moveTo(padding, padding + chartH - (dataPoints[0] / maxVal) * chartH);

    for (let i = 1; i < dataPoints.length; i++) {
      const x = padding + i * stepX;
      const y = padding + chartH - (dataPoints[i] / maxVal) * chartH;
      ctx.lineTo(x, y);
    }

    // Stroke
    ctx.strokeStyle = strokeColor || '#06b6d4';
    ctx.lineWidth = 2.5;
    ctx.shadowColor = strokeColor || '#06b6d4';
    ctx.shadowBlur = 8;
    ctx.stroke();
    ctx.shadowBlur = 0;

    // Fill under line
    ctx.lineTo(padding + (dataPoints.length - 1) * stepX, padding + chartH);
    ctx.lineTo(padding, padding + chartH);
    ctx.closePath();
    const grad = ctx.createLinearGradient(0, padding, 0, height - padding);
    grad.addColorStop(0, fillColor || 'rgba(6, 182, 212, 0.25)');
    grad.addColorStop(1, 'rgba(6, 182, 212, 0.0)');
    ctx.fillStyle = grad;
    ctx.fill();
  }

  static drawBarChart(canvasId, labels, values, colors) {
    const canvas = document.getElementById(canvasId);
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    const width = canvas.width = canvas.parentElement.clientWidth;
    const height = canvas.height = canvas.parentElement.clientHeight;

    ctx.clearRect(0, 0, width, height);
    const padding = 30;
    const chartW = width - padding * 2;
    const chartH = height - padding * 2;
    const maxVal = Math.max(...values, 10);
    const barWidth = Math.min(50, (chartW / values.length) * 0.5);
    const stepX = chartW / values.length;

    values.forEach((val, i) => {
      const x = padding + i * stepX + (stepX - barWidth) / 2;
      const barH = (val / maxVal) * chartH;
      const y = padding + chartH - barH;

      ctx.fillStyle = colors[i] || '#8b5cf6';
      ctx.beginPath();
      ctx.roundRect ? ctx.roundRect(x, y, barWidth, barH, [4, 4, 0, 0]) : ctx.rect(x, y, barWidth, barH);
      ctx.fill();

      // Value label
      ctx.fillStyle = '#f8fafc';
      ctx.font = '11px monospace';
      ctx.textAlign = 'center';
      ctx.fillText(val.toString(), x + barWidth / 2, y - 6);

      // Category label
      ctx.fillStyle = '#94a3b8';
      ctx.font = '11px sans-serif';
      ctx.fillText(labels[i], x + barWidth / 2, height - 10);
    });
  }
}

// Formatters
function formatUptime(ms) {
  if (!ms) return '0s';
  const totalSec = Math.floor(ms / 1000);
  const h = Math.floor(totalSec / 3600);
  const m = Math.floor((totalSec % 3600) / 60);
  const s = totalSec % 60;
  if (h > 0) return `${h}h ${m}m ${s}s`;
  if (m > 0) return `${m}m ${s}s`;
  return `${s}s`;
}

function formatTime(ms) {
  if (!ms) return '-';
  const d = new Date(ms);
  return d.toLocaleTimeString();
}

// API Poller
async function fetchServerData() {
  try {
    const [metricsRes, workersRes, eventsRes, secRes, rpsRes] = await Promise.all([
      fetch('/api/metrics').catch(() => null),
      fetch('/api/workers').catch(() => null),
      fetch('/api/events').catch(() => null),
      fetch('/api/security').catch(() => null),
      fetch('/api/rps').catch(() => null)
    ]);

    if (!metricsRes || !metricsRes.ok) {
      setServerStatus(false);
      return;
    }

    state.metrics = await metricsRes.json();
    state.workers = workersRes && workersRes.ok ? await workersRes.json() : [];
    state.events = eventsRes && eventsRes.ok ? await eventsRes.json() : [];
    state.securityEvents = secRes && secRes.ok ? await secRes.json() : [];
    state.rpsHistory = rpsRes && rpsRes.ok ? await rpsRes.json() : [];

    setServerStatus(true);
    updateDashboardUI();
  } catch (err) {
    console.error('Polling error:', err);
    setServerStatus(false);
  }
}

function setServerStatus(online) {
  state.serverOnline = online;
  const badge = document.getElementById('serverStatusBadge');
  const badgeText = document.getElementById('serverStatusText');
  if (online) {
    badge.className = 'server-badge';
    badgeText.textContent = 'ONLINE (C SERVER)';
  } else {
    badge.className = 'server-badge offline';
    badgeText.textContent = 'OFFLINE / DISCONNECTED';
  }
}

// UI Updating
function updateDashboardUI() {
  const m = state.metrics;
  if (!m) return;

  // Live Timestamp
  const now = new Date();
  setText('lastSyncTime', now.toTimeString().split(' ')[0]);

  // Overview / Header Stats
  setText('statUptime', formatUptime(m.uptime_ms));
  setText('statTotalReq', m.total_requests.toLocaleString());
  setText('statActiveConn', m.active_connections);
  setText('statActiveConn2', m.active_connections);
  setText('statRps', m.rps);
  setText('statAvgLat', `${m.avg_latency_ms.toFixed(2)} ms`);
  setText('statMinMaxLat', `${m.min_latency_ms} ms / ${m.max_latency_ms} ms`);

  // CO3 Memory & CPU Telemetry
  const ramWs = m.ram_working_set_kb || 0;
  const ramPeak = m.ram_peak_kb || 0;
  const ramWsFmt = ramWs >= 1024 ? `${(ramWs / 1024).toFixed(2)} MB` : `${ramWs} KB`;
  const ramPeakFmt = ramPeak >= 1024 ? `${(ramPeak / 1024).toFixed(2)} MB` : `${ramPeak} KB`;
  setText('statRamWs', ramWsFmt);
  setText('statRamPeak', ramPeakFmt);
  setText('statCpu', `${(m.cpu_usage_pct || 0).toFixed(1)}%`);

  const qMemBytes = m.queue_memory_bytes || (m.queue_capacity * 32);
  const qMemFmt = qMemBytes >= 1024 ? `${(qMemBytes / 1024).toFixed(1)} KB` : `${qMemBytes} B`;
  setText('statQueueMemText', `Heap: ${qMemFmt} bounded`);
  
  const totalResp = (m.status_2xx + m.status_3xx + m.status_4xx + m.status_5xx) || 1;
  const successRate = ((m.status_2xx / totalResp) * 100).toFixed(1);
  setText('statSuccessRate', `${successRate}%`);

  // Queue info
  setText('statQueueDepth', `${m.queue_depth} / ${m.queue_capacity}`);
  const fillPct = Math.min(100, Math.round((m.queue_depth / (m.queue_capacity || 1)) * 100));
  setText('queueFillPctBadge', `${fillPct}% FILL`);
  const qBar = document.getElementById('queueProgressBar');
  if (qBar) {
    qBar.style.width = `${fillPct}%`;
  }

  // Adaptive controller info
  setText('adaptiveStatusText', m.adaptive_enabled ? 'ENABLED' : 'DISABLED');
  setText('adaptiveReasonText', m.adaptive_last_reason || 'No recent scaling event');
  const adaptBtn = document.getElementById('btnToggleAdaptive');
  if (adaptBtn) {
    adaptBtn.textContent = m.adaptive_enabled ? 'Disable Adaptive Pool' : 'Enable Adaptive Pool';
    adaptBtn.className = m.adaptive_enabled ? 'btn btn-danger' : 'btn btn-primary';
  }

  // Render Charts if tab visible
  if (state.activeTab === 'overview' || state.activeTab === 'monitor') {
    MiniChart.drawLineChart('rpsLiveChart', state.rpsHistory, '#22d3ee', 'rgba(6, 182, 212, 0.2)');
    MiniChart.drawBarChart(
      'statusLiveChart',
      ['2xx Success', '3xx Redir', '4xx Client', '5xx Server'],
      [m.status_2xx, m.status_3xx, m.status_4xx, m.status_5xx],
      ['#10b981', '#38bdf8', '#fbbf24', '#ef4444']
    );
  }

  // Workers rendering
  renderWorkers(state.workers);

  // Recent Requests Explorer
  renderEventsTable(state.events);

  // Security Events
  renderSecurityTable(state.securityEvents);
}

function setText(id, text) {
  const el = document.getElementById(id);
  if (el) el.textContent = text;
}

// Workers Renderer
function renderWorkers(workers) {
  const container = document.getElementById('workerGrid');
  if (!container) return;

  if (!workers || workers.length === 0) {
    container.innerHTML = '<div style="color: #64748b; font-size: 0.9rem;">No workers reporting.</div>';
    return;
  }

  container.innerHTML = workers.map(w => {
    const isBusy = w.state === 'busy';
    const isStopped = w.state === 'stopped';
    const stateClass = isBusy ? 'busy' : (isStopped ? 'stopped' : 'idle');
    const stateLabel = isBusy ? 'BUSY (Processing)' : (isStopped ? 'STOPPED' : 'IDLE (Waiting)');

    return `
      <div class="worker-card ${stateClass}">
        <div class="worker-card-header">
          <div class="worker-name">
            <span style="display:inline-block; width:8px; height:8px; border-radius:50%; background:${isBusy ? '#fbbf24' : (isStopped ? '#64748b' : '#34d399')};"></span>
            Worker #${w.id}
          </div>
          <span class="worker-state-badge ${stateClass}">${stateLabel}</span>
        </div>
        <div class="worker-meta">
          <span>Active Task:</span>
          <span style="font-family:var(--font-mono); color:${isBusy ? 'var(--accent-cyan)' : 'var(--text-dim)'};">
            ${isBusy ? `REQ #${w.current_req}` : 'Idle (In wait)'}
          </span>
        </div>
        <div class="worker-meta">
          <span>Requests Completed:</span>
          <span style="font-family:var(--font-mono); font-weight:600;">${w.requests}</span>
        </div>
      </div>
    `;
  }).join('');
}

// Events Table Renderer
function renderEventsTable(events) {
  const tbody = document.getElementById('eventsTableBody');
  if (!tbody) return;

  if (!events || events.length === 0) {
    tbody.innerHTML = '<tr><td colspan="7" style="text-align:center; color:#64748b;">No recent requests logged.</td></tr>';
    return;
  }

  const filter = (document.getElementById('eventFilterSelect')?.value || 'all').toLowerCase();
  const search = (document.getElementById('eventSearchInput')?.value || '').toLowerCase();

  const filtered = events.filter(ev => {
    if (filter === '2xx' && (ev.status < 200 || ev.status >= 300)) return false;
    if (filter === 'err' && ev.status < 400) return false;
    if (search && !ev.uri.toLowerCase().includes(search) && !ev.method.toLowerCase().includes(search)) return false;
    return true;
  });

  tbody.innerHTML = filtered.slice(0, 25).map(ev => {
    const mClass = ev.method.toLowerCase();
    const sClass = ev.status >= 400 ? (ev.status >= 500 ? 's5xx' : 's4xx') : 's2xx';

    return `
      <tr>
        <td style="font-family:var(--font-mono); color:var(--text-dim);">#${ev.id}</td>
        <td><span class="method-badge ${mClass}">${ev.method}</span></td>
        <td style="font-family:var(--font-mono); color:#e2e8f0;">${escapeHtml(ev.uri)}</td>
        <td><span class="status-badge ${sClass}">${ev.status}</span></td>
        <td style="font-family:var(--font-mono);">${ev.duration_ms} ms</td>
        <td style="color:var(--accent-purple-light); font-weight:600;">Worker #${ev.worker_id || '1'}</td>
        <td style="color:var(--text-dim); font-size:0.8rem;">${formatTime(ev.timestamp_ms)}</td>
      </tr>
    `;
  }).join('');
}

// Security Table Renderer
function renderSecurityTable(secEvents) {
  const tbody = document.getElementById('securityTableBody');
  if (!tbody) return;

  if (!secEvents || secEvents.length === 0) {
    tbody.innerHTML = '<tr><td colspan="4" style="text-align:center; color:#64748b;">No security anomalies detected. Server is safe.</td></tr>';
    return;
  }

  tbody.innerHTML = secEvents.slice(0, 20).map(s => {
    const isCrit = s.type === 'TRAVERSAL' || s.type === 'TOO_LARGE';
    const badgeColor = isCrit ? 'background:rgba(239, 68, 68, 0.2); color:#f87171; border:1px solid rgba(239, 68, 68, 0.4);' 
                              : 'background:rgba(245, 158, 11, 0.2); color:#fbbf24; border:1px solid rgba(245, 158, 11, 0.4);';

    return `
      <tr>
        <td><span style="font-family:var(--font-mono); font-size:0.75rem; font-weight:700; padding:0.2rem 0.5rem; border-radius:4px; ${badgeColor}">${s.type}</span></td>
        <td style="color:#e2e8f0; font-family:var(--font-mono); font-size:0.85rem;">${escapeHtml(s.detail)}</td>
        <td><span style="color:#34d399; font-weight:600; font-size:0.8rem;">BLOCKED & REJECTED (HTTP 403/400)</span></td>
        <td style="color:var(--text-dim); font-size:0.8rem;">${formatTime(s.timestamp_ms)}</td>
      </tr>
    `;
  }).join('');
}

function escapeHtml(str) {
  if (!str) return '';
  return str.replace(/[&<>"']/g, m => ({ '&':'&amp;', '<':'&lt;', '>':'&gt;', '"':'&quot;', "'":'&#39;' }[m]));
}

// Actions & Experiments
async function runRaceConditionExperiment() {
  const btn = document.getElementById('btnRunRace');
  const resultCard = document.getElementById('raceResultCard');
  const threads = parseInt(document.getElementById('raceThreadsSelect').value, 10) || 4;
  const iters = parseInt(document.getElementById('raceItersSelect').value, 10) || 100000;

  btn.disabled = true;
  btn.textContent = 'Executing C Experiment...';
  resultCard.style.display = 'block';
  setText('raceExpVal', 'Calculating...');
  setText('raceUnsafeVal', 'Computing...');
  setText('raceSafeVal', 'Computing...');

  try {
    const res = await fetch('/api/sync/race', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ threads, iterations: iters })
    });

    const data = await res.json();
    setText('raceExpVal', data.expected_value.toLocaleString());
    setText('raceUnsafeVal', data.unsafe_result.toLocaleString());
    setText('raceSafeVal', data.safe_result.toLocaleString());
    setText('raceTimeUnsafe', `${data.duration_unsafe_ms} ms`);
    setText('raceTimeSafe', `${data.duration_safe_ms} ms`);

    const badge = document.getElementById('raceDetectionBadge');
    if (data.race_detected) {
      badge.textContent = 'DATA RACE CONFIRMED (LOST UPDATES DETECTED)';
      badge.style.background = 'rgba(239, 68, 68, 0.2)';
      badge.style.color = '#f87171';
      badge.style.border = '1px solid rgba(239, 68, 68, 0.4)';
    } else {
      badge.textContent = 'RACE NOT OBSERVED THIS RUN (NON-DETERMINISTIC)';
      badge.style.background = 'rgba(245, 158, 11, 0.2)';
      badge.style.color = '#fbbf24';
      badge.style.border = '1px solid rgba(245, 158, 11, 0.4)';
    }

    setText('raceExplanation', data.explanation);
  } catch (err) {
    alert('Failed to run race experiment: ' + err.message);
  } finally {
    btn.disabled = false;
    btn.textContent = 'Run Race Arena Experiment';
  }
}

async function runDeadlockDemo() {
  const btn = document.getElementById('btnRunDeadlock');
  const resultCard = document.getElementById('deadlockResultCard');
  btn.disabled = true;
  btn.textContent = 'Running Detection, Recovery & Prevention...';
  resultCard.style.display = 'block';

  try {
    const res = await fetch('/api/sync/deadlock', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ threads: 3 })
    });

    const data = await res.json();
    const isSuccess = data.result === 'completed';
    const statusEl = document.getElementById('deadlockStatus');
    if (statusEl) {
      statusEl.textContent = isSuccess ? 'SUCCESS: RECOVERED & PREVENTED' : 'TIMEOUT';
      statusEl.style.color = isSuccess ? '#34d399' : '#ef4444';
    }
    setText('deadlockTime', `${data.duration_ms} ms`);
    setText('deadlockDetectedCount', `${data.deadlocks_detected || 0} cycles`);
    setText('deadlockRecoveryCount', `${data.recoveries_performed || 0} backoffs`);
    setText('deadlockUnsafeCounter', `${data.counter_unsafe || 0} / ${data.expected_value || 0}`);
    setText('deadlockUnsafeTime', `${data.duration_unsafe_ms || 0} ms`);
    setText('deadlockSafeCounter', `${data.counter_safe || 0} / ${data.expected_value || 0}`);
    setText('deadlockSafeTime', `${data.duration_safe_ms || 0} ms`);
    setText('deadlockExplanation', data.explanation);
  } catch (err) {
    alert('Failed to run deadlock demo: ' + err.message);
  } finally {
    btn.disabled = false;
    btn.textContent = 'Run Deadlock Detection, Recovery & Prevention Demo';
  }
}

async function runLoadBenchmark() {
  const btn = document.getElementById('btnRunBench');
  const reqCount = parseInt(document.getElementById('benchReqCount').value, 10) || 100;
  const threads = parseInt(document.getElementById('benchConcurrency').value, 10) || 4;

  btn.disabled = true;
  btn.textContent = 'Running C Load Test...';
  document.getElementById('benchResultCard').style.display = 'block';

  try {
    const res = await fetch('/api/bench/run', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ requests: reqCount, threads })
    });

    const data = await res.json();
    setText('benchCompleted', data.completed);
    setText('benchFailed', data.failed);
    setText('benchRps', `${data.rps.toFixed(1)} req/sec`);
    setText('benchDuration', `${data.total_duration_ms} ms`);
    setText('benchAvgLat', `${data.avg_latency_ms.toFixed(2)} ms`);
    setText('benchP50', `${data.p50_latency_ms.toFixed(2)} ms`);
    setText('benchP95', `${data.p95_latency_ms.toFixed(2)} ms`);
    setText('benchP99', `${data.p99_latency_ms.toFixed(2)} ms`);
    setText('benchNote', data.note);
  } catch (err) {
    alert('Failed to run load test: ' + err.message);
  } finally {
    btn.disabled = false;
    btn.textContent = 'Launch C Multithreaded Load Test';
  }
}

async function runPerformanceComparison() {
  const btn = document.getElementById('btnRunComparison');
  btn.disabled = true;
  btn.textContent = 'Executing Sequential Test (1 thread)...';
  const requests = 100;

  try {
    // 1. Sequential run
    const seqRes = await fetch('/api/bench/run', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ requests, threads: 1 })
    });
    state.comparisonData.sequential = await seqRes.json();

    // 2. Multithreaded run
    btn.textContent = 'Executing Concurrent Test (4 threads)...';
    const conRes = await fetch('/api/bench/run', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ requests, threads: 4 })
    });
    state.comparisonData.concurrent = await conRes.json();

    // Render results
    const s = state.comparisonData.sequential;
    const c = state.comparisonData.concurrent;

    setText('cmpSeqRps', `${s.rps.toFixed(1)} RPS`);
    setText('cmpSeqLat', `${s.avg_latency_ms.toFixed(2)} ms`);
    setText('cmpSeqTime', `${s.total_duration_ms} ms`);

    setText('cmpConRps', `${c.rps.toFixed(1)} RPS`);
    setText('cmpConLat', `${c.avg_latency_ms.toFixed(2)} ms`);
    setText('cmpConTime', `${c.total_duration_ms} ms`);

    const speedup = (c.rps / (s.rps || 1)).toFixed(2);
    setText('cmpSpeedupBadge', `${speedup}x Throughput Factor`);

    document.getElementById('comparisonResultsContainer').style.display = 'block';
  } catch (err) {
    alert('Comparison failed: ' + err.message);
  } finally {
    btn.disabled = false;
    btn.textContent = 'Run Full Sequential vs Multithreaded Suite';
  }
}

async function toggleAdaptivePool() {
  try {
    const res = await fetch('/api/adaptive/toggle', { method: 'POST' });
    const data = await res.json();
    fetchServerData();
  } catch (err) {
    alert('Failed to toggle adaptive pool: ' + err.message);
  }
}

async function simulateSecurityAttack(type) {
  try {
    if (type === 'traversal') {
      await fetch('/../../../etc/passwd');
    } else if (type === 'bad_method') {
      await fetch('/', { method: 'DELETE' });
    } else if (type === 'overflow') {
      await fetch('/api/nonexistent_overflow_path_' + 'A'.repeat(600));
    }
    // Refresh security logs
    setTimeout(fetchServerData, 200);
  } catch (err) {
    // Expected to get error or blocked
    setTimeout(fetchServerData, 200);
  }
}

function exportMetricsJson() {
  const data = {
    exportedAt: new Date().toISOString(),
    metrics: state.metrics,
    workers: state.workers,
    recentEvents: state.events,
    securityEvents: state.securityEvents
  };
  downloadFile('threadstorm-report.json', JSON.stringify(data, null, 2), 'application/json');
}

function exportEventsCsv() {
  if (!state.events || state.events.length === 0) {
    alert('No events to export.');
    return;
  }
  let csv = 'id,method,uri,status,duration_ms,worker_id,timestamp\r\n';
  state.events.forEach(ev => {
    csv += `${ev.id},${ev.method},"${ev.uri}",${ev.status},${ev.duration_ms},${ev.worker_id},${ev.timestamp_ms}\r\n`;
  });
  downloadFile('threadstorm-events.csv', csv, 'text/csv');
}

function downloadFile(filename, content, type) {
  const blob = new Blob([content], { type });
  const url = URL.createObjectURL(blob);
  const a = document.createElement('a');
  a.href = url;
  a.download = filename;
  document.body.appendChild(a);
  a.click();
  document.body.removeChild(a);
  URL.revokeObjectURL(url);
}

// Navigation & Tab Switcher
function setupNavigation() {
  const navTabs = document.querySelectorAll('.nav-tab');
  navTabs.forEach(tab => {
    tab.addEventListener('click', () => {
      const target = tab.dataset.tab;
      if (!target) return;

      navTabs.forEach(t => t.classList.remove('active'));
      tab.classList.add('active');

      document.querySelectorAll('.tab-pane').forEach(p => p.classList.remove('active'));
      const activePane = document.getElementById(`tab-${target}`);
      if (activePane) activePane.classList.add('active');

      state.activeTab = target;

      // Redraw charts if target needs it
      if (target === 'overview' || target === 'monitor') {
        setTimeout(() => {
          if (state.metrics) {
            MiniChart.drawLineChart('rpsLiveChart', state.rpsHistory, '#22d3ee', 'rgba(6, 182, 212, 0.2)');
            MiniChart.drawBarChart(
              'statusLiveChart',
              ['2xx Success', '3xx Redir', '4xx Client', '5xx Server'],
              [state.metrics.status_2xx, state.metrics.status_3xx, state.metrics.status_4xx, state.metrics.status_5xx],
              ['#10b981', '#38bdf8', '#fbbf24', '#ef4444']
            );
          }
        }, 50);
      }
    });
  });

  // Polling rate selector
  const pollSel = document.getElementById('pollRateSelect');
  if (pollSel) {
    pollSel.addEventListener('change', e => {
      const val = parseInt(e.target.value, 10);
      clearInterval(state.pollTimer);
      if (val > 0) {
        state.pollIntervalMs = val;
        state.isPolling = true;
        state.pollTimer = setInterval(fetchServerData, state.pollIntervalMs);
      } else {
        state.isPolling = false;
      }
    });
  }

  // Filter & Search events
  document.getElementById('eventFilterSelect')?.addEventListener('change', () => renderEventsTable(state.events));
  document.getElementById('eventSearchInput')?.addEventListener('input', () => renderEventsTable(state.events));

  // Action Buttons
  document.getElementById('btnRunRace')?.addEventListener('click', runRaceConditionExperiment);
  document.getElementById('btnRunDeadlock')?.addEventListener('click', runDeadlockDemo);
  document.getElementById('btnRunBench')?.addEventListener('click', runLoadBenchmark);
  document.getElementById('btnRunComparison')?.addEventListener('click', runPerformanceComparison);
  document.getElementById('btnToggleAdaptive')?.addEventListener('click', toggleAdaptivePool);
  document.getElementById('btnExportJson')?.addEventListener('click', exportMetricsJson);
  document.getElementById('btnExportCsv')?.addEventListener('click', exportEventsCsv);

  // Security Simulation Buttons
  document.getElementById('btnSimTraversal')?.addEventListener('click', () => simulateSecurityAttack('traversal'));
  document.getElementById('btnSimBadMethod')?.addEventListener('click', () => simulateSecurityAttack('bad_method'));
  document.getElementById('btnSimOverflow')?.addEventListener('click', () => simulateSecurityAttack('overflow'));

  // Window resize handler for canvas charts
  window.addEventListener('resize', () => {
    if (state.metrics) {
      MiniChart.drawLineChart('rpsLiveChart', state.rpsHistory, '#22d3ee', 'rgba(6, 182, 212, 0.2)');
      MiniChart.drawBarChart(
        'statusLiveChart',
        ['2xx Success', '3xx Redir', '4xx Client', '5xx Server'],
        [state.metrics.status_2xx, state.metrics.status_3xx, state.metrics.status_4xx, state.metrics.status_5xx],
        ['#10b981', '#38bdf8', '#fbbf24', '#ef4444']
      );
    }
  });
}

// Initial Boot
document.addEventListener('DOMContentLoaded', () => {
  setupNavigation();
  fetchServerData();
  state.pollTimer = setInterval(fetchServerData, state.pollIntervalMs);
});

