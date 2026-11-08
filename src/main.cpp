#include "common.h"
#include "process_manager.h"
#include "ipc_server.h"
#include "taskqueue.h"
#include "bt_manager.h"
#include "mcp_server.h"
#include "gui_dashboard.h"
#include "cli.h"
#include <shellapi.h>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <vector>

using namespace pc;

static void PrintUsage() {
    std::cout <<
"Hancho " PC_VERSION R"( - Native Win32 Multi-LLM Process & ConPTY Manager + Live GUI Dashboard

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
)";
}

// Asks the running GUI to exit (same path as the tray menu's "Exit") and waits for it to go.
static int RunQuitCommand() {
    const wchar_t* kWindowClass = L"HanchoDashboardWnd";
    if (!FindWindowW(kWindowClass, nullptr)) {
        std::cout << "Hancho is not running" << std::endl;
        return 1;
    }
    UINT quitMessage = RegisterWindowMessageW(L"HanchoQuit");
    PostMessageW(FindWindowW(kWindowClass, nullptr), quitMessage, 0, 0);
    for (int i = 0; i < 100 && FindWindowW(kWindowClass, nullptr); ++i) Sleep(100);
    if (FindWindowW(kWindowClass, nullptr)) {
        std::cout << "Hancho did not exit within 10 seconds" << std::endl;
        return 1;
    }
    std::cout << "Hancho quit" << std::endl;
    return 0;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        args.push_back(WideToUtf8(argvW[i]));
    }
    LocalFree(argvW);

    std::vector<std::string> filtered;
    filtered.push_back(args.empty() ? "Hancho.exe" : args[0]);
    std::string dataDirArg;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--data-dir" && i + 1 < args.size()) {
            dataDirArg = args[++i];
        } else if (args[i].rfind("--data-dir=", 0) == 0) {
            dataDirArg = args[i].substr(11);
        } else {
            filtered.push_back(args[i]);
        }
    }
    args.swap(filtered);
    if (!dataDirArg.empty()) {
        SetDataDirectoryOverride(Utf8ToWide(dataDirArg));
    }

    std::string subcmd = (args.size() >= 2) ? args[1] : "gui";

    if (subcmd == "--help" || subcmd == "-h" || subcmd == "help") {
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--json" || args[i] == "-j") {
                return RunCli(args);
            }
        }
        PrintUsage();
        return 0;
    }

    // Like hancho-cli, --json may come before or after the command.
    bool jsonOutput = false;
    std::string firstCmd;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--json" || args[i] == "-j") jsonOutput = true;
        else if (firstCmd.empty()) firstCmd = args[i];
    }
    if (firstCmd == "--version" || firstCmd == "-V" || firstCmd == "version") {
        if (jsonOutput) std::cout << "{\"ok\":true,\"program\":\"Hancho\",\"version\":\"" PC_VERSION "\"}" << std::endl;
        else std::cout << "Hancho " PC_VERSION << std::endl;
        return 0;
    }

    if (subcmd == "mcp") {
        return RunMcpServer();
    }

    if (subcmd == "quit" || subcmd == "--quit") {
        return RunQuitCommand();
    }

    if (subcmd == "gui" || subcmd == "--gui") {
        HANDLE hMutex = CreateMutexW(nullptr, TRUE, GetGuiMutexNameWide().c_str());
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            HWND existing = FindWindowW(L"HanchoDashboardWnd", nullptr);
            if (existing) {
                ShowWindow(existing, SW_SHOW);
                if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
                SetForegroundWindow(existing);
            }
            if (hMutex) CloseHandle(hMutex);
            return 0;
        }

        // Hide the console window of the GUI daemon instead of calling FreeConsole().
        // Keeping an allocated (hidden) console in the parent process is required by Windows
        // so that CreateProcessW + PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE properly wires
        // StandardInput/StandardOutput/StandardError in child ConPTY processes!
        HWND hConsole = GetConsoleWindow();
        if (hConsole) {
            ShowWindow(hConsole, SW_HIDE);
        } else {
            AllocConsole();
            hConsole = GetConsoleWindow();
            if (hConsole) ShowWindow(hConsole, SW_HIDE);
        }

        // Record unhandled C++ exceptions (e.g. from a worker thread) before the process aborts.
        std::set_terminate([] {
            WriteAuditLog("CRASH", "std::terminate called: unhandled exception");
            std::abort();
        });

        ProcessManager pm;
        TaskQueueManager tqm;
        tqm.SetProcessManager(&pm);
        pm.SetProcessFinishedCallback([&tqm](int pid, int ec, const std::string& kind) {
            tqm.OnProcessFinished(pid, ec, kind);
        });
        BtManager btm(pm, tqm);
        IpcServer ipc(pm, tqm, &btm);
        ipc.Start();
        int rc = RunGuiDashboard(pm, ipc, tqm);
        ipc.Stop();
        pm.SetProcessFinishedCallback(nullptr);
        btm.Shutdown();
        tqm.ShutdownTaskQueueManager();
        if (hMutex) CloseHandle(hMutex);
        return rc;
    }

    // Backward-compatible console commands (prefer hancho-cli.exe)
    return RunCli(args);
}
