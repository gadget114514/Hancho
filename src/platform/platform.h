#pragma once

// Platform boundary for the headless core (doc/design-v3.md 2.4).
//
// The core (task queue, behaviour tree, stream parsing) calls only this header.
// Windows implementations live next to the existing Win32 code; POSIX
// implementations live in the *_posix.cpp files in this directory.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pc {
namespace platform {

// ---------------------------------------------------------------------------
// IShellQuote
// ---------------------------------------------------------------------------

// Quote one command argument for the platform shell.
std::string QuoteCommandArg(const std::string& arg);

// True when the argument still needs quoting (whitespace, quotes, globs, ...).
bool NeedsCommandArgQuoting(const std::string& arg);

// Quote a complete command line for /bin/sh -c (POSIX) or cmd.exe /c (Windows).
std::string QuoteShellCommand(const std::string& command);

// ---------------------------------------------------------------------------
// IInstanceLock
// ---------------------------------------------------------------------------

// One instance per data directory: named mutex on Windows, flock on POSIX.
class IInstanceLock {
public:
    virtual ~IInstanceLock() = default;
    virtual bool Acquire(std::string& outError) = 0;
    virtual bool IsHeld() const = 0;
    virtual void Release() = 0;
};

std::unique_ptr<IInstanceLock> CreateInstanceLock(const std::string& name);

// Non-owning probe used by hancho-cli reset: true while another process holds it.
bool IsInstanceLockHeld(const std::string& name);

// ---------------------------------------------------------------------------
// IProcessTree
// ---------------------------------------------------------------------------

struct ProcessMetrics {
    uint32_t processCount = 0;
    uint64_t memoryBytes = 0;
    uint64_t cpuTimeMs = 0;
};

// A managed process tree: Job Object on Windows, process group / cgroup v2 on
// POSIX. The root process is attached right after spawn.
class IProcessTree {
public:
    virtual ~IProcessTree() = default;
    virtual bool Attach(uint64_t rootPid, std::string& outError) = 0;
    virtual bool Query(ProcessMetrics& outMetrics) = 0;
    virtual bool Kill(int exitCode) = 0;
    virtual void Release() = 0;
};

std::unique_ptr<IProcessTree> CreateProcessTree();

// ---------------------------------------------------------------------------
// IIpcTransport
// ---------------------------------------------------------------------------

class IIpcConnection {
public:
    virtual ~IIpcConnection() = default;
    // Returns bytes read, 0 on EOF, -1 on error.
    virtual int Read(void* buffer, size_t length, std::string& outError) = 0;
    virtual bool WriteAll(const void* data, size_t length, std::string& outError) = 0;
    // Half-close: the peer sees EOF after the buffered request.
    virtual void ShutdownWrite() = 0;
    virtual void Close() = 0;
};

class IIpcServer {
public:
    virtual ~IIpcServer() = default;
    virtual bool Start(std::string& outError) = 0;
    virtual std::unique_ptr<IIpcConnection> Accept(std::string& outError) = 0;
    virtual void Stop() = 0;
    virtual std::string Endpoint() const = 0;
};

std::unique_ptr<IIpcServer> CreateIpcServer(const std::string& endpoint);

// Connect with retries. timeoutMs <= 0 means try once.
std::unique_ptr<IIpcConnection> ConnectIpc(const std::string& endpoint, int timeoutMs,
                                           std::string& outError);

// ---------------------------------------------------------------------------
// IPty
// ---------------------------------------------------------------------------

struct PtySpawnOptions {
    std::string command;            // passed to the platform shell
    std::string workingDir;         // empty = inherited
    std::vector<std::string> env;   // KEY=VALUE additions
    uint16_t cols = 120;
    uint16_t rows = 30;
    bool newSession = true;
};

class IPty {
public:
    virtual ~IPty() = default;
    virtual int64_t Pid() const = 0;
    virtual int Read(void* buffer, size_t length) = 0;
    virtual bool Write(const void* data, size_t length) = 0;
    virtual bool Resize(uint16_t cols, uint16_t rows) = 0;
    virtual int WaitExit() = 0;
    virtual void Close() = 0;
};

std::unique_ptr<IPty> SpawnPty(const PtySpawnOptions& options, std::string& outError);

// ---------------------------------------------------------------------------
// Small helpers shared by both implementations
// ---------------------------------------------------------------------------

// Current process id as a portable unsigned value.
uint32_t CurrentProcessId();

// Close a file descriptor, ignoring EINTR. No-op on Windows.
void CloseFd(int fd);

// Convert a Windows-style path to the native separator (identity on Windows).
std::wstring ToNativePath(const std::wstring& path);

} // namespace platform
} // namespace pc
