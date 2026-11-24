# Behavior Tree Token Optimization Guide

Hancho Behavior Trees (BTs) are designed from the ground up to eliminate the single largest driver of cost and latency in autonomous LLM agents: **step-by-step context inflation and unnecessary LLM roundtrips**.

This document explains the token-saving principles behind BT execution and provides best practices for minimizing token consumption when authoring and monitoring trees.

---

## 1. Core Principles of Token Reduction (Macro Perspective)

### 1. Zero-Token Step-to-Step Orchestration
* **The Traditional Problem:**
  Multi-step agent workflows ("build → inspect log → patch code → rebuild") typically require the supervisor LLM to loop through full transcript context on every step. Context grows quadratically with each roundtrip.
* **The BT Solution:**
  The supervisor LLM submits the complete multi-step workflow in one JSON definition with `bt_start`. Hancho executes sequential nodes, parallel branches, and retries **natively in C++**. No LLM tokens are consumed between routine steps.

### 2. Native Evaluation of Conditions and Loops
* Conditions such as `exitCode`, `fileExists`, `fileNewer`, `outputMatches`, `jsonValue`, and `var` are evaluated immediately inside Hancho.
* The LLM never needs to read raw log files just to confirm whether a compilation succeeded or a target file was generated.
* Fix-and-retry loops (`retry`, `repeat`) run locally without prompting the supervisor.

### 3. Safety Limits Against Runaway Token Consumption
Infinite loops are strictly capped at the native C++ layer:
* `retry.max` / `repeat.max`: Required (`1..100`)
* `limits.maxNodeRuns`: Default 200 runs
* `limits.maxWallSec`: Default 86,400 seconds (24 hours)
Runaway models cannot burn millions of tokens in unbounded retry loops.

---

## 2. API Response Optimizations (Micro Perspective)

All BT API endpoints are tuned to return minimal payloads to supervisor LLMs.

### 1. `bt_wait` with `compact: true`
* **Feature:**
  Passing `"compact": true` to `bt_wait` (CLI: `hancho-cli bt wait --compact`) omits the full multi-KB `tree` JSON definition from the reply upon completion or escalation, returning only the status metadata and a compact one-line-per-node text summary.
* **Impact:**
  Saves **thousands to tens of thousands of tokens** per workflow completion. Full state can be inspected on demand with `bt_get`.

### 2. Lightweight Polling Timeouts
* When `bt_wait` times out before the tree finishes or escalates, it returns only `{ok, id, state, detail, version, nodeRuns, timedOut: true}` without any tree nodes, ensuring polling loops never pay for repeated tree dumps.

### 3. Localized Escalation Decision Packets
When a tree encounters an `escalate` gate, it sends only what the supervisor needs:
* **Log Lines Limit (`lines` / `logLines`):**
  `include.logs` captures only the tail lines of specified nodes, defaulting to 100 lines (and configurable down to custom counts, e.g., `lines: 30`).
* **File Size Limit (`maxBytes` / `fileMaxBytes`):**
  `include.files` truncates included files at 16 KB by default, appending `...[truncated, N bytes]`.
* **Variable Previews (`BtVarPreview`):**
  Variables listed in the global `vars` map are clipped to 200 bytes. Variables whose full content is critical should be requested explicitly in `include.vars`.

### 4. Echo Suppression for Node Prompts and Instructions
* When full node state is retrieved, large static strings such as `prompt` and `instructionsText` are automatically preview-clipped (to 200 bytes). This prevents the supervisor LLM from paying to re-read the prompts it originally wrote.

### 5. Structured Worker Artifacts (`outputSchema`)
* Worker LLM outputs are routed directly to filesystem artifacts via `PC_BT_RESULT_FILE` and verified against `outputSchema`.
* Downstream nodes access values via `${node.output.<field>}` without polluting the supervisor's conversation context with intermediate raw transcripts.

---

## 3. Best Practices Checklist for LLM Agents

1. **Always pass `compact: true` to `bt_wait`:**
   ```json
   { "action": "bt_wait", "id": "my-workflow", "compact": true }
   ```
2. **Use condition nodes instead of LLM log inspection:**
   - Prefer `outputMatches` (regex matching on tail logs) or `jsonValue` rather than prompting a model to read full logs.
3. **Minimize `escalate.include` payloads:**
   - Only include nodes and files directly relevant to the decision.
   - Use `lines: 30` or `maxBytes: 4096` to keep decision packets ultra-compact.
4. **Set workflow-appropriate `limits.maxNodeRuns`:**
   - Specify a tight ceiling (e.g., `maxNodeRuns: 20` for a 3-step retry workflow) to stop failures early.
