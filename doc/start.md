# Start

**[English](start.md) | [日本語 (Japanese)](start-ja.md)**

```powershell
hancho-cli.exe mcp install --client opencode   # or --client claude (writes .mcp.json)
hancho-cli.exe help --json                     # machine-readable guide: see "agent.quick_start"
```

Rules of thumb (the same rules are encoded in `help --json`):

- **One command per job.** For multiple steps or dependencies use `taskqueue_add` with `dependsOn`; shell chains (`&&`, `||`, `&`, PowerShell `;`) and polling loops are rejected.
- **Timeouts are per job** (`timeoutSec`), never summed. Every job also needs a one-line `summary`.
- **Success is verified** by the exit code plus optional `filesNewer` / `outputRegex`, not merely "the process exited".
- **Watch the states**: `waiting` = queued and ready (waiting for a slot), `pending` = not queued yet (dependencies not satisfied), `blocked` = a dependency failed. Use `taskqueue_wait` / `taskqueue_graph`, and `taskqueue_retry` after fixing the cause.

## Build Instructions

Requires Windows 10/11 (x64), Visual Studio 2022 (C++17), and CMake 3.20+:

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

Output binaries:
- `build/Release/Hancho.exe` — GUI dashboard, Named Pipe IPC server, and MCP stdio server (alongside `conpty.dll` and `OpenConsole.exe`)
- `build/Release/hancho-cli.exe` — Console CLI client

## Executables: `Hancho.exe` vs. `hancho-cli.exe`

| Executable | Role | Typical commands |
| :--- | :--- | :--- |
| `Hancho.exe` | GUI dashboard, Named Pipe IPC server, and MCP stdio server. Owns all managed ConPTY processes. | `Hancho.exe gui`<br>`Hancho.exe mcp`<br>`Hancho.exe help` |
| `hancho-cli.exe` | Console client that sends requests to the running dashboard. No GUI of its own. | `hancho-cli.exe list`<br>`hancho-cli.exe start -- cmd.exe`<br>`hancho-cli.exe help --json` |

- **`Hancho.exe`** starts the dashboard with `gui` (the default if no command is given). If the dashboard is already running, `gui` restores it from the System Tray instead of starting a second instance. Use `mcp` to expose Hancho as an MCP stdio server (it auto-starts the dashboard when needed).
- **`hancho-cli.exe`** is the recommended console interface for humans, scripts, and LLM agents. It communicates with the dashboard over `\\.\pipe\Hancho_IPC_v1`, auto-starts `Hancho.exe` when the dashboard is not running, and locates the GUI binary in the same directory. Keep both `.exe` files together.
- **Legacy CLI compatibility**: `Hancho.exe <command> [options]` forwards `start`, `list`, `logs`, `send`, `wait`, `kill`, `kill-all`, and `clear` to the same handler as `hancho-cli.exe`. Prefer `hancho-cli.exe` for new usage.
- **LLM discovery**: `hancho-cli.exe help --json` is the canonical machine-readable guide. It includes `concepts` (IDs, modes, stdin, logs, `.done` files, cwd/env, data directory), `cli_exit_codes`, `environment_variables`, `common_examples`, `data_dir_option` with the resolved `effective_data_dir`, every command (including the `taskqueue` subcommands and `mcp install`), `setup.mcp.auto_install`, and an `agent` object with `working_model`, `data_dir_consistency`, `decision`, `states`, and `quick_start`.
