#pragma once

#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace pc {

struct EphemeralCoreOptions {
    // If empty, defaults to <cwd>\.hancho-ci\<pid>.
    std::wstring dataDir;
    // Optional directory to copy config files from (config.json, engines.json,
    // llms.json, failure-rules.json, permits.json).
    std::wstring seedFrom;
    // When false (default), writes triage_mode="off" and triage_llm_mode="manual"
    // to <dataDir>/config.json so CI runs do not invoke an LLM for triage.
    bool triage = false;
};

// Launches a disposable `HanchoCore.exe --headless` bound to this process's
// lifetime via a Win32 Job Object (KILL_ON_JOB_CLOSE) or POSIX process group.
// Stops the core cleanly on Stop() or destruction, cancelling any remaining jobs.
class EphemeralCoreGuard {
public:
    EphemeralCoreGuard() = default;
    ~EphemeralCoreGuard();

    EphemeralCoreGuard(const EphemeralCoreGuard&) = delete;
    EphemeralCoreGuard& operator=(const EphemeralCoreGuard&) = delete;

    bool Start(const EphemeralCoreOptions& opts, std::string& outError);
    void Stop();

    bool IsRunning() const { return started_; }
    const std::wstring& DataDir() const { return dataDir_; }

private:
    bool started_ = false;
    std::wstring dataDir_;
#ifdef _WIN32
    HANDLE job_ = nullptr;
    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
#else
    int pid_ = -1;
#endif
};

} // namespace pc
