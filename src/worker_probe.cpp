#include "worker_probe.h"

#include <chrono>
#include <cctype>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace pc {

using nlohmann::json;

namespace {

constexpr const char* kProbePrompt = "Reply with the single word OK.";

int64_t NowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

std::string CacheKey(const std::string& engine, const std::string& model) {
    return engine + "/" + model;
}

struct RunResult {
    bool started = false;
    bool timedOut = false;
    int exitCode = -1;
    std::string output;
};

RunResult RunCommand(const std::string& command, int timeoutSec) {
    RunResult r;
    const int64_t deadline = NowEpochMs() + static_cast<int64_t>(timeoutSec) * 1000;
#ifdef _WIN32
    HANDLE hOutRead = nullptr, hOutWrite = nullptr;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    if (!CreatePipe(&hOutRead, &hOutWrite, &sa, 0)) return r;
    SetHandleInformation(hOutRead, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hOutWrite;
    si.hStdError = hOutWrite;
    si.hStdInput = nullptr; // closed stdin

    std::string cmd = command;
    std::vector<char> mutableCmd(cmd.begin(), cmd.end());
    mutableCmd.push_back('\0');
    PROCESS_INFORMATION pi{};
    BOOL created = CreateProcessA(nullptr, mutableCmd.data(), nullptr, nullptr, TRUE,
                                  CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(hOutWrite);
    if (!created) {
        CloseHandle(hOutRead);
        return r;
    }
    r.started = true;

    auto drain = [&]() {
        char buf[2048];
        for (;;) {
            DWORD avail = 0;
            if (!PeekNamedPipe(hOutRead, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
            DWORD got = 0;
            if (!ReadFile(hOutRead, buf, sizeof(buf), &got, nullptr) || got == 0) break;
            r.output.append(buf, got);
            if (r.output.size() > 65536) break;
        }
    };

    for (;;) {
        drain();
        DWORD wait = WaitForSingleObject(pi.hProcess, 25);
        if (wait == WAIT_OBJECT_0) break;
        if (NowEpochMs() >= deadline) {
            r.timedOut = true;
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 1000);
            break;
        }
    }
    drain();
    DWORD code = 1;
    if (GetExitCodeProcess(pi.hProcess, &code)) r.exitCode = static_cast<int>(code);
    else r.exitCode = 1;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(hOutRead);
    return r;
#else
    int fds[2];
    if (pipe(fds) != 0) return r;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]); close(fds[1]);
        return r;
    }
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, STDIN_FILENO);
        close(fds[1]);
        execl("/bin/sh", "sh", "-c", command.c_str(), (char*)nullptr);
        _exit(127);
    }
    close(fds[1]);
    r.started = true;
    int status = 0;
    for (;;) {
        pollfd pfd{fds[0], POLLIN, 0};
        if (poll(&pfd, 1, 25) > 0 && (pfd.revents & POLLIN)) {
            char buf[2048];
            ssize_t got = read(fds[0], buf, sizeof(buf));
            if (got > 0) r.output.append(buf, static_cast<size_t>(got));
            else if (got == 0) break;
        }
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) break;
        if (NowEpochMs() >= deadline) {
            r.timedOut = true;
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            break;
        }
    }
    close(fds[0]);
    r.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    return r;
#endif
}

bool LooksLikeAuthError(const std::string& text) {
    std::string lower = text;
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (const char* needle : {"auth", "unauthorized", "forbidden", "401", "403",
                              "invalid api key", "api key", "sign in", "login required"}) {
        if (lower.find(needle) != std::string::npos) return true;
    }
    return false;
}

ProbeOutcome Classify(const RunResult& run, int elapsedMs,
                      const std::string& engine, const std::string& model) {
    ProbeOutcome out;
    out.elapsedMs = elapsedMs;
    out.engine = engine;
    out.model = model;
    if (!run.started) {
        out.failureClass = "probe_failed";
        out.detail = "failed to launch engine command";
        return out;
    }
    if (run.timedOut) {
        out.failureClass = "startup_hang";
        out.detail = run.output.empty()
            ? "no first event within the timeout"
            : "engine did not exit within the timeout";
        return out;
    }
    if (run.exitCode != 0) {
        out.failureClass = LooksLikeAuthError(run.output) ? "auth_error" : "probe_failed";
        out.detail = "exit " + std::to_string(run.exitCode);
        return out;
    }
    if (Trim(run.output).empty()) {
        out.failureClass = "probe_failed";
        out.detail = "exit 0 but no output event";
        return out;
    }
    out.ok = true;
    out.detail = "ok";
    return out;
}

bool LoadCache(const std::wstring& path, json& out) {
    out = json::object();
    const std::string text = ReadFileUtf8Wide(path);
    if (text.empty()) return true;
    json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return false;
    out = root;
    return true;
}

void SaveCache(const std::wstring& path, json& root) {
    WriteFileAtomicWide(path, root.dump(2));
}

} // namespace

json ProbeOutcome::ToJson() const {
    json o{{"ok", ok}, {"engine", engine}, {"model", model}, {"elapsedMs", elapsedMs},
           {"cached", cached}, {"detail", detail}};
    if (!failureClass.empty()) o["failureClass"] = failureClass;
    return o;
}

std::wstring ProbeCacheDefaultPath() {
    return PathJoinWide(GetEffectiveDataDirectoryWide(), L"probes.json");
}

ProbeOutcome ProbeEngine(const WorkerEngineRegistry& registry,
                         const std::string& engine,
                         const std::string& model,
                         const ProbeOptions& options,
                         const std::wstring& cachePath) {
    const std::string key = CacheKey(engine, model);
    json root;
    if (!options.noCache && LoadCache(cachePath, root) && root.contains(key)) {
        const json& c = root[key];
        int64_t at = c.value("at", static_cast<int64_t>(0));
        bool cachedOk = c.value("ok", false);
        int ttl = cachedOk ? options.ttlSuccessSec : options.ttlFailureSec;
        if (ttl > 0 && (NowEpochMs() - at) < static_cast<int64_t>(ttl) * 1000) {
            ProbeOutcome out;
            out.ok = cachedOk;
            out.cached = true;
            out.engine = engine;
            out.model = model;
            out.elapsedMs = c.value("elapsedMs", 0);
            out.failureClass = c.value("failureClass", std::string());
            out.detail = c.value("detail", std::string());
            return out;
        }
    }

    std::string command, stream, err;
    if (!registry.BuildCommand(engine, model, kProbePrompt, options.timeoutSec,
                               command, stream, err)) {
        ProbeOutcome out;
        out.failureClass = "probe_failed";
        out.detail = err.empty() ? "engine registry rejected the candidate" : err;
        out.engine = engine;
        out.model = model;
        return out;
    }

    const int64_t started = NowEpochMs();
    RunResult run = RunCommand(command, options.timeoutSec);
    const int elapsed = static_cast<int>(NowEpochMs() - started);
    ProbeOutcome out = Classify(run, elapsed, engine, model);

    if (!LoadCache(cachePath, root)) root = json::object();
    root[key] = json{{"ok", out.ok},
                     {"at", NowEpochMs()},
                     {"elapsedMs", out.elapsedMs},
                     {"failureClass", out.failureClass},
                     {"detail", out.detail}};
    SaveCache(cachePath, root);
    return out;
}

} // namespace pc
