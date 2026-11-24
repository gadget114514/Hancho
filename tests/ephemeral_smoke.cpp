// Smoke test for disposable CI core (`run --ephemeral`, Card 105-2).
//
// Tests:
// 1. `quit` without `cancel_jobs` preserves existing behavior.
// 2. `run --ephemeral` success (0) and failure (1) exit codes, no core left behind,
//    triage_mode="off" & triage_llm_mode="manual" written to config.json.
// 3. `run --ephemeral --seed-from <dir>` copies config files and disables triage.
// 4. `run --ephemeral` fails when a core is already running in `--data-dir`.
// 5. Terminating `hancho-cli` with `TerminateProcess` kills both the ephemeral core
//    and its running job (`ping -n 60 127.0.0.1`) within 5 seconds via Job Object.

#include "common.h"
#include "ephemeral_core.h"
#include "ipc_client.h"
#include "nlohmann/json.hpp"

#include <windows.h>
#include <tlhelp32.h>
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

std::wstring SiblingExe(const wchar_t* name) {
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring path = buf;
    size_t slash = path.find_last_of(L"\\/");
    std::wstring dir = (slash == std::wstring::npos) ? L"" : path.substr(0, slash + 1);
    return dir + name;
}

bool IpcCall(const json& req, json& out) {
    std::string resp, err;
    if (!SendIpcRequest(req.dump(), resp, err, false)) return false;
    out = json::parse(resp, nullptr, false);
    return !out.is_discarded();
}

bool IsPidAlive(DWORD pid) {
    if (pid == 0) return false;
    HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!h) return false;
    DWORD r = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return r == WAIT_TIMEOUT;
}

std::vector<DWORD> FindChildPidsByExe(DWORD parentPid, const wchar_t* exeName) {
    std::vector<DWORD> pids;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return pids;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ParentProcessID == parentPid &&
                _wcsicmp(pe.szExeFile, exeName) == 0) {
                pids.push_back(pe.th32ProcessID);
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return pids;
}

int RunCliSync(const std::wstring& cliExe, const std::wstring& args, std::string* outOutput = nullptr) {
    SECURITY_ATTRIBUTES sa = {sizeof(sa), nullptr, TRUE};
    HANDLE hRead = nullptr, hWrite = nullptr;
    CreatePipe(&hRead, &hWrite, &sa, 0);
    SetHandleInformation(hRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hWrite;
    si.hStdError = hWrite;
    PROCESS_INFORMATION pi = {};

    std::wstring cmd = L"\"" + cliExe + L"\" " + args;
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');

    BOOL ok = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hWrite);
    if (!ok) {
        CloseHandle(hRead);
        return -1;
    }
    CloseHandle(pi.hThread);

    std::string output;
    char tmp[2048];
    DWORD nr = 0;
    while (ReadFile(hRead, tmp, sizeof(tmp), &nr, nullptr) && nr > 0) {
        output.append(tmp, nr);
    }
    CloseHandle(hRead);

    WaitForSingleObject(pi.hProcess, 60000);
    DWORD ec = 1;
    GetExitCodeProcess(pi.hProcess, &ec);
    CloseHandle(pi.hProcess);
    if (outOutput) *outOutput = std::move(output);
    return static_cast<int>(ec);
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);

    const std::wstring hanchoExe = SiblingExe(L"Hancho.exe");
    const std::wstring cliExe = SiblingExe(L"hancho-cli.exe");

    wchar_t tempBase[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempBase);
    const std::wstring rootDir = std::wstring(tempBase) + L"pc_ephemeral_smoke_" +
                                 std::to_wstring(GetCurrentProcessId());
    RemoveDirRecursive(rootDir);
    CreateDirectoryW(rootDir.c_str(), nullptr);

    // 1. `quit` without `cancel_jobs` preserves existing behavior
    {
        const std::wstring dir1 = rootDir + L"\\quit_default";
        CreateDirectoryW(dir1.c_str(), nullptr);
        SetDataDirectoryOverride(dir1);

        std::wstring cmd = L"\"" + hanchoExe + L"\" --headless --data-dir \"" + dir1 + L"\"";
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        BOOL started = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        Check(started != FALSE, "105-2: core starts for default quit test");
        if (started) {
            CloseHandle(pi.hThread);
            json ping;
            bool up = false;
            for (int i = 0; i < 150; ++i) {
                if (IpcCall({{"action", "ping"}}, ping) && ping.value("ok", false)) {
                    up = true;
                    break;
                }
                Sleep(100);
            }
            Check(up, "105-2: core answers ping");
            json quitResp;
            bool quitOk = IpcCall({{"action", "quit"}}, quitResp);
            Check(quitOk && quitResp.value("ok", false), "105-2: quit without cancel_jobs accepted");
            WaitForSingleObject(pi.hProcess, 15000);
            CloseHandle(pi.hProcess);
        }
    }

    // 2. `run --ephemeral` success (0), failure (1), triage config, and no leftover core
    {
        const std::wstring dataDir = rootDir + L"\\eph_ok";
        const std::wstring queueFile = rootDir + L"\\ok.json";
        WriteFileAtomicWide(queueFile,
            "{\"jobs\":[{\"id\":\"eph_ok_1\",\"command\":\"cmd /c exit 0\",\"timeout_sec\":30}]}");

        std::string out;
        int rc = RunCliSync(cliExe,
            L"run --ephemeral --data-dir \"" + dataDir + L"\" --file \"" + queueFile + L"\"", &out);
        Check(rc == 0, "105-2: run --ephemeral returns 0 on success");

        SetDataDirectoryOverride(dataDir);
        Check(!IsHanchoRunningForDataDirWide(), "105-2: no core remains after run --ephemeral finishes");

        const std::string cfgText = ReadFileUtf8Wide(dataDir + L"\\config.json");
        json cfg = json::parse(cfgText, nullptr, false);
        Check(cfg.is_object() &&
              cfg.value("triage_mode", "") == "off" &&
              cfg.value("triage_llm_mode", "") == "manual",
              "105-2: ephemeral config.json disables triage by default");

        // Failure case
        const std::wstring failDir = rootDir + L"\\eph_fail";
        const std::wstring failFile = rootDir + L"\\fail.json";
        WriteFileAtomicWide(failFile,
            "{\"jobs\":[{\"id\":\"eph_fail_1\",\"command\":\"cmd /c exit 1\",\"timeout_sec\":30}]}");
        int failRc = RunCliSync(cliExe,
            L"run --ephemeral --data-dir \"" + failDir + L"\" --file \"" + failFile + L"\"");
        Check(failRc == 1, "105-2: run --ephemeral returns 1 on job failure");
    }

    // 3. `--seed-from <dir>` copies config files and writes triage off
    {
        const std::wstring seedDir = rootDir + L"\\seed_src";
        const std::wstring dataDir = rootDir + L"\\eph_seeded";
        CreateDirectoryW(seedDir.c_str(), nullptr);
        WriteFileAtomicWide(seedDir + L"\\config.json", "{\"triage_mode\":\"auto\",\"custom_key\":42}\n");
        WriteFileAtomicWide(seedDir + L"\\engines.json", "{\"custom_engine\":{\"command\":\"echo hi\"}}\n");
        WriteFileAtomicWide(seedDir + L"\\llms.json", "{\"agent\":[]}\n");
        WriteFileAtomicWide(seedDir + L"\\failure-rules.json", "{\"rules\":[]}\n");
        WriteFileAtomicWide(seedDir + L"\\permits.json", "{\"permits\":[]}\n");

        const std::wstring queueFile = rootDir + L"\\seed_jobs.json";
        WriteFileAtomicWide(queueFile,
            "{\"jobs\":[{\"id\":\"seed_1\",\"command\":\"cmd /c exit 0\",\"timeout_sec\":30}]}");

        int rc = RunCliSync(cliExe,
            L"run --ephemeral --data-dir \"" + dataDir + L"\" --seed-from \"" + seedDir +
            L"\" --file \"" + queueFile + L"\"");
        Check(rc == 0, "105-2: run --ephemeral --seed-from succeeds");

        json cfg = json::parse(ReadFileUtf8Wide(dataDir + L"\\config.json"), nullptr, false);
        Check(cfg.is_object() && cfg.value("custom_key", 0) == 42 &&
              cfg.value("triage_mode", "") == "off" &&
              cfg.value("triage_llm_mode", "") == "manual",
              "105-2: --seed-from preserves config.json keys while disabling triage");
        Check(PathExistsWide(dataDir + L"\\engines.json"), "105-2: --seed-from copies engines.json");
        Check(PathExistsWide(dataDir + L"\\llms.json"), "105-2: --seed-from copies llms.json");
        Check(PathExistsWide(dataDir + L"\\failure-rules.json"), "105-2: --seed-from copies failure-rules.json");
        Check(PathExistsWide(dataDir + L"\\permits.json"), "105-2: --seed-from copies permits.json");
    }

    // 4. Error when a core is already running in --data-dir
    {
        const std::wstring busyDir = rootDir + L"\\busy_dir";
        CreateDirectoryW(busyDir.c_str(), nullptr);
        SetDataDirectoryOverride(busyDir);

        std::wstring cmd = L"\"" + hanchoExe + L"\" --headless --data-dir \"" + busyDir + L"\"";
        std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back(L'\0');
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        BOOL started = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        if (started) {
            CloseHandle(pi.hThread);
            json ping;
            for (int i = 0; i < 150; ++i) {
                if (IpcCall({{"action", "ping"}}, ping) && ping.value("ok", false)) break;
                Sleep(100);
            }
            const std::wstring queueFile = rootDir + L"\\busy.json";
            WriteFileAtomicWide(queueFile,
                "{\"jobs\":[{\"id\":\"busy_1\",\"command\":\"cmd /c exit 0\",\"timeout_sec\":30}]}");
            std::string errOut;
            int rc = RunCliSync(cliExe,
                L"run --ephemeral --data-dir \"" + busyDir + L"\" --file \"" + queueFile + L"\"",
                &errOut);
            Check(rc == 1, "105-2: run --ephemeral fails when core is already running in data-dir");

            json quitResp;
            IpcCall({{"action", "quit"}}, quitResp);
            WaitForSingleObject(pi.hProcess, 15000);
            CloseHandle(pi.hProcess);
        }
    }

    // 5. TerminateProcess on hancho-cli kills ephemeral core and its running job within 5 seconds
    {
        const std::wstring killDir = rootDir + L"\\kill_dir";
        const std::wstring queueFile = rootDir + L"\\kill_jobs.json";
        WriteFileAtomicWide(queueFile,
            "{\"jobs\":[{\"id\":\"kill_1\",\"command\":\"ping -n 60 127.0.0.1 >nul\",\"timeout_sec\":120}]}");

        std::wstring cmd = L"\"" + cliExe + L"\" run --ephemeral --data-dir \"" + killDir +
                           L"\" --file \"" + queueFile + L"\"";
        std::vector<wchar_t> buf(cmd.begin(), cmd.end());
        buf.push_back(L'\0');
        STARTUPINFOW si = {};
        si.cb = sizeof(si);
        PROCESS_INFORMATION pi = {};
        BOOL started = CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
        Check(started != FALSE, "105-2: hancho-cli run --ephemeral starts for TerminateProcess test");
        if (started) {
            CloseHandle(pi.hThread);
            SetDataDirectoryOverride(killDir);
            DWORD corePid = 0;
            bool jobRunning = false;
            for (int i = 0; i < 200; ++i) {
                json ping;
                if (IpcCall({{"action", "ping"}}, ping) && ping.value("ok", false)) {
                    corePid = static_cast<DWORD>(ping.value("pid", 0));
                    json listResp;
                    if (IpcCall({{"action", "taskqueue_list"}}, listResp) &&
                        listResp.value("ok", false) && listResp["jobs"].is_array() &&
                        !listResp["jobs"].empty() &&
                        listResp["jobs"][0].value("state", "") == "running") {
                        jobRunning = true;
                        break;
                    }
                }
                Sleep(100);
            }
            Check(corePid != 0 && jobRunning, "105-2: ephemeral core and long job are running before kill");

            std::vector<DWORD> pingPids = FindChildPidsByExe(corePid, L"ping.exe");
            if (pingPids.empty()) {
                // `ping` may be wrapped in `cmd.exe` depending on shell
                for (DWORD cmdPid : FindChildPidsByExe(corePid, L"cmd.exe")) {
                    auto nested = FindChildPidsByExe(cmdPid, L"ping.exe");
                    pingPids.insert(pingPids.end(), nested.begin(), nested.end());
                }
            }

            TerminateProcess(pi.hProcess, 137);
            WaitForSingleObject(pi.hProcess, 5000);
            CloseHandle(pi.hProcess);

            bool allGone = false;
            for (int i = 0; i < 50; ++i) {
                bool coreAlive = IsPidAlive(corePid);
                bool anyPingAlive = false;
                for (DWORD ppid : pingPids) {
                    if (IsPidAlive(ppid)) anyPingAlive = true;
                }
                if (!coreAlive && !anyPingAlive) {
                    allGone = true;
                    break;
                }
                Sleep(100);
            }
            Check(allGone, "105-2: TerminateProcess on hancho-cli kills core and running job within 5s");
        }
    }

    RemoveDirRecursive(rootDir);
    std::printf("%s (%d passed, %d failures)\n", g_failures ? "FAILED" : "ALL PASSED",
                g_passes, g_failures);
    return g_failures ? 1 : 0;
}
