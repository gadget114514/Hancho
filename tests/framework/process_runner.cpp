#include "process_runner.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace htest {
namespace {

#ifdef _WIN32
std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    const int n = MultiByteToWideChar(CP_UTF8, 0, &s[0], static_cast<int>(s.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, &s[0], static_cast<int>(s.size()), &out[0], n);
    return out;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, &s[0], static_cast<int>(s.size()),
                                      nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, &s[0], static_cast<int>(s.size()), &out[0], n,
                        nullptr, nullptr);
    return out;
}
#endif

void StripCr(std::string& s) {
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
}

} // namespace

std::string ExecutableDir() {
#ifdef _WIN32
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, &buf[0], static_cast<DWORD>(buf.size()));
    if (n == 0 || n >= buf.size()) return std::string();
    buf.resize(n);
    const size_t slash = buf.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return std::string();
    return WideToUtf8(buf.substr(0, slash + 1));
#else
    char buf[4096] = {};
    const ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return std::string();
    buf[n] = '\0';
    std::string path(buf);
    const size_t slash = path.find_last_of("/");
    if (slash == std::string::npos) return std::string();
    return path.substr(0, slash + 1);
#endif
}

std::string ExpandExeDir(const std::string& text) {
    const std::string dir = ExecutableDir();
    std::string out;
    const std::string token = "${EXEDIR}";
    size_t pos = 0;
    while (true) {
        const size_t found = text.find(token, pos);
        if (found == std::string::npos) {
            out.append(text, pos, std::string::npos);
            break;
        }
        out.append(text, pos, found - pos);
        out.append(dir);
        pos = found + token.size();
    }
    return out;
}

#ifdef _WIN32
ProcessResult RunProcess(const std::string& command, const std::string& workingDir,
                         int timeoutSec) {
    ProcessResult result;
    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE outRead = nullptr;
    HANDLE outWrite = nullptr;
    HANDLE errRead = nullptr;
    HANDLE errWrite = nullptr;
    if (!CreatePipe(&outRead, &outWrite, &sa, 0) ||
        !CreatePipe(&errRead, &errWrite, &sa, 0)) {
        result.error = "CreatePipe failed";
        return result;
    }
    SetHandleInformation(outRead, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(errRead, HANDLE_FLAG_INHERIT, 0);

    std::wstring cmdLine = L"cmd.exe /D /S /C \"" + Utf8ToWide(command) + L"\"";
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = outWrite;
    si.hStdError = errWrite;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi = {};
    const std::wstring wideDir = Utf8ToWide(workingDir);
    const BOOL created = CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW, nullptr,
                                        workingDir.empty() ? nullptr : wideDir.c_str(),
                                        &si, &pi);
    if (!created) {
        result.error = "CreateProcessW failed for: " + command;
        CloseHandle(outRead);
        CloseHandle(outWrite);
        CloseHandle(errRead);
        CloseHandle(errWrite);
        return result;
    }
    CloseHandle(outWrite);
    CloseHandle(errWrite);

    auto drain = [](HANDLE h, std::string& sink) {
        while (true) {
            DWORD available = 0;
            if (!PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr) || available == 0) {
                break;
            }
            char buf[4096];
            DWORD read = 0;
            const DWORD want = (std::min)(available, static_cast<DWORD>(sizeof(buf)));
            if (!ReadFile(h, buf, want, &read, nullptr) || read == 0) break;
            sink.append(buf, read);
        }
    };

    const DWORD timeoutMs = timeoutSec > 0 ? static_cast<DWORD>(timeoutSec) * 1000 : 0;
    const DWORD started = GetTickCount();
    while (true) {
        drain(outRead, result.stdoutText);
        drain(errRead, result.stderrText);
        const DWORD wait = WaitForSingleObject(pi.hProcess, 50);
        if (wait == WAIT_OBJECT_0) break;
        if (wait != WAIT_TIMEOUT) {
            result.error = "WaitForSingleObject failed";
            break;
        }
        if (timeoutMs != 0 && GetTickCount() - started >= timeoutMs) {
            TerminateProcess(pi.hProcess, 1);
            WaitForSingleObject(pi.hProcess, 5000);
            result.timedOut = true;
            break;
        }
    }
    drain(outRead, result.stdoutText);
    drain(errRead, result.stderrText);

    DWORD code = 0;
    if (GetExitCodeProcess(pi.hProcess, &code)) {
        result.exitCode = static_cast<int>(code);
    }
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(outRead);
    CloseHandle(errRead);
    StripCr(result.stdoutText);
    StripCr(result.stderrText);
    return result;
}
#else
ProcessResult RunProcess(const std::string& command, const std::string& workingDir,
                         int timeoutSec) {
    ProcessResult result;
    int outPipe[2] = {-1, -1};
    int errPipe[2] = {-1, -1};
    if (pipe(outPipe) != 0 || pipe(errPipe) != 0) {
        result.error = "pipe failed";
        return result;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        result.error = "fork failed";
        return result;
    }
    if (pid == 0) {
        dup2(outPipe[1], STDOUT_FILENO);
        dup2(errPipe[1], STDERR_FILENO);
        close(outPipe[0]);
        close(errPipe[0]);
        close(outPipe[1]);
        close(errPipe[1]);
        if (!workingDir.empty()) chdir(workingDir.c_str());
        execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    close(outPipe[1]);
    close(errPipe[1]);
    fcntl(outPipe[0], F_SETFL, O_NONBLOCK);
    fcntl(errPipe[0], F_SETFL, O_NONBLOCK);

    bool finished = false;
    int status = 0;
    const auto started = std::chrono::steady_clock::now();
    while (!finished) {
        pollfd fds[2] = {{outPipe[0], POLLIN, 0}, {errPipe[0], POLLIN, 0}};
        poll(fds, 2, 50);
        auto readAll = [](int fd, std::string& sink) {
            char buf[4096];
            while (true) {
                const ssize_t n = read(fd, buf, sizeof(buf));
                if (n <= 0) break;
                sink.append(buf, static_cast<size_t>(n));
            }
        };
        readAll(outPipe[0], result.stdoutText);
        readAll(errPipe[0], result.stderrText);

        const pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) {
            finished = true;
        } else if (timeoutSec > 0) {
            const auto elapsed = std::chrono::steady_clock::now() - started;
            if (elapsed >= std::chrono::seconds(timeoutSec)) {
                kill(pid, SIGKILL);
                waitpid(pid, &status, 0);
                result.timedOut = true;
                finished = true;
            }
        }
    }
    close(outPipe[0]);
    close(errPipe[0]);
    if (!result.timedOut) {
        result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    }
    StripCr(result.stdoutText);
    StripCr(result.stderrText);
    return result;
}
#endif

} // namespace htest
