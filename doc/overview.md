# Overview

**[English](overview.md) | [Japanese](overview-ja.md)**

How Hancho fits between a supervisor LLM and worker processes, plus the main features.

## Express

- Open the [Releases](https://github.com/gadget114514/Hancho/releases) page, then download and install the latest `Hancho-vx.y.z-win64-setup.exe` (portable: `Hancho-vx.y.z-win64.zip`)
- In your llm, "find hancho in PATH, then read help to use it" 
- In your llm, "create task and use (opencode|claude|agy|codex) to execute the task via hancho" 
- That's all

LLM will execute "hancho help --json", then the llm will know how to use it.
You use the LLM as frontend supervisor llm, and you can specify other llm as worker.


## Delegating to a worker LLM: `run_worker`

The MCP tool `run_worker` hands one task to a worker LLM CLI, blocks until the worker exits, and returns only its final result. The worker's transcript stays out of the supervisor's context, and the raw stream stays in `stream_file`.

```json
{
  "name": "run_worker",
  "arguments": {
    "cmd": "claude -p \"Summarize the failing tests\" --output-format stream-json --verbose",
    "name": "test-summary",
    "cwd": "D:/work/myproject",
    "timeout_sec": 1800
  }
}
```

| Argument | Meaning |
| :--- | :--- |
| `cmd` (required) | Worker command line, one command only |
| `name`, `cwd` | Dashboard label; working directory |
| `agent_type` | Stream format: `claude`, `agy`, `opencode` or `auto` (default: detected from the command) |
| `timeout_sec`, `idle_timeout_sec` | Kill after this runtime, or after this long with no output (0 = unlimited) |
| `supervisor_goal`, `supervisor_notes` | Text shown on the dashboard |
| `wait_ms` | Max wait in ms (default 0 = until the worker exits) |

The response carries `finished`, `success`, `state`, `exit_code`, `result`, `result_source`, `result_truncated`, `tool_calls`, `tool_errors`, `num_turns`, `permission_denials`, `total_tokens`, `cost_usd`, `log_file` and `stream_file`.

- `result` is capped at 64 KB; `result_truncated` tells you it was cut. If the stream has no result event, `result` holds the last output lines and `result_source` is `output`.
- If `wait_ms` runs out, the response has `finished: false` and the worker's current activity, and the worker keeps running. Continue with `wait_worker` and the returned `id`.
- Your harness does not see this as a native sub-agent. To have it shown as one, define a thin custom agent (Claude Code: `.claude/agents/<name>.md`, cheap model, this tool only) that calls `run_worker` and returns the result. The wrapper costs extra tokens; calling `run_worker` directly is cheapest.
- For several steps with an order, dependencies or retries, use the [task queue](taskqueue.md) instead.

## Suspend the PC after a specific task finishes

After an overnight build or a long agent run, Hancho can put the PC to sleep automatically. It waits until the target job has finished **and every other job and Hancho-managed process is idle**, then starts a grace period.

```powershell
hancho-cli.exe power suspend --after-job final-build --delay-sec 30
hancho-cli.exe power status    # other work still running, seconds left
hancho-cli.exe power cancel    # disarm
```

You can also set `"suspendWhenDone": true, "suspendDelaySec": 30` when registering the job. The MCP tools `power_suspend` / `power_cancel` / `power_status` expose the same feature. Use `--hibernate` for hibernation and `--dry-run` to verify without sleeping.

## Behavior trees: the next-step decisions run inside Hancho

When the next step depends on the result of the previous one (build → fix → rebuild, test → fix → retest, "if this file says X, do Y"), the supervisor LLM used to stay in the loop: wait for a job, read its result, decide, add the next job, and repeat. Hancho now has a built-in behavior tree (BT) engine. The LLM writes the whole plan once as a JSON tree and submits it with `bt_start`; Hancho walks the tree, runs every step as an ordinary task-queue job, and makes the routine decisions itself.

| | Before (LLM drives each step) | With behavior trees |
| :--- | :--- | :--- |
| Who decides the next step | The supervisor LLM, one turn per step | Hancho, from the tree (branches, loops, retries, condition checks) |
| Tokens and turns | Every step costs a turn and context | One `bt_start`; the LLM is called back only for real decisions (`escalate`) or at the end |
| Supervisor availability | Must stay alive and attentive | Can wait on `bt_wait`, sleep, or have its context compacted; the tree keeps running |
| Runaway loops | Depends on the LLM stopping | `retry` / `repeat` need `max`; `maxNodeRuns` and `maxWallSec` cap the whole tree |
| Mechanical checks | The LLM reads logs and exit codes | Built-in conditions: `exitCode`, `fileExists`, `fileNewer`, `outputMatches`, `jsonValue`, `var` |
| Decisions | Lost in the chat history | `escalate` sends a self-contained decision packet (goal, question, guidance, logs, files) and records the answer with its rationale |
| Restarts | The LLM must remember where it was | The tree's state is saved after every change and resumes after a Hancho restart |
| Worker engines | Command lines written by hand per CLI | `worker` steps name an engine (`claude`, `agy`, `opencode`, `codex` or your own in `engines.json`); one tree can mix engines |

What stays the same: each step is still a normal task-queue job with its own timeout, success check, resource locks and logs, and it appears in the task list like any other job. The **BT** toolbar button (Ctrl+Shift+B) opens a view of each tree, showing which node started which task and how far each tree has run. LLMs learn the format from `hancho-cli help --json` and the documentation linked below.

### Behavior Tree Documentation

- **[Behavior Trees Guide](behavior-tree.md)** ([日本語版](behavior-tree-ja.md)): Comprehensive guide covering JSON definition format, node types, variables, worker engines, escalate decision packets, and complete examples.
- **[Token Optimization Guide](behavior-tree-token-optimization.md)** ([日本語版](behavior-tree-token-optimization-ja.md)): Token-saving principles, `compact: true` in `bt_wait`, escalation line/byte limits, preview truncation, and authoring best practices.
- **[Architecture & Design Plan](behavior-tree-plan-en.md)** ([日本語版](behavior-tree-plan.md)): Original design specification, execution model, safety limits, and integration with the task queue and dashboard.

## Stable events and adapters

Hancho writes a versioned event stream next to the raw one. For a worker process, `logs/proc_<id>_<name>_stream.jsonl` keeps the CLI's raw lines and `logs/proc_<id>_<name>.events.jsonl` holds normalized `hancho.event/1` events (`tool.call`, `tool.result`, `usage`, `run.ended`, ...). The schema is published as [schema/hancho.event.v1.json](schema/hancho.event.v1.json).

- `hancho-cli worker stream <id>` returns the normalized events from `.events.jsonl` (`hancho.event/1`).
- MCP `get_worker_stream` returns the same shape (`hancho.event/1`).
- Add a CLI without C++ by dropping `<data-dir>/adapters/<name>.json` (Tier 1: `match`, `rules`, JSON Pointer `map`), or point `exec` at a converter process (Tier 3: raw lines on stdin, `hancho.event` lines on stdout).
- `engines.json` may set `adapter` and `capabilities` (`jsonStream`, `resume`, `reportsUsage`, `reportsSubagents`, `permissionModes`).
- Wrapper scripts can write events directly: pipe-mode workers get `HANCHO_EVENT_PIPE`, and `hancho-cli event emit --kind message --text "..."` writes to it.
- `hancho-cli adapter test <name>` checks recorded inputs against expected `{kind, data}` pairs under `tests/adapters/<name>/`.

**Event schema and legacy removal.** Both `hancho-cli worker stream` and MCP `get_worker_stream` return `hancho.event/1`. The older internal `type` strings were deprecated in v0.9.3 and removed in **v0.11.0**. Requests passing `--schema legacy` (CLI) or `schema: "legacy"` (MCP) are rejected as unsupported. Migration details and the `type` → `kind` mapping are in [LLM-jsonstream-format.md](LLM-jsonstream-format.md#legacy-type-strings-and-migration-to-hanchoevent1).

## Leaving Hancho (graduation)

Hancho is a means to an end. When your harness covers the reasons in [Hancho v3 Design §5.2](design-v3-en.md), you can leave without losing work.

1. `hancho-cli.exe mcp uninstall` removes the Hancho entry from `.mcp.json` / `opencode.json`.
2. `hancho-cli.exe agent uninstall-template` removes `.claude\agents\hancho-runner.md`.
3. The data directory is kept by default. Add `--delete-data-dir` to either command to remove it too.

All Hancho data is plain JSON / JSONL (task queues, behavior trees, stream events, semantic memory) and stays readable without Hancho. Before leaving, check the feature map and replacement conditions in [design-v3-en.md §5.3–5.5](design-v3-en.md); every release reviews that table using the [release checklist](release-checklist.md).
