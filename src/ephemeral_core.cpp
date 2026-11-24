#include "ephemeral_core.h"
#include "common.h"
#include "ipc_client.h"
#include "nlohmann/json.hpp"

#include <vector>

#ifndef _WIN32
#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#endif

namespace pc {

namespace {

#ifdef _WIN32
std::wstring ResolveEphemeralCoreExePath() {
    wchar_t exePath[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return L"";
    std::wstring current = exePath;
    const size_t slash = current.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return current;
    const std::wstring dir = current.substr(0, slash + 1);
    std::wstring hanchoExe = dir + L"Hancho.exe";
    if (GetFileAttributesW(hanchoExe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        hanchoExe = current;
    }
    const std::wstring coreExe = dir + L"HanchoCore.exe";
    WIN32_FILE_ATTRIBUTE_DATA a = {}, b = {};
    const bool haveCore = GetFileAttributesExW(coreExe.c_str(), GetFileExInfoStandard, &a) != FALSE;
    const bool haveHancho = GetFileAttributesExW(hanchoExe.c_str(), GetFileExInfoStandard, &b) != FALSE;
    if (haveCore && haveHancho &&
        a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
        a.ftLastWriteTime.dwHighDateTime == b.ftLastWriteTime.dwHighDateTime &&
        a.ftLastWriteTime.dwLowDateTime == b.ftLastWriteTime.dwLowDateTime) {
        return coreExe;
    }
    if (haveHancho && _wcsicmp(hanchoExe.c_str(), coreExe.c_str()) != 0) {
        DeleteFileW(coreExe.c_str());
        if (CopyFileW(hanchoExe.c_str(), coreExe.c_str(), FALSE)) {
            return coreExe;
        }
    }
    if (haveCore) return coreExe;
    return hanchoExe;
}
#else
std::string ResolveEphemeralCoreExePathPosix() {
    const std::string current = GetExecutablePathUtf8();
    const size_t slash = current.find_last_of('/');
    const std::string dir = (slash == std::string::npos) ? std::string(".") : current.substr(0, slash);
    for (const char* name : {"Hancho", "hancho", "hanchod"}) {
        std::wstring candidate = Utf8ToWide(dir + "/" + name);
        if (PathExistsWide(candidate) && !IsDirectoryWide(candidate)) {
            return WideToUtf8(candidate);
        }
    }
    return current;
}
#endif

void SeedConfigFiles(const std::wstring& seedFrom, const std::wstring& dataDir) {
    static const wchar_t* kSeedFiles[] = {
        L"config.json",
        L"engines.json",
        L"llms.json",
        L"failure-rules.json",
        L"permits.json",
    };
    for (const wchar_t* name : kSeedFiles) {
        const std::wstring src = PathJoinWide(seedFrom, name);
        if (!PathExistsWide(src) || IsDirectoryWide(src)) continue;
        const std::string content = ReadFileUtf8Wide(src);
        WriteFileAtomicWide(PathJoinWide(dataDir, name), content);
    }
}

void DisableTriageInConfig(const std::wstring& dataDir) {
    const std::wstring cfgPath = PathJoinWide(dataDir, L"config.json");
    nlohmann::json cfg = nlohmann::json::object();
    if (PathExistsWide(cfgPath) && !IsDirectoryWide(cfgPath)) {
        const std::string existing = ReadFileUtf8Wide(cfgPath);
        if (!existing.empty()) {
            auto parsed = nlohmann::json::parse(existing, nullptr, false);
            if (parsed.is_object()) {
                cfg = std::move(parsed);
            }
        }
    }
    cfg["triage_mode"] = "off";
    cfg["triage_llm_mode"] = "manual";
    WriteFileAtomicWide(cfgPath, cfg.dump(2) + "\n");
}

} // namespace

EphemeralCoreGuard::~EphemeralCoreGuard() {
    Stop();
}

bool EphemeralCoreGuard::Start(const EphemeralCoreOptions& opts, std::string& outError) {
    Stop();
    outError.clear();

    if (!opts.seedFrom.empty()) {
        const std::wstring absSeed = ResolveAbsolutePathWide(opts.seedFrom);
        if (!IsDirectoryWide(absSeed)) {
            outError = "seed-from directory does not exist: " + WideToUtf8(absSeed);
            return false;
        }
    }

    if (opts.dataDir.empty()) {
        const std::wstring ciRoot = PathJoinWide(GetCurrentDirectoryWide(), L".hancho-ci");
        dataDir_ = PathJoinWide(ciRoot, std::to_wstring(GetCurrentProcessIdPortable()));
    } else {
        dataDir_ = ResolveAbsolutePathWide(opts.dataDir);
    }

    SetDataDirectoryOverride(dataDir_);
    EnsureDirectoryWide(dataDir_);

    if (IsHanchoRunningForDataDirWide()) {
        outError = "a Hancho core is already running for data directory: " + WideToUtf8(dataDir_);
        return false;
    }

    if (!opts.seedFrom.empty()) {
        SeedConfigFiles(ResolveAbsolutePathWide(opts.seedFrom), dataDir_);
    }
    if (!opts.triage) {
        DisableTriageInConfig(dataDir_);
    }

#ifdef _WIN32
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (!job_) {
        outError = "CreateJobObjectW failed (GetLastError=" + std::to_string(GetLastError()) + ")";
        return false;
    }
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
    jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli))) {
        outError = "SetInformationJobObject failed (GetLastError=" + std::to_string(GetLastError()) + ")";
        CloseHandle(job_);
        job_ = nullptr;
        return false;
    }

    const std::wstring coreExe = ResolveEphemeralCoreExePath();
    if (coreExe.empty()) {
        outError = "cannot locate HanchoCore.exe / Hancho.exe";
        CloseHandle(job_);
        job_ = nullptr;
        return false;
    }

    std::wstring cmdLine = L"\"" + coreExe + L"\" --headless --data-dir \"" + dataDir_ + L"\"";
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        outError = "failed to launch ephemeral core (GetLastError=" + std::to_string(GetLastError()) + ")";
        CloseHandle(job_);
        job_ = nullptr;
        return false;
    }

    if (!AssignProcessToJobObject(job_, pi.hProcess)) {
        outError = "AssignProcessToJobObject failed (GetLastError=" + std::to_string(GetLastError()) + ")";
        TerminateProcess(pi.hProcess, 1);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CloseHandle(job_);
        job_ = nullptr;
        return false;
    }

    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    process_ = pi.hProcess;
    pid_ = pi.dwProcessId;
    started_ = true;

    const ULONGLONG deadline = GetTickCount64() + 30000ULL;
    bool ready = false;
    while (GetTickCount64() < deadline) {
        std::string resp, err;
        if (SendIpcRequest("{\"action\":\"ping\"}", resp, err, false, 2000)) {
            auto j = nlohmann::json::parse(resp, nullptr, false);
            if (j.is_object() && j.value("ok", false)) {
                ready = true;
                break;
            }
        }
        if (WaitForSingleObject(process_, 100) == WAIT_OBJECT_0) {
            DWORD ec = 0;
            GetExitCodeProcess(process_, &ec);
            outError = "ephemeral core exited prematurely (code=" + std::to_string(ec) + ")";
            Stop();
            return false;
        }
    }
    if (!ready) {
        outError = "timed out waiting 30s for ephemeral core ping";
        Stop();
        return false;
    }
    return true;
#else
    const std::string coreExe = ResolveEphemeralCoreExePathPosix();
    if (coreExe.empty()) {
        outError = "cannot locate Hancho core executable";
        return false;
    }
    const std::string dataDirUtf8 = WideToUtf8(dataDir_);
    pid_t child = ::fork();
    if (child < 0) {
        outError = "fork failed";
        return false;
    }
    if (child == 0) {
        ::setpgid(0, 0);
#ifdef __linux__
        ::prctl(PR_SET_PDEATHSIG, SIGTERM);
#endif
        int devNull = ::open("/dev/null", O_RDWR);
        if (devNull >= 0) {
            ::dup2(devNull, STDIN_FILENO);
            ::dup2(devNull, STDOUT_FILENO);
            ::dup2(devNull, STDERR_FILENO);
            if (devNull > STDERR_FILENO) ::close(devNull);
        }
        std::vector<std::string> args = {coreExe, "--headless", "--data-dir", dataDirUtf8};
        std::vector<char*> argv;
        argv.reserve(args.size() + 1);
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        ::execv(coreExe.c_str(), argv.data());
        _exit(127);
    }
    ::setpgid(child, child);
    pid_ = static_cast<int>(child);
    started_ = true;

    bool ready = false;
    for (int i = 0; i < 300; ++i) {
        std::string resp, err;
        if (SendIpcRequest("{\"action\":\"ping\"}", resp, err, false, 2000)) {
            auto j = nlohmann::json::parse(resp, nullptr, false);
            if (j.is_object() && j.value("ok", false)) {
                ready = true;
                break;
            }
        }
        int status = 0;
        if (::waitpid(child, &status, WNOHANG) == child) {
            pid_ = -1;
            outError = "ephemeral core exited prematurely";
            Stop();
            return false;
        }
        SleepMs(100);
    }
    if (!ready) {
        outError = "timed out waiting 30s for ephemeral core ping";
        Stop();
        return false;
    }
    return true;
#endif
}

void EphemeralCoreGuard::Stop() {
    if (!started_) {
#ifdef _WIN32
        if (process_) {
            CloseHandle(process_);
            process_ = nullptr;
        }
        if (job_) {
            CloseHandle(job_);
            job_ = nullptr;
        }
        pid_ = 0;
#else
        pid_ = -1;
#endif
        return;
    }
    started_ = false;

    std::string resp, err;
    SendIpcRequest("{\"action\":\"quit\",\"cancel_jobs\":true}", resp, err, false, 5000);

#ifdef _WIN32
    if (process_) {
        WaitForSingleObject(process_, 10000);
        CloseHandle(process_);
        process_ = nullptr;
    }
    if (job_) {
        // Closing the job handle with KILL_ON_JOB_CLOSE terminates any remaining
        // core or job processes if the graceful quit did not finish in 10s.
        CloseHandle(job_);
        job_ = nullptr;
    }
    pid_ = 0;
#else
    if (pid_ > 0) {
        bool exited = false;
        for (int i = 0; i < 100; ++i) {
            int status = 0;
            pid_t r = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
            if (r == static_cast<pid_t>(pid_) || (r < 0 && errno == ECHILD)) {
                exited = true;
                break;
            }
            SleepMs(100);
        }
        if (!exited) {
            ::kill(-static_cast<pid_t>(pid_), SIGTERM);
            for (int i = 0; i < 20; ++i) {
                int status = 0;
                if (::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG) == static_cast<pid_t>(pid_)) {
                    exited = true;
                    break;
                }
                SleepMs(50);
            }
            if (!exited) {
                ::kill(-static_cast<pid_t>(pid_), SIGKILL);
                int status = 0;
                ::waitpid(static_cast<pid_t>(pid_), &status, 0);
            }
        }
        pid_ = -1;
    }
#endif
}

} // namespace pc
