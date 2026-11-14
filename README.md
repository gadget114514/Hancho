# Hancho

**[English](README.md) | [Japanese](README-ja.md)**

**Hancho** (班長, "foreman") is a job foreman for AI coding agents on Windows: your LLM, or you, hands it work, and Hancho runs that work as real terminal processes, in order, with retries, on a dashboard you can watch.

Built around that one idea: a ConPTY multi-terminal dashboard, a task queue with dependencies, timeouts and retries, behavior trees for multi-step plans, and control from both **MCP** and the **CLI**. It treats `claude`, `opencode`, `codex`, build scripts and interactive shells alike.

Check the version with `hancho-cli.exe --version` (current: 0.9.14).

### Who is this for?

- **Windows developers who use LLM coding agents** (Claude Code, opencode, Codex, etc.) and want one supervising LLM to delegate work to other CLI agents running as worker processes.
- **People running long, parallel, or interactive jobs** — builds, test suites, dev servers, TUI agents — that must outlive the chat session, follow a fixed plan with dependencies and retries, or mix in non-Claude workers.
- **Anyone who wants to watch and steer** many agent terminals live on one dashboard, with timeouts, retries, dependencies, and logs that survive restarts.

**Probably not for you if** your tasks are short reasoning or code edits that LLM sub-agents already handle well. Token cost is now about the same either way; see [Sub-agents vs. Hancho](doc/pros-cons-comparison.md#when-token-cost-is-no-longer-the-difference) for what Hancho adds (durability, deterministic control, visibility) and the full [Pros/Cons comparison](doc/pros-cons-comparison.md).

![Hancho Dashboard](doc/images/dashboardgraph.jpg)
![Hancho Dashboard](doc/images/dashboard.jpg)

The dashboard has an **execution monitor pane** below the process and task lists. It follows the selected process and shows its goal, what it is doing now, its TODO progress, job success condition, attempts and time limits, plus a plain-language activity history with elapsed times. JSON-streaming processes are marked `{}` in the process list. See [Key Features](doc/features.md) for details.

![Architecture](doc/images/archimage.jpg)

---

## What it does

Hand Hancho one command, a queue of ordered jobs, or a whole behavior tree. It runs each step as a real terminal process with its own timeout, success check, retries, resource locks and logs, and reports the result back to your LLM. The supervisor LLM stays in charge of the plan; Hancho owns the processes.

See [Overview](doc/overview.md) for the working model in detail: `run_worker`, suspending the PC after a job, behavior trees, stable events and adapters, and how to leave Hancho without losing work.

## Using the Hancho skill

`skills/hancho/SKILL.md` teaches an AI agent how to drive Hancho well: the supervisor / Hancho / worker model, the one-command-per-job rule, `start` vs. task queues vs. behavior trees, `run_worker`, triage, Chat, memory and the common mistakes. It is a normal agent skill (Markdown only), so no build step is needed.

**1. Install Hancho first.** Install the release, then make `hancho-cli.exe` reachable on `PATH`. Optionally register the MCP server once per project:

```powershell
hancho-cli.exe --version
hancho-cli.exe mcp install --client opencode   # or --client claude
```

**2. Put the skill where your agent looks for skills.** From this repository (after the setup, the same folder is at `%LOCALAPPDATA%\Programs\Hancho\skills\hancho`):

```powershell
# opencode: project scope
New-Item -ItemType Directory -Force .opencode\skills | Out-Null
New-Item -ItemType Junction -Path .opencode\skills\hancho -Target (Resolve-Path skills\hancho)

# opencode: global scope (all projects)
New-Item -ItemType Directory -Force "$env:USERPROFILE\.config\opencode\skills" | Out-Null
New-Item -ItemType Junction -Path "$env:USERPROFILE\.config\opencode\skills\hancho" -Target "D:\path\to\Hancho\skills\hancho"

# Claude Code: project scope
New-Item -ItemType Directory -Force .claude\skills | Out-Null
New-Item -ItemType Junction -Path .claude\skills\hancho -Target (Resolve-Path skills\hancho)
```

A plain copy of the `hancho` folder works too; a junction keeps one source of truth. On opencode, adding `"skills": ["./skills"]` to `opencode.json` is an alternative to linking.

**3. Restart or reload your agent, then just ask.** The skill is advertised by its description, so requests like "build this with Hancho", "run these steps as a queue", or "delegate this to a worker LLM" load it automatically. In opencode you can also name it directly with `@hancho`.

The skill starts with `hancho-cli.exe help --json`, so the commands it shows always come from the installed version rather than a stale list.


## Documentation

| Document | Contents |
| :--- | :--- |
| [Overview](doc/overview.md) | Working model, `run_worker`, suspend, behavior trees, stable events, graduation |
| [Start](doc/start.md) | Quick start for LLM agents, build instructions, `Hancho.exe` vs. `hancho-cli.exe` |
| [Key Features](doc/features.md) | Dashboard GUI, shell launcher, history, tray, logs, Job Objects, settings, task queue overview |
| [Console CLI](doc/cli.md) | `hancho-cli.exe` usage, JSON output, stdin modes, completion, `help --json` keys |
| [Task Queue](doc/taskqueue.md) | Ordered jobs, dependencies, job fields, states, ChainGuard |
| [Behavior Trees](doc/behavior-tree.md) | BT format, node types, variables, worker engines, escalate, examples |
| [BT Token Optimization](doc/behavior-tree-token-optimization.md) | Token-saving architecture, `compact: true`, lightweight escalation packets |
| [BT Design Plan](doc/behavior-tree-plan-en.md) | Architecture and implementation plan for the BT engine |
| [MCP Server](doc/mcp.md) | MCP registration and the MCP tools |
| [WCSP](doc/wcsp.md) | WinConsoleServer protocol (WCSP v1): framing, handshake, frames, sessions and security |
| [Data Directory](doc/data-directory.md) | Project-local, git-managed `--data-dir` |
| [Pros/Cons](doc/pros-cons-comparison.md) | When to use Hancho vs. sub-agents and alternatives |
| [Critical Comparison](doc/critical-comparison.md) | A skeptical look at Hancho's limits and when not to use it |
| [Competitive Comparison](doc/competitive-comparison.md) | How Hancho compares with agent frameworks and memory services |
| [Hancho v3 Design](doc/design-v3-en.md) | Event schema and adapters, headless/remote, semantic memory, failure analysis, graduation guide |
| [Release Checklist](doc/release-checklist.md) | Per-release review of the graduation feature map and cleanup commands |
| [CLI help output](doc/cli-help.md) | Full `Hancho.exe --help` and `hancho-cli.exe help` text |
| [Hancho skill](skills/hancho/SKILL.md) | Agent skill for using Hancho well |
| [tools/ww](tools/ww/README-ww.md) | Optional, independent task-card runner template (not part of Hancho) |

---

## Release Notes

### v0.9.16
- **Tabbed terminal client window**: the WCSP client moved out of the dashboard into its own window (`Hancho.exe terminal`). Each tab is an independent session, so several local or remote consoles can be connected at once; `+ Launch` and `[>_]` open the client.
- **Terminal client controls**: follows the dashboard theme, Copy/Paste in the Edit and right-click menus, always-on-top, window-position memory, and a byte-based scrollback budget (64-2048 MB per tab, default 1024 MB). `terminal.history_bytes` (default 32 MiB) controls the server-side replay budget; replays are split into frames, so a 6 MB (~100k-line) history attaches correctly.
- **Dashboard**: remembers the window layout and shows status-aware tooltips; the standalone dashboard no longer draws embedded terminal tabs.
- **Performance**: the usage graph and monitor pane have configurable refresh intervals, task-queue lookups use a job index, and idle polling does less work.
- **Chat**: "Send to Hancho Chat (Compose)" fills the compose box, answers are extracted from worker result events, and hidden Chat starts keep the window hidden.
- **Auto-start**: clients hide their console and keep working when the hidden WMI launch fails.
- **Docs**: new Overview, WCSP protocol and CLI help pages, the `skills/hancho` agent skill, and a rewritten README.
- **Parsing**: replaced `sscanf`/`sscanf_s` with `src/parse_util.h` helpers (no MSVC C4996 warnings).

### v0.9.15
- **Triage**: every failed task-queue job, BT escalation, standalone process failure, timing divergence or resource wait now gets one persistent card. Hancho collects the information (cause, background, environment, conditions, reason), candidate actions and a deterministic recommendation; a person chooses and records a decision from the dashboard, `hancho-cli triage`, or the MCP `triage_list` / `triage_get` / `triage_decide` tools. `triage_mode` selects manual, semi (default; safe high-confidence recommendations run automatically) or auto, optional LLM advice runs as a hidden `_triage` job, and `triage handoff` gives pending cards to a batch supervisor in the hidden `_supervisor` queue. Cards resolve naturally, support follow-up and acknowledgement of automatic decisions, and each kind can be turned on or off. See `doc/triage.md`.
- **Terminal sessions (WCSP)**: the core exposes every ConPTY job over the WinConsoleServer protocol (WCSP v1), and the standalone dashboard attaches through a local TCP listener (`terminal` in `config.json`), so closing the dashboard, switching tabs or detaching no longer stops the job. Enable `terminal.remote` to attach from WinConsoleTerminal on another PC with `attach:<id>`. Plain TCP without encryption; trusted LAN only. See `doc/features.md`.
- **Dashboard**: a Triage KPI shows pending task/process cards and unreviewed automatic decisions, and the task list has a Triage column. The Settings dialog gains dedicated **WinConsoleServer** (local/remote connection switches) and **Triage** (mode, LLM mode and per-kind switches) tabs.
- **Gantt**: the range buttons now pick the latest 3 / 5 / 10 jobs (default 10) instead of a time range, so the chart changes even when there are few jobs.
- **Core startup**: callers wait up to 30 seconds for a starting core, refuse further auto-starts for 60 seconds after a failure, and launch the dashboard outside the caller's process tree, preventing duplicate cores and spawn storms.
- **Fix**: the hidden-tasks toggle is on the Settings Behavior tab.

### v0.9.14
- **Chat handler jobs are hidden**: messages handled by the answering LLM no longer appear in `taskqueue list` or the dashboard task list. They still run (with timeout/concurrency and usage accounting); `taskqueue get <id>` still shows one.
- **Replies work again**: the handler prompt is passed inline instead of asking the worker to read a file, so headless permission checks no longer block the answer; raw stream output is never sent as a Chat reply.
- **Settings**: a "Skip tool permissions (dangerous)" checkbox sets `handler.skip_permissions`, so a received message can run local tools on a trusted LAN.
- **Show hidden tasks**: the dashboard Settings has a "Show hidden tasks" toggle, so the Chat handler jobs can be shown again when needed.
- **Agent name follows the nickname**: the default `<nickname>-llm` tracks nickname changes, and a stored `noname-llm` is treated as automatic and regenerated.
- **Chat send mode**: choosing a host in the Chat host list switches the compose back to send mode.
- **Chat toolbar badge**: the dashboard's Chat button shows the unread Chat count.
- **Dark theme**: combo boxes in the Chat window use the dark theme instead of white fields with dark text.

### v0.9.13
- **Chat tray menu**: the exit item is now labelled **Hancho Chat終了** (Quit Hancho Chat) so it is clear that only the Chat window closes and the dashboard/core keeps running.

### v0.9.12
- **Chat dialog layout**: messages (list, received body and attachments) on the left, hosts in the upper right and the compose in the lower right.
- **Simpler compose**: the send row only picks the destination (selected hosts / local agents / reply). The sender name comes from Settings; `to_agent` and the audience selector are no longer shown, so messages are handled by the destination's answering LLM.
- **Reply mode**: selecting a message, or choosing Reply in the destination combo, shows the received body and attachments (empty when nothing is selected). Choosing another destination leaves reply mode.
- **Log**: separate Sender and Destination columns.
- **Hosts**: the address:port column is gone; peer names fall back to the host name before the key.
- **Settings**: the answering-LLM controls live only in the Settings tab.
- **Defaults**: nickname `noname`; agent name auto-generated as `<nickname>-llm` and used as the sender name.

### v0.9.11
- **Chat**: the IPMsg-compatible feature is now named Chat everywhere: `hancho-cli chat` (alias `msg`), the MCP `chat_*` tools, the standalone `Hancho.exe chat` window and the dashboard toolbar. `doc/ipmsg.md` became `doc/chat.md`.
- **`chat_handler` MCP tool**: show or change the LLM that answers incoming Chat messages (engine, model, agent, cwd, instructions, taskqueue, auto-reply, timeout, max hops, concurrency).
- **Chat reliability**: restarting the node is serialised, untrusted message text is delimited before it reaches the answering LLM, and older `ipmsg` message data is moved to `chat`.
- **Docs**: CLI, MCP and feature docs renamed and updated for Chat.

### v0.9.10
- **Chat (IPMsg compatible)**: the core speaks the classic IPMsg protocol (presence, messages, read receipts, regular-file transfer) on UDP/TCP port 2426, off by default. LLM-to-LLM messaging adds `from_agent` / `to_agent` / `reply_to`, an `audience` (human or agent) and a hop guard, and a worker-LLM handler replies automatically. Use `hancho-cli chat` (alias `msg`), the MCP `chat_*` tools (including `chat_handler` to change the answering LLM), or the standalone dialog (`Hancho.exe chat [--tray]`, also on the dashboard toolbar) with tray notifications. See `doc/chat.md`.
- **Dashboard hover**: truncated cells in the process, task, monitor and hint lists and in the JSON tree show their full text in a tooltip.
- **WebUI settings**: Settings -> Maintenance has a port field (1-65535) and a read-only access URL that includes the LAN address and session token.
- **Dashboard Gantt**: range buttons (6h / 12h / 24h / All) and tick labels placed on local hour / day boundaries.
- **Usage panel**: added a 4h range. Hints that have not been opened yet show a badge on the hint-history button.
- **Execution monitor**: right-click a subtask or step in the list to open a details window with its description, status, timing, tokens, tool calls, event content and raw JSON, plus a Copy button.

### v0.9.9
- **Supervisor hint toast**: the default auto-dismiss time is now 7 seconds (previously 6). `hint_auto_dismiss_sec` now uses `-1` to keep the toast open until it is closed, `0` to disable the toast, and a positive number for auto-dismiss. A value already stored in `config.json` still overrides the default.

### v0.9.8
- **Suspend after a task**: arm a job with `suspendWhenDone` / `suspendDelaySec` / `suspendHibernate`, or run `hancho-cli power suspend --after-job <id>`. The PC sleeps or hibernates after that job and every other task-queue job / Hancho-managed process is idle; new work cancels and restarts the countdown. `power status` / `power cancel`, MCP `power_suspend` / `power_cancel` / `power_status`, and `--dry-run` are included. Local host only.
- **Behavior-tree remaining time and provisional tasks**: the dashboard shows idle job/worker leaves of a running BT as `planned` rows and adds their estimates to the task remaining time. Rows are re-derived from the current tree, so retry / fallback / `if` branch changes and completion remove stale rows automatically. `bt_get` / `bt_list` expose `pendingEstimateSec` and `plannedTasks`; `taskqueue_list` accepts `include_planned`.
- **Dashboard**: `Hancho.exe gui` starts a missing headless core automatically instead of asking (`--no-prompt` keeps the fail-instead behavior). The supervisor hint window button is labeled "Resolve".
- **List search and Gantt**: the process and task list captions have a search button with an inline case-insensitive filter. The task Gantt lays out provisional BT rows as a forecast chain in BT order and repaints as soon as the task/BT state changes.
- **Docs**: power/suspend, planned BT tasks and the `planned` queue state.

### v0.9.7
- **Audit feedback fixes**: `timing_feedback` now measures only the last successful attempt (a fast-failing attempt no longer reports `diverged_under`), ignores runs under 5 seconds for `diverged_under`, and reads its thresholds from `config.json` (`audit_timing_divergence_threshold`, default 2.0; `audit_timing_under_threshold`, default 0.33).
- **`resource_wait` in `taskqueue list`**: each job shows which job and resource a pending job is waiting for, as `taskqueue graph` already did.
- **Supervisor hint window**: copy prompts use existing commands (`taskqueue why` / `retry`, `bt get --id`, `bt resume`); behavior-tree escalations are detected again; several divergences / failures / resource waits are shown one after another, a visible hint is no longer replaced automatically, and a condition that comes back is shown again.
- **Help**: new AUDIT section in `hancho-cli help` and `agent.audit` in `help --json`; `estimateSec` is listed in `taskqueue add`.
- **Docs**: Audit LLM implementation design (`doc/audit-llm-design.md`).

### v0.9.6
- **Audit LLM / supervisor feedback (preview)**: every finished job with an `estimateSec` now exposes deterministic `timing_feedback` (actual duration, ratio, `accurate` / `diverged_over` / `diverged_under`, and concrete advice). The Windows dashboard adds a supervisor guidance hint window for timing divergences, with a one-click corrective prompt and configurable auto-dismiss. See `doc/audit-llm.md`.
- **WebUI exposure settings**: the `<data-dir>/config.json` -> `webui` object controls `enabled`, `scope` (`loopback` / `lan` / `custom`), `host`, `port`, `allowed_hosts` and `allowed_origins`. The default remains loopback-only; LAN mode binds `0.0.0.0`, keeps token authentication, and accepts IP literals plus configured hostnames. Changes apply live through `config_set`. Settings -> Maintenance exposes enable, exposure and allowed hosts.
- **Help and smoke tests**: `hancho-cli help` states that `start` is a process, not a task, and that task lists / per-model usage require `taskqueue add`, `run` or `bt start`; `fileExists` accepts a final-component glob; fixed the five v0.9.5 review regressions (cli_top_smoke registration, BT IPC streaming and data-dir isolation, headless wait/encoding, timestamp race, event schema path).
- **Docs**: Audit LLM specification, updated feature tables, and a refreshed architecture image.

### v0.9.5
- **Browser dashboard**: `hancho-cli web` opens an embedded HTTP + WebSocket dashboard (127.0.0.1:9138, token auth) with the same process / queue / monitor / why / memory views. See `doc/browser-ui.md`.
- **Interactive TUI**: `hancho-cli top` is now a full terminal dashboard with five tabs, row navigation, stop/retry/cancel, remote jobs and `--once --json`, with no external curses dependency. See `doc/tui-design.md`.
- **Remote hosts completed**: one persistent WebSocket multiplexes requests and streams logs/events with sequence resume; a restarted local dashboard reattaches to remote attempts, and `success.filesNewer` is evaluated on the remote host's own clock.
- **Long-term usage**: daily/monthly `hancho.usage.day/1` rollups, `hancho-cli usage stats|export|compact`, MCP `usage_stats` / `usage_export`, IPC `get_usage_daily`, and dashboard range selection. Per-engine/model attribution comes from task-queue / behavior-tree jobs that emit a JSON stream and receive `--model`.
- **Memory operations**: external ranker plugin with automatic BM25 fallback, `memory prune` with a `memory_max_bytes` cap, L3 promotion by configured workers, and L1 records for plain runs outside task queues (`taskqueue record-plain`, `memory rebuild`).
- **Checks and safety**: `success.testReport` (JUnit/NUnit) and scoped named resource locks (`@name:{root}`, `@name:{cwd}`); `hancho.event/1` `data` is strictly typed; `--schema legacy` is rejected.
- **Docs and help**: `hancho-cli help` now states that `start` is a process, not a task, and that task lists and per-model usage require `taskqueue add` / `run` / `bt start`.

### v0.9.4
- **Semantic memory**: L1 run records, L2 notes and L3 approved knowledge under `memory/`. `hancho-cli memory search|get|brief|stats|note|approve|rebuild|prune` and MCP `memory_search`, `memory_get`, `memory_brief`, `memory_stats`, `memory_note`, `memory_retract`. `{memory}` injects a bounded brief and records the ids on the attempt.
- **Failure analysis**: deterministic per-attempt diagnosis (class, confidence, summary, evidence, last activity, suggested action); `hancho-cli taskqueue why <id>`, MCP task results and behavior-tree conditions use it; retry policy can be class-dependent.
- **Headless, POSIX and remote foundations**: `Hancho.exe --headless`, `hancho-cli run --exit-on-drain --report junit.xml`, POSIX build targets and platform boundaries, `hosts.json` and remote job mirroring. These are early; see `doc/build-posix.md` and `doc/remote-hosts.md`.
- **Graduation**: `hancho-cli export` for queues / BTs / knowledge, `mcp uninstall`, `agent uninstall-template`, and a release checklist.
- **ETA and paused start**: jobs accept `estimateSec` so the dashboard shows the card estimate instead of the timeout; `bt start --paused true` registers a tree without creating jobs.

### v0.9.3
- **Legacy stream shape deprecated**: `hancho-cli worker stream` and MCP `get_worker_stream` return `hancho.event/1` by default. The old `type` strings stay available with `--schema legacy` / `schema: "legacy"` through v0.10.x and are removed in v0.11.0. See [migration steps](doc/LLM-jsonstream-format.md#legacy-type-strings-and-migration-to-hanchoevent1).
- **BT-wide task list**: `hancho-cli bt tasks --id <id>`, `taskqueue list --bt <id>` and MCP `taskqueue_list` with `bt` list every job of a behavior tree across its parallel branch queues. Parallel branches keep their own queues (and their own concurrency); the dashboard already shows all queues together.

### v0.9.2
- **Stable event schema**: worker output is also written as normalized `hancho.event/1` events to `logs/proc_<id>_<name>.events.jsonl`, next to the raw `.stream.jsonl`. Read them with `hancho-cli worker stream <id> --schema v1` or MCP `get_worker_stream` with `schema: "v1"`. The JSON Schema ships in `doc/schema/hancho.event.v1.json`.
- **CLI adapters without C++**: add `<data-dir>/adapters/<name>.json` for a JSON-lines CLI (Tier 1 `match` / `rules` / JSON Pointer `map`), or set `exec` to a converter process that reads raw lines on stdin and writes `hancho.event` lines on stdout (Tier 3). `engines.json` accepts `adapter` and `capabilities`.
- **Events from scripts**: pipe-mode workers receive `HANCHO_EVENT_PIPE`; `hancho-cli event emit --kind ... --text ...` writes to it. `hancho-cli adapter test <name>` checks recorded inputs against expected `{kind, data}` pairs.
- **Parallel behavior-tree branches**: each child of a `parallel` node now runs in its own task queue, so the branches actually run at the same time; long queue names get a hash suffix to keep their files unique.
- **Docs and help**: README and `hancho-cli help` describe the event schema, adapters, `HANCHO_EVENT_PIPE`, the new `--schema v1` option, `event emit` and `adapter test`.

### v0.9.1
- **Fix: Resources column header color**: the `Resources` column header in the task list now uses the same themed owner-drawn color as the other columns instead of the native default.
- **Fix: `help --json` MCP auto-register command**: `setup.mcp.auto_install` now recommends `hancho-cli.exe mcp install`. Previously it showed `Hancho.exe mcp install`, which launched the MCP stdio server instead of writing the client config.

### v0.9.0
- **Renamed from ProcessCommander 2.9.x to Hancho 0.9.0**: the product, executable (`Hancho.exe`), GUI window classes, Named Pipe / mutex names, default data folder (`.hancho`), MCP server name (`hancho`) and the `HANCHO_LOG_DIR` environment variable were renamed. The console client was renamed from `pcder.exe` to `hancho-cli.exe`. Old `.processcommander` data and the old pipe/mutex names are not reused or migrated automatically; create a fresh data directory with `--data-dir`.

### v2.9.3
- **See which session added each task**: every task-queue job records its origin (agent session id, Claude desktop session id, MCP client, working folder, process id), filled in automatically by MCP `taskqueue_add`, `hancho-cli taskqueue add` and behavior trees. Name the work with `originLabel`, `--origin-label` or `PC_ORIGIN_LABEL`. The dashboard shows it in a new **Origin** column (hover for every field), and `taskqueue_list` `origin: "self"` (`hancho-cli taskqueue list --origin self`) lists only your own session's jobs. See [doc/taskqueue.md](doc/taskqueue.md).
- **Fewer tokens in behavior tree replies**: only `bt_get` returns the whole tree; the other BT tools and `bt_wait compact: true` leave it out, and long variable values and prompts are cut to a preview. Escalation `include` takes `lines` and `maxBytes`. Worker nodes now see their `outputSchema`.

### v2.9.1
- **Cheaper `bt_wait` for supervisor LLMs**: when the timeout passes with nothing to act on, `bt_wait` now returns only `{state, version, nodeRuns, timedOut}` instead of the whole tree. Finish and escalation replies are unchanged and still return at once.
- **Long, cancellable waits**: the MCP `bt_wait` waits up to 10 minutes by default (a negative `timeoutMs` waits with no limit). Other MCP calls are still served while it waits, and `notifications/cancelled` stops it within about 2 seconds. `hancho-cli bt wait` keeps a 60-second default.

### v2.9.0
- **Behavior trees**: a supervisor LLM can submit a whole multi-step plan (branches, build-fix loops, retries, condition checks, worker LLM steps, decisions) as one JSON tree with `bt_start`. Hancho runs it, adds each step as an ordinary task-queue job, and calls the LLM back only for `escalate` decisions or at the end. New MCP tools `bt_validate`, `bt_start`, `bt_get`, `bt_list`, `bt_wait`, `bt_resume`, `bt_retry`, `bt_update`, `bt_cancel`, `bt_pause` and `hancho-cli bt ...` commands. See [Behavior trees](#behavior-trees-the-next-step-decisions-run-inside-hancho) and [doc/behavior-tree.md](doc/behavior-tree.md).
- **Worker engines**: `worker` steps name an engine (`claude`, `agy`, `opencode`, `codex`, or your own in `engines.json` in the data directory) and a model; permission flags stay in the engine definition.
- **Behavior tree view**: the **BT** toolbar button (Ctrl+Shift+B), or "Show in Behavior Tree" on a task's right-click menu, shows each tree, which node started which task, and how far it has run. Active trees come first; finished trees older than 24 hours are hidden unless "Show all" is on.
- **Help for LLMs**: `Hancho.exe --help`, `hancho-cli help` and `hancho-cli help --json` explain when to use a behavior tree instead of adding jobs by hand.
- **Task queue**: `taskqueue_retry` / `taskqueue_update` from MCP or the CLI on a job started by a behavior tree are refused (use `bt_retry` / `bt_update`); Retry in the dashboard retries the tree from that step.

### v2.7.2
- **Remaining time ignores jobs that cannot run**: the total remaining time (task remaining KPI and tooltip) no longer counts blocked jobs, or pending jobs whose upstream finished without satisfying them. Their counts are unchanged.
- **Pending / blocked KPI animation**: the PENDING / BLOCKED card animates while its count is 1 or more.
- **OpenCode usage attribution**: OpenCode token usage was recorded under agy in the per-model usage view. It is now recorded under OpenCode.

### v2.7.1
- **Font size for lists**: the process list and the task queue list each have their own font size (10-24 px), set on the 表示 (Appearance) tab of Settings.
- **Row colors on a 色 tab**: the per-state row background colors moved from the Appearance tab to a new 色 (Colors) tab in Settings.
- **Window title**: when started with `--data-dir`, the dashboard title shows the data directory, so several dashboards can be told apart.

### v2.7.0
- **Task queue time filter**: `hancho-cli taskqueue list --since 6h` (and the MCP `taskqueue_list` `since` field) shows only jobs updated within the period. Units are m, h and d.
- **Retry all failures**: `hancho-cli taskqueue retry --failed [<queue>] [--since 6h]` requeues every failed job at once, optionally in one queue or only those that failed recently. The MCP `taskqueue_retry` tool accepts `failed: true` without an id for the same bulk retry.
- **Process cleanup**: when a process exits, its leftover child processes (python, cmd, ...) are terminated too, so nothing is left running in the job.
- **Monitor pane**: the activity tree shows only sub-agents; the root agent's own steps are no longer listed.

### v2.5.2
- **Tray recovery**: the tray icon is registered again after Explorer restarts. If the tray icon cannot be added, minimizing keeps the window on the taskbar instead of hiding it.
- **Crash logging**: an exception in the dashboard, IPC, process monitor or output reader is written to the audit log (`CRASH_GUARD`) instead of silently ending the program. Unhandled exceptions are logged as `CRASH`.
- **End blink**: process and task rows blink for a few seconds when they end. Turn it off or change the duration (1-60 s) in the Settings dialog.
- **Failure sound**: a failed, timed-out or killed process plays a different system sound from a completed one. It follows the completion sound setting.

### v2.5.1
- **Row colors**: the process and task lists can color rows by state (running, idle, completed, timeout, failed) in the Settings dialog. Dark and light themes have separate colors, and the text switches between black and white to stay readable.
- **`hancho-cli reset [--yes]`**: deletes all project data in the data directory. Without `--yes` it only lists what would be deleted, and it is refused while Hancho is running.
- **`hancho-cli taskqueue purge --older-than-days N`**: removes finished jobs (succeeded, failed, cancelled) that have not been updated for N days.

### v2.5.0
- **Dashboard refresh**: the update interval now defaults to 2000 ms and can be set in ms in the Settings dialog (100-60000 ms). It doubles while no process or queue job is running.
- **Dashboard redraw**: the process and task lists and the KPI cards are repainted only when their contents change, so idle refresh ticks do less drawing.
- **Task monitor**: the ID column is 1.5x wider and the Queue column is 3/4 as wide.

### v2.3.7
- **Task monitor**: the goal field takes half the pane height, without the old 150px limit.

### v2.3.6
- **Security**: fixed a command injection in the dashboard auto-start. A path or data directory containing `‘` or `’` could run PowerShell commands as the current user.

### v2.3.5
- **Sub-agents**: Claude sub-agents are tracked separately and shown as a tree in the monitor pane. `hancho-cli worker status` and the IPC state list them.
- **Streams**: Claude partial messages (`--include-partial-messages`) and `codex exec --json` events are parsed.
- **Toolbar**: added a refresh icon button to the left of the clock.
- **Settings**: the number of stream events kept per process is configurable (default 2000; -1 keeps them until memory runs out).
- **Tools**: added `hancho-cli worker install-template` (hancho-runner agent), `-Agent` / `WW_AGENT` for ww-run and ww-watch, and `tools/ww-unity`.
- **Lists**: updates pause while a list scrollbar is held, and the task list keeps its scroll position when it refreshes.
- **Tokens**: Claude usage is counted once per message, and the result line adds only what the messages did not report.

### v2.3.4
- **Task monitor**: the scrollbars of the goal and preview fields now follow the theme colours.

### v2.3.3
- **Themes**: added Deep Purple, Pale Purple, Deep Yellow and Pale Yellow.
- **List headers**: the background now has a slight tint of the theme accent colour.
- **Scrollbars**: the process and task list scrollbars are double-buffered to reduce flicker.
- **Settings**: fixed the sound section overlapping the buttons above it.

### v2.3.2
- **Themes**: five new pastel themes with pale backgrounds: Pastel Blue, Orange, Pink, Red and Green.
- **KPI cards**: the colour bar is twice as tall, the text is bold, and the cards are more compact.
- **CLI**: `hancho-cli agent` is now `hancho-cli worker`. The old name still works.
- **MCP**: `get_agent_stream`, `get_agent_state` and `update_agent_info` are now `get_worker_stream`, `get_worker_state` and `update_worker_info`. The old names still work but are no longer listed.

### v2.3.1
- **MCP**: `run_agent` and `wait_agent` are now `run_worker` and `wait_worker`, so they are not mistaken for your harness's native sub-agent feature. The old names still work but are no longer listed.

### v2.3.0
- **MCP**: added tools that hand one task to a worker LLM and return only its final result.
- **Monitor pane**: a tool call's description has its own column, and the subtask list takes two thirds of the width.
- **Task list**: hover to see the full Resources text, and copy cells.
- **ww-run**: finishes when the engine exits, not when its stdout closes.

Older versions: [Releases](https://github.com/gadget114514/Hancho/releases)

