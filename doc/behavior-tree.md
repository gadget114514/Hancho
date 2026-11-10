# Behavior Trees for LLM Agents (`bt_*`)

Hancho Behavior Trees (BTs) let an LLM agent declare a complete multi-step workflow—including conditional branching, bounded retry/fix loops, LLM worker invocations, file/JSON/log assertions, and human/supervisor escalation gates—in a single JSON definition. Hancho ticks the tree automatically on job completion and timers without consuming LLM tokens between steps.

> **Related Documents:**
> - [Token Optimization Guide](behavior-tree-token-optimization.md): How BTs eliminate token overhead, `compact: true`, and payload tuning
> - [Architecture & Design Plan](behavior-tree-plan-en.md) ([日本語版](behavior-tree-plan.md)): Architectural design, execution model, and safety limits
> - [日本語ガイド (Japanese Guide)](behavior-tree-ja.md)

---

## 1. When to Use a Behavior Tree vs. `taskqueue_add`

| Scenario | Recommended Tool |
|---|---|
| Single command or a fixed DAG of commands where `dependsOn` (`success` / `done`) is sufficient | `taskqueue_add` |
| Multi-step workflow with `if`/`else` branching, build-fix-rebuild loops (`retry`/`repeat`), `worker` LLM steps, structured output checks (`jsonValue`, `outputMatches`), or mid-flight supervisor decisions (`escalate`) | `bt_start` + `bt_wait` (+ `bt_resume`) |

**Standard lifecycle for a supervisor LLM:**
1. Validate or start the tree with `bt_validate` / `bt_start` (via MCP or `hancho-cli bt start --file <path>`).
2. Call `bt_wait` (`id`, `sinceVersion`, `timeoutMs`, `compact`) in a loop until `state` is terminal (`succeeded`, `failed`, `cancelled`) or `waitingForLlm`. `bt_wait` returns at once on finish or escalation, so a long `timeoutMs` never delays a decision. If `timeoutMs` passes first, the reply is only `{state, version, nodeRuns, timedOut: true}` without the tree. Pass `compact: true` to omit the heavy `tree` JSON object and save tokens on completion. The default is `600000` (10 minutes) for the MCP tool and `60000` for `hancho-cli bt wait`; a negative value waits with no limit. If the MCP client cancels the call (`notifications/cancelled`, for example when its own tool-call timeout fires), the wait stops within about 2 seconds and sends no reply, so a long `timeoutMs` is safe even when the client gives up first.
3. When `state == "waitingForLlm"`, read the returned `escalation` decision packet and call `bt_resume` with `id`, `node`, `answer`, and a required `rationale` (plus optional `vars`).
4. If the tree fails and you fix an external issue or update a non-running node with `bt_update`, resume from the failed node with `bt_retry`.

**Reply sizes.** Every reply is read by an LLM, so only `bt_get` (without `compact`) returns the whole state document:
- `bt_start`, `bt_resume`, `bt_retry`, `bt_update`, `bt_cancel`, `bt_pause`, `bt_get` with `compact: true`, and `bt_wait` with `compact: true` return `{ok, id, state, detail, version, nodeRuns, taskqueue, compact}` plus `escalation` when the tree is `waitingForLlm`. There is no `tree` field.
- `bt_wait` (`compact: false`, when it returns on finish or escalation) adds `tree`, with each variable value, `prompt`, and `instructionsText` longer than 200 bytes cut to a prefix plus `...[truncated, N bytes]`.
- `bt_get` returns `tree` with every variable in full.

---

## 2. Top-Level JSON Schema

```json
{
  "id": "feature-auth",
  "summary": "Build, test, and verify auth module",
  "intent": "Implement token refresh and ensure all unit tests pass without breaking public APIs.",
  "taskqueue": "bt-feature-auth",
  "cwd": "D:/repo",
  "notifyFile": "artifacts/bt-notify.json",
  "limits": {
    "maxNodeRuns": 200,
    "maxWallSec": 86400
  },
  "worker": {
    "engine": "claude",
    "model": "sonnet"
  },
  "vars": {
    "config": "Release",
    "target": "auth_tests"
  },
  "root": { "...": "..." }
}
```

### Top-Level Fields

| Field | Required | Default | Description |
|---|---|---|---|
| `id` | **Yes** | — | Unique non-empty string identifier for the behavior tree. |
| `root` | **Yes** | — | Root node object. |
| `summary` | No | `""` | Short human/LLM summary of the tree. |
| `intent` | No | `""` | 1-paragraph overall goal included in `escalate` decision packets. |
| `taskqueue` | No | `"bt-" + id` | Task queue name where BT jobs are registered. |
| `cwd` | No | `""` | Default working directory for nodes and relative file paths. |
| `notifyFile` | No | `""` | Path (relative to `cwd` or absolute) written atomically on `escalated`, `btFailed`, and `btSucceeded`. |
| `limits.maxNodeRuns` | No | `200` | Max total `job` + `worker` launches across the tree (`1..10000`). |
| `limits.maxWallSec` | No | `86400` | Max wall-clock seconds before failing with `limitExceeded` (`> 0`). |
| `worker.engine` | No | Registry default (`agy`) | Default worker engine for `worker` and `escalate.fallback` nodes. |
| `worker.model` | No | Engine default (`gemini-3.8-flash-medium` for `agy`) | Default model when a node uses the default engine. |
| `vars` | No | `{}` | Initial string key/value variables (booleans/numbers are coerced to strings). |

---

## 3. Node Types Reference

Every node is a JSON object with a required `"type"` string and an optional `"id"` string (must be unique across the tree).
- When `"id"` is omitted, Hancho addresses the node by its JSON path key (`"root"`, `"root.children[0]"`, `"root.children[1].child"`, etc.).
- Nodes can be referenced in APIs (`bt_resume`, `bt_retry`, `bt_update`) and conditions (`exitCode`, `outputMatches`, `nodeState`) by `<nodeId>`, `<pathKey>`, or `<btId>/<nodeKey>`.

### 3.1 Composite & Control Nodes

| `type` | Child Slots | Fields & Behavior |
|---|---|---|
| `sequence` | `"children": [...]` (non-empty array) | Runs children in order. Succeeds when all succeed; fails immediately on the first child failure. |
| `fallback` (or `selector`) | `"children": [...]` (non-empty array) | Runs children in order until one succeeds. Fails only if all children fail. |
| `parallel` | `"children": [...]` (non-empty array) | Starts all children concurrently. Optional `"success"`: `"all"` (default), `"any"`, or integer `1..N` (number of children required to succeed). Cancels remaining running children once the outcome is decided. |
| `if` | `"cond"` (object), `"then"` (object), optional `"else"` (object) | Evaluates `"cond"`. If `"cond"` succeeds, ticks `"then"`; if `"cond"` fails, ticks `"else"` (or succeeds immediately if `"else"` is omitted). |
| `retry` | `"child"` (object) | Required `"max"` (`1..100`). Optional `"delaySec"` (or `"retryDelaySec"`, default `0`). Re-runs `"child"` when it fails, up to `"max"` total runs. Succeeds as soon as `"child"` succeeds. |
| `repeat` | `"child"` (object), optional `"until"` (object) | Required `"max"` (`1..100`). Optional `"delaySec"` (default `0`). Re-runs `"child"` when it succeeds: if `"until"` is present, evaluates `"until"` after each successful `"child"` run and succeeds when `"until"` succeeds (fails if `"max"` runs complete without `"until"` succeeding); if `"until"` is omitted, succeeds after `"max"` successful runs. Fails immediately if `"child"` fails. |
| `invert` | `"child"` (object) | Turns `"child"` `succeeded` into `failed` and `failed` into `succeeded`. |
| `forceSuccess` | `"child"` (object) | Runs `"child"` and always finishes as `succeeded` once `"child"` finishes. |
| `forceFailure` | `"child"` (object) | Runs `"child"` and always finishes as `failed` once `"child"` finishes. |
| `timeout` | `"child"` (object) | Required positive integer `"sec"`. Cancels `"child"` and fails with `detail: "timeout"` if `"child"` does not finish within `"sec"` seconds. |

*(Note: `"subtree"` is reserved in the plan and currently rejected at validation time with `"subtree is not supported yet"`.)*

### 3.2 Leaf Action Nodes

| `type` | Required Fields | Optional Fields & Behavior |
|---|---|---|
| `job` | `"command"`, `"summary"`, `"timeoutSec"` (`> 0`) | Optional: `"estimateSec"` (expected duration for the ETA display; the timeout is still enforced), `"cwd"`, `"env"` (object), `"idle"` (`{outputSec, cpuSec}`), `"success"` (`{exitCode, filesNewer, outputRegex}`), `"resources"` (`{write:[...], read:[...]}`), `"instructionsText"`, `"instructionsFile"`, `"maxAttempts"` (default `1`), `"capture"`. Launches a task-queue job with ID `<btId>-<seq>`. Note: `ChainGuard` is enforced when the job launches (shell chains `&&`, `||`, `;` are rejected with `launchFailed`). |
| `worker` | `"summary"`, `"timeoutSec"` (`> 0`), and either `"prompt"` or `"promptFile"` | Optional: `"engine"`, `"model"`, `"outputSchema"` (JSON Schema object or string), `"cwd"`, `"env"`, `"idle"`, `"success"`, `"resources"`, `"maxAttempts"`, `"capture"`. Runs an LLM CLI worker via the engine registry. |
| `wait` | `"sec"` (`>= 0`) | Pauses this branch for `"sec"` seconds, then succeeds. |
| `set` | `"var"` (or `"name"`), `"value"` | Expands `${...}` in `"value"`, sets `vars[var]`, emits a `varSet` event, and succeeds immediately. |
| `escalate` | `"question"` | Optional: `"guidance"`, `"expect"` (array of allowed answer strings), `"include"` (`{logs:[nodeIds], vars:[varNames], files:[relPaths], lines:100, maxBytes:16384}`), `"timeoutSec"` (default `86400`), `"fallback"` (`{afterSec, engine, model}`). Pauses the branch, sets tree state to `waitingForLlm`, writes the decision packet, and waits for `bt_resume` (or `fallback`). |
| `fail` | *(none)* | Optional `"detail"` (or `"reason"`, default `"fail node"`). Fails immediately with that detail. |
| `succeed` | *(none)* | Succeeds immediately. |

### 3.3 Condition Leaf Nodes

Condition leaves complete in the same tick they are evaluated (`succeeded` when true, `failed` when false). Comparison operators supported on `jsonValue` and `var`: `"eq"`, `"ne"`, `"in"` (array), `"gt"`, `"lt"`, `"exists"` (bool).

| `type` | Required Fields | Behavior |
|---|---|---|
| `fileExists` | `"path"` | Succeeds if the file (resolved against `cwd`) exists on disk. |
| `fileNewer` | `"path"`, `"than"` | Succeeds if `"path"` is newer than `"than"`, where `"than"` is either another file path or an ISO-8601 timestamp such as `"${build.start}"`. |
| `outputMatches` | `"node"`, `"regex"` | Optional `"lines"` (default `200`). Succeeds if `std::regex_search` matches the last `"lines"` lines of `"node"`'s log file. |
| `exitCode` | `"node"`, and one of `"eq"`, `"ne"`, `"in"` | Compares the last exit code of `"node"`. |
| `jsonValue` | `"file"`, `"path"`, and a comparison op (`eq`/`ne`/`in`/`gt`/`lt`/`exists`) | Parses `"file"` as JSON, navigates dotted/indexed `"path"` (e.g. `"summary.failed"`, `"items[0].name"`), and applies the comparison operator. |
| `var` | `"name"` (or `"var"`), and a comparison op (`eq`/`ne`/`in`/`gt`/`lt`/`exists`) | Compares `vars[name]` (supports numeric `gt`/`lt` when both sides parse as numbers). |
| `nodeState` | `"node"`, and `"eq"` (or `"state"` / `"ne"` / `"in"`) | Compares the current state (`idle`, `running`, `succeeded`, `failed`, `skipped`) of `"node"`. |

---

## 4. Variables, Quoting, and `capture`

### 4.1 `${...}` Expansion and Quoting
- Any string field in a running node (`command`, `summary`, `cwd`, `env` values, `prompt`, `instructionsText`, `set.value`, `fail.detail`, `escalate.question`/`guidance`, condition paths/operands) expands `${name}`.
- **Undefined variables**: Checked dynamically when a node executes. Referencing an undefined `${name}` immediately fails that node with `detail: "undefinedVar: <name>"`.
- **Automatic quoting in `command`**: In a `job`'s `"command"`, each `${...}` placeholder is quoted as a single Windows command-line argument (`QuoteCommandArg`), preventing spaces or special characters in variable values from splitting arguments. In all other fields (`prompt`, `summary`, `set.value`, conditions), `${...}` is substituted verbatim without extra quotes.

### 4.2 Engine-Populated Node Variables
When a node with key `<node>` runs or finishes, Hancho populates:

| Variable | Populated By | Value |
|---|---|---|
| `${<node>.start}` | `job`, `worker` | ISO-8601 UTC timestamp when the attempt started (use with `fileNewer`). |
| `${<node>.exitCode}` | `job`, `worker` | Process exit code as a string (e.g. `"0"`, `"1"`). |
| `${<node>.logFile}` | `job`, `worker` | Path to the attempt's log file. |
| `${<node>.resultFile}` | `worker` | Path to `PC_BT_RESULT_FILE` (`<dataDir>/behaviortrees/<btId>/results/<jobId>.json`). |
| `${<node>.result}` | `worker` | Worker's final response text (or log tail fallback). |
| `${<node>.output}` | `worker` | Raw JSON text written to `PC_BT_RESULT_FILE`. |
| `${<node>.output.<path>}` | `worker` | Any nested field or array element from `PC_BT_RESULT_FILE` (e.g. `${w1.output.target}`, `${w1.output.files[0]}`). |
| `${<node>.answer}` | `escalate` | Answer supplied via `bt_resume` or the escalate fallback worker. |

### 4.3 `capture` Rules on `job` and `worker` Nodes
A `job` or `worker` node can extract variables into `vars` when the attempt finishes:
```json
"capture": {
  "errFile": { "regex": "ERROR in ([^:]+):", "group": 1 },
  "failCount": { "jsonFile": "build/report.json", "path": "summary.failed" }
}
```
- `regex`: Searches the job's log tail and stores capture group `group` (default `1`).
- `jsonFile` + `path`: Reads the JSON file (relative to `cwd`) and extracts the value at `path`.

---

## 5. `worker` Nodes and Worker Engines

### 5.1 Built-in Engines and `<dataDir>/engines.json`
Hancho includes 4 built-in worker engines (`agy` [default, `defaultModel`: `"gemini-3.8-flash-medium"`], `claude`, `opencode`, `codex`). You can override them or add custom engines in `<dataDir>/engines.json`:
```json
{
  "default": "agy",
  "engines": {
    "agy": {
      "command": "agy.exe -p {prompt} --output-format stream-json --model {model} {permissionArgs} --print-timeout {timeoutMin}m",
      "defaultModel": "gemini-3.8-flash-medium",
      "stream": "agy",
      "permissionArgs": "--dangerously-skip-permissions"
    },
    "claude": {
      "command": "claude -p {prompt} --output-format stream-json --verbose --model {model} {permissionArgs}",
      "defaultModel": "claude-sonnet-4-6",
      "models": ["claude-sonnet-4-6", "claude-opus-4-6"],
      "stream": "claude",
      "permissionArgs": "--dangerously-skip-permissions"
    }
  }
}
```
- **Permission flags (`permissionArgs`) belong in `engines.json`**, never inside a BT node's `prompt`.
- Supported placeholders in `command`: `{prompt}` (quoted via `QuoteCommandArg`), `{model}` (quoted when needed; if empty, a preceding `--model` / `-m` flag is omitted automatically), `{permissionArgs}`, `{timeoutSec}`, and `{timeoutMin}` (`ceil(timeoutSec / 60)`).

### 5.2 Structured Output (`PC_BT_RESULT_FILE` and `outputSchema`)
- Every `worker` job receives the environment variable `PC_BT_RESULT_FILE` pointing to `<dataDir>\behaviortrees\<btId>\results\<jobId>.json`.
- When `"outputSchema"` is set on a `worker` node (supports `type`, `required`, `properties`, `enum`, `items`), Hancho appends the schema and the result file path to the worker's instructions file, and validates the file when the worker exits.
- If the worker exits `0` but `PC_BT_RESULT_FILE` is missing, invalid JSON, or violates `outputSchema`, the node fails with `detail: "outputInvalid: <reason>"` (for example `outputInvalid: output is empty` or `outputInvalid: $.count: ...`).

---

## 6. Limits

| Limit | Default | Allowed Range | On Violation |
|---|---|---|---|
| `retry.max` / `repeat.max` | *(required)* | `1..100` | Rejected by `bt_validate` / `bt_start`. |
| `limits.maxNodeRuns` | `200` | `1..10000` | Tree fails with `detail: "limitExceeded"`. |
| `limits.maxWallSec` | `86400` (24h) | `> 0` | Tree fails with `detail: "limitExceeded"`. |
| Max tree depth | — | `<= 32` | Rejected by `bt_validate` / `bt_start`. |
| Max node count | — | `<= 1000` | Rejected by `bt_validate` / `bt_start`. |

---

## 7. `escalate`, `bt_wait`, and `bt_resume`

When execution reaches an `escalate` node:
1. Only that branch pauses; sibling branches under a `parallel` node continue running.
2. Hancho snapshots the decision context and writes `<dataDir>/behaviortrees/<id>/escalations/<node>.<run>.json` and `.md`.
3. The decision packet includes `btId`, `node`, `run`, `intent`, `question`, `guidance`, `expect`, `path` (root-to-node trail with states/runs), `vars` (every variable, values over 200 bytes truncated), `include` snapshots (`logs` up to 200 lines per node, `vars`, `files` up to 64 KB per file), and `howToResume`.
4. `bt_wait` returns immediately with `"state": "waitingForLlm"` and `"escalation": { ... }`.
5. Call `bt_resume` with `id`, `node`, `answer`, `rationale` (**required** non-empty string), and optional `vars` object. If `expect` is non-empty, `answer` must match one of its entries.
6. Optional `"fallback"`: `{"afterSec": 300, "engine": "claude", "model": "sonnet"}`. If no `bt_resume` arrives within `afterSec`, Hancho launches a fallback worker with the `.md` decision packet; the worker writes `{"answer": "...", "rationale": "...", "vars": {...}}` to `PC_BT_RESULT_FILE`. (`fallback.command` is not supported and is rejected at validation.)
7. If `timeoutSec` (default `86400`) elapses without an answer, the node fails with `detail: "escalationTimeout"`.

---

## 8. Retry, Update, Cancel, and Pause

- **`bt_retry`** (`id`, optional `from`, `resetCounters`): Resets the target failed node (or the last failed leaf when `from` is omitted) and its ancestors along the path to `idle`, then ticks the tree. Set `resetCounters: true` to also reset `runs` counters.
- **`bt_update`** (`id`, optional `node`, `bt`/`patch`/`file`): Replaces the definition of nodes that are **not `running`**. The tree shape (`type`, `id`, child slots, and `children` count) must remain identical so persisted runtime state stays valid.
- **`bt_cancel`** (`id`): Marks the tree `cancelled` and cancels all currently running BT jobs in the task queue.
- **`bt_pause`** (`id`, `paused`): Pauses or resumes the tree (`paused` <-> `running`) and pauses/resumes its task queue.
- **`bt tasks`** (`id`): Lists every task-queue job of the tree across its parallel branch queues. `hancho-cli taskqueue list --bt <id>` does the same from the CLI.
- **Task-queue protection**: Calling `taskqueue_retry` or `taskqueue_update` via MCP/CLI on a BT-owned job is rejected with `"this job is managed by a behavior tree; use bt_retry"`.

---

## 9. Complete Examples

### Example 1: Build-Fix Loop with a `worker` Node
```json
{
  "id": "build-fix-loop",
  "summary": "Build Release and fix compiler errors with a worker",
  "cwd": "D:/repo",
  "limits": { "maxNodeRuns": 20, "maxWallSec": 3600 },
  "worker": { "engine": "claude", "model": "sonnet" },
  "root": {
    "type": "retry",
    "id": "loop",
    "max": 3,
    "child": {
      "type": "sequence",
      "children": [
        {
          "type": "fallback",
          "children": [
            {
              "type": "job",
              "id": "build",
              "summary": "Build Release target",
              "command": "cmake --build build --config Release",
              "timeoutSec": 600
            },
            {
              "type": "sequence",
              "children": [
                {
                  "type": "worker",
                  "id": "fixer",
                  "summary": "Fix build failure from ${build.logFile}",
                  "prompt": "Inspect build log ${build.logFile}, fix the compile error, and write summary JSON to PC_BT_RESULT_FILE.",
                  "timeoutSec": 900,
                  "outputSchema": {
                    "type": "object",
                    "required": ["fixedFile"],
                    "properties": { "fixedFile": { "type": "string" } }
                  }
                },
                { "type": "fail", "detail": "trigger retry to re-run build after fix in ${fixer.output.fixedFile}" }
              ]
            }
          ]
        }
      ]
    }
  }
}
```

### Example 2: Test -> Fix -> Retest
```json
{
  "id": "test-fix-retest",
  "summary": "Run smoke tests, fix if failing, and re-verify",
  "cwd": "D:/repo",
  "root": {
    "type": "fallback",
    "children": [
      {
        "type": "job",
        "id": "initial_test",
        "summary": "Run smoke test suite",
        "command": "ctest --test-dir build -C Release --output-on-failure",
        "timeoutSec": 300
      },
      {
        "type": "sequence",
        "children": [
          {
            "type": "worker",
            "id": "diagnose_and_fix",
            "summary": "Diagnose test failure and patch code",
            "prompt": "Tests failed (exit ${initial_test.exitCode}, log ${initial_test.logFile}). Fix the bug without changing test expectations.",
            "timeoutSec": 900
          },
          {
            "type": "job",
            "id": "rebuild",
            "summary": "Rebuild after fix",
            "command": "cmake --build build --config Release",
            "timeoutSec": 600
          },
          {
            "type": "job",
            "id": "retest",
            "summary": "Re-run smoke test suite",
            "command": "ctest --test-dir build -C Release --output-on-failure",
            "timeoutSec": 300,
            "success": { "exitCode": 0, "outputRegex": "100% tests passed" }
          }
        ]
      }
    ]
  }
}
```

### Example 3: Change Requiring Supervisor Review (`escalate`)
```json
{
  "id": "schema-migration-review",
  "summary": "Generate migration plan and escalate if breaking changes are detected",
  "intent": "Safely apply database migration without dropping active columns in production.",
  "cwd": "D:/repo",
  "root": {
    "type": "sequence",
    "children": [
      {
        "type": "worker",
        "id": "plan_step",
        "summary": "Analyze schema diff and write migration plan",
        "prompt": "Compare schema.sql against prod and write risk analysis JSON to PC_BT_RESULT_FILE.",
        "timeoutSec": 600,
        "outputSchema": {
          "type": "object",
          "required": ["risk", "sqlFile"],
          "properties": {
            "risk": { "type": "string", "enum": ["low", "breaking"] },
            "sqlFile": { "type": "string" }
          }
        }
      },
      {
        "type": "if",
        "cond": {
          "type": "var",
          "name": "plan_step.output.risk",
          "eq": "breaking"
        },
        "then": {
          "type": "sequence",
          "children": [
            {
              "type": "escalate",
              "id": "review_gate",
              "question": "Migration ${plan_step.output.sqlFile} is marked breaking. Approve or abort?",
              "guidance": "Approve only if a backward-compatible view is included in the SQL file.",
              "expect": ["approve", "abort"],
              "include": {
                "logs": ["plan_step"],
                "vars": ["plan_step.output.sqlFile", "plan_step.output.risk"],
                "files": ["${plan_step.output.sqlFile}"]
              },
              "timeoutSec": 3600,
              "fallback": { "afterSec": 600, "engine": "claude", "model": "opus" }
            },
            {
              "type": "if",
              "cond": { "type": "var", "name": "review_gate.answer", "eq": "approve" },
              "then": { "type": "succeed" },
              "else": { "type": "fail", "detail": "Supervisor aborted breaking migration" }
            }
          ]
        }
      },
      {
        "type": "job",
        "id": "apply_migration",
        "summary": "Apply migration ${plan_step.output.sqlFile}",
        "command": "cmd.exe /c tools\\apply_sql.bat ${plan_step.output.sqlFile}",
        "timeoutSec": 300
      }
    ]
  }
}
```

---

## 10. Reading `bt_get`, Compact Output, and Error Details

- **`bt_get` with `compact: true`**:
  - Returns `"compact"`: a one-line-per-node indented tree showing `[state] type id "summary"  job <jobId>  - <detail>`.
  - Omits `"tree"`. Call `bt_get` without `compact` for the full state document (definitions, statuses, and variables in full).
- **Files on disk**:
  - State file: `<dataDir>/behaviortrees/<id>.json`
  - Event log: `<dataDir>/bt-<id>.events.jsonl` (events: `btStarted`, `nodeStarted`, `nodeFinished`, `varSet`, `escalated`, `btSucceeded`, `btFailed`, `btCancelled`)
- **Common `detail` failure codes**:
  - `exit <N>` / `timeout` / `idleTimeout`: Underlying task-queue job failed or timed out.
  - `launchFailed`: Job failed to start (e.g. rejected by `ChainGuard` for using `&&`, `||`, or `;`).
  - `outputInvalid: <reason>`: Worker exited `0`, but `PC_BT_RESULT_FILE` was missing, unparseable, or failed `outputSchema`.
  - `undefinedVar: <name>`: A `${name}` placeholder could not be resolved when the node ran.
  - `escalationTimeout`: `escalate.timeoutSec` expired before `bt_resume` or `fallback` answered.
  - `limitExceeded`: Tree exceeded `limits.maxNodeRuns` or `limits.maxWallSec`.
  - `jobMissing`: On startup recovery, a previously running job ID was not found in the task queue.
