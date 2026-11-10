#include "platform.h"

#ifndef _WIN32

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <thread>

#include <fcntl.h>
#include <fnmatch.h>
#include <signal.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#if defined(__linux__)
#include <pty.h>
#elif defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

namespace pc {
namespace platform {

namespace {

std::string HashName(const std::string& name) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char c : name) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    char buf[32] = {};
    std::snprintf(buf, sizeof(buf), "%016llx", static_cast<unsigned long long>(hash));
    return buf;
}

std::string LockPathForName(const std::string& name) {
    const char* runtime = std::getenv("XDG_RUNTIME_DIR");
    std::string dir = (runtime && *runtime) ? runtime : "/tmp";
    return dir + "/hancho-" + std::to_string(static_cast<long long>(getuid())) + "-" +
           HashName(name) + ".lock";
}

bool IsShellSafe(char c) {
    if (std::isalnum(static_cast<unsigned char>(c))) return true;
    switch (c) {
        case '_': case '@': case '%': case '+': case '=': case ':':
        case ',': case '.': case '/': case '-':
            return true;
        default:
            return false;
    }
}

uint64_t ReadUintFile(const std::string& path) {
    std::ifstream in(path);
    uint64_t value = 0;
    in >> value;
    return value;
}

std::string ReadProcField(const std::string& path, const char* key) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
        if (line.compare(0, std::strlen(key), key) == 0) return line;
    }
    return "";
}

} // namespace

// ---------------------------------------------------------------------------
// IShellQuote
// ---------------------------------------------------------------------------

std::string QuoteCommandArg(const std::string& arg) {
    if (arg.empty()) return "''";
    bool safe = true;
    for (char c : arg) {
        if (!IsShellSafe(c)) {
            safe = false;
            break;
        }
    }
    if (safe) return arg;

    std::string out;
    out.reserve(arg.size() + 8);
    out.push_back('\'');
    for (char c : arg) {
        if (c == '\'') {
            out += "'\\''";
        } else {
            out.push_back(c);
        }
    }
    out.push_back('\'');
    return out;
}

bool NeedsCommandArgQuoting(const std::string& arg) {
    return arg.empty() || QuoteCommandArg(arg) != arg;
}

std::string QuoteShellCommand(const std::string& command) {
    return command;
}

// ---------------------------------------------------------------------------
// IInstanceLock (flock)
// ---------------------------------------------------------------------------

class FlockInstanceLock final : public IInstanceLock {
public:
    explicit FlockInstanceLock(std::string name) : name_(std::move(name)), path_(LockPathForName(name_)) {}

    ~FlockInstanceLock() override { Release(); }

    bool Acquire(std::string& outError) override {
        if (fd_ >= 0) return true;
        fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (fd_ < 0) {
            outError = "cannot open instance lock " + path_ + ": " + std::strerror(errno);
            return false;
        }
        if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
            outError = "another Hancho instance owns this data directory (" + path_ + ")";
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        std::string pid = std::to_string(static_cast<long long>(getpid())) + "\n";
        (void)!::write(fd_, pid.data(), pid.size());
        held_ = true;
        return true;
    }

    bool IsHeld() const override { return held_; }

    void Release() override {
        if (fd_ >= 0) {
            if (held_) ::flock(fd_, LOCK_UN);
            ::close(fd_);
            fd_ = -1;
        }
        held_ = false;
    }

private:
    std::string name_;
    std::string path_;
    int fd_ = -1;
    bool held_ = false;
};

std::unique_ptr<IInstanceLock> CreateInstanceLock(const std::string& name) {
    return std::make_unique<FlockInstanceLock>(name);
}

bool IsInstanceLockHeld(const std::string& name) {
    const std::string path = LockPathForName(name);
    int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
        ::flock(fd, LOCK_UN);
        ::close(fd);
        return false;
    }
    ::close(fd);
    return true;
}

// ---------------------------------------------------------------------------
// IProcessTree (process group, /proc metrics, optional cgroup v2)
// ---------------------------------------------------------------------------

class PosixProcessTree final : public IProcessTree {
public:
    PosixProcessTree() = default;
    ~PosixProcessTree() override { Release(); }

    bool Attach(uint64_t rootPid, std::string& outError) override {
        (void)outError;
        rootPid_ = rootPid;
        attached_ = rootPid > 0;
#if defined(__linux__)
        // Best effort: a delegated cgroup v2 gives exact accounting. Most
        // installations do not allow creating one, so this is optional.
        cgroupPath_ = "/sys/fs/cgroup/hancho-" + std::to_string(rootPid);
        if (::mkdir(cgroupPath_.c_str(), 0755) == 0) {
            std::ofstream procs(cgroupPath_ + "/cgroup.procs");
            if (procs) procs << rootPid << "\n";
            if (!procs) {
                ::rmdir(cgroupPath_.c_str());
                cgroupPath_.clear();
            }
        } else {
            cgroupPath_.clear();
        }
#endif
        return attached_;
    }

    bool Query(ProcessMetrics& outMetrics) override {
        outMetrics = ProcessMetrics{};
        if (!attached_ || rootPid_ == 0) return false;

#if defined(__linux__)
        if (!cgroupPath_.empty()) {
            uint64_t memory = ReadUintFile(cgroupPath_ + "/memory.current");
            uint64_t micros = 0;
            {
                std::ifstream in(cgroupPath_ + "/cpu.stat");
                std::string key;
                while (in >> key) {
                    if (key == "usage_usec") {
                        in >> micros;
                        break;
                    }
                    std::string rest;
                    std::getline(in, rest);
                }
            }
            uint32_t count = 0;
            {
                std::ifstream in(cgroupPath_ + "/cgroup.procs");
                std::string pidLine;
                while (std::getline(in, pidLine)) {
                    if (!pidLine.empty()) count++;
                }
            }
            if (count > 0 || memory > 0) {
                outMetrics.processCount = count;
                outMetrics.memoryBytes = memory;
                outMetrics.cpuTimeMs = micros / 1000;
                return true;
            }
        }
#endif

#if !defined(__linux__)
        // Outside Linux only the root process is accounted for.
        outMetrics.processCount = 1;
        return true;
#else
        // Process-group fallback: scan /proc for the session/group id.
        uint32_t count = 0;
        uint64_t rssPages = 0;
        uint64_t cpuTicks = 0;
        long ticksPerSec = 100;
        const std::string procDir = "/proc";
        if (DIR* dir = ::opendir(procDir.c_str())) {
            while (struct dirent* ent = ::readdir(dir)) {
                if (ent->d_name[0] < '0' || ent->d_name[0] > '9') continue;
                const std::string statPath = procDir + "/" + ent->d_name + "/stat";
                std::ifstream in(statPath);
                std::string line;
                if (!std::getline(in, line)) continue;
                const size_t rparen = line.rfind(')');
                if (rparen == std::string::npos || rparen + 2 >= line.size()) continue;
                std::istringstream fields(line.substr(rparen + 2));
                std::string state;
                long ppid = 0, pgrp = 0, session = 0;
                fields >> state >> ppid >> pgrp >> session;
                if (pgrp != static_cast<long>(rootPid_) && session != static_cast<long>(rootPid_)) continue;
                unsigned long utime = 0, stime = 0;
                unsigned long skip = 0;
                for (int i = 0; i < 7; ++i) fields >> skip;
                fields >> utime >> stime;
                uint64_t pages = 0;
                std::ifstream statm(procDir + "/" + ent->d_name + "/statm");
                statm >> pages >> pages;
                count++;
                rssPages += pages;
                cpuTicks += utime + stime;
            }
            ::closedir(dir);
        }
        if (count == 0) {
            outMetrics.processCount = 1;
            outMetrics.memoryBytes = 0;
            outMetrics.cpuTimeMs = 0;
            return true;
        }
        const long pageSize = ::sysconf(_SC_PAGESIZE);
        const long clkTck = ::sysconf(_SC_CLK_TCK);
        if (clkTck > 0) ticksPerSec = clkTck;
        outMetrics.processCount = count;
        outMetrics.memoryBytes = rssPages * static_cast<uint64_t>(pageSize > 0 ? pageSize : 4096);
        outMetrics.cpuTimeMs = cpuTicks * 1000ull / static_cast<uint64_t>(ticksPerSec);
        return true;
#endif
    }

    bool Kill(int exitCode) override {
        (void)exitCode;
        if (!attached_ || rootPid_ == 0) return false;
        bool ok = false;
#if defined(__linux__)
        if (!cgroupPath_.empty()) {
            std::ofstream procs(cgroupPath_ + "/cgroup.kill");
            if (procs) {
                procs << 1 << "\n";
                ok = true;
            }
        }
#endif
        if (::kill(-static_cast<pid_t>(rootPid_), SIGKILL) == 0) ok = true;
        if (::kill(static_cast<pid_t>(rootPid_), SIGKILL) == 0) ok = true;
        return ok;
    }

    void Release() override {
#if defined(__linux__)
        if (!cgroupPath_.empty()) {
            std::ofstream procs(cgroupPath_ + "/cgroup.procs");
            // Move leftovers away when possible; removing the directory is best effort.
            ::rmdir(cgroupPath_.c_str());
            cgroupPath_.clear();
        }
#endif
        attached_ = false;
    }

private:
    uint64_t rootPid_ = 0;
    bool attached_ = false;
    std::string cgroupPath_;
};

std::unique_ptr<IProcessTree> CreateProcessTree() {
    return std::make_unique<PosixProcessTree>();
}

// ---------------------------------------------------------------------------
// IIpcTransport (Unix domain socket)
// ---------------------------------------------------------------------------

namespace {

class SocketConnection final : public IIpcConnection {
public:
    explicit SocketConnection(int fd) : fd_(fd) {}
    ~SocketConnection() override { Close(); }

    int Read(void* buffer, size_t length, std::string& outError) override {
        while (true) {
            ssize_t n = ::read(fd_, buffer, length);
            if (n >= 0) return static_cast<int>(n);
            if (errno == EINTR) continue;
            outError = std::string("read failed: ") + std::strerror(errno);
            return -1;
        }
    }

    bool WriteAll(const void* data, size_t length, std::string& outError) override {
        const char* p = static_cast<const char*>(data);
        size_t left = length;
        while (left > 0) {
            ssize_t n = ::write(fd_, p, left);
            if (n < 0) {
                if (errno == EINTR) continue;
                outError = std::string("write failed: ") + std::strerror(errno);
                return false;
            }
            p += n;
            left -= static_cast<size_t>(n);
        }
        return true;
    }

    void ShutdownWrite() override {
        if (fd_ >= 0) ::shutdown(fd_, SHUT_WR);
    }

    void Close() override {
        if (fd_ >= 0) {
            ::shutdown(fd_, SHUT_RDWR);
            ::close(fd_);
            fd_ = -1;
        }
    }

private:
    int fd_ = -1;
};

class UnixIpcServer final : public IIpcServer {
public:
    explicit UnixIpcServer(std::string endpoint) : endpoint_(std::move(endpoint)) {}
    ~UnixIpcServer() override { Stop(); }

    bool Start(std::string& outError) override {
        if (endpoint_.empty()) {
            outError = "empty IPC endpoint";
            return false;
        }
        if (endpoint_.size() >= sizeof(((sockaddr_un*)nullptr)->sun_path)) {
            outError = "IPC endpoint path is too long: " + endpoint_;
            return false;
        }
        listenFd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (listenFd_ < 0) {
            outError = std::string("socket failed: ") + std::strerror(errno);
            return false;
        }
        ::unlink(endpoint_.c_str());
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", endpoint_.c_str());
        if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            outError = "bind " + endpoint_ + " failed: " + std::strerror(errno);
            ::close(listenFd_);
            listenFd_ = -1;
            return false;
        }
        ::chmod(endpoint_.c_str(), 0600);
        if (::listen(listenFd_, 16) != 0) {
            outError = std::string("listen failed: ") + std::strerror(errno);
            ::close(listenFd_);
            listenFd_ = -1;
            ::unlink(endpoint_.c_str());
            return false;
        }
        return true;
    }

    std::unique_ptr<IIpcConnection> Accept(std::string& outError) override {
        while (listenFd_ >= 0) {
#if defined(__linux__)
            int fd = ::accept4(listenFd_, nullptr, nullptr, SOCK_CLOEXEC);
#else
            int fd = ::accept(listenFd_, nullptr, nullptr);
            if (fd >= 0) ::fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
            if (fd >= 0) return std::make_unique<SocketConnection>(fd);
            if (errno == EINTR) continue;
            if (errno == EBADF || errno == EINVAL) break;
            outError = std::string("accept failed: ") + std::strerror(errno);
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        return nullptr;
    }

    void Stop() override {
        if (listenFd_ >= 0) {
            ::shutdown(listenFd_, SHUT_RDWR);
            ::close(listenFd_);
            listenFd_ = -1;
        }
        if (!endpoint_.empty()) ::unlink(endpoint_.c_str());
    }

    std::string Endpoint() const override { return endpoint_; }

private:
    std::string endpoint_;
    int listenFd_ = -1;
};

} // namespace

std::unique_ptr<IIpcServer> CreateIpcServer(const std::string& endpoint) {
    return std::make_unique<UnixIpcServer>(endpoint);
}

std::unique_ptr<IIpcConnection> ConnectIpc(const std::string& endpoint, int timeoutMs,
                                           std::string& outError) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(
        timeoutMs > 0 ? timeoutMs : 0);
    while (true) {
        int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) {
            outError = std::string("socket failed: ") + std::strerror(errno);
            return nullptr;
        }
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", endpoint.c_str());
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
            return std::make_unique<SocketConnection>(fd);
        }
        const int connectErrno = errno;
        ::close(fd);
        if (timeoutMs <= 0 || std::chrono::steady_clock::now() >= deadline) {
            outError = "cannot connect to " + endpoint + ": " + std::strerror(connectErrno);
            return nullptr;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// ---------------------------------------------------------------------------
// IPty (forkpty)
// ---------------------------------------------------------------------------

class ForkPty final : public IPty {
public:
    ForkPty(pid_t pid, int fd) : pid_(pid), fd_(fd) {}
    ~ForkPty() override { Close(); }

    int64_t Pid() const override { return pid_; }

    int Read(void* buffer, size_t length) override {
        while (true) {
            ssize_t n = ::read(fd_, buffer, length);
            if (n >= 0) return static_cast<int>(n);
            if (errno == EINTR) continue;
            return -1;
        }
    }

    bool Write(const void* data, size_t length) override {
        const char* p = static_cast<const char*>(data);
        size_t left = length;
        while (left > 0) {
            ssize_t n = ::write(fd_, p, left);
            if (n < 0) {
                if (errno == EINTR) continue;
                return false;
            }
            p += n;
            left -= static_cast<size_t>(n);
        }
        return true;
    }

    bool Resize(uint16_t cols, uint16_t rows) override {
        winsize ws{};
        ws.ws_col = cols;
        ws.ws_row = rows;
        return ::ioctl(fd_, TIOCSWINSZ, &ws) == 0;
    }

    int WaitExit() override {
        if (pid_ <= 0) return -1;
        int status = 0;
        while (::waitpid(pid_, &status, 0) < 0) {
            if (errno != EINTR) return -1;
        }
        pid_ = -1;
        if (WIFEXITED(status)) return WEXITSTATUS(status);
        if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
        return -1;
    }

    void Close() override {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        if (pid_ > 0) {
            int status = 0;
            ::waitpid(pid_, &status, WNOHANG);
            pid_ = -1;
        }
    }

private:
    pid_t pid_ = -1;
    int fd_ = -1;
};

std::unique_ptr<IPty> SpawnPty(const PtySpawnOptions& options, std::string& outError) {
    winsize ws{};
    ws.ws_col = options.cols;
    ws.ws_row = options.rows;
    int masterFd = -1;
    pid_t pid = ::forkpty(&masterFd, nullptr, nullptr, &ws);
    if (pid < 0) {
        outError = std::string("forkpty failed: ") + std::strerror(errno);
        return nullptr;
    }
    if (pid == 0) {
        // forkpty/login_tty already created the session and controlling terminal.
        for (const auto& entry : options.env) {
            const size_t eq = entry.find('=');
            if (eq == std::string::npos) continue;
            ::setenv(entry.substr(0, eq).c_str(), entry.substr(eq + 1).c_str(), 1);
        }
        if (!options.workingDir.empty() && ::chdir(options.workingDir.c_str()) != 0) {
            _exit(127);
        }
        const char* shell = std::getenv("SHELL");
        if (!shell || !*shell) shell = "/bin/sh";
        ::execl(shell, shell, "-c", options.command.c_str(), static_cast<char*>(nullptr));
        _exit(127);
    }
    ::fcntl(masterFd, F_SETFD, FD_CLOEXEC);
    return std::make_unique<ForkPty>(pid, masterFd);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

uint32_t CurrentProcessId() {
    return static_cast<uint32_t>(getpid());
}

void CloseFd(int fd) {
    if (fd >= 0) ::close(fd);
}

std::wstring ToNativePath(const std::wstring& path) {
    std::wstring out = path;
    std::replace(out.begin(), out.end(), L'\\', L'/');
    return out;
}

} // namespace platform
} // namespace pc

#endif // !_WIN32
