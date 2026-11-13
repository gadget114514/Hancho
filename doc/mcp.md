# Usage 2: MCP Server for Claude Code / Claude Desktop

**[English](mcp.md) | [日本語 (Japanese)](mcp-ja.md)**

The MCP server is provided by **`Hancho.exe mcp`** (not `hancho-cli.exe`). Add it to your `.mcp.json` or `claude_desktop_config.json`:

```json
{
  "mcpServers": {
    "hancho": {
      "command": "C:/wsgit/Hancho/build/Release/Hancho.exe",
      "args": ["mcp"]
    }
  }
}
```

Or let `hancho-cli.exe` write the config for you (merges with existing servers, idempotent):

```powershell
hancho-cli.exe mcp install                     # .mcp.json (Claude Code)
hancho-cli.exe mcp install --client opencode   # opencode.json
hancho-cli.exe mcp install --dry-run --json    # preview
hancho-cli.exe mcp uninstall                   # remove the entry (keeps data)
hancho-cli.exe mcp uninstall --delete-data-dir # also delete the data directory
```

`mcp uninstall` is the inverse of `mcp install`: it removes only the Hancho entry and leaves other servers alone. Data is kept by default; all Hancho data stays in JSON / JSONL and is readable without Hancho. See [design-v3-en.md section 5.3-5.5](design-v3-en.md) for when leaving is the right call.

You can also run the server directly with `Hancho.exe mcp`. It connects to the dashboard over the Named Pipe and automatically launches `Hancho.exe gui` if the dashboard is not running.

## Provided MCP Tools

**Process tools**

| Tool | Description |
| :--- | :--- |
| `start_process` | Start one command (`cmd`, `name`, `mode` [`pty`\|`pipe`\|`console`], `pty`, `open_stdin`, `cwd`, `log_file`, `done_file`, `timeout_sec`, `idle_timeout_sec`, `estimate_end`, `wait`, `wait_ms`). Chained commands are rejected. |
| `list_processes` | List managed processes (`id` optional). |
| `get_process_output` | Read output (`id`, `tail`, `wait_ms`). |
| `send_process_input` | Send input to stdin/ConPTY (`id`, `input`, `newline`, `wait_ms`). |
| `wait_process` | Wait for exit (`id`, `wait_ms`). |
| `kill_process` | Kill a process tree (`id`; `0` = all). A killed task-queue attempt is retried by the scheduler. |

**Task queue tools**

| Tool | Description |
| :--- | :--- |
| `taskqueue_add` | Register jobs (`taskqueue`, `jobs[]`, `concurrency?`, `notifyFile?`). Each job needs `id`, `command`, `summary`, `timeoutSec`. |
| `taskqueue_list` | List jobs (`taskqueue?`, `state?`). |
| `taskqueue_get` | Full job, attempts and instructions (`id`, `includeInstructions?`, `rev?`). |
| `taskqueue_retry` | Retry failed/blocked/cancelled (`id`, `resetAttempts?`, `refreshInstructions?`); blocked dependents are requeued. Bulk: `failed: true` with no `id` retries all failed jobs, optionally limited by `taskqueue` and `since` (e.g. `6h`). |
| `taskqueue_update` | Update a pending/failed/blocked job. |
| `taskqueue_cancel` | Cancel a job (kills a running attempt, no retry). |
| `taskqueue_pause` / `taskqueue_resume` | Stop/restart starting new jobs in a queue. |
| `taskqueue_wait` | Long-poll for a queue version change (`taskqueue`, `sinceVersion`, `timeoutMs`). |
| `taskqueue_purge` | Remove finished jobs (`taskqueue`, `states[]`). |
| `taskqueue_link` / `taskqueue_unlink` | Add/remove one dependency (`job`, `dependsOn`, `on` = `success`\|`done`). |
| `taskqueue_graph` | Dependency graph with `ready` / `waiting_for` / `dependents`. |

**Power tools**

| Tool | Description |
| :--- | :--- |
| `power_suspend` | Suspend the PC. With `jobId`, wait until that job finishes and every other job / managed process is idle, then suspend. Without it, suspend after `delaySec` (default 3). `hibernate` selects hibernation; `dryRun` only reports. Local host only. |
| `power_cancel` | Clear every armed `suspendWhenDone` and cancel the countdown. |
| `power_status` | Return armed jobs, the phase (`waiting_target` / `waiting_others` / `countdown`), other running jobs/processes, and seconds until suspend. |

**Memory tools**

| Tool | Description |
| :--- | :--- |
| `memory_search` | BM25-like search over L1 runs, L2 notes and L3 knowledge (`query`, `kinds?`, `scope?`, `file?`, `jobName?`, `taskqueue?`, `engine?`, `model?`, `outcome?`, `failureClass?`, `since?`, `limit?`). |
| `memory_get` | One record in full, with its source file and line (`id`, `kind?`). |
| `memory_brief` | Fixed-shape summary for a job (`jobName?`, `taskqueue?`, `file?`, `files?`, `scopes?`, `budgetChars?`, `includeAgentNotes?`, `maxPreviousRuns?`): previous runs, common failures, related notes and project knowledge, with an id on every line. |
| `memory_stats` | L1 statistics: success rate, duration median/p90, failure classes and token/cost totals, grouped by job, engine and model. |
| `memory_approve` | Promote an L2 note to L3 project knowledge (`id`, `worker?`, `by?`). Allowed only for specific workers enumerated in `memory.l3Promoters` in `config.json`. |

**LLM usage tools**

| Tool | Description |
| :--- | :--- |
| `usage_stats` | Query aggregated LLM token usage and estimated cost statistics (U1) across daily rollup logs (`since?`, `until?`, `groupBy?`: `day`\|`month`\|`engine`\|`model`). |
| `usage_export` | Export aggregated LLM usage to CSV or JSON format (`format?`: `csv`\|`json`, `since?`, `until?`, `groupBy?`, `out?`). |

Promotion to L3 project knowledge is permitted for human operators (`hancho-cli memory approve <id>` or the GUI) and authorized workers listed in `memory.l3Promoters` in `config.json` via MCP tool `memory_approve`. Unauthorized agents and unlisted workers are rejected.
