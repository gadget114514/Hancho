#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "common.h"
#include "process_manager.h"
#include "ipc_server.h"
#include "taskqueue.h"
#include "bt_manager.h"
#include "mcp_server.h"
#include "gui_dashboard.h"
#include "gui_ipmsg.h"
#include "term_client.h"
#include "cli.h"
#include "ipc_client.h"
#include "power.h"
#include "platform/platform.h"
#include <shellapi.h>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <thread>
#include <vector>

using namespace pc;

static void PrintUsage() {
    std::cout <<
"Hancho " PC_VERSION R"( - Native Win32 Multi-LLM Process & ConPTY Manager + Live GUI Dashboard

USAGE:
  Hancho.exe [gui] [--data-dir <path>] [--no-prompt]
      Launch the Win32 Dashboard GUI as an independent IPC client.
      Connects to the running core over Named Pipe. If the core is not running,
      starts it in headless mode automatically (no confirmation prompt).
      --no-prompt fails instead of starting the core (for scripts).
      Closing the GUI leaves jobs running; reopening the GUI reconnects and
      restores all state.

  Hancho.exe --headless [--data-dir <path>]
      Run the core (process manager, task queue, behavior trees) and the Named
      Pipe server without a window or tray icon. Jobs keep running after the
      dashboard is closed; `Hancho.exe gui` attaches again later.

  Hancho.exe quit
      Quit the running core, including a headless one or one hidden in the
      system tray (its tray icon is removed).

  Hancho.exe mcp [--data-dir <path>]
      Run as a Model Context Protocol (MCP) stdio server for Claude.
      Automatically connects to (or launches) the Win32 GUI Dashboard.

  Hancho.exe chat [--data-dir <path>] [--tray]   (alias: ipmsg)
      Run the Chat dialog only (IPMsg compatible protocol; no main dashboard).
      Connects to the running core, or starts the headless core first.
      One dialog per data dir; a second launch brings the existing one forward.
      --tray starts hidden in the notification area. Close (X) hides to tray;
      exit from the tray menu.

  Hancho.exe terminal [options]
      Run the standalone WinConsoleServer (WCSP) terminal client in its own
      tabbed window. Each tab is an independent session, so several consoles
      can be connected at once. Without options it opens an empty window.
      --host <host> --port <port> --token <token>   connect to a remote core
      --shell <shell>    powershell / cmd / bash (default powershell)
      --target <target>  "attach:<job id>" to view an existing job
      --local            force a local core connection
      --no-auto-attach   do not attach to new local ConPTY jobs automatically
      --no-connect       do not open a tab (and disable auto-attach)

  Hancho.exe --help
      Show this help.

  Hancho.exe --version
      Show the version.

OPTIONS (per launch):
  --webui-host <host>       WebUI bind address: 127.0.0.1 (default) or 0.0.0.0 for LAN.
  --webui-scope <scope>     loopback (default), lan, or custom. Also config.json "webui.scope".
  --webui-allowed-hosts <csv>    Extra hostnames allowed in Host/Origin (LAN scope).
  --webui-allowed-origins <csv>  Extra exact origins allowed (LAN scope).
  --webui-port <port>       WebUI port (default 9138).
  --webui-disable           Do not start the browser UI.
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

  IMPORTANT: start_process / `hancho-cli start` only manages a process. It is
  NOT a task: no job id, task history, success checks, retries or per-model
  usage. Register work with taskqueue_add (ordered/dependent jobs), run
  (add + wait + report) or bt_start (behavior tree) to make it a task. Only
  those jobs appear in task lists and count toward per-engine/model usage.

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

  MCP tools (57):
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
    Power (3):
      power_suspend, power_cancel, power_status
    Chat (11):
      chat_status, chat_peers, chat_refresh, chat_send, chat_get, chat_inbox,
      chat_wait, chat_read, chat_download, chat_log, chat_handler
    Memory (7):
      memory_search, memory_get, memory_brief, memory_stats, memory_note,
      memory_retract, memory_approve
    Usage (2):
      usage_stats, usage_export

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

// ---------------------------------------------------------------------------
// Core host
//
// The core (ProcessManager / TaskQueueManager / BtManager / IpcServer) owns the
// jobs. A --headless host runs it with a hidden message window and no dashboard;
// the dashboard can be attached later by any process. In the normal `gui` mode
// the dashboard runs on the main thread and the core is still independent of
// the window: closing the window hides the dashboard (tray) without stopping
// jobs, and `Hancho.exe gui` restores it.
// ---------------------------------------------------------------------------

static const wchar_t* kCoreHostClass = L"HanchoCoreHostWnd";
static const wchar_t* kDashboardClass = L"HanchoDashboardWnd";
static const UINT WM_PC_HOST_SHOW_GUI = WM_APP + 201;
static const UINT WM_PC_HOST_GUI_CLOSED = WM_APP + 202;
static const UINT WM_PC_HOST_QUIT = WM_APP + 203;

static HWND g_coreHostHwnd = nullptr;

// Window classes are process-global, so two Hancho data directories can each own
// a dashboard. Only ever treat a window from this process as ours.
static HWND FindOwnWindow(const wchar_t* cls) {
    HWND h = FindWindowW(cls, nullptr);
    while (h) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) return h;
        h = FindWindowExW(nullptr, h, cls, nullptr);
    }
    return nullptr;
}

struct CoreHostContext {
    ProcessManager* pm = nullptr;
    TaskQueueManager* tqm = nullptr;
    BtManager* btm = nullptr;
    IpcServer* ipc = nullptr;
    HWND hwnd = nullptr;
    std::thread guiThread;
    bool guiRunning = false;
    bool guiShowPending = false;
    bool quit = false;
};

static void CoreHostShowGui(CoreHostContext* ctx) {
    if (!ctx) return;
    HWND dash = FindWindowW(kDashboardClass, nullptr);
    if (dash) {
        if (IsIconic(dash)) ShowWindow(dash, SW_RESTORE);
        else ShowWindow(dash, SW_SHOW);
        SetForegroundWindow(dash);
        return;
    }
    // Launch standalone GUI as a separate process
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring cmd = L"\"";
    cmd += exePath;
    cmd += L"\" gui";
    if (HasCustomDataDirectory()) {
        cmd += L" --data-dir \"";
        cmd += GetEffectiveDataDirectoryWide();
        cmd += L"\"";
    }
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    if (CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                       0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

static LRESULT CALLBACK CoreHostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    CoreHostContext* ctx =
        reinterpret_cast<CoreHostContext*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    static const UINT quitMessage = RegisterWindowMessageW(L"HanchoQuit");
    if (msg == quitMessage) {
        PostMessageW(hwnd, WM_PC_HOST_QUIT, 0, 0);
        return 0;
    }

    switch (msg) {
        case WM_NCCREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ctx = reinterpret_cast<CoreHostContext*>(cs ? cs->lpCreateParams : nullptr);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ctx));
            if (ctx) ctx->hwnd = hwnd;
            return DefWindowProcW(hwnd, msg, wParam, lParam);
        }
        case WM_PC_HOST_SHOW_GUI:
            CoreHostShowGui(ctx);
            return 0;
        case WM_PC_HOST_GUI_CLOSED:
            if (ctx) {
                if (ctx->guiThread.joinable()) ctx->guiThread.join();
                ctx->guiRunning = false;
                if (ctx->guiShowPending) {
                    ctx->guiShowPending = false;
                    CoreHostShowGui(ctx);
                }
            }
            return 0;
        case WM_PC_HOST_QUIT:
            if (ctx) ctx->quit = true;
            PostQuitMessage(0);
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static BOOL WINAPI CoreHostCtrlHandler(DWORD type) {
    switch (type) {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
        case CTRL_CLOSE_EVENT:
        case CTRL_LOGOFF_EVENT:
        case CTRL_SHUTDOWN_EVENT:
            if (g_coreHostHwnd) {
                PostMessageW(g_coreHostHwnd, WM_PC_HOST_QUIT, 0, 0);
                Sleep(500);
            }
            return TRUE;
    }
    return FALSE;
}

static int RunCoreHost(platform::IInstanceLock* instanceLock, bool showGuiAtStart) {
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
    tqm.SetSuspendCallback([](const std::string& jobId, const std::string& state,
                              bool hibernate) {
        (void)jobId;
        (void)state;
        SleepMs(500);
        std::string err;
        if (!pc::SuspendSystem(hibernate, err)) {
            WriteAuditLog("POWER_FAILED", err);
        }
    });

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = CoreHostWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kCoreHostClass;
    RegisterClassExW(&wc);

    CoreHostContext ctx;
    ctx.pm = &pm;
    ctx.tqm = &tqm;
    ctx.btm = &btm;

    IpcServer ipc(pm, tqm, &btm);
    ctx.ipc = &ipc;
    ipc.SetHeadless(true);

    HWND hwnd = CreateWindowExW(0, kCoreHostClass, L"Hancho Core", WS_POPUP,
                                0, 0, 0, 0, nullptr, nullptr, wc.hInstance, &ctx);
    if (!hwnd) {
        if (instanceLock) instanceLock->Release();
        return 1;
    }
    g_coreHostHwnd = hwnd;
    SetConsoleCtrlHandler(CoreHostCtrlHandler, TRUE);

    ipc.SetGuiShowCallback([hwnd] {
        PostMessageW(hwnd, WM_PC_HOST_SHOW_GUI, 0, 0);
    });
    ipc.SetShutdownCallback([hwnd] {
        PostMessageW(hwnd, WM_PC_HOST_QUIT, 0, 0);
    });
    ipc.SetPowerCallback([](bool hibernate, std::string& outError) {
        return pc::SuspendSystem(hibernate, outError);
    });

    ipc.Start();
    if (showGuiAtStart) CoreHostShowGui(&ctx);

    MSG msg;
    while (!ctx.quit && GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    // Ask an attached dashboard to close and wait for its thread.
    if (ctx.guiRunning) {
        if (HWND dash = FindOwnWindow(kDashboardClass)) {
            PostMessageW(dash, RegisterWindowMessageW(L"HanchoQuit"), 0, 0);
        }
    }
    if (ctx.guiThread.joinable()) ctx.guiThread.join();

    ipc.SetGuiShowCallback(nullptr);
    ipc.SetShutdownCallback(nullptr);
    ipc.SetPowerCallback(nullptr);
    ipc.Stop();
    pm.SetProcessFinishedCallback(nullptr);
    tqm.SetSuspendCallback(nullptr);
    btm.Shutdown();
    tqm.ShutdownTaskQueueManager();

    g_coreHostHwnd = nullptr;
    if (instanceLock) instanceLock->Release();
    return 0;
}

// Asks the running core (headless host or dashboard) to exit and waits for it to go.
static int RunQuitCommand() {
    if (HasCustomDataDirectory()) {
        std::string resp, err;
        if (!SendIpcRequest("{\"action\":\"quit\"}", resp, err, false)) {
            std::cout << "Hancho is not running" << std::endl;
            return 1;
        }
        for (int i = 0; i < 100 && IsHanchoRunningForDataDirWide(); ++i) {
            Sleep(100);
        }
        if (IsHanchoRunningForDataDirWide()) {
            std::cout << "Hancho did not exit within 10 seconds" << std::endl;
            return 1;
        }
        std::cout << "Hancho quit" << std::endl;
        return 0;
    }
    HWND coreTarget = FindWindowW(kCoreHostClass, nullptr);
    HWND dashTarget = FindWindowW(kDashboardClass, nullptr);
    if (!coreTarget && !dashTarget) {
        std::cout << "Hancho is not running" << std::endl;
        return 1;
    }
    UINT quitMessage = RegisterWindowMessageW(L"HanchoQuit");
    if (coreTarget) PostMessageW(coreTarget, quitMessage, 0, 0);
    if (dashTarget) PostMessageW(dashTarget, quitMessage, 0, 0);
    for (int i = 0; i < 100 && (FindWindowW(kCoreHostClass, nullptr) ||
                                FindWindowW(kDashboardClass, nullptr)); ++i) {
        Sleep(100);
    }
    if (FindWindowW(kCoreHostClass, nullptr) || FindWindowW(kDashboardClass, nullptr)) {
        std::cout << "Hancho did not exit within 10 seconds" << std::endl;
        return 1;
    }
    std::cout << "Hancho quit" << std::endl;
    return 0;
}

namespace pc {
int RunStandaloneGuiDashboard();
}

static const wchar_t* TrMain(const wchar_t* ja, const wchar_t* en) {
    LANGID lang = GetUserDefaultUILanguage();
    if (PRIMARYLANGID(lang) == LANG_JAPANESE) return ja;
    return en;
}

static void HideOwnConsoleWindow() {
    HWND hConsole = GetConsoleWindow();
    if (!hConsole) return;
    DWORD procList[2] = {};
    DWORD count = GetConsoleProcessList(procList, 2);
    // When count == 1, this Hancho process is the only process attached to the console
    // (e.g. launched from Explorer, desktop shortcut), so we hide it immediately.
    // If count > 1, it was launched from an existing cmd/PowerShell prompt, so do not hide it.
    if (count == 1) {
        ShowWindow(hConsole, SW_HIDE);
    }
}

// Starts the background core without asking. The dashboard cannot work without a core, so the
// old yes/no prompt only added a click to every launch. --no-prompt keeps the script-friendly
// "fail instead of launching" behavior.
static bool WaitForCorePing(HANDLE coreProcess, int timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    while (GetTickCount64() < deadline) {
        Sleep(100);
        std::string resp, err;
        if (SendIpcRequest("{\"action\":\"ping\"}", resp, err, false)) return true;
        // The core we launched exited (crash, or it lost the instance lock to another
        // core) and nobody holds the lock now: waiting longer cannot succeed.
        if (coreProcess && WaitForSingleObject(coreProcess, 0) == WAIT_OBJECT_0 &&
            !IsHanchoRunningForDataDirWide()) {
            return false;
        }
    }
    return false;
}

static bool StartCoreHeadless() {
    // Restoring many jobs can make the core slow to open its pipe; the old 5 seconds
    // made callers give up and start yet another core.
    const int kCoreStartWaitMs = 30000;

    // A core is already starting (it holds the instance lock): wait for it instead of
    // launching another one.
    if (IsHanchoRunningForDataDirWide()) {
        if (WaitForCorePing(nullptr, kCoreStartWaitMs)) return true;
        const wchar_t* waitErrText = TrMain(
            L"Hancho コアは起動していますが、応答がありません。",
            L"Hancho core is running but does not respond."
        );
        MessageBoxW(nullptr, waitErrText, TrMain(L"エラー", L"Error"), MB_ICONERROR);
        return false;
    }

    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::wstring cmd = L"\"";
    cmd += exePath;
    cmd += L"\" --headless";
    if (HasCustomDataDirectory()) {
        cmd += L" --data-dir \"";
        cmd += GetEffectiveDataDirectoryWide();
        cmd += L"\"";
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        const wchar_t* errText = TrMain(
            L"Hancho コアの起動に失敗しました。",
            L"Failed to launch Hancho headless core."
        );
        const wchar_t* errTitle = TrMain(L"エラー", L"Error");
        MessageBoxW(nullptr, errText, errTitle, MB_ICONERROR);
        return false;
    }
    CloseHandle(pi.hThread);
    const bool ready = WaitForCorePing(pi.hProcess, kCoreStartWaitMs);
    CloseHandle(pi.hProcess);
    if (ready) return true;

    const wchar_t* waitErrText = TrMain(
        L"Hancho コアの起動待機中にタイムアウトしました。",
        L"Timed out waiting for Hancho headless core to start."
    );
    const wchar_t* errTitle = TrMain(L"エラー", L"Error");
    MessageBoxW(nullptr, waitErrText, errTitle, MB_ICONERROR);
    return false;
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

    const bool isGuiOrHeadless = (subcmd == "gui" || subcmd == "--gui" ||
                                  subcmd == "chat" || subcmd == "ipmsg" ||
                                  subcmd == "headless" || subcmd == "--headless");
    if (isGuiOrHeadless) {
        HideOwnConsoleWindow();
    }

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
    bool noPrompt = false;
    bool trayFlag = false;
    std::string firstCmd;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--json" || args[i] == "-j") jsonOutput = true;
        else if (args[i] == "--no-prompt" || args[i] == "--no-start-core") noPrompt = true;
        else if (args[i] == "--tray") trayFlag = true;
        // Starting is now the default; keep the old flag accepted so existing shortcuts still work.
        else if (args[i] == "--auto-start-core" || args[i] == "--start-core") {}
        else if (args[i] == "--webui-port" && i + 1 < args.size()) {
            _putenv_s("HANCHO_WEBUI_PORT", args[++i].c_str());
        } else if (args[i].rfind("--webui-port=", 0) == 0) {
            _putenv_s("HANCHO_WEBUI_PORT", args[i].substr(13).c_str());
        } else if (args[i] == "--webui-dir" && i + 1 < args.size()) {
            _putenv_s("HANCHO_WEBUI_DIR", args[++i].c_str());
        } else if (args[i].rfind("--webui-dir=", 0) == 0) {
            _putenv_s("HANCHO_WEBUI_DIR", args[i].substr(12).c_str());
        } else if (args[i] == "--webui-host" && i + 1 < args.size()) {
            _putenv_s("HANCHO_WEBUI_HOST", args[++i].c_str());
        } else if (args[i].rfind("--webui-host=", 0) == 0) {
            _putenv_s("HANCHO_WEBUI_HOST", args[i].substr(13).c_str());
        } else if (args[i] == "--webui-scope" && i + 1 < args.size()) {
            _putenv_s("HANCHO_WEBUI_SCOPE", args[++i].c_str());
        } else if (args[i].rfind("--webui-scope=", 0) == 0) {
            _putenv_s("HANCHO_WEBUI_SCOPE", args[i].substr(14).c_str());
        } else if (args[i] == "--webui-allowed-hosts" && i + 1 < args.size()) {
            _putenv_s("HANCHO_WEBUI_ALLOWED_HOSTS", args[++i].c_str());
        } else if (args[i].rfind("--webui-allowed-hosts=", 0) == 0) {
            _putenv_s("HANCHO_WEBUI_ALLOWED_HOSTS", args[i].substr(23).c_str());
        } else if (args[i] == "--webui-allowed-origins" && i + 1 < args.size()) {
            _putenv_s("HANCHO_WEBUI_ALLOWED_ORIGINS", args[++i].c_str());
        } else if (args[i].rfind("--webui-allowed-origins=", 0) == 0) {
            _putenv_s("HANCHO_WEBUI_ALLOWED_ORIGINS", args[i].substr(25).c_str());
        } else if (args[i] == "--webui-disable") {
            _putenv_s("HANCHO_WEBUI_DISABLE", "1");
        } else if (args[i] == "--webui-enable") {
            _putenv_s("HANCHO_WEBUI_DISABLE", "0");
        } else if (firstCmd.empty()) firstCmd = args[i];
    }
    if (firstCmd == "--version" || firstCmd == "-V" || firstCmd == "version") {
        if (jsonOutput) std::cout << "{\"ok\":true,\"program\":\"Hancho\",\"version\":\"" PC_VERSION "\"}" << std::endl;
        else std::cout << "Hancho " PC_VERSION << std::endl;
        return 0;
    }

    if (subcmd == "mcp") {
        return RunMcpServer();
    }

    if (subcmd == "chat" || subcmd == "ipmsg") {
        // IPC client only: no main dashboard. Start the headless core if it is
        // not running (same behavior as `Hancho.exe gui`).
        std::string pingResp, pingErr;
        if (!SendIpcRequest("{\"action\":\"ping\"}", pingResp, pingErr, false)) {
            if (noPrompt) {
                std::cerr << "Hancho core is not running. Please start it with: Hancho.exe --headless" << std::endl;
                return 1;
            }
            if (!StartCoreHeadless()) return 1;
        }
        // Hide the console window of the GUI client process if present.
        HideOwnConsoleWindow();
        return RunIpmsgDialog(trayFlag);
    }

    if (subcmd == "quit" || subcmd == "--quit") {
        return RunQuitCommand();
    }

    if (subcmd == "terminal" || subcmd == "--terminal" || subcmd == "term-client") {
        TermClientOptions opt;
        bool sawRemote = false;
        bool forceLocal = false;
        bool anyConnectOption = false;
        for (size_t i = 2; i < args.size(); ++i) {
            const std::string& a = args[i];
            auto next = [&](std::string& out) {
                if (i + 1 < args.size()) {
                    out = args[++i];
                    return true;
                }
                return false;
            };
            if (a == "--host") {
                std::string v;
                if (next(v)) { opt.host = v; sawRemote = true; anyConnectOption = true; }
            } else if (a.rfind("--host=", 0) == 0) {
                opt.host = a.substr(7);
                sawRemote = true;
                anyConnectOption = true;
            } else if (a == "--port") {
                std::string v;
                if (next(v)) { opt.port = std::atoi(v.c_str()); sawRemote = true; anyConnectOption = true; }
            } else if (a.rfind("--port=", 0) == 0) {
                opt.port = std::atoi(a.substr(7).c_str());
                sawRemote = true;
                anyConnectOption = true;
            } else if (a == "--token") {
                std::string v;
                if (next(v)) opt.token = v;
            } else if (a.rfind("--token=", 0) == 0) {
                opt.token = a.substr(8);
            } else if (a == "--shell") {
                std::string v;
                if (next(v)) { opt.target = v; anyConnectOption = true; }
            } else if (a.rfind("--shell=", 0) == 0) {
                opt.target = a.substr(8);
                anyConnectOption = true;
            } else if (a == "--target") {
                std::string v;
                if (next(v)) { opt.target = v; anyConnectOption = true; }
            } else if (a.rfind("--target=", 0) == 0) {
                opt.target = a.substr(9);
                anyConnectOption = true;
            } else if (a == "--attach") {
                std::string v;
                if (next(v)) { opt.target = "attach:" + v; anyConnectOption = true; }
            } else if (a.rfind("--attach=", 0) == 0) {
                opt.target = "attach:" + a.substr(9);
                anyConnectOption = true;
            } else if (a == "--local") {
                forceLocal = true;
                anyConnectOption = true;
            }
        }
        opt.local = forceLocal || !sawRemote;
        opt.autoConnect = anyConnectOption;
        for (size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--no-connect") {
                opt.autoConnect = false;
                opt.autoAttach = false;
            } else if (args[i] == "--no-auto-attach") {
                opt.autoAttach = false;
            }
        }
        if (opt.autoConnect && opt.target.empty()) opt.target = "powershell";
        if (opt.port > 0 && opt.host.empty()) opt.host = "127.0.0.1";
        return RunTerminalClient(opt);
    }

    const bool headlessRequested = (subcmd == "headless" || subcmd == "--headless");
    if (subcmd == "gui" || subcmd == "--gui" || headlessRequested) {
        if (headlessRequested) {
            auto instanceLock = platform::CreateInstanceLock(WideToUtf8(GetGuiMutexNameWide()));
            std::string lockError;
            if (!instanceLock || !instanceLock->Acquire(lockError)) {
                std::cout << "Hancho core is already running" << std::endl;
                return 0;
            }
            std::cout << "Hancho headless core started (pipe: "
                      << WideToUtf8(GetPipeNameWide()) << ")" << std::endl;
            return RunCoreHost(instanceLock.get(), false);
        }

        // GUI standalone client launch (doc/design-v3.md §2.3 段階 A, Card 060c):
        // 1. If a dashboard window is already open, bring it to front and exit.
        HWND existingDash = FindWindowW(kDashboardClass, nullptr);
        if (existingDash) {
            if (IsIconic(existingDash)) ShowWindow(existingDash, SW_RESTORE);
            else ShowWindow(existingDash, SW_SHOW);
            SetForegroundWindow(existingDash);
            return 0;
        }

        // 2. Check if the Hancho core is running.
        std::string pingResp, pingErr;
        bool coreRunning = SendIpcRequest("{\"action\":\"ping\"}", pingResp, pingErr, false);

        // 3. If not running, start the core in headless mode automatically.
        if (!coreRunning) {
            if (noPrompt) {
                std::cerr << "Hancho core is not running. Please start it with: Hancho.exe --headless" << std::endl;
                return 1;
            }
            if (!StartCoreHeadless()) {
                return 1;
            }
        }

        // 4. Hide the console window of the GUI client process if present.
        HideOwnConsoleWindow();

        // 5. Run the Win32 GUI Dashboard as an independent IPC client.
        // Jobs continue running in the core when the dashboard window is closed.
        return RunStandaloneGuiDashboard();
    }

    // Backward-compatible console commands (prefer hancho-cli.exe)
    return RunCli(args);
}
