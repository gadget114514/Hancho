#include "platform.h"

#ifdef _WIN32

// Windows implementations of the platform boundary (doc/design-v3.md 2.4).
// Everything that touches Win32 for the headless core lives here: shell
// quoting, the per-data-directory instance lock, Job Object process trees,
// Named Pipe IPC and ConPTY.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <psapi.h>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace pc {
namespace platform {

namespace {

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return std::wstring();
    int need = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                   nullptr, 0);
    if (need <= 0) return std::wstring();
    std::wstring out(static_cast<size_t>(need), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                        out.data(), need);
    return out;
}

std::string Win32ErrorMessage(DWORD code) {
    return "Win32 error " + std::to_string(static_cast<unsigned long>(code));
}

} // namespace

// ---------------------------------------------------------------------------
// IShellQuote
// ---------------------------------------------------------------------------

std::string QuoteCommandArg(const std::string& arg) {
    // CommandLineToArgvW-compatible quoting. This is the exact rule the Windows
    // core used before the boundary existed, so command lines do not change.
    std::string out;
    out.reserve(arg.size() + 2);
    out.push_back('"');
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            out.push_back(c);
        } else if (c == '"') {
            out.append(backslashes + 1, '\\');
            out.push_back('"');
            backslashes = 0;
        } else {
            backslashes = 0;
            out.push_back(c);
        }
    }
    out.append(backslashes, '\\');
    out.push_back('"');
    return out;
}

bool NeedsCommandArgQuoting(const std::string& arg) {
    for (char c : arg) {
        if (std::isspace(static_cast<unsigned char>(c)) || c == '"' || c == '&' ||
            c == '|' || c == '<' || c == '>' || c == '^' || c == '(' || c == ')') {
            return true;
        }
    }
    return false;
}

std::string QuoteShellCommand(const std::string& command) {
    // Quote for cmd.exe /d /s /c. Internal quotes are escaped so the outer
    // pair survives cmd's quote-stripping rules.
    if (command.empty()) return "\"\"";
    std::string escaped;
    escaped.reserve(command.size() + 2);
    for (char c : command) {
        if (c == '"') escaped.push_back('\\');
        escaped.push_back(c);
    }
    return "\"" + escaped + "\"";
}

// ---------------------------------------------------------------------------
// IInstanceLock (named mutex)
// ---------------------------------------------------------------------------

namespace {

class MutexInstanceLock final : public IInstanceLock {
public:
    explicit MutexInstanceLock(std::string name) : name_(std::move(name)) {}
    ~MutexInstanceLock() override { Release(); }

    bool Acquire(std::string& outError) override {
        if (held_) return true;
        const std::wstring wideName = Utf8ToWide(name_);
        HANDLE handle = CreateMutexW(nullptr, TRUE, wideName.c_str());
        if (!handle) {
            outError = "CreateMutexW failed: " + Win32ErrorMessage(GetLastError());
            return false;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(handle);
            outError = "another Hancho instance owns this data directory (" + name_ + ")";
            return false;
        }
        handle_ = handle;
        held_ = true;
        return true;
    }

    bool IsHeld() const override { return held_; }

    void Release() override {
        if (handle_) {
            if (held_) ReleaseMutex(handle_);
            CloseHandle(handle_);
            handle_ = nullptr;
        }
        held_ = false;
    }

private:
    std::string name_;
    HANDLE handle_ = nullptr;
    bool held_ = false;
};

} // namespace

std::unique_ptr<IInstanceLock> CreateInstanceLock(const std::string& name) {
    return std::make_unique<MutexInstanceLock>(name);
}

bool IsInstanceLockHeld(const std::string& name) {
    const std::wstring wideName = Utf8ToWide(name);
    HANDLE handle = OpenMutexW(SYNCHRONIZE, FALSE, wideName.c_str());
    if (!handle) return false;
    CloseHandle(handle);
    return true;
}

// ---------------------------------------------------------------------------
// IProcessTree (Job Object)
// ---------------------------------------------------------------------------

namespace {

class JobProcessTree final : public IProcessTree {
public:
    JobProcessTree() = default;
    ~JobProcessTree() override { Release(); }

    bool Attach(uint64_t rootPid, std::string& outError) override {
        Release();
        if (rootPid == 0) {
            outError = "cannot attach a process tree to pid 0";
            return false;
        }

        job_ = CreateJobObjectW(nullptr, nullptr);
        if (!job_) {
            outError = "CreateJobObjectW failed: " + Win32ErrorMessage(GetLastError());
            return false;
        }

        JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
        jeli.BasicLimitInformation.LimitFlags =
            JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
        SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));

        HANDLE process = OpenProcess(
            PROCESS_SET_QUOTA | PROCESS_TERMINATE | PROCESS_QUERY_LIMITED_INFORMATION,
            FALSE, static_cast<DWORD>(rootPid));
        if (!process) {
            outError = "OpenProcess failed: " + Win32ErrorMessage(GetLastError());
            CloseHandle(job_);
            job_ = nullptr;
            return false;
        }

        rootProcess_ = process;
        rootPid_ = rootPid;
        attached_ = true;

        assigned_ = AssignProcessToJobObject(job_, process) != FALSE;
        if (!assigned_) {
            // Keep the root handle so Kill() can still terminate the process;
            // descendants will not be covered by the job in this case.
            outError = "AssignProcessToJobObject failed: " + Win32ErrorMessage(GetLastError());
        }
        return true;
    }

    bool Query(ProcessMetrics& outMetrics) override {
        outMetrics = ProcessMetrics{};
        if (!attached_ || rootPid_ == 0) return false;

        if (job_) {
            JOBOBJECT_BASIC_ACCOUNTING_INFORMATION acct = {};
            if (QueryInformationJobObject(job_, JobObjectBasicAccountingInformation,
                                          &acct, sizeof(acct), nullptr)) {
                uint64_t total100ns = static_cast<uint64_t>(acct.TotalUserTime.QuadPart) +
                                      static_cast<uint64_t>(acct.TotalKernelTime.QuadPart);
                outMetrics.cpuTimeMs = total100ns / 10000ull;
            }
        }

        alignas(JOBOBJECT_BASIC_PROCESS_ID_LIST)
        BYTE buffer[sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + sizeof(ULONG_PTR) * 1024] = {};
        auto* pidList = reinterpret_cast<PJOBOBJECT_BASIC_PROCESS_ID_LIST>(buffer);
        bool listed = false;
        if (job_ && assigned_) {
            listed = QueryInformationJobObject(job_, JobObjectBasicProcessIdList, pidList,
                                               sizeof(buffer), nullptr) != FALSE;
        }

        if (listed) {
            outMetrics.processCount = static_cast<uint32_t>(pidList->NumberOfProcessIdsInList);
            uint64_t totalMemory = 0;
            for (DWORD i = 0; i < pidList->NumberOfProcessIdsInList; ++i) {
                DWORD pid = static_cast<DWORD>(pidList->ProcessIdList[i]);
                HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ,
                                             FALSE, pid);
                if (!process) continue;
                PROCESS_MEMORY_COUNTERS pmc = {};
                pmc.cb = sizeof(pmc);
                if (GetProcessMemoryInfo(process, &pmc, sizeof(pmc))) {
                    totalMemory += pmc.WorkingSetSize;
                }
                CloseHandle(process);
            }
            outMetrics.memoryBytes = totalMemory;
        } else if (rootProcess_) {
            // The process is not in the job (Attach failed); account the root only.
            outMetrics.processCount = 1;
            PROCESS_MEMORY_COUNTERS pmc = {};
            pmc.cb = sizeof(pmc);
            if (GetProcessMemoryInfo(rootProcess_, &pmc, sizeof(pmc))) {
                outMetrics.memoryBytes = pmc.WorkingSetSize;
            }
        }
        return true;
    }

    bool Kill(int exitCode) override {
        if (!attached_) return false;
        bool ok = false;
        if (job_) {
            if (TerminateJobObject(job_, static_cast<UINT>(exitCode))) ok = true;
        }
        if (rootProcess_) {
            if (TerminateProcess(rootProcess_, static_cast<UINT>(exitCode))) ok = true;
        }
        return ok;
    }

    void Release() override {
        if (job_) {
            // Release must not kill: clear KILL_ON_JOB_CLOSE before closing.
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
            jeli.BasicLimitInformation.LimitFlags = 0;
            SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
            CloseHandle(job_);
            job_ = nullptr;
        }
        if (rootProcess_) {
            CloseHandle(rootProcess_);
            rootProcess_ = nullptr;
        }
        attached_ = false;
        assigned_ = false;
        rootPid_ = 0;
    }

private:
    HANDLE job_ = nullptr;
    HANDLE rootProcess_ = nullptr;
    uint64_t rootPid_ = 0;
    bool attached_ = false;
    bool assigned_ = false;
};

} // namespace

std::unique_ptr<IProcessTree> CreateProcessTree() {
    return std::make_unique<JobProcessTree>();
}

// ---------------------------------------------------------------------------
// IIpcTransport (Named Pipe, byte mode)
// ---------------------------------------------------------------------------

namespace {

class PipeConnection final : public IIpcConnection {
public:
    explicit PipeConnection(HANDLE handle) : handle_(handle) {}
    ~PipeConnection() override { Close(); }

    int Read(void* buffer, size_t length, std::string& outError) override {
        if (handle_ == INVALID_HANDLE_VALUE || !handle_) return 0;
        DWORD got = 0;
        if (ReadFile(handle_, buffer, static_cast<DWORD>(length), &got, nullptr)) {
            return static_cast<int>(got);
        }
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED ||
            err == ERROR_HANDLE_EOF) {
            return 0;
        }
        outError = "ReadFile failed: " + Win32ErrorMessage(err);
        return -1;
    }

    bool WriteAll(const void* data, size_t length, std::string& outError) override {
        if (handle_ == INVALID_HANDLE_VALUE || !handle_) {
            outError = "pipe is closed";
            return false;
        }
        const char* p = static_cast<const char*>(data);
        size_t left = length;
        while (left > 0) {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(left, 1u << 20));
            DWORD written = 0;
            if (!WriteFile(handle_, p, chunk, &written, nullptr)) {
                outError = "WriteFile failed: " + Win32ErrorMessage(GetLastError());
                return false;
            }
            p += written;
            left -= written;
        }
        return true;
    }

    void ShutdownWrite() override {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) FlushFileBuffers(handle_);
    }

    void Close() override {
        if (handle_ && handle_ != INVALID_HANDLE_VALUE) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};

class PipeIpcServer final : public IIpcServer {
public:
    explicit PipeIpcServer(std::string endpoint)
        : endpoint_(std::move(endpoint)), wideEndpoint_(Utf8ToWide(endpoint_)) {}
    ~PipeIpcServer() override { Stop(); }

    bool Start(std::string& outError) override {
        if (endpoint_.empty()) {
            outError = "empty IPC endpoint";
            return false;
        }
        stop_ = false;
        started_ = true;
        return true;
    }

    std::unique_ptr<IIpcConnection> Accept(std::string& outError) override {
        if (!started_) {
            outError = "IPC server is not started";
            return nullptr;
        }
        while (!stop_) {
            HANDLE pipe = CreateNamedPipeW(
                wideEndpoint_.c_str(),
                PIPE_ACCESS_DUPLEX,
                PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                PIPE_UNLIMITED_INSTANCES,
                256 * 1024,
                64 * 1024,
                0,
                nullptr);
            if (pipe == INVALID_HANDLE_VALUE) {
                if (stop_) break;
                outError = "CreateNamedPipeW failed: " + Win32ErrorMessage(GetLastError());
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                continue;
            }

            BOOL connected = ConnectNamedPipe(pipe, nullptr)
                                 ? TRUE
                                 : (GetLastError() == ERROR_PIPE_CONNECTED);
            if (stop_) {
                CloseHandle(pipe);
                break;
            }
            if (connected) {
                return std::make_unique<PipeConnection>(pipe);
            }
            DWORD err = GetLastError();
            CloseHandle(pipe);
            if (err == ERROR_OPERATION_ABORTED) break;
            outError = "ConnectNamedPipe failed: " + Win32ErrorMessage(err);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        outError = "IPC server stopped";
        return nullptr;
    }

    void Stop() override {
        stop_ = true;
        started_ = false;
        // Wake a blocking ConnectNamedPipe with a short-lived client.
        HANDLE dummy = CreateFileW(wideEndpoint_.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                   nullptr, OPEN_EXISTING, 0, nullptr);
        if (dummy != INVALID_HANDLE_VALUE) CloseHandle(dummy);
    }

    std::string Endpoint() const override { return endpoint_; }

private:
    std::string endpoint_;
    std::wstring wideEndpoint_;
    std::atomic<bool> stop_{false};
    bool started_ = false;
};

} // namespace

std::unique_ptr<IIpcServer> CreateIpcServer(const std::string& endpoint) {
    return std::make_unique<PipeIpcServer>(endpoint);
}

std::unique_ptr<IIpcConnection> ConnectIpc(const std::string& endpoint, int timeoutMs,
                                           std::string& outError) {
    const std::wstring wideEndpoint = Utf8ToWide(endpoint);
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs > 0 ? timeoutMs : 0);
    while (true) {
        HANDLE pipe = CreateFileW(wideEndpoint.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                                  nullptr, OPEN_EXISTING, 0, nullptr);
        if (pipe != INVALID_HANDLE_VALUE) {
            return std::make_unique<PipeConnection>(pipe);
        }
        DWORD err = GetLastError();
        if (timeoutMs <= 0 || std::chrono::steady_clock::now() >= deadline) {
            outError = "cannot connect to " + endpoint + ": " + Win32ErrorMessage(err);
            return nullptr;
        }
        if (err == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(wideEndpoint.c_str(), 200);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

// ---------------------------------------------------------------------------
// IPty (ConPTY)
// ---------------------------------------------------------------------------

namespace {

using FnCreatePseudoConsole = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, HPCON*);
using FnResizePseudoConsole = HRESULT(WINAPI*)(HPCON, COORD);
using FnClosePseudoConsole = void(WINAPI*)(HPCON);

struct ConptyApi {
    HMODULE dll = nullptr;
    FnCreatePseudoConsole create = nullptr;
    FnResizePseudoConsole resize = nullptr;
    FnClosePseudoConsole close = nullptr;
    bool loaded = false;
};

ConptyApi& GetConptyApi() {
    static ConptyApi api = [] {
        ConptyApi result;
        wchar_t exeDir[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exeDir, MAX_PATH);
        wchar_t* slash = wcsrchr(exeDir, L'\\');
        if (slash) *slash = L'\0';

        const std::wstring candidates[] = {
            std::wstring(exeDir) + L"\\conpty.dll",
            std::wstring(exeDir) + L"\\..\\lib\\conpty.dll",
            L"conpty.dll",
        };
        for (const auto& path : candidates) {
            result.dll = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (result.dll) break;
        }
        if (!result.dll) {
            result.dll = LoadLibraryW(L"conpty.dll");
        }
        if (result.dll) {
            result.create = reinterpret_cast<FnCreatePseudoConsole>(
                GetProcAddress(result.dll, "ConptyCreatePseudoConsole"));
            result.resize = reinterpret_cast<FnResizePseudoConsole>(
                GetProcAddress(result.dll, "ConptyResizePseudoConsole"));
            result.close = reinterpret_cast<FnClosePseudoConsole>(
                GetProcAddress(result.dll, "ConptyClosePseudoConsole"));
            if (!result.create || !result.resize || !result.close) {
                result.create = nullptr;
                result.resize = nullptr;
                result.close = nullptr;
            }
        }
        if (!result.create) {
            // Fall back to the system implementation when no bundled conpty.dll exists.
            HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
            if (!kernel) kernel = LoadLibraryW(L"kernel32.dll");
            if (kernel) {
                result.create = reinterpret_cast<FnCreatePseudoConsole>(
                    GetProcAddress(kernel, "CreatePseudoConsole"));
                result.resize = reinterpret_cast<FnResizePseudoConsole>(
                    GetProcAddress(kernel, "ResizePseudoConsole"));
                result.close = reinterpret_cast<FnClosePseudoConsole>(
                    GetProcAddress(kernel, "ClosePseudoConsole"));
            }
        }
        result.loaded = result.create && result.resize && result.close;
        return result;
    }();
    return api;
}

std::vector<wchar_t> BuildEnvironmentBlock(const std::vector<std::string>& extra,
                                           const std::wstring& cwd) {
    std::vector<std::pair<std::wstring, std::wstring>> additions;
    for (const auto& entry : extra) {
        size_t eq = entry.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        additions.emplace_back(Utf8ToWide(entry.substr(0, eq)),
                               Utf8ToWide(entry.substr(eq + 1)));
    }
    if (!cwd.empty()) {
        additions.emplace_back(L"PWD", cwd);
        additions.emplace_back(L"INIT_CWD", cwd);
    }

    auto overridden = [&additions](const std::wstring& key) {
        for (const auto& kv : additions) {
            if (_wcsicmp(kv.first.c_str(), key.c_str()) == 0) return true;
        }
        return false;
    };

    std::vector<wchar_t> block;
    auto append = [&block](const std::wstring& entry) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back(L'\0');
    };

    LPWCH env = GetEnvironmentStringsW();
    if (env) {
        for (LPWCH p = env; *p;) {
            std::wstring entry(p);
            p += entry.size() + 1;
            if (entry[0] != L'=') {
                size_t eq = entry.find(L'=');
                std::wstring key = (eq == std::wstring::npos) ? entry : entry.substr(0, eq);
                if (_wcsicmp(key.c_str(), L"PWD") == 0 ||
                    _wcsicmp(key.c_str(), L"OLDPWD") == 0 ||
                    _wcsicmp(key.c_str(), L"INIT_CWD") == 0 ||
                    overridden(key)) {
                    continue;
                }
            }
            append(entry);
        }
        FreeEnvironmentStringsW(env);
    }
    for (const auto& kv : additions) append(kv.first + L"=" + kv.second);
    block.push_back(L'\0');
    return block;
}

class WindowsPty final : public IPty {
public:
    WindowsPty(HANDLE inputWrite, HANDLE outputRead, HANDLE process, HANDLE thread,
               HPCON pseudoConsole, PPROC_THREAD_ATTRIBUTE_LIST attributes, uint32_t pid)
        : inputWrite_(inputWrite),
          outputRead_(outputRead),
          process_(process),
          thread_(thread),
          pseudoConsole_(pseudoConsole),
          attributes_(attributes),
          pid_(pid) {}

    ~WindowsPty() override { Close(); }

    int64_t Pid() const override { return static_cast<int64_t>(pid_); }

    int Read(void* buffer, size_t length) override {
        if (!outputRead_ || outputRead_ == INVALID_HANDLE_VALUE) return 0;
        DWORD got = 0;
        if (ReadFile(outputRead_, buffer, static_cast<DWORD>(length), &got, nullptr)) {
            return static_cast<int>(got);
        }
        DWORD err = GetLastError();
        if (err == ERROR_BROKEN_PIPE || err == ERROR_PIPE_NOT_CONNECTED ||
            err == ERROR_HANDLE_EOF || err == ERROR_OPERATION_ABORTED) {
            return 0;
        }
        return -1;
    }

    bool Write(const void* data, size_t length) override {
        if (!inputWrite_ || inputWrite_ == INVALID_HANDLE_VALUE) return false;
        const char* p = static_cast<const char*>(data);
        size_t left = length;
        while (left > 0) {
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(left, 1u << 20));
            DWORD written = 0;
            if (!WriteFile(inputWrite_, p, chunk, &written, nullptr)) return false;
            p += written;
            left -= written;
        }
        return true;
    }

    bool Resize(uint16_t cols, uint16_t rows) override {
        ConptyApi& api = GetConptyApi();
        if (!pseudoConsole_ || !api.resize) return false;
        COORD size{static_cast<SHORT>(std::max<uint16_t>(2, cols)),
                   static_cast<SHORT>(std::max<uint16_t>(1, rows))};
        return SUCCEEDED(api.resize(pseudoConsole_, size));
    }

    int WaitExit() override {
        if (!process_) return -1;
        WaitForSingleObject(process_, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(process_, &code);
        return static_cast<int>(code);
    }

    void Close() override {
        if (inputWrite_ && inputWrite_ != INVALID_HANDLE_VALUE) {
            CloseHandle(inputWrite_);
            inputWrite_ = INVALID_HANDLE_VALUE;
        }
        if (process_) {
            if (WaitForSingleObject(process_, 200) == WAIT_TIMEOUT) {
                TerminateProcess(process_, 1);
            }
        }
        ConptyApi& api = GetConptyApi();
        if (pseudoConsole_ && api.close) {
            api.close(pseudoConsole_);
            pseudoConsole_ = nullptr;
        }
        if (outputRead_ && outputRead_ != INVALID_HANDLE_VALUE) {
            CloseHandle(outputRead_);
            outputRead_ = INVALID_HANDLE_VALUE;
        }
        if (attributes_) {
            DeleteProcThreadAttributeList(attributes_);
            HeapFree(GetProcessHeap(), 0, attributes_);
            attributes_ = nullptr;
        }
        if (thread_) {
            CloseHandle(thread_);
            thread_ = nullptr;
        }
        if (process_) {
            CloseHandle(process_);
            process_ = nullptr;
        }
        pid_ = 0;
    }

private:
    HANDLE inputWrite_ = INVALID_HANDLE_VALUE;
    HANDLE outputRead_ = INVALID_HANDLE_VALUE;
    HANDLE process_ = nullptr;
    HANDLE thread_ = nullptr;
    HPCON pseudoConsole_ = nullptr;
    PPROC_THREAD_ATTRIBUTE_LIST attributes_ = nullptr;
    uint32_t pid_ = 0;
};

} // namespace

std::unique_ptr<IPty> SpawnPty(const PtySpawnOptions& options, std::string& outError) {
    ConptyApi& api = GetConptyApi();
    if (!api.loaded) {
        outError = "ConPTY is not available (ConptyCreatePseudoConsole not found)";
        return nullptr;
    }

    SECURITY_ATTRIBUTES sa = {};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE ptyInputRead = INVALID_HANDLE_VALUE;
    HANDLE ptyInputWrite = INVALID_HANDLE_VALUE;
    HANDLE ptyOutputRead = INVALID_HANDLE_VALUE;
    HANDLE ptyOutputWrite = INVALID_HANDLE_VALUE;
    if (!CreatePipe(&ptyInputRead, &ptyInputWrite, &sa, 0) ||
        !CreatePipe(&ptyOutputRead, &ptyOutputWrite, &sa, 0)) {
        outError = "CreatePipe failed: " + Win32ErrorMessage(GetLastError());
        if (ptyInputRead != INVALID_HANDLE_VALUE) CloseHandle(ptyInputRead);
        if (ptyInputWrite != INVALID_HANDLE_VALUE) CloseHandle(ptyInputWrite);
        if (ptyOutputRead != INVALID_HANDLE_VALUE) CloseHandle(ptyOutputRead);
        if (ptyOutputWrite != INVALID_HANDLE_VALUE) CloseHandle(ptyOutputWrite);
        return nullptr;
    }
    SetHandleInformation(ptyInputWrite, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(ptyOutputRead, HANDLE_FLAG_INHERIT, 0);

    COORD size{static_cast<SHORT>(std::max<uint16_t>(2, options.cols)),
               static_cast<SHORT>(std::max<uint16_t>(1, options.rows))};
    HPCON pseudoConsole = nullptr;
    HRESULT hr = api.create(size, ptyInputRead, ptyOutputWrite, 0, &pseudoConsole);
    if (FAILED(hr)) {
        outError = "ConptyCreatePseudoConsole failed: 0x" + std::to_string(static_cast<unsigned long>(hr));
        CloseHandle(ptyInputRead);
        CloseHandle(ptyInputWrite);
        CloseHandle(ptyOutputRead);
        CloseHandle(ptyOutputWrite);
        return nullptr;
    }

    CloseHandle(ptyInputRead);
    CloseHandle(ptyOutputWrite);

    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
    auto* attributes = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(
        HeapAlloc(GetProcessHeap(), 0, attributeBytes));
    if (!attributes) {
        outError = "HeapAlloc for the attribute list failed";
        api.close(pseudoConsole);
        CloseHandle(ptyInputWrite);
        CloseHandle(ptyOutputRead);
        return nullptr;
    }
    InitializeProcThreadAttributeList(attributes, 1, 0, &attributeBytes);
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                   pseudoConsole, sizeof(pseudoConsole), nullptr, nullptr)) {
        outError = "UpdateProcThreadAttribute failed: " + Win32ErrorMessage(GetLastError());
        DeleteProcThreadAttributeList(attributes);
        HeapFree(GetProcessHeap(), 0, attributes);
        api.close(pseudoConsole);
        CloseHandle(ptyInputWrite);
        CloseHandle(ptyOutputRead);
        return nullptr;
    }

    STARTUPINFOEXW siEx = {};
    siEx.StartupInfo.cb = sizeof(siEx);
    siEx.lpAttributeList = attributes;

    std::wstring wideCommand = Utf8ToWide(options.command);
    std::vector<wchar_t> commandLine(wideCommand.begin(), wideCommand.end());
    commandLine.push_back(L'\0');

    std::wstring wideCwd = Utf8ToWide(options.workingDir);
    std::vector<wchar_t> envBlock = BuildEnvironmentBlock(options.env, wideCwd);

    DWORD flags = EXTENDED_STARTUPINFO_PRESENT | CREATE_UNICODE_ENVIRONMENT;
    if (options.newSession) flags |= CREATE_NEW_PROCESS_GROUP;

    PROCESS_INFORMATION pi = {};
    BOOL created = CreateProcessW(
        nullptr,
        commandLine.data(),
        nullptr,
        nullptr,
        TRUE,
        flags,
        envBlock.data(),
        wideCwd.empty() ? nullptr : wideCwd.c_str(),
        &siEx.StartupInfo,
        &pi);
    if (!created) {
        outError = "CreateProcessW failed: " + Win32ErrorMessage(GetLastError());
        DeleteProcThreadAttributeList(attributes);
        HeapFree(GetProcessHeap(), 0, attributes);
        api.close(pseudoConsole);
        CloseHandle(ptyInputWrite);
        CloseHandle(ptyOutputRead);
        return nullptr;
    }

    return std::make_unique<WindowsPty>(ptyInputWrite, ptyOutputRead, pi.hProcess, pi.hThread,
                                        pseudoConsole, attributes, pi.dwProcessId);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

uint32_t CurrentProcessId() {
    return static_cast<uint32_t>(GetCurrentProcessId());
}

void CloseFd(int fd) {
    (void)fd; // Windows does not use POSIX file descriptors.
}

std::wstring ToNativePath(const std::wstring& path) {
    return path;
}

} // namespace platform
} // namespace pc

#endif // _WIN32
