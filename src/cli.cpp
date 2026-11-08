#include "cli.h"
#include "common.h"
#include "ipc_client.h"
#include "nlohmann/json.hpp"
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace pc {

namespace {

// pc-runner.md, embedded from tools/agents/pc-runner.md (src/agent_template.rc).
constexpr WORD kRunnerTemplateResourceId = 101;

bool LoadRunnerTemplate(std::string& outText) {
    HRSRC res = FindResourceW(nullptr, MAKEINTRESOURCEW(kRunnerTemplateResourceId), RT_RCDATA);
    if (!res) return false;
    HGLOBAL handle = LoadResource(nullptr, res);
    if (!handle) return false;
    const char* data = static_cast<const char*>(LockResource(handle));
    if (!data) return false;
    outText.assign(data, SizeofResource(nullptr, res));
    return true;
}

// hancho-cli agent install-template [--path <dir>] [--dry-run] [--json]
// Writes <dir>\.claude\agents\pc-runner.md (default dir: the current directory). An existing file
// is never overwritten.
int RunAgentInstallTemplate(const std::vector<std::string>& args, size_t startIdx, bool jsonOutput) {
    std::string dir = ".";
    bool dryRun = false;
    for (size_t i = startIdx; i < args.size(); ++i) {
        if (args[i] == "--path" && i + 1 < args.size()) dir = args[++i];
        else if (args[i] == "--dry-run") dryRun = true;
    }

    auto fail = [&](const std::string& message) {
        if (jsonOutput) {
            std::cout << "{\"ok\":false,\"error\":\"" << JsonEscape(message) << "\"}\n";
        } else {
            std::cerr << "Error: " << message << "\n";
        }
        return 1;
    };

    std::filesystem::path target = std::filesystem::path(Utf8ToWide(dir)) / L".claude" / L"agents" / L"pc-runner.md";
    const std::string targetUtf8 = WideToUtf8(target.wstring());

    std::string text;
    if (!LoadRunnerTemplate(text)) return fail("pc-runner template is not embedded in this build");

    std::error_code ec;
    if (std::filesystem::exists(target, ec)) {
        return fail("already exists, not overwritten: " + targetUtf8);
    }
    if (!dryRun) {
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) return fail("cannot create " + WideToUtf8(target.parent_path().wstring()) + ": " + ec.message());
        std::ofstream out(target, std::ios::binary);
        if (!out) return fail("cannot write " + targetUtf8);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) return fail("cannot write " + targetUtf8);
    }

    if (jsonOutput) {
        std::cout << "{\"ok\":true,\"path\":\"" << JsonEscape(targetUtf8) << "\",\"written\":"
                  << (dryRun ? "false" : "true") << ",\"dry_run\":" << (dryRun ? "true" : "false")
                  << ",\"bytes\":" << text.size() << "}\n";
    } else {
        std::cout << (dryRun ? "Would write " : "Wrote ") << targetUtf8 << " (" << text.size() << " bytes)\n";
    }
    return 0;
}

std::string ResolveGuiExePathUtf8() {
    wchar_t exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0) return "Hancho.exe";

    std::wstring current(exePath, len);
    auto slash = current.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        std::wstring candidate = current.substr(0, slash + 1) + L"Hancho.exe";
        if (_wcsicmp(candidate.c_str(), current.c_str()) == 0) {
            return WideToUtf8(current);
        }
        DWORD attr = GetFileAttributesW(candidate.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            return WideToUtf8(candidate);
        }
    }
    return WideToUtf8(current);
}


struct McpInstallOutcome {
    bool ok = false;
    bool changed = false;
    bool dryRun = false;
    std::string error;
    std::string targetPath;
    std::string entryJson;
};

bool FindJsonContainerBounds(const std::string& json, const std::string& key, size_t& outOpen, size_t& outClose) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = json.find(pattern, pos)) != std::string::npos) {
        size_t after = pos + pattern.size();
        while (after < json.size() && std::isspace(static_cast<unsigned char>(json[after]))) after++;
        if (after < json.size() && json[after] == ':') {
            after++;
            while (after < json.size() && std::isspace(static_cast<unsigned char>(json[after]))) after++;
            if (after < json.size() && json[after] == '{') {
                int depth = 0;
                bool inStr = false;
                bool esc = false;
                for (size_t i = after; i < json.size(); ++i) {
                    char c = json[i];
                    if (inStr) {
                        if (esc) esc = false;
                        else if (c == '\\') esc = true;
                        else if (c == '"') inStr = false;
                        continue;
                    }
                    if (c == '"') { inStr = true; continue; }
                    if (c == '{') depth++;
                    else if (c == '}') {
                        depth--;
                        if (depth == 0) {
                            outOpen = after;
                            outClose = i;
                            return true;
                        }
                    }
                }
            }
        }
        pos += pattern.size();
    }
    return false;
}

bool JsonContainerHasKey(const std::string& json, size_t open, size_t close, const std::string& name) {
    std::string pattern = "\"" + name + "\"";
    size_t pos = open + 1;
    while (true) {
        size_t found = json.find(pattern, pos);
        if (found == std::string::npos || found >= close) return false;
        size_t after = found + pattern.size();
        while (after < close && std::isspace(static_cast<unsigned char>(json[after]))) after++;
        if (after < close && json[after] == ':') return true;
        pos = found + pattern.size();
    }
}

std::string InsertJsonEntry(const std::string& json, size_t open, size_t close,
                            const std::string& name, const std::string& entryBody) {
    size_t p = open + 1;
    while (p < close && std::isspace(static_cast<unsigned char>(json[p]))) p++;
    bool empty = (p >= close);
    std::string insertion = "\n    \"" + name + "\": " + entryBody;
    if (!empty) insertion += ",";
    std::string out = json;
    out.insert(open + 1, insertion);
    return out;
}

McpInstallOutcome InstallMcpConfig(const std::string& client, const std::string& explicitPath,
                                   const std::string& serverName, bool dryRun, const std::string& exePath,
                                   const std::string& dataDir) {
    McpInstallOutcome out;
    out.dryRun = dryRun;

    bool opencode = (client == "opencode");
    std::string container = opencode ? "mcp" : "mcpServers";
    std::string dataArgs;
    if (!dataDir.empty()) {
        dataArgs = ",\"--data-dir\",\"" + JsonEscape(dataDir) + "\"";
    }
    out.entryJson = opencode
        ? "{\"type\":\"local\",\"command\":[\"" + JsonEscape(exePath) + "\",\"mcp\"" + dataArgs + "],\"enabled\":true}"
        : "{\"command\":\"" + JsonEscape(exePath) + "\",\"args\":[\"mcp\"" + dataArgs + "]}";

    std::string target = explicitPath;
    if (target.empty()) {
        if (opencode) {
            if (GetFileAttributesW(L"opencode.json") != INVALID_FILE_ATTRIBUTES) target = "opencode.json";
            else if (GetFileAttributesW(L"opencode.jsonc") != INVALID_FILE_ATTRIBUTES) target = "opencode.jsonc";
            else target = "opencode.json";
        } else {
            target = ".mcp.json";
        }
    }

    std::wstring targetW = ResolveAbsolutePathWide(Utf8ToWide(target));
    out.targetPath = WideToUtf8(targetW);

    std::string content = ReadFileUtf8Wide(targetW);
    if (content.empty()) {
        std::string fresh = "{\n  \"" + container + "\": {\n    \"" + serverName + "\": " +
                            out.entryJson + "\n  }\n}\n";
        if (!dryRun) WriteFileAtomicWide(targetW, fresh);
        out.ok = true;
        out.changed = true;
        return out;
    }

    size_t open = 0;
    size_t close = 0;
    if (!FindJsonContainerBounds(content, container, open, close)) {
        out.error = "config file exists but has no \"" + container + "\" object: " + out.targetPath +
                    " -- add this entry manually: \"" + serverName + "\": " + out.entryJson;
        return out;
    }
    if (JsonContainerHasKey(content, open, close, serverName)) {
        out.ok = true;
        out.changed = false;
        return out;
    }
    std::string updated = InsertJsonEntry(content, open, close, serverName, out.entryJson);
    if (!dryRun) WriteFileAtomicWide(targetW, updated);
    out.ok = true;
    out.changed = true;
    return out;
}

} // namespace

void PrintCliUsage() {
    std::cout <<
"hancho-cli " PC_VERSION " - Hancho console client\n"
R"(
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
  HANCHO_LOG_DIR   Override the logs directory (state, logs, queues).

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
         --agent-type <claude|agy|antigravity|opencode|codex|aider|ollama>
                                  Hint worker type for JSON stream parsing (auto-detected if omitted).
         --json-stream            Enable JSONL stream parsing and write <log>.stream.jsonl for LLM worker output.
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

  worker status <id>
      Show current worker status, active tool, thoughts, message, and supervisor context.

  worker stream <id> [--tail <n>] [--type <type>] [--since <seq>] [--summary]
      View captured JSON streaming events for claude, agy, or opencode workers.

  worker info <id> [--notes "<notes>"] [--goal "<goal>"]
      Update supervisor notes or goal for an ongoing worker.

  agent install-template [--path <dir>] [--dry-run]
      Write the pc-runner subagent to <dir>\.claude\agents\pc-runner.md (default: the current
      directory). It runs long commands through Hancho and returns a short summary.
      An existing file is not overwritten.

  mcp install [--client claude|opencode] [--path <file>] [--name <name>] [--data-dir <path>] [--dry-run]
      Register Hancho as an MCP server automatically. Writes or updates
      .mcp.json (Claude Code, default) or opencode.json (--client opencode).
      --data-dir embeds "--data-dir <path>" in the generated MCP server args so the
      project keeps its logs/state/task queues inside that directory.
      --json prints the machine-readable result; --dry-run previews without writing.

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

  taskqueue list [<name>] [--state <state>] [--since <period>] [--origin <self|label|session>]
      List jobs with state, queue, summary and blockedBy. --since keeps only
      jobs updated within the period (30m, 6h, 2d), e.g. --since 6h.
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
)";
    std::cout <<
"      {\n"
"        \"mcpServers\": {\n"
"          \"hancho\": {\n"
"            \"command\": \"" << JsonEscape(ResolveGuiExePathUtf8()) << "\",\n"
"            \"args\": [\"mcp\"]\n"
"          }\n"
"        }\n"
"      }\n"
"\n"
"    AUTO-REGISTER (for LLM agents; no manual JSON editing):\n"
"      hancho-cli.exe mcp install                    # .mcp.json (Claude Code)\n"
"      hancho-cli.exe mcp install --client opencode  # opencode.json\n"
"      hancho-cli.exe mcp install --path <file> --json --dry-run\n"
"\n"
"    ORCHESTRATION MODEL (how an LLM should use this):\n"
"      A supervising LLM uses Hancho as its execution layer to run\n"
"      child-process worker LLMs/CLIs as managed jobs.\n"
"\n"
"        Supervisor LLM  ->  Hancho  ->  Worker LLM / build command\n"
"        plans + declares    timeouts, logs,        one job command;\n"
"        jobs                retries, state         writes artifacts\n"
"\n"
"      - The supervisor keeps its context small: it plans and declares jobs; it\n"
"        does not run the work itself, chain commands, or fabricate success.\n"
"      - One job = one command. Use start_process for a single worker; use\n"
"        taskqueue_add with dependsOn for multiple steps or dependencies.\n"
"      - When the next step depends on results (build -> fix -> rebuild,\n"
"        test -> fix -> retest, branch on a file's content), do not watch and\n"
"        add jobs by hand: write the plan as a behavior tree and submit it once\n"
"        with bt_start, then loop on bt_wait and answer escalations with\n"
"        bt_resume (answer + rationale). See BEHAVIOR TREES above and\n"
"        doc\\behavior-tree.md.\n"
"      - Each job needs a one-line summary and its own timeoutSec. Pass detailed\n"
"        instructions via instructionsFile/instructionsText; the worker reads\n"
"        {instructions} / PC_JOB_INSTRUCTIONS and writes artifacts.\n"
"      - Workers may be LLM CLIs (opencode run, claude -p, ...) or build scripts.\n"
"      - Role clarity: when the supervisor and worker use the same engine\n"
"        (e.g. Claude supervising `claude -p`, or opencode supervising\n"
"        `opencode run`), roles can blur and cause confusion; prefer a\n"
"        different worker engine for workers, or give workers a clearly\n"
"        distinct role/prompt and avoid nested supervision.\n"
"      - Verify success with exit code + filesNewer/outputRegex; observe with\n"
"        taskqueue_wait / taskqueue_list / taskqueue_graph and logs; retry with\n"
"        taskqueue_retry. Blocked dependents resume automatically.\n"
"      - Never poll .done files; use taskqueue_wait or wait_process.\n"
"      - Delegating to a worker LLM: run_worker blocks until the worker exits and\n"
"        returns only its final result; the raw stream stays in stream_file.\n"
"      - Your harness does not see Hancho as a native sub-agent. To have\n"
"        it shown as one, define a thin custom agent (Claude Code:\n"
"        .claude/agents/<name>.md, cheap model, this tool only) that calls run_worker\n"
"        and returns the result. The wrapper costs extra tokens; calling run_worker\n"
"        directly is cheapest.\n"
"\n"
"    MCP tools (34):\n"
"      Process:    start_process, list_processes, get_process_output,\n"
"                  send_process_input, wait_process, kill_process\n"
"      Worker:     run_worker, wait_worker, get_worker_stream, get_worker_state,\n"
"                  update_worker_info\n"
"      Task queue: taskqueue_add, taskqueue_list, taskqueue_get, taskqueue_retry,\n"
"                  taskqueue_update, taskqueue_cancel, taskqueue_pause, taskqueue_resume,\n"
"                  taskqueue_wait, taskqueue_purge, taskqueue_link, taskqueue_unlink,\n"
"                  taskqueue_graph\n"
"      Behavior tree: bt_validate, bt_start, bt_get, bt_list, bt_wait, bt_resume,\n"
"                  bt_retry, bt_update, bt_cancel, bt_pause\n"
"\n"
"NOTE:\n"
"  The GUI dashboard is provided by Hancho.exe, which is auto-started\n"
"  on first use if it is not already running.\n";
}

namespace {

void PrintCliUsageJson() {
    const std::string guiExe = JsonEscape(ResolveGuiExePathUtf8());
    const std::string dataDir = JsonEscape(WideToUtf8(GetEffectiveDataDirectoryWide()));
    std::cout << R"({
  "ok": true,
  "program": "hancho-cli",
  "version": ")" PC_VERSION R"(",
  "aliases": {},
  "description": "Hancho console client. Sends commands to the running Hancho GUI dashboard over a named pipe.",
  "usage": "hancho-cli [--json] <command> [options]",
  "effective_data_dir": ")" << dataDir << R"(",
  "global_options": [
    {"name": "--json", "alias": "-j", "description": "Print the raw machine-readable JSON response. May be placed before or after the command."},
    {"name": "--help", "alias": "-h", "description": "Show usage. Combine with --json to get this machine-readable help."},
    {"name": "--version", "alias": "-V", "description": "Show the version. Combine with --json for {\"ok\":true,\"version\":...}."}
  ],
  "data_dir_option": {
    "name": "--data-dir",
    "value": "<path>",
    "per_invocation": true,
    "description": "Store logs, state, task queues and config in <path> for this invocation only. May be placed before or after the command. Use a project-local directory to git-manage the data. Each data dir gets its own dashboard instance (separate Named Pipe and mutex), so different projects can run concurrently.",
    "examples": [
      "hancho-cli.exe --data-dir .hancho taskqueue add --file jobs.json",
      "hancho-cli.exe taskqueue list main --data-dir .hancho",
      "hancho-cli.exe mcp install --data-dir .hancho"
    ]
  },
  "concepts": {
    "process_ids": "Numeric IDs assigned by the dashboard (#1, #2, ...); use them with list/logs/send/wait/kill.",
    "data_dir": "Logs, state, task queues and config live in one data directory. Pass --data-dir <path> on every hancho-cli invocation (or embed it with hancho-cli mcp install --data-dir) to keep that data inside the current project for git. Each data directory has its own dashboard instance and Named Pipe.",
    "modes": {
      "pipe": "Default. Anonymous pipes for stdout/stdin; best for non-interactive programs and LLM CLIs. Use --close-stdin for programs that wait for EOF.",
      "pty": "Win32 ConPTY; interactive TUI (opencode/claude). --pty is a shortcut.",
      "console": "Separate visible console window."
    },
    "stdin": "Pipe mode keeps stdin writable by default (hancho-cli send). --close-stdin closes it after launch so readers receive EOF.",
    "logs": "Each process writes a live log; default logs/proc_<id>_<name>_<timestamp>.log. Use --log-file for a custom path.",
    "done_files": "On exit an atomic JSON .done file is written (default logs/proc_<id>.done) with state, exit_code, reason, log_file and done_file.",
    "cwd": "Defaults to the caller's current directory (resolved to an absolute path). PWD and INIT_CWD are set for the child; OLDPWD is removed.",
    "wait": "start --wait blocks until exit; --wait-ms 0 = unlimited. wait --id blocks for an existing process. Exit code 124 means the wait timed out.",
    "json": "Pass --json (or -j) before or after the command for a single JSON object. Errors set ok=false and exit code 1.",
    "lifecycle": "start returns immediately unless --wait; use wait --id or the .done file to detect completion."
  },
  "cli_exit_codes": {
    "0": "success / completed",
    "1": "CLI or configuration error; also a process failure without --wait",
    "124": "wait timed out while the process is still running",
    "child_exit_code": "start --wait / wait return the managed process exit code"
  },
  "environment_variables": {
    "HANCHO_LOG_DIR": "Override the logs directory (state, logs, task queues)."
  },
  "common_examples": [
    "hancho-cli.exe list --json",
    "hancho-cli.exe start --name build --cmd \"cmd /c build.cmd\" --timeout 1800 --wait --wait-ms 0 --json",
    "hancho-cli.exe start --name tui --pty --cmd \"opencode\"",
    "hancho-cli.exe logs --id 1 --tail 80",
    "hancho-cli.exe send --id 1 --input \"status\"",
    "hancho-cli.exe taskqueue add --file jobs.json --taskqueue main",
    "hancho-cli.exe mcp install --client opencode"
  ],
  "commands": [
    {
      "name": "list",
      "aliases": ["status"],
      "description": "List managed processes and their live metrics.",
      "usage": "hancho-cli list [--id <id>]",
      "options": [
        {"name": "--id", "value": "<id>", "description": "Show only the specified process ID."}
      ]
    },
    {
      "name": "logs",
      "aliases": ["output"],
      "description": "Fetch process output logs. ANSI/VT100 escape codes are stripped by default.",
      "usage": "hancho-cli logs --id <id> [--tail <lines>] [--wait-ms <ms>] [--raw]",
      "options": [
        {"name": "--id", "value": "<id>", "description": "Process ID."},
        {"name": "--tail", "value": "<lines>", "description": "Return only the last N lines (0 = all)."},
        {"name": "--wait-ms", "value": "<ms>", "description": "Wait up to N ms before reading output."},
        {"name": "--raw", "description": "Keep ANSI/VT100 escape codes."}
      ]
    },
    {
      "name": "start",
      "aliases": ["run"],
      "description": "Start a managed process under a Win32 Job Object and display it in the dashboard. Use '--' to pass the command line verbatim. Add --wait (with --wait-ms 0) to block until completion.",
      "usage": "hancho-cli start [options] [--wait] [--wait-ms <ms>] -- <command...>",
      "options": [
        {"name": "--cmd", "value": "<command>", "description": "Command line to execute."},
        {"name": "--name", "value": "<label>", "description": "Display name on the dashboard."},
        {"name": "--mode", "value": "<pty|pipe|console>", "values": ["pty", "pipe", "console"], "description": "Execution mode. Default: pipe (no ConPTY)."},
        {"name": "--pty", "description": "Enable ConPTY. Shortcut for --mode pty. Without this flag or an explicit --mode pty, no ConPTY is used."},
        {"name": "--open-stdin", "description": "Pipe mode only (default): keep stdin writable for hancho-cli send."},
        {"name": "--close-stdin", "description": "Pipe mode only: close stdin after launch so non-interactive programs (e.g. opencode run) receive EOF and can finish."},
        {"name": "--cwd", "value": "<dir>", "description": "Working directory. Default: the caller's current directory."},
        {"name": "--log-file", "aliases": ["--log"], "value": "<path>", "description": "Custom live-output log path."},
        {"name": "--done-file", "aliases": ["--done"], "value": "<path>", "description": "Custom completion marker path, written when the process exits."},
        {"name": "--timeout", "value": "<sec>", "description": "Maximum runtime in seconds (0 = unlimited)."},
        {"name": "--idle-timeout", "value": "<sec>", "description": "Kill if no output for N seconds (0 = unlimited)."},
        {"name": "--eta", "aliases": ["--estimate-end"], "value": "<time>", "description": "Estimated end time shown in gray in the End Time column while the process is running (e.g. '18:30' or '2026-10-22 18:30:00'). If omitted, --timeout causes the column to show remaining seconds."},
        {"name": "--agent-type", "value": "<claude|agy|antigravity|opencode|codex|aider|ollama>", "description": "Hint worker type for JSON stream parsing (auto-detected if omitted)."},
        {"name": "--json-stream", "description": "Enable JSONL stream parsing and write <log>.stream.jsonl for LLM worker output."},
        {"name": "--wait", "description": "Block until the process exits (unlimited unless --wait-ms is set)."},
        {"name": "--wait-ms", "value": "<ms>", "description": "With --wait: maximum wait (0 = unlimited). Without --wait: initial output wait. Default: 500."}
      ]
    },
    {
      "name": "send",
      "aliases": ["write"],
      "description": "Send text input to a running process's stdin or ConPTY session.",
      "usage": "hancho-cli send --id <id> --input \"<text>\" [--no-newline] [--wait-ms <ms>]",
      "options": [
        {"name": "--id", "value": "<id>", "description": "Process ID."},
        {"name": "--input", "value": "<text>", "description": "Text to send."},
        {"name": "--no-newline", "description": "Do not append a newline (default: append)."},
        {"name": "--wait-ms", "value": "<ms>", "description": "Wait up to N ms for output after sending. Default: 400."}
      ]
    },
    {
      "name": "wait",
      "description": "Wait until the specified process exits or the timeout elapses.",
      "usage": "hancho-cli wait --id <id> [--wait-ms <ms>]",
      "options": [
        {"name": "--id", "value": "<id>", "description": "Process ID."},
        {"name": "--wait-ms", "value": "<ms>", "description": "Maximum wait time. Default: 30000."}
      ]
    },
    {
      "name": "kill",
      "description": "Forcefully kill the entire process tree (Job Object) for a process ID.",
      "usage": "hancho-cli kill --id <id>",
      "options": [
        {"name": "--id", "value": "<id>", "description": "Process ID."}
      ]
    },
    {
      "name": "kill-all",
      "description": "Forcefully kill all running managed process trees.",
      "usage": "hancho-cli kill-all",
      "options": []
    },
    {
      "name": "clear",
      "description": "Remove finished processes from the dashboard history.",
      "usage": "hancho-cli clear",
      "options": []
    },
    {
      "name": "mcp install",
      "description": "Register Hancho as an MCP server by writing or updating a client config file (.mcp.json for Claude Code, opencode.json for opencode).",
      "usage": "hancho-cli mcp install [--client claude|opencode] [--path <file>] [--name <name>] [--data-dir <path>] [--dry-run] [--json]",
      "options": [
        {"name": "--client", "value": "<claude|opencode>", "description": "Target client. Default: claude."},
        {"name": "--path", "value": "<file>", "description": "Config file to write. Default: .mcp.json (claude) or opencode.json (opencode)."},
        {"name": "--name", "value": "<name>", "description": "MCP server name. Default: hancho."},
        {"name": "--data-dir", "value": "<path>", "description": "Embed --data-dir <path> in the generated MCP server args so this project keeps its logs/state/task queues in that directory."},
        {"name": "--dry-run", "description": "Report the change without writing the file."}
      ]
    },
    {
      "name": "taskqueue",
      "description": "Manage the persistent task queue: ordered jobs, dependencies, per-job timeouts, success checks and automatic retries.",
      "usage": "hancho-cli taskqueue <add|list|get|retry|cancel|pause|resume|graph|wait|purge> ...",
      "subcommands": [
        {"name": "add", "usage": "hancho-cli taskqueue add --file <jobs.json> [--taskqueue <name>] [--concurrency <n>] [--notify-file <path>] [--origin-label <text>]", "description": "Register jobs. Each job needs id, command, summary and timeoutSec; dependsOn replaces shell chaining. The caller's session id, working folder and --origin-label are recorded as the job's origin."},
        {"name": "list", "usage": "hancho-cli taskqueue list [<name>] [--state <state>] [--since <period>] [--origin <self|label|session>]", "description": "List jobs with state, queue, summary and blockedBy. --since keeps only jobs updated within the period (e.g. 30m, 6h, 2d). --origin self keeps only the jobs this session added."},
        {"name": "get", "usage": "hancho-cli taskqueue get <id> [--rev <n>] [--no-instructions]", "description": "Show one job with attempts and instructions."},
        {"name": "retry", "usage": "hancho-cli taskqueue retry <id> | hancho-cli taskqueue retry --failed [<name>] [--since <period>]", "description": "Requeue a failed/blocked/cancelled job; blocked dependents resume automatically. --failed retries every failed job instead, optionally limited to one queue and to jobs that failed within --since (e.g. --since 6h = last 6 hours)."},
        {"name": "cancel", "usage": "hancho-cli taskqueue cancel <id>", "description": "Cancel a job; a running attempt is killed and is not retried."},
        {"name": "pause", "usage": "hancho-cli taskqueue pause <name>", "description": "Stop starting new jobs in the queue; running jobs continue."},
        {"name": "resume", "usage": "hancho-cli taskqueue resume <name>", "description": "Resume starting jobs in the queue."},
        {"name": "graph", "usage": "hancho-cli taskqueue graph [<name>] [--root <id>]", "description": "Dependency graph with ready, waiting_for, resource_wait, retry_in_sec, blocked and dependents."},
        {"name": "wait", "usage": "hancho-cli taskqueue wait <name> [--since <version>] [--timeout-ms <ms>]", "description": "Long-poll until the queue version changes."},
        {"name": "purge", "usage": "hancho-cli taskqueue purge <name> [--states succeeded,failed,cancelled]", "description": "Remove finished jobs."}
      ],
      "options": []
    },)" R"(
    {
      "name": "bt",
      "description": "Behavior trees: submit a whole multi-step workflow (branches, fix loops, checks, worker LLM steps, escalations) as one JSON document; Hancho runs it and each leaf becomes an ordinary task-queue job. See agent.behavior_tree and doc/behavior-tree.md.",
      "usage": "hancho-cli bt <validate|start|get|list|wait|resume|retry|update|cancel|pause> ...",
      "subcommands": [
        {"name": "validate", "usage": "hancho-cli bt validate --file <bt.json> | --bt <json>", "description": "Check a tree without running it."},
        {"name": "start", "usage": "hancho-cli bt start --file <bt.json> | --bt <json>", "description": "Register and start a tree."},
        {"name": "wait", "usage": "hancho-cli bt wait --id <id> [--since-version <n>] [--timeout-ms <ms>]", "description": "Long-poll until the tree finishes or waits for a decision (state waitingForLlm, with the decision packet)."},
        {"name": "resume", "usage": "hancho-cli bt resume --id <id> --node <node> --answer <answer> --rationale <text> [--vars <json>]", "description": "Answer an escalate node; rationale is required."},
        {"name": "get", "usage": "hancho-cli bt get --id <id> [--compact]", "description": "Node states, runs, variables and jobs; --compact prints one line per node."},
        {"name": "list", "usage": "hancho-cli bt list", "description": "All trees and their states."},
        {"name": "retry", "usage": "hancho-cli bt retry --id <id> [--from <node>] [--reset-counters]", "description": "Resume a failed tree from the failed leaf or the given node."},
        {"name": "update", "usage": "hancho-cli bt update --id <id> [--node <node>] --file <path> | --bt <json> | --patch <json>", "description": "Replace the definition of non-running nodes; the tree shape cannot change."},
        {"name": "cancel", "usage": "hancho-cli bt cancel --id <id>", "description": "Cancel the tree and its running jobs."},
        {"name": "pause", "usage": "hancho-cli bt pause --id <id> [--paused true|false]", "description": "Stop or resume starting new leaves; running jobs continue."}
      ],
      "options": []
    },
    {
      "name": "worker",
      "description": "Inspect and supervise LLM workers (Claude, agy, opencode, codex, aider, ollama) running under Hancho. JSON stream events are parsed into <log>.stream.jsonl and visible in the GUI (LLM JSON Stream dialog). Use hancho-cli/Hancho to supervise and watch streams.",
      "usage": "hancho-cli worker <status|stream|info|install-template> [<id>] [options]",
      "subcommands": [
        {"name": "status", "usage": "hancho-cli worker status <id>", "description": "Display current worker status, model, active tool, thoughts, message, supervisor context, and Claude sub-agents (subagents array)."},
        {"name": "stream", "usage": "hancho-cli worker stream <id> [--tail <n>] [--type <type>] [--since <seq>] [--summary]", "description": "View captured JSON streaming events from <log>.stream.jsonl (parsed tool_call/tool_result/thinking/message, subagent_start/subagent_end with parent_tool_use_id and agent_id)."},
        {"name": "info", "usage": "hancho-cli worker info <id> [--notes <notes>] [--goal <goal>]", "description": "Update supervisor instructions, notes, or goals."},
        {"name": "install-template", "usage": "hancho-cli worker install-template [--path <dir>] [--dry-run] [--json]", "description": "Write .claude/agents/pc-runner.md (a Claude Code subagent that runs long commands through Hancho) under <dir>. Default: the current directory. An existing file is not overwritten."}
      ],
      "options": [
        {"name": "--tail", "value": "<n>", "description": "For stream: number of recent events."},
        {"name": "--type", "value": "<type>", "description": "For stream: filter by event type (tool_call/tool_result/thinking/message/...)."},
        {"name": "--since", "value": "<seq>", "description": "For stream: only events with seq > N."},
        {"name": "--summary", "description": "For stream: show summary view."},
        {"name": "--path", "value": "<dir>", "description": "For install-template: project directory that receives .claude/agents/pc-runner.md. Default: current directory."},
        {"name": "--dry-run", "description": "For install-template: print the target without writing it."},
        {"name": "--notes", "value": "<text>", "description": "For info: supervisor notes."},
        {"name": "--goal", "value": "<text>", "description": "For info: supervisor goal."}
      ]
    },
    {
      "name": "help",
      "description": "Show command usage.",
      "usage": "hancho-cli help [--json]",
      "options": []
    },
    {
      "name": "version",
      "description": "Show the hancho-cli version.",
      "usage": "hancho-cli version [--json]",
      "options": []
    }
  ],
  "completion": {
    "description": "Use --wait to keep hancho-cli.exe alive until the managed process exits. The .done file is written atomically on exit. Without --wait, start returns immediately.",
    "blocking_start": "hancho-cli.exe start ... --wait --wait-ms 0",
    "blocking_wait": "hancho-cli.exe wait --id <id> --wait-ms 0",
    "wait_option": "--wait",
    "wait_ms_option": "--wait-ms <ms>; 0 means unlimited with --wait",
    "exit_codes": {
      "0": "completed",
      "child_exit_code": "failed",
      "124": "wait timed out while the process is still running"
    },
    "done_file": "JSON file written on process exit with state, exit_code, reason, log_file, and done_file"
  },
  "setup": {
    "cli": {
      "recommended_binary": "hancho-cli.exe",
      "description": "Use hancho-cli.exe for command-line control. It locates and auto-starts the Hancho GUI dashboard if needed.",
      "invocation": "hancho-cli.exe [--json] <command> [options]",
      "help_command": "hancho-cli.exe help --json"
    },
    "mcp": {
      "description": "Register Hancho.exe as an MCP stdio server so LLM clients can call its tools directly.",
      "transport": "stdio",
      "config_files": [".mcp.json", "claude_desktop_config.json"],
      "command": ")" << guiExe << R"(",
      "args": ["mcp"],
      "auto_install": {
        "description": "Writes or updates the MCP server entry automatically. Recommended for LLM agents.",
        "argv": [")" << guiExe << R"(", "mcp", "install"],
        "command": ")" << guiExe << R"( mcp install",
        "clients": {
          "claude": {"default_path": ".mcp.json", "container": "mcpServers"},
          "opencode": {"default_path": "opencode.json", "container": "mcp"}
        }
      },
      "config_example": {
        "mcpServers": {
          "hancho": {
            "command": ")" << guiExe << R"(",
            "args": ["mcp"]
          }
        }
      },
      "tools": ["start_process", "list_processes", "get_process_output", "send_process_input", "wait_process", "kill_process", "run_worker", "wait_worker", "get_worker_stream", "get_worker_state", "update_worker_info", "taskqueue_add", "taskqueue_list", "taskqueue_get", "taskqueue_retry", "taskqueue_update", "taskqueue_cancel", "taskqueue_pause", "taskqueue_resume", "taskqueue_wait", "taskqueue_purge", "taskqueue_link", "taskqueue_unlink", "taskqueue_graph", "bt_validate", "bt_start", "bt_get", "bt_list", "bt_wait", "bt_resume", "bt_retry", "bt_update", "bt_cancel", "bt_pause"]
    }
  },
  "agent": {
    "purpose": "Reliable execution of CLI workers and build jobs: one OS command per job with timeouts, tree kill, logs, completion markers, and a persistent dependency-aware task queue.",
    "llm_stream_setup": {
      "description": "To see live worker state/stream in the GUI and parsed events, enable JSONL streaming for LLM CLIs.",
      "hancho-cli_start_example": "hancho-cli start --name opencode --cmd \"opencode run ...\" --json-stream --agent-type opencode --wait --wait-ms 0 --json",
      "notes": [
        "Hancho auto-detects common worker CLIs (claude, agy/antigravity, opencode, codex, aider, ollama) and enables JSON stream parsing if detected.",
        "Parsed events are written to <logfile>.stream.jsonl. Use GUI context menu 'LLM JSON Stream Events...' or 'hancho-cli worker stream <id>'.",
        "Model is captured from stream fields (model/modelId/llm.model/metadata.model) when present; hover the agent icon in the stream dialog to see it.",
        "Format reference: doc/LLM-jsonstream-format.md"
      ]
    },
    "working_model": {
      "summary": "A supervising LLM uses Hancho as its execution layer to run child-process worker LLMs/CLIs as managed jobs.",
      "roles": {
        "supervisor": "The LLM the user talks to. Plans work, declares jobs and dependencies, observes results and retries. It does not run the work itself, chain commands or fabricate success.",
        "hancho": "Owns processes: timeouts, tree kill, logs, completion markers, persistence and retries.",
        "worker": "A child process, often another LLM CLI such as opencode or claude, that performs one job command and writes artifacts and logs."
      },
      "flow": [
        "The supervisor decides the steps and the success condition for each step.",
        "The supervisor registers one job per step with summary and timeoutSec, using dependsOn for order.",
        "Hancho launches workers as managed processes when dependencies and slots allow.",
        "Workers write files and logs; Hancho verifies exit code + filesNewer/outputRegex.",
        "The supervisor observes with taskqueue_wait, taskqueue_list and taskqueue_graph, and reads logs and artifacts.",
        "After fixing a failure the supervisor calls taskqueue_retry; blocked dependents resume automatically."
      ],
      "rules": [
        "One job = one command; never use &&, ||, &, PowerShell ';', newlines or wait loops.",
        "Give each job its own timeoutSec; never sum timeouts.",
        "Prefer filesNewer/outputRegex when exit code 0 does not prove the work was done.",
        "Keep the supervisor context small: pass detailed instructions to workers and read artifacts, not raw transcripts.",
        "Use taskqueue_wait instead of polling .done files.",
        "To delegate one task to a worker LLM, use run_worker: it blocks until the worker exits and returns only the final result; the raw stream stays in stream_file.",
        "Harnesses do not see Hancho as a native sub-agent. To have it shown as one, define a thin custom agent (Claude Code: .claude/agents/<name>.md, cheap model, this tool only) that calls run_worker and returns the result. The wrapper costs extra tokens; calling run_worker directly is cheapest.",
        "Never force success; Hancho verifies it."
      ],
      "example": {
        "note": "A supervisor registers worker jobs; each worker is an ordinary process, often an LLM CLI, that writes artifacts.",
        "jobs": [
          {"id": "research", "command": "tools\\run-worker.cmd {instructions} {summary}", "summary": "Research and write artifacts/research.md", "timeoutSec": 1800, "instructionsFile": "tasks/research.md", "success": {"filesNewer": ["artifacts/research.md"]}},
          {"id": "build", "command": "tools\\run-worker.cmd {instructions} {summary}", "summary": "Build using the research", "timeoutSec": 3600, "dependsOn": ["research"], "instructionsFile": "tasks/build.md", "success": {"filesNewer": ["dist/app.exe"]}}
        ]
      }
    },
    "preconditions": [
      "Windows 10/11 x64; same user session as the Hancho dashboard",
      "The dashboard auto-starts on first use; administrator rights are not required",
      "State, logs and task queues live under the logs directory. Override it via HANCHO_LOG_DIR.",
      "One dashboard instance per Windows session owns the Named Pipe server",
      "A job is exactly one command; shell chains and polling loops are rejected"
    ],
    "cli": {
      "recommended_binary": "hancho-cli.exe",
      "legacy": "Hancho.exe <command> has the same implementation",
      "json_mode": "Add --json or -j (before or after the command) for one JSON object.",
      "data_dir": "Pass --data-dir <path> on each hancho-cli invocation (or embed it with hancho-cli mcp install --data-dir <path>) to keep logs/state/task queues inside the current project. Each data dir gets its own dashboard instance, so different projects can run concurrently.",
      "auto_start": "hancho-cli.exe auto-starts the GUI dashboard when it is not running."
    },
    "register_mcp": {
      "command": "hancho-cli.exe mcp install",
      "opencode": "hancho-cli.exe mcp install --client opencode",
      "preview": "hancho-cli.exe mcp install --dry-run --json",
      "note": "Run once per project/client; restart or reload the LLM client afterwards."
    },
    "decision": {
      "single_command": "start_process",
      "multiple_commands_or_dependencies": "taskqueue_add",
      "next_step_depends_on_results": "bt_start + bt_wait (+ bt_resume): branches, build-fix loops, test-fix-retest, result checks, worker LLM steps or decisions you must make mid-way. Do not watch and add jobs by hand.",
      "never": ["&&", "||", "single &", "powershell ;", "newline chains", "polling loops such as Test-Path or goto"],
      "reason": "One job = one command, so each job keeps its own timeout, success check and retry; chains are rejected."
    },
    "taskqueue_job": {
      "required": ["id", "command", "summary", "timeoutSec"],
      "optional": ["cwd", "env", "dependsOn", "idle", "maxAttempts", "retryDelaySec", "defer", "success", "killExclude", "resources", "priority", "instructionsFile", "instructionsText"],
      "summary": "One line, max 200 chars; exported as PC_JOB_SUMMARY and substituted for {summary}.",
      "timeoutSec": "Per job; > 0 and <= queue maxTimeoutSec (default 7200). Never summed.",
      "success": "exitCode (default 0) AND filesNewer (exist and newer than the attempt start) AND outputRegex (last 200 log lines).",
      "idle": "outputSec and cpuSec must both be set; kill only when output AND CPU are idle.",
      "defer": "{exitCodes (default [75]), delaySec (60), limit (60, 0 = no limit)}: exit codes meaning nothing ran; the job is re-run after delaySec without using up maxAttempts. {\"exitCodes\": []} turns it off."
    },
)" R"(    "behavior_tree": {
      "summary": "One JSON document describing the whole workflow. Hancho ticks it on job completion and timers, adds each leaf as an ordinary task-queue job, and returns to you only when a decision is needed (escalate) or the tree finishes.",
      "when": "Use instead of taskqueue_add when the next step depends on results: if/else, build -> fix -> rebuild, test -> fix -> retest, file/JSON/log checks, worker LLM steps, or a decision you must make mid-way.",
      "flow": [
        "Optionally bt_validate, then bt_start with {bt: {...}} or {file: \"plan.json\"}.",
        "Loop on bt_wait {id, sinceVersion, timeoutMs, compact}; it returns on finish or when state is waitingForLlm with a decision packet. Pass compact=true to omit the heavy tree JSON and save tokens. It returns at once on those events, so long timeouts are fine: timeoutMs defaults to 600000 for the MCP tool (a client cancel stops it) and 60000 for hancho-cli bt wait; on timeout it returns only {state, version, nodeRuns, timedOut: true}.",
        "Answer with bt_resume {id, node, answer, rationale}. The packet contains the intent, question, guidance, allowed answers, path, and the logs/vars/files you listed in include, so you can decide without earlier context.",
        "On failure read bt_get {id, compact: true}; fix the cause (bt_update changes a non-running node's definition) and call bt_retry {id, from}."
      ],
      "nodes": {
        "leaves": "job {command, summary, timeoutSec, success, capture}, worker {prompt, summary, timeoutSec, engine, model, outputSchema}, wait {sec}, set {var, value}, escalate {question, guidance, expect, include, timeoutSec, fallback {afterSec, engine, model}}, fail, succeed",
        "composites": "sequence, fallback (selector), parallel {success: all|any|n}, if {cond, then, else}",
        "decorators": "retry {max}, repeat {max, until}, invert, forceSuccess, forceFailure, timeout {sec}",
        "conditions": "fileExists, fileNewer, outputMatches, exitCode, jsonValue, var, nodeState"
      },
      "variables": "${name} expands BT vars; in command it is quoted as one argument. Node variables: ${<id>.state|exitCode|logFile|jobId|runs}, ${<worker>.result} (final response), ${<worker>.output.<path>} (JSON the worker wrote to PC_BT_RESULT_FILE), ${<escalate>.answer}.",
      "engines": "Workers name an engine (claude, agy, opencode, codex, or ones in <data dir>/engines.json) and optionally a model; permission flags live only in the engine definition.",
      "limits": "retry/repeat max <= 100 (required); limits.maxNodeRuns default 200; limits.maxWallSec default 86400; depth 32; 1000 nodes.",
      "rules": [
        "Write intent (the goal) and, on escalate, question + guidance for your future self.",
        "Do not use taskqueue_retry/taskqueue_update on BT jobs; use bt_retry/bt_update.",
        "subtree is not supported yet."
      ],
      "example": {"id": "fix-build", "summary": "Build and fix", "intent": "Release build passes", "cwd": "D:/repo",
        "root": {"type": "retry", "max": 3, "child": {"type": "fallback", "children": [
          {"type": "job", "id": "build", "summary": "Build", "timeoutSec": 1800, "command": "cmake --build build --config Release"},
          {"type": "sequence", "children": [
            {"type": "worker", "id": "fix", "engine": "claude", "summary": "Fix build", "timeoutSec": 3600, "prompt": "Fix the build errors in ${build.logFile}."},
            {"type": "fail"}]}]}}},
      "guide": "doc/behavior-tree.md (English) and doc/behavior-tree-ja.md (Japanese) in the Hancho installation folder; hancho-cli bt --help for the CLI."
    },
    "states": {
      "waiting": "Queued and ready; waiting for a free slot or the retry delay.",
      "pending": "Not in the run queue yet; dependencies are not satisfied.",
      "blocked": "A success-dependency failed/cancelled; fix and retry the dependency.",
      "running": "An attempt is executing.",
      "succeeded": "Success conditions met.",
      "failed": "Attempts exhausted or launch failed.",
      "cancelled": "Cancelled; not retried."
    },
    "quick_start": [
      {"step": "register_mcp", "command": "hancho-cli.exe mcp install"},
      {"step": "add_jobs", "tool": "taskqueue_add", "args": {"taskqueue": "main", "jobs": [{"id": "A", "command": "run A", "summary": "Prepare inputs", "timeoutSec": 1800}, {"id": "B", "command": "run B", "summary": "Build outputs", "timeoutSec": 3600, "dependsOn": ["A"]}]}},
      {"step": "observe", "tool": "taskqueue_wait", "args": {"taskqueue": "main", "sinceVersion": 0, "timeoutMs": 30000}},
      {"step": "inspect", "tools": ["taskqueue_list", "taskqueue_graph", "taskqueue_get"]},
      {"step": "recover", "tool": "taskqueue_retry", "args": {"id": "A", "refreshInstructions": true}}
    ],
    "environment_variables": {
      "PC_JOB_ID": "Job id",
      "PC_ATTEMPT": "1-based attempt number",
      "PC_JOB_SUMMARY": "Job summary",
      "PC_JOB_INSTRUCTIONS": "Path to the materialized instructions revision",
      "PC_JOB_INSTRUCTIONS_REV": "Instructions revision",
      "PC_PREV_REASON": "Previous attempt reason for retries",
      "PC_PREV_EXIT": "Previous exit code for retries",
      "PC_PREV_TAIL": "Path to the previous attempt's last 50 log lines"
    },
    "data_dir_consistency": {
      "problem": "--data-dir is per invocation, so different calls could accidentally use different directories.",
      "guarantee": [
        "Choose one project-local directory, for example <project>/.hancho.",
        "Register the MCP server once with it: hancho-cli.exe mcp install --data-dir .hancho. This embeds --data-dir into the project's MCP config, so every MCP tool call for that project uses the same directory.",
        "Pass the same --data-dir on every direct hancho-cli invocation.",
        "Relative paths resolve against the caller's current directory; use an absolute path in the MCP config if the cwd is uncertain.",
        "Different data directories are isolated: each has its own dashboard instance, Named Pipe and mutex, and they never share processes or queues."
      ],
    "verify": "hancho-cli.exe --data-dir <path> help --json returns effective_data_dir; the dashboard status bar shows the same path.",
    "llm_stream": {
      "help": "Run 'hancho-cli help --json' and look under agent.llm_stream_setup for instructions to enable JSONL streaming and view the stream window."
    }
  },
    "notes": [
      "Use filesNewer when exit code 0 does not prove the work was done.",
      "Use dependsOn instead of waiting for .done files or polling.",
      "taskqueue_wait is a long poll; it returns when the queue version changes.",
      "kill_process on a task-queue attempt means 'killed attempt' and is retried (use taskqueue_cancel to stop it).",
      "The project may change between calls: pass --data-dir <project>/.hancho on every hancho-cli invocation, or embed it once with hancho-cli mcp install --data-dir .hancho."
    ]
  },
  "notes": [
    "The GUI dashboard (Hancho.exe) is started automatically if it is not already running.",
    "Process IDs are shown in the dashboard and returned by the start and list commands.",
    "With --json, every response is a single JSON object. Errors have \"ok\": false and set exit code 1.",
    "Run 'hancho-cli help --json' at any time to retrieve this machine-readable usage and setup information."
  ]
})" << "\n";
}

bool FindJsonValuePos(const std::string& json, const std::string& key, size_t& outPos) {
    const std::string pattern = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = json.find(pattern, pos)) != std::string::npos) {
        size_t afterKey = pos + pattern.size();
        while (afterKey < json.size() && std::isspace(static_cast<unsigned char>(json[afterKey]))) {
            afterKey++;
        }
        if (afterKey < json.size() && json[afterKey] == ':') {
            afterKey++;
            while (afterKey < json.size() && std::isspace(static_cast<unsigned char>(json[afterKey]))) {
                afterKey++;
            }
            outPos = afterKey;
            return true;
        }
        pos += pattern.size();
    }
    return false;
}

std::string ExtractJsonObject(const std::string& json, const std::string& key) {
    size_t pos = 0;
    if (!FindJsonValuePos(json, key, pos) || pos >= json.size() || json[pos] != '{') {
        return "";
    }

    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (size_t i = pos; i < json.size(); ++i) {
        char c = json[i];
        if (inString) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inString = false;
            continue;
        }
        if (c == '"') {
            inString = true;
        } else if (c == '{') {
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0) {
                return json.substr(pos, i - pos + 1);
            }
        }
    }
    return "";
}

std::vector<std::string> ExtractJsonObjectArray(const std::string& json, const std::string& key) {
    std::vector<std::string> objects;
    size_t pos = 0;
    if (!FindJsonValuePos(json, key, pos) || pos >= json.size() || json[pos] != '[') {
        return objects;
    }

    size_t i = pos + 1;
    while (i < json.size()) {
        while (i < json.size() && (std::isspace(static_cast<unsigned char>(json[i])) || json[i] == ',')) {
            i++;
        }
        if (i >= json.size() || json[i] == ']') break;
        if (json[i] != '{') {
            i++;
            continue;
        }

        size_t start = i;
        int depth = 0;
        bool inString = false;
        bool escaped = false;
        for (; i < json.size(); ++i) {
            char c = json[i];
            if (inString) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') inString = false;
                continue;
            }
            if (c == '"') {
                inString = true;
            } else if (c == '{') {
                depth++;
            } else if (c == '}') {
                depth--;
                if (depth == 0) {
                    objects.push_back(json.substr(start, i - start + 1));
                    i++;
                    break;
                }
            }
        }
    }
    return objects;
}

std::string UpperAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return value;
}

std::string ProcessStateLabel(const std::string& procJson) {
    std::string state = UpperAscii(JsonGetString(procJson, "state", "unknown"));
    if (state == "RUNNING" && JsonGetInt(procJson, "idle_ms", 0) >= 10000) {
        state = "IDLE";
    }
    int exitCode = static_cast<int>(JsonGetInt(procJson, "exit_code", STILL_ACTIVE));
    if (state != "RUNNING" && state != "IDLE" && exitCode != STILL_ACTIVE) {
        state += " exit=" + std::to_string(exitCode);
    }
    return state;
}

void PrintProcessLine(const std::string& procJson, const std::string& indent = "  ") {
    std::cout << indent << "#" << JsonGetInt(procJson, "id", 0);
    std::string name = JsonGetString(procJson, "name", "");
    if (!name.empty()) std::cout << " " << name;
    std::cout << " [" << ProcessStateLabel(procJson) << "]"
              << " pid=" << JsonGetInt(procJson, "pid", 0)
              << " procs=" << JsonGetInt(procJson, "tree_procs", 0)
              << " mode=" << JsonGetString(procJson, "mode", "")
              << " uptime=" << JsonGetString(procJson, "uptime_str", "")
              << " cpu=" << JsonGetString(procJson, "cpu_time_str", "")
              << " mem=" << JsonGetString(procJson, "memory_str", "");

    std::string state = JsonGetString(procJson, "state", "");
    if (state == "running") {
        uint64_t idleMs = static_cast<uint64_t>(JsonGetInt(procJson, "idle_ms", 0));
        std::cout << " idle=" << FormatDurationSec(idleMs);
    }
    std::string agentType = JsonGetString(procJson, "agent_type", "");
    if (!agentType.empty()) {
        std::cout << " [" << agentType << "]";
    }
    std::cout << "\n";
}

void PrintProcessDetails(const std::string& procJson, const std::string& indent = "       ") {
    std::string command = JsonGetString(procJson, "command", "");
    std::string cwd = JsonGetString(procJson, "cwd", "");
    std::string start = JsonGetString(procJson, "start_time", "");
    std::string end = JsonGetString(procJson, "end_time", "");
    std::string cpu = JsonGetString(procJson, "cpu_time_str", "");
    std::string logFile = JsonGetString(procJson, "log_file", "");
    std::string doneFile = JsonGetString(procJson, "done_file", "");
    std::string lastLine = JsonGetString(procJson, "last_line", "");

    if (!command.empty()) std::cout << indent << "cmd : " << command << "\n";
    if (!cwd.empty()) std::cout << indent << "cwd : " << cwd << "\n";
    if (!start.empty()) std::cout << indent << "start: " << start << "\n";
    if (!end.empty()) std::cout << indent << "end  : " << end << "\n";
    if (!cpu.empty()) std::cout << indent << "cpu  : " << cpu << "\n";
    if (!logFile.empty()) std::cout << indent << "log : " << logFile << "\n";
    if (!doneFile.empty()) std::cout << indent << "done: " << doneFile << "\n";
    if (!lastLine.empty()) std::cout << indent << "last: " << lastLine << "\n";

    std::string agentType = JsonGetString(procJson, "agent_type", "");
    if (!agentType.empty()) {
        std::string agentSt = JsonGetString(procJson, "agent_status", "");
        std::cout << indent << "agent: [" << agentType << "] " << (agentSt.empty() ? "active" : agentSt) << "\n";
        std::string curTool = JsonGetString(procJson, "current_tool", "");
        if (!curTool.empty()) {
            std::string curAction = JsonGetString(procJson, "current_tool_action", "");
            std::cout << indent << "tool : " << curTool;
            if (!curAction.empty()) std::cout << " (" << curAction << ")";
            std::cout << "\n";
        }
        std::string goal = JsonGetString(procJson, "supervisor_goal", "");
        if (!goal.empty()) std::cout << indent << "goal : " << goal << "\n";
    }
}

void PrintOutput(const std::string& text, const std::string& label = "Output") {
    if (text.empty()) {
        std::cout << label << ": (empty)\n";
        return;
    }
    std::cout << label << ":\n" << text;
    if (text.back() != '\n') std::cout << "\n";
}

std::string BuildTransportError(const std::string& message) {
    return "{\"ok\":false,\"error\":\"" + JsonEscape(message) + "\"}";
}

int CompletionExitCode(const std::string& procJson) {
    std::string state = JsonGetString(procJson, "state", "");
    if (state == "completed") return 0;
    if (state == "running") return 124;
    int64_t ec = JsonGetInt(procJson, "exit_code", 1);
    if (ec == 0 || ec == STILL_ACTIVE) return 1;
    return static_cast<int>(ec);
}

int PrintResponse(const std::string& action, const std::string& respJson, bool jsonOutput,
                  int requestedId = 0, bool waitForCompletion = false) {
    if (jsonOutput) {
        std::cout << respJson << "\n";
        bool ok = JsonGetBool(respJson, "ok", false);
        if (!ok) return 1;
        std::string proc = ExtractJsonObject(respJson, "process");
        if (action == "start" && waitForCompletion && !proc.empty()) {
            return CompletionExitCode(proc);
        }
        if (action == "wait") {
            if (!JsonGetBool(respJson, "finished", false)) return 124;
            if (!proc.empty()) return CompletionExitCode(proc);
        }
        return 0;
    }

    if (!JsonGetBool(respJson, "ok", false)) {
        std::cerr << "Error: " << JsonGetString(respJson, "error", "Unknown error.") << "\n";
        return 1;
    }

    if (action.rfind("taskqueue_", 0) == 0) {
        if (action == "taskqueue_list") {
            auto jobs = ExtractJsonObjectArray(respJson, "jobs");
            if (jobs.empty()) {
                std::cout << "No task queue jobs found.\n";
                return 0;
            }
            std::cout << "Task queue jobs (" << jobs.size() << "):\n\n";
            for (const auto& j : jobs) {
                std::cout << "  " << JsonGetString(j, "id", "")
                          << " [" << JsonGetString(j, "state", "") << "]"
                          << " queue=" << JsonGetString(j, "taskqueue", "")
                          << " max_attempts=" << JsonGetInt(j, "max_attempts", 0)
                          << " :: " << JsonGetString(j, "summary", "");
                std::string notes = JsonGetString(j, "notes", "");
                if (!notes.empty()) std::cout << "  notes=" << notes;
                std::string blockedBy = JsonGetString(j, "blocked_by", "");
                if (!blockedBy.empty()) std::cout << " (blocked by " << blockedBy << ")";
                std::cout << "\n";
            }
            return 0;
        }
        std::cout << respJson << "\n";
        return 0;
    }

    if (action.rfind("bt_", 0) == 0) {
        if (action == "bt_validate") {
            std::cout << "Behavior tree definition is valid.\n";
            return 0;
        }
        if (action == "bt_list") {
            auto trees = ExtractJsonObjectArray(respJson, "trees");
            if (trees.empty()) {
                std::cout << "No behavior trees found.\n";
                return 0;
            }
            std::cout << "Behavior trees (" << trees.size() << "):\n\n";
            for (const auto& t : trees) {
                std::cout << "  [" << JsonGetString(t, "state", "idle") << "] "
                          << JsonGetString(t, "id", "");
                std::string sum = JsonGetString(t, "summary", "");
                if (!sum.empty()) std::cout << " \"" << sum << "\"";
                std::cout << "  runs=" << JsonGetInt(t, "nodeRuns", 0)
                          << "  version=" << JsonGetInt(t, "version", 0);
                std::string detail = JsonGetString(t, "detail", "");
                if (!detail.empty()) std::cout << "  detail=" << detail;
                std::string upd = JsonGetString(t, "updatedAt", "");
                if (!upd.empty()) std::cout << "  " << upd;
                std::cout << "\n";
            }
            return 0;
        }
        std::string compact = JsonGetString(respJson, "compact", "");
        if (!compact.empty()) {
            std::cout << compact << "\n";
        } else {
            std::cout << "[" << JsonGetString(respJson, "state", "") << "] "
                      << JsonGetString(respJson, "id", "");
            if (respJson.find("\"timedOut\":true") != std::string::npos) {
                std::cout << "  (timed out; version=" << JsonGetInt(respJson, "version", 0)
                          << ", runs=" << JsonGetInt(respJson, "nodeRuns", 0) << ")";
            }
            std::cout << "\n";
        }
        std::string esc = ExtractJsonObject(respJson, "escalation");
        if (!esc.empty()) {
            std::cout << "Escalation (" << JsonGetString(esc, "node", "") << "): "
                      << JsonGetString(esc, "question", "") << "\n";
        }
        return 0;
    }

    if (action == "list") {
        auto processes = ExtractJsonObjectArray(respJson, "processes");
        if (processes.empty()) {
            if (requestedId > 0) {
                std::cout << "Process #" << requestedId << " not found.\n";
            } else {
                std::cout << "No managed processes found.\n";
            }
            return 0;
        }
        std::cout << "Managed processes (" << processes.size() << "):\n\n";
        for (const auto& proc : processes) {
            PrintProcessLine(proc);
            PrintProcessDetails(proc);
            std::cout << "\n";
        }
        return 0;
    }

    std::string proc = ExtractJsonObject(respJson, "process");

    if (action == "start") {
        std::cout << "Started process";
        if (!proc.empty()) {
            PrintProcessLine(proc, " ");
            PrintProcessDetails(proc);
        } else {
            std::cout << "\n";
        }
        std::string logFile = JsonGetString(respJson, "log_file", "");
        if (!logFile.empty()) std::cout << "       log : " << logFile << "\n";
        PrintOutput(JsonGetString(respJson, "output", ""));
        if (waitForCompletion && !proc.empty()) {
            return CompletionExitCode(proc);
        }
        return 0;
    }

    if (action == "logs") {
        if (!proc.empty()) {
            PrintProcessLine(proc);
            PrintProcessDetails(proc);
            std::cout << "\n";
        }
        PrintOutput(JsonGetString(respJson, "output", ""));
        return 0;
    }

    if (action == "send") {
        std::cout << "Sent input to process #" << JsonGetInt(respJson, "id", 0) << ".\n";
        if (!proc.empty()) {
            PrintProcessLine(proc);
            std::cout << "\n";
        }
        PrintOutput(JsonGetString(respJson, "recent_output", ""), "Recent output");
        return 0;
    }

    if (action == "wait") {
        int id = proc.empty() ? 0 : static_cast<int>(JsonGetInt(proc, "id", 0));
        bool finished = JsonGetBool(respJson, "finished", false);
        if (finished) {
            std::cout << "Process #" << id << " finished.\n";
        } else {
            std::cout << "Process #" << id << " is still running (wait timed out).\n";
        }
        if (!proc.empty()) {
            PrintProcessLine(proc);
            PrintProcessDetails(proc);
            std::cout << "\n";
        }
        PrintOutput(JsonGetString(respJson, "output", ""));
        if (!finished) return 124;
        if (!proc.empty()) return CompletionExitCode(proc);
        return 0;
    }

    if (action == "kill") {
        std::cout << "Killed process #" << JsonGetInt(respJson, "killed_id", 0) << ".\n";
        return 0;
    }

    if (action == "kill_all") {
        std::cout << "Killed " << JsonGetInt(respJson, "killed_count", 0) << " running process(es).\n";
        return 0;
    }

    if (action == "clear") {
        std::cout << "Cleared " << JsonGetInt(respJson, "cleared_count", 0) << " finished process(es).\n";
        return 0;
    }

    if (action == "get_agent_stream") {
        std::string summary = JsonGetString(respJson, "summary", "");
        if (!summary.empty()) {
            std::cout << summary << "\n";
            return 0;
        }
        std::string stateObj = ExtractJsonObject(respJson, "state");
        if (!stateObj.empty()) {
            std::cout << "Worker State: [" << JsonGetString(stateObj, "agent_type", "") << "] "
                      << JsonGetString(stateObj, "status", "") << "\n";
            std::string curTool = JsonGetString(stateObj, "current_tool", "");
            if (!curTool.empty()) {
                std::cout << "  Active Tool: " << curTool << " (" << JsonGetString(stateObj, "current_tool_action", "") << ")\n";
            }
        }
        auto events = ExtractJsonObjectArray(respJson, "events");
        std::cout << "Events (" << events.size() << "):\n";
        for (const auto& ev : events) {
            std::cout << "  #" << JsonGetInt(ev, "seq", 0) << " [" << JsonGetString(ev, "type", "") << "]";
            std::string tn = JsonGetString(ev, "tool_name", "");
            if (!tn.empty()) std::cout << " tool=" << tn;
            std::string ta = JsonGetString(ev, "tool_action", "");
            if (!ta.empty()) std::cout << " (" << ta << ")";
            std::string cnt = JsonGetString(ev, "content", "");
            if (!cnt.empty()) {
                if (cnt.size() > 80) cnt = cnt.substr(0, 77) + "...";
                std::cout << " : " << cnt;
            }
            std::cout << "\n";
        }
        return 0;
    }

    if (action == "get_agent_state") {
        std::cout << "Worker #" << JsonGetInt(respJson, "id", 0)
                  << " [" << JsonGetString(respJson, "agent_type", "generic") << "] "
                  << "Status: " << JsonGetString(respJson, "status", "idle") << "\n";
        std::string curTool = JsonGetString(respJson, "current_tool", "");
        if (!curTool.empty()) {
            std::cout << "  Active Tool : " << curTool << " (" << JsonGetString(respJson, "current_tool_action", "") << ")\n";
        }
        std::string lastThought = JsonGetString(respJson, "last_thought", "");
        if (!lastThought.empty()) {
            std::cout << "  Thinking    : " << lastThought << "\n";
        }
        std::string lastMsg = JsonGetString(respJson, "last_message", "");
        if (!lastMsg.empty()) {
            std::cout << "  Message     : " << lastMsg << "\n";
        }
        std::cout << "  Steps       : " << JsonGetInt(respJson, "step_count", 0) << "\n"
                  << "  Events      : " << JsonGetInt(respJson, "event_count", 0) << "\n";
        std::string streamFile = JsonGetString(respJson, "stream_file", "");
        if (!streamFile.empty()) {
            std::cout << "  Stream File : " << streamFile << "\n";
        }
        std::string goal = JsonGetString(respJson, "supervisor_goal", "");
        if (!goal.empty()) {
            std::cout << "  Goal        : " << goal << "\n";
        }
        std::string notes = JsonGetString(respJson, "supervisor_notes", "");
        if (!notes.empty()) {
            std::cout << "  Notes       : " << notes << "\n";
        }
        auto subagents = ExtractJsonObjectArray(respJson, "subagents");
        if (!subagents.empty()) {
            std::cout << "  Subagents   : " << subagents.size() << "\n";
            for (const auto& sub : subagents) {
                std::cout << "    [" << JsonGetString(sub, "status", "") << "] "
                          << JsonGetString(sub, "subagent_type", "") << " "
                          << JsonGetString(sub, "description", "")
                          << " (tokens " << JsonGetInt(sub, "total_tokens", 0)
                          << ", tool uses " << JsonGetInt(sub, "tool_uses", 0) << ")\n";
            }
        }
        return 0;
    }

    if (action == "update_agent_info") {
        std::cout << "Updated supervisor info for worker #" << JsonGetInt(respJson, "id", 0) << ".\n";
        return 0;
    }

    std::cout << respJson << "\n";
    return 0;
}

} // namespace

// hancho-cli reset [--yes]: deletes every entry under the project data directory.
// Without --yes it only lists the targets. Refused while Hancho is running for that directory.
static int RunProjectReset(const std::vector<std::string>& args, bool jsonOutput) {
    auto fail = [&](const std::string& msg) {
        if (jsonOutput) std::cout << "{\"ok\":false,\"error\":\"" << JsonEscape(msg) << "\"}\n";
        else std::cerr << "Error: " << msg << "\n";
        return 1;
    };
    bool yes = false;
    for (size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--yes") yes = true;
        else return fail("unknown option for reset: " + args[i] + " (use: hancho-cli reset [--yes])");
    }
    if (IsHanchoRunningForDataDirWide()) {
        return fail("Hancho is running for this data directory. Quit it (hancho-cli quit) and retry.");
    }
    auto entries = ListProjectDataEntriesWide();
    std::string dirUtf8 = WideToUtf8(GetEffectiveDataDirectoryWide());
    if (!yes) {
        if (jsonOutput) {
            std::cout << "{\"ok\":true,\"executed\":false,\"data_dir\":\"" << JsonEscape(dirUtf8) << "\",\"targets\":[";
            for (size_t i = 0; i < entries.size(); ++i) {
                if (i > 0) std::cout << ",";
                std::cout << "\"" << JsonEscape(WideToUtf8(entries[i])) << "\"";
            }
            std::cout << "]}\n";
        } else {
            std::cout << "Data directory: " << dirUtf8 << "\n";
            std::cout << "The following " << entries.size() << " entries will be DELETED:\n";
            for (const auto& e : entries) std::cout << "  " << WideToUtf8(e) << "\n";
            std::cout << "Nothing was deleted. Re-run with --yes to reset.\n";
        }
        return 0;
    }
    std::string err;
    size_t removed = ResetProjectDataWide(err);
    if (!err.empty()) return fail("reset stopped after " + std::to_string(removed) + " entries: " + err);
    if (jsonOutput) std::cout << "{\"ok\":true,\"executed\":true,\"removed\":" << removed << "}\n";
    else std::cout << "Reset complete: removed " << removed << " entries from " << dirUtf8 << "\n";
    return 0;
}

int RunCli(const std::vector<std::string>& rawArgs) {
    std::vector<std::string> args;
    args.push_back(rawArgs.empty() ? "hancho-cli" : rawArgs[0]);

    bool jsonOutput = false;
    bool waitForCompletion = false;
    std::string dataDirArg;
    for (size_t i = 1; i < rawArgs.size(); ++i) {
        if (rawArgs[i] == "--json" || rawArgs[i] == "-j") {
            jsonOutput = true;
        } else if (rawArgs[i] == "--data-dir" && i + 1 < rawArgs.size()) {
            dataDirArg = rawArgs[++i];
        } else if (rawArgs[i].rfind("--data-dir=", 0) == 0) {
            dataDirArg = rawArgs[i].substr(11);
        } else {
            args.push_back(rawArgs[i]);
        }
    }
    if (!dataDirArg.empty()) {
        SetDataDirectoryOverride(Utf8ToWide(dataDirArg));
    }

    std::string subcmd = (args.size() >= 2) ? args[1] : "";
    if (subcmd.empty()) {
        if (jsonOutput) PrintCliUsageJson();
        else PrintCliUsage();
        return 1;
    }
    if (subcmd == "--help" || subcmd == "-h" || subcmd == "help") {
        if (jsonOutput) PrintCliUsageJson();
        else PrintCliUsage();
        return 0;
    }
    if (subcmd == "--version" || subcmd == "-V" || subcmd == "version") {
        if (jsonOutput) std::cout << "{\"ok\":true,\"program\":\"hancho-cli\",\"version\":\"" PC_VERSION "\"}" << std::endl;
        else std::cout << "hancho-cli " PC_VERSION << std::endl;
        return 0;
    }

    std::ostringstream reqJson;
    int requestedId = 0;

    if (subcmd == "reset") return RunProjectReset(args, jsonOutput);

if (subcmd == "start" || subcmd == "run") {
        std::string name;
        std::string cmd;
        std::string cwd;
        std::string logFile;
        std::string doneFile;
        std::string mode = "pipe";
        bool openStdin = true;
        int timeoutSec = 0;
        int idleTimeoutSec = 0;
        std::string estimateEnd;
        std::string agentType;
        bool jsonStream = false;
        std::string supervisorNotes;
        std::string supervisorGoal;
        bool waitFinish = false;
        bool waitMsExplicit = false;
        bool allowChain = false;
        int waitMs = 500;

        for (size_t i = 2; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (a == "--name" && i + 1 < args.size()) {
                name = args[++i];
            } else if (a == "--cmd" && i + 1 < args.size()) {
                cmd = args[++i];
            } else if (a == "--cwd" && i + 1 < args.size()) {
                cwd = args[++i];
            } else if ((a == "--log-file" || a == "--log") && i + 1 < args.size()) {
                logFile = args[++i];
            } else if ((a == "--done-file" || a == "--done") && i + 1 < args.size()) {
                doneFile = args[++i];
            } else if (a == "--mode" && i + 1 < args.size()) {
                mode = args[++i];
            } else if (a == "--pty") {
                mode = "pty";
            } else if (a == "--open-stdin") {
                openStdin = true;
            } else if (a == "--close-stdin") {
                openStdin = false;
            } else if (a == "--timeout" && i + 1 < args.size()) {
                timeoutSec = std::stoi(args[++i]);
            } else if (a == "--idle-timeout" && i + 1 < args.size()) {
                idleTimeoutSec = std::stoi(args[++i]);
            } else if ((a == "--eta" || a == "--estimate-end") && i + 1 < args.size()) {
                estimateEnd = args[++i];
            } else if (a == "--agent" && i + 1 < args.size()) {
                agentType = args[++i];
            } else if (a == "--stream") {
                jsonStream = true;
            } else if (a == "--notes" && i + 1 < args.size()) {
                supervisorNotes = args[++i];
            } else if (a == "--goal" && i + 1 < args.size()) {
                supervisorGoal = args[++i];
            } else if (a == "--wait") {
                waitFinish = true;
                waitForCompletion = true;
            } else if (a == "--allow-chain") {
                allowChain = true;
            } else if (a == "--wait-ms" && i + 1 < args.size()) {
                waitMs = std::stoi(args[++i]);
                waitMsExplicit = true;
            } else if (a == "--") {
                std::ostringstream cmdOss;
                for (size_t j = i + 1; j < args.size(); ++j) {
                    if (j > i + 1) cmdOss << " ";
                    if (args[j].find(' ') != std::string::npos) {
                        cmdOss << "\"" << args[j] << "\"";
                    } else {
                        cmdOss << args[j];
                    }
                }
                cmd = cmdOss.str();
                break;
            } else if (cmd.empty() && a[0] != '-') {
                cmd = a;
            }
        }

        if (cmd.empty()) {
            if (jsonOutput) {
                std::cout << "{\"ok\":false,\"error\":\"Missing --cmd or command arguments.\"}\n";
            } else {
                std::cerr << "Error: Missing --cmd or command arguments.\n";
            }
            return 1;
        }

        cwd = ResolveAbsolutePathUtf8(cwd);
        if (waitFinish && !waitMsExplicit) {
            waitMs = 0;
        }

        reqJson << "{\"action\":\"start\""
                << ",\"name\":\"" << JsonEscape(name) << "\""
                << ",\"cmd\":\"" << JsonEscape(cmd) << "\""
                << ",\"cwd\":\"" << JsonEscape(cwd) << "\""
                << ",\"log_file\":\"" << JsonEscape(logFile) << "\""
                << ",\"done_file\":\"" << JsonEscape(doneFile) << "\""
                << ",\"mode\":\"" << JsonEscape(mode) << "\""
                << ",\"open_stdin\":" << (openStdin ? "true" : "false")
                << ",\"timeout_sec\":" << timeoutSec
                << ",\"idle_timeout_sec\":" << idleTimeoutSec
                << ",\"estimate_end\":\"" << JsonEscape(estimateEnd) << "\""
                << ",\"agent_type\":\"" << JsonEscape(agentType) << "\""
                << ",\"json_stream\":" << (jsonStream ? "true" : "false")
                << ",\"supervisor_notes\":\"" << JsonEscape(supervisorNotes) << "\""
                << ",\"supervisor_goal\":\"" << JsonEscape(supervisorGoal) << "\""
                << ",\"wait\":" << (waitFinish ? "true" : "false")
                << ",\"wait_ms\":" << waitMs
                << ",\"allow_chain\":" << (allowChain ? "true" : "false") << "}";
    }
    else if (subcmd == "list" || subcmd == "status") {
        int id = 0;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--id" && i + 1 < args.size()) id = std::stoi(args[++i]);
            else if (id == 0 && args[i][0] != '-') { try { id = std::stoi(args[i]); } catch (...) {} }
        }
        reqJson << "{\"action\":\"list\",\"id\":" << id << "}";
        requestedId = id;
        subcmd = "list";
    }
    else if (subcmd == "logs" || subcmd == "output") {
        int id = 0;
        int tail = 0;
        int waitMs = 0;
        bool raw = false;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--id" && i + 1 < args.size()) id = std::stoi(args[++i]);
            else if (args[i] == "--tail" && i + 1 < args.size()) tail = std::stoi(args[++i]);
            else if (args[i] == "--wait-ms" && i + 1 < args.size()) waitMs = std::stoi(args[++i]);
            else if (args[i] == "--raw") raw = true;
            else if (id == 0 && args[i][0] != '-') { try { id = std::stoi(args[i]); } catch (...) {} }
        }
        reqJson << "{\"action\":\"logs\",\"id\":" << id
                << ",\"tail\":" << tail
                << ",\"wait_ms\":" << waitMs
                << ",\"raw\":" << (raw ? "true" : "false") << "}";
        requestedId = id;
        subcmd = "logs";
    }
    else if (subcmd == "send" || subcmd == "write") {
        int id = 0;
        std::string input;
        bool newline = true;
        int waitMs = 400;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--id" && i + 1 < args.size()) id = std::stoi(args[++i]);
            else if (args[i] == "--input" && i + 1 < args.size()) input = args[++i];
            else if (args[i] == "--no-newline") newline = false;
            else if (args[i] == "--wait-ms" && i + 1 < args.size()) waitMs = std::stoi(args[++i]);
            else if (id == 0 && args[i][0] != '-') { try { id = std::stoi(args[i]); } catch (...) {} }
            else if (input.empty() && args[i][0] != '-') input = args[i];
        }
        reqJson << "{\"action\":\"send\",\"id\":" << id
                << ",\"input\":\"" << JsonEscape(input) << "\""
                << ",\"newline\":" << (newline ? "true" : "false")
                << ",\"wait_ms\":" << waitMs << "}";
        requestedId = id;
        subcmd = "send";
    }
    else if (subcmd == "wait") {
        int id = 0;
        int waitMs = 30000;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--id" && i + 1 < args.size()) id = std::stoi(args[++i]);
            else if (args[i] == "--wait-ms" && i + 1 < args.size()) waitMs = std::stoi(args[++i]);
            else if (id == 0 && args[i][0] != '-') { try { id = std::stoi(args[i]); } catch (...) {} }
        }
        reqJson << "{\"action\":\"wait\",\"id\":" << id << ",\"wait_ms\":" << waitMs << "}";
        requestedId = id;
    }
    else if (subcmd == "kill") {
        int id = 0;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--id" && i + 1 < args.size()) id = std::stoi(args[++i]);
            else if (id == 0 && args[i][0] != '-') { try { id = std::stoi(args[i]); } catch (...) {} }
        }
        reqJson << "{\"action\":\"kill\",\"id\":" << id << "}";
        requestedId = id;
    }
    else if (subcmd == "kill-all") {
        reqJson << "{\"action\":\"kill_all\"}";
        subcmd = "kill_all";
    }
    else if (subcmd == "clear") {
        reqJson << "{\"action\":\"clear\"}";
    }
    else if (subcmd == "mcp") {
        std::string op = (args.size() >= 3) ? args[2] : "";
        if (op != "install") {
            std::string msg = "usage: hancho-cli mcp install [--client claude|opencode] [--path <file>] [--name <name>] [--dry-run]";
            if (jsonOutput) std::cout << "{\"ok\":false,\"error\":\"" << JsonEscape(msg) << "\"}\n";
            else std::cerr << msg << "\n";
            return 1;
        }
        std::string client = "claude";
        std::string installPath;
        std::string serverName = "hancho";
        bool dryRun = false;
        for (size_t i = 3; i < args.size(); ++i) {
            if ((args[i] == "--client" || args[i] == "-c") && i + 1 < args.size()) client = args[++i];
            else if ((args[i] == "--path" || args[i] == "-p") && i + 1 < args.size()) installPath = args[++i];
            else if (args[i] == "--name" && i + 1 < args.size()) serverName = args[++i];
            else if (args[i] == "--dry-run") dryRun = true;
        }
        if (client != "claude" && client != "opencode") {
            std::string msg = "unknown client '" + client + "' (expected claude or opencode)";
            if (jsonOutput) std::cout << "{\"ok\":false,\"error\":\"" << JsonEscape(msg) << "\"}\n";
            else std::cerr << "Error: " << msg << "\n";
            return 1;
        }
        McpInstallOutcome outcome = InstallMcpConfig(client, installPath, serverName, dryRun,
                                                     ResolveGuiExePathUtf8(), dataDirArg);
        if (!outcome.ok) {
            if (jsonOutput) {
                std::cout << "{\"ok\":false,\"error\":\"" << JsonEscape(outcome.error) << "\"}\n";
            } else {
                std::cerr << "Error: " << outcome.error << "\n";
            }
            return 1;
        }
        if (jsonOutput) {
            std::cout << "{\"ok\":true"
                      << ",\"client\":\"" << JsonEscape(client) << "\""
                      << ",\"path\":\"" << JsonEscape(outcome.targetPath) << "\""
                      << ",\"server\":\"" << JsonEscape(serverName) << "\""
                      << ",\"changed\":" << (outcome.changed ? "true" : "false")
                      << ",\"dry_run\":" << (outcome.dryRun ? "true" : "false")
                      << ",\"entry\":" << outcome.entryJson
                      << "}\n";
        } else {
            if (outcome.dryRun) {
                std::cout << "[dry-run] Would " << (outcome.changed ? "register" : "keep")
                          << " MCP server '" << serverName << "' in " << outcome.targetPath << "\n";
            } else if (outcome.changed) {
                std::cout << "Registered MCP server '" << serverName << "' in " << outcome.targetPath << "\n";
            } else {
                std::cout << "MCP server '" << serverName << "' is already registered in " << outcome.targetPath << "\n";
            }
            std::cout << "Entry: " << outcome.entryJson << "\n";
            if (outcome.changed && !outcome.dryRun) {
                std::cout << "Restart your LLM client (or reload its MCP servers) to pick it up.\n";
            }
        }
        return 0;
    }
    else if (subcmd == "taskqueue") {
        std::string op = (args.size() >= 3) ? args[2] : "";
        auto argVal = [&args](const std::string& flag, const std::string& def) -> std::string {
            for (size_t i = 3; i + 1 < args.size(); ++i) {
                if (args[i] == flag) return args[i + 1];
            }
            return def;
        };
        auto firstPositional = [&args](size_t startIdx) -> std::string {
            for (size_t i = startIdx; i < args.size(); ++i) {
                if (!args[i].empty() && args[i][0] != '-') return args[i];
            }
            return "";
        };
        auto failWith = [&](const std::string& msg) {
            if (jsonOutput) std::cout << "{\"ok\":false,\"error\":\"" << JsonEscape(msg) << "\"}\n";
            else std::cerr << "Error: " << msg << "\n";
            return 1;
        };
        // First non-flag argument, skipping the values of flags that take one (e.g. "--since 6h").
        auto positionalSkippingValues = [&args](size_t startIdx) -> std::string {
            static const char* const valueFlags[] = {"--state", "--since", "--origin", "--rev", "--root", "--states", "--older-than-days"};
            for (size_t i = startIdx; i < args.size(); ++i) {
                bool isValueFlag = false;
                for (const char* f : valueFlags) {
                    if (args[i] == f) { isValueFlag = true; break; }
                }
                if (isValueFlag) { ++i; continue; }
                if (!args[i].empty() && args[i][0] != '-') return args[i];
            }
            return "";
        };

        if (op == "add") {
            std::string file = argVal("--file", "");
            std::string tqName = argVal("--taskqueue", "main");
            int concurrency = 0;
            std::string concStr = argVal("--concurrency", "");
            if (!concStr.empty()) {
                try { concurrency = std::stoi(concStr); } catch (...) { concurrency = 0; }
            }
            std::string notifyFile = argVal("--notify-file", "");
            if (file.empty()) return failWith("--file is required.");
            std::string content = ReadFileUtf8Wide(ResolveAbsolutePathWide(Utf8ToWide(file)));
            if (content.empty()) return failWith("cannot read jobs file: " + file);

            std::string jobsRaw;
            size_t first = content.find_first_not_of(" \t\r\n");
            if (first != std::string::npos && content[first] == '[') {
                jobsRaw = content.substr(first);
            } else {
                auto objs = ExtractJsonObjectArray(content, "jobs");
                if (objs.empty()) return failWith("jobs array not found in " + file);
                std::ostringstream ja;
                ja << "[";
                for (size_t k = 0; k < objs.size(); ++k) {
                    if (k) ja << ",";
                    ja << objs[k];
                }
                ja << "]";
                jobsRaw = ja.str();
                if (notifyFile.empty()) notifyFile = JsonGetString(content, "notify_file", "");
                if (tqName == "main") {
                    std::string t = JsonGetString(content, "taskqueue", "");
                    if (!t.empty()) tqName = t;
                }
                if (concurrency == 0) concurrency = static_cast<int>(JsonGetInt(content, "concurrency", 0));
            }
            reqJson << "{\"action\":\"taskqueue_add\",\"taskqueue\":\"" << JsonEscape(tqName) << "\""
                    << ",\"jobs\":" << jobsRaw
                    << ",\"concurrency\":" << concurrency
                    << ",\"notify_file\":\"" << JsonEscape(notifyFile) << "\""
                    << ",\"origin\":" << BuildCallerOriginJson("hancho-cli-cli", argVal("--origin-label", "")) << "}";
            subcmd = "taskqueue_add";
        } else if (op == "list") {
            std::string tqName = positionalSkippingValues(3);
            std::string state = argVal("--state", "");
            std::string since = argVal("--since", "");
            std::string originFilter = argVal("--origin", "");
            if (originFilter == "self") originFilter = CallerSelfOriginFilter();
            reqJson << "{\"action\":\"taskqueue_list\",\"taskqueue\":\"" << JsonEscape(tqName)
                    << "\",\"state\":\"" << JsonEscape(state) << "\""
                    << ",\"since\":\"" << JsonEscape(since) << "\""
                    << ",\"origin\":\"" << JsonEscape(originFilter) << "\"}";
            subcmd = "taskqueue_list";
        } else if (op == "get") {
            std::string id = firstPositional(3);
            int rev = 0;
            std::string revStr = argVal("--rev", "");
            if (!revStr.empty()) { try { rev = std::stoi(revStr); } catch (...) {} }
            bool includeInstr = true;
            for (size_t i = 3; i < args.size(); ++i) {
                if (args[i] == "--no-instructions") includeInstr = false;
            }
            reqJson << "{\"action\":\"taskqueue_get\",\"id\":\"" << JsonEscape(id)
                    << "\",\"include_instructions\":" << (includeInstr ? "true" : "false")
                    << ",\"rev\":" << rev << "}";
            subcmd = "taskqueue_get";
        } else if (op == "retry") {
            bool failedMode = false;
            for (size_t i = 3; i < args.size(); ++i) {
                if (args[i] == "--failed") failedMode = true;
            }
            if (failedMode) {
                // Bulk: every failed job, optionally limited to one queue and to the last --since period.
                std::string tqName = positionalSkippingValues(3);
                std::string since = argVal("--since", "");
                reqJson << "{\"action\":\"taskqueue_retry\",\"id\":\"\",\"failed\":true"
                        << ",\"taskqueue\":\"" << JsonEscape(tqName) << "\""
                        << ",\"since\":\"" << JsonEscape(since) << "\""
                        << ",\"reset_attempts\":true,\"refresh_instructions\":true}";
            } else {
                std::string id = firstPositional(3);
                reqJson << "{\"action\":\"taskqueue_retry\",\"id\":\"" << JsonEscape(id)
                        << "\",\"reset_attempts\":true,\"refresh_instructions\":true}";
            }
            subcmd = "taskqueue_retry";
        } else if (op == "cancel") {
            std::string id = firstPositional(3);
            reqJson << "{\"action\":\"taskqueue_cancel\",\"id\":\"" << JsonEscape(id) << "\"}";
            subcmd = "taskqueue_cancel";
        } else if (op == "pause" || op == "resume") {
            std::string tqName = firstPositional(3);
            reqJson << "{\"action\":\"taskqueue_" << op << "\",\"taskqueue\":\"" << JsonEscape(tqName) << "\"}";
            subcmd = "taskqueue_" + op;
        } else if (op == "graph") {
            std::string tqName = firstPositional(3);
            std::string root = argVal("--root", "");
            reqJson << "{\"action\":\"taskqueue_graph\",\"taskqueue\":\"" << JsonEscape(tqName)
                    << "\",\"root\":\"" << JsonEscape(root) << "\"}";
            subcmd = "taskqueue_graph";
        } else if (op == "wait") {
            std::string tqName = firstPositional(3);
            int since = 0;
            int timeoutMs = 30000;
            std::string sinceStr = argVal("--since", "");
            if (!sinceStr.empty()) { try { since = std::stoi(sinceStr); } catch (...) {} }
            std::string tmoStr = argVal("--timeout-ms", "");
            if (!tmoStr.empty()) { try { timeoutMs = std::stoi(tmoStr); } catch (...) {} }
            reqJson << "{\"action\":\"taskqueue_wait\",\"taskqueue\":\"" << JsonEscape(tqName)
                    << "\",\"since_version\":" << since << ",\"timeout_ms\":" << timeoutMs << "}";
            subcmd = "taskqueue_wait";
        } else if (op == "purge" && !argVal("--older-than-days", "").empty()) {
            // Age-based purge: finished jobs not updated for N days (all queues unless a name is given).
            std::string daysStr = argVal("--older-than-days", "");
            long long days = 0;
            try { days = std::stoll(daysStr); } catch (...) { days = -1; }
            if (days < 0) return failWith("--older-than-days requires a non-negative integer");
            // Positional name, skipping the value that follows --older-than-days.
            std::string tqName;
            for (size_t i = 3; i < args.size(); ++i) {
                if (args[i] == "--older-than-days") { ++i; continue; }
                if (!args[i].empty() && args[i][0] != '-') { tqName = args[i]; break; }
            }
            reqJson << "{\"action\":\"taskqueue_purge\",\"taskqueue\":\"" << JsonEscape(tqName)
                    << "\",\"older_than_sec\":" << (days * 24 * 60 * 60) << "}";
            subcmd = "taskqueue_purge";
        } else if (op == "purge") {
            std::string tqName = firstPositional(3);
            std::string statesStr = argVal("--states", "succeeded,failed,cancelled");
            std::ostringstream ja;
            ja << "[";
            size_t pos = 0;
            bool firstState = true;
            while (pos <= statesStr.size()) {
                size_t comma = statesStr.find(',', pos);
                std::string st = statesStr.substr(pos, (comma == std::string::npos) ? std::string::npos : comma - pos);
                if (!st.empty()) {
                    if (!firstState) ja << ",";
                    firstState = false;
                    ja << "\"" << JsonEscape(st) << "\"";
                }
                if (comma == std::string::npos) break;
                pos = comma + 1;
            }
            ja << "]";
            reqJson << "{\"action\":\"taskqueue_purge\",\"taskqueue\":\"" << JsonEscape(tqName)
                    << "\",\"states\":" << ja.str() << "}";
            subcmd = "taskqueue_purge";
        } else {
            return failWith("unknown taskqueue command: " + op +
                            " (add, list, get, retry, cancel, pause, resume, graph, wait, purge)");
        }
    }
    else if (subcmd == "bt") {
        auto printBtHelp = []() {
            std::cout
                << "Usage: hancho-cli bt <subcommand> [options] [--json]\n\n"
                << "Subcommands:\n"
                << "  validate  --file <path> | --bt <json>              Validate a behavior tree JSON definition\n"
                << "  start     --file <path> | --bt <json>              Register and start a behavior tree\n"
                << "  get       --id <id> [--compact]                    Get behavior tree state and node status\n"
                << "  list                                               List behavior trees\n"
                << "  wait      --id <id> [--since-version <n>] [--timeout-ms <ms>]\n"
                << "                                                     Wait for completion or escalation\n"
                << "  resume    --id <id> --node <node> --answer <ans> --rationale <text> [--vars <json>]\n"
                << "                                                     Answer an active escalate node\n"
                << "  retry     --id <id> [--from <node>] [--reset-counters]\n"
                << "                                                     Retry from failed leaf or specified node\n"
                << "  update    --id <id> [--node <node>] (--file <path> | --bt <json> | --patch <json>)\n"
                << "                                                     Replace definition of non-running nodes\n"
                << "  cancel    --id <id>                                Cancel a behavior tree and its running jobs\n"
                << "  pause     --id <id> [--paused true|false]          Pause or resume a behavior tree\n\n"
                << "See doc/behavior-tree.md for the full JSON format and guide.\n";
        };

        std::string op = (args.size() >= 3) ? args[2] : "";
        bool wantsHelp = (op.empty() || op == "--help" || op == "-h" || op == "help");
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--help" || args[i] == "-h") wantsHelp = true;
        }
        if (wantsHelp) {
            printBtHelp();
            return 0;
        }

        auto failWith = [&](const std::string& msg) {
            if (jsonOutput) std::cout << "{\"ok\":false,\"error\":\"" << JsonEscape(msg) << "\"}\n";
            else std::cerr << "Error: " << msg << "\n";
            return 1;
        };

        std::string id;
        std::string file;
        std::string btRaw;
        std::string patchRaw;
        std::string node;
        std::string answer;
        std::string rationale;
        std::string varsRaw;
        std::string fromNode;
        bool resetCounters = false;
        bool compact = false;
        bool paused = true;
        uint64_t sinceVersion = 0;
        int timeoutMs = 60000;

        for (size_t i = 3; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (a == "--id" && i + 1 < args.size()) id = args[++i];
            else if (a == "--file" && i + 1 < args.size()) file = args[++i];
            else if (a == "--bt" && i + 1 < args.size()) btRaw = args[++i];
            else if (a == "--patch" && i + 1 < args.size()) patchRaw = args[++i];
            else if (a == "--node" && i + 1 < args.size()) node = args[++i];
            else if (a == "--answer" && i + 1 < args.size()) answer = args[++i];
            else if (a == "--rationale" && i + 1 < args.size()) rationale = args[++i];
            else if (a == "--vars" && i + 1 < args.size()) varsRaw = args[++i];
            else if (a == "--from" && i + 1 < args.size()) fromNode = args[++i];
            else if (a == "--reset-counters") {
                resetCounters = true;
                if (i + 1 < args.size() && (args[i + 1] == "true" || args[i + 1] == "false")) {
                    resetCounters = (args[++i] == "true");
                }
            } else if (a == "--compact") {
                compact = true;
                if (i + 1 < args.size() && (args[i + 1] == "true" || args[i + 1] == "false")) {
                    compact = (args[++i] == "true");
                }
            } else if (a == "--paused") {
                if (i + 1 < args.size() && args[i + 1][0] != '-') {
                    std::string pv = args[++i];
                    paused = (pv != "false" && pv != "0");
                } else {
                    paused = true;
                }
            } else if ((a == "--since-version" || a == "--since") && i + 1 < args.size()) {
                try { sinceVersion = static_cast<uint64_t>(std::stoull(args[++i])); } catch (...) {}
            } else if ((a == "--timeout-ms" || a == "--wait-ms") && i + 1 < args.size()) {
                try { timeoutMs = std::stoi(args[++i]); } catch (...) {}
            } else if (!a.empty() && a[0] != '-') {
                if ((op == "validate" || op == "start") && file.empty() && btRaw.empty()) file = a;
                else if (id.empty()) id = a;
            }
        }

        auto loadBtText = [&](std::string& outText) -> bool {
            if (!btRaw.empty()) {
                outText = btRaw;
                return true;
            }
            if (!patchRaw.empty()) {
                outText = patchRaw;
                return true;
            }
            if (!file.empty()) {
                std::wstring absW = ResolveAbsolutePathWide(Utf8ToWide(file));
                std::ifstream ifs(absW, std::ios::binary);
                if (!ifs) return false;
                std::ostringstream ss;
                ss << ifs.rdbuf();
                outText = ss.str();
                return true;
            }
            return false;
        };

        nlohmann::json jReq;
        if (op == "validate" || op == "start") {
            std::string text;
            if (!loadBtText(text)) {
                return failWith(file.empty() ? (op + " requires --file <path> or --bt <json>")
                                             : ("cannot read file: " + file));
            }
            jReq["action"] = "bt_" + op;
            jReq["bt"] = text;
            if (op == "start") jReq["origin"] = nlohmann::json::parse(BuildCallerOriginJson("hancho-cli-cli", ""), nullptr, false);
            subcmd = "bt_" + op;
        } else if (op == "get") {
            if (id.empty()) return failWith("get requires --id <id>");
            jReq["action"] = "bt_get";
            jReq["id"] = id;
            jReq["compact"] = compact;
            subcmd = "bt_get";
        } else if (op == "list") {
            jReq["action"] = "bt_list";
            subcmd = "bt_list";
        } else if (op == "wait") {
            if (id.empty()) return failWith("wait requires --id <id>");
            jReq["action"] = "bt_wait";
            jReq["id"] = id;
            jReq["sinceVersion"] = sinceVersion;
            jReq["timeoutMs"] = timeoutMs;
            jReq["compact"] = compact;
            subcmd = "bt_wait";
        } else if (op == "resume") {
            if (id.empty()) return failWith("resume requires --id <id>");
            if (node.empty()) return failWith("resume requires --node <node>");
            if (answer.empty()) return failWith("resume requires --answer <answer>");
            if (rationale.empty()) return failWith("resume requires --rationale <rationale>");
            jReq["action"] = "bt_resume";
            jReq["id"] = id;
            jReq["node"] = node;
            jReq["answer"] = answer;
            jReq["rationale"] = rationale;
            if (!varsRaw.empty()) {
                auto vj = nlohmann::json::parse(varsRaw, nullptr, false);
                if (vj.is_discarded() || !vj.is_object()) {
                    return failWith("--vars must be a valid JSON object");
                }
                jReq["vars"] = std::move(vj);
            }
            subcmd = "bt_resume";
        } else if (op == "retry") {
            if (id.empty()) return failWith("retry requires --id <id>");
            jReq["action"] = "bt_retry";
            jReq["id"] = id;
            jReq["from"] = fromNode;
            jReq["resetCounters"] = resetCounters;
            subcmd = "bt_retry";
        } else if (op == "update") {
            if (id.empty()) return failWith("update requires --id <id>");
            std::string text;
            if (!loadBtText(text)) {
                return failWith(file.empty() ? "update requires --file <path>, --bt <json>, or --patch <json>"
                                             : ("cannot read file: " + file));
            }
            jReq["action"] = "bt_update";
            jReq["id"] = id;
            jReq["node"] = node;
            jReq["bt"] = text;
            subcmd = "bt_update";
        } else if (op == "cancel") {
            if (id.empty()) return failWith("cancel requires --id <id>");
            jReq["action"] = "bt_cancel";
            jReq["id"] = id;
            subcmd = "bt_cancel";
        } else if (op == "pause") {
            if (id.empty()) return failWith("pause requires --id <id>");
            jReq["action"] = "bt_pause";
            jReq["id"] = id;
            jReq["paused"] = paused;
            subcmd = "bt_pause";
        } else {
            return failWith("unknown bt command: " + op +
                            " (validate, start, get, list, wait, resume, retry, update, cancel, pause)");
        }
        reqJson << jReq.dump();
    }
    else if (subcmd == "worker" || subcmd == "agent") {  // agent: name used up to v2.3.1
        std::string op = (args.size() >= 3 && args[2][0] != '-') ? args[2] : "status";
        size_t startIdx = (args.size() >= 3 && args[2][0] != '-') ? 3 : 2;
        // Local file write only: no process ID and no IPC.
        if (op == "install-template") return RunAgentInstallTemplate(args, startIdx, jsonOutput);
        int id = 0;
        int tail = 50;
        std::string typeFilter;
        uint64_t sinceSeq = 0;
        std::string format = "json";
        std::string notes;
        std::string goal;

        for (size_t i = startIdx; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (a == "--id" && i + 1 < args.size()) {
                id = std::stoi(args[++i]);
            } else if (a == "--tail" && i + 1 < args.size()) {
                tail = std::stoi(args[++i]);
            } else if (a == "--type" && i + 1 < args.size()) {
                typeFilter = args[++i];
            } else if (a == "--since" && i + 1 < args.size()) {
                sinceSeq = static_cast<uint64_t>(std::stoull(args[++i]));
            } else if (a == "--summary") {
                format = "summary";
            } else if (a == "--format" && i + 1 < args.size()) {
                format = args[++i];
            } else if (a == "--notes" && i + 1 < args.size()) {
                notes = args[++i];
            } else if (a == "--goal" && i + 1 < args.size()) {
                goal = args[++i];
            } else if (id == 0 && a[0] != '-') {
                try { id = std::stoi(a); } catch (...) {}
            }
        }

        if (id <= 0) {
            if (jsonOutput) {
                std::cout << "{\"ok\":false,\"error\":\"Process ID required for worker command (e.g. hancho-cli worker status 1).\"}\n";
            } else {
                std::cerr << "Error: Process ID required for worker command (e.g. hancho-cli worker status 1).\n";
            }
            return 1;
        }

        requestedId = id;
        if (op == "status" || op == "state") {
            reqJson << "{\"action\":\"get_agent_state\",\"id\":" << id << "}";
            subcmd = "get_agent_state";
        } else if (op == "stream" || op == "events") {
            reqJson << "{\"action\":\"get_agent_stream\",\"id\":" << id
                    << ",\"tail\":" << tail
                    << ",\"type\":\"" << JsonEscape(typeFilter) << "\""
                    << ",\"since_seq\":" << sinceSeq
                    << ",\"format\":\"" << JsonEscape(format) << "\"}";
            subcmd = "get_agent_stream";
        } else if (op == "info" || op == "update") {
            reqJson << "{\"action\":\"update_agent_info\",\"id\":" << id
                    << ",\"supervisor_notes\":\"" << JsonEscape(notes) << "\""
                    << ",\"supervisor_goal\":\"" << JsonEscape(goal) << "\"}";
            subcmd = "update_agent_info";
        } else {
            if (jsonOutput) {
                std::cout << "{\"ok\":false,\"error\":\"Unknown worker command: " << JsonEscape(op) << " (status, stream, info)\"}\n";
            } else {
                std::cerr << "Error: Unknown worker command: " << op << " (status, stream, info)\n";
            }
            return 1;
        }
    }
    else {
        if (jsonOutput) {
            std::cout << "{\"ok\":false,\"error\":\"Unknown command: " << JsonEscape(subcmd)
                      << "\",\"hint\":\"Run 'hancho-cli help --json' to list commands and setup instructions.\"}\n";
        } else {
            std::cerr << "Unknown command: " << subcmd << "\n\n";
            PrintCliUsage();
        }
        return 1;
    }

    std::string respJson;
    std::string err;
    if (!SendIpcRequest(reqJson.str(), respJson, err, true)) {
        if (jsonOutput) {
            std::cout << BuildTransportError(err) << "\n";
        } else {
            std::cerr << "Error: " << err << "\n";
        }
        return 1;
    }

    return PrintResponse(subcmd, respJson, jsonOutput, requestedId, waitForCompletion);
}

} // namespace pc
