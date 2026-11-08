# Usage 1: Console CLI (`hancho-cli.exe`)

**[English](cli.md) | [日本語 (Japanese)](cli-ja.md)**

The console client `hancho-cli.exe` communicates with the running GUI Dashboard via Named Pipe (`\\.\pipe\Hancho_IPC_v1`). If the dashboard is not yet running, `Hancho.exe` is located in the same directory and launched automatically.

> **Naming:** `Hancho.exe` is the GUI dashboard / MCP server; `hancho-cli.exe` is its console client.

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

**Register as an MCP server automatically (recommended for LLM agents):**

```powershell
hancho-cli.exe mcp install                     # .mcp.json (Claude Code)
hancho-cli.exe mcp install --client opencode   # opencode.json
hancho-cli.exe mcp install --dry-run --json    # preview without writing
```

**Install the `pc-runner` subagent (Claude Code):**

```powershell
hancho-cli.exe agent install-template                  # writes .claude\agents\pc-runner.md
hancho-cli.exe agent install-template --path C:\proj --dry-run
```

`pc-runner` runs long builds and tests through Hancho's MCP tools and returns only a short summary. The file is never overwritten.

`hancho-cli.exe help` is a complete, self-contained usage guide. It covers **HANCHO-CLI CONCEPTS** (process IDs, modes, stdin, logs, `.done` files, cwd/env), **exit codes**, **environment variables**, **examples**, the **per-invocation `--data-dir`** and how to keep one project's data consistent, the **task queue**, **MCP auto-registration**, the **LLM orchestration model**, and the full **19-tool MCP list**.

`hancho-cli.exe help --json` returns the same material as machine-readable JSON:

| Key | Contents |
| :--- | :--- |
| `version`, `aliases` | hancho-cli version and aliases (currently empty) |
| `concepts` | Process IDs, modes, stdin, logs, `.done` files, cwd/env, data directory, lifecycle |
| `cli_exit_codes`, `environment_variables` | Exit codes and `HANCHO_LOG_DIR` |
| `data_dir_option`, `effective_data_dir` | Per-invocation `--data-dir`, its resolved absolute path for this call, and project consistency rules |
| `common_examples` | Ready-to-run `hancho-cli.exe` commands |
| `commands` | Every command and option, including `taskqueue add/list/get/retry/cancel/pause/resume/graph/wait/purge` and `mcp install` |
| `setup.mcp` | MCP registration, a ready-to-use config, and `auto_install` (`argv` + `command`) |
| `agent` | `working_model` (supervisor → Hancho → worker LLM), `data_dir_consistency`, `decision`, `states`, `quick_start` |

This makes it possible for an LLM to read `help --json`, register the MCP server, pick the right execution model, and start using the tool without extra instructions. The legacy `Hancho.exe <command>` form remains available for compatibility, but `hancho-cli.exe` is the recommended console interface. `Hancho.exe --help` documents the GUI/MCP server, the working model, and the same per-launch `--data-dir` option (see the [appendix](../README.md#appendix-hanchoexe---help)).
