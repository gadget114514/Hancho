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
                            {"cmd", "cmd /c ping -n 4 127.0.0.1"},
                            {"name", "headless_smoke_job"},
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

    // 3. The job keeps running and finishes even though every client (dashboard)
    //    connection was closed in between.
    json waitResp;
    bool waited = IpcCall({{"action", "wait"}, {"id", jobId}, {"wait_ms", 20000}}, waitResp);
    Check(waited && waitResp.value("ok", false) && waitResp.value("finished", false),
          "job continues and finishes after client reconnects");
    if (waited && waitResp.contains("process")) {
        Check(waitResp["process"].value("state", "") == "completed",
              "job finished with state=completed");
    }

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

    RemoveDirRecursive(tempDirW);
    std::printf("%s (%d passed, %d failures)\n", g_failures ? "FAILED" : "ALL PASSED",
                g_passes, g_failures);
    return g_failures ? 1 : 0;
}
