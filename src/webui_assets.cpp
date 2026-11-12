#include "webui_assets.h"

#include "common.h"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <filesystem>

namespace pc {

namespace {

const char kIndexHtml_0[] = R"HTML_0(<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>Hancho Dashboard</title>
  <link rel="stylesheet" href="style.css">
  <link rel="icon" type="image/svg+xml" href="assets/icon.svg">
</head>
<body>
  <div id="app" class="app-container">
    <!-- Header -->
    <header class="app-header">
      <div class="header-left">
        <div class="logo">
          <svg class="logo-icon" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2">
            <rect x="2" y="3" width="20" height="14" rx="2" ry="2"></rect>
            <line x1="8" y1="21" x2="16" y2="21"></line>
            <line x1="12" y1="17" x2="12" y2="21"></line>
          </svg>
          <span class="logo-title">Hancho</span>
          <span class="logo-badge">Core</span>
        </div>
      </div>

      <nav class="nav-tabs" id="main-nav">
        <button class="nav-tab active" data-tab="processes">
          <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
            <rect x="4" y="4" width="16" height="16" rx="2" ry="2"></rect>
            <rect x="9" y="9" width="6" height="6"></rect>
            <line x1="9" y1="1" x2="9" y2="4"></line>
            <line x1="15" y1="1" x2="15" y2="4"></line>
            <line x1="9" y1="20" x2="9" y2="23"></line>
            <line x1="15" y1="20" x2="15" y2="23"></line>
            <line x1="20" y1="9" x2="23" y2="9"></line>
            <line x1="20" y1="14" x2="23" y2="14"></line>
            <line x1="1" y1="9" x2="4" y2="9"></line>
            <line x1="1" y1="14" x2="4" y2="14"></line>
          </svg>
          <span>Processes</span>
          <span class="tab-badge" id="badge-proc-count">0</span>
        </button>

        <button class="nav-tab" data-tab="taskqueue">
          <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
            <line x1="8" y1="6" x2="21" y2="6"></line>
            <line x1="8" y1="12" x2="21" y2="12"></line>
            <line x1="8" y1="18" x2="21" y2="18"></line>
            <line x1="3" y1="6" x2="3.01" y2="6"></line>
            <line x1="3" y1="12" x2="3.01" y2="12"></line>
            <line x1="3" y1="18" x2="3.01" y2="18"></line>
          </svg>
          <span>Task Queue</span>
          <span class="tab-badge" id="badge-tq-count">0</span>
        </button>

        <button class="nav-tab" data-tab="monitor">
          <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
            <polyline points="22 12 18 12 15 21 9 3 6 12 2 12"></polyline>
          </svg>
          <span>Monitor</span>
        </button>

        <button class="nav-tab" data-tab="diagnosis">
          <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
            <path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"></path>
            <line x1="12" y1="9" x2="12" y2="13"></line>
            <line x1="12" y1="17" x2="12.01" y2="17"></line>
          </svg>
          <span>Failure Diagnosis</span>
          <span class="tab-badge badge-err" id="badge-fail-count">0</span>
        </button>

        <button class="nav-tab" data-tab="memory">
          <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
            <ellipse cx="12" cy="5" rx="9" ry="3"></ellipse>
            <path d="M21 12c0 1.66-4 3-9 3s-9-1.34-9-3"></path>
            <path d="M3 5v14c0 1.66 4 3 9 3s9-1.34 9-3V5"></path>
          </svg>
          <span>Memory</span>
          <span class="tab-badge" id="badge-mem-count">0</span>
        </button>
      </nav>

      <div class="header-right">
        <label class="auto-refresh-toggle" title="Toggle automatic 3-second data refresh">
          <input type="checkbox" id="chk-auto-refresh" checked>
          <span>Auto-refresh</span>
        </label>
        <div class="connection-status" id="connection-status" title="Connection status">
          <span class="status-dot disconnected" id="status-dot"></span>
          <span class="status-text" id="status-text">Disconnected</span>
        </div>
        <button id="btn-refresh" class="btn btn-icon" title="Refresh state">
          <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
            <path d="M23 4v6h-6"></path>
            <path d="M1 20v-6h6"></path>
            <path d="M3.51 9a9 9 0 0 1 14.85-3.36L23 10M1 14l4.64 4.36A9 9 0 0 0 20.49 15"></path>
          </svg>
        </button>
      </div>
    </header>

    <!-- Disconnect / Reconnect Banner -->
    <div id="reconnect-banner" class="banner banner-warning hidden">
      <div class="banner-content">
        <svg viewBox="0 0 24 24" width="18" height="18" fill="none" stroke="currentColor" stroke-width="2">
          <circle cx="12" cy="12" r="10"></circle>
          <line x1="12" y1="8" x2="12" y2="12"></line>
          <line x1="12" y1="16" x2="12.01" y2="16"></line>
        </svg>
        <span id="reconnect-msg">Connection lost. Reconnecting to Hancho core... Background jobs are not affected.</span>
      </div>
      <button id="btn-reconnect-now" class="btn btn-sm">Reconnect Now</button>
    </div>

    <!-- PANE 1: PROCESSES -->
    <section id="pane-processes" class="tab-pane active">
      <div class="pane-toolbar">
        <div class="action-buttons">
          <button id="btn-proc-start" class="btn btn-primary">
            <svg viewBox="0 0 24 24" width="15" height="15" fill="none" stroke="currentColor" stroke-width="2">
              <polygon points="5 3 19 12 5 21 5 3"></polygon>
            </svg>
            <span>Start Process</span>
          </button>
          <button id="btn-proc-kill-all" class="btn btn-danger" title="Stop all currently running processes">
            <span>Kill All</span>
          </button>
          <button id="btn-proc-clear" class="btn" title="Clear finished processes from the list">
            <span>Clear Finished</span>
          </button>
          <button id="btn-proc-reset-ids" class="btn" title="Reset process IDs counter">
            <span>Reset IDs</span>
          </button>
        </div>

        <div class="search-box">
          <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
            <circle cx="11" cy="11" r="8"></circle>
            <line x1="21" y1="21" x2="16.65" y2="16.65"></line>
          </svg>
          <input type="text" id="input-proc-search" placeholder="Filter processes by ID, PID, name, command...">
        </div>

        <div class="filter-pills" id="proc-filter-pills">
          <button class="pill active" data-filter="all">All</button>
          <button class="pill" data-filter="running">Running</button>
          <button class="pill" data-filter="completed">Finished</button>
          <button class="pill" data-filter="failed">Failed</button>
        </div>
      </div>

      <div class="table-container">
        <table class="data-table">
          <thead>
            <tr>
              <th style="width: 70px;">ID</th>
              <th style="width: 110px;">State</th>
              <th style="width: 80px;">PID</th>
              <th>Name / Command</th>
              <th style="width: 90px;">CPU %</th>
              <th style="width: 100px;">Memory</th>
              <th style="width: 80px;">Exit</th>
              <th style="width: 140px;">Started</th>
              <th style="width: 220px; text-align: center;">Actions</th>
            </tr>
          </thead>
          <tbody id="proc-tbody">
            <tr class="empty-row"><td colspan="9">Loading processes...</td></tr>
          </tbody>
        </table>
      </div>
    </section>

    <!-- PANE 2: TASK QUEUE -->
    <section id="pane-taskqueue" class="tab-pane">
      <div class="kpi-header-row">
        <div class="kpi-card" data-state="all">
          <span class="kpi-value" id="kpi-tq-total">0</span>
          <span class="kpi-label">Total Jobs</span>
        </div>
        <div class="kpi-card kpi-running" data-state="running">
          <span class="kpi-value" id="kpi-tq-running">0</span>
          <span class="kpi-label">Running</span>
        </div>
        <div class="kpi-card kpi-pending" data-state="waiting">
          <span class="kpi-value" id="kpi-tq-waiting">0</span>
          <span class="kpi-label">Waiting</span>
        </div>
        <div class="kpi-card kpi-pending" data-state="pending">
          <span class="kpi-value" id="kpi-tq-pending">0</span>
          <span class="kpi-label">Pending</span>
        </div>
        <div class="kpi-card kpi-blocked" data-state="blocked">
          <span class="kpi-value" id="kpi-tq-blocked">0</span>
          <span class="kpi-label">Blocked</span>
        </div>
        <div class="kpi-card kpi-completed" data-state="succeeded">
          <span class="kpi-value" id="kpi-tq-succeeded">0</span>
          <span class="kpi-label">Succeeded</span>
        </div>
        <div class="kpi-card kpi-failed" data-state="failed">
          <span class="kpi-value" id="kpi-tq-failed">0</span>
          <span class="kpi-label">Failed</span>
        </div>
        <div class="kpi-card kpi-cancelled" data-state="cancelled">
          <span class="kpi-value" id="kpi-tq-cancelled">0</span>
          <span class="kpi-label">Cancelled</span>
        </div>
      </div>

      <div class="pane-toolbar">
        <div class="action-buttons">
          <button id="btn-tq-pause" class="btn" title="Pause queue execution">
            <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
              <rect x="6" y="4" width="4" height="16"></rect>
              <rect x="14" y="4" width="4" height="16"></rect>
            </svg>
            <span>Pause Queue</span>
          </button>
          <button id="btn-tq-resume" class="btn" title="Resume queue execution">
            <svg viewBox="0 0 24 24" width="14" height="14" fill="none" stroke="currentColor" stroke-width="2">
              <polygon points="5 3 19 12 5 21 5 3"></polygon>
            </svg>
            <span>Resume Queue</span>
          </button>
          <button id="btn-tq-purge" class="btn btn-danger" title="Purge finished, failed, and cancelled jobs">
            <span>Purge Finished</span>
          </button>
          <label class="toggle-checkbox" title="Prevent conflicting jobs from running simultaneously">
            <input type="checkbox" id="chk-resource-locks" checked>
            <span>Resource Locks</span>
          </label>
        </div>

        <div class="search-box">
          <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
            <circle cx="11" cy="11" r="8"></circle>
            <line x1="21" y1="21" x2="16.65" y2="16.65"></line>
          </svg>
          <input type="text" id="input-tq-search" placeholder="Filter jobs by key, name, queue, command...">
        </div>

        <div class="filter-pills" id="tq-filter-pills">
          <button class="pill active" data-filter="all">All</button>
          <button class="pill" data-filter="running">Running</button>
          <button class="pill" data-filter="pending">Pending</button>
          <button class="pill" data-filter="succeeded">Succeeded</button>
          <button class="pill" data-filter="failed">Failed</button>
          <button class="pill" data-filter="cancelled">Cancelled</button>
        </div>
      </div>

      <div class="table-container">
        <table class="data-table">
          <thead>
            <tr>
              <th style="width: 110px;">State</th>
              <th style="width: 170px;">Job Key / ID</th>
              <th style="width: 180px;">Name</th>
              <th style="width: 110px;">Queue</th>
)HTML_0";
const char kIndexHtml_1[] = R"HTML_1(              <th>Command</th>
              <th style="width: 80px;">Exit</th>
              <th style="width: 90px;">Duration</th>
              <th style="width: 140px;">Updated</th>
              <th style="width: 180px; text-align: center;">Actions</th>
            </tr>
          </thead>
          <tbody id="tq-tbody">
            <tr class="empty-row"><td colspan="9">Loading task queue...</td></tr>
          </tbody>
        </table>
      </div>
    </section>

    <!-- PANE 3: MONITOR -->
    <section id="pane-monitor" class="tab-pane">
      <div class="monitor-container">
        <!-- Top Agent Selector & Goal Editor -->
        <div class="monitor-top-bar card">
          <div class="monitor-agent-select">
            <label for="select-monitor-agent">Active Agent / Process:</label>
            <select id="select-monitor-agent" class="form-select">
              <option value="">(Select a process to monitor)</option>
            </select>
            <span class="badge" id="monitor-agent-status">-</span>
          </div>

          <div class="monitor-goal-box">
            <div class="goal-input-col">
              <label for="input-agent-goal">Goal:</label>
              <input type="text" id="input-agent-goal" placeholder="Autonomous agent goal...">
            </div>
            <div class="goal-input-col">
              <label for="input-agent-notes">Supervisor Notes:</label>
              <input type="text" id="input-agent-notes" placeholder="Notes for next step...">
            </div>
            <button id="btn-save-agent-info" class="btn btn-sm btn-primary">Save Info</button>
          </div>
        </div>

        <div class="monitor-grid">
          <!-- Left Column: Event Stream -->
          <div class="monitor-card card">
            <div class="card-header">
              <div class="card-title">
                <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
                  <polyline points="22 12 18 12 15 21 9 3 6 12 2 12"></polyline>
                </svg>
                <span>Agent Event Stream (hancho.event/1)</span>
              </div>
              <div class="filter-pills" id="monitor-event-filters">
                <button class="pill active" data-type="">All</button>
                <button class="pill" data-type="tool_call">Tools</button>
                <button class="pill" data-type="thought">Thinking</button>
                <button class="pill" data-type="progress">Progress</button>
                <button class="pill" data-type="error">Errors</button>
              </div>
            </div>
            <div class="event-stream-container" id="monitor-events-list">
              <div class="empty-message">Select a running agent to view events.</div>
            </div>
          </div>

          <!-- Right Column: Usage Analytics & Graphs -->
          <div class="monitor-card card">
            <div class="card-header">
              <div class="card-title">
                <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
                  <line x1="18" y1="20" x2="18" y2="10"></line>
                  <line x1="12" y1="20" x2="12" y2="4"></line>
                  <line x1="6" y1="20" x2="6" y2="14"></line>
                </svg>
                <span>Usage & Cost Analytics</span>
              </div>
              <span class="text-muted" id="usage-timeframe">Last 24 Hours</span>
            </div>

            <div class="usage-kpis">
              <div class="usage-stat">
                <span class="stat-value" id="stat-prompt-tokens">0</span>
                <span class="stat-label">Prompt Tokens</span>
              </div>
              <div class="usage-stat">
                <span class="stat-value" id="stat-output-tokens">0</span>
                <span class="stat-label">Output Tokens</span>
              </div>
              <div class="usage-stat">
                <span class="stat-value" id="stat-total-tokens">0</span>
                <span class="stat-label">Total Tokens</span>
              </div>
              <div class="usage-stat highlight">
                <span class="stat-value" id="stat-cost-usd">$0.00</span>
                <span class="stat-label">Estimated Cost</span>
              </div>
            </div>

            <!-- SVG Responsive Usage Trend Graph -->
            <div class="chart-wrapper">
              <svg id="usage-chart" class="usage-svg-chart" viewBox="0 0 600 180" preserveAspectRatio="none">
                <rect width="600" height="180" fill="transparent"/>
                <line x1="40" y1="150" x2="580" y2="150" stroke="#2e2f42" stroke-width="1"/>
                <line x1="40" y1="85" x2="580" y2="85" stroke="#2e2f42" stroke-dasharray="4" stroke-width="1"/>
                <line x1="40" y1="20" x2="580" y2="20" stroke="#2e2f42" stroke-dasharray="4" stroke-width="1"/>
                <g id="chart-bars"></g>
                <text x="30" y="153" fill="#6b7280" font-size="10" text-anchor="end">0</text>
                <text x="30" y="88" fill="#6b7280" font-size="10" text-anchor="end" id="chart-y-mid">-</text>
                <text x="30" y="23" fill="#6b7280" font-size="10" text-anchor="end" id="chart-y-max">-</text>
              </svg>
            </div>

            <!-- Daily Rollup Table -->
            <div class="daily-table-wrap">
              <table class="mini-table">
                <thead>
                  <tr>
                    <th>Date / Time</th>
                    <th>Model</th>
                    <th>Tokens</th>
                    <th>Cost</th>
                  </tr>
                </thead>
                <tbody id="usage-daily-tbody">
                  <tr><td colspan="4" class="text-muted">No usage data recorded.</td></tr>
                </tbody>
              </table>
            </div>
          </div>
        </div>
      </div>
    </section>

    <!-- PANE 4: FAILURE DIAGNOSIS -->
    <section id="pane-diagnosis" class="tab-pane">
      <div class="diagnosis-container">
        <!-- Breakdown Card -->
        <div class="card diagnosis-breakdown-card">
          <div class="card-header">
            <div class="card-title">
              <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
                <path d="M10.29 3.86L1.82 18a2 2 0 0 0 1.71 3h16.94a2 2 0 0 0 1.71-3L13.71 3.86a2 2 0 0 0-3.42 0z"></path>
                <line x1="12" y1="9" x2="12" y2="13"></line>
                <line x1="12" y1="17" x2="12.01" y2="17"></line>
              </svg>
              <span>Queue Failure Classification (Why Did It Fail?)</span>
            </div>
            <button id="btn-refresh-diag" class="btn btn-sm">Refresh Diagnosis</button>
          </div>

          <div class="breakdown-badges-row" id="diag-classes-container">
            <div class="empty-message">No failure statistics available.</div>
          </div>
        </div>

        <!-- Diagnosed Failures Feed -->
        <div class="diag-list-header">
          <h3>Failed Jobs & Process Diagnoses</h3>
          <div class="search-box">
            <input type="text" id="input-diag-search" placeholder="Filter by job ID, error class, evidence...">
          </div>
        </div>

        <div class="diag-cards-list" id="diag-cards-list">
          <div class="empty-message">No failures currently detected. Everything is running smoothly!</div>
        </div>
      </div>
    </section>

    <!-- PANE 5: MEMORY -->
    <section id="pane-memory" class="tab-pane">
      <div class="memory-container">
        <!-- Storage Stats Header -->
        <div class="card memory-stats-card">
          <div class="card-header">
            <div class="card-title">
              <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
                <ellipse cx="12" cy="5" rx="9" ry="3"></ellipse>
                <path d="M21 12c0 1.66-4 3-9 3s-9-1.34-9-3"></path>
                <path d="M3 5v14c0 1.66 4 3 9 3s9-1.34 9-3V5"></path>
              </svg>
              <span>Hierarchical Memory Storage (L1 Runs / L2 Notes / L3 Knowledge)</span>
            </div>
            <div class="memory-actions">
              <button id="btn-mem-prune" class="btn btn-sm btn-danger" title="Clean up old L1 execution run records">
                Prune L1 Records
              </button>
            </div>
          </div>

          <div class="memory-stats-grid">
            <div class="mem-stat-item">
              <span class="stat-label">Total Storage</span>
              <span class="stat-value" id="mem-stat-total">0 KB</span>
            </div>
            <div class="mem-stat-item">
              <span class="stat-label">L1 Runs</span>
              <span class="stat-value" id="mem-stat-l1">0 KB</span>
            </div>
            <div class="mem-stat-item">
              <span class="stat-label">L2 Notes</span>
              <span class="stat-value" id="mem-stat-l2">0 KB</span>
            </div>
            <div class="mem-stat-item">
              <span class="stat-label">L3 Knowledge</span>
              <span class="stat-value" id="mem-stat-l3">0 KB</span>
            </div>
            <div class="mem-stat-item">
              <span class="stat-label">Quota Status</span>
              <span class="stat-value" id="mem-stat-warning">Normal</span>
            </div>
          </div>

          <div class="quota-bar-wrapper">
            <div class="quota-bar-fill" id="quota-bar-fill" style="width: 0%;"></div>
          </div>
        </div>

        <!-- Search & Layer Filters -->
        <div class="pane-toolbar">
          <div class="search-box">
            <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
              <circle cx="11" cy="11" r="8"></circle>
              <line x1="21" y1="21" x2="16.65" y2="16.65"></line>
            </svg>
            <input type="text" id="input-mem-search" placeholder="Search memory records by query, keyword, job, outcome...">
          </div>

          <div class="filter-pills" id="mem-layer-pills">
            <button class="pill active" data-kind="all">All Layers</button>
            <button class="pill" data-kind="l1">L1 Runs</button>
            <button class="pill" data-kind="l2">L2 Notes</button>
            <button class="pill" data-kind="l3">L3 Knowledge</button>
          </div>
        </div>

        <!-- Memory Records Table -->
        <div class="table-container">
          <table class="data-table">
            <thead>
              <tr>
                <th style="width: 90px;">Layer</th>
                <th style="width: 170px;">Record ID</th>
                <th style="width: 150px;">Job / Queue</th>
                <th style="width: 100px;">Outcome</th>
                <th>Summary / Key Insights</th>
                <th style="width: 140px;">Created</th>
                <th style="width: 190px; text-align: center;">Actions</th>
              </tr>
            </thead>
            <tbody id="mem-tbody">
              <tr class="empty-row"><td colspan="7">Loading memory records...</td></tr>
            </tbody>
          </table>
        </div>
      </div>
    </section>

    <!-- MODAL: START PROCESS -->
    <div id="modal-start-proc" class="modal hidden">
      <div class="modal-backdrop"></div>
      <div class="modal-dialog">
        <div class="modal-header">
          <h3>Start New Process</h3>
          <button class="btn-close modal-close-btn">&times;</button>
        </div>
        <div class="modal-body">
          <div class="form-group">
            <label for="form-proc-cmd">Command Line: *</label>
            <input type="text" id="form-proc-cmd" class="form-input" placeholder="e.g. bash -c 'echo hello; sleep 5'">
          </div>
          <div class="form-group">
)HTML_1";
const char kIndexHtml_2[] = R"HTML_2(            <label for="form-proc-name">Process / Job Name:</label>
            <input type="text" id="form-proc-name" class="form-input" placeholder="e.g. my-worker">
          </div>
          <div class="form-group">
            <label for="form-proc-cwd">Working Directory:</label>
            <input type="text" id="form-proc-cwd" class="form-input" placeholder="(leave empty for core current working dir)">
          </div>
          <div class="form-row">
            <div class="form-group col">
              <label for="form-proc-mode">Execution Mode:</label>
              <select id="form-proc-mode" class="form-select">
                <option value="pipe" selected>Pipe (standard I/O)</option>
                <option value="pty">PTY (pseudo-terminal)</option>
              </select>
            </div>
            <div class="form-group col">
              <label for="form-proc-timeout">Timeout (seconds):</label>
              <input type="number" id="form-proc-timeout" class="form-input" placeholder="0 (no timeout)">
            </div>
          </div>
        </div>
        <div class="modal-footer">
          <button class="btn modal-close-btn">Cancel</button>
          <button id="btn-submit-start-proc" class="btn btn-primary">Start Process</button>
        </div>
      </div>
    </div>

    <!-- MODAL: PROCESS LOGS / TERMINAL -->
    <div id="modal-proc-logs" class="modal hidden">
      <div class="modal-backdrop"></div>
      <div class="modal-dialog modal-lg">
        <div class="modal-header">
          <h3 id="logs-modal-title">Process Terminal & Logs</h3>
          <button class="btn-close modal-close-btn">&times;</button>
        </div>
        <div class="modal-body">
          <div class="terminal-container">
            <pre id="terminal-output" class="terminal-pre">Loading output...</pre>
          </div>
          <div class="stdin-input-row">
            <input type="text" id="input-proc-stdin" class="form-input" placeholder="Send input string to process standard input...">
            <button id="btn-send-stdin" class="btn btn-primary">Send</button>
          </div>
        </div>
        <div class="modal-footer">
          <span class="text-muted" id="logs-meta-info"></span>
          <div style="flex: 1;"></div>
          <button class="btn modal-close-btn">Close</button>
        </div>
      </div>
    </div>

    <!-- MODAL: JOB DETAILS -->
    <div id="modal-job-details" class="modal hidden">
      <div class="modal-backdrop"></div>
      <div class="modal-dialog">
        <div class="modal-header">
          <h3 id="job-modal-title">Job Details</h3>
          <button class="btn-close modal-close-btn">&times;</button>
        </div>
        <div class="modal-body" id="job-modal-body">
          <!-- Filled dynamically -->
        </div>
        <div class="modal-footer" id="job-modal-footer">
          <button id="btn-job-modal-retry" class="btn btn-primary">Retry</button>
          <button id="btn-job-modal-cancel" class="btn btn-danger">Cancel</button>
          <button class="btn modal-close-btn">Close</button>
        </div>
      </div>
    </div>

    <!-- MODAL: MEMORY RECORD DETAILS -->
    <div id="modal-mem-details" class="modal hidden">
      <div class="modal-backdrop"></div>
      <div class="modal-dialog modal-lg">
        <div class="modal-header">
          <h3 id="mem-modal-title">Memory Record</h3>
          <button class="btn-close modal-close-btn">&times;</button>
        </div>
        <div class="modal-body" id="mem-modal-body">
          <!-- Filled dynamically -->
        </div>
        <div class="modal-footer" id="mem-modal-footer">
          <button class="btn modal-close-btn">Close</button>
        </div>
      </div>
    </div>

    <!-- MODAL: PRUNE MEMORY -->
    <div id="modal-mem-prune" class="modal hidden">
      <div class="modal-backdrop"></div>
      <div class="modal-dialog">
        <div class="modal-header">
          <h3>Prune L1 Run Records</h3>
          <button class="btn-close modal-close-btn">&times;</button>
        </div>
        <div class="modal-body">
          <p class="text-muted mb-3">
            Delete older L1 raw execution records to free disk space. L2 Notes and L3 Knowledge will not be modified.
          </p>
          <div class="form-group">
            <label for="input-prune-days">Retention (days):</label>
            <input type="number" id="input-prune-days" class="form-input" value="90" min="1">
          </div>
          <label class="toggle-checkbox">
            <input type="checkbox" id="chk-prune-dry-run" checked>
            <span>Dry run only (calculate space without deleting)</span>
          </label>
          <div id="prune-result-box" class="mt-3 text-muted"></div>
        </div>
        <div class="modal-footer">
          <button class="btn modal-close-btn">Cancel</button>
          <button id="btn-submit-prune" class="btn btn-danger">Execute Prune</button>
        </div>
      </div>
    </div>

  </div>
  <script src="app.js"></script>
</body>
</html>
)HTML_2";
static const std::string& GetIndexHtml() {
    static const std::string s = std::string(kIndexHtml_0) + kIndexHtml_1 + kIndexHtml_2;
    return s;
}



const char kStyleCss_0[] = R"CSS_0(/* Hancho Dashboard Modern Dark Theme - 5 Panes */
:root {
  --bg-primary: #121217;
  --bg-secondary: #1a1a23;
  --bg-card: #22222e;
  --bg-hover: #2a2a3a;
  --border-color: #2e2f42;
  --text-primary: #f0f0f6;
  --text-secondary: #9ca3af;
  --text-muted: #6b7280;
  --accent: #38bdf8;
  --accent-hover: #0284c7;
  --success: #4ade80;
  --success-bg: rgba(74, 222, 128, 0.12);
  --warning: #fbbf24;
  --warning-bg: rgba(251, 191, 36, 0.12);
  --error: #f87171;
  --error-bg: rgba(248, 113, 113, 0.12);
  --running: #60a5fa;
  --running-bg: rgba(96, 165, 250, 0.12);
  --pending: #fbbf24;
  --pending-bg: rgba(251, 191, 36, 0.12);
  --blocked: #f97316;
  --blocked-bg: rgba(249, 115, 22, 0.12);
  --cancelled: #9ca3af;
  --cancelled-bg: rgba(156, 163, 175, 0.12);
  --l1-color: #818cf8;
  --l1-bg: rgba(129, 140, 248, 0.12);
  --l2-color: #fbbf24;
  --l2-bg: rgba(251, 191, 36, 0.12);
  --l3-color: #34d399;
  --l3-bg: rgba(52, 211, 153, 0.12);
  --radius-sm: 4px;
  --radius-md: 6px;
  --radius-lg: 10px;
  --font-mono: ui-monospace, SFMono-Regular, Menlo, Monaco, Consolas, "Liberation Mono", "Courier New", monospace;
  --font-sans: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, Helvetica, Arial, sans-serif;
}

* {
  box-sizing: border-box;
  margin: 0;
  padding: 0;
}

body {
  font-family: var(--font-sans);
  background-color: var(--bg-primary);
  color: var(--text-primary);
  line-height: 1.5;
  -webkit-font-smoothing: antialiased;
}

.app-container {
  display: flex;
  flex-direction: column;
  height: 100vh;
  overflow: hidden;
}

/* Header */
.app-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 8px 18px;
  background-color: var(--bg-secondary);
  border-bottom: 1px solid var(--border-color);
  flex-shrink: 0;
  gap: 16px;
}

.header-left, .header-right {
  display: flex;
  align-items: center;
  gap: 12px;
}

.logo {
  display: flex;
  align-items: center;
  gap: 8px;
}

.logo-icon {
  width: 22px;
  height: 22px;
  color: var(--accent);
}

.logo-title {
  font-size: 1.15rem;
  font-weight: 700;
  letter-spacing: -0.02em;
  color: #fff;
}

.logo-badge {
  font-size: 0.7rem;
  font-weight: 600;
  background-color: rgba(56, 189, 248, 0.15);
  color: var(--accent);
  padding: 2px 6px;
  border-radius: var(--radius-sm);
  text-transform: uppercase;
}

/* Top Navigation Tabs */
.nav-tabs {
  display: flex;
  align-items: center;
  gap: 4px;
}

.nav-tab {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 8px 14px;
  background: transparent;
  border: 1px solid transparent;
  border-radius: var(--radius-md);
  color: var(--text-secondary);
  font-size: 0.88rem;
  font-weight: 500;
  cursor: pointer;
  transition: all 0.15s ease;
}

.nav-tab:hover {
  background-color: var(--bg-card);
  color: var(--text-primary);
}

.nav-tab.active {
  background-color: var(--bg-card);
  border-color: var(--border-color);
  color: var(--accent);
  font-weight: 600;
}

.tab-badge {
  font-size: 0.72rem;
  font-weight: 600;
  padding: 1px 6px;
  border-radius: 10px;
  background-color: var(--border-color);
  color: var(--text-secondary);
}

.nav-tab.active .tab-badge {
  background-color: rgba(56, 189, 248, 0.2);
  color: var(--accent);
}

.tab-badge.badge-err {
  background-color: var(--error-bg);
  color: var(--error);
}

/* Connection Status */
.connection-status {
  display: flex;
  align-items: center;
  gap: 6px;
  padding: 4px 10px;
  background-color: var(--bg-card);
  border: 1px solid var(--border-color);
  border-radius: var(--radius-md);
  font-size: 0.78rem;
}

.status-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
}

.status-dot.connected {
  background-color: var(--success);
  box-shadow: 0 0 8px rgba(74, 222, 128, 0.5);
}

.status-dot.connecting {
  background-color: var(--warning);
  animation: pulse 1.2s infinite;
}

.status-dot.disconnected {
  background-color: var(--error);
}

@keyframes pulse {
  0%, 100% { opacity: 1; transform: scale(1); }
  50% { opacity: 0.4; transform: scale(0.85); }
}

/* Banner */
.banner {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 8px 16px;
  font-size: 0.85rem;
  flex-shrink: 0;
}

.banner-warning {
  background-color: rgba(251, 191, 36, 0.15);
  border-bottom: 1px solid rgba(251, 191, 36, 0.3);
  color: #fde68a;
}

.banner-content {
  display: flex;
  align-items: center;
  gap: 8px;
}

/* Tab Panes */
.tab-pane {
  display: none;
  flex: 1;
  flex-direction: column;
  overflow: hidden;
  padding: 14px 18px;
  gap: 12px;
}

.tab-pane.active {
  display: flex;
}

/* KPI Cards */
.kpi-header-row {
  display: flex;
  gap: 10px;
  flex-wrap: wrap;
}

.kpi-card {
  display: flex;
  flex-direction: column;
  align-items: center;
  padding: 6px 14px;
  background-color: var(--bg-secondary);
  border: 1px solid var(--border-color);
  border-radius: var(--radius-md);
  min-width: 90px;
  cursor: pointer;
  transition: all 0.15s ease;
}

.kpi-card:hover {
  background-color: var(--bg-card);
  border-color: var(--accent);
}

.kpi-value {
  font-size: 1.2rem;
  font-weight: 700;
  line-height: 1.2;
}

.kpi-label {
  font-size: 0.7rem;
  color: var(--text-secondary);
  text-transform: uppercase;
  font-weight: 600;
}

.kpi-running .kpi-value { color: var(--running); }
.kpi-pending .kpi-value { color: var(--pending); }
.kpi-blocked .kpi-value { color: var(--blocked); }
.kpi-completed .kpi-value { color: var(--success); }
.kpi-failed .kpi-value { color: var(--error); }
.kpi-cancelled .kpi-value { color: var(--cancelled); }

/* Toolbars */
.pane-toolbar {
  display: flex;
  align-items: center;
  gap: 12px;
  flex-wrap: wrap;
  flex-shrink: 0;
}

.action-buttons {
  display: flex;
  align-items: center;
  gap: 8px;
}

.search-box {
  display: flex;
  align-items: center;
  gap: 8px;
  background-color: var(--bg-secondary);
  border: 1px solid var(--border-color);
  border-radius: var(--radius-md);
  padding: 4px 10px;
  min-width: 260px;
  flex: 1;
  max-width: 450px;
}

.search-box input {
  background: transparent;
  border: none;
  outline: none;
  color: var(--text-primary);
  font-size: 0.85rem;
  width: 100%;
}

.search-box input::placeholder {
  color: var(--text-muted);
}

/* Filter Pills */
.filter-pills {
  display: flex;
  gap: 4px;
}

.pill {
  background-color: var(--bg-secondary);
  border: 1px solid var(--border-color);
  color: var(--text-secondary);
  padding: 4px 10px;
  border-radius: 12px;
  font-size: 0.78rem;
  font-weight: 500;
  cursor: pointer;
  transition: all 0.15s ease;
}

.pill:hover {
  background-color: var(--bg-card);
  color: var(--text-primary);
}

.pill.active {
  background-color: var(--accent);
  border-color: var(--accent);
  color: #000;
  font-weight: 600;
}

/* Buttons */
.btn {
  display: inline-flex;
  align-items: center;
  justify-content: center;
  gap: 6px;
  padding: 6px 12px;
  font-size: 0.82rem;
  font-weight: 500;
  border-radius: var(--radius-md);
  border: 1px solid var(--border-color);
  background-color: var(--bg-card);
  color: var(--text-primary);
  cursor: pointer;
  transition: all 0.15s ease;
}

.btn:hover {
  background-color: var(--bg-hover);
  border-color: var(--accent);
}

.btn-primary {
  background-color: var(--accent);
  border-color: var(--accent);
  color: #0b1120;
  font-weight: 600;
}

.btn-primary:hover {
  background-color: #7dd3fc;
  border-color: #7dd3fc;
}

.btn-danger {
  background-color: rgba(248, 113, 113, 0.15);
  border-color: rgba(248, 113, 113, 0.3);
  color: var(--error);
}

.btn-danger:hover {
  background-color: var(--error);
  color: #fff;
}

.btn-sm {
  padding: 3px 8px;
  font-size: 0.75rem;
}

.btn-icon {
  padding: 6px;
}

.btn-close {
  background: none;
  border: none;
  color: var(--text-muted);
  font-size: 1.3rem;
  cursor: pointer;
  line-height: 1;
}

.btn-close:hover {
  color: #fff;
}

/* Checkbox Toggle */
.toggle-checkbox, .auto-refresh-toggle {
  display: flex;
  align-items: center;
  gap: 6px;
  font-size: 0.8rem;
  color: var(--text-secondary);
  cursor: pointer;
}

/* Tables */
.table-container {
  flex: 1;
  overflow: auto;
  background-color: var(--bg-secondary);
  border: 1px solid var(--border-color);
  border-radius: var(--radius-lg);
}

.data-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 0.82rem;
  text-align: left;
}

.data-table thead {
  position: sticky;
  top: 0;
  background-color: #171720;
  z-index: 10;
}

.data-table th {
  padding: 10px 14px;
  font-weight: 600;
  color: var(--text-secondary);
  border-bottom: 1px solid var(--border-color);
  white-space: nowrap;
}

.data-table td {
  padding: 8px 14px;
  border-bottom: 1px solid rgba(46, 47, 66, 0.6);
  white-space: nowrap;
  vertical-align: middle;
}

.data-table tbody tr:hover {
  background-color: rgba(255, 255, 255, 0.02);
}

.empty-row td {
  text-align: center;
  padding: 40px;
  color: var(--text-muted);
}

/* Badges */
.badge {
  display: inline-block;
  padding: 2px 7px;
  border-radius: var(--radius-sm);
  font-size: 0.72rem;
  font-weight: 600;
  text-transform: uppercase;
}

.badge-running { background-color: var(--running-bg); color: var(--running); }
.badge-pending { background-color: var(--pending-bg); color: var(--pending); }
.badge-blocked { background-color: var(--blocked-bg); color: var(--blocked); }
.badge-completed, .badge-succeeded { background-color: var(--success-bg); color: var(--success); }
.badge-failed { background-color: var(--error-bg); color: var(--error); }
.badge-cancelled { background-color: var(--cancelled-bg); color: var(--cancelled); }
.badge-l1 { background-color: var(--l1-bg); color: var(--l1-color); }
.badge-l2 { background-color: var(--l2-bg); color: var(--l2-color); }
.badge-l3 { background-color: var(--l3-bg); color: var(--l3-color); }

/* Monospace text */
.font-mono {
  font-family: var(--font-mono);
}

/* Cards */
.card {
  background-color: var(--bg-secondary);
  border: 1px solid var(--border-color);
  border-radius: var(--radius-lg);
  padding: 14px;
}

.card-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 12px;
}

.card-title {
  display: flex;
  align-items: center;
  gap: 8px;
  font-size: 0.95rem;
  font-weight: 600;
  color: #fff;
}

/* Monitor Styles */
.monitor-container {
  display: flex;
  flex-direction: column;
  gap: 14px;
  flex: 1;
  overflow: hidden;
}

.monitor-top-bar {
  display: flex;
  align-items: center;
  justify-content: space-between;
  gap: 16px;
  flex-shrink: 0;
}

.monitor-agent-select {
  display: flex;
  align-items: center;
  gap: 10px;
  font-size: 0.85rem;
}

.form-select, .form-input {
  background-color: var(--bg-card);
  border: 1px solid var(--border-color);
  border-radius: var(--radius-md);
  color: var(--text-primary);
  padding: 6px 10px;
  font-size: 0.85rem;
  outline: none;
}

.form-select:focus, .form-input:focus {
  border-color: var(--accent);
}

.monitor-goal-box {
  display: flex;
  align-items: center;
  gap: 10px;
  flex: 1;
  max-width: 800px;
}

.goal-input-col {
  display: flex;
  align-items: center;
  gap: 6px;
  flex: 1;
  font-size: 0.8rem;
}

.goal-input-col input {
  flex: 1;
  background-color: var(--bg-card);
  border: 1px solid var(--border-color);
  border-radius: var(--radius-md);
  color: var(--text-primary);
  padding: 5px 8px;
  font-size: 0.8rem;
}

.monitor-grid {
  display: grid;
  grid-template-columns: 1fr 1fr;
  gap: 14px;
  flex: 1;
  overflow: hidden;
}

.monitor-card {
  display: flex;
  flex-direction: column;
  overflow: hidden;
}

.event-stream-container {
  flex: 1;
  overflow-y: auto;
  display: flex;
  flex-direction: column;
  gap: 8px;
  padding: 4px;
}

.event-card {
  background-color: var(--bg-card);
  border: 1px solid var(--border-color);
  border-radius: var(--radius-md);
  padding: 10px;
  font-size: 0.82rem;
}

.event-card-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-bottom: 6px;
}

.event-card-body {
)CSS_0";
const char kStyleCss_1[] = R"CSS_1(  font-family: var(--font-mono);
  font-size: 0.78rem;
  color: var(--text-secondary);
  background-color: #121217;
  padding: 6px 10px;
  border-radius: var(--radius-sm);
  white-space: pre-wrap;
  word-break: break-all;
  max-height: 120px;
  overflow-y: auto;
}

/* Usage KPIs */
.usage-kpis {
  display: flex;
  gap: 10px;
  margin-bottom: 12px;
}

.usage-stat {
  flex: 1;
  display: flex;
  flex-direction: column;
  align-items: center;
  background-color: var(--bg-card);
  border: 1px solid var(--border-color);
  border-radius: var(--radius-md);
  padding: 8px;
}

.usage-stat.highlight .stat-value {
  color: var(--success);
}

.stat-value {
  font-size: 1.15rem;
  font-weight: 700;
}

.stat-label {
  font-size: 0.7rem;
  color: var(--text-secondary);
  text-transform: uppercase;
}

/* SVG Chart */
.chart-wrapper {
  background-color: var(--bg-card);
  border: 1px solid var(--border-color);
  border-radius: var(--radius-md);
  padding: 8px;
  margin-bottom: 12px;
}

.usage-svg-chart {
  width: 100%;
  height: 140px;
}

.daily-table-wrap {
  flex: 1;
  overflow-y: auto;
}

.mini-table {
  width: 100%;
  border-collapse: collapse;
  font-size: 0.78rem;
}

.mini-table th {
  padding: 6px 10px;
  border-bottom: 1px solid var(--border-color);
  color: var(--text-muted);
  text-align: left;
}

.mini-table td {
  padding: 6px 10px;
  border-bottom: 1px solid rgba(46, 47, 66, 0.4);
}

/* Failure Diagnosis Styles */
.diagnosis-container {
  display: flex;
  flex-direction: column;
  gap: 14px;
  flex: 1;
  overflow-y: auto;
}

.diagnosis-breakdown-card {
  flex-shrink: 0;
}

.breakdown-badges-row {
  display: flex;
  gap: 10px;
  flex-wrap: wrap;
}

.diag-class-badge {
  display: flex;
  align-items: center;
  gap: 8px;
  padding: 6px 12px;
  border-radius: var(--radius-md);
  background-color: var(--bg-card);
  border: 1px solid var(--border-color);
  font-size: 0.82rem;
}

.diag-class-count {
  font-weight: 700;
  color: var(--error);
  font-size: 1.05rem;
}

.diag-list-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  margin-top: 4px;
}

.diag-list-header h3 {
  font-size: 0.95rem;
  color: #fff;
}

.diag-cards-list {
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.diag-card {
  background-color: var(--bg-secondary);
  border: 1px solid var(--border-color);
  border-left: 4px solid var(--error);
  border-radius: var(--radius-md);
  padding: 12px 16px;
  display: flex;
  flex-direction: column;
  gap: 8px;
}

.diag-card-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
}

.diag-card-title {
  display: flex;
  align-items: center;
  gap: 10px;
  font-weight: 600;
  font-size: 0.9rem;
}

.diag-card-evidence {
  font-family: var(--font-mono);
  font-size: 0.78rem;
  background-color: #121217;
  padding: 8px 12px;
  border-radius: var(--radius-sm);
  color: #fca5a5;
  white-space: pre-wrap;
  word-break: break-all;
}

.diag-card-advice {
  font-size: 0.82rem;
  color: var(--accent);
  display: flex;
  align-items: center;
  gap: 6px;
}

/* Memory Styles */
.memory-container {
  display: flex;
  flex-direction: column;
  gap: 14px;
  flex: 1;
  overflow: hidden;
}

.memory-stats-card {
  flex-shrink: 0;
}

.memory-stats-grid {
  display: grid;
  grid-template-columns: repeat(5, 1fr);
  gap: 12px;
  margin-bottom: 12px;
}

.mem-stat-item {
  display: flex;
  flex-direction: column;
  background-color: var(--bg-card);
  border: 1px solid var(--border-color);
  border-radius: var(--radius-md);
  padding: 8px 12px;
}

.quota-bar-wrapper {
  width: 100%;
  height: 6px;
  background-color: var(--bg-card);
  border-radius: 3px;
  overflow: hidden;
}

.quota-bar-fill {
  height: 100%;
  background-color: var(--accent);
  transition: width 0.3s ease;
}

/* Modals */
.modal {
  position: fixed;
  top: 0;
  left: 0;
  width: 100vw;
  height: 100vh;
  display: flex;
  align-items: center;
  justify-content: center;
  z-index: 1000;
}

.modal.hidden {
  display: none;
}

.modal-backdrop {
  position: absolute;
  top: 0;
  left: 0;
  width: 100%;
  height: 100%;
  background-color: rgba(0, 0, 0, 0.65);
  backdrop-filter: blur(2px);
}

.modal-dialog {
  position: relative;
  background-color: var(--bg-secondary);
  border: 1px solid var(--border-color);
  border-radius: var(--radius-lg);
  width: 90%;
  max-width: 580px;
  max-height: 85vh;
  display: flex;
  flex-direction: column;
  box-shadow: 0 16px 36px rgba(0, 0, 0, 0.4);
  z-index: 10;
}

.modal-dialog.modal-lg {
  max-width: 820px;
}

.modal-header {
  display: flex;
  align-items: center;
  justify-content: space-between;
  padding: 14px 18px;
  border-bottom: 1px solid var(--border-color);
}

.modal-header h3 {
  font-size: 1.05rem;
  color: #fff;
}

.modal-body {
  padding: 16px 18px;
  overflow-y: auto;
  flex: 1;
}

.modal-footer {
  display: flex;
  align-items: center;
  justify-content: flex-end;
  gap: 10px;
  padding: 12px 18px;
  border-top: 1px solid var(--border-color);
}

/* Forms */
.form-group {
  display: flex;
  flex-direction: column;
  gap: 6px;
  margin-bottom: 12px;
}

.form-group label {
  font-size: 0.8rem;
  color: var(--text-secondary);
  font-weight: 500;
}

.form-row {
  display: flex;
  gap: 12px;
}

.form-row .col {
  flex: 1;
}

/* Terminal Modal */
.terminal-container {
  background-color: #0b0c10;
  border: 1px solid var(--border-color);
  border-radius: var(--radius-md);
  padding: 12px;
  height: 360px;
  overflow-y: auto;
  margin-bottom: 10px;
}

.terminal-pre {
  font-family: var(--font-mono);
  font-size: 0.82rem;
  color: #e2e8f0;
  white-space: pre-wrap;
  word-break: break-all;
  line-height: 1.4;
}

.stdin-input-row {
  display: flex;
  gap: 8px;
}

.stdin-input-row input {
  flex: 1;
}

/* Detail key-values */
.detail-row {
  display: flex;
  padding: 6px 0;
  border-bottom: 1px solid rgba(46, 47, 66, 0.4);
  font-size: 0.84rem;
}

.detail-label {
  width: 150px;
  font-weight: 600;
  color: var(--text-secondary);
  flex-shrink: 0;
}

.detail-value {
  color: var(--text-primary);
  word-break: break-all;
  flex: 1;
}

/* Helpers */
.text-muted { color: var(--text-muted); }
.mb-3 { margin-bottom: 12px; }
.mt-3 { margin-top: 12px; }
.empty-message {
  padding: 24px;
  text-align: center;
  color: var(--text-muted);
  font-size: 0.85rem;
}
)CSS_1";
static const std::string& GetStyleCss() {
    static const std::string s = std::string(kStyleCss_0) + kStyleCss_1;
    return s;
}



const char kAppJs_0[] = R"JS_0(/**
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
)JS_0";
const char kAppJs_1[] = R"JS_1(    if (data.events && Array.isArray(data.events)) {
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

)JS_1";
const char kAppJs_2[] = R"JS_2(    if (filtered.length === 0) {
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
)JS_2";
const char kAppJs_3[] = R"JS_3(      const s = (j.state || '').toLowerCase();
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
)JS_3";
const char kAppJs_4[] = R"JS_4(        if (p.getAttribute('data-filter') === st) p.classList.add('active');
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
)JS_4";
static const std::string& GetAppJs() {
    static const std::string s = std::string(kAppJs_0) + kAppJs_1 + kAppJs_2 + kAppJs_3 + kAppJs_4;
    return s;
}



const char kIconSvg[] = R"SVG(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32" fill="none">
  <rect width="32" height="32" rx="6" fill="#1e1e28"/>
  <rect x="4" y="6" width="24" height="16" rx="2" stroke="#38bdf8" stroke-width="2"/>
  <line x1="10" y1="26" x2="22" y2="26" stroke="#38bdf8" stroke-width="2" stroke-linecap="round"/>
  <line x1="16" y1="22" x2="16" y2="26" stroke="#38bdf8" stroke-width="2"/>
  <circle cx="9" cy="11" r="1.5" fill="#4ade80"/>
  <line x1="13" y1="11" x2="23" y2="11" stroke="#9ca3af" stroke-width="1.5" stroke-linecap="round"/>
  <line x1="9" y1="15" x2="20" y2="15" stroke="#9ca3af" stroke-width="1.5" stroke-linecap="round"/>
</svg>
)SVG";

std::string SanitizeWebPath(const std::string& rawPath) {
    std::string path = rawPath;
    if (path.empty() || path == "/") return "index.html";
    if (path.front() == '/') path.erase(0, 1);
    // Strip query parameters
    size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);
    // Strip hash
    size_t h = path.find('#');
    if (h != std::string::npos) path = path.substr(0, h);
    return path;
}

bool IsSafeRelativePath(const std::string& path) {
    if (path.find("..") != std::string::npos) return false;
    if (path.find('\\') != std::string::npos) return false;
    if (path.find(':') != std::string::npos) return false;
    return true;
}

} // namespace

std::string GetMimeTypeForPath(const std::string& path) {
    std::string ext;
    size_t dot = path.rfind('.');
    if (dot != std::string::npos) {
        ext = path.substr(dot);
        for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".ico") return "image/x-icon";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".txt") return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

bool GetWebUiAsset(const std::string& requestPath,
                   const std::string& webuiOverrideDir,
                   std::string& outContent,
                   std::string& outMimeType) {
    const std::string cleanPath = SanitizeWebPath(requestPath);
    if (!IsSafeRelativePath(cleanPath)) {
        return false;
    }

    outMimeType = GetMimeTypeForPath(cleanPath);

    // 1. Try disk override directory if provided
    if (!webuiOverrideDir.empty()) {
        std::filesystem::path dirPath = std::filesystem::u8path(webuiOverrideDir);
        std::filesystem::path filePath = dirPath / std::filesystem::u8path(cleanPath);
        std::error_code ec;
        if (std::filesystem::exists(filePath, ec) && std::filesystem::is_regular_file(filePath, ec)) {
            std::ifstream file(filePath, std::ios::binary);
            if (file) {
                std::ostringstream ss;
                ss << file.rdbuf();
                outContent = ss.str();
                return true;
            }
        }
    }

    // 2. Check embedded assets
    if (cleanPath == "index.html" || cleanPath.empty()) {
        outContent = GetIndexHtml();
        outMimeType = "text/html; charset=utf-8";
        return true;
    }
    if (cleanPath == "style.css") {
        outContent = GetStyleCss();
        outMimeType = "text/css; charset=utf-8";
        return true;
    }
    if (cleanPath == "app.js") {
        outContent = GetAppJs();
        outMimeType = "application/javascript; charset=utf-8";
        return true;
    }
    if (cleanPath == "assets/icon.svg" || cleanPath == "favicon.ico" || cleanPath == "favicon.svg") {
        outContent = kIconSvg;
        outMimeType = "image/svg+xml";
        return true;
    }

    return false;
}

} // namespace pc