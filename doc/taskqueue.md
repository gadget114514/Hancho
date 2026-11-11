# Task Queue: Ordered Jobs, Dependencies and Retries

**[English](taskqueue.md) | [日本語 (Japanese)](taskqueue-ja.md)**

Hancho can own the workflow: you declare jobs and dependencies; it decides what runs when, enforces per-job timeouts, verifies success, and retries automatically.

**One job = one command.** Do not write `A && B && C`. Register three jobs and declare dependencies:

```json
{ "taskqueue": "main", "concurrency": 1,
  "jobs": [
    { "id": "A", "command": "run A", "summary": "Prepare inputs", "timeoutSec": 1800 },
    { "id": "B", "command": "run B", "summary": "Build outputs", "timeoutSec": 3600, "dependsOn": ["A"] },
    { "id": "C", "command": "run C", "summary": "Publish", "timeoutSec": 900,
      "dependsOn": [{ "id": "B", "on": "success" }] }
  ] }
```

```powershell
hancho-cli.exe taskqueue add --file jobs.json --taskqueue main
hancho-cli.exe taskqueue list main
hancho-cli.exe taskqueue graph main
hancho-cli.exe taskqueue wait main --since 0 --timeout-ms 30000
```

**Job fields**

| Field | Meaning |
| :--- | :--- |
| `id`, `command` | Job id (unique across all queues) and exactly one command. |
| `summary` (required) | One line, <=200 chars. Substituted for `{summary}` and exported as `PC_JOB_SUMMARY`. |
| `timeoutSec` (required) | Per-job limit. Never summed. Must be <= the queue `maxTimeoutSec` (default 7200). |
| `estimateSec` (optional) | Expected duration used by the dashboard's remaining/ETA display. It does not change the enforced `timeoutSec`. |
| `dependsOn` | `["A"]` (same as `on: success`) or `[{ "id": "A", "on": "done" }]`. Cross-queue ids are allowed. |
| `idle` | `{ "outputSec": 720, "cpuSec": 720 }`; both must be set (output **and** CPU idle). |
| `maxAttempts`, `retryDelaySec` | Automatic retries (default 2) and delay between attempts. |
| `defer` | `{ "exitCodes": [75], "delaySec": 60, "limit": 60 }` (the default). An attempt that exits with one of `exitCodes` means "nothing ran, try again later" (75 = EX_TEMPFAIL, e.g. `ww-run` refusing an overlapping card): the attempt is recorded as `deferred`, the job goes back to pending and starts again after `delaySec`, without using up `maxAttempts`. After `limit` deferrals (`0` = no limit) the exit code is an ordinary failure. `{ "exitCodes": [] }` turns it off. The graph shows `retry_in_sec`. |
| `success` | `exitCode` (default 0) AND `filesNewer` (exist and newer than the attempt start) AND `outputRegex` (last 200 log lines) AND `testReport` (path to a JUnit or NUnit XML file: newer than the attempt start, at least one test case, none failed; the first failed test names go into the failure detail). |
| `cwd`, `env`, `killExclude`, `priority` | Working directory, extra environment, image names excluded from tree kill, scheduling priority. |
| `resources` | `{ "write": [...], "read": [...] }`: paths/globs (relative to `cwd`; a bare file name `Foo.cs` means `**/Foo.cs`) or `@name` locks. Jobs whose sets overlap (write/write, write/read) never run together, across all queues; read/read may. On by default (Settings checkbox). See [resource-locks.md](resource-locks.md). |
| `instructionsFile` / `instructionsText` | Optional instructions captured into the job and materialized per revision. |
| `memory` | `{ "inject": true, "budgetChars": 1500, "scopes": ["job","file","project"] }` — inject `memory_brief` into the instructions at launch. Off by default. The brief is expanded at `{memory}`, or appended when the placeholder is absent. The file scope comes from `resources` and the previous run's `filesEdited`. The injected ids are recorded as `memory_refs` on the attempt. `includeAgentNotes: false` leaves out L2 notes written by agents. |

**Origin: which session added a job**

Every job records who added it, so jobs from several agent sessions can be told apart. hancho-cli fills it in automatically; the job JSON carries it as `origin`:

| Field | Source |
| :--- | :--- |
| `label` | MCP `taskqueue_add` / `bt_start` `originLabel`, CLI `--origin-label`, or the `PC_ORIGIN_LABEL` environment variable. Optional free text such as `release v2.9.2`. |
| `session` | `PC_ORIGIN_SESSION`, else `CLAUDE_CODE_SESSION_ID`. |
| `host_session` | `CLAUDE_CODE_HOST_SESSION_ID` (the Claude desktop app session). |
| `client` | MCP `clientInfo` name and version, or `hancho-cli-cli`. |
| `cwd`, `pid` | Working folder of the caller; `CLAUDE_PID`, else the process that started hancho-cli. |

Jobs that a behavior tree adds carry the origin of the `bt_start` call. The dashboard shows the origin in the **Origin** column (label, or folder name and the first 8 characters of the session id); hover over it to see every field. `taskqueue list --origin self` (MCP `taskqueue_list` `origin: "self"`) lists only the jobs your own session added; `--origin` also takes a label (whole, case-insensitive), a session or host session id (or its prefix), or `pid:<n>`.

**States**

| State | Meaning |
| :--- | :--- |
| `waiting` | Queued and ready; waiting for a free slot (or the retry delay). |
| `pending` | Not in the run queue yet; dependencies are not satisfied. |
| `blocked` | A success-dependency failed/cancelled. Fix it and `taskqueue_retry` the dependency; blocked dependents are requeued automatically. |
| `running` | An attempt (one managed process) is executing. |
| `succeeded` / `failed` / `cancelled` | Terminal states. Failed jobs auto-retry up to `maxAttempts`. |

**Failure diagnosis and `failure-rules.json`**

Each failed attempt gets a diagnosis (`failure` on the attempt and on `jobAttemptFailed` / `jobFailed` events): `class`, `confidence`, `summary`, `detail`, `evidence`, `lastActivity`, `suggestedAction` and `retryAt`. `hancho-cli taskqueue why <id>` prints it; the GUI, MCP `taskqueue_get` / `taskqueue_wait` and memory stats use the same record.

Rules are ordered and deterministic: the first match wins. In addition to the built-in rules, `<data-dir>/failure-rules.json` can add classes or take precedence over a built-in rule with the same class. External rules run before the built-ins, in file order.

```json
{
  "rules": [
    { "class": "auth_error", "when": { "logRegex": "(?i)(invalid api key|401 unauthorized)" },
      "confidence": "high", "action": "human" },
    { "class": "lint_error", "when": { "jobName": "lint", "exitCode": [2], "logRegex": "(?i)problems? found" },
      "summary": "Lint reported findings", "action": "fix_spec" }
  ]
}
```

Every condition in `when` must hold. Supported conditions: `jobName`, `taskqueue`, `endKind`, `exitCode` (number or array), `logRegex`, `eventKind`, `eventRegex`, `launchFailed`, `cancelled`, `interrupted`. `logRegex` and `eventRegex` accept a leading `(?i)` for case-insensitive matching; an invalid regex simply does not match. `summary`, `detail`, `confidence` (`high` / `medium` / `low`) and `action` are optional. A rule with `logRegex` or `eventRegex` points its evidence at the matched log line or event `seq`; other rules fall back to the last log line or task-queue event.

When the rules leave the failure undecided (`class: "unknown"` or `confidence: "low"`), an existing BT `escalate` node may ask an LLM for a second opinion. Pass it only the evidence packet (attempt facts, the tail of the normalized events and the tail of the log), never the command, environment or instructions. Store the answer in the composite reference column: it is written as `failure.llmGuess` with `confidence: "low"` and `advisory: true`. It never changes `class`, `confidence`, `suggestedAction` or `retryAt`, and automatic retry ignores it. If the LLM is unavailable or returns invalid JSON, the deterministic diagnosis is unchanged. External classes flow into `memory stats` and the dashboard failure-class counts exactly like built-in classes. They can also be named in `retry.on` / `retry.never`.

**ChainGuard:** `start_process`, `taskqueue_add` and `taskqueue_update` reject `&&`, `||`, a single `&`, PowerShell `;`, newline chains, and polling loops (`while` + `Test-Path`, `goto` loops). Splitting commands into jobs is the intended fix. CLI `--allow-chain` bypasses the guard for a single process; MCP never exposes it.

**Events and environment:** each attempt receives `PC_JOB_ID`, `PC_ATTEMPT`, `PC_JOB_SUMMARY`, `PC_JOB_INSTRUCTIONS`, `PC_JOB_INSTRUCTIONS_REV`, plus `PC_PREV_REASON`, `PC_PREV_EXIT`, `PC_PREV_TAIL` on retries. Events are appended to `logs/taskqueue-<name>.events.jsonl`; `notifyFile` is updated for `jobFailed`, `taskqueueStalled` and `taskqueueDrained`.

**GUI:** click the view mode toggle button (leftmost toolbar button) or press **Ctrl+T** to cycle between **Two Rows** (Processes + Tasks), **Processes Only**, and **Tasks Only**. In Two Rows mode, drag the horizontal splitter bar to adjust relative heights. Running jobs are listed first and highlighted; failed rows use a muted red and timed-out rows a muted amber (theme colors).
