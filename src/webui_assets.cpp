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

        <button class="nav-tab" data-tab="routines">
          <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
            <polygon points="5 3 19 12 5 21 5 3"></polygon>
          </svg>
          <span>Routines</span>
          <span class="tab-badge" id="badge-routine-count">0</span>
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
              <th style="width: 140px;">End time</th>
              <th style="width: 220px; text-align: center;">Actions</th>
            </tr>
          </thead>
          <tbody id="proc-tbody">
            <tr class="empty-row"><td colspan="10">Loading processes...</td></tr>
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

        <div class="filter-pills" id="tq-view-mode-pills" style="margin-right: 8px;">
          <button class="pill active" data-view="jobs">Jobs</button>
          <button class="pill" data-view="tasks">Tasks</button>
        </div>

        <div class="filter-pills" id="tq-filter-pills">
          <button class="pill active" data-filter="all">All</button>
)HTML_0";
const char kIndexHtml_1[] = R"HTML_1(          <button class="pill" data-filter="running">Running</button>
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
              <th style="width: 160px;">Task</th>
              <th style="width: 180px;">Name</th>
              <th style="width: 110px;">Queue</th>
              <th>Command</th>
              <th style="width: 80px;">Exit</th>
              <th style="width: 90px;">Duration</th>
              <th style="width: 150px;">Started</th>
              <th style="width: 150px;">Ended</th>
              <th style="width: 140px;">Updated</th>
              <th style="width: 180px; text-align: center;">Actions</th>
            </tr>
          </thead>
          <tbody id="tq-tbody">
            <tr class="empty-row"><td colspan="12">Loading task queue...</td></tr>
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
)HTML_1";
const char kIndexHtml_2[] = R"HTML_2(            <tbody id="mem-tbody">
              <tr class="empty-row"><td colspan="7">Loading memory records...</td></tr>
            </tbody>
          </table>
        </div>
      </div>
    </section>

    <!-- PANE 6: ROUTINES -->
    <section id="pane-routines" class="tab-pane">
      <div class="card">
        <div class="card-header">
          <div class="card-title">
            <svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="2">
              <polygon points="5 3 19 12 5 21 5 3"></polygon>
            </svg>
            <span>Routines</span>
          </div>
          <div class="memory-actions">
            <button id="btn-routine-run" class="btn btn-sm btn-primary" title="Run the selected routine">▶ Run</button>
            <button id="btn-routine-approve" class="btn btn-sm" title="Approve the pending proposal">Approve</button>
            <button id="btn-routine-reject" class="btn btn-sm" title="Reject the pending proposal">Reject</button>
            <button id="btn-routine-refresh" class="btn btn-sm">Refresh</button>
          </div>
        </div>
        <div class="memory-actions" style="padding: 8px 12px;">
          <input type="text" id="input-routine-ask" class="form-control" style="flex:1; min-width:240px;" placeholder="LLM に頼む (例: 全スモークを流して)" />
          <button id="btn-routine-ask" class="btn btn-sm">LLM に頼む</button>
        </div>
        <div id="routine-status" class="pane-info"></div>
        <table class="data-table">
          <thead>
            <tr><th>Name</th><th>Summary</th><th>Inputs</th><th>Last run</th><th>Started</th></tr>
          </thead>
          <tbody id="routine-tbody">
            <tr class="empty-row"><td colspan="5">Loading routines...</td></tr>
          </tbody>
        </table>
      </div>
      <div class="card" style="margin-top: 12px;">
        <div class="card-header"><div class="card-title">Last run details</div></div>
        <div id="routine-detail" class="pane-info">Select a routine to see its node results.</div>
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
            <label for="form-proc-name">Process / Job Name:</label>
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

    <!-- MODAL: TASK DETAILS -->
    <div id="modal-task-details" class="modal hidden">
      <div class="modal-backdrop"></div>
      <div class="modal-dialog modal-lg">
        <div class="modal-header">
          <h3 id="task-modal-title">Task Details</h3>
          <button class="btn-close modal-close-btn">&times;</button>
        </div>
        <div class="modal-body" id="task-modal-body">
          <!-- Filled dynamically -->
        </div>
        <div class="modal-footer">
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
  --taken-over: #a78bfa;
  --taken-over-bg: rgba(167, 139, 250, 0.12);
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
.badge-planned { background-color: var(--pending-bg); color: var(--pending); border: 1px dashed var(--pending); opacity: 0.8; }
.badge-blocked { background-color: var(--blocked-bg); color: var(--blocked); }
.badge-completed, .badge-succeeded { background-color: var(--success-bg); color: var(--success); }
.badge-failed { background-color: var(--error-bg); color: var(--error); }
.badge-cancelled { background-color: var(--cancelled-bg); color: var(--cancelled); }
.badge-taken-over { background-color: var(--taken-over-bg); color: var(--taken-over); border: 1px solid var(--taken-over); }
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

)CSS_0";
const char kStyleCss_1[] = R"CSS_1(.event-card {
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
  font-family: var(--font-mono);
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

/* Task Lineage & Details */
.task-heading-row {
  background-color: rgba(56, 189, 248, 0.06);
  cursor: pointer;
}
.task-heading-row:hover {
  background-color: rgba(56, 189, 248, 0.12);
}
.row-superseded {
  opacity: 0.62;
}
.task-link {
  color: var(--accent, #38bdf8);
  cursor: pointer;
  text-decoration: underline;
  text-decoration-style: dotted;
}
.task-link:hover {
  text-decoration-style: solid;
}
.task-intent-box {
  background: var(--bg-secondary, #1a1b26);
  border: 1px solid var(--border-color, #2e2f42);
  border-radius: 6px;
  padding: 10px 12px;
  white-space: pre-wrap;
  word-break: break-word;
  font-size: 0.84rem;
  margin: 6px 0 12px;
  max-height: 220px;
  overflow-y: auto;
}
.task-log-list {
  display: flex;
  flex-direction: column;
  gap: 6px;
  max-height: 260px;
  overflow-y: auto;
  margin-top: 6px;
}
.task-log-entry {
  font-size: 0.8rem;
  padding: 6px 8px;
  background: var(--bg-secondary, #1a1b26);
  border-radius: 4px;
  border-left: 3px solid var(--accent, #38bdf8);
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
    tasks: [],
    directions: [],
    tqViewMode: 'jobs',
    expandedTasks: {},
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
    selectedMemoryRecord: null,

    // Routines
    routines: [],
    selectedRoutine: null,
    routineRequestKind: '',
    routineBusy: false
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
  const elBadgeRoutineCount = document.getElementById('badge-routine-count');

  // Routines Elements
  const elRoutineTbody = document.getElementById('routine-tbody');
  const elRoutineStatus = document.getElementById('routine-status');
  const elRoutineDetail = document.getElementById('routine-detail');
  const elBtnRoutineRun = document.getElementById('btn-routine-run');
  const elBtnRoutineRefresh = document.getElementById('btn-routine-refresh');
  const elBtnRoutineAsk = document.getElementById('btn-routine-ask');
  const elInputRoutineAsk = document.getElementById('input-routine-ask');
  const elBtnRoutineApprove = document.getElementById('btn-routine-approve');
  const elBtnRoutineReject = document.getElementById('btn-routine-reject');

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
  const elTqViewModePills = document.getElementById('tq-view-mode-pills');
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

  const elModalTaskDetails = document.getElementById('modal-task-details');
  const elTaskModalTitle = document.getElementById('task-modal-title');
  const elTaskModalBody = document.getElementById('task-modal-body');

  const elModalMemDetails = document.getElementById('modal-mem-details');
  const elMemModalBody = document.getElementById('mem-modal-body');

  const elModalMemPrune = document.getElementById('modal-mem-prune');
  const elInputPruneDays = document.getElementById('input-prune-days');
  const elChkPruneDryRun = document.getElementById('chk-prune-dry-run');
)JS_0";
const char kAppJs_1[] = R"JS_1(  const elBtnSubmitPrune = document.getElementById('btn-submit-prune');
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
      scheduleTranslateFlush(0);
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

    // translate_get answer (card 106-4)
    if (data.ok === true && Array.isArray(data.items) && typeof data.lang === 'string' && 'revision' in data) {
      handleTranslateResponse(data);
      return;
    }

    // Process list response
    if (data.processes && Array.isArray(data.processes)) {
      state.processes = data.processes;
      elBadgeProcCount.textContent = state.processes.length;
      renderProcessesTable();
      updateMonitorAgentOptions();
      return;
    }

    // Task queue jobs list response
    if (data.jobs && Array.isArray(data.jobs) && !data.task) {
      state.jobs = data.jobs;
      elBadgeTqCount.textContent = state.jobs.length;
      renderTaskQueueTable();
      checkFailuresFromJobs();
      return;
    }

    // Task list response
    if (data.tasks && Array.isArray(data.tasks)) {
      state.tasks = data.tasks;
      if (state.activeTab === 'taskqueue') {
        renderTaskQueueTable();
      }
      return;
    }

    // Direction list response
    if (data.directions && Array.isArray(data.directions) && !data.task) {
      state.directions = data.directions;
      if (state.activeTab === 'taskqueue') {
        renderTaskQueueTable();
      }
      return;
    }

    // Card show response
    if (data.ok && data.card && data.direction && !data.task) {
      renderCardDetailsModal(data);
      return;
    }

    // Task show response
    if (data.ok && data.task && Array.isArray(data.jobs) && Array.isArray(data.log)) {
      renderTaskDetailsModal(data);
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
    if ((data.engine !== undefined || data.agent_type !== undefined) && data.status !== undefined && data.id !== undefined) {
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

    // Routines
    if (data.routines && Array.isArray(data.routines)) {
      state.routines = data.routines;
      elBadgeRoutineCount.textContent = state.routines.length;
      renderRoutinesTable();
      if (state.selectedRoutine) {
        const current = state.routines.find(r => r.name === state.selectedRoutine);
        if (current && current.lastRun && current.lastRun.state === 'running' && !state.routineBusy) {
          startRoutineRun(current.name);
        }
      }
      return;
    }
    if (data.definition && Array.isArray(data.nodes)) {
      renderRoutineDetail(data);
      return;
    }
    if (data.name && (data.status === 'proposed' || data.status === 'approved' || data.status === 'rejected') && data.engine !== undefined) {
      elRoutineStatus.textContent = `routine ${data.name}: ${data.status}` +
        (data.rationale ? ` - ${data.rationale}` : '');
      state.selectedRoutine = data.name;
      loadRoutineDetail(data.name);
      sendWsRequest({ action: 'routine_list' });
      return;
    }
    if (data.name && data.status === 'approved' && data.approvedVia !== undefined) {
      elRoutineStatus.textContent = `routine ${data.name} approved via ${data.approvedVia}`;
      state.selectedRoutine = data.name;
      loadRoutineDetail(data.name);
      sendWsRequest({ action: 'routine_list' });
      return;
    }
    if (data.name && data.status === 'rejected' && data.reason !== undefined) {
      elRoutineStatus.textContent = `routine ${data.name} proposal rejected: ${data.reason}`;
      sendWsRequest({ action: 'routine_list' });
      return;
    }
    if (data.runId && data.taskqueue && data.inputsState) {
      const failed = data.waited && data.state !== 'succeeded';
      elRoutineStatus.textContent = (data.inputsState === 'stale' ? 'STALE inputs - ' : '') +
        `run ${data.runId}: ${data.state || 'running'}`;
      if (failed) elRoutineStatus.textContent += ' (failed)';
      state.routineBusy = false;
      state.selectedRoutine = data.name || state.selectedRoutine;
      loadRoutineDetail(state.selectedRoutine);
      sendWsRequest({ action: 'routine_list' });
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
)JS_1";
const char kAppJs_2[] = R"JS_2(        tab.classList.remove('active');
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
      sendWsRequest({ action: 'taskqueue_list', include_planned: true });
      sendWsRequest({ action: 'task_list' });
      sendWsRequest({ action: 'direction_list' });
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
      // Keep the canonical (unfiltered) list fresh here too. A state:'failed' reply would
      // replace state.jobs with a partial list and hide a waitingForLlm task group.
      sendWsRequest({ action: 'taskqueue_list', include_planned: true });
    } else if (state.activeTab === 'memory') {
      sendWsRequest({ action: 'memory_storage_stats' });
      searchMemory();
    } else if (state.activeTab === 'routines') {
      sendWsRequest({ action: 'routine_list' });
    }
  }

  function loadRoutineDetail(name) {
    if (!name) return;
    state.routineRequestKind = 'show';
    sendWsRequest({ action: 'routine_show', name: name });
  }

  function startRoutineRun(name) {
    if (!name) return;
    state.routineBusy = true;
    state.selectedRoutine = name;
    elRoutineStatus.textContent = `Starting ${name}...`;
    sendWsRequest({ action: 'routine_run', name: name, wait: false });
  }

  function askRoutine(request, name) {
    if (!request) return;
    elRoutineStatus.textContent = `Asking the LLM to ${name ? 'fix ' + name : 'create a routine'}...`;
    const req = { action: 'routine_ask', request: request, cwd: state.projectCwd || '' };
    if (name) req.name = name;
    sendWsRequest(req);
  }

  function approveRoutine(name) {
    if (!name) return;
    const said = window.prompt('承認しますか？あなたの言葉を入力してください (空欄で拒否):', '承認する');
    if (said === null) return;
    sendWsRequest({ action: 'routine_approve', name: name, userSaid: said, via: 'chat' });
  }

  function rejectRoutine(name) {
    if (!name) return;
    const reason = window.prompt('却下の理由を入力してください:', '');
    if (reason === null) return;
    sendWsRequest({ action: 'routine_reject', name: name, reason: reason });
  }

  function renderRoutinesTable() {
    if (!elRoutineTbody) return;
    if (state.routines.length === 0) {
      elRoutineTbody.innerHTML = '<tr class="empty-row"><td colspan="5">No routines saved yet.</td></tr>';
      return;
    }
    let html = '';
    state.routines.forEach(r => {
      const stale = r.state === 'stale';
      const diff = stale
        ? ` (+${(r.added || []).length} -${(r.removed || []).length} ~${(r.changed || []).length})`
        : '';
      const last = r.lastRun || {};
      html += `<tr class="routine-row${state.selectedRoutine === r.name ? ' selected' : ''}" data-routine="${escapeHtml(r.name)}" style="cursor:pointer;">` +
        `<td>${escapeHtml(r.name)}</td>` +
        `<td>${escapeHtml(r.summary || '')}</td>` +
        `<td class="${stale ? 'status-failed' : 'status-success'}">${stale ? 'STALE' : 'ok'}${diff}</td>` +
        `<td>${escapeHtml(last.state || '-')}</td>` +
        `<td>${escapeHtml(last.startedAt || '-')}` +
        `<button class="btn btn-xs btn-fix-routine" data-fix="${escapeHtml(r.name)}" title="LLM に直させる">LLM に直させる</button>` +
        (r.hasProposal ? ' <span class="status-failed">[提案中]</span>' : '') +
        `</td></tr>`;
    });
    elRoutineTbody.innerHTML = html;
    elRoutineTbody.querySelectorAll('.routine-row').forEach(row => {
      row.addEventListener('click', () => {
        state.selectedRoutine = row.getAttribute('data-routine');
        renderRoutinesTable();
        loadRoutineDetail(state.selectedRoutine);
      });
    });
    elRoutineTbody.querySelectorAll('.btn-fix-routine').forEach(btn => {
      btn.addEventListener('click', ev => {
        ev.stopPropagation();
        const name = btn.getAttribute('data-fix');
        askRoutine(`直す: ${name}。${rSummary(name)} を確認して、失敗したノードと入力の差分を直してください。`, name);
      });
    });
  }

  function rSummary(name) {
    const row = (state.routines || []).find(r => r.name === name);
    return row ? (row.summary || name) : name;
  }

  function renderRoutineDetail(data) {
    if (!elRoutineDetail) return;
    const last = data.lastRun || {};
    let html = `<div><strong>${escapeHtml(data.name || '')}</strong> [${escapeHtml(data.scope || '')}] ` +
      `inputs: ${escapeHtml(data.state || 'ok')}</div>`;
    if (data.state === 'stale') {
      html += `<div class="status-failed">added: ${escapeHtml(JSON.stringify(data.added || []))} ` +
        `removed: ${escapeHtml(JSON.stringify(data.removed || []))} ` +
        `changed: ${escapeHtml(JSON.stringify(data.changed || []))} ` +
        `absorbed: ${escapeHtml(JSON.stringify(data.absorbed || []))}</div>`;
    }
    html += `<div>last run: ${escapeHtml(last.runId || '-')} (${escapeHtml(last.state || '-')})</div>`;
    if (data.hasProposal && data.proposal) {
      const p = data.proposal;
      html += `<div class="status-failed">提案中 (${escapeHtml(p.by || '')} / ${escapeHtml(p.engine || '')}): ` +
        `${escapeHtml(p.request || '')}</div>`;
      html += `<div>rationale: ${escapeHtml(p.rationale || '')}</div>`;
      const dry = data.dryRun || {};
      html += `<div>dry run: ${escapeHtml(String(dry.count || 0))} job(s)</div>`;
      if (Array.isArray(dry.jobs)) {
        html += '<table class="data-table"><thead><tr><th>Node</th><th>Command</th></tr></thead><tbody>';
        dry.jobs.forEach(j => {
          html += `<tr><td>${escapeHtml(j.node || '')}</td><td>${escapeHtml(j.command || '')}</td></tr>`;
        });
        html += '</tbody></table>';
      }
      html += `<div>承認: <button id="routine-detail-approve" class="btn btn-xs">Approve</button> ` +
        `<button id="routine-detail-reject" class="btn btn-xs">Reject</button></div>`;
    }
    if (Array.isArray(data.newlyFailed) || Array.isArray(data.fixed)) {
      html += `<div>vs previous: newly failed ${escapeHtml(JSON.stringify(data.newlyFailed || []))}, ` +
        `fixed ${escapeHtml(JSON.stringify(data.fixed || []))}</div>`;
    }
    html += '<table class="data-table"><thead><tr><th>Node</th><th>State</th><th>ms</th><th>Tests</th></tr></thead><tbody>';
    (data.nodes || []).forEach(n => {
      let tests = '';
      (n.jobs || []).forEach(j => {
        const s = j.test_report_summary;
        if (s) tests += `${s.total || 0}/${s.failed || 0}f/${s.skipped || 0}s `;
      });
      html += `<tr><td>${escapeHtml(n.node || '')}</td><td>${escapeHtml(n.state || '')}</td>` +
        `<td>${n.duration_ms || 0}</td><td>${escapeHtml(tests)}</td></tr>`;
    });
    html += '</tbody></table>';
    elRoutineDetail.innerHTML = html;
    const approveBtn = document.getElementById('routine-detail-approve');
    if (approveBtn) approveBtn.addEventListener('click', () => approveRoutine(data.name));
    const rejectBtn = document.getElementById('routine-detail-reject');
    if (rejectBtn) rejectBtn.addEventListener('click', () => rejectRoutine(data.name));
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
)JS_2";
const char kAppJs_3[] = R"JS_3(    const s = Math.floor(sec % 60);
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

  function parseTimestamp(value) {
    if (!value) return null;
    if (typeof value === 'number') return new Date(value * 1000);
    const text = String(value);
    const d = new Date(text.includes('T') ? text : text.replace(' ', 'T'));
    return isNaN(d.getTime()) ? null : d;
  }

  function formatDateTime(value) {
    if (!value) return '-';
    const d = parseTimestamp(value);
    if (!d) return String(value);
    const pad = n => String(n).padStart(2, '0');
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ` +
           `${pad(d.getHours())}:${pad(d.getMinutes())}:${pad(d.getSeconds())}`;
  }

  function taskAttemptTimes(job) {
    const attempts = Array.isArray(job.attempts) ? job.attempts : [];
    const last = attempts.length ? attempts[attempts.length - 1] : null;
    const start = (last && (last.start || last.started_at)) ||
                  job.started_at || job.startedAt || job.start_time || '';
    const end = (last && (last.end || last.ended_at)) ||
                job.ended_at || job.endedAt || job.finished_at || job.finishedAt || '';
    return { start, end };
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
    if (s === 'pending' || s === 'queued' || s === 'waiting' || s === 'planned' || s === 'waitingforllm' || s === 'paused') return 'badge badge-planned';
    if (s === 'blocked') return 'badge badge-blocked';
    if (s === 'completed' || s === 'succeeded') return 'badge badge-completed';
    if (s === 'failed' || s === 'timed_out' || s === 'idle_timed_out' || s === 'startup_timed_out') return 'badge badge-failed';
    if (s === 'cancelled' || s === 'killed') return 'badge badge-cancelled';
    if (s === 'takenover') return 'badge badge-taken-over';
    if (s === 'waitingforpermit') return 'badge badge-blocked';
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
        if (filter === 'failed' && st !== 'failed' && st !== 'timed_out' && st !== 'idle_timed_out' && st !== 'startup_timed_out') return false;
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
      elProcTbody.innerHTML = `<tr class="empty-row"><td colspan="10">${escapeHtml(msg)}</td></tr>`;
      return;
    }

    filtered.sort((a, b) => Number(b.id) - Number(a.id));

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
      const ended = formatTime(p.end_time);

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
        <td>${ended}</td>
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

  function getTaskRecordById(taskId) {
    if (!taskId) return null;
    return state.tasks.find(t => t.id === taskId) || null;
  }

  function getJobTaskInfo(job) {
    if (job.task && typeof job.task === 'object') {
      const id = job.task.id || '';
      const rec = getTaskRecordById(id);
      return {
        id: id,
        title: job.task.title || (rec ? rec.title : '') || '',
        state: job.task.state || (rec ? rec.state : '') || '',
)JS_3";
const char kAppJs_4[] = R"JS_4(        hasRecord: !!(job.task.title || rec)
      };
    }
    const id = job.task_id || job.taskId || '';
    const rec = getTaskRecordById(id);
    return {
      id: id,
      title: rec ? (rec.title || '') : '',
      state: rec ? (rec.state || '') : '',
      hasRecord: !!rec
    };
  }

  function jobStateSymbol(st) {
    const s = (st || '').toLowerCase();
    if (s === 'succeeded' || s === 'completed') return '✓';
    if (s === 'failed' || s === 'blocked' || s === 'timed_out' || s === 'idle_timed_out' || s === 'startup_timed_out') return '✗';
    if (s === 'running' || s === 'waitingforllm' || s === 'paused') return '▶';
    if (s === 'cancelled' || s === 'closed' || s === 'takenover' || s === 'killed') return '−';
    return '·';
  }

  function formatTaskResultsCompact(jobs) {
    const syms = jobs.map(j => jobStateSymbol(j.state));
    if (syms.length <= 8) return syms.join('');
    let out = '';
    let i = 0;
    while (i < syms.length) {
      const sym = syms[i];
      let j = i + 1;
      while (j < syms.length && syms[j] === sym) j++;
      const run = j - i;
      if (out) out += ' ';
      out += sym + (run > 1 ? '×' + run : '');
      i = j;
    }
    return out;
  }

  function buildJobRowHtml(job, indent) {
    const key = escapeHtml(job.key || job.id || '-');
    const name = escapeHtml(job.name || D(job.summary || '') || '(unnamed)');
    const queue = escapeHtml(job.taskqueue || job.queue || 'default');
    const cmd = escapeHtml(job.command || job.commandLine || job.cmd || '-');
    const stateBadge = `<span class="${getBadgeClass(job.state)}">${escapeHtml(job.state || 'unknown')}</span>`;
    const attempts = Array.isArray(job.attempts) ? job.attempts : [];
    const lastAttempt = attempts.length ? attempts[attempts.length - 1] : null;
    const exitValue = job.exit_code !== undefined ? job.exit_code
      : (lastAttempt && lastAttempt.exit_code !== undefined ? lastAttempt.exit_code : job.exitCode);
    const exitCode = exitValue !== undefined ? exitValue : '-';

    const isRunning = (job.state || '').toLowerCase() === 'running';
    const isFailed = (job.state || '').toLowerCase() === 'failed';
    const supBy = job.superseded_by || job.supersededBy || '';
    const supFrom = job.supersedes || '';
    const isSuperseded = !!supBy;

    const { start: startAt, end: endAt } = taskAttemptTimes(job);
    const started = startAt ? formatDateTime(startAt) : '-';
    const ended = endAt ? formatDateTime(endAt) : (isRunning ? 'running' : '-');
    let durationSec = job.duration_sec !== undefined ? job.duration_sec
      : (job.durationSec !== undefined ? job.durationSec
      : (job.elapsed_sec !== undefined ? job.elapsed_sec : job.elapsedSec));
    if (durationSec === undefined && startAt && endAt) {
      const startDate = parseTimestamp(startAt);
      const endDate = parseTimestamp(endAt);
      if (startDate && endDate) {
        durationSec = Math.max(0, (endDate.getTime() - startDate.getTime()) / 1000);
      }
    }
    const duration = formatDuration(durationSec);
    const updatedRaw = job.updated_at || job.updatedAt || job.created_at || job.createdAt;
    const updated = formatDateTime(updatedRaw);

    const taskInfo = getJobTaskInfo(job);
    let taskCell = '<span class="text-muted">-</span>';
    if (taskInfo.id) {
      const taskLabel = taskInfo.title ? `${taskInfo.title} (${taskInfo.id})` : taskInfo.id;
      if (taskInfo.hasRecord) {
        taskCell = `<a class="task-link" data-task-id="${escapeHtml(taskInfo.id)}" title="${escapeHtml(taskLabel)}">${escapeHtml(D(taskInfo.title || '') || taskInfo.id)}</a>`;
      } else {
        taskCell = `<span class="text-muted" title="${escapeHtml(taskLabel)}">${escapeHtml(taskInfo.id)}</span>`;
      }
    }

    let lineageBadge = '';
    if (supFrom) lineageBadge += ` <span class="text-muted" title="Supersedes ${escapeHtml(supFrom)}">↩ ${escapeHtml(supFrom)}</span>`;
    if (supBy) lineageBadge += ` <span class="text-muted" title="Superseded by ${escapeHtml(supBy)}">→ ${escapeHtml(supBy)}</span>`;

    const indentLevel = typeof indent === 'number' ? indent : (indent ? 1 : 0);
    const padStyle = indentLevel > 0 ? ` style="padding-left: ${indentLevel * 24}px;"` : '';
    const rowClass = isSuperseded ? ' class="row-superseded"' : '';

    return `<tr${rowClass} data-job-key="${key}">
      <td>${stateBadge}</td>
      <td class="job-key font-mono"${padStyle} title="${key}">${indentLevel > 0 ? '↳ ' : ''}${key}${lineageBadge}</td>
      <td class="job-task">${taskCell}</td>
      <td class="job-name" title="${name}">${name}</td>
      <td>${queue}</td>
      <td class="job-cmd" title="${cmd}">${cmd}</td>
      <td>${exitCode}</td>
      <td>${duration}</td>
      <td title="${escapeHtml(startAt)}">${started}</td>
      <td title="${escapeHtml(endAt)}">${ended}</td>
      <td>${updated}</td>
      <td style="text-align: center;">
        <button class="btn btn-sm btn-job-details" data-key="${key}">Details</button>
        ${isFailed ? `<button class="btn btn-sm btn-primary btn-job-retry" data-key="${key}">Retry</button>` : ''}
        ${isRunning ? `<button class="btn btn-sm btn-danger btn-job-cancel" data-key="${key}">Cancel</button>` : ''}
      </td>
    </tr>`;
  }

  function renderTaskQueueTable() {
    const q = state.tqSearch.toLowerCase().trim();
    const filter = state.tqFilter.toLowerCase();

    const filtered = state.jobs.filter(job => {
      const st = (job.state || '').toLowerCase();
      if (filter !== 'all') {
        if (filter === 'succeeded' && st !== 'succeeded' && st !== 'completed') return false;
        if (filter === 'failed' && st !== 'failed' && st !== 'timed_out' && st !== 'idle_timed_out' && st !== 'startup_timed_out') return false;
        if (filter === 'pending' && st !== 'pending' && st !== 'queued' && st !== 'waiting' &&
            st !== 'planned' && st !== 'waitingforllm' && st !== 'paused') return false;
        if (filter === 'running' && st !== 'running') return false;
        if (filter === 'cancelled' && st !== 'cancelled' && st !== 'killed') return false;
      }
      if (q) {
        const key = (job.key || job.id || '').toLowerCase();
        const name = (job.name || '').toLowerCase();
        const queue = (job.taskqueue || job.queue || '').toLowerCase();
        const cmd = (job.commandLine || job.cmd || '').toLowerCase();
        const taskInfo = getJobTaskInfo(job);
        const taskText = (taskInfo.id + ' ' + taskInfo.title).toLowerCase();
        if (!key.includes(q) && !name.includes(q) && !queue.includes(q) && !cmd.includes(q) && !taskText.includes(q)) {
          return false;
        }
      }
      return true;
    });

    let html = '';

    if (state.tqViewMode === 'tasks') {
      // Group jobs by effective task id
      const supMap = {};
      const expMap = {};
      for (const j of state.jobs) {
        const jid = j.id || j.key || '';
        const tid = j.task_id || j.taskId || (j.task && j.task.id) || '';
        if (jid && j.supersedes) supMap[jid] = j.supersedes;
        if (jid && tid) expMap[jid] = tid;
      }
      function resolveTaskId(job) {
        const tid = job.task_id || job.taskId || (job.task && job.task.id) || '';
        if (tid) return tid;
        let cur = job.id || job.key || '';
        for (let step = 0; step < 128; step++) {
          if (expMap[cur]) return expMap[cur];
          if (!supMap[cur] || supMap[cur] === cur) break;
          cur = supMap[cur];
        }
        const did = job.direction_id || job.directionId || '';
        if (did && cur === (job.id || job.key || '')) return did;
        if (cur.startsWith('direction:')) return cur.slice(10);
        return cur;
      }

      const groupMap = {};
      const groupOrder = [];
      function ensureGroup(id) {
        if (!groupMap[id]) {
          const rec = getTaskRecordById(id);
          groupMap[id] = {
            id: id,
            title: rec ? (rec.title || '') : '',
            state: rec ? (rec.state || 'open') : '',
            hasRecord: !!rec,
            jobs: []
          };
          groupOrder.push(id);
        }
        return groupMap[id];
      }

      for (const t of state.tasks) {
        if (!t || !t.id) continue;
        const st = (t.state || '').toLowerCase();
        if (filter !== 'all' && st !== filter) continue;
        if (q) {
          const txt = (t.id + ' ' + (t.title || '')).toLowerCase();
          if (!txt.includes(q)) continue;
        }
        ensureGroup(t.id);
      }

      const dirsByTask = {};
      for (const dir of state.directions) {
        if (!dir || dir.implicit || !dir.taskId) continue;
        if (!dirsByTask[dir.taskId]) dirsByTask[dir.taskId] = [];
        dirsByTask[dir.taskId].push(dir);
      }

      for (const job of filtered) {
        const tid = resolveTaskId(job);
        const g = ensureGroup(tid);
        const info = getJobTaskInfo(job);
        if (!g.title && info.title) g.title = info.title;
)JS_4";
const char kAppJs_5[] = R"JS_5(        if (!g.state && info.state) g.state = info.state;
        if (info.hasRecord) g.hasRecord = true;
        g.jobs.push(job);
      }

      function compareCardNumberNatural(a, b) {
        let ia = 0, ib = 0;
        while (ia < a.length && ib < b.length) {
          const da = a[ia] >= '0' && a[ia] <= '9';
          const db = b[ib] >= '0' && b[ib] <= '9';
          if (da && db) {
            let za = ia; while (za < a.length && a[za] === '0') za++;
            let ea = za; while (ea < a.length && a[ea] >= '0' && a[ea] <= '9') ea++;
            let zb = ib; while (zb < b.length && b[zb] === '0') zb++;
            let eb = zb; while (eb < b.length && b[eb] >= '0' && b[eb] <= '9') eb++;
            const la = ea - za, lb = eb - zb;
            if (la !== lb) return la - lb;
            const sa = a.slice(za, ea), sb = b.slice(zb, eb);
            if (sa !== sb) return sa < sb ? -1 : 1;
            if ((ea - ia) !== (eb - ib)) return (ea - ia) - (eb - ib);
            ia = ea; ib = eb;
          } else {
            if (a[ia] !== b[ib]) return a[ia] < b[ib] ? -1 : 1;
            ia++; ib++;
          }
        }
        return a.length - b.length;
      }

      if (groupOrder.length === 0) {
        const msg = (state.jobs.length === 0 && state.tasks.length === 0) ? 'No tasks or jobs in queue.' : 'No tasks match current filter.';
        elTqTbody.innerHTML = `<tr class="empty-row"><td colspan="12">${escapeHtml(msg)}</td></tr>`;
        return;
      }

      for (const gid of groupOrder) {
        const g = groupMap[gid];
        const dirMap = {};
        const dirOrder = [];
        const taskDirs = dirsByTask[gid] || [];
        for (const dir of taskDirs) {
          const did = dir.id;
          if (!did) continue;
          if (!dirMap[did]) {
            dirMap[did] = {
              id: did,
              kind: dir.kind || 'jobs',
              title: (dir.card && dir.card.title) || dir.title || '',
              state: dir.state || '',
              isVirtualBt: false,
              isLegacyGroup: false,
              isCard: !!dir.card,
              cardNumber: dir.card ? (dir.card.number || '') : '',
              waitingFor: Array.isArray(dir.waitingFor) ? dir.waitingFor : [],
              supersedes: dir.supersedes || '',
              supersededBy: dir.supersededBy || '',
              realJobs: [],
              virtualJob: null
            };
            dirOrder.push(did);
          }
        }
        for (const job of g.jobs) {
          const jid = job.id || job.key || '';
          let did = job.direction_id || job.directionId || '';
          if (!did) {
            if (jid.startsWith('direction:')) did = jid.slice(10);
            else if (jid.startsWith('group:')) did = jid;
            else did = 'job:' + jid;
          }
          if (!dirMap[did]) {
            dirMap[did] = { id: did, kind: 'jobs', title: '', state: '', isVirtualBt: false, isLegacyGroup: false, isCard: false, cardNumber: '', waitingFor: [], supersedes: '', supersededBy: '', realJobs: [], virtualJob: null };
            dirOrder.push(did);
          }
          const d = dirMap[did];
          if (jid.startsWith('direction:') || jid.startsWith('group:')) {
            d.isVirtualBt = true;
            d.isLegacyGroup = jid.startsWith('group:');
            d.kind = 'bt';
            if (!d.title) d.title = job.summary || '';
            if (!d.state) d.state = job.state || '';
            d.virtualJob = job;
          } else {
            d.realJobs.push(job);
            if (job.bt && typeof job.bt === 'object') d.kind = 'bt';
          }
        }
        dirOrder.sort((a, b) => {
          const da = dirMap[a], db = dirMap[b];
          if (da.isCard !== db.isCard) return da.isCard ? 1 : -1;
          if (da.isCard && db.isCard) {
            const cmp = compareCardNumberNatural(da.cardNumber, db.cardNumber);
            if (cmp !== 0) return cmp;
            return a < b ? -1 : (a > b ? 1 : 0);
          }
          return 0;
        });
        let totalRealJobs = 0;
        const dirStateObjs = [];
        for (const did of dirOrder) {
          const d = dirMap[did];
          totalRealJobs += d.realJobs.length;
          if (!d.title && d.realJobs.length) d.title = d.realJobs[0].summary || d.realJobs[0].name || '';
          if (!d.state) {
            const lastJ = d.realJobs.length ? d.realJobs[d.realJobs.length - 1] : null;
            d.state = lastJ ? (lastJ.state || 'open') : 'open';
          }
          d.hasHeading = d.isCard || (d.state === 'planned') || (d.isVirtualBt && (!d.isLegacyGroup || d.realJobs.length > 0)) || d.realJobs.length >= 2;
          dirStateObjs.push({ state: d.state });
        }

        if (!g.hasRecord && g.jobs.length === 1 && !dirMap[dirOrder[0]].hasHeading) {
          html += buildJobRowHtml(g.jobs[0], 0);
          continue;
        }

        const emitTaskHeading = g.hasRecord || dirOrder.length > 1;
        let taskExpanded = true;
        if (emitTaskHeading) {
          const taskState = g.state || (g.jobs.length ? (g.jobs[g.jobs.length - 1].state || 'open') : 'open');
          const defaultExp = taskState === 'running' || taskState === 'failed' || taskState === 'open';
          taskExpanded = state.expandedTasks[gid] !== undefined ? state.expandedTasks[gid] : defaultExp;
          const arrow = taskExpanded ? '▾' : '▸';
          const compact = formatTaskResultsCompact(dirStateObjs);
          const titleText = g.title ? `${D(g.title)} (${gid})` : gid;
          const stateBadge = `<span class="${getBadgeClass(taskState)}">${escapeHtml(taskState)}</span>`;
          const detailsBtn = g.hasRecord
            ? `<button class="btn btn-sm btn-task-details" data-task-id="${escapeHtml(gid)}">Task Details</button>`
            : '';

          html += `<tr class="task-heading-row" data-task-id="${escapeHtml(gid)}">
            <td>${stateBadge}</td>
            <td class="font-mono" colspan="2"><strong>${arrow} ${escapeHtml(titleText)}</strong></td>
            <td colspan="3"><span class="font-mono">${escapeHtml(compact)}</span> <span class="text-muted">(${totalRealJobs} job${totalRealJobs === 1 ? '' : 's'})</span></td>
            <td colspan="5"></td>
            <td style="text-align: center;">${detailsBtn}</td>
          </tr>`;
        }

        if (taskExpanded) {
          const baseIndent = emitTaskHeading ? 1 : 0;
          for (const did of dirOrder) {
            const d = dirMap[did];
            if (!d.hasHeading) {
              for (const job of d.realJobs) {
                html += buildJobRowHtml(job, baseIndent);
              }
              continue;
            }
            const dirKey = 'direction:' + d.id;
            const dExpDef = d.state === 'running' || d.state === 'failed' || d.state === 'open' ||
                            d.state === 'waitingForLlm' || d.state === 'paused';
            const dExpanded = state.expandedTasks[dirKey] !== undefined ? state.expandedTasks[dirKey] : dExpDef;
            const dArrow = dExpanded ? '▾' : '▸';
            const dCompact = formatTaskResultsCompact(d.realJobs);
            const dTitle = d.title ? `${D(d.title)} (${dirKey})` : dirKey;
            const dBadge = `<span class="${getBadgeClass(d.state)}">${escapeHtml(d.state)}</span>`;
            let redoDepth = 0;
            let curSup = d.supersedes;
            const seenSup = new Set();
            while (curSup && !seenSup.has(curSup) && redoDepth < 16) {
              seenSup.add(curSup);
              if (!dirMap[curSup]) break;
              redoDepth++;
              curSup = dirMap[curSup].supersedes;
            }
            const effIndent = baseIndent + redoDepth;
            const dPad = effIndent > 0 ? ` style="padding-left: ${effIndent * 24}px;"` : '';
            const dStLower = (d.state || '').toLowerCase();
            if (d.isCard) {
              const dispState = (dStLower === 'planned' && d.waitingFor.length > 0) ? 'waiting' : d.state;
              const cBadge = `<span class="${getBadgeClass(dispState)}">${escapeHtml(dispState)}</span>`;
              const cArrow = d.realJobs.length > 0 ? dArrow : '·';
              const cTitle = d.title ? `${d.cardNumber} ${D(d.title)}` : d.cardNumber;
              const waitTxt = d.waitingFor.length > 0 ? `<span class="text-muted">(waiting: ${escapeHtml(d.waitingFor.join(', '))})</span> ` : '';
              const dimStyle = (dStLower === 'planned' || d.supersededBy) ? ' style="opacity: 0.65;"' : '';
              const cBtn = `<button class="btn btn-sm btn-card-details" data-dir-id="${escapeHtml(d.id)}" data-task-id="${escapeHtml(gid)}">Card</button>`;
              html += `<tr class="direction-heading-row card-direction-row" data-direction-id="${escapeHtml(dirKey)}" data-card-id="${escapeHtml(d.id)}" data-task-id="${escapeHtml(gid)}" data-card-number="${escapeHtml(d.cardNumber)}" data-card-state="${escapeHtml(dStLower)}"${dimStyle}>
                <td>${cBadge}</td>
)JS_5";
const char kAppJs_6[] = R"JS_6(                <td class="font-mono" colspan="2"${dPad}><strong>${cArrow} ${escapeHtml(cTitle)}</strong></td>
                <td colspan="3">${waitTxt}<span class="font-mono">${escapeHtml(dCompact)}</span> <span class="text-muted">(${d.realJobs.length} job${d.realJobs.length === 1 ? '' : 's'})</span></td>
                <td colspan="5"></td>
                <td style="text-align: center;">${cBtn}</td>
              </tr>`;
            } else {
              const dRetryBtn = dStLower === 'failed'
                ? `<button class="btn btn-sm btn-primary btn-job-retry" data-key="${escapeHtml(dirKey)}">Retry</button>` : '';
              const dCancelBtn = (dStLower === 'running' || dStLower === 'waitingforllm' || dStLower === 'paused')
                ? `<button class="btn btn-sm btn-danger btn-job-cancel" data-key="${escapeHtml(dirKey)}">Cancel</button>` : '';

              html += `<tr class="direction-heading-row" data-direction-id="${escapeHtml(dirKey)}">
                <td>${dBadge}</td>
                <td class="font-mono" colspan="2"${dPad}><strong>${dArrow} ${escapeHtml(dTitle)}</strong></td>
                <td colspan="3"><span class="text-muted">${T('direction')} (${escapeHtml(d.kind)})</span> <span class="font-mono">${escapeHtml(dCompact)}</span> <span class="text-muted">(${d.realJobs.length} job${d.realJobs.length === 1 ? '' : 's'})</span></td>
                <td colspan="5"></td>
                <td style="text-align: center;">${dRetryBtn} ${dCancelBtn}</td>
              </tr>`;
            }

            if (dExpanded) {
              for (const job of d.realJobs) {
                html += buildJobRowHtml(job, effIndent + 1);
              }
            }
          }
        }
      }
    } else {
      if (filtered.length === 0) {
        const msg = state.jobs.length === 0 ? 'No jobs in queue.' : 'No jobs match current filter.';
        elTqTbody.innerHTML = `<tr class="empty-row"><td colspan="12">${escapeHtml(msg)}</td></tr>`;
        return;
      }
      for (const job of filtered) {
        html += buildJobRowHtml(job, false);
      }
    }

    elTqTbody.innerHTML = html;

    // Attach listeners
    elTqTbody.querySelectorAll('.task-heading-row').forEach(row => {
      row.addEventListener('click', function() {
        const tid = this.getAttribute('data-task-id');
        if (!tid) return;
        const cur = state.expandedTasks[tid];
        const gRowState = (this.querySelector('.badge') ? this.querySelector('.badge').textContent : '').toLowerCase();
        const defExp = gRowState === 'running' || gRowState === 'failed' || gRowState === 'open';
        state.expandedTasks[tid] = cur !== undefined ? !cur : !defExp;
        renderTaskQueueTable();
      });
    });

    elTqTbody.querySelectorAll('.direction-heading-row').forEach(row => {
      row.addEventListener('click', function() {
        const did = this.getAttribute('data-direction-id');
        if (!did) return;
        const cur = state.expandedTasks[did];
        const dRowState = (this.querySelector('.badge') ? this.querySelector('.badge').textContent : '').toLowerCase();
        const defExp = dRowState === 'running' || dRowState === 'failed' || dRowState === 'open' ||
                       dRowState === 'waitingforllm' || dRowState === 'paused';
        state.expandedTasks[did] = cur !== undefined ? !cur : !defExp;
        renderTaskQueueTable();
      });
    });

    elTqTbody.querySelectorAll('.btn-task-details, .task-link').forEach(el => {
      el.addEventListener('click', function(e) {
        e.stopPropagation();
        const tid = this.getAttribute('data-task-id');
        if (tid) openTaskDetails(tid);
      });
    });

    elTqTbody.querySelectorAll('.btn-card-details').forEach(el => {
      el.addEventListener('click', function(e) {
        e.stopPropagation();
        const did = this.getAttribute('data-dir-id');
        const tid = this.getAttribute('data-task-id');
        if (did) openCardDetails(did, tid);
      });
    });

    elTqTbody.querySelectorAll('.card-direction-row').forEach(row => {
      row.addEventListener('contextmenu', function(e) {
        e.preventDefault();
        e.stopPropagation();
        showCardContextMenu(e.clientX, e.clientY, {
          id: this.getAttribute('data-card-id') || '',
          taskId: this.getAttribute('data-task-id') || '',
          number: this.getAttribute('data-card-number') || '',
          state: this.getAttribute('data-card-state') || ''
        });
      });
    });

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
        setTimeout(() => {
          sendWsRequest({ action: 'taskqueue_list', include_planned: true });
          sendWsRequest({ action: 'task_list' });
        }, 200);
      });
    });

    elTqTbody.querySelectorAll('.btn-job-cancel').forEach(btn => {
      btn.addEventListener('click', function(e) {
        e.stopPropagation();
        const key = this.getAttribute('data-key');
        sendWsRequest({ action: 'taskqueue_cancel', id: key });
        setTimeout(() => {
          sendWsRequest({ action: 'taskqueue_list', include_planned: true });
          sendWsRequest({ action: 'task_list' });
        }, 200);
      });
    });
  }

  function openTaskDetails(taskId) {
    if (!taskId) return;
    elTaskModalTitle.textContent = `Task: ${taskId}`;
    elTaskModalBody.innerHTML = '<div class="empty-message">Loading task details...</div>';
    elModalTaskDetails.classList.remove('hidden');
    sendWsRequest({ action: 'task_show', id: taskId, log_limit: 200 });
  }

  function openCardDetails(dirId, taskId) {
    if (!dirId) return;
    elTaskModalTitle.textContent = `Card: ${dirId}`;
    elTaskModalBody.innerHTML = '<div class="empty-message">Loading card details...</div>';
    elModalTaskDetails.classList.remove('hidden');
    sendWsRequest({ action: 'direction_show', id: dirId, taskId: taskId || '' });
  }

  function renderCardDetailsModal(data) {
    const dir = data.direction || {};
    const card = data.card || {};
    const status = data.status || {};
    const waiting = Array.isArray(data.waitingFor) ? data.waitingFor : [];
    const st = status.state || 'planned';
    const dispState = (st === 'planned' && waiting.length > 0) ? 'waiting' : st;
    elTaskModalTitle.textContent = `Card ${card.number || dir.id || '-'}: ${D(card.title || '', 'card.title')}`;
    let html = '';
    const fields = [
      ['Card Number', card.number || '-'],
      ['Direction ID', dir.id || '-'],
      ['Task ID', dir.taskId || '-'],
      ['State', dispState],
      ['Template', card.template || 'card'],
      ['Waiting For', waiting.length ? waiting.join(', ') : '-']
    ];
    for (const [k, v] of fields) {
      html += `<div class="detail-row">
        <div class="detail-label">${escapeHtml(k)}:</div>
        <div class="detail-value">${escapeHtml(String(v))}</div>
      </div>`;
    }
    html += `<div class="mt-3"><strong>Body</strong></div>
      <pre class="task-intent-box">${escapeHtml(card.body ? D(card.body, 'card.body') : '(no body)')}</pre>`;
    elTaskModalBody.innerHTML = html;
    elModalTaskDetails.classList.remove('hidden');
  }

  function showCardContextMenu(x, y, card) {
    const existing = document.getElementById('card-context-menu');
    if (existing) existing.remove();
    const menu = document.createElement('div');
    menu.id = 'card-context-menu';
    menu.style.cssText = `position:fixed;left:${x}px;top:${y}px;z-index:9999;background:var(--bg-card,#1e2430);border:1px solid var(--border,#334155);border-radius:6px;padding:4px 0;min-width:160px;box-shadow:0 4px 16px rgba(0,0,0,0.4);`;
    const addItem = (label, fn) => {
      const btn = document.createElement('button');
      btn.textContent = label;
      btn.style.cssText = 'display:block;width:100%;text-align:left;padding:6px 14px;background:none;border:none;color:inherit;cursor:pointer;font-size:13px;';
      btn.addEventListener('click', () => { menu.remove(); fn(); });
      menu.appendChild(btn);
    };
    if (card.state === 'planned') {
      addItem('Run card', () => {
        sendWsRequest({ action: 'direction_run', taskId: card.taskId, number: card.number });
        setTimeout(() => { sendWsRequest({ action: 'direction_list' }); sendWsRequest({ action: 'taskqueue_list', include_planned: true }); }, 250);
      });
    }
    if (card.state === 'failed' || card.state === 'closed' || card.state === 'cancelled' || card.state === 'succeeded') {
      addItem('Redo card (rescue)', () => {
)JS_6";
const char kAppJs_7[] = R"JS_7(        sendWsRequest({ action: 'direction_redo', taskId: card.taskId, number: card.number, run: true });
        setTimeout(() => { sendWsRequest({ action: 'direction_list' }); sendWsRequest({ action: 'taskqueue_list', include_planned: true }); }, 250);
      });
    }
    if (card.state !== 'closed') {
      addItem('Close direction', () => {
        sendWsRequest({ action: 'direction_close', id: card.id, result: 'ignored', reason: 'closed from webui' });
        setTimeout(() => { sendWsRequest({ action: 'direction_list' }); sendWsRequest({ action: 'taskqueue_list', include_planned: true }); }, 250);
      });
    }
    addItem('Show card body', () => openCardDetails(card.id, card.taskId));
    document.body.appendChild(menu);
    const closeMenu = () => { menu.remove(); document.removeEventListener('click', closeMenu); };
    setTimeout(() => document.addEventListener('click', closeMenu), 0);
  }

  function renderTaskDetailsModal(data) {
    const t = data.task || {};
    const status = data.status || {};
    const jobs = Array.isArray(data.jobs) ? data.jobs : [];
    const log = Array.isArray(data.log) ? data.log : [];
    const st = status.state || t.state || 'open';

    elTaskModalTitle.textContent = `Task: ${D(t.title || '') || t.id || '-'}`;

    let html = '';
    const fields = [
      ['Task ID', t.id || '-'],
      ['Title', D(t.title || '') || '-'],
      ['State', st + (status.paused ? ' (paused)' : '')],
      ['Created At', t.createdAt || '-'],
      ['Updated At', t.updatedAt || '-']
    ];
    if (t.closed && typeof t.closed === 'object') {
      fields.push(['Closed', `${t.closed.result || '-'} (${t.closed.at || '-'}) - ${t.closed.reason || ''}`]);
    }
    if (data.repeatedFailures > 0) {
      fields.push(['Repeated Failures', String(data.repeatedFailures)]);
    }

    for (const [k, v] of fields) {
      html += `<div class="detail-row">
        <div class="detail-label">${escapeHtml(k)}:</div>
        <div class="detail-value">${escapeHtml(String(v))}</div>
      </div>`;
    }

    html += `<div class="mt-3"><strong>Intent</strong></div>
      <pre class="task-intent-box">${escapeHtml(t.intent ? D(t.intent) : '(none)')}</pre>`;

    html += `<div class="mt-3"><strong>Jobs (${jobs.length})</strong></div>`;
    if (jobs.length === 0) {
      html += `<div class="text-muted" style="padding: 6px 0;">No jobs yet.</div>`;
    } else {
      html += `<div class="task-log-list">`;
      for (const j of jobs) {
        const lineage = [
          j.supersedes ? `↩ ${j.supersedes}` : '',
          j.supersededBy ? `→ ${j.supersededBy}` : ''
        ].filter(Boolean).join(' ');
        html += `<div class="task-log-entry">
          <span class="${getBadgeClass(j.state)}">${escapeHtml(j.state || '')}</span>
          <strong class="font-mono">${escapeHtml(j.id || '')}</strong>
          <span>${escapeHtml(j.name || '')}</span>
          <span class="text-muted">[${escapeHtml(j.taskqueue || 'default')}] ${escapeHtml(lineage)}</span>
        </div>`;
      }
      html += `</div>`;
    }

    html += `<div class="mt-3"><strong>Progress Log (${log.length})</strong></div>`;
    if (log.length === 0) {
      html += `<div class="text-muted" style="padding: 6px 0;">No log entries.</div>`;
    } else {
      html += `<div class="task-log-list">`;
      for (const entry of log) {
        const ref = [entry.job ? `job:${entry.job}` : '', entry.bt ? `bt:${entry.bt}` : ''].filter(Boolean).join(' ');
        html += `<div class="task-log-entry">
          <span class="text-muted font-mono">${escapeHtml(entry.at || '')}</span>
          <strong>[${escapeHtml(entry.kind || '')}]</strong>
          ${ref ? `<span class="text-muted font-mono">${escapeHtml(ref)}</span>` : ''}
          <div>${escapeHtml(entry.text || '')}</div>
        </div>`;
      }
      html += `</div>`;
    }

    elTaskModalBody.innerHTML = html;
    elModalTaskDetails.classList.remove('hidden');
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
    elMonitorAgentStatus.textContent = agentData.status || agentData.engine || agentData.agent_type || 'Active';
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
)JS_7";
const char kAppJs_8[] = R"JS_8(      </div>`;
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
      return s === 'failed' || s === 'timed_out' || s === 'idle_timed_out' || s === 'startup_timed_out';
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
      return s === 'failed' || s === 'timed_out' || s === 'idle_timed_out' || s === 'startup_timed_out';
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
      const summary = escapeHtml(D(rec.summary || rec.title || rec.text || '-'));
      const created = formatTime(rec.createdAt || rec.created);

      const isL2 = kind === 'l2';

      html += `<tr data-mem-id="${id}">
        <td>${kindBadge}</td>
        <td class="font-mono" title="${id}">${id}</td>
        <td>${job}</td>
        <td>${outcome}</td>
        <td class="job-cmd" title="${summary}">${summary}</td>
)JS_8";
const char kAppJs_9[] = R"JS_9(        <td>${created}</td>
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

  // Routines Actions
  if (elBtnRoutineRun) elBtnRoutineRun.addEventListener('click', () => startRoutineRun(state.selectedRoutine));
  if (elBtnRoutineRefresh) elBtnRoutineRefresh.addEventListener('click', () => refreshCurrentTab());
  if (elBtnRoutineAsk) elBtnRoutineAsk.addEventListener('click', () => {
    const request = elInputRoutineAsk ? elInputRoutineAsk.value.trim() : '';
    if (!request) return;
    askRoutine(request, state.selectedRoutine);
    elInputRoutineAsk.value = '';
  });
  if (elInputRoutineAsk) elInputRoutineAsk.addEventListener('keydown', ev => {
    if (ev.key === 'Enter' && elBtnRoutineAsk) elBtnRoutineAsk.click();
  });
  if (elBtnRoutineApprove) elBtnRoutineApprove.addEventListener('click', () => approveRoutine(state.selectedRoutine));
  if (elBtnRoutineReject) elBtnRoutineReject.addEventListener('click', () => rejectRoutine(state.selectedRoutine));

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

  if (elTqViewModePills) {
    elTqViewModePills.addEventListener('click', (e) => {
      const pill = e.target.closest('.pill');
      if (!pill) return;
      state.tqViewMode = pill.getAttribute('data-view') || 'jobs';
      elTqViewModePills.querySelectorAll('.pill').forEach(p => p.classList.remove('active'));
      pill.classList.add('active');
      renderTaskQueueTable();
    });
  }

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
)JS_9";
const char kAppJs_10[] = R"JS_10(    if (confirm('Purge completed, failed, and cancelled jobs from queue?')) {
      sendWsRequest({ action: 'taskqueue_purge', taskqueue: '', states: ['completed', 'failed', 'cancelled'] });
      setTimeout(() => sendWsRequest({ action: 'taskqueue_list', include_planned: true }), 300);
    }
  });

  elChkResourceLocks.addEventListener('change', (e) => {
    sendWsRequest({ action: 'taskqueue_resource_locks', enabled: e.target.checked });
  });

  elBtnJobModalRetry.addEventListener('click', () => {
    if (state.selectedJob) {
      sendWsRequest({ action: 'taskqueue_retry', id: state.selectedJob.key || state.selectedJob.id });
      elModalJobDetails.classList.add('hidden');
      setTimeout(() => sendWsRequest({ action: 'taskqueue_list', include_planned: true }), 300);
    }
  });

  elBtnJobModalCancel.addEventListener('click', () => {
    if (state.selectedJob) {
      sendWsRequest({ action: 'taskqueue_cancel', id: state.selectedJob.key || state.selectedJob.id });
      elModalJobDetails.classList.add('hidden');
      setTimeout(() => sendWsRequest({ action: 'taskqueue_list', include_planned: true }), 300);
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
    sendWsRequest({ action: 'taskqueue_list', include_planned: true });
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

  // --- Display language (card 106-4) ---
  // Fixed strings are translated by key (the English text). Free text (job summaries, task and
  // card titles, bodies, memory summaries) goes through D(), which shows the cached translation
  // from the core (translate_get) or the original while it is pending.
  const UI_LANGS = ['en', 'ja', 'es'];
  const UI_LANG_KEY = 'hancho_ui_lang';
  const uiState = { lang: 'en' };
  let uiLangSelect = null;

  const I18N = {
    ja: {
      'Active Agent / Process': '対象のエージェント / プロセス',
      'Actions': '操作',
      'Agent Event Stream (hancho.event/1)': 'エージェントのイベント (hancho.event/1)',
      'All': 'すべて',
      'All Layers': 'すべての層',
      'Approve': '承認',
      'Auto-refresh': '自動更新',
      'Blocked': 'ブロック',
      'Cancel': 'キャンセル',
      'Cancelled': 'キャンセル済み',
      'Card Number': 'カード番号',
      'Clear Finished': '終了分を消去',
      'Close': '閉じる',
      'Command': 'コマンド',
      'Command Line: *': 'コマンドライン: *',
      'Connected': '接続済み',
      'Connecting...': '接続中...',
      'Core': 'コア',
      'Cost': 'コスト',
      'Created': '作成',
      'Created At': '作成日時',
      'Date / Time': '日時',
      'Direction ID': '指示 ID',
      'direction': '指示',
      'Direction': '指示',
      'Close direction': '指示を閉じる',
      'Disconnected': '切断中',
      'Display language': '表示言語',
      'Dry run only (calculate space without deleting)': '試し実行のみ（削除せず容量だけ計算）',
      'Duration': '所要時間',
      'End time': '終了時刻',
      'Ended': '終了',
      'Errors': 'エラー',
      'Estimated Cost': '推定コスト',
      'Execute Prune': '整理を実行',
      'Execution Mode': '実行モード',
      'Exit': '終了コード',
      'Failed': '失敗',
      'Failed Jobs & Process Diagnoses': '失敗したジョブとプロセスの診断',
      'Failure Diagnosis': '失敗の診断',
      'Finished': '完了',
      'Goal': '目標',
      'Hancho Dashboard': 'Hancho ダッシュボード',
      'Hierarchical Memory Storage (L1 Runs / L2 Notes / L3 Knowledge)': '階層メモリ（L1 実行記録 / L2 ノート / L3 知識）',
      'ID': 'ID',
      'Inputs': '入力',
      'Intent': '意図',
      'Job / Queue': 'ジョブ / キュー',
      'Job Details': 'ジョブの詳細',
      'Job Key / ID': 'ジョブのキー / ID',
      'Jobs': 'ジョブ',
      'Kill All': 'すべて停止',
      'L1 Runs': 'L1 実行記録',
      'L2 Notes': 'L2 ノート',
      'L3 Knowledge': 'L3 知識',
      'Last 24 Hours': '直近 24 時間',
      'Last run': '最終実行',
      'Last run details': '最終実行の詳細',
      'Layer': '層',
      'Loading memory records...': 'メモリの記録を読み込み中...',
      'Loading output...': '出力を読み込み中...',
      'Loading processes...': 'プロセスを読み込み中...',
      'Loading routines...': 'ルーチンを読み込み中...',
      'Loading task queue...': 'タスクキューを読み込み中...',
      'Memory': 'メモリ',
      'Memory Record': 'メモリの記録',
      'Model': 'モデル',
      'Monitor': 'モニター',
      'Name': '名前',
      'Name / Command': '名前 / コマンド',
      'No failure statistics available.': '失敗の統計はありません。',
      'No failures currently detected. Everything is running smoothly!': '現在、失敗は検出されていません。',
      'No usage data recorded.': '使用量の記録がありません。',
      'Normal': '通常',
      'Outcome': '結果',
      'Output Tokens': '出力トークン',
      'PID': 'PID',
      'PTY (pseudo-terminal)': 'PTY（擬似端末）',
      'Pause Queue': 'キューを一時停止',
      'Pending': '保留',
      'Pipe (standard I/O)': 'パイプ（標準入出力）',
      'Process / Job Name': 'プロセス / ジョブ名',
      'Process Terminal & Logs': 'プロセスの端末とログ',
      'Processes': 'プロセス',
      'Progress': '進捗',
      'Prompt Tokens': '入力トークン',
      'Prune L1 Records': 'L1 記録を整理',
      'Prune L1 Run Records': 'L1 実行記録を整理',
      'Purge Finished': '終了分を削除',
      'Queue': 'キュー',
      'Queue Failure Classification (Why Did It Fail?)': 'キューの失敗分類（なぜ失敗したか）',
      'Quota Status': '割り当ての状態',
      'Reconnect Now': '今すぐ再接続',
      'Record ID': '記録 ID',
      'Refresh': '更新',
      'Refresh Diagnosis': '診断を更新',
      'Refresh state': '状態を更新',
      'Reject': '却下',
      'Repeated Failures': '繰り返した失敗',
      'Reset IDs': 'ID を振り直す',
      'Resource Locks': 'リソースロック',
      'Resume Queue': 'キューを再開',
)JS_10";
const char kAppJs_11[] = R"JS_11(      'Retention (days)': '保持期間（日）',
      'Retry': '再試行',
      'Routines': 'ルーチン',
      'Running': '実行中',
      'Save Info': '情報を保存',
      'Select a routine to see its node results.': 'ルーチンを選ぶと、ノードの結果を表示します。',
      'Select a running agent to view events.': 'イベントを見るには実行中のエージェントを選んでください。',
      'Send': '送信',
      'Start New Process': '新しいプロセスを開始',
      'Start Process': 'プロセスを開始',
      'Started': '開始',
      'State': '状態',
      'Succeeded': '成功',
      'Summary': '概要',
      'Summary / Key Insights': '概要 / 要点',
      'Supervisor Notes': 'supervisor のメモ',
      'Task': 'タスク',
      'Task Details': 'タスクの詳細',
      'Task ID': 'タスク ID',
      'Task Queue': 'タスクキュー',
      'Tasks': 'タスク',
      'Template': 'テンプレート',
      'Thinking': '思考',
      'Timeout (seconds)': 'タイムアウト（秒）',
      'Tokens': 'トークン',
      'Tools': 'ツール',
      'Total Jobs': 'ジョブ総数',
      'Total Storage': '総使用量',
      'Total Tokens': 'トークン総数',
      'Updated': '更新',
      'Updated At': '更新日時',
      'Usage & Cost Analytics': '使用量とコストの分析',
      'Waiting': '待機中',
      'Waiting For': '待ち対象',
      'Working Directory': '作業ディレクトリ',
      'Body': '本文',
      'Title': 'タイトル'
    },
    es: {
      'Active Agent / Process': 'Agente / proceso activo',
      'Actions': 'Acciones',
      'Agent Event Stream (hancho.event/1)': 'Flujo de eventos del agente (hancho.event/1)',
      'All': 'Todos',
      'All Layers': 'Todas las capas',
      'Approve': 'Aprobar',
      'Auto-refresh': 'Actualización automática',
      'Blocked': 'Bloqueado',
      'Body': 'Cuerpo',
      'Cancel': 'Cancelar',
      'Cancelled': 'Cancelado',
      'Card Number': 'Número de tarjeta',
      'Clear Finished': 'Limpiar terminados',
      'Close': 'Cerrar',
      'Command': 'Comando',
      'Command Line: *': 'Línea de comandos: *',
      'Connected': 'Conectado',
      'Connecting...': 'Conectando...',
      'Core': 'Núcleo',
      'Cost': 'Coste',
      'Created': 'Creado',
      'Created At': 'Creado el',
      'Date / Time': 'Fecha / hora',
      'Direction ID': 'ID de dirección',
      'direction': 'dirección',
      'Direction': 'Dirección',
      'Close direction': 'Cerrar dirección',
      'Disconnected': 'Desconectado',
      'Display language': 'Idioma de la interfaz',
      'Dry run only (calculate space without deleting)': 'Solo simulación (calcular el espacio sin borrar)',
      'Duration': 'Duración',
      'End time': 'Hora de fin',
      'Ended': 'Terminado',
      'Errors': 'Errores',
      'Estimated Cost': 'Coste estimado',
      'Execute Prune': 'Ejecutar limpieza',
      'Execution Mode': 'Modo de ejecución',
      'Exit': 'Salida',
      'Failed': 'Fallido',
      'Failed Jobs & Process Diagnoses': 'Trabajos fallidos y diagnósticos de procesos',
      'Failure Diagnosis': 'Diagnóstico de fallos',
      'Finished': 'Finalizado',
      'Goal': 'Objetivo',
      'Hancho Dashboard': 'Panel de Hancho',
      'Hierarchical Memory Storage (L1 Runs / L2 Notes / L3 Knowledge)': 'Memoria jerárquica (ejecuciones L1 / notas L2 / conocimiento L3)',
      'ID': 'ID',
      'Inputs': 'Entradas',
      'Intent': 'Intención',
      'Job / Queue': 'Trabajo / cola',
      'Job Details': 'Detalles del trabajo',
      'Job Key / ID': 'Clave / ID del trabajo',
      'Jobs': 'Trabajos',
      'Kill All': 'Detener todos',
      'L1 Runs': 'Ejecuciones L1',
      'L2 Notes': 'Notas L2',
      'L3 Knowledge': 'Conocimiento L3',
      'Last 24 Hours': 'Últimas 24 horas',
      'Last run': 'Última ejecución',
      'Last run details': 'Detalles de la última ejecución',
      'Layer': 'Capa',
      'Loading memory records...': 'Cargando registros de memoria...',
      'Loading output...': 'Cargando salida...',
      'Loading processes...': 'Cargando procesos...',
      'Loading routines...': 'Cargando rutinas...',
      'Loading task queue...': 'Cargando cola de tareas...',
      'Memory': 'Memoria',
      'Memory Record': 'Registro de memoria',
      'Model': 'Modelo',
      'Monitor': 'Monitor',
      'Name': 'Nombre',
      'Name / Command': 'Nombre / comando',
      'No failure statistics available.': 'No hay estadísticas de fallos.',
      'No failures currently detected. Everything is running smoothly!': 'No se detectan fallos. ¡Todo funciona bien!',
      'No usage data recorded.': 'No hay datos de uso registrados.',
      'Normal': 'Normal',
      'Outcome': 'Resultado',
      'Output Tokens': 'Tokens de salida',
      'PID': 'PID',
      'PTY (pseudo-terminal)': 'PTY (pseudoterminal)',
      'Pause Queue': 'Pausar cola',
      'Pending': 'Pendiente',
      'Pipe (standard I/O)': 'Tubería (E/S estándar)',
      'Process / Job Name': 'Proceso / nombre del trabajo',
      'Process Terminal & Logs': 'Terminal y registros del proceso',
      'Processes': 'Procesos',
      'Progress': 'Progreso',
      'Prompt Tokens': 'Tokens de entrada',
      'Prune L1 Records': 'Limpiar registros L1',
      'Prune L1 Run Records': 'Limpiar registros de ejecución L1',
      'Purge Finished': 'Purgar terminados',
      'Queue': 'Cola',
      'Queue Failure Classification (Why Did It Fail?)': 'Clasificación de fallos de la cola (¿por qué falló?)',
      'Quota Status': 'Estado de la cuota',
      'Reconnect Now': 'Reconectar ahora',
      'Record ID': 'ID del registro',
      'Refresh': 'Actualizar',
      'Refresh Diagnosis': 'Actualizar diagnóstico',
      'Refresh state': 'Actualizar estado',
      'Reject': 'Rechazar',
      'Repeated Failures': 'Fallos repetidos',
      'Reset IDs': 'Reiniciar IDs',
      'Resource Locks': 'Bloqueos de recursos',
      'Resume Queue': 'Reanudar cola',
      'Retention (days)': 'Retención (días)',
      'Retry': 'Reintentar',
      'Routines': 'Rutinas',
      'Running': 'En ejecución',
      'Save Info': 'Guardar información',
      'Select a routine to see its node results.': 'Selecciona una rutina para ver los resultados de sus nodos.',
      'Select a running agent to view events.': 'Selecciona un agente en ejecución para ver sus eventos.',
      'Send': 'Enviar',
      'Start New Process': 'Iniciar nuevo proceso',
      'Start Process': 'Iniciar proceso',
      'Started': 'Iniciado',
      'State': 'Estado',
      'Succeeded': 'Correcto',
      'Summary': 'Resumen',
      'Summary / Key Insights': 'Resumen / puntos clave',
      'Supervisor Notes': 'Notas del supervisor',
      'Task': 'Tarea',
      'Task Details': 'Detalles de la tarea',
      'Task ID': 'ID de tarea',
      'Task Queue': 'Cola de tareas',
      'Tasks': 'Tareas',
      'Template': 'Plantilla',
      'Thinking': 'Razonamiento',
      'Timeout (seconds)': 'Tiempo límite (segundos)',
      'Title': 'Título',
      'Tokens': 'Tokens',
      'Tools': 'Herramientas',
      'Total Jobs': 'Trabajos totales',
      'Total Storage': 'Almacenamiento total',
      'Total Tokens': 'Tokens totales',
      'Updated': 'Actualizado',
      'Updated At': 'Actualizado el',
      'Usage & Cost Analytics': 'Análisis de uso y coste',
      'Waiting': 'En espera',
      'Waiting For': 'Esperando',
      'Working Directory': 'Directorio de trabajo',
      'Intent': 'Intención'
    }
  };

  function browserUiLang() {
    const langs = (navigator.languages && navigator.languages.length) ? navigator.languages : [navigator.language || 'en'];
    for (const l of langs) {
      const code = String(l || '').slice(0, 2).toLowerCase();
      if (UI_LANGS.includes(code)) return code;
    }
    return 'en';
  }

  function loadUiLang() {
    let saved = '';
    try { saved = localStorage.getItem(UI_LANG_KEY) || ''; } catch (e) {}
    return UI_LANGS.includes(saved) ? saved : browserUiLang();
  }

  function T(text) {
    const dict = uiState.lang === 'en' ? null : I18N[uiState.lang];
    return (dict && dict[text]) || text;
  }

  // The fixed string for orig (untrimmed). A trailing colon is kept, so "Task ID:" uses "Task ID".
  function translateFixed(orig) {
    const en = orig.trim();
    if (!en || uiState.lang === 'en') return orig;
    let base = en;
    let colon = '';
    if (base.endsWith(':')) {
      base = base.slice(0, -1).trim();
      colon = ':';
    }
    const out = T(base);
    if (out === base) return orig;
    return orig.replace(en, () => out + colon);
  }

  const I18N_ATTRS = ['placeholder', 'title', 'aria-label'];
  const i18nTextRecs = new WeakMap();
  const i18nAttrRecs = new WeakMap();
  let fixedI18nActive = false;
  let fixedI18nFrame = 0;

  // Walks the page and sets each text node and label attribute to the current language.
  // Each node keeps its English original, so switching back to English restores it.
  function applyFixedI18n() {
    const root = document.body;
)JS_11";
const char kAppJs_12[] = R"JS_12(    if (!root) return;
    const walker = document.createTreeWalker(root, NodeFilter.SHOW_TEXT, {
      acceptNode(node) {
        const p = node.parentNode;
        return (p && /^(SCRIPT|STYLE|PRE|TEXTAREA|CODE)$/.test(p.nodeName)) ? NodeFilter.FILTER_REJECT : NodeFilter.FILTER_ACCEPT;
      }
    });
    const nodes = [];
    while (walker.nextNode()) nodes.push(walker.currentNode);
    for (const node of nodes) {
      const current = node.nodeValue;
      const rec = i18nTextRecs.get(node);
      const orig = (rec && rec.set === current) ? rec.orig : current;
      const next = translateFixed(orig);
      if (next !== current) node.nodeValue = next;
      i18nTextRecs.set(node, { orig: orig, set: next });
    }
    for (const el of root.querySelectorAll('[placeholder],[title],[aria-label]')) {
      let recs = i18nAttrRecs.get(el);
      if (!recs) {
        recs = {};
        i18nAttrRecs.set(el, recs);
      }
      for (const attr of I18N_ATTRS) {
        if (!el.hasAttribute(attr)) continue;
        const cur = el.getAttribute(attr);
        const r = recs[attr];
        const orig = (r && r.set === cur) ? r.orig : cur;
        const next = translateFixed(orig);
        if (next !== cur) el.setAttribute(attr, next);
        recs[attr] = { orig: orig, set: next };
      }
    }
    fixedI18nActive = uiState.lang !== 'en';
  }

  // Re-applies the fixed strings after the page re-renders (throttled to one pass per frame).
  function scheduleFixedI18n() {
    if (!fixedI18nActive) return;
    if (fixedI18nFrame) return;
    fixedI18nFrame = requestAnimationFrame(function() {
      fixedI18nFrame = 0;
      applyFixedI18n();
    });
  }

  function startFixedI18nObserver() {
    if (!window.MutationObserver) return;
    const observer = new MutationObserver(scheduleFixedI18n);
    observer.observe(document.body, {
      childList: true,
      subtree: true,
      characterData: true,
      attributes: true,
      attributeFilter: I18N_ATTRS
    });
  }

  function buildUiLangSelect() {
    const sel = document.createElement('select');
    sel.id = 'select-ui-lang';
    sel.className = 'ui-lang-select';
    sel.title = 'Display language';
    sel.setAttribute('aria-label', 'Display language');
    for (const [code, label] of [['en', 'English'], ['ja', '日本語'], ['es', 'Español']]) {
      const opt = document.createElement('option');
      opt.value = code;
      opt.textContent = label;
      sel.appendChild(opt);
    }
    sel.value = uiState.lang;
    sel.addEventListener('change', function() { setUiLang(this.value); });
    if (elStatusDot && elStatusDot.parentElement) {
      elStatusDot.parentElement.insertBefore(sel, elStatusDot);
    }
    uiLangSelect = sel;
  }

  function setUiLang(lang) {
    if (!UI_LANGS.includes(lang) || lang === uiState.lang) return;
    uiState.lang = lang;
    try { localStorage.setItem(UI_LANG_KEY, lang); } catch (e) {}
    document.documentElement.lang = lang;
    if (uiLangSelect) uiLangSelect.value = lang;
    applyFixedI18n();
    refreshTranslatedViews();
  }

  // Free text: translate_get through the core, one batch per language and field.
  const TR_BATCH = 50;
  const TR_RETRY_MS = 3000;
  const TR_MAX_ATTEMPTS = 20;
  const trCache = new Map();    // lang + text -> the text to show (a translation, or the original)
  const trPending = new Map();  // key -> entry waiting to be sent
  const trWaiting = new Map();  // key -> entry sent and not answered yet
  let trFlushTimer = null;

  function trKey(lang, text) {
    return lang + '\u001f' + text;
  }

  // The text to show for a free-text value. English shows the original. Otherwise a cached
  // translation is shown; a missing one is queued and the original is shown until it arrives.
  function D(text, field) {
    if (typeof text !== 'string' || text === '') return text || '';
    const lang = uiState.lang;
    if (lang === 'en' || !/\p{L}/u.test(text)) return text;
    const key = trKey(lang, text);
    const cached = trCache.get(key);
    if (cached !== undefined) return cached;
    if (!trPending.has(key) && !trWaiting.has(key)) {
      trPending.set(key, { key: key, lang: lang, text: text, field: field || '', attempts: 0 });
      scheduleTranslateFlush(300);
    }
    return text;
  }

  function scheduleTranslateFlush(delay) {
    if (trFlushTimer) return;
    trFlushTimer = setTimeout(flushTranslate, delay);
  }

  function releaseWaiting(entry) {
    if (trWaiting.get(entry.key) !== entry) return;
    trWaiting.delete(entry.key);
    trPending.set(entry.key, entry);
    scheduleTranslateFlush(0);
  }

  function flushTranslate() {
    trFlushTimer = null;
    if (!state.connected || trPending.size === 0) return;
    const groups = new Map();
    let count = 0;
    for (const [key, entry] of trPending) {
      if (count >= TR_BATCH) break;
      trPending.delete(key);
      count++;
      if (entry.attempts >= TR_MAX_ATTEMPTS) {
        trCache.set(key, entry.text);
        continue;
      }
      entry.attempts++;
      trWaiting.set(key, entry);
      setTimeout(function() { releaseWaiting(entry); }, TR_RETRY_MS);
      const gk = entry.lang + '\u001f' + entry.field;
      if (!groups.has(gk)) groups.set(gk, { lang: entry.lang, field: entry.field, texts: [] });
      groups.get(gk).texts.push(entry.text);
    }
    for (const g of groups.values()) {
      const req = { action: 'translate_get', lang: g.lang, texts: g.texts };
      if (g.field) req.field = g.field;
      sendWsRequest(req);
    }
    if (trPending.size > 0) scheduleTranslateFlush(50);
  }

  // The core's answer to translate_get: {ok, lang, revision, enabled, items: [{text, translated, state}]}.
  function handleTranslateResponse(data) {
    const lang = data.lang;
    let changed = false;
    for (const item of data.items) {
      if (!item || typeof item.text !== 'string') continue;
      const key = trKey(lang, item.text);
      if (item.state === 'pending' || item.state === 'unavailable') continue;
      trWaiting.delete(key);
      const shown = (item.state === 'translated' && typeof item.translated === 'string' && item.translated)
        ? item.translated : item.text;
      trCache.set(key, shown);
      if (shown !== item.text) changed = true;
    }
    if (changed) refreshTranslatedViews();
  }

  // Redraws the visible tab so that cached translations appear.
  function refreshTranslatedViews() {
    if (state.activeTab === 'taskqueue') renderTaskQueueTable();
    else if (state.activeTab === 'memory') renderMemoryTable();
    scheduleFixedI18n();
  }

  // --- Initialize ---
  uiState.lang = loadUiLang();
  document.documentElement.lang = uiState.lang;
  buildUiLangSelect();
  applyFixedI18n();
  startFixedI18nObserver();
  setupAutoRefresh();
  connectWs();
})();
)JS_12";

static const std::string& GetAppJs() {
    static const std::string s = std::string(kAppJs_0) + kAppJs_1 + kAppJs_2 + kAppJs_3 + kAppJs_4 + kAppJs_5 + kAppJs_6 + kAppJs_7 + kAppJs_8 + kAppJs_9 + kAppJs_10 + kAppJs_11 + kAppJs_12;
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