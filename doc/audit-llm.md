# Audit LLM: Supervisor Monitoring and Closed-Loop Feedback

**[English](audit-llm.md) | [日本語 (Japanese)](audit-llm-ja.md)**

Hancho is the execution and observation layer for autonomous AI coding agents.
While traditional multi-agent orchestration follows a one-way command path ("Human → Supervisor LLM → Worker LLM / Commands"), Hancho introduces the **Audit LLM**: an independent monitoring engine that objectively reviews the Supervisor LLM's orchestration decisions, detects divergences and inefficiencies, and closes the feedback loop.

---

## 1. The 3-Tier Multi-Agent Hierarchy & the Audit LLM

The orchestration structure in Hancho is organized into three operating tiers plus one audit layer:

```
        ┌──────────────────────────────────────────────────────────┐
        │            Human User (Final Authority / Pilot)          │
        └────────────────────────────┬─────────────────────────────┘
                                     │ High-level Goal / Approvals
                                     ▼
   ┌─────────────────────────────────────────────────────────────────────────────┐
   │                   Supervisor LLM (Planner & Director)                       │
   │  - Task planning (`taskqueue_add`, `bt_start`)                              │
   │  - Execution wait (`taskqueue_wait`)                                        │
   │  - Failure recovery (`taskqueue_retry`)                                     │
   └──────────────────┬──────────────────────────────────────▲───────────────────┘
                      │ Job Execution (1 job = 1 command)    │
                      │                                      │ Audit Feedback
                      ▼                                      │ (Timing, Thrashing,
   ┌──────────────────────────────────────┐                  │  Directives)
   │         Worker LLMs / Scripts        │                  │
   │  (`claude -p`, `agy`, builds, tests) │                  │
   └──────────────────┬───────────────────┘                  │
                      │ stdout / exit_code / duration        │
                      ▼                                      │
   ┌─────────────────────────────────────────────────────────┴───────────────────┐
   │                       Hancho Core Management Layer                          │
   │  - ProcessManager, TaskQueueManager, BtManager                              │
   │  - ResourceLock, Token/Stream Tracker                                       │
   │  - Telemetry: actual duration, estimates, retry count, failure diagnostics  │
   └──────────────────────────────────┬──────────────────────────────────────────┘
                                      │ Structured Telemetry JSON
                                      ▼
   ┌─────────────────────────────────────────────────────────────────────────────┐
   │                  Audit LLM (Objective Auditor / Evaluator)                  │
   │  - Timing divergence analysis (actual duration vs estimate)                 │
   │  - Thrashing detection (repeated retries without code changes)              │
   │  - Task design audit (granularity, deadlock risk, command hygiene)          │
   └──────────────────────────────────┬──────────────────────────────────────────┘
                                      │ Actionable Directive Generation
                                      ▼
         ┌────────────────────────────┴────────────────────────────┐
         │                                                         │
   [Route A: Human Guidance]                       [Route B: Machine Injection]
   Hancho Hint Window (pop-up toast)               Embedded in `"audit"` field of
   "💡 [Audit Alert] Supervisor retried            `taskqueue_wait` / `taskqueue_get`
     3 times without modifying code."               or `get_audit_feedback` tool
```

### Roles and Responsibilities
- **Supervisor LLM**:
  - The primary conversation session (Claude Code, OpenCode, Antigravity, etc.).
  - Responsible for decomposing goals into tasks, defining Behavior Trees (BTs), dispatching workers, and fixing errors.
- **Worker LLMs / Scripts**:
  - Individual processes that perform a discrete unit of work (editing a file, running a compilation, executing a test suite).
- **Audit LLM**:
  - Does not execute code or edit files.
  - Objectively audits the **Supervisor's orchestration decisions** using structured telemetry captured by Hancho.
  - Employs a fast, low-cost model (e.g. Gemini Flash, Claude Haiku) to evaluate performance and issue corrective guidance.

---

## 2. The 5 Core Audit Metrics

The Audit LLM audits five critical dimensions from Hancho's telemetry:

### ① Timing Divergence Audit
- **Problem**: Supervisor LLMs lack awareness of local hardware speed and repository size, frequently resulting in wildly optimistic or pessimistic estimates (`estimate_sec`) and timeouts (`timeout_sec`).
- **Audit Action**:
  - Calculates the divergence ratio: `durationSec / estimateSec`.
  - Emits alerts when actual execution exceeds 2.0x or finishes in less than 0.3x of estimate.
  - Returns concrete correction advice: *"Job 'build' took 142s vs estimate 30s (+373%). Adjust remaining build estimates to ~150s and set timeout_sec >= 300s."*

### ② Thrashing & Unmodified Retry Audit
- **Problem**: When a task fails, supervisors frequently fall into retry loops without addressing the root cause, wasting tokens and time.
- **Audit Action**:
  - Inspects file modification timestamps in the workspace between consecutive failures.
  - Detects if `taskqueue_retry` was invoked without any code modifications, flagging it as an unmodified retry loop.

### ③ Command Design & Granularity Audit
- **Problem**: Attempting to chain shell commands (`&&`, `;`) instead of declaring proper dependencies, or decomposing tasks into hundreds of unnecessary micro-jobs.
- **Audit Action**:
  - Monitors ChainGuard rejections, dependency structures, and deadlocks.

### ④ Context Hygiene & Token Efficiency Audit
- **Problem**: Supervisors ingesting thousands of raw log lines instead of utilizing summaries and artifacts, exhausting context windows.
- **Audit Action**:
  - Verifies that the supervisor inspects `taskqueue_diagnose` summaries and tail lines rather than raw streams.

### ⑤ Resource Contention & Lock Safety
- **Problem**: Multiple parallel jobs attempting to mutate overlapping files, causing prolonged `blocked_by` wait states.
- **Audit Action**:
  - Detects resource lock bottlenecks and advises serialization.

---

## 3. Two Feedback Loops

Audit findings are delivered through two complementary channels:

### Loop A: Human-in-the-Loop (Hint Window)
A non-intrusive floating hint window appears at the bottom-right of the Hancho GUI Dashboard:
- Summarizes the current audit finding in plain language.
- Includes a **"Copy Prompt" button** allowing the user to paste the exact corrective instruction directly into the Supervisor's chat prompt.
- Automatically dismisses after a few seconds (configurable).

### Loop B: Fully Autonomous (Direct Machine Injection)
When the Supervisor LLM calls `taskqueue_wait`, `taskqueue_get`, or `bt_wait`, Hancho attaches an `"audit"` object to the JSON response.
The Supervisor receives the Audit LLM's directives as part of its normal tool returns, enabling automated self-correction without human intervention.

---

## 4. Configuration

The Audit LLM and Hint Window are configured via the GUI Settings dialog or in `logs/config.json`:

```json
{
  "audit_llm_enabled": true,
  "audit_llm_engine": "agy",
  "audit_llm_model": "gemini-3.8-flash-medium",
  "audit_timing_divergence_threshold": 2.0,
  "show_supervisor_hints": true,
  "hint_auto_dismiss_sec": 7
}
```
