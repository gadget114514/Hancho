# Hancho Behavior Tree (BT) Design Proposal

> **Related documents:**
> - [Behavior Tree Guide (Japanese)](behavior-tree-ja.md) / [English Guide](behavior-tree.md): comprehensive guide for users and LLMs
> - [Token Consumption Reduction Guide (Japanese)](behavior-tree-token-optimization-ja.md): BT-based token reduction design and best practices
> - [日本語版 (Japanese original)](behavior-tree-plan.md)

## 1. Purpose

The task queue moved "order, dependencies, re-execution" into PC. But **branching decisions** still remain on the LLM side:

- If the build fails → run a fix agent → build again (up to 3 times)
- If the tests pass → next. If they fail → pick the test name from the log and re-run
- If `status` in `result.json` is `needs_review` → ask the LLM for a decision and stop

These decisions are mechanically decidable, yet the LLM is woken every time, reads the result, and decides the next step. The same failures as before the task queue occur: forgetting a decision, mixing it up, or stopping without being woken.

**Purpose:** The LLM writes the "task procedure and branching rules" once as BT data and hands it over. PC executes it and performs condition evaluation, branching, loops and retries. The LLM is woken only when the BT decides that something "cannot be decided mechanically" (`escalate`), and when the BT completes or fails.

## 2. Division of Roles

| What the LLM does | What PC does |
|---|---|
| Write a BT and pass it with `bt_start` | Validate the BT (syntax, limits, ChainGuard) |
| Read a BT stopped at `escalate`, decide, and return a value with `bt_resume` | Execute nodes, evaluate conditions, branch, loop and retry |
| Read the result of a completed or failed BT | Start each action as a task-queue job (timeouts, success checks and resource locks are used as they are) |
| | Persist state (resume after a PC restart) and notify |

## 3. Design Principles

1. **Actions are task-queue jobs.** A BT only decides the order in which jobs start and how to branch. Timeouts, idle detection, success conditions, tree kill, resource locks, logs and the GUI list all use the existing mechanisms. No BT-specific process launch path is created.
2. **Event-driven.** It does not tick every frame like a game BT. It ticks only on job termination, expiry of a wait, `bt_resume`, and PC startup.
3. **It always terminates.** Infinite loops cannot be written. `repeat` / `retry` require a count limit, and the whole BT has limits on executed nodes and elapsed time.
4. **PC does not interpret content.** Conditions are decided only by files, exit codes, log regular expressions, JSON values and variables. If a semantic judgment is needed, it is returned to the LLM with `escalate`.
5. **Success cannot be rewritten.** As with the task queue, MCP cannot mark condition nodes or actions as successful (an answer to `escalate` only returns a value; the conditions written in the BT decide what to do with it).

## 4. Data Format (JSON)

JSON, as with the task queue. A node's kind is given by `type`. `id` is unique within the BT (when omitted, PC assigns `n1`, `n2`, ...).

```json
{
  "id": "feature-x",
  "summary": "Build, test, and fix feature X",
  "intent": "Make feature X build and pass tests without changing public APIs.",
  "taskqueue": "main",
  "cwd": "C:\\work\\proj",
  "limits": { "maxNodeRuns": 200, "maxWallSec": 21600 },
  "worker": { "engine": "agy", "model": "gemini-4-argon-high" },
  "vars": { "target": "Release" },
  "root": {
    "type": "sequence",
    "children": [
      { "type": "retry", "max": 3, "id": "build-loop",
        "child": {
          "type": "fallback",
          "children": [
            { "type": "job", "id": "build",
              "summary": "Build ${target}",
              "command": "cmake --build build --config ${target}",
              "timeoutSec": 1800,
              "capture": { "errFile": { "regex": "^(\\S+\\.cpp)\\(\\d+\\): error", "group": 1 } } },
            { "type": "sequence", "children": [
                { "type": "worker", "id": "fix",
                  "summary": "Fix build error in ${errFile}",
                  "prompt": "Fix the build error in ${errFile}. Log: ${build.logFile}",
                  "timeoutSec": 3600 },
                { "type": "fail" }
            ] }
          ] } },
      { "type": "job", "id": "test", "summary": "Run tests",
        "command": "ctest --test-dir build -C ${target}", "timeoutSec": 1800 },
      { "type": "if",
        "cond": { "type": "jsonValue", "file": "out/result.json", "path": "status", "eq": "needs_review" },
        "then": { "type": "escalate", "id": "review",
                  "question": "Approve the result of feature X?",
                  "guidance": "Approve if only known flaky tests failed; otherwise reject.",
                  "expect": ["approve", "reject"],
                  "include": { "logs": ["test"], "vars": ["target"], "files": ["out/result.json"] },
                  "timeoutSec": 7200,
                  "fallback": { "afterSec": 1800, "engine": "claude", "model": "claude-opus-5-5" } } }
    ]
  }
}
```

What PC does in this example: build → if it fails, pick the erroring file name from the log, start a fix agent → rebuild (up to 3 times) → test → look at `result.json` and return to the LLM only when needed. The LLM is not woken once during that time.

### 4.1 Node Types

**Composite nodes**

| type | Behavior |
|---|---|
| `sequence` | Run children in order. Fails if any fails; succeeds if all succeed. |
| `fallback` | Run children in order. Succeeds if any succeeds; fails if all fail (alias `selector`). |
| `parallel` | Start children at the same time. `success: "all"` (default) / `"any"` / a number. Cancel the rest once the outcome is decided. Each child runs in its own queue derived from the tree id and the branch, so the children actually run in parallel; resource locks can still serialize them. |
| `if` | If `cond` (a condition node) succeeds, run `then`; otherwise `else` (treated as success when omitted). |

**Decorators**

| type | Behavior |
|---|---|
| `retry` | Restart the child from the beginning when it fails. `max` required. |
| `repeat` | Repeat while the child succeeds. `max` required. Succeed and exit when `until` (a condition node) is satisfied. |
| `invert` | Swap success and failure. |
| `forceSuccess` / `forceFailure` | Fix the result. |
| `timeout` | Time limit for the whole child (`sec`). Cancel running jobs and fail when exceeded. |

**Actions (leaves)**

| type | Behavior |
|---|---|
| `job` | Register and start one task-queue job. Fields are the same as a task-queue job (`command`, `summary`, `timeoutSec`, `success`, `idle`, `resources`, `instructionsText`, ...). Succeeds if the job succeeds; fails if it fails or is cancelled. |
| `worker` | Pass a prompt to an LLM worker and run it. No command line is written; the engine is chosen with `engine` (defaults to the BT's `worker.engine`, then PC's default) and `model`. PC builds the command from the engine definition (4.3) and starts it as a task-queue job like `job`. `prompt` / `promptFile`, `summary` and `timeoutSec` are required. `success` / `capture` / `resources` are the same as `job`. The final response goes into `${<id>.result}` and structured results into `${<id>.output.<path>}` (4.3). |
| `wait` | Wait `sec` seconds and succeed. |
| `set` | Put a value into a variable and succeed. |
| `escalate` | Return a decision that cannot be made mechanically to the calling LLM. Stop that branch, notify, and wait for `bt_resume`. The answer goes into the variable `${<id>.answer}`. See 5.6 for details. |
| `fail` / `succeed` | Fail / succeed as-is. |
| `subtree` | Load and run a BT from another file (`file`). Variables are inherited from the parent. |

**Conditions (leaves, evaluated immediately)**

| type | Condition for success |
|---|---|
| `fileExists` | `path` (globs allowed) exists. |
| `fileNewer` | `path` is newer than `than` (a file, or `${node.start}`). |
| `outputMatches` | The end of the log (default 200 lines) of the last attempt of node `node` matches `regex`. |
| `exitCode` | The last exit code of node `node` satisfies `eq` / `in`. |
| `jsonValue` | Read `file` as JSON and the value at `path` satisfies `eq` / `ne` / `in` / `gt` / `lt` / `exists`. |
| `var` | A variable satisfies `eq` / `ne` / `in` / `gt` / `lt` / `exists`. |
| `nodeState` | The last result of node `node` is `succeeded` / `failed`. |

Conditions are evaluated immediately (they do not start a process). To judge by the result of running a command, place a `job` and use its success or `exitCode`.

### 4.2 Variables (Blackboard)

- Initial values are given with `vars`. They are changed by `set`, `capture`, and answers to `escalate`.
- With a `job`'s `capture`, a log regular-expression group (`regex` + `group`) or a JSON file value (`jsonFile` + `path`) is brought into a variable.
- Variables provided by PC: `${<node>.state}`, `${<node>.exitCode}`, `${<node>.logFile}`, `${<node>.jobId}`, `${<node>.runs}`, `${<node>.start}`, `${bt.id}`, `${bt.runs}`.
- `${var}` in `command` is replaced **as a single quoted argument**, like `{summary}` (so a value containing `&` does not become a chain). `instructionsText` / `summary` / `path`-like fields are replaced as-is.
- Referencing an undefined variable makes that node fail (reason `undefinedVar`).

### 4.3 Worker Engine Abstraction

A BT is not tied to a specific LLM CLI. `worker` nodes and an `escalate`'s `fallback` hold only an engine name and a model. Differences between CLIs are absorbed by PC's engine definitions.

**Engine definition (engine registry)**

PC includes definitions for claude / agy / opencode / codex, and they can be overridden or extended with `engines.json` in the data directory (for example, an in-house CLI or a different configuration of the same CLI).

```json
{
  "default": "agy",
  "engines": {
    "agy":      { "command": "agy.exe -p {prompt} --output-format stream-json --model {model} {permissionArgs} --print-timeout {timeoutMin}m",
                  "defaultModel": "gemini-3.8-flash-medium", "stream": "agy",
                  "permissionArgs": "--dangerously-skip-permissions" },
    "claude":   { "command": "claude -p {prompt} --output-format stream-json --verbose --model {model} {permissionArgs}",
                  "stream": "claude", "permissionArgs": "--dangerously-skip-permissions" },
    "opencode": { "command": "opencode.cmd run --format json --model {model} {prompt}", "stream": "opencode" },
    "codex":    { "command": "codex exec --json --model {model} {permissionArgs} {prompt}",
                  "stream": "codex", "permissionArgs": "--dangerously-bypass-approvals-and-sandbox" }
  }
}
```

- `{prompt}`: a **short launch sentence** quoted as a single argument. The prompt body is written to an instructions file (`{instructions}`, the same mechanism as task-queue instructions), and the launch sentence says "read `<path to the instructions>` and execute it. Write the result to `PC_BT_RESULT_FILE`". This avoids per-CLI argument-length limits and quoting differences.
- `{model}`: node `model` → BT `worker.model` → engine `defaultModel`, in that order. If none is present and `command` contains `{model}`, omit that argument entirely (drop the `--model {model}` pair).
- `stream`: how to read output (the `llm_stream` parser). Used to extract the final response, token counts and state.
- `permissionArgs`: arguments for running without confirmation. They live only in the engine definition and cannot be specified from a BT (so an LLM writing a BT cannot widen permissions).
- If `engine` / `model` names an engine that is not defined, validation (`bt_validate` / `bt_start`) fails.

**Receiving results (engine-independent form)**

- Final response text: the last response extracted by the engine's `stream` parser goes into `${<id>.result}` (up to the first 64 KB).
- Structured results: PC passes `PC_BT_RESULT_FILE` (an environment variable) to each worker. If the worker writes JSON there, it can be referenced with `${<id>.output.<path>}` and judged by the condition node `jsonValue` with `"file": "${<id>.resultFile}"`. If a `worker`'s `outputSchema` (a subset of JSON Schema: `required` and `type`) is written, PC checks whether it is satisfied and fails the node (`outputInvalid`) if not.
- Success: by default, "the process exits with code 0 and, if `outputSchema` is present, the result file satisfies it". `success` can add the same conditions as `job`.

**Boundary with the BT core**

- The BT core does not resolve engine names. A `worker` node is passed to the runner as a `BtWorkerSpec` (engine, model, prompt body, summary, timeoutSec, success, resources, outputSchema). Command-line assembly, writing the instructions file and preparing `PC_BT_RESULT_FILE` are done by the runner-side `WorkerEngineRegistry` (`src/worker_engines.{h,cpp}`).
- For validation the core only needs to know "whether that engine name and model are accepted", so the core receives only an interface (`IBtEngineCatalog`) to ask that. Tests use a fake catalog.

## 5. Execution Model

### 5.1 States

Whole BT: `running` / `waitingForLlm` (some branch is stopped at `escalate`) / `succeeded` / `failed` / `cancelled` / `paused`.
Node: `idle` / `running` / `succeeded` / `failed` / `skipped`.

### 5.2 Tick

- It ticks only at these times: at start, when a job reaches a terminal state (subscribing to task-queue events), at a `wait` / `timeout` / `retryDelay` deadline, on `bt_resume`, on `bt_retry`, and at PC startup.
- A tick descends from the root to a `running` leaf. When it reaches a leaf that should newly run, it starts it and stops. It returns the result of a finished leaf to its parent, and the parent decides the next step.
- A "reactive" type that re-evaluates earlier children of a running `sequence` on every tick is not built in the first stage (interrupting when a condition changes is assumed to be covered by `timeout` and `parallel`'s `any`).

### 5.3 Mapping to Jobs

- Each time a `job` node runs, it registers a job `<BT id>.<node id>.<n>` in the BT's `taskqueue` (default `bt-<BT id>`). `n` increments each time it is re-run by `retry` / `repeat`. A `parallel` child uses its own queue named `<taskqueue>.p<hash>` so the branches really run in parallel.
- Give the job side a `bt: { id, node, run }` so `taskqueue_get` (MCP / CLI) can go back to the BT. Do not show it in the GUI (chapter 8).
- The task queue's own re-execution (`maxAttempts`) defaults to 1 for BT jobs. Infrastructure failures (`interrupted` from a PC restart, `deferred`) are not counted, as now. "What to do on failure" is written in the BT's `retry` / `fallback`.
- Rejecting `taskqueue_retry` / `taskqueue_update` on a BT job from outside the BT and telling the caller to use `bt_retry` (so it does not diverge from the BT state). `taskqueue_cancel` is allowed and treated as a node failure.

### 5.4 Limits

- `retry` / `repeat` `max` is required (limit 100).
- Whole BT: `limits.maxNodeRuns` (default 200, limit 10000) job starts, and `limits.maxWallSec` (default 24 hours). Exceeding either fails the BT (reason `limitExceeded`) and notifies.
- Tree depth 32, node count 1000, `subtree` nesting 8.

### 5.5 Failure and Resume

- When a BT fails, `bt_get` lets you trace "which node and why" (the path to the failed node, the last job's attempts and log tail).
- `bt_retry <BT id> [--from <node id>]`: resume from the failed node. By default it returns the last failed leaf to `idle` and leaves parent `retry` counters as they are. `--reset-counters` also resets the counters.
- Definition fixes: `bt_update` can replace the definition of a node that is not `running` (for example, fixing a `command`). The tree shape (adding or removing children) cannot be changed. To change the shape, submit it as a new BT.

### 5.6 escalate: Returning a Decision to the Calling LLM

The decision is made in principle by the calling LLM that submitted the BT. PC does not decide.

**Flow**
1. After `bt_start`, the caller waits with `bt_wait` (a long poll; call it again if it times out).
2. When `escalate` is reached, PC stops only that branch (other parallel branches continue), saves the decision packet (below), and returns from `bt_wait`. It records an `escalated` event and updates `notifyFile`.
3. The caller reads the decision packet and returns `bt_resume` (`node`, `answer`, `rationale`, optional `vars`). Answers not in `expect` are rejected.
4. PC puts the answer into `${<id>.answer}` and resumes that branch. The `escalate` node itself succeeds when it receives an answer. How the answer is used is written in a later `if` or similar.

**Handling context: make the decision packet self-sufficient**

Do not rely on the caller's conversation context (because it may be compacted while waiting, the session may end, or the question may be asked hours later). PC gathers and passes the material needed for the decision.

What the LLM writes for its "future self" when authoring a BT:

| Field | Contents |
|---|---|
| `intent` (whole BT) | The purpose of the whole task. One paragraph. |
| `question` (required) | What decision is needed. |
| `guidance` | Criteria for the decision. |
| `expect` | The list of allowed answers. Free-form when omitted. |
| `include` | Material to attach. `logs` (node ids; last 200 lines of the last attempt), `vars`, `files` (relative to cwd, up to 64 KB per file). |
| `timeoutSec` | If no answer arrives in time, the node fails (reason `escalationTimeout`). Default 24 hours. |
| `fallback` | See below. |

The decision packet PC builds at escalate time (`bt_wait` / `bt_get` return it, and it is saved to `behaviortrees/<id>/escalations/<node>.<n>.json` and `.md`):
- `intent`, `question`, `guidance`, `expect`
- The path from the root to this node, and the results and counts of the nodes along the way (for example, `build-loop 3/3 failed`, `fix 2 runs`)
- The `include` material, saved as a snapshot at that moment (so it does not drift if files change later)
- The current values of variables
- How to answer (how to call `bt_resume`)

**The three kinds of answerer**

| Caller | How to answer |
|---|---|
| Waiting with `bt_wait` | Read the decision packet and answer in the same conversation. It can also use the conversation context, so accuracy is highest. |
| Asleep after ending its turn | Woken by `notifyFile` (Claude Code's Monitor / hooks, etc.), read the decision packet and answer. |
| No longer present | If `fallback` (`afterSec`, `engine`, `model`) is present and there is no answer after `afterSec` seconds, PC starts an LLM worker. It uses the same mechanism as `worker` nodes (4.3) and passes the decision packet's `.md` as the instructions. The worker answers by writing `{ "answer": ..., "rationale": ... }` to `PC_BT_RESULT_FILE` (with the same validation as `bt_resume`). The worker has no conversation context, so it relies only on the decision packet. Its engine may differ from the caller's. |

- An answer must include a short `rationale`, which is kept in the `escalate` record, so it is possible to trace later "why it went this way".
- One answerer per `escalate`. If `bt_resume` arrives first, the `fallback` worker is cancelled.

## 6. API

### MCP
| Tool | Contents |
|---|---|
| `bt_validate` | Validate only, without running (syntax, limits, ChainGuard, static detection of undefined variables) |
| `bt_start` | Register and start a BT. `bt` (JSON) or `file` |
| `bt_get` | Per-node state, counts, variables, running jobs. `compact` gives a one-line tree view |
| `bt_list` | BT list and states |
| `bt_wait` | Long-poll for state changes (same shape as `taskqueue_wait`). Returns when `escalate` is reached (including the decision packet) and on completion |
| `bt_resume` | Answer an `escalate` (`node`, `answer`, `rationale`, optional `vars`) |
| `bt_retry`, `bt_update`, `bt_cancel`, `bt_pause` | As in 5.5 |

### CLI
`hancho-cli bt validate|start|get|list|wait|resume|retry|update|cancel|pause` (same arguments as MCP, `--json`).

### Notifications
- Record `nodeStarted` / `nodeFinished` / `varSet` / `escalated` / `btFinished` in `logs/bt-<id>.events.jsonl`.
- Update `notifyFile` (specifiable per BT) on `escalated` / `btFailed` / `btSucceeded`. The LLM wakes on this (or on `bt_wait`).

## 7. Persistence

- Save the definition (the version at registration) and execution state (node states, counts, variables, node-to-job mapping) in `<PC data directory>/behaviortrees/<id>.json`.
- Format: the BT definition itself plus the whole-BT execution state (`state`, `nodeRuns`, `vars`, `createdAt`, `updatedAt`) and each node's `status`. The debug view (8.1) reads only this file.

```json
{
  "id": "feature-x", "summary": "...", "intent": "...", "taskqueue": "bt-feature-x",
  "state": "running", "nodeRuns": 5, "limits": { "maxNodeRuns": 200 },
  "vars": { "target": "Release", "errFile": "src/a.cpp" },
  "createdAt": "...", "updatedAt": "...",
  "root": { "type": "sequence", "status": { "state": "running" }, "children": [
    { "type": "retry", "id": "build-loop", "max": 3, "status": { "state": "running", "runs": 2 },
      "child": { "type": "job", "id": "build", "summary": "...", "command": "...",
                 "status": { "state": "failed", "runs": 2, "jobs": ["feature-x-1", "feature-x-3"],
                             "detail": "exit 1" } } }
  ] }
}
```

- Node `status`: `state` (`idle` / `running` / `succeeded` / `failed` / `skipped`), `runs` (execution count), `jobs` (job ids started by this node, oldest first), `detail` (reason for the latest result); for `escalate`, `answer` / `rationale`. Nodes that have not run yet have no `status`.
- Save on every state change (write to a temporary file and replace). Load at PC startup, leave `running` jobs to the task queue's `interrupted` handling, and continue ticking based on the result.
- Finished BTs can be removed when old, using the same rules as task-queue purge.

## 8. How Users (Humans) See It

**A BT is a mechanism where an LLM writes it and PC executes it. Users (developers) do not need to be aware of it in normal use, but they should be able to see which BTs are running** (the original policy was to hide it completely, but since the users are developers, it was changed to allow seeing the BT contents).

- The entry points for viewing a BT are the **BT** toolbar button (Ctrl+Shift+B) and the right-click menu "Show in Behavior Tree" on a BT-derived job. They open the view in 8.1.
- Do not add BT-specific columns or terminology to the task list itself.
- Jobs started by a BT are listed in the task list **as ordinary jobs**. Only the job's `summary` is shown; the BT id / node id are not displayed (so they also do not appear in the job id; ids take an ordinary form such as `<short name made from the BT summary>-<serial>`).
- Subsequent nodes that have not started yet are not shown in the task list (they may not run depending on the branch). From the user's perspective, it just looks like "the next task appears in order and flows through".
- `escalate` is a notification to the LLM and shows nothing to the user.
- When the user operates a job by hand (cancel, GUI re-run or force-success), PC accepts it as the result of the corresponding BT node. The user can operate as usual without knowing the BT exists (5.3's "reject `taskqueue_retry` from outside the BT" applies only to MCP / CLI. GUI operations are not rejected and are translated into the equivalent of `bt_retry`).
- How to write a BT is documented in LLM-facing material (MCP tool descriptions, `--help` / `hancho-cli help`, `doc/behavior-tree.md`). Since help is meant for LLMs to read, it describes how to use BTs.

### 8.1 Behavior Tree View

A window that shows the relationship between tasks and BTs in a TreeView (title "Behavior Tree"). It opens with the **BT** toolbar button, **Ctrl+Shift+B**, or "Show in Behavior Tree" on the right-click menu of a BT-derived job.

- Left: TreeView. The top level is the BT (`[state] id "summary" runs n/max updated time`), with the node tree below. Each row is `[state] (cond:/then:/else:/until:) type id` plus type-specific essentials (a `retry`'s count `2/3`, a `job`'s summary and last job id, an `escalate`'s question and answer, a condition node's judgment, `status.detail`). After a job id, show the job's current state in the task queue.
- Row background colors are split by state (running / failed / succeeded use the same theme colors as the task list).
- Right: details of the selected row. The node definition and `status` (JSON without child nodes), followed by the `taskqueue_get` result for each job in `jobs`.
- When opened with a job selected in the task list, select and show the node that started that job.
- Re-read `behaviortrees/*.json` every 2 seconds. Rebuild the tree only when the contents change, and preserve the open/closed state and selection.
- Buttons: reload, copy details, open folder, close. Files that could not be read are shown on the line below.

## 9. Implementation Stages

1. **Core:** `src/behavior_tree.{h,cpp}` (data model, JSON read/write, validation, tick, variable expansion). Make it pure logic independent of the GUI and processes, receiving job and worker start/termination notifications through interfaces. Engine-name validity is asked to `IBtEngineCatalog`.
1b. **Engine registry:** `src/worker_engines.{h,cpp}` (built-in definitions, loading `engines.json`, command-line assembly, result file and `outputSchema` checks). Make it pure logic, and test command assembly and quoting for the four engines in `tests/worker_engines_smoke.cpp`. In `tests/behavior_tree_smoke.cpp`, test all node types, limits, resume, and recovery from persistence using a fake job runner.
2. **Task queue integration:** add a `bt` reference to `TaskQueueJob`, notify the BT on job terminal events, and reject `taskqueue_retry` on BT-derived jobs.
3. **MCP / CLI / IPC:** the tools in chapter 6. Add `doc/behavior-tree.md` / `-ja.md` for LLMs (not referenced from README or features).
4. **GUI:** as in chapter 8, show BT-derived jobs as ordinary jobs and translate GUI operations into BT operations. Do not create a BT-specific screen.
5. **LLM authoring guide:** put BT authoring guidance and examples (build-fix loop, test → fix, waiting for review) in `tools/agents/`, and encourage agents to submit a BT instead of stepping through the order by hand.

## 10. Decisions to Settle

| Item | Proposal (recommended) | Other options |
|---|---|---|
| Action substance | Task-queue jobs (chapter 3, principle 1) | Build a BT-specific executor (would mean rebuilding timeouts, resource locks and the GUI) |
| Format | JSON (aligned with the task queue) | BehaviorTree.CPP-compatible XML, YAML |
| How to return to the LLM | `escalate` node + `bt_wait` / `notifyFile` | Return only when the BT fails (cannot ask mid-branch) |
| Reactive type | Do not build in the first stage | Add `reactiveSequence` from the start |
| BT job `maxAttempts` | Default 1 (retries are written in the BT) | Use the task queue's default (2) |
