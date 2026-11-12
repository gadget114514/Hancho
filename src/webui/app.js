/**
 * Hancho Dashboard WebUI Client - 5 Panes Implementation
 * SPA for Linux / macOS browser dashboard.
 * Panes: Processes, Task Queue, Monitor, Failure Diagnosis, Memory.
 */
(function() {
  'use strict';

  // --- Token Management & URL Scrubbing ---
  const params = new URLSearchParams(window.location.search);
  let sessionToken = params.get('token') || '';
  if (sessionToken) {
    sessionStorage.setItem('hancho_token', sessionToken);
    const cleanUrl = window.location.pathname + (window.location.hash || '');
    window.history.replaceState({}, document.title, cleanUrl);
  } else {
    sessionToken = sessionStorage.getItem('hancho_token') || '';
  }

  // --- State ---
  const state = {
    connected: false,
    activeTab: 'processes',
    autoRefresh: true,
    refreshIntervalId: null,
    ws: null,
    reconnectTimer: null,

    // Processes
    processes: [],
    procFilter: 'all',
    procSearch: '',
    selectedProcId: null,
    procLogsInterval: null,

    // Task Queue
    jobs: [],
    tqFilter: 'all',
    tqSearch: '',
    tqCounts: { total: 0, running: 0, waiting: 0, pending: 0, blocked: 0, succeeded: 0, failed: 0, cancelled: 0 },
    selectedJob: null,
    resourceLocks: true,

    // Monitor
    monitorAgentId: null,
    monitorAgents: [],
    monitorEvents: [],
    monitorEventFilter: '',
    usageHistory: [],
    usageDaily: [],

    // Failure Diagnosis
    diagBreakdown: {},
    diagnosedFailures: [],
    diagSearch: '',

    // Memory
    memoryStorageStats: null,
    memoryRecords: [],
    memLayerFilter: 'all',
    memSearch: '',
    selectedMemoryRecord: null
  };

  // --- DOM Elements ---
  const elStatusDot = document.getElementById('status-dot');
  const elStatusText = document.getElementById('status-text');
  const elReconnectBanner = document.getElementById('reconnect-banner');
  const elReconnectMsg = document.getElementById('reconnect-msg');
  const elBtnReconnectNow = document.getElementById('btn-reconnect-now');
  const elBtnRefresh = document.getElementById('btn-refresh');
  const elChkAutoRefresh = document.getElementById('chk-auto-refresh');
  const elNavTabs = document.querySelectorAll('.nav-tab');

  // Badges in Header
  const elBadgeProcCount = document.getElementById('badge-proc-count');
  const elBadgeTqCount = document.getElementById('badge-tq-count');
  const elBadgeFailCount = document.getElementById('badge-fail-count');
  const elBadgeMemCount = document.getElementById('badge-mem-count');

  // Processes Elements
  const elProcTbody = document.getElementById('proc-tbody');
  const elInputProcSearch = document.getElementById('input-proc-search');
  const elProcFilterPills = document.getElementById('proc-filter-pills');
  const elBtnProcStart = document.getElementById('btn-proc-start');
  const elBtnProcKillAll = document.getElementById('btn-proc-kill-all');
  const elBtnProcClear = document.getElementById('btn-proc-clear');
  const elBtnProcResetIds = document.getElementById('btn-proc-reset-ids');

  // Task Queue Elements
  const elTqTbody = document.getElementById('tq-tbody');
  const elInputTqSearch = document.getElementById('input-tq-search');
  const elTqFilterPills = document.getElementById('tq-filter-pills');
  const elBtnTqPause = document.getElementById('btn-tq-pause');
  const elBtnTqResume = document.getElementById('btn-tq-resume');
  const elBtnTqPurge = document.getElementById('btn-tq-purge');
  const elChkResourceLocks = document.getElementById('chk-resource-locks');

  // Task Queue KPI Cards
  const elKpiTqTotal = document.getElementById('kpi-tq-total');
  const elKpiTqRunning = document.getElementById('kpi-tq-running');
  const elKpiTqWaiting = document.getElementById('kpi-tq-waiting');
  const elKpiTqPending = document.getElementById('kpi-tq-pending');
  const elKpiTqBlocked = document.getElementById('kpi-tq-blocked');
  const elKpiTqSucceeded = document.getElementById('kpi-tq-succeeded');
  const elKpiTqFailed = document.getElementById('kpi-tq-failed');
  const elKpiTqCancelled = document.getElementById('kpi-tq-cancelled');

  // Monitor Elements
  const elSelectMonitorAgent = document.getElementById('select-monitor-agent');
  const elMonitorAgentStatus = document.getElementById('monitor-agent-status');
  const elInputAgentGoal = document.getElementById('input-agent-goal');
  const elInputAgentNotes = document.getElementById('input-agent-notes');
  const elBtnSaveAgentInfo = document.getElementById('btn-save-agent-info');
  const elMonitorEventsList = document.getElementById('monitor-events-list');
  const elMonitorEventFilters = document.getElementById('monitor-event-filters');
  const elStatPromptTokens = document.getElementById('stat-prompt-tokens');
  const elStatOutputTokens = document.getElementById('stat-output-tokens');
  const elStatTotalTokens = document.getElementById('stat-total-tokens');
  const elStatCostUsd = document.getElementById('stat-cost-usd');
  const elUsageDailyTbody = document.getElementById('usage-daily-tbody');
  const elChartBars = document.getElementById('chart-bars');
  const elChartYMid = document.getElementById('chart-y-mid');
  const elChartYMax = document.getElementById('chart-y-max');

  // Failure Diagnosis Elements
  const elDiagClassesContainer = document.getElementById('diag-classes-container');
  const elDiagCardsList = document.getElementById('diag-cards-list');
  const elInputDiagSearch = document.getElementById('input-diag-search');
  const elBtnRefreshDiag = document.getElementById('btn-refresh-diag');

  // Memory Elements
  const elMemStatTotal = document.getElementById('mem-stat-total');
  const elMemStatL1 = document.getElementById('mem-stat-l1');
  const elMemStatL2 = document.getElementById('mem-stat-l2');
  const elMemStatL3 = document.getElementById('mem-stat-l3');
  const elMemStatWarning = document.getElementById('mem-stat-warning');
  const elQuotaBarFill = document.getElementById('quota-bar-fill');
  const elInputMemSearch = document.getElementById('input-mem-search');
  const elMemLayerPills = document.getElementById('mem-layer-pills');
  const elMemTbody = document.getElementById('mem-tbody');
  const elBtnMemPrune = document.getElementById('btn-mem-prune');

  // Modals
  const elModalStartProc = document.getElementById('modal-start-proc');
  const elBtnSubmitStartProc = document.getElementById('btn-submit-start-proc');
  const elFormProcCmd = document.getElementById('form-proc-cmd');
  const elFormProcName = document.getElementById('form-proc-name');
  const elFormProcCwd = document.getElementById('form-proc-cwd');
  const elFormProcMode = document.getElementById('form-proc-mode');
  const elFormProcTimeout = document.getElementById('form-proc-timeout');

  const elModalProcLogs = document.getElementById('modal-proc-logs');
  const elTerminalOutput = document.getElementById('terminal-output');
  const elInputProcStdin = document.getElementById('input-proc-stdin');
  const elBtnSendStdin = document.getElementById('btn-send-stdin');
  const elLogsMetaInfo = document.getElementById('logs-meta-info');

  const elModalJobDetails = document.getElementById('modal-job-details');
  const elJobModalBody = document.getElementById('job-modal-body');
  const elBtnJobModalRetry = document.getElementById('btn-job-modal-retry');
  const elBtnJobModalCancel = document.getElementById('btn-job-modal-cancel');

  const elModalMemDetails = document.getElementById('modal-mem-details');
  const elMemModalBody = document.getElementById('mem-modal-body');

  const elModalMemPrune = document.getElementById('modal-mem-prune');
  const elInputPruneDays = document.getElementById('input-prune-days');
  const elChkPruneDryRun = document.getElementById('chk-prune-dry-run');
  const elBtnSubmitPrune = document.getElementById('btn-submit-prune');
  const elPruneResultBox = document.getElementById('prune-result-box');

  // --- WebSocket Connection ---
  function getWsUrl() {
    const loc = window.location;
    const proto = loc.protocol === 'https:' ? 'wss:' : 'ws:';
    const tokenQuery = sessionToken ? '?token=' + encodeURIComponent(sessionToken) : '';
    return `${proto}//${loc.host}/ws${tokenQuery}`;
  }

  function setConnectionState(status) {
    if (status === 'connected') {
      state.connected = true;
      state.reconnecting = false;
      elStatusDot.className = 'status-dot connected';
      elStatusText.textContent = 'Connected';
      elReconnectBanner.classList.add('hidden');
    } else if (status === 'connecting') {
      state.connected = false;
      elStatusDot.className = 'status-dot connecting';
      elStatusText.textContent = 'Connecting...';
    } else {
      state.connected = false;
      elStatusDot.className = 'status-dot disconnected';
      elStatusText.textContent = 'Disconnected';
      elReconnectBanner.classList.remove('hidden');
    }
  }

  function connectWs() {
    if (state.ws) {
      try { state.ws.close(); } catch (e) {}
      state.ws = null;
    }
    if (state.reconnectTimer) {
      clearTimeout(state.reconnectTimer);
      state.reconnectTimer = null;
    }

    setConnectionState('connecting');

    try {
      const wsUrl = getWsUrl();
      const ws = new WebSocket(wsUrl);
      state.ws = ws;

      ws.onopen = function() {
        setConnectionState('connected');
        if (sessionToken) {
          ws.send(JSON.stringify({ action: 'auth', token: sessionToken }));
        }
        refreshCurrentTab();
      };

      ws.onmessage = function(event) {
        try {
          const data = JSON.parse(event.data);
          handleWsMessage(data);
        } catch (err) {
          console.error('Failed to parse WS message:', err);
        }
      };

      ws.onclose = function() {
        setConnectionState('disconnected');
        scheduleReconnect();
      };

      ws.onerror = function() {
        setConnectionState('disconnected');
        try { ws.close(); } catch (e) {}
      };
    } catch (e) {
      console.error('WebSocket connection error:', e);
      setConnectionState('disconnected');
      scheduleReconnect();
    }
  }

  function scheduleReconnect() {
    if (state.reconnectTimer) return;
    state.reconnecting = true;
    elReconnectMsg.textContent = 'Connection lost. Reconnecting to Hancho core... Background jobs are not affected.';
    state.reconnectTimer = setTimeout(function() {
      state.reconnectTimer = null;
      connectWs();
    }, 2000);
  }

  function sendWsRequest(payload) {
    if (state.ws && state.ws.readyState === WebSocket.OPEN) {
      state.ws.send(JSON.stringify(payload));
      return true;
    }
    return false;
  }

  // --- Message Handling ---
  function handleWsMessage(data) {
    if (!data) return;

    // Process list response
    if (data.processes && Array.isArray(data.processes)) {
      state.processes = data.processes;
      elBadgeProcCount.textContent = state.processes.length;
      renderProcessesTable();
      updateMonitorAgentOptions();
      return;
    }

    // Task queue jobs list response
    if (data.jobs && Array.isArray(data.jobs)) {
      state.jobs = data.jobs;
      elBadgeTqCount.textContent = state.jobs.length;
      renderTaskQueueTable();
      checkFailuresFromJobs();
      return;
    }

    // Task queue counts response
    if (data.counts && typeof data.counts === 'object') {
      state.tqCounts = data.counts;
      updateTqKpis();
      return;
    }

    // Resource locks response
    if (typeof data.enabled === 'boolean') {
      state.resourceLocks = data.enabled;
      elChkResourceLocks.checked = state.resourceLocks;
      return;
    }

    // Process Logs output response
    if (data.action === 'logs' || (data.ok && typeof data.output === 'string')) {
      if (state.selectedProcId && data.id === state.selectedProcId) {
        elTerminalOutput.textContent = data.output || '(no output produced yet)';
        elTerminalOutput.scrollTop = elTerminalOutput.scrollHeight;
      }
      return;
    }

    // Agent Stream events
    if (data.events && Array.isArray(data.events)) {
      state.monitorEvents = data.events;
      renderMonitorEvents();
      return;
    }

    // Agent State
    if (data.agent_type !== undefined && data.status !== undefined && data.id !== undefined) {
      updateAgentOverview(data);
      return;
    }

    // Usage History
    if (data.points && Array.isArray(data.points)) {
      state.usageHistory = data.points;
      renderUsageAnalytics();
      return;
    }

    // Daily Usage
    if (data.daily && Array.isArray(data.daily)) {
      state.usageDaily = data.daily;
      renderDailyUsage();
      return;
    }

    // Failure Diagnosis breakdown
    if (data.classes && typeof data.classes === 'object') {
      state.diagBreakdown = data.classes;
      renderDiagBreakdown();
      return;
    }

    // Memory storage stats
    if (data.total_bytes !== undefined && data.l1_bytes !== undefined) {
      state.memoryStorageStats = data;
      renderMemoryStats();
      return;
    }

    // Memory Search results
    if (data.results && Array.isArray(data.results)) {
      state.memoryRecords = data.results;
      elBadgeMemCount.textContent = state.memoryRecords.length;
      renderMemoryTable();
      return;
    }
  }

  // --- Tab Navigation & Refreshing ---
  function switchTab(tabName) {
    state.activeTab = tabName;

    elNavTabs.forEach(tab => {
      if (tab.getAttribute('data-tab') === tabName) {
        tab.classList.add('active');
      } else {
        tab.classList.remove('active');
      }
    });

    document.querySelectorAll('.tab-pane').forEach(pane => {
      if (pane.id === `pane-${tabName}`) {
        pane.classList.add('active');
      } else {
        pane.classList.remove('active');
      }
    });

    refreshCurrentTab();
  }

  function refreshCurrentTab() {
    if (!state.connected) return;

    // Always keep counts fresh
    sendWsRequest({ action: 'taskqueue_counts' });

    if (state.activeTab === 'processes') {
      sendWsRequest({ action: 'list' });
    } else if (state.activeTab === 'taskqueue') {
      sendWsRequest({ action: 'taskqueue_list' });
      sendWsRequest({ action: 'taskqueue_resource_locks' });
    } else if (state.activeTab === 'monitor') {
      sendWsRequest({ action: 'list' });
      sendWsRequest({ action: 'get_usage_history', since_sec: 86400 });
      sendWsRequest({ action: 'get_usage_daily' });
      if (state.monitorAgentId) {
        sendWsRequest({ action: 'get_agent_state', id: state.monitorAgentId });
        sendWsRequest({ action: 'get_agent_stream', id: state.monitorAgentId, schema: 'v1', tail: 50 });
      }
    } else if (state.activeTab === 'diagnosis') {
      sendWsRequest({ action: 'taskqueue_diagnose', breakdown: true });
      sendWsRequest({ action: 'taskqueue_list', state: 'failed' });
    } else if (state.activeTab === 'memory') {
      sendWsRequest({ action: 'memory_storage_stats' });
      searchMemory();
    }
  }

  function setupAutoRefresh() {
    if (state.refreshIntervalId) {
      clearInterval(state.refreshIntervalId);
      state.refreshIntervalId = null;
    }
    if (state.autoRefresh) {
      state.refreshIntervalId = setInterval(function() {
        if (state.connected) {
          refreshCurrentTab();
        }
      }, 3000);
    }
  }

  // --- Utility Helpers ---
  function escapeHtml(str) {
    if (str === undefined || str === null) return '';
    return String(str)
      .replace(/&/g, '&amp;')
      .replace(/</g, '&lt;')
      .replace(/>/g, '&gt;')
      .replace(/"/g, '&quot;')
      .replace(/'/g, '&#039;');
  }

  function formatDuration(sec) {
    if (sec === undefined || sec === null || isNaN(sec) || sec < 0) return '-';
    if (sec < 60) return sec.toFixed(1) + 's';
    const m = Math.floor(sec / 60);
    const s = Math.floor(sec % 60);
    return `${m}m ${s}s`;
  }

  function formatTime(isoOrEpoch) {
    if (!isoOrEpoch) return '-';
    try {
      let d;
      if (typeof isoOrEpoch === 'number') {
        d = new Date(isoOrEpoch * 1000);
      } else {
        d = new Date(isoOrEpoch);
      }
      if (isNaN(d.getTime())) return String(isoOrEpoch);
      return d.toLocaleTimeString([], { hour: '2-digit', minute: '2-digit', second: '2-digit' });
    } catch (e) {
      return String(isoOrEpoch);
    }
  }

  function formatBytes(bytes) {
    if (bytes === undefined || bytes === null || isNaN(bytes)) return '-';
    const b = Number(bytes);
    if (b < 1024) return b + ' B';
    if (b < 1024 * 1024) return (b / 1024).toFixed(1) + ' KB';
    if (b < 1024 * 1024 * 1024) return (b / (1024 * 1024)).toFixed(1) + ' MB';
    return (b / (1024 * 1024 * 1024)).toFixed(2) + ' GB';
  }

  function getBadgeClass(stateStr) {
    const s = (stateStr || '').toLowerCase();
    if (s === 'running') return 'badge badge-running';
    if (s === 'pending' || s === 'queued' || s === 'waiting') return 'badge badge-pending';
    if (s === 'blocked') return 'badge badge-blocked';
    if (s === 'completed' || s === 'succeeded') return 'badge badge-completed';
    if (s === 'failed' || s === 'timed_out' || s === 'idle_timed_out') return 'badge badge-failed';
    if (s === 'cancelled' || s === 'killed') return 'badge badge-cancelled';
    return 'badge';
  }

  // ==========================================
  // PANE 1: PROCESSES
  // ==========================================
  function renderProcessesTable() {
    const q = state.procSearch.toLowerCase().trim();
    const filter = state.procFilter.toLowerCase();

    const filtered = state.processes.filter(p => {
      const st = (p.state || '').toLowerCase();
      if (filter !== 'all') {
        if (filter === 'completed' && st !== 'completed' && st !== 'finished') return false;
        if (filter === 'failed' && st !== 'failed' && st !== 'timed_out' && st !== 'idle_timed_out') return false;
        if (filter === 'running' && st !== 'running') return false;
      }
      if (q) {
        const idStr = String(p.id || '');
        const pidStr = String(p.pid || '');
        const cmdStr = (p.command || p.commandLine || p.cmd || '').toLowerCase();
        const nameStr = (p.name || '').toLowerCase();
        if (!idStr.includes(q) && !pidStr.includes(q) && !cmdStr.includes(q) && !nameStr.includes(q)) {
          return false;
        }
      }
      return true;
    });

    if (filtered.length === 0) {
      const msg = state.processes.length === 0 ? 'No processes found.' : 'No processes match the filter.';
      elProcTbody.innerHTML = `<tr class="empty-row"><td colspan="9">${escapeHtml(msg)}</td></tr>`;
      return;
    }

    let html = '';
    for (const p of filtered) {
      const id = p.id;
      const pid = p.pid !== undefined && p.pid > 0 ? p.pid : '-';
      const cmd = escapeHtml(p.name || p.command || p.commandLine || p.cmd || '-');
      const stateBadge = `<span class="${getBadgeClass(p.state)}">${escapeHtml(p.state || 'unknown')}</span>`;
      const cpu = p.cpu_percent !== undefined ? p.cpu_percent.toFixed(1) + '%' : '-';
      const mem = formatBytes(p.memory_bytes || p.workingSetSize);
      const exitCode = p.exit_code !== undefined ? p.exit_code : '-';
      const started = formatTime(p.start_time || p.createdAt);

      const isRunning = (p.state || '').toLowerCase() === 'running';

      html += `<tr data-proc-id="${id}">
        <td class="font-mono">#${id}</td>
        <td>${stateBadge}</td>
        <td class="font-mono">${pid}</td>
        <td class="job-cmd" title="${cmd}">${cmd}</td>
        <td>${cpu}</td>
        <td>${mem}</td>
        <td>${exitCode}</td>
        <td>${started}</td>
        <td style="text-align: center;">
          <button class="btn btn-sm btn-proc-logs" data-id="${id}">Logs</button>
          ${isRunning
            ? `<button class="btn btn-sm btn-danger btn-proc-kill" data-id="${id}">Kill</button>`
            : `<button class="btn btn-sm btn-proc-remove" data-id="${id}">Remove</button>`
          }
        </td>
      </tr>`;
    }

    elProcTbody.innerHTML = html;

    // Attach listeners
    elProcTbody.querySelectorAll('.btn-proc-logs').forEach(btn => {
      btn.addEventListener('click', function(e) {
        e.stopPropagation();
        openProcessLogs(Number(this.getAttribute('data-id')));
      });
    });

    elProcTbody.querySelectorAll('.btn-proc-kill').forEach(btn => {
      btn.addEventListener('click', function(e) {
        e.stopPropagation();
        const procId = Number(this.getAttribute('data-id'));
        if (confirm(`Kill process #${procId}?`)) {
          sendWsRequest({ action: 'kill', id: procId });
          setTimeout(() => sendWsRequest({ action: 'list' }), 200);
        }
      });
    });

    elProcTbody.querySelectorAll('.btn-proc-remove').forEach(btn => {
      btn.addEventListener('click', function(e) {
        e.stopPropagation();
        const procId = Number(this.getAttribute('data-id'));
        sendWsRequest({ action: 'remove', id: procId });
        setTimeout(() => sendWsRequest({ action: 'list' }), 200);
      });
    });
  }

  function openProcessLogs(procId) {
    state.selectedProcId = procId;
    const proc = state.processes.find(p => p.id === procId);
    const title = proc ? `Process #${procId} (${proc.command || proc.name || 'proc'})` : `Process #${procId}`;
    document.getElementById('logs-modal-title').textContent = title;
    elTerminalOutput.textContent = 'Fetching logs...';
    elLogsMetaInfo.textContent = proc ? `State: ${proc.state} | PID: ${proc.pid || '-'} | Mode: ${proc.mode || 'pipe'}` : '';

    elModalProcLogs.classList.remove('hidden');

    // Request logs immediately
    sendWsRequest({ action: 'logs', id: procId, tail: 200 });

    // Poll logs while open
    if (state.procLogsInterval) clearInterval(state.procLogsInterval);
    state.procLogsInterval = setInterval(() => {
      if (state.selectedProcId) {
        sendWsRequest({ action: 'logs', id: state.selectedProcId, tail: 200 });
      }
    }, 1500);
  }

  function closeProcessLogs() {
    elModalProcLogs.classList.add('hidden');
    state.selectedProcId = null;
    if (state.procLogsInterval) {
      clearInterval(state.procLogsInterval);
      state.procLogsInterval = null;
    }
  }

  // ==========================================
  // PANE 2: TASK QUEUE
  // ==========================================
  function updateTqKpis() {
    const c = state.tqCounts;
    elKpiTqTotal.textContent = c.total || state.jobs.length;
    elKpiTqRunning.textContent = c.running || 0;
    elKpiTqWaiting.textContent = c.waiting || 0;
    elKpiTqPending.textContent = c.pending || 0;
    elKpiTqBlocked.textContent = c.blocked || 0;
    elKpiTqSucceeded.textContent = c.succeeded || 0;
    elKpiTqFailed.textContent = c.failed || 0;
    elKpiTqCancelled.textContent = c.cancelled || 0;
  }

  function renderTaskQueueTable() {
    const q = state.tqSearch.toLowerCase().trim();
    const filter = state.tqFilter.toLowerCase();

    const filtered = state.jobs.filter(job => {
      const st = (job.state || '').toLowerCase();
      if (filter !== 'all') {
        if (filter === 'succeeded' && st !== 'succeeded' && st !== 'completed') return false;
        if (filter === 'failed' && st !== 'failed' && st !== 'timed_out' && st !== 'idle_timed_out') return false;
        if (filter === 'pending' && st !== 'pending' && st !== 'queued' && st !== 'waiting') return false;
        if (filter === 'running' && st !== 'running') return false;
        if (filter === 'cancelled' && st !== 'cancelled' && st !== 'killed') return false;
      }
      if (q) {
        const key = (job.key || job.id || '').toLowerCase();
        const name = (job.name || '').toLowerCase();
        const queue = (job.taskqueue || job.queue || '').toLowerCase();
        const cmd = (job.commandLine || job.cmd || '').toLowerCase();
        if (!key.includes(q) && !name.includes(q) && !queue.includes(q) && !cmd.includes(q)) {
          return false;
        }
      }
      return true;
    });

    if (filtered.length === 0) {
      const msg = state.jobs.length === 0 ? 'No jobs in queue.' : 'No jobs match current filter.';
      elTqTbody.innerHTML = `<tr class="empty-row"><td colspan="9">${escapeHtml(msg)}</td></tr>`;
      return;
    }

    let html = '';
    for (const job of filtered) {
      const key = escapeHtml(job.key || job.id || '-');
      const name = escapeHtml(job.name || '(unnamed)');
      const queue = escapeHtml(job.taskqueue || job.queue || 'default');
      const cmd = escapeHtml(job.commandLine || job.cmd || '-');
      const stateBadge = `<span class="${getBadgeClass(job.state)}">${escapeHtml(job.state || 'unknown')}</span>`;
      const exitCode = job.exitCode !== undefined ? job.exitCode : '-';
      const duration = formatDuration(job.durationSec || job.elapsedSec);
      const updated = formatTime(job.updatedAt || job.createdAt);

      const isRunning = (job.state || '').toLowerCase() === 'running';
      const isFailed = (job.state || '').toLowerCase() === 'failed';

      html += `<tr data-job-key="${key}">
        <td>${stateBadge}</td>
        <td class="job-key font-mono" title="${key}">${key}</td>
        <td class="job-name" title="${name}">${name}</td>
        <td>${queue}</td>
        <td class="job-cmd" title="${cmd}">${cmd}</td>
        <td>${exitCode}</td>
        <td>${duration}</td>
        <td>${updated}</td>
        <td style="text-align: center;">
          <button class="btn btn-sm btn-job-details" data-key="${key}">Details</button>
          ${isFailed ? `<button class="btn btn-sm btn-primary btn-job-retry" data-key="${key}">Retry</button>` : ''}
          ${isRunning ? `<button class="btn btn-sm btn-danger btn-job-cancel" data-key="${key}">Cancel</button>` : ''}
        </td>
      </tr>`;
    }

    elTqTbody.innerHTML = html;

    // Attach listeners
    elTqTbody.querySelectorAll('.btn-job-details').forEach(btn => {
      btn.addEventListener('click', function(e) {
        e.stopPropagation();
        openJobDetailsModal(this.getAttribute('data-key'));
      });
    });

    elTqTbody.querySelectorAll('.btn-job-retry').forEach(btn => {
      btn.addEventListener('click', function(e) {
        e.stopPropagation();
        const key = this.getAttribute('data-key');
        sendWsRequest({ action: 'taskqueue_retry', id: key });
        setTimeout(() => sendWsRequest({ action: 'taskqueue_list' }), 200);
      });
    });

    elTqTbody.querySelectorAll('.btn-job-cancel').forEach(btn => {
      btn.addEventListener('click', function(e) {
        e.stopPropagation();
        const key = this.getAttribute('data-key');
        sendWsRequest({ action: 'taskqueue_cancel', id: key });
        setTimeout(() => sendWsRequest({ action: 'taskqueue_list' }), 200);
      });
    });
  }

  function openJobDetailsModal(key) {
    const job = state.jobs.find(j => (j.key || j.id) === key);
    if (!job) return;

    state.selectedJob = job;
    document.getElementById('job-modal-title').textContent = `Job: ${job.name || key}`;

    const fields = [
      ['Job Key / ID', job.key || job.id],
      ['Name', job.name],
      ['Queue', job.taskqueue || job.queue || 'default'],
      ['State', job.state],
      ['Command', job.commandLine || job.cmd],
      ['Working Dir', job.workingDir || job.cwd || '-'],
      ['Mode', job.mode || 'pipe'],
      ['Exit Code', job.exitCode !== undefined ? job.exitCode : '-'],
      ['Duration', formatDuration(job.durationSec || job.elapsedSec)],
      ['Created At', job.createdAt || '-'],
      ['Started At', job.startedAt || '-'],
      ['Updated At', job.updatedAt || '-'],
      ['Attempt', job.attemptN !== undefined ? job.attemptN : '-'],
      ['Supervisor Notes', job.supervisorNotes || '-'],
      ['Stdout Path', job.stdoutPath || '-'],
      ['Stderr Path', job.stderrPath || '-']
    ];

    let detailsHtml = '';
    for (const [k, v] of fields) {
      detailsHtml += `<div class="detail-row">
        <div class="detail-label">${escapeHtml(k)}:</div>
        <div class="detail-value">${escapeHtml(String(v || '-'))}</div>
      </div>`;
    }

    elJobModalBody.innerHTML = detailsHtml;

    // Configure footer buttons
    const isRunning = (job.state || '').toLowerCase() === 'running';
    const isFailed = (job.state || '').toLowerCase() === 'failed';
    elBtnJobModalRetry.style.display = isFailed ? 'inline-flex' : 'none';
    elBtnJobModalCancel.style.display = isRunning ? 'inline-flex' : 'none';

    elModalJobDetails.classList.remove('hidden');
  }

  // ==========================================
  // PANE 3: MONITOR
  // ==========================================
  function updateMonitorAgentOptions() {
    const prevSelected = elSelectMonitorAgent.value;
    let optionsHtml = '<option value="">(Select a process to monitor)</option>';

    for (const p of state.processes) {
      const isRunning = (p.state || '').toLowerCase() === 'running';
      const label = `#${p.id} - ${p.name || p.command || 'proc'} [${p.state}]`;
      optionsHtml += `<option value="${p.id}" ${isRunning ? 'style="color:#38bdf8;font-weight:bold;"' : ''}>${escapeHtml(label)}</option>`;
    }

    elSelectMonitorAgent.innerHTML = optionsHtml;
    if (prevSelected) {
      elSelectMonitorAgent.value = prevSelected;
    } else if (state.processes.length > 0) {
      // Pick first running or first available
      const firstRunning = state.processes.find(p => (p.state || '').toLowerCase() === 'running');
      if (firstRunning) {
        elSelectMonitorAgent.value = firstRunning.id;
        state.monitorAgentId = firstRunning.id;
        requestAgentMonitorData(firstRunning.id);
      }
    }
  }

  function requestAgentMonitorData(procId) {
    if (!procId) return;
    state.monitorAgentId = procId;
    sendWsRequest({ action: 'get_agent_state', id: procId });
    sendWsRequest({ action: 'get_agent_stream', id: procId, schema: 'v1', tail: 50 });
  }

  function updateAgentOverview(agentData) {
    elMonitorAgentStatus.textContent = agentData.status || agentData.agent_type || 'Active';
    elMonitorAgentStatus.className = 'badge ' + getBadgeClass(agentData.status);
    elInputAgentGoal.value = agentData.supervisor_goal || agentData.goal || '';
    elInputAgentNotes.value = agentData.supervisor_notes || agentData.notes || '';
  }

  function renderMonitorEvents() {
    const filter = state.monitorEventFilter.toLowerCase();
    const filtered = state.monitorEvents.filter(ev => {
      if (!filter) return true;
      const type = (ev.type || '').toLowerCase();
      if (filter === 'tool_call' && type.includes('tool')) return true;
      if (filter === 'thought' && (type.includes('thought') || type.includes('think'))) return true;
      if (filter === 'progress' && type.includes('prog')) return true;
      if (filter === 'error' && (type.includes('err') || ev.is_error)) return true;
      return type.includes(filter);
    });

    if (filtered.length === 0) {
      elMonitorEventsList.innerHTML = `<div class="empty-message">${state.monitorEvents.length === 0 ? 'No events recorded for this agent.' : 'No events match the selected filter.'}</div>`;
      return;
    }

    let html = '';
    for (const ev of filtered) {
      const type = ev.type || 'event';
      const timeStr = formatTime(ev.ts || ev.timestamp);
      const payloadStr = typeof ev.payload === 'object' ? JSON.stringify(ev.payload, null, 2) : (ev.data || ev.detail || JSON.stringify(ev));

      html += `<div class="event-card">
        <div class="event-card-header">
          <span class="badge ${type.includes('err') ? 'badge-failed' : 'badge-running'}">${escapeHtml(type)}</span>
          <span class="text-muted font-mono" style="font-size:0.75rem;">${timeStr}</span>
        </div>
        <div class="event-card-body">${escapeHtml(payloadStr)}</div>
      </div>`;
    }

    elMonitorEventsList.innerHTML = html;
  }

  function renderUsageAnalytics() {
    let promptTokens = 0;
    let outputTokens = 0;
    let totalTokens = 0;
    let cost = 0.0;

    for (const pt of state.usageHistory) {
      promptTokens += (pt.input || pt.input_tokens || 0);
      outputTokens += (pt.output || pt.output_tokens || 0);
      totalTokens += (pt.total || pt.total_tokens || 0);
      cost += (pt.cost || pt.cost_usd || 0.0);
    }

    elStatPromptTokens.textContent = promptTokens.toLocaleString();
    elStatOutputTokens.textContent = outputTokens.toLocaleString();
    elStatTotalTokens.textContent = totalTokens.toLocaleString();
    elStatCostUsd.textContent = '$' + cost.toFixed(4);

    // Render SVG Bar Chart
    renderSvgUsageChart();
  }

  function renderSvgUsageChart() {
    if (!state.usageHistory || state.usageHistory.length === 0) {
      elChartBars.innerHTML = '';
      elChartYMid.textContent = '-';
      elChartYMax.textContent = '-';
      return;
    }

    const pts = state.usageHistory.slice(-20); // Last 20 points
    let maxVal = 100;
    for (const pt of pts) {
      const val = pt.total || 0;
      if (val > maxVal) maxVal = val;
    }

    elChartYMax.textContent = maxVal.toLocaleString();
    elChartYMid.textContent = Math.round(maxVal / 2).toLocaleString();

    const chartWidth = 540;
    const chartHeight = 130;
    const startX = 40;
    const barWidth = Math.max(6, Math.floor(chartWidth / pts.length) - 4);

    let barsHtml = '';
    pts.forEach((pt, idx) => {
      const val = pt.total || 0;
      const h = Math.round((val / maxVal) * chartHeight);
      const x = startX + idx * (barWidth + 4);
      const y = 150 - h;

      barsHtml += `<rect x="${x}" y="${y}" width="${barWidth}" height="${h}" rx="2" fill="#38bdf8" opacity="0.85">
        <title>Tokens: ${val} (Cost: $${(pt.cost || 0).toFixed(4)})</title>
      </rect>`;
    });

    elChartBars.innerHTML = barsHtml;
  }

  function renderDailyUsage() {
    if (!state.usageDaily || state.usageDaily.length === 0) {
      elUsageDailyTbody.innerHTML = `<tr><td colspan="4" class="text-muted">No usage data recorded.</td></tr>`;
      return;
    }

    let html = '';
    for (const item of state.usageDaily) {
      const date = item.date || item.day || '-';
      const model = item.model || 'all';
      const tokens = (item.total_tokens || item.tokens || 0).toLocaleString();
      const cost = '$' + (item.cost_usd || item.cost || 0.0).toFixed(4);

      html += `<tr>
        <td class="font-mono">${escapeHtml(date)}</td>
        <td>${escapeHtml(model)}</td>
        <td class="font-mono">${tokens}</td>
        <td class="font-mono" style="color:#4ade80;">${cost}</td>
      </tr>`;
    }
    elUsageDailyTbody.innerHTML = html;
  }

  // ==========================================
  // PANE 4: FAILURE DIAGNOSIS
  // ==========================================
  function checkFailuresFromJobs() {
    const failedJobs = state.jobs.filter(j => {
      const s = (j.state || '').toLowerCase();
      return s === 'failed' || s === 'timed_out' || s === 'idle_timed_out';
    });

    elBadgeFailCount.textContent = failedJobs.length;

    // Collect diagnoses
    if (state.activeTab === 'diagnosis') {
      sendWsRequest({ action: 'taskqueue_diagnose', breakdown: true });
    }
  }

  function renderDiagBreakdown() {
    const classes = state.diagBreakdown || {};
    const keys = Object.keys(classes);

    if (keys.length === 0) {
      elDiagClassesContainer.innerHTML = '<div class="empty-message">No failure categories currently reported.</div>';
      return;
    }

    let html = '';
    for (const k of keys) {
      const count = classes[k];
      html += `<div class="diag-class-badge">
        <span class="diag-class-label">${escapeHtml(k)}</span>
        <span class="diag-class-count">&times; ${count}</span>
      </div>`;
    }
    elDiagClassesContainer.innerHTML = html;

    // Render list of failed jobs
    renderDiagnosedCards();
  }

  function renderDiagnosedCards() {
    const q = state.diagSearch.toLowerCase().trim();
    const failedJobs = state.jobs.filter(j => {
      const s = (j.state || '').toLowerCase();
      return s === 'failed' || s === 'timed_out' || s === 'idle_timed_out';
    });

    if (failedJobs.length === 0) {
      elDiagCardsList.innerHTML = `<div class="empty-message">No failures currently detected. Everything is running smoothly!</div>`;
      return;
    }

    let html = '';
    for (const job of failedJobs) {
      const key = job.key || job.id || '-';
      const cmd = job.commandLine || job.cmd || '-';
      const exitCode = job.exitCode !== undefined ? job.exitCode : 1;
      const isTimeout = (job.state || '').toLowerCase().includes('time');

      const failureClass = isTimeout ? 'timeout' : (exitCode !== 0 ? 'exit_nonzero' : 'failure');
      const advice = isTimeout
        ? 'Consider increasing timeoutSec or idleTimeoutSec in job configuration.'
        : 'Process exited with non-zero status. Inspect output logs or fix environment dependencies.';

      if (q && !key.toLowerCase().includes(q) && !cmd.toLowerCase().includes(q) && !failureClass.includes(q)) {
        continue;
      }

      html += `<div class="diag-card">
        <div class="diag-card-header">
          <div class="diag-card-title">
            <span class="badge badge-failed">${escapeHtml(failureClass)}</span>
            <span class="font-mono">${escapeHtml(key)}</span>
            <span class="text-muted">(${escapeHtml(job.name || 'job')})</span>
          </div>
          <button class="btn btn-sm btn-job-details" data-key="${escapeHtml(key)}">Inspect Job</button>
        </div>
        <div class="diag-card-evidence">Command: ${escapeHtml(cmd)}\nExit Code: ${exitCode}\nState: ${escapeHtml(job.state)}</div>
        <div class="diag-card-advice">
          <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
            <circle cx="12" cy="12" r="10"></circle>
            <line x1="12" y1="16" x2="12" y2="12"></line>
            <line x1="12" y1="8" x2="12.01" y2="8"></line>
          </svg>
          <span>Remediation Advice: ${escapeHtml(advice)}</span>
        </div>
      </div>`;
    }

    elDiagCardsList.innerHTML = html;

    elDiagCardsList.querySelectorAll('.btn-job-details').forEach(btn => {
      btn.addEventListener('click', function() {
        openJobDetailsModal(this.getAttribute('data-key'));
      });
    });
  }

  // ==========================================
  // PANE 5: MEMORY
  // ==========================================
  function renderMemoryStats() {
    if (!state.memoryStorageStats) return;
    const st = state.memoryStorageStats;

    elMemStatTotal.textContent = formatBytes(st.total_bytes);
    elMemStatL1.textContent = formatBytes(st.l1_bytes);
    elMemStatL2.textContent = formatBytes(st.l2_bytes);
    elMemStatL3.textContent = formatBytes(st.l3_bytes);

    if (st.warning) {
      elMemStatWarning.textContent = st.warning;
      elMemStatWarning.style.color = '#f87171';
    } else {
      elMemStatWarning.textContent = 'Healthy';
      elMemStatWarning.style.color = '#4ade80';
    }

    if (st.limit_bytes && st.limit_bytes > 0) {
      const pct = Math.min(100, Math.round((st.total_bytes / st.limit_bytes) * 100));
      elQuotaBarFill.style.width = pct + '%';
      elQuotaBarFill.style.backgroundColor = pct > 85 ? '#f87171' : (pct > 70 ? '#fbbf24' : '#38bdf8');
    } else {
      elQuotaBarFill.style.width = '10%';
      elQuotaBarFill.style.backgroundColor = '#38bdf8';
    }
  }

  function searchMemory() {
    const q = state.memSearch.trim();
    const kind = state.memLayerFilter === 'all' ? '' : state.memLayerFilter;
    sendWsRequest({
      action: 'memory_search',
      text: q,
      kinds: kind ? [kind] : ['l1', 'l2', 'l3']
    });
  }

  function renderMemoryTable() {
    if (!state.memoryRecords || state.memoryRecords.length === 0) {
      elMemTbody.innerHTML = `<tr class="empty-row"><td colspan="7">No memory records found.</td></tr>`;
      return;
    }

    let html = '';
    for (const rec of state.memoryRecords) {
      const kind = (rec.kind || 'l1').toLowerCase();
      const kindBadge = `<span class="badge badge-${kind}">${kind.toUpperCase()}</span>`;
      const id = escapeHtml(rec.id || '-');
      const job = escapeHtml(rec.jobName || rec.taskqueue || '-');
      const outcome = escapeHtml(rec.outcome || rec.status || '-');
      const summary = escapeHtml(rec.summary || rec.title || rec.text || '-');
      const created = formatTime(rec.createdAt || rec.created);

      const isL2 = kind === 'l2';

      html += `<tr data-mem-id="${id}">
        <td>${kindBadge}</td>
        <td class="font-mono" title="${id}">${id}</td>
        <td>${job}</td>
        <td>${outcome}</td>
        <td class="job-cmd" title="${summary}">${summary}</td>
        <td>${created}</td>
        <td style="text-align: center;">
          <button class="btn btn-sm btn-mem-view" data-id="${id}">View</button>
          ${isL2 ? `
            <button class="btn btn-sm btn-primary btn-mem-promote" data-id="${id}" title="Promote note to L3 established knowledge">Promote</button>
            <button class="btn btn-sm btn-danger btn-mem-retract" data-id="${id}" title="Retract invalid or obsolete note">Retract</button>
          ` : ''}
        </td>
      </tr>`;
    }

    elMemTbody.innerHTML = html;

    // Attach listeners
    elMemTbody.querySelectorAll('.btn-mem-view').forEach(btn => {
      btn.addEventListener('click', function(e) {
        e.stopPropagation();
        openMemoryRecordModal(this.getAttribute('data-id'));
      });
    });

    elMemTbody.querySelectorAll('.btn-mem-promote').forEach(btn => {
      btn.addEventListener('click', function(e) {
        e.stopPropagation();
        const id = this.getAttribute('data-id');
        if (confirm(`Promote note ${id} to L3 Established Knowledge?`)) {
          sendWsRequest({ action: 'memory_promote', id: id, approved_by: 'webui:human' });
          setTimeout(searchMemory, 300);
        }
      });
    });

    elMemTbody.querySelectorAll('.btn-mem-retract').forEach(btn => {
      btn.addEventListener('click', function(e) {
        e.stopPropagation();
        const id = this.getAttribute('data-id');
        const reason = prompt(`Enter retraction reason for note ${id}:`, 'Outdated or superseded');
        if (reason) {
          sendWsRequest({ action: 'memory_retract', id: id, reason: reason, retracted_by: 'webui:human' });
          setTimeout(searchMemory, 300);
        }
      });
    });
  }

  function openMemoryRecordModal(id) {
    const rec = state.memoryRecords.find(r => r.id === id);
    if (!rec) return;

    state.selectedMemoryRecord = rec;
    document.getElementById('mem-modal-title').textContent = `Memory Record: ${id} (${(rec.kind || 'l1').toUpperCase()})`;

    let detailsHtml = '';
    const fields = [
      ['Record ID', rec.id],
      ['Kind / Layer', (rec.kind || '').toUpperCase()],
      ['Status', rec.status],
      ['Job Name', rec.jobName],
      ['Task Queue', rec.taskqueue],
      ['Outcome', rec.outcome],
      ['Failure Class', rec.failureClass],
      ['Engine', rec.engine],
      ['Model', rec.model],
      ['Score', rec.score],
      ['Created At', rec.createdAt],
      ['Expires At', rec.expiresAt],
      ['Summary', rec.summary]
    ];

    for (const [k, v] of fields) {
      if (v !== undefined && v !== null && v !== '') {
        detailsHtml += `<div class="detail-row">
          <div class="detail-label">${escapeHtml(k)}:</div>
          <div class="detail-value">${escapeHtml(String(v))}</div>
        </div>`;
      }
    }

    if (rec.text || rec.content) {
      detailsHtml += `<div class="mt-3">
        <strong>Content:</strong>
        <pre class="terminal-pre mt-2" style="max-height:220px;overflow-y:auto;">${escapeHtml(rec.text || rec.content)}</pre>
      </div>`;
    }

    elMemModalBody.innerHTML = detailsHtml;
    elModalMemDetails.classList.remove('hidden');
  }

  // ==========================================
  // EVENT LISTENERS & MODAL MANAGEMENT
  // ==========================================
  // Navigation Tabs
  elNavTabs.forEach(tab => {
    tab.addEventListener('click', function() {
      const targetTab = this.getAttribute('data-tab');
      if (targetTab) switchTab(targetTab);
    });
  });

  // Global Header Actions
  elBtnRefresh.addEventListener('click', () => refreshCurrentTab());
  elBtnReconnectNow.addEventListener('click', () => connectWs());
  elChkAutoRefresh.addEventListener('change', (e) => {
    state.autoRefresh = e.target.checked;
    setupAutoRefresh();
  });

  // Processes Actions
  elInputProcSearch.addEventListener('input', (e) => {
    state.procSearch = e.target.value;
    renderProcessesTable();
  });

  elProcFilterPills.addEventListener('click', (e) => {
    const pill = e.target.closest('.pill');
    if (!pill) return;
    state.procFilter = pill.getAttribute('data-filter') || 'all';
    elProcFilterPills.querySelectorAll('.pill').forEach(p => p.classList.remove('active'));
    pill.classList.add('active');
    renderProcessesTable();
  });

  elBtnProcStart.addEventListener('click', () => {
    elFormProcCmd.value = '';
    elFormProcName.value = '';
    elFormProcCwd.value = '';
    elFormProcTimeout.value = '';
    elModalStartProc.classList.remove('hidden');
  });

  elBtnSubmitStartProc.addEventListener('click', () => {
    const cmd = elFormProcCmd.value.trim();
    if (!cmd) {
      alert('Please enter a command line to execute.');
      return;
    }
    const req = {
      action: 'start',
      cmd: cmd,
      name: elFormProcName.value.trim(),
      cwd: elFormProcCwd.value.trim(),
      mode: elFormProcMode.value || 'pipe'
    };
    const timeout = parseInt(elFormProcTimeout.value, 10);
    if (!isNaN(timeout) && timeout > 0) req.timeout_sec = timeout;

    sendWsRequest(req);
    elModalStartProc.classList.add('hidden');
    setTimeout(() => sendWsRequest({ action: 'list' }), 300);
  });

  elBtnProcKillAll.addEventListener('click', () => {
    if (confirm('Kill all currently running processes?')) {
      sendWsRequest({ action: 'kill_all' });
      setTimeout(() => sendWsRequest({ action: 'list' }), 300);
    }
  });

  elBtnProcClear.addEventListener('click', () => {
    sendWsRequest({ action: 'clear' });
    setTimeout(() => sendWsRequest({ action: 'list' }), 200);
  });

  elBtnProcResetIds.addEventListener('click', () => {
    if (confirm('Reset process IDs? (Only applies when no processes are active)')) {
      sendWsRequest({ action: 'reset_ids' });
      setTimeout(() => sendWsRequest({ action: 'list' }), 200);
    }
  });

  // Process Logs / Terminal Stdin
  elBtnSendStdin.addEventListener('click', () => {
    const input = elInputProcStdin.value;
    if (state.selectedProcId && input !== '') {
      sendWsRequest({ action: 'send', id: state.selectedProcId, input: input });
      elInputProcStdin.value = '';
      setTimeout(() => {
        sendWsRequest({ action: 'logs', id: state.selectedProcId, tail: 200 });
      }, 200);
    }
  });

  elInputProcStdin.addEventListener('keydown', (e) => {
    if (e.key === 'Enter') elBtnSendStdin.click();
  });

  // Task Queue Actions
  elInputTqSearch.addEventListener('input', (e) => {
    state.tqSearch = e.target.value;
    renderTaskQueueTable();
  });

  elTqFilterPills.addEventListener('click', (e) => {
    const pill = e.target.closest('.pill');
    if (!pill) return;
    state.tqFilter = pill.getAttribute('data-filter') || 'all';
    elTqFilterPills.querySelectorAll('.pill').forEach(p => p.classList.remove('active'));
    pill.classList.add('active');
    renderTaskQueueTable();
  });

  // KPI cards click to filter
  document.querySelectorAll('.kpi-header-row .kpi-card').forEach(card => {
    card.addEventListener('click', function() {
      const st = this.getAttribute('data-state');
      if (!st) return;
      state.tqFilter = st;
      elTqFilterPills.querySelectorAll('.pill').forEach(p => {
        if (p.getAttribute('data-filter') === st) p.classList.add('active');
        else p.classList.remove('active');
      });
      renderTaskQueueTable();
    });
  });

  elBtnTqPause.addEventListener('click', () => {
    sendWsRequest({ action: 'taskqueue_pause', taskqueue: '' });
  });

  elBtnTqResume.addEventListener('click', () => {
    sendWsRequest({ action: 'taskqueue_resume', taskqueue: '' });
  });

  elBtnTqPurge.addEventListener('click', () => {
    if (confirm('Purge completed, failed, and cancelled jobs from queue?')) {
      sendWsRequest({ action: 'taskqueue_purge', taskqueue: '', states: ['completed', 'failed', 'cancelled'] });
      setTimeout(() => sendWsRequest({ action: 'taskqueue_list' }), 300);
    }
  });

  elChkResourceLocks.addEventListener('change', (e) => {
    sendWsRequest({ action: 'taskqueue_resource_locks', enabled: e.target.checked });
  });

  elBtnJobModalRetry.addEventListener('click', () => {
    if (state.selectedJob) {
      sendWsRequest({ action: 'taskqueue_retry', id: state.selectedJob.key || state.selectedJob.id });
      elModalJobDetails.classList.add('hidden');
      setTimeout(() => sendWsRequest({ action: 'taskqueue_list' }), 300);
    }
  });

  elBtnJobModalCancel.addEventListener('click', () => {
    if (state.selectedJob) {
      sendWsRequest({ action: 'taskqueue_cancel', id: state.selectedJob.key || state.selectedJob.id });
      elModalJobDetails.classList.add('hidden');
      setTimeout(() => sendWsRequest({ action: 'taskqueue_list' }), 300);
    }
  });

  // Monitor Actions
  elSelectMonitorAgent.addEventListener('change', (e) => {
    const val = parseInt(e.target.value, 10);
    if (!isNaN(val) && val > 0) {
      requestAgentMonitorData(val);
    }
  });

  elBtnSaveAgentInfo.addEventListener('click', () => {
    if (state.monitorAgentId) {
      sendWsRequest({
        action: 'update_agent_info',
        id: state.monitorAgentId,
        goal: elInputAgentGoal.value.trim(),
        notes: elInputAgentNotes.value.trim()
      });
      alert('Supervisor notes and goal updated.');
    }
  });

  elMonitorEventFilters.addEventListener('click', (e) => {
    const pill = e.target.closest('.pill');
    if (!pill) return;
    state.monitorEventFilter = pill.getAttribute('data-type') || '';
    elMonitorEventFilters.querySelectorAll('.pill').forEach(p => p.classList.remove('active'));
    pill.classList.add('active');
    renderMonitorEvents();
  });

  // Failure Diagnosis Actions
  elBtnRefreshDiag.addEventListener('click', () => {
    sendWsRequest({ action: 'taskqueue_diagnose', breakdown: true });
    sendWsRequest({ action: 'taskqueue_list', state: 'failed' });
  });

  elInputDiagSearch.addEventListener('input', (e) => {
    state.diagSearch = e.target.value;
    renderDiagnosedCards();
  });

  // Memory Actions
  elInputMemSearch.addEventListener('input', (e) => {
    state.memSearch = e.target.value;
    searchMemory();
  });

  elMemLayerPills.addEventListener('click', (e) => {
    const pill = e.target.closest('.pill');
    if (!pill) return;
    state.memLayerFilter = pill.getAttribute('data-kind') || 'all';
    elMemLayerPills.querySelectorAll('.pill').forEach(p => p.classList.remove('active'));
    pill.classList.add('active');
    searchMemory();
  });

  elBtnMemPrune.addEventListener('click', () => {
    elPruneResultBox.textContent = '';
    elModalMemPrune.classList.remove('hidden');
  });

  elBtnSubmitPrune.addEventListener('click', () => {
    const days = parseInt(elInputPruneDays.value, 10) || 90;
    const dryRun = elChkPruneDryRun.checked;
    sendWsRequest({
      action: 'memory_prune',
      retention_days: days,
      dry_run: dryRun
    });
    elPruneResultBox.textContent = `Prune request (${dryRun ? 'dry-run' : 'execute'}) submitted.`;
    setTimeout(() => {
      sendWsRequest({ action: 'memory_storage_stats' });
      searchMemory();
    }, 500);
  });

  // Close modals
  document.querySelectorAll('.modal-close-btn, .modal-backdrop').forEach(el => {
    el.addEventListener('click', function() {
      this.closest('.modal').classList.add('hidden');
      if (state.selectedProcId) closeProcessLogs();
    });
  });

  window.addEventListener('keydown', (e) => {
    if (e.key === 'Escape') {
      document.querySelectorAll('.modal:not(.hidden)').forEach(m => {
        m.classList.add('hidden');
      });
      if (state.selectedProcId) closeProcessLogs();
    }
  });

  // --- Initialize ---
  setupAutoRefresh();
  connectWs();
})();
