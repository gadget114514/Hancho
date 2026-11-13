# Usage 1: Console CLI (`hancho-cli.exe`)

**[English](cli.md) | [日本語 (Japanese)](cli-ja.md)**

The console client `hancho-cli.exe` communicates with the running GUI Dashboard via Named Pipe (`\\.\pipe\Hancho_IPC_v1`). If the dashboard is not yet running, `Hancho.exe` is located in the same directory and launched automatically.

> **Naming:** `Hancho.exe` is the GUI dashboard / MCP server; `hancho-cli.exe` is its console client.

## Linux / macOS: `hancho-cli top` (Interactive Terminal Dashboard)

On POSIX the core runs headless and the CLI talks to a Unix domain socket. `top` provides an interactive terminal dashboard featuring 5 views equivalent to the desktop dashboard (Processes, Queue, Monitor, Why, Memory):

```sh
hancho-cli --data-dir ./ww-data top              # interactive TUI (refreshes every 1000ms)
hancho-cli top --interval 500                    # refresh every 500ms
hancho-cli top --once                            # emit one formatted snapshot to stdout and exit
hancho-cli top --once --json                     # one machine-readable JSON snapshot
```

- **Tabs**: `Tab` / `Shift+Tab` to cycle, or `1`-`5` to switch directly between Processes, Queue, Monitor, Why, and Memory.
- **Row navigation**: `j` / `k` or `Down` / `Up` arrow keys.
- **Job operations**: `s` to stop/kill, `r` to retry, `c` to cancel.
- **Help**: `?` toggles keyboard shortcut reference, `q` or `Ctrl+C` to quit.

See [tui-design.md](tui-design.md) for full design, shortcuts, and POSIX verification checklist. Build instructions are in [build-posix.md](build-posix.md).

## Version (`--version` / `-V` / `version`)

```powershell
hancho-cli.exe --version              # hancho-cli 0.9.1
hancho-cli.exe version --json         # {"ok":true,"program":"hancho-cli","version":"0.9.1"}
Hancho.exe --version   # Hancho 0.9.1
Hancho.exe version --json  # {"ok":true,"program":"Hancho","version":"0.9.1"}
```

The version also appears in the first line of both help outputs, as `version` in `hancho-cli.exe help --json`, and as `serverInfo.version` in the MCP `initialize` response.

## JSON Output (`--json` / `-j`)

By default, `hancho-cli.exe` prints human-friendly text. Add the global `--json` (or `-j`) flag — before or after the command — to receive the raw machine-readable JSON response instead. This is intended for scripts, CI pipelines, and LLM agents:

```powershell
# List all managed processes as JSON
hancho-cli.exe list --json

# The flag can also precede the command
hancho-cli.exe --json logs --id 1 --tail 80

# Short form
hancho-cli.exe wait --id 1 --wait-ms 30000 -j
```

Every response is a single JSON object written to stdout. Successful responses contain `"ok": true` (for example, `list` returns an `ok` flag and a `processes` array); errors contain `"ok": false` plus an `error` message and set the process exit code to `1`. With `--json`, errors are also emitted as JSON on stdout:

```json
{"ok":false,"error":"Process ID not found."}
```

Example `list` response (fields abbreviated):

```json
{"ok":true,"processes":[{"id":1,"name":"agent-task","command":"opencode","mode":"pty","state":"running","pid":12345,"exit_code":0,"elapsed_ms":5230,"tree_procs":3,"memory_bytes":48234496,"log_file":"C:/logs/task42.log","done_file":"C:/logs/task42.done","last_line":"..."}]}
```

The examples below show the default human-friendly output:

```powershell
# Launch the GUI Dashboard (or restore from System Tray if already running)
Hancho.exe gui

# Start a process with ConPTY enabled (use --pty; pipe mode is the default)
hancho-cli.exe start --name "agent-task" --pty --cmd "opencode run \"Fix issue #42\"" --log-file "C:/logs/task42.log" --done-file "C:/logs/task42.done"

# List all managed processes (human-friendly table)
hancho-cli.exe list

# Read the latest 80 lines of clean output from process #1
hancho-cli.exe logs --id 1 --tail 80

# Send input to process #1's ConPTY / stdin
hancho-cli.exe send --id 1 --input "y"

# Wait for process #1 to finish (up to 30 seconds)
hancho-cli.exe wait --id 1 --wait-ms 30000

# Kill a process tree or all running processes
hancho-cli.exe kill --id 1
hancho-cli.exe kill-all

# Clear finished process history
hancho-cli.exe clear
```

By default, `hancho-cli.exe start` uses **pipe mode**, which launches the process directly without ConPTY. Add `--pty` (or `--mode pty`) to enable ConPTY for interactive terminals, or use `--mode console` to launch a separate visible console window. The MCP `start_process` tool follows the same rule: `mode` defaults to `pipe`, and `pty: true` enables ConPTY.

In pipe mode, stdin remains writable by default so `hancho-cli.exe send` can feed input. Non-interactive programs such as `opencode run` may wait forever for stdin EOF; pass `--close-stdin` to close stdin immediately after launch so they can finish:

```powershell
# Keep stdin open for interactive input via hancho-cli.exe send
hancho-cli.exe start --name "pipe-shell" --cmd "cmd.exe"

# Non-interactive program that reads stdin: close it so EOF is delivered
hancho-cli.exe start --name "oc-run" --close-stdin --cmd "opencode run OK" --timeout 90
```

For an interactive TUI such as `opencode` (without `run`), prefer `--pty`. The MCP equivalent of `--close-stdin` is `open_stdin: false`.

While a process is running, the **End Time** column shows an estimated finish time in gray. Pass `--eta "18:30"` (or `--estimate-end`) to set it explicitly; otherwise `--timeout` makes the column show remaining seconds (e.g. `120s left`).

If `--cwd` is omitted, `hancho-cli.exe` uses the directory where it was invoked and resolves it to an absolute path. Hancho sets the child's actual working directory and its `PWD` / `INIT_CWD` environment variables to that path (removing `OLDPWD`), so tools such as opencode that resolve their project directory from `PWD` operate on the intended folder.

**Completion notification:** Hancho writes a `.done` JSON file atomically when a process exits. To block until completion, use `hancho-cli.exe start --wait` (optionally with `--wait-ms` as a maximum), MCP `start_process` with `"wait": true`, or MCP `wait_process`. Without `wait`, `start` returns immediately and callers should poll the `.done` file or call `wait`.

**Claude Code background tasks:** launch a blocking command with `run_in_background`:

```powershell
hancho-cli.exe start --name "task" --cwd C:\ws\Survivors --close-stdin --timeout 3600 --wait --wait-ms 0 --json --cmd C:\path\run_task.cmd
```

`hancho-cli.exe` stays alive until the managed process exits, so Claude Code emits its `<task-notification>` when `hancho-cli.exe` exits. The exit code is `0` on completed, the child's exit code on failure, and `124` if the wait timeout expires. The captured output file contains the final JSON with `state`, `exit_code`, `log_file`, and `done_file`.

**Task queue (ordered/dependent jobs):**

```powershell
# jobs.json: one command per job, each with its own timeout
# [{"id":"A","command":"run A","summary":"Prepare","timeoutSec":1800},
#  {"id":"B","command":"run B","summary":"Build","timeoutSec":3600,"dependsOn":["A"]}]
hancho-cli.exe taskqueue add --file jobs.json --taskqueue main --concurrency 1
hancho-cli.exe taskqueue list main
hancho-cli.exe taskqueue get A
hancho-cli.exe taskqueue graph main
hancho-cli.exe taskqueue wait main --since 0 --timeout-ms 30000
hancho-cli.exe taskqueue retry A

# Only jobs updated within the last 6 hours (--since takes 30m, 6h, 2d)
hancho-cli.exe taskqueue list main --state failed --since 6h

# Name the work these jobs belong to (shown in the dashboard's Origin column),
# then list only the jobs this session added
hancho-cli.exe taskqueue add --file jobs.json --taskqueue main --origin-label "release v2.9.2"
hancho-cli.exe taskqueue list --origin self

# Retry every failed job at once (optionally one queue, and only recent failures)
hancho-cli.exe taskqueue retry --failed
hancho-cli.exe taskqueue retry --failed main --since 6h
```

**Suspend after a specific task and all other work (sleep the PC):**

```powershell
# After this job, and after every other job / managed process is idle, sleep in 30s
hancho-cli.exe power suspend --after-job final-build --delay-sec 30

# Or arm the job at registration time
# {"id":"final-build","command":"...","summary":"...","timeoutSec":600,
#  "suspendWhenDone":true,"suspendDelaySec":30}

hancho-cli.exe power status   # other jobs/processes still running, seconds left
hancho-cli.exe power cancel   # disarm
hancho-cli.exe power suspend --dry-run   # verify without sleeping
```

The job may end as `succeeded`, `failed`, `cancelled` or `blocked`. The core then waits until no other task queue job (`running` / `waiting` / `pending`) and no Hancho-managed process is running. If new work starts during the countdown, the countdown is cancelled and restarted when the machine is idle again. Use `--hibernate` for hibernation and `--dry-run` to verify. When several jobs are armed, all of them must finish. `power_*` actions are local-host only.

**Memory (previous runs, notes and project knowledge):**

```powershell
hancho-cli.exe memory search "LNK2019" --job-name build
hancho-cli.exe memory brief --job-name build --budget-chars 1500
hancho-cli.exe memory get rr_j_7f3a_2
hancho-cli.exe memory approve nt_01JC9Z --by alice
```

`memory search [query] [filters] [--ranker <path>] [--limit <n>]` searches L1 run records, L2 notes, and L3 approved knowledge. Ranking uses BM25-like keyword scoring by default. An external ranker plugin process can be specified in `<data-dir>/config.json` via `"ranker_plugin": "<path>"` or `{"ranker_plugin": {"path": "<path>", "timeoutMs": 5000}}` (or overridden on the command line via `--ranker <path>`). The plugin protocol sends `{"query": ..., "candidates": [{"id": ..., "kind": ..., "text": ..., "createdEpoch": ...}]}` via stdin and receives candidate scores on stdout as a JSON array `[<score>, ...]` or `{"scores": {"<id>": <score>, ...}}` / `{"scores": [<score>, ...]}`. If the plugin fails to start, exits abnormally, times out, or produces malformed output, search does not halt; it falls back cleanly to built-in BM25 ranking and reports a `rankerWarning`. Sample plugins are available under `tests/data/sample_ranker.py` and `tests/data/sample_ranker.bat`.

`memory brief` prints the fixed block (previous runs, common failures, related notes, project knowledge) with an id on every line. Sections are trimmed from the bottom up to fit `--budget-chars` (default 1500), keeping L3 project knowledge to the end. `--no-agent-notes` leaves out L2 notes written by agents. Jobs can receive the same brief automatically: see the `memory` job field in [taskqueue.md](taskqueue.md).

`memory approve <note-id> [--by <name>] [--worker <name>]` promotes an approved L2 note to L3 project knowledge in `memory/knowledge.jsonl`, adding `approvedBy` and `approvedAt` and retiring the note. In addition to human operators (CLI/GUI), specific workers enumerated in `<data-dir>/config.json` under `memory.l3Promoters` (array of names or comma-separated string) are authorized to promote notes using `--by worker:<name>` or `--worker <name>`. Unlisted workers and unauthorized callers are rejected. The file is plain JSONL with one approved fact per line, meant to be committed to git and shared with the team.

`memory prune [--dry-run] [--days <n>]` removes L1 run records (`memory/records/*.jsonl`) from months older than the retention period (default 90 days). If `memory_max_bytes` is configured in `config.json`, it also prunes the oldest L1 files when the memory directory exceeds the size limit. L2 notes and L3 knowledge are never deleted automatically; a warning is issued when usage approaches the limit (>= 80%). Use `--dry-run` to preview candidate files without deleting them. `memory stats` also includes current memory usage and configured limits.

**Query LLM usage statistics (`usage stats`):**

```powershell
# Breakdown by day x engine x model (default)
hancho-cli.exe usage stats

# Aggregate by day, month, engine, or model
hancho-cli.exe usage stats --group-by day
hancho-cli.exe usage stats --group-by month
hancho-cli.exe usage stats --group-by engine
hancho-cli.exe usage stats --group-by model

# Time window filtering
hancho-cli.exe usage stats --since today
hancho-cli.exe usage stats --since 7d
hancho-cli.exe usage stats --since 2026-11-01 --until 2026-11-30

# Output machine-readable JSON
hancho-cli.exe usage stats --group-by day --json
```

`usage stats` reads daily rollup records (`usage/daily/<YYYY-MM>.jsonl`, schema `hancho.usage.day/1`) and sums deltas across the same key, returning token breakdowns (input, output, thinking, cache read/write) and estimated USD costs. Engines that do not report cost (e.g. `agy`) include `costKnown: false`.

**Export LLM usage data (`usage export`):**

```powershell
# Export to CSV (default)
hancho-cli.exe usage export
hancho-cli.exe usage export --format csv --out usage.csv

# Export grouped by month / engine / day
hancho-cli.exe usage export --format csv --group-by month
hancho-cli.exe usage export --format json --group-by engine --out engine_usage.json
```

**Compact daily usage delta logs (`usage compact`):**

```powershell
# Compact all months or a specific month into 1 line per date x engine x model
hancho-cli.exe usage compact
hancho-cli.exe usage compact --month 2026-11
```

Compaction runs automatically upon month rollover, aggregating deltas and atomically replacing the file via a temporary file to keep disk usage under 1 MB for 12 months.

**Register as an MCP server automatically (recommended for LLM agents):**

```powershell
hancho-cli.exe mcp install                     # .mcp.json (Claude Code)
hancho-cli.exe mcp install --client opencode   # opencode.json
hancho-cli.exe mcp install --dry-run --json    # preview without writing
hancho-cli.exe mcp uninstall                   # remove the entry (keeps data)
hancho-cli.exe mcp uninstall --delete-data-dir # remove the entry and the data directory
```

**Install the `hancho-runner` subagent (Claude Code):**

```powershell
hancho-cli.exe agent install-template                  # writes .claude\agents\hancho-runner.md
hancho-cli.exe agent install-template --path C:\proj --dry-run
```

`hancho-runner` runs long builds and tests through Hancho's MCP tools and returns only a short summary. The file is never overwritten.

**Leaving Hancho:** `hancho-cli.exe agent uninstall-template` removes `hancho-runner.md`, and `hancho-cli.exe mcp uninstall` removes the MCP entry. The data directory is kept unless you pass `--delete-data-dir`. All Hancho data is plain JSON / JSONL and remains readable without Hancho. See [design-v3-en.md section 5.3-5.5](design-v3-en.md) for the feature map and [release-checklist.md](release-checklist.md) for the per-release review.

**Exporting data (readable without Hancho):**

```powershell
# Whole task queue in dependency order, as a runnable PowerShell script
hancho-cli.exe export queues main --format powershell --out main.ps1

# Or as a CI workflow skeleton / open data
hancho-cli.exe export queues main --format github-actions --out .github/workflows/main.yml
hancho-cli.exe export queues main --format jsonl

# Behavior tree as a Markdown runbook
hancho-cli.exe export bt my-tree --out runbook.md

# L3 approved project knowledge as Markdown to paste into CLAUDE.md
hancho-cli.exe export knowledge --out project-knowledge.md

# Everything into ./export (queues/, behavior-trees/, knowledge.md)
hancho-cli.exe export
hancho-cli.exe export all --out C:\backup\hancho-export

# See what is available in the data directory
hancho-cli.exe export list
```

`export` writes with no Hancho-specific runtime: queues become a dependency-ordered script (`powershell`, `bash`), a `github-actions` workflow skeleton, or `jsonl` / `json`; behavior trees become a Markdown runbook (`markdown` or the original open `json`); and L3 knowledge becomes Markdown (`markdown`) or `jsonl`. Without `--out`, a single queue/tree/knowledge export prints to stdout; multiple items are written under `export/queues`, `export/behavior-trees`, and `export/knowledge.md`. Dependencies declared with `on: "done"` still run after a dependency fails; normal (`success`) dependents are skipped. A dependency cycle is reported as a warning and the remaining jobs keep their file order.

`hancho-cli.exe help` is a complete, self-contained usage guide. It covers **HANCHO-CLI CONCEPTS** (process IDs, modes, stdin, logs, `.done` files, cwd/env), **exit codes**, **environment variables**, **examples**, the **per-invocation `--data-dir`** and how to keep one project's data consistent, the **task queue**, **MCP auto-registration**, the **LLM orchestration model**, and the full **19-tool MCP list**.

`hancho-cli.exe help --json` returns the same material as machine-readable JSON:

| Key | Contents |
| :--- | :--- |
| `version`, `aliases` | hancho-cli version and aliases (currently empty) |
| `concepts` | Process IDs, modes, stdin, logs, `.done` files, cwd/env, data directory, lifecycle |
| `cli_exit_codes`, `environment_variables` | Exit codes and `HANCHO_LOG_DIR` |
| `data_dir_option`, `effective_data_dir` | Per-invocation `--data-dir`, its resolved absolute path for this call, and project consistency rules |
| `common_examples` | Ready-to-run `hancho-cli.exe` commands |
| `commands` | Every command and option, including `taskqueue add/list/get/retry/cancel/pause/resume/graph/wait/purge`, `mcp install/uninstall` and `agent uninstall-template` |
| `setup.mcp` | MCP registration, a ready-to-use config, and `auto_install` / `uninstall` (`argv` + `command`) |
| `agent` | `working_model` (supervisor → Hancho → worker LLM), `data_dir_consistency`, `decision`, `states`, `quick_start` |
| `leaving` | Graduation steps, JSON / JSONL data formats, keep-or-delete-data behavior, and the release checklist |

This makes it possible for an LLM to read `help --json`, register the MCP server, pick the right execution model, and start using the tool without extra instructions. The legacy `Hancho.exe <command>` form remains available for compatibility, but `hancho-cli.exe` is the recommended console interface. `Hancho.exe --help` documents the GUI/MCP server, the working model, and the same per-launch `--data-dir` option (see the [appendix](../README.md#appendix-hanchoexe---help)).
