// Headless core smoke test (src/main.cpp --headless + src/ipc_server.cpp).
//
// Starts a real `Hancho.exe --headless` process, talks to it over the Named Pipe
// the same way hancho-cli does, disconnects and reconnects between requests while
// a job is running, then asks the core to stop.
//
// Build: cmake --build build --config Release --target headless_smoke
// Run:   build\Release\headless_smoke.exe

#include "common.h"
#include "ipc_client.h"
#include "nlohmann/json.hpp"

#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace pc;
using json = nlohmann::json;

namespace {

int g_failures = 0;
int g_passes = 0;

void Check(bool ok, const std::string& msg) {
    if (ok) {
        ++g_passes;
    } else {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
    }
}

void RemoveDirRecursive(const std::wstring& dir) {
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring full = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirRecursive(full);
            } else {
                SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(full.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

bool IpcCall(const json& req, json& out, std::string* outError = nullptr) {
    std::string resp;
    std::string err;
    if (!SendIpcRequest(req.dump(), resp, err, false)) {
        if (outError) *outError = err;
        return false;
    }
    out = json::parse(resp, nullptr, false);
    if (out.is_discarded()) {
        if (outError) *outError = "invalid JSON response: " + resp;
        return false;
    }
    return true;
}

// The test binary is emitted next to Hancho.exe (see CMakeLists.txt), so look
// for the sibling executable first. This avoids baking a path with backslashes
// into a C string literal.
std::wstring ExePath() {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path = buf;
    size_t slash = path.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos) ? std::wstring() : path.substr(0, slash + 1);
    std::wstring sibling = dir + L"Hancho.exe";
    if (GetFileAttributesW(sibling.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return sibling;
    }
    return path;
}

HWND FindProcessWindow(const wchar_t* className, DWORD pid) {
    HWND w = nullptr;
    while ((w = FindWindowExW(nullptr, w, className, nullptr)) != nullptr) {
        DWORD owner = 0;
        GetWindowThreadProcessId(w, &owner);
        if (owner == pid) return w;
    }
    return nullptr;
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);

    wchar_t tempBase[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempBase);
    std::wstring tempDirW = std::wstring(tempBase) + L"pc_headless_smoke_" +
                            std::to_wstring(GetCurrentProcessId());
    RemoveDirRecursive(tempDirW);
    CreateDirectoryW(tempDirW.c_str(), nullptr);
    SetDataDirectoryOverride(tempDirW);

    std::wstring exe = ExePath();
    std::wstring cmdLine = L"\"" + exe + L"\" --headless --data-dir \"" + tempDirW + L"\"";
    std::vector<wchar_t> mutableCmd(cmdLine.begin(), cmdLine.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    BOOL started = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, FALSE,
                                  CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    Check(started != FALSE, "Hancho.exe --headless starts");
    if (!started) {
        RemoveDirRecursive(tempDirW);
        return 1;
    }
    CloseHandle(pi.hThread);

    // 1. The core comes up and answers ping without any dashboard window.
    json ping;
    bool connected = false;
    for (int i = 0; i < 150; ++i) {
        if (IpcCall({{"action", "ping"}}, ping) && ping.value("ok", false)) {
            connected = true;
            break;
        }
        Sleep(100);
    }
    Check(connected, "headless core answers IPC ping");
    Check(ping.value("headless", false), "headless core reports headless=true");
    bool childOwnsDashboard = false;
    for (HWND w = FindWindowW(L"HanchoDashboardWnd", nullptr); w;
         w = FindWindowExW(nullptr, w, L"HanchoDashboardWnd", nullptr)) {
        DWORD ownerPid = 0;
        GetWindowThreadProcessId(w, &ownerPid);
        if (ownerPid == pi.dwProcessId) {
            childOwnsDashboard = true;
            break;
        }
    }
    Check(!childOwnsDashboard, "headless mode creates no dashboard window");

    // 2. Start a job that outlives several client connections, like a dashboard
    //    being closed and reopened.
    json startResp;
    bool startOk = IpcCall({{"action", "start"},
                            {"cmd", "cmd /c ping -n 12 127.0.0.1 >nul 2>&1"},
                            {"name", "headless_smoke_job"},
                            {"allow_chain", true},
                            {"wait", false}},
                           startResp);
    int jobId = startOk ? startResp.value("id", 0) : 0;
    Check(startOk && jobId > 0, "headless core starts a job");

    // A fresh Named Pipe connection per call: this is the reconnect path used by
    // hancho-cli and by a dashboard attaching later.
    bool sawRunning = false;
    for (int i = 0; i < 20 && jobId > 0; ++i) {
        json list;
        if (IpcCall({{"action", "list"}}, list) && list.value("ok", false) &&
            list["processes"].is_array()) {
            for (const auto& p : list["processes"]) {
                if (p.value("id", 0) == jobId) {
                    std::string state = p.value("state", "");
                    if (state == "running" || state == "completed") sawRunning = true;
                }
            }
        }
        if (sawRunning) break;
        Sleep(100);
    }
    Check(sawRunning, "job is visible after reconnecting to the core");

    json guiStatus;
    Check(IpcCall({{"action", "gui_status"}}, guiStatus) && guiStatus.value("ok", false) &&
              guiStatus.value("headless", false),
          "gui_status reports a headless core");

    // 2b. Standalone GUI launch and reconnect (Card 060c)
    // Verify that launching Hancho.exe gui starts an independent GUI process
    // connected to the running core, closing it leaves jobs running, and
    // reopening reconnects.
    {
        std::wstring guiCmd = L"\"" + exe + L"\" gui --no-prompt --data-dir \"" + tempDirW + L"\"";
        std::vector<wchar_t> mutableGuiCmd(guiCmd.begin(), guiCmd.end());
        mutableGuiCmd.push_back(L'\0');

        STARTUPINFOW guiSi = {};
        guiSi.cb = sizeof(guiSi);
        PROCESS_INFORMATION guiPi = {};
        BOOL guiStarted = CreateProcessW(nullptr, mutableGuiCmd.data(), nullptr, nullptr, FALSE,
                                         0, nullptr, nullptr, &guiSi, &guiPi);
        Check(guiStarted != FALSE, "Hancho.exe gui standalone client starts");
        if (guiStarted) {
            CloseHandle(guiPi.hThread);
            Check(guiPi.dwProcessId != pi.dwProcessId, "GUI runs as a separate process from core");

            // Wait for GUI window to appear
            HWND dashHwnd = nullptr;
            for (int attempt = 0; attempt < 50; ++attempt) {
                dashHwnd = FindProcessWindow(L"HanchoDashboardWnd", guiPi.dwProcessId);
                if (dashHwnd) break;
                Sleep(100);
            }
            Check(dashHwnd != nullptr, "standalone GUI creates dashboard window");

            // Close the GUI window using registered HanchoQuit message
            UINT quitMsg = RegisterWindowMessageW(L"HanchoQuit");
            if (dashHwnd) PostMessageW(dashHwnd, quitMsg, 0, 0);

            DWORD guiExit = WaitForSingleObject(guiPi.hProcess, 10000);
            Check(guiExit == WAIT_OBJECT_0, "standalone GUI process exits cleanly when closed");
            CloseHandle(guiPi.hProcess);

            // Verify core keeps running and answering ping after GUI exits
            json pingCheck;
            Check(IpcCall({{"action", "ping"}}, pingCheck) && pingCheck.value("ok", false),
                  "core keeps running after GUI process exits");

            // Reconnect: Launch GUI a second time while job is running
            mutableGuiCmd = std::vector<wchar_t>(guiCmd.begin(), guiCmd.end());
            mutableGuiCmd.push_back(L'\0');
            PROCESS_INFORMATION guiPi2 = {};
            BOOL guiReconnected = CreateProcessW(nullptr, mutableGuiCmd.data(), nullptr, nullptr, FALSE,
                                                 0, nullptr, nullptr, &guiSi, &guiPi2);
            Check(guiReconnected != FALSE, "GUI reconnects on second launch");
            if (guiReconnected) {
                CloseHandle(guiPi2.hThread);
                HWND dashHwnd2 = nullptr;
                for (int attempt = 0; attempt < 50; ++attempt) {
                    dashHwnd2 = FindProcessWindow(L"HanchoDashboardWnd", guiPi2.dwProcessId);
                    if (dashHwnd2) break;
                    Sleep(100);
                }
                Check(dashHwnd2 != nullptr, "reconnected GUI creates dashboard window");

                if (dashHwnd2) PostMessageW(dashHwnd2, quitMsg, 0, 0);
                DWORD guiExit2 = WaitForSingleObject(guiPi2.hProcess, 10000);
                Check(guiExit2 == WAIT_OBJECT_0, "reconnected GUI process exits cleanly");
                CloseHandle(guiPi2.hProcess);
            }
        }
    }

    // 3. The job keeps running and finishes even though every client (dashboard)
    //    connection was closed in between.
    json waitResp;
    std::string waitErr;
    bool waited = IpcCall({{"action", "wait"}, {"id", jobId}, {"wait_ms", 20000}}, waitResp, &waitErr);
    std::printf("DEBUG: waited=%d err=%s waitResp=%s\n", (int)waited, waitErr.c_str(), waitResp.dump().c_str());
    Check(waited && waitResp.value("ok", false) && waitResp.value("finished", false),
          "job continues and finishes after client reconnects");
    if (waited && waitResp.contains("process")) {
        Check(waitResp["process"].value("state", "") == "completed",
              "job finished with state=completed");
    }

    // 3b. Headless core task queue operation without GUI (Card 060d)
    // Verify that the headless core can manage task queues and queue status via IPC
    // without any GUI or desktop session present.
    json tqAddResp;
    json tqJob = {
        {"id", "headless_smoke_job_1"},
        {"command", "cmd /c exit 0"},
        {"summary", "Headless task queue smoke job"},
        {"timeout_sec", 30}
    };
    bool tqAdd = IpcCall({{"action", "taskqueue_add"},
                          {"taskqueue", "headless_q"},
                          {"jobs", json::array({tqJob})}},
                         tqAddResp);
    std::printf("DEBUG: tqAdd=%d tqAddResp=%s\n", (int)tqAdd, tqAddResp.dump().c_str());
    Check(tqAdd && tqAddResp.value("ok", false), "headless core accepts taskqueue_add via IPC");

    json tqListResp;
    bool tqList = IpcCall({{"action", "taskqueue_list"}}, tqListResp);
    std::printf("DEBUG: tqList=%d tqListResp=%s\n", (int)tqList, tqListResp.dump().c_str());
    Check(tqList && tqListResp.value("ok", false) && tqListResp.contains("jobs") &&
              tqListResp["jobs"].is_array(),
          "headless core lists task queues via IPC");

    // 4. A client can stop the headless core.
    json quitResp;
    bool quitSent = IpcCall({{"action", "quit"}}, quitResp);
    Check(quitSent && quitResp.value("ok", false), "quit request is accepted");
    DWORD exitWait = WaitForSingleObject(pi.hProcess, 20000);
    Check(exitWait == WAIT_OBJECT_0, "headless core exits after quit");
    CloseHandle(pi.hProcess);

    // 5. After the core is gone, the pipe is no longer served.
    json pingAfter;
    Check(!IpcCall({{"action", "ping"}}, pingAfter) || !pingAfter.value("ok", false),
          "IPC is no longer served after the core exits");

    // 6. Launching GUI standalone with --no-prompt when core is not running exits with error
    {
        std::wstring guiCmd = L"\"" + exe + L"\" gui --no-prompt --data-dir \"" + tempDirW + L"\"";
        std::vector<wchar_t> mutableGuiCmd(guiCmd.begin(), guiCmd.end());
        mutableGuiCmd.push_back(L'\0');

        STARTUPINFOW guiSi = {};
        guiSi.cb = sizeof(guiSi);
        PROCESS_INFORMATION guiPi = {};
        if (CreateProcessW(nullptr, mutableGuiCmd.data(), nullptr, nullptr, FALSE,
                           0, nullptr, nullptr, &guiSi, &guiPi)) {
            CloseHandle(guiPi.hThread);
            WaitForSingleObject(guiPi.hProcess, 5000);
            DWORD ec = 0;
            GetExitCodeProcess(guiPi.hProcess, &ec);
            CloseHandle(guiPi.hProcess);
            Check(ec != 0, "GUI exits with error when core is not running and --no-prompt is set");
        }
    }

    RemoveDirRecursive(tempDirW);
    std::printf("%s (%d passed, %d failures)\n", g_failures ? "FAILED" : "ALL PASSED",
                g_passes, g_failures);
    return g_failures ? 1 : 0;
}
