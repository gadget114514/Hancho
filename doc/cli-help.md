# CLI help output

**[English](cli-help.md) | [Japanese](cli-help-ja.md)**

Full `--help` output for `Hancho.exe` and `hancho-cli.exe` (moved out of the README).

## Appendix: `Hancho.exe --help`

```text
Hancho 0.9.3 - Native Win32 Multi-LLM Process & ConPTY Manager + Live GUI Dashboard

USAGE:
  Hancho.exe [gui] [--data-dir <path>]
      Launch the live Win32 Dashboard GUI and Named Pipe server.

  Hancho.exe quit
      Quit the running GUI, including one hidden in the system tray (its tray icon is removed).

  Hancho.exe mcp [--data-dir <path>]
      Run as a Model Context Protocol (MCP) stdio server for Claude.
      Automatically connects to (or launches) the Win32 GUI Dashboard.

  Hancho.exe --help
      Show this help.

  Hancho.exe --version
      Show the version.

OPTIONS (per launch):
  --data-dir <path>   Store logs, state, task queues and config in <path>.
                      Use a project-local directory to git-manage them. Each
                      data dir gets its own dashboard instance (separate pipe).
                      Equivalent environment variable: HANCHO_LOG_DIR.
                      Consistency: pass the same --data-dir on every call, or embed
                      it once with `hancho-cli.exe mcp install --data-dir <path>`.
                      Different data dirs are isolated dashboard instances.

WORKING MODEL (LLM orchestration):
  Hancho is the execution layer for a supervising LLM.

    Supervisor LLM (Claude Code, opencode, ...)
      |  start_process   -> one long-running worker command
      |  taskqueue_add   -> ordered/dependent worker jobs
      |  bt_start        -> a behavior tree: steps, branches, retries, checks
      v
    Hancho
      |  owns processes, timeouts, logs, retries, state
      v
    Worker LLM / build command
      -> one job command; writes artifacts and logs

    Audit LLM (Objective Auditor / Evaluator)
      -> observes telemetry (timing divergence, thrashing, task hygiene);
         feeds corrective guidance back to the supervisor via hints & MCP.

  Assumed structure:
    1. The supervisor plans and declares jobs. It does not run the work itself,
       does not chain commands, and does not fabricate success.
    2. Each unit of work is one job with a one-line summary, its own timeoutSec,
       and an optional success check (filesNewer / outputRegex). Steps or
       dependencies become separate jobs linked with dependsOn.
    3. Workers are ordinary processes and may themselves be LLM CLIs. Pass
       detailed instructions via instructionsFile/instructionsText; workers read
       {instructions} / PC_JOB_INSTRUCTIONS and write artifacts.
       Role clarity: when supervisor and worker share the same engine
       (Claude/Claude, opencode/opencode), roles can blur; prefer a different
       worker engine or a clearly distinct worker role/prompt.
    4. The supervisor observes with taskqueue_wait / taskqueue_list /
       taskqueue_graph and reads logs; it never polls .done files.
    5. On failure, fix the cause and call taskqueue_retry. Blocked dependents
       resume automatically.
    6. Keep the supervisor context small: read artifacts, not raw transcripts.
    7. When the next step depends on results (build -> fix -> rebuild loops,
       test -> fix -> retest, "if this file says X then ..."), do not watch and
       add jobs by hand: write the whole plan as a behavior tree (BT) and submit
       it once with bt_start. Hancho runs it, adding each step as an
       ordinary job, and returns to you only when a decision is needed
       (escalate) or the tree finishes. See BEHAVIOR TREES below.
    8. The Audit LLM acts as an objective observer of the supervisor's orchestration.
       It detects execution timing divergences (actual duration vs estimate),
       prevents unmodified retry loops (thrashing), and closes the feedback loop
       via popup hints and MCP responses. See doc/audit-llm.md (Japanese: doc/audit-llm-ja.md).

CONSOLE CLIENT / CLI SETUP:
  Use hancho-cli.exe for CLI commands (start, list, logs, send, wait, kill, kill-all, clear).
  It auto-starts the GUI dashboard and locates Hancho.exe in its folder.
  Run `hancho-cli.exe help` for human-readable usage, or `hancho-cli.exe help --json` for
  machine-readable commands and setup instructions for LLMs.

COMPLETION NOTIFICATION / BACKGROUND TASKS:
  `hancho-cli.exe start ... --wait --wait-ms 0 --json` keeps hancho-cli.exe alive until the
  managed process exits. `--wait-ms 0` means unlimited wait. Exit code is 0 on
  completion, the child exit code on failure, and 124 if the wait times out.
  For an existing process: `hancho-cli.exe wait --id <id> --wait-ms 0 --json`.
  Hancho also writes a .done JSON file on exit.

MCP SERVER SETUP:
  Register this executable in .mcp.json (Claude Code) or claude_desktop_config.json:
    {
      "mcpServers": {
        "hancho": {
          "command": "<absolute path to Hancho.exe>",
          "args": ["mcp"]
        }
      }
    }
  Auto-register (recommended for LLM agents):
    hancho-cli.exe mcp install                    (writes .mcp.json in the current directory)
    hancho-cli.exe mcp install --client opencode  (writes/updates opencode.json)

  PRECONDITIONS (LLM agents):
    - Windows 10/11 x64, same user session as the dashboard; no admin required.
    - The dashboard auto-starts on first use and owns the Named Pipe server.
    - Logs, state and task queues live under logs/ (override: HANCHO_LOG_DIR).

  MCP tools (34):
    Process (6):
      start_process, list_processes, get_process_output, send_process_input,
      wait_process, kill_process
    Worker (5):
      run_worker, wait_worker, get_worker_stream, get_worker_state, update_worker_info
    Task queue (13):
      taskqueue_add, taskqueue_list, taskqueue_get, taskqueue_retry,
      taskqueue_update, taskqueue_cancel, taskqueue_pause, taskqueue_resume,
      taskqueue_wait, taskqueue_purge, taskqueue_link, taskqueue_unlink,
      taskqueue_graph
    Behavior tree (10):
      bt_validate, bt_start, bt_get, bt_list, bt_wait, bt_resume, bt_retry,
      bt_update, bt_cancel, bt_pause

  HOW TO USE (decision guide):
    - One command, e.g. a build script, CLI worker or shell -> start_process
    - One task for a worker LLM, final result only         -> run_worker
      Harnesses do not see this as a native sub-agent; a thin custom agent that
      calls run_worker makes it one, at the cost of the wrapper's tokens.
    - Multiple steps, fixed order or dependencies         -> taskqueue_add + dependsOn
    - Steps whose next step depends on results: branches,
      fix loops, checks, worker LLM steps, your decisions -> bt_start + bt_wait
    - Never write &&, ||, a single &, PowerShell ';', newline chains or polling
      loops. ChainGuard rejects them: one job = one command with its own timeoutSec.
    - Required taskqueue job fields: id, command, summary, timeoutSec.
      Optional: cwd, env, dependsOn, idle, maxAttempts, retryDelaySec, success,
      killExclude, resources {write,read}, priority, instructionsFile / instructionsText.
    - resources: jobs whose write/read sets overlap never run together (read/read is
      fine). Paths/globs resolve against cwd; "@name" is a named lock.
    - Success is verified: exit code AND optional filesNewer / outputRegex.
    - States: waiting = queued and ready; pending = dependencies not satisfied;
      blocked = a dependency failed; running; succeeded; failed; cancelled.
    - Observe with taskqueue_wait / taskqueue_list / taskqueue_graph; after fixing
      the cause use taskqueue_retry (blocked dependents resume automatically).

  BEHAVIOR TREES (bt_*):
    A behavior tree is one JSON document describing the whole workflow. Leaves run
    work (job = one command, worker = one task for a worker LLM); composites decide
    what runs next (sequence, fallback, parallel, if, retry, repeat, timeout);
    conditions check results (fileExists, fileNewer, outputMatches, exitCode,
    jsonValue, var, nodeState); escalate hands a decision back to you.
    Flow:
      1. bt_validate (optional), then bt_start {bt: {...}} or {file: "plan.json"}.
      2. Loop on bt_wait {id, sinceVersion, timeoutMs}. It returns when the tree
         finishes or waits for you (state "waitingForLlm" + a decision packet).
      3. Answer with bt_resume {id, node, answer, rationale}. The packet holds the
         question, guidance, allowed answers and the logs/files/vars you asked to
         include, so you can decide even after your context was compacted.
      4. On failure read bt_get {id, compact: true}; fix the cause (bt_update for
         a non-running node's definition) and call bt_retry {id, from}.
    Rules:
      - retry / repeat need "max". Jobs need command, summary, timeoutSec; workers
        need prompt, summary, timeoutSec and optionally engine / model (claude,
        agy, opencode, codex or ones defined in <data dir>\engines.json).
      - Write "intent" (the goal) and, on escalate, "question" + "guidance" for
        your future self. ${name} expands variables; in command it is quoted as
        one argument. Do not retry BT jobs with taskqueue_retry; use bt_retry.
    Minimal example (build, and on failure let a worker fix it, up to 3 times):
      {"id": "fix-build", "summary": "Build and fix", "intent": "Release build passes",
       "cwd": "D:/repo",
       "root": {"type": "retry", "max": 3, "child": {"type": "fallback", "children": [
         {"type": "job", "id": "build", "summary": "Build", "timeoutSec": 1800,
          "command": "cmake --build build --config Release"},
         {"type": "sequence", "children": [
           {"type": "worker", "id": "fix", "engine": "claude", "summary": "Fix build",
            "timeoutSec": 3600, "prompt": "Fix the build errors in ${build.logFile}."},
           {"type": "fail"}]}]}}}
    Full format, every node type and three complete examples:
      doc\behavior-tree.md (English), doc\behavior-tree-ja.md (Japanese), in the
      Hancho installation folder. `hancho-cli.exe bt --help` lists the CLI.

  For the full machine-readable guide (agent.quick_start, decision, states):
    hancho-cli.exe help --json
```

---

## Appendix: `hancho-cli.exe help` output

```text
hancho-cli 0.9.3 - Hancho console client

USAGE:
  hancho-cli [--json] <command> [options]

GLOBAL OPTIONS:
  --json, -j               Print machine-readable JSON instead of human-friendly text.
  -h, --help               Show this help.
  -V, --version            Show the version (also: hancho-cli version).

DATA DIRECTORY (per invocation):
  --data-dir <path>        Store logs, state, task queues and config in <path> for
                           THIS hancho-cli call. May be placed before or after the command.
                           Point it at a project folder to git-manage the data:
                             hancho-cli.exe --data-dir .hancho taskqueue add --file jobs.json
                             hancho-cli.exe taskqueue list main --data-dir .hancho
                           Each data dir gets its own dashboard instance, so different
                           projects can run at the same time.

  CONSISTENCY (one data dir per project):
    --data-dir is per invocation, not a global setting. To keep a project's
    logs/state/task queues consistent:
      1. Choose one project-local directory, e.g. .hancho in the project root.
      2. Register the MCP server once with it:
           hancho-cli.exe mcp install --data-dir .hancho
         This writes "--data-dir .hancho" into the project's MCP config,
         so every MCP tool call for the project uses the same directory.
      3. Pass the same --data-dir on every direct hancho-cli call.
      4. Relative paths resolve against the caller's current directory. MCP clients
         normally start the server in the project root; if the cwd is uncertain,
         put an absolute path in the MCP config.
      5. Different data dirs are isolated (own dashboard instance, Named Pipe and
         mutex); they never share processes or queues.
      6. Verify: hancho-cli.exe --data-dir <path> help --json prints
         "effective_data_dir", and the dashboard status bar shows the same path.

HANCHO-CLI CONCEPTS:
  Process IDs : Numeric IDs assigned by the dashboard (#1, #2, ...). Use them with
                list --id, logs --id, send --id, wait --id and kill --id.
  Modes       : pipe    = anonymous pipes (default; non-interactive CLI workers)
                pty     = Win32 ConPTY (interactive TUI: opencode, claude)
                console = separate visible console window
  stdin       : pipe mode keeps stdin writable (hancho-cli send) unless --close-stdin;
                close it for programs that wait for EOF.
  Logs        : logs/proc_<id>_<name>_<timestamp>.log (or --log-file).
  Streams     : logs/proc_<id>_<name>_stream.jsonl (raw) and
                logs/proc_<id>_<name>.events.jsonl (normalized hancho.event/1).
  Adapters    : <data dir>/adapters/<name>.json (Tier 1 declarative or Tier 3 exec).
  Done file   : atomic JSON on exit (logs/proc_<id>.done or --done-file) with
                state, exit_code, reason, log_file and done_file.
  cwd / env   : --cwd defaults to the caller's directory; PWD and INIT_CWD are
                set for the child and OLDPWD is removed.
  JSON output : --json / -j before or after the command prints one JSON object.
  Lifecycle   : start returns immediately unless --wait; use wait --id or the
                .done file to detect completion.

EXIT CODES:
  0    success / completed
  1    CLI or configuration error (also: process failure without --wait)
  124  wait timed out while the process is still running
       start --wait / wait return the managed process's exit code

ENVIRONMENT:
  HANCHO_LOG_DIR     Override the logs directory (state, logs, queues).
  HANCHO_EVENT_PIPE  Set by Hancho for pipe-mode workers; hancho-cli event emit
                     writes hancho.event/1 lines to this pipe.

EXAMPLES:
  hancho-cli.exe list --json
  hancho-cli.exe start --name build --cmd "cmd /c build.cmd" --timeout 1800 --wait --wait-ms 0 --json
  hancho-cli.exe start --name tui --pty --cmd "opencode"
  hancho-cli.exe logs --id 1 --tail 80
  hancho-cli.exe send --id 1 --input "status"
  hancho-cli.exe taskqueue add --file jobs.json --taskqueue main
  hancho-cli.exe mcp install --client opencode

COMMANDS:
  list [--id <id>]
      List all managed processes and their live metrics.

  logs --id <id> [--tail <lines>] [--wait-ms <ms>] [--raw]
      Fetch process output logs (ANSI codes stripped by default unless --raw).

  start [options] -- <command...>
      Start a managed process (e.g. claude, opencode) under a Win32 Job Object.
      Options:
        --cmd "<command>"        Command line to execute
        --name <label>           Display name on dashboard (e.g. "opencode-worker")
        --mode <pty|pipe|console>
                                 pty     = Win32 ConPTY interactive session
                                 pipe    = Anonymous pipes (default; no ConPTY)
                                 console = Separate visible console window
        --pty                    Shortcut for --mode pty (enable ConPTY).
        --open-stdin             Pipe mode only (default): keep stdin writable for hancho-cli send.
        --close-stdin            Pipe mode only: close stdin after launch so non-interactive
                                 programs (e.g. opencode run) receive EOF and can finish.
        --cwd <dir>              Working directory (default: the caller's current directory)
        --log-file <path>        Custom path for live output log
        --done-file <path>       Custom completion marker file created on exit
        --timeout <sec>          Max runtime timeout in seconds (0 = unlimited)
        --idle-timeout <sec>     Hang timeout if no output for N seconds (0 = unlimited)
        --eta <time>             Estimated end time shown in gray while running
                                 (e.g. "18:30" or "2026-10-22 18:30:00").
                                 If omitted, --timeout shows "remaining N seconds".
         --estimate-end <time>    Alias for --eta
         --agent-type <claude|agy|antigravity|opencode|codex|aider|ollama|adapter>
                                  Hint worker type for JSON stream parsing (auto-detected if omitted).
                                  A name with <data-dir>/adapters/<name>.json uses that Tier 1/3 adapter.
         --json-stream            Enable JSONL stream parsing; writes <log>.stream.jsonl and the
                                  normalized <log>.events.jsonl (hancho.event/1) for LLM workers.
         --wait                   Block until process exits (unlimited unless --wait-ms is set)
        --wait-ms <ms>           Max wait when --wait is set; initial output wait otherwise
                                 (default: 500; 0 = unlimited with --wait)

  send --id <id> --input "<text>" [--no-newline] [--wait-ms <ms>]
      Send input text to a running process's stdin or ConPTY session.

  wait --id <id> [--wait-ms <ms>]
      Wait until the specified process exits (or wait-ms elapses).

  kill --id <id>
      Forcefully kill the entire process tree (Job Object) for <id>.

  kill-all
      Forcefully kill all running managed process trees.

  clear
      Remove finished processes from the dashboard list.

  event emit --kind <kind> [--text <text>] [--tool <name>] [--activity <a>] [--target <t>]
             [--status <s>] [--data <json>] [--pipe <name>]
      Write one hancho.event/1 line to HANCHO_EVENT_PIPE (set for pipe-mode workers).

  adapter test <name> [--file <adapter.json>] [--dir <test-dir>]
      Run the Tier 1 declarative adapter against tests/adapters/<name>/*.input.jsonl and
      *.expected.jsonl (each expected line is {kind, data}).

  worker status <id>
      Show current worker status, active tool, thoughts, message, and supervisor context.

  worker stream <id> [--tail <n>] [--type <kind>] [--since <seq>] [--summary] [--schema <v1>]
      View captured JSON streaming events for claude, agy, or opencode workers.
      --schema v1 (default) returns the stable hancho.event/1 shape from <log>.events.jsonl.
      --schema legacy is not supported (removed in v0.11.0).
      --type matches the kind (tool.call, message, ...).

  worker info <id> [--notes "<notes>"] [--goal "<goal>"]
      Update supervisor notes or goal for an ongoing worker.

  agent install-template [--path <dir>] [--dry-run]
      Write the hancho-runner subagent to <dir>\.claude\agents\hancho-runner.md (default: the current
      directory). It runs long commands through Hancho and returns a short summary.
      An existing file is not overwritten.

  agent uninstall-template [--path <dir>] [--dry-run] [--delete-data-dir]
      Remove the hancho-runner subagent written by install-template (the inverse command).
      Empty .claude\agents and .claude directories are removed too. Add --delete-data-dir
      to also delete the data directory (kept by default).

  mcp install [--client claude|opencode] [--path <file>] [--name <name>] [--data-dir <path>] [--dry-run]
      Register Hancho as an MCP server automatically. Writes or updates
      .mcp.json (Claude Code, default) or opencode.json (--client opencode).
      --data-dir embeds "--data-dir <path>" in the generated MCP server args so the
      project keeps its logs/state/task queues inside that directory.
      --json prints the machine-readable result; --dry-run previews without writing.

  mcp uninstall [--client claude|opencode] [--path <file>] [--name <name>] [--data-dir <path>]
                [--delete-data-dir] [--dry-run]
      Remove the Hancho entry from the MCP config (the inverse of mcp install).
      The data directory is kept by default; add --delete-data-dir to delete it too.
      The data directory is the one embedded by mcp install, --data-dir, or the default.

LEAVING HANCHO (graduation):
  Stop using Hancho without losing your work:
    1. hancho-cli.exe mcp uninstall                  # remove MCP registration (keep data)
    2. hancho-cli.exe agent uninstall-template       # remove hancho-runner.md
    3. Delete the data directory only if you want to:
         hancho-cli.exe mcp uninstall --delete-data-dir
  While you keep it, all Hancho data stays in open formats: queues, behavior trees,
  events and memory are JSON / JSONL and readable without Hancho.
  When a harness covers your use, compare it with doc\design-v3.md sections 5.3-5.5
  (feature map and conditions) before leaving. doc\release-checklist.md records the
  per-release review of that table.

TASK QUEUE:
  taskqueue add --file <jobs.json> [--taskqueue <name>] [--concurrency <n>] [--notify-file <path>]
                [--origin-label <text>]
      Register jobs (array, or an object with "jobs"/"taskqueue"/"concurrency").
      Each job records its origin: the caller's session id (CLAUDE_CODE_SESSION_ID
      or PC_ORIGIN_SESSION), working folder and pid, plus --origin-label (or
      PC_ORIGIN_LABEL). The dashboard shows it in the Origin column.
      Each job needs id, command, summary, timeoutSec; dependsOn replaces '&&'.
      Optional: cwd, env, dependsOn, idle, maxAttempts, retryDelaySec, success,
      killExclude, resources {write,read}, defer, priority, instructionsFile/instructionsText.
      resources: jobs whose write/read sets overlap (write/write, write/read) never
      run together, across all queues; read/read may. Paths/globs resolve against
      cwd; a bare file name (Foo.cs) means **/Foo.cs; "@name" is a named lock.
      Disable with Settings / config.json resource_locks.
      defer {exitCodes,delaySec,limit} (default [75], 60, 60): an attempt that exits
      with one of exitCodes ("nothing ran, try again later", e.g. ww-run refusing an
      overlapping card) goes back to pending after delaySec without using up
      maxAttempts; after limit deferrals (0 = no limit) it is an ordinary failure.

  taskqueue list [<name>] [--state <state>] [--since <period>] [--origin <self|label|session>] [--bt <id>]
      List jobs with state, queue, summary and blockedBy. --since keeps only
      jobs updated within the period (30m, 6h, 2d), e.g. --since 6h.
      --bt <id> lists every job of a behavior tree across its parallel branch queues.
      --origin self keeps only the jobs this session added.

  taskqueue get <id> [--rev <n>] [--no-instructions]
  taskqueue retry <id>
  taskqueue retry --failed [<name>] [--since <period>]
      Retry every failed job at once, optionally only one queue and only jobs
      that failed within the period, e.g. `taskqueue retry --failed --since 6h`
      retries the failures of the last 6 hours. Blocked dependents resume.
  taskqueue cancel <id>
  taskqueue pause|resume <name>
  taskqueue graph [<name>] [--root <id>]     (ready, waiting_for, resource_wait, retry_in_sec, blocked)
  taskqueue wait <name> [--since <version>] [--timeout-ms <ms>]
  taskqueue purge <name> [--states succeeded,failed,cancelled]
  taskqueue purge [<name>] --older-than-days <N>   (finished jobs not updated for N days; all queues if <name> omitted)
  reset [--yes]                                    (delete ALL project data in the data directory; lists targets without --yes; refused while running)

BEHAVIOR TREES (for LLM agents; next step depends on results):
  bt validate|start --file <bt.json>    Check / register and run a behavior tree
  bt wait --id <id> [--since-version <n>] [--timeout-ms <ms>]
      Long-poll until the tree finishes or waits for your decision (escalate).
  bt resume --id <id> --node <node> --answer <a> --rationale <why> [--vars <json>]
  bt get --id <id> [--compact]  |  bt list
  bt retry --id <id> [--from <node>] [--reset-counters]
  bt update --id <id> [--node <node>] (--file <path> | --bt <json> | --patch <json>)
  bt cancel --id <id>  |  bt pause --id <id> [--paused true|false]
      Leaves: job (one command), worker (one task for a worker LLM; engine/model);
      composites: sequence, fallback, parallel, if, retry/repeat (max required),
      timeout; conditions: fileExists, fileNewer, outputMatches, exitCode,
      jsonValue, var, nodeState; escalate returns a decision packet to you.
      Each leaf runs as an ordinary task-queue job; retry BT jobs with bt retry,
      not taskqueue retry. Full format and examples: doc\behavior-tree.md in the
      installation folder; `hancho-cli bt --help` for all options.

  start/run ... --allow-chain
      Allow a chained command (&&, ||, &, ';') as a single process. Task-queue
      jobs never allow chains; use dependsOn instead.

COMPLETION / BACKGROUND TASKS:
  Use --wait to keep hancho-cli.exe alive until the managed process exits. Add
  --wait-ms 0 for an unlimited wait (or a positive value for a maximum).
  The exit code is 0 on completion, the child exit code on failure, and 124
  if the wait times out.

    hancho-cli.exe start --name task --wait --wait-ms 0 --json --cmd "opencode run ..."
    hancho-cli.exe wait --id <id> --wait-ms 0 --json

  Hancho also writes a .done JSON file on exit. Without --wait,
  start returns immediately; poll the .done file or call wait.

SETUP FOR LLM AGENTS:
  CLI:
    Use hancho-cli.exe (recommended) or the legacy "Hancho.exe <command>" form.
    Run "hancho-cli.exe help --json" to retrieve this help as machine-readable JSON.
    hancho-cli.exe auto-starts the GUI dashboard on first use if it is not running.

  MCP (stdio):
    Register Hancho.exe in .mcp.json (Claude Code) or
    claude_desktop_config.json (Claude Desktop):
      {
        "mcpServers": {
          "hancho": {
            "command": "D:\\wsgit\\Hancho\\build\\Release\\Hancho.exe",
            "args": ["mcp"]
          }
        }
      }

    AUTO-REGISTER (for LLM agents; no manual JSON editing):
      hancho-cli.exe mcp install                    # .mcp.json (Claude Code)
      hancho-cli.exe mcp install --client opencode  # opencode.json
      hancho-cli.exe mcp install --path <file> --json --dry-run

    ORCHESTRATION MODEL (how an LLM should use this):
      A supervising LLM uses Hancho as its execution layer to run
      child-process worker LLMs/CLIs as managed jobs.

        Supervisor LLM  ->  Hancho  ->  Worker LLM / build command
        plans + declares    timeouts, logs,        one job command;
        jobs                retries, state         writes artifacts

      - The supervisor keeps its context small: it plans and declares jobs; it
        does not run the work itself, chain commands, or fabricate success.
      - One job = one command. Use start_process for a single worker; use
        taskqueue_add with dependsOn for multiple steps or dependencies.
      - When the next step depends on results (build -> fix -> rebuild,
        test -> fix -> retest, branch on a file's content), do not watch and
        add jobs by hand: write the plan as a behavior tree and submit it once
        with bt_start, then loop on bt_wait and answer escalations with
        bt_resume (answer + rationale). See BEHAVIOR TREES above and
        doc\behavior-tree.md.
      - Each job needs a one-line summary and its own timeoutSec. Pass detailed
        instructions via instructionsFile/instructionsText; the worker reads
        {instructions} / PC_JOB_INSTRUCTIONS and writes artifacts.
      - Workers may be LLM CLIs (opencode run, claude -p, ...) or build scripts.
      - Role clarity: when the supervisor and worker use the same engine
        (e.g. Claude supervising `claude -p`, or opencode supervising
        `opencode run`), roles can blur and cause confusion; prefer a
        different worker engine for workers, or give workers a clearly
        distinct role/prompt and avoid nested supervision.
      - Verify success with exit code + filesNewer/outputRegex; observe with
        taskqueue_wait / taskqueue_list / taskqueue_graph and logs; retry with
        taskqueue_retry. Blocked dependents resume automatically.
      - Never poll .done files; use taskqueue_wait or wait_process.
      - Delegating to a worker LLM: run_worker blocks until the worker exits and
        returns only its final result; the raw stream stays in stream_file.
      - Your harness does not see Hancho as a native sub-agent. To have
        it shown as one, define a thin custom agent (Claude Code:
        .claude/agents/<name>.md, cheap model, this tool only) that calls run_worker
        and returns the result. The wrapper costs extra tokens; calling run_worker
        directly is cheapest.

    MCP tools (34):
      Process:    start_process, list_processes, get_process_output,
                  send_process_input, wait_process, kill_process
      Worker:     run_worker, wait_worker, get_worker_stream, get_worker_state,
                  update_worker_info
      Task queue: taskqueue_add, taskqueue_list, taskqueue_get, taskqueue_retry,
                  taskqueue_update, taskqueue_cancel, taskqueue_pause, taskqueue_resume,
                  taskqueue_wait, taskqueue_purge, taskqueue_link, taskqueue_unlink,
                  taskqueue_graph
      Behavior tree: bt_validate, bt_start, bt_get, bt_list, bt_wait, bt_resume,
                  bt_retry, bt_update, bt_cancel, bt_pause

NOTE:
  The GUI dashboard is provided by Hancho.exe, which is auto-started
  on first use if it is not already running.
```
