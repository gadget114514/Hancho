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
| `defaults` | No | — | Values filled into nodes that do not set them explicitly: `worker.idle`, `worker.maxAttempts`, `worker.probe`, `job.idle`. Explicit values (including `0`) always win. See §5.3. |
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
| `foreach` | `"glob"` (string), `"as"` (simple identifier), `"do"` (node template) | Expands `"do"` once per matching file (paths are relative to `cwd`; `*` / `?` work inside one segment, `**` spans segments). Optional `"mode"`: `"sequence"` (default, stop at the first failure), `"all"` (run every child, then fail if any failed), `"parallel"` (one queue per child). Optional `"exclude"` (array of relative glob strings) and `"minMatches"` (integer; fail if fewer matches). Inside `"do"`, use `${<as>.stem}`, `${<as>.name}`, `${<as>.path}`, or `${<as>.index}` (0-based). Children are rebuilt on the next expansion when the matched files change. |
| `if` | `"cond"` (object), `"then"` (object), optional `"else"` (object) | Evaluates `"cond"`. If `"cond"` succeeds, ticks `"then"`; if `"cond"` fails, ticks `"else"` (or succeeds immediately if `"else"` is omitted). |
| `catch` | `"child"` (object), `"handlers"` (non-empty array) | Runs `"child"`; on failure, executes the `do` of the first matching handler (`class` string/array/`"*"`, `cause`, both = AND). `"thenRetry": true` re-runs `"child"` up to `"maxRetries"` (default `1`, `1..10`). `"otherwise"` is `"fail"` (default) or a node object. See §11.3. |
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
| `job` | `"command"`, `"summary"`, `"timeoutSec"` (`> 0`) | Optional: `"estimateSec"` (expected duration for the ETA display; the timeout is still enforced), `"cwd"`, `"env"` (object), `"idle"` (`{outputSec, cpuSec}`), `"success"` (`{exitCode, filesNewer, outputRegex, testReport, contentFile, minBytes, contentRegex, engineResult}`), `"resources"` (`{write:[...], read:[...]}`), `"instructionsText"`, `"instructionsFile"`, `"maxAttempts"` (default `1`), `"capture"`. Launches a task-queue job with ID `<btId>-<node>-<run>` (node id and run number). Note: `ChainGuard` is enforced when the job launches (shell chains `&&`, `||`, `;` are rejected with `launchFailed`). |
| `worker` | `"summary"`, `"timeoutSec"` (`> 0`), and either `"prompt"` or `"promptFile"` | Optional: `"engine"`, `"model"`, `"engines"` (candidate list, mutually exclusive with `engine`/`model`), `"switchEngineOn"` (failure classes that advance to the next candidate), `"probe"` (preflight health check), `"outputSchema"` (JSON Schema object or string), `"cwd"`, `"env"`, `"idle"`, `"success"`, `"resources"`, `"maxAttempts"`, `"capture"`. Runs an LLM CLI worker via the engine registry. See §5.3 and §5.4. |
| `probe` | `"engine"` | Optional: `"model"`, `"timeoutSec"` (default `60`). Sends a minimal prompt with stdin closed; succeeds when the first output arrives and the process exits 0. Failure is always `probe_failed` (with the original class in `detail`). Cached in `<dataDir>/probes.json` (success 10 min, failure 2 min). |
| `wait` | `"sec"` (`>= 0`) | Pauses this branch for `"sec"` seconds, then succeeds. |
| `set` | `"var"` (or `"name"`), `"value"` | Expands `${...}` in `"value"`, sets `vars[var]`, emits a `varSet` event, and succeeds immediately. |
| `escalate` | `"question"` | Optional: `"guidance"`, `"expect"` (array of allowed answer strings), `"include"` (`{logs:[nodeIds], vars:[varNames], files:[relPaths], lines:100, maxBytes:16384}`), `"timeoutSec"` (default `86400`), `"fallback"` (`{afterSec, engine, model}`). Pauses the branch, sets tree state to `waitingForLlm`, writes the decision packet, and waits for `bt_resume` (or `fallback`). The answer `"takeover"` is always accepted (even when absent from `expect`) and records `takenOver: true` plus a `takeover` object on the node while the node itself finishes as `succeeded`; it appears in `bt_get` / `compact` and the dashboard. |
| `fail` | *(none)* | Optional `"detail"` (or `"reason"`, default `"fail node"`). Fails immediately with that detail. |
| `succeed` | *(none)* | Succeeds immediately. |

### 3.3 Condition Leaf Nodes

Condition leaves complete in the same tick they are evaluated (`succeeded` when true, `failed` when false). Comparison operators supported on `jsonValue` and `var`: `"eq"`, `"ne"`, `"in"` (array), `"gt"`, `"lt"`, `"exists"` (bool).

| `type` | Required Fields | Behavior |
|---|---|---|
| `fileExists` | `"path"` | Succeeds if the file (resolved against `cwd`) exists on disk; a simple glob in the final component is allowed (e.g. `*.json`). |
| `fileNewer` | `"path"`, `"than"` | Succeeds if `"path"` is newer than `"than"`, where `"than"` is either another file path or an ISO-8601 timestamp such as `"${build.start}"`. |
| `outputMatches` | `"node"`, `"regex"` | Optional `"lines"` (default `200`). Succeeds if `std::regex_search` matches the last `"lines"` lines of `"node"`'s log file. |
| `exitCode` | `"node"`, and one of `"eq"`, `"ne"`, `"in"` | Compares the last exit code of `"node"`. |
| `failure` | `"node"`, and `"class"` (string or array) and/or `"cause"` (string) | Succeeds when the referenced node failed with a matching failure class and/or `likelyCauses[].id`. Both given = AND. At least one is required. Unknown node ids are validation errors; a node that has not run or succeeded always yields false. See §11.2. |
| `jsonValue` | `"file"`, `"path"`, and a comparison op (`eq`/`ne`/`in`/`gt`/`lt`/`exists`) | Parses `"file"` as JSON, navigates dotted/indexed `"path"` (e.g. `"summary.failed"`, `"items[0].name"`), and applies the comparison operator. |
| `var` | `"name"` (or `"var"`), and a comparison op (`eq`/`ne`/`in`/`gt`/`lt`/`exists`) | Compares `vars[name]` (supports numeric `gt`/`lt` when both sides parse as numbers). |
| `nodeState` | `"node"`, and `"eq"` (or `"state"` / `"ne"` / `"in"`) | Compares the current state (`idle`, `running`, `succeeded`, `failed`, `skipped`) of `"node"`. |

---

## 4. Variables, Quoting, and `capture`

### 4.1 `${...}` Expansion and Quoting
- Any string field in a running node (`command`, `summary`, `cwd`, `env` values, `prompt`, `instructionsText`, `set.value`, `fail.detail`, `escalate.question`/`guidance`, condition paths/operands) expands `${name}`.
- **Undefined variables**: Checked dynamically when a node executes. Referencing an undefined `${name}` immediately fails that node with `detail: "undefinedVar: <name>"`.
- **Automatic quoting in `command`**: In a `job`'s `"command"`, each `${...}` placeholder is quoted as a single Windows command-line argument (`QuoteCommandArg`), preventing spaces or special characters in variable values from splitting arguments. In all other fields (`prompt`, `summary`, `set.value`, conditions), `${...}` is substituted verbatim without extra quotes.
- **List expansion `${foreach:<glob>:<field>:<sep>}`**: expands `<glob>` at launch time and joins `<field>` (`stem`, `name`, `path`, `index`) for every match with `<sep>` (default a single space). Values are emitted as separate command arguments, so automatic quoting is not applied. Unlike a `foreach` node, this expansion has no `exclude` — use a narrower glob if some matches must be skipped.

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
| `${<node>.failureClass}` | `job`, `worker` | Failure class of the last attempt (e.g. `startup_hang`, `idle_stall`, `rate_limited`, `permission_denied`); empty on success. |
| `${<node>.failureSummary}` | `job`, `worker` | One-line failure summary; empty on success. |
| `${<node>.likelyCauses}` | `job`, `worker` | Comma-separated cause ids (e.g. `no_model,waiting_for_input`); empty on success. |
| `${<node>.suggestedAction}` | `job`, `worker` | Suggested action (`retry`, `retry_with_change`, ...); empty on success. |
| `${<node>.retryAt}` | `job`, `worker` | Retry-available time for rate limits; empty when absent. |
| `${<node>.engine}` / `${<node>.model}` | `worker` | Engine/model actually used by the last attempt (including after an engine switch). |

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

### 4.4 `foreach` and `${foreach:...}` Example
```json
{
  "id": "test-suite",
  "root": {
    "type": "sequence",
    "children": [
      {
        "type": "job",
        "id": "build",
        "summary": "Build all targets",
        "command": "cmake --build build --config Release --target Hancho hancho-cli ${foreach:tests/*_smoke.cpp:stem: }",
        "timeoutSec": 3600
      },
      {
        "type": "foreach",
        "id": "smokes",
        "glob": "tests/*_smoke.cpp",
        "as": "t",
        "mode": "all",
        "exclude": ["tests/posix_core_smoke.cpp"],
        "do": {
          "type": "job",
          "id": "test_${t.stem}",
          "command": "build\\Release\\${t.stem}.exe",
          "summary": "Run ${t.stem}",
          "timeoutSec": 900
        }
      }
    ]
  }
}
```
The build command builds every smoke target found by the glob; the `foreach` node runs each resulting exe and skips the excluded one. For a reusable, named version see [Routines](routines.md).

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

### 5.3 `engines`, `switchEngineOn`, `probe`, and `defaults`

A `worker` (or a `job` that launches an LLM) can list pre-approved engine candidates instead of a single `engine`/`model`:

```json
{ "type": "worker", "id": "impl", "prompt": "<worker prompt>", "summary": "<task summary>",
  "timeoutSec": 5400,
  "probe": true,
  "engines": [
    { "engine": "opencode", "model": "<primary-model>" },
    { "engine": "agy", "model": "<backup-model>" }
  ],
  "switchEngineOn": ["startup_hang", "auth_error", "probe_failed"] }
```

- Candidates are tried in order. A failure class listed in `switchEngineOn` advances to the next candidate; other failures (e.g. `test_failed`) do not. The node fails when candidates run out.
- Specifying both `engine`/`model` and `engines` is a parse error; `engines` needs at least one entry.
- Each switch emits an `engineSwitched` event and updates `${<node>.engine}` / `${<node>.model}`. The node state records `engineCandidateIndex` and `engineAttempts` (engine, model, result, failureClass, detail, list, entry).
- The LLM lists are re-read immediately before launch; a candidate that is no longer `approved` + `enabled` is skipped.
- Job commands can use `{engine}` / `{model}` placeholders, e.g. `ww-run.cmd 091-5 -Main -Engine {engine} -Model {model} -Root <root> -JsonStdout` together with `"engine": "opencode", "model": "..."`. A command without placeholders is not switched. `{engine}` / `{model}` are runtime placeholders (unlike the `<...>` placeholders in `bt template`).
- `"probe": true` runs each candidate's probe before the real worker starts. A failed probe ends the node as `probe_failed` without launching production (put `probe_failed` in `switchEngineOn` to try the next candidate).
- `defaults` fills in values a node did not specify: `defaults.worker.idle` (`firstEventSec`, `outputSec`, `cpuSec`), `defaults.worker.maxAttempts`, `defaults.worker.probe`, and `defaults.job.idle`. Explicit values win, including explicit `0`. Worker defaults also apply to LLM-like `job` nodes (`ww-run`, known LLM CLIs, or a node with `engine`/`agentType`); ordinary build jobs use `defaults.job.idle` instead.

### 5.4 LLM lists, permits, and `needs`

Candidate approval lives in `<dataDir>/llms.json` with three independent lists — `worker` (real work), `consult` (escalation fallback answers), `supervisor` (Hancho-launched supervisor) — plus `history`. The `bt worker` `engines` list uses `worker`, an `escalate` `fallback` uses `consult`, and the Hancho-launched supervisor uses `supervisor`. Only `approved` + `enabled` rows are used. Rows added by an LLM through CLI/MCP are `proposed` and unused until a human approves them (dashboard LLM page → `approvedVia: dashboard`, or the quoted user words via `llm approve --user-said` → `approvedVia: chat`). Changes to engine/model/provider return a row to `proposed`; rank changes and disable are safe. All changes are recorded in `history`.

CLI and MCP equivalents:

```powershell
hancho-cli llm list --list worker --json
hancho-cli llm add --list worker --engine opencode --model opencode-go/deepseek-v4.1-flash --reason "primary"
hancho-cli llm approve --list worker --engine opencode --model opencode-go/deepseek-v4.1-flash --user-said "yes, use opencode"
hancho-cli llm remove --list worker --engine opencode --model opencode-go/deepseek-v4.1-flash --reason "retired"
hancho-cli llm enable --list worker --engine opencode --model opencode-go/deepseek-v4.1-flash --reason "back in service"
hancho-cli llm disable --list worker --engine opencode --model opencode-go/deepseek-v4.1-flash --reason "quota exhausted"
hancho-cli worker probe --engine opencode --model opencode-go/deepseek-v4.1-flash --json
hancho-cli worker probe --engine dummy --timeout 5 --no-cache --json
hancho-cli worker health --json
hancho-cli worker health --reset opencode/opencode-go/deepseek-v4.1-flash --json
```

MCP tools: `llm_list` (`list`), `llm_add` (`list`, `engine`, `model`, `reason`, optional `provider`/`rank`/`note`), `llm_remove` (`list`, `engine`, `model`, `reason`), `llm_set_enabled` (`list`, `engine`, `model`, `enabled`, `reason`), `llm_approve` (`list`, `engine`, `model`, `userSaid`), `worker_probe` (`engine`, optional `model`/`timeout`/`no_cache`), `worker_health` (no args to list, or `engine`/`model`/`reset`). `reason` / `userSaid` are required where shown; `no_cache` maps to CLI `--no-cache`.

Worker permissions live in `<dataDir>/permits.json` (`{"permits":[...],"history":[...]}`). A row has `kind` (`read_path`/`write_path`/`command`/`network`), `target` (glob), optional `engines`, `scope` (`global` / `taskqueue:<name>` / `bt:<id>`), and approval fields. Jobs and BT nodes can declare `"needs": [{"kind": "read_path", "target": "..."}]`; unapproved needs produce a `bt validate` warning and a `--strict` error (`permit_not_approved`). A denied worker produces failure class `permission_denied`; after `permissionResumeLimit` (default 2) session resumes, the job is held in `waitingForPermit` with an auto-requested permit and resumes automatically once approved (`onPermissionDenied: "fail"` disables the hold). Approved permits are converted to engine permissions at launch; the user's global config files are never written.

```powershell
hancho-cli permit list --json
hancho-cli permit request --kind read_path --target "D:/repo/tools/**" --reason "read build tools"
hancho-cli permit approve <id> --user-said "yes, allow reading tools"
hancho-cli permit revoke <id> --reason "no longer needed"
```

MCP tools: `permit_list` (`scope`), `permit_request` (`kind`, `target`, `reason`, optional `engine`/`engines`/`scope`), `permit_approve` (`id`, `userSaid`), `permit_revoke` (`id`, `reason`). The dashboard also has an LLM page and a Permits page for approval.

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

- **`bt_retry`** (`id`, optional `from`, `resetCounters`): Resets the target failed node (or the last failed leaf when `from` is omitted) and its ancestors along the path to `idle`, then ticks the tree. `--from <node>` re-runs that node **and the nodes executed after it**: later siblings in a `sequence` / `fallback` / `parallel`, the unchecked branch of an `if`, a `catch` handler or `otherwise`, and an alternative branch (such as an escalation gate) that ran because the earlier branch failed. Previously succeeded siblings before the target are kept and not re-run, and stale values such as `${gate.answer}` from the discarded subtree are cleared. Set `resetCounters: true` to also reset `runs` counters.
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
- **Remaining time and provisional tasks**: `bt_get` reports `pendingEstimateSec` (the estimated work in job/worker leaves on the current branch that have not started yet) and `plannedTasks` (their node keys, queues and estimates). The dashboard lists the same leaves as provisional queue rows (`state: "planned"`, `id: btplan:<btId>-<node>-<run>` using the job id the leaf will get when it starts). They are re-derived from the tree state on every query, so when a retry, fallback, `if` branch or cancel moves the tree on, rows that will not run disappear automatically.
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

---

## 11. Writing Hang-Resilient Behavior Trees

An LLM worker can silently stall for an hour (no events, no output) before a plain `timeoutSec` fires. Write the hang protection into the tree **when you author it**, instead of reacting after a failure. The pieces from §5.3 / §5.4 fit together as follows:

1. `defaults.worker.idle.firstEventSec` catches a worker that never emits its first event; `outputSec` catches a silent worker later.
2. `defaults.worker.probe: true` (or a `probe` leaf) checks each engine before production work starts; a bad engine fails as `probe_failed` instead of wasting an hour.
3. `engines` + `switchEngineOn` move to the next **approved** candidate on `startup_hang` / `auth_error` / `probe_failed`.
4. `catch` + `failure` branch on the failure class/cause and still escalate if everything fails.
5. `{engine}` / `{model}` make ww-card job commands switchable; `needs` declares required permissions up front.

### 11.1 `bt validate` warnings and `--strict`

`bt validate` returns warnings in `warnings[]` without failing. `--strict` (MCP `bt_validate` with `strict: true`) promotes every warning to an `errors[]` entry and makes `ok`/`valid` false. `bt start` also returns `warnings` but still starts the tree.

| `code` | Meaning |
|---|---|
| `no_hang_watch` | worker/LLM job has no `idle.firstEventSec` / `idle.outputSec` and no `defaults` |
| `opencode_without_model` | opencode launched without `model` / `--model` |
| `unguarded_worker` | no `catch` / `fallback` / `retry` above the worker |
| `engines_without_switch` | 2+ candidates but no `switchEngineOn` |
| `switch_without_engines` | `switchEngineOn` but fewer than 2 candidates |
| `engine_not_permitted` | an `engines` / job candidate is absent, disabled or unapproved in its list |
| `unswitchable_command` | LLM job command does not use `{engine}` / `{model}` |
| `escalate_without_takeover` | `expect` has no `takeover` |
| `permit_not_approved` | a declared `needs` entry is not approved |
| `permit_invalid` | a `needs` entry is malformed |

```powershell
hancho-cli bt validate --file plan.json --json          # {"ok":true,"valid":true,"warnings":[...]}
hancho-cli bt validate --file plan.json --strict        # warnings become errors
hancho-cli bt template                                  # list templates
hancho-cli bt template resilient-worker --json > plan.json
```

MCP: `bt_validate` accepts `bt`, `file`, and `strict`; `bt_template` accepts `name` (omit to list). Replace every `<...>` placeholder in a template before validation; `{engine}` / `{model}` are runtime placeholders and are left as-is.

### 11.2 Example: `bt template resilient-worker --json`

Actual output of `build\Release\hancho-cli.exe bt template resilient-worker --json`:

```json
{
  "defaults": {
    "worker": {
      "idle": {
        "firstEventSec": 180,
        "outputSec": 600
      },
      "probe": true
    }
  },
  "id": "resilient-worker",
  "intent": "Run one resilient worker; replace <...> placeholders before validation",
  "root": {
    "child": {
      "engines": [
        {
          "engine": "opencode",
          "model": "<primary-model>"
        },
        {
          "engine": "agy",
          "model": "<backup-model>"
        }
      ],
      "id": "impl",
      "prompt": "Stay inside the project (cwd): do not read or write files outside it. If the task requires one, add it to needs and request a permit instead. <worker prompt>",
      "summary": "<task summary>",
      "switchEngineOn": [
        "startup_hang",
        "auth_error",
        "probe_failed"
      ],
      "timeoutSec": 5400,
      "type": "worker"
    },
    "handlers": [
      {
        "class": [
          "startup_hang",
          "idle_stall"
        ],
        "do": {
          "type": "set",
          "value": "engine-switch-handled",
          "var": "hang"
        }
      },
      {
        "class": "rate_limited",
        "do": {
          "sec": 600,
          "type": "wait"
        },
        "thenRetry": true
      },
      {
        "class": "permission_denied",
        "do": {
          "type": "set",
          "value": "requested",
          "var": "permit"
        },
        "thenRetry": false
      },
      {
        "class": "*",
        "do": {
          "expect": [
            "continue",
            "stop",
            "takeover"
          ],
          "guidance": "The worker failed after all approved engine candidates were tried.",
          "include": {
            "logLines": 100,
            "logs": [
              "impl"
            ]
          },
          "question": "<what should the supervisor decide?>",
          "type": "escalate"
        }
      }
    ],
    "id": "impl_guard",
    "maxRetries": 2,
    "otherwise": "fail",
    "type": "catch"
  },
  "summary": "A worker with hang watch, preflight probe, engine fallback and escalation"
}
```

`defaults.worker.idle` + `probe: true` provide startup monitoring; two `engines` + `switchEngineOn` provide automatic fallback; the `catch` handlers cover hangs, rate limits and permission denials, and the `"*"` handler escalates with `takeover` available.

### 11.3 Authoring checklist

1. Start from `bt template resilient-worker` (or `card-sequence` for a ww-card sequence) and replace `<...>` placeholders.
2. Pick worker candidates from `llm list --list worker`, and escalation-fallback candidates from `llm list --list consult`. If a needed model is missing, propose it with `llm add --list <l> --reason "..."` and get a human to approve it (dashboard LLM page, or chat confirmation recorded with `llm approve --user-said "..."`) **before** relying on it.
3. Run `bt validate --strict` and drive warnings to zero.
4. Optionally preflight with `worker probe`; check shared engine health with `worker health`.
5. Only then run `bt start` and monitor with `bt wait` / `bt get --compact`.

If you did not have time to build this into the tree, the recovery procedure in `skills/hancho/SKILL.md` ("ワーカーが失敗・ハングしたとき") applies after the fact.

### 11.4 `failure` condition and `catch` example

```json
{
  "type": "failure",
  "node": "impl",
  "class": ["startup_hang", "idle_stall"],
  "cause": "no_model"
}
```

```json
{ "type": "catch", "id": "impl_guard",
  "child": { "type": "worker", "id": "impl", "...": "..." },
  "handlers": [
    { "class": ["startup_hang", "idle_stall"], "do": { "type": "set", "var": "fix", "value": "switch-model" }, "thenRetry": true },
    { "class": "rate_limited", "do": { "type": "wait", "sec": 600 } },
    { "class": "permission_denied", "do": { "type": "set", "var": "permit", "value": "requested" } },
    { "class": "*", "do": { "type": "escalate", "question": "...", "expect": ["continue", "stop", "takeover"] } }
  ],
  "otherwise": "fail",
  "maxRetries": 2 }
```

- If `child` succeeds, `catch` succeeds. On failure, handlers are checked top-down and the first match's `do` runs. `class` is a string, an array, or `"*"` (any failure); `cause` matches `likelyCauses[].id`; both given = AND.
- If `do` succeeds with `thenRetry: true`, `child` runs again (bounded by `maxRetries`). On retry-limit exceeded or no match, `otherwise` applies (`"fail"` by default, or a node object).
- The selected handler is recorded as `handler[<index>] class=... cause=...` in the node state, `detail`, and the `nodeFinished` event.
- The `failure` condition requires `node` and at least one of `class` / `cause`. It is false when the target has not run, succeeded, has no diagnostic, or does not match.
- Listing a failed node in an `escalate` node's `include.logs` adds its full diagnostic to `include.diagnostics[<node>]` in the decision packet and to the `Included Failure Diagnostics` Markdown section.
