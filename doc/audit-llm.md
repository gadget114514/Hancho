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

### Loop A: Human-in-the-Loop (Triage)
Triage presents one card per item that needs attention. Hancho prepares the card with information grouped as cause, background, environment, conditions and reason, then provides candidate actions and a deterministic recommendation. The flow is **information → advice → candidates → choose**: Hancho's recommendation is deterministic, while optional LLM advice only chooses among the candidates and explains why; it does not invent or execute actions.

The `triage_mode` setting controls decisions:
- `manual`: a person chooses every action; LLM advice follows `triage_llm_mode`.
- `semi` (default): Hancho may execute a high-confidence, safe recommendation when its guards pass; other cards await a person. LLM advice follows `triage_llm_mode`.
- `auto`: every pending card is sent for LLM advice; Hancho executes only a safe candidate with high/medium confidence and passing guards, otherwise it waits for a person.

In manual/semi mode, `triage_llm_mode` is `uncertain` (default: recommendation confidence is low, a timeout is repeated/unknown, or the previous attempt had the same failure class), `manual` (request advice explicitly), or `always`. Deterministic guards allow only candidates on the card, prohibit automatic unsafe actions, limit automatic retry to once per job and failure class, cap timeout increases at 4× the original timeout and the queue's `maxTimeoutSec`, and block automation for `needsHuman`, low-confidence advice or advice errors. A pending card becomes `resolved` when its condition naturally goes away. Decisions are recorded on the card and appended to a target job's notes so the Supervisor can read them with `taskqueue_get`.

LLM advice runs as a hidden job in the `_triage` task queue, with one advice job running at a time. The card information, available candidates and Hancho recommendation are included in the advice request. See the [Triage design](triage.md) for card fields, candidates, settings and behavior.

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
