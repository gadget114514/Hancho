#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace pc {

class ProcessManager;

struct TermSessionInfo {
    uint32_t id = 0;
    uint32_t jobId = 0;     // ConPTY job the session is attached to (0 before attach)
    std::string remoteAddr; // e.g. "127.0.0.1:54321"
    std::string target;     // e.g. "powershell" or "attach:12"
    int64_t connectedSec = 0;
};

// WCSP (WinConsoleServer protocol) terminal server. It exposes the core's
// headless ConPTY jobs to the standalone dashboard and (optionally) to
// WinConsoleTerminal on another PC.
class TermServer {
public:
    explicit TermServer(ProcessManager& pm);
    ~TermServer();

    TermServer(const TermServer&) = delete;
    TermServer& operator=(const TermServer&) = delete;

    bool Start(std::string& error);
    void Stop();

    bool IsRunning() const;
    uint16_t Port() const;
    std::string Token() const;
    void SetToken(const std::string& token);

    std::vector<TermSessionInfo> ListSessions() const;
    bool KillSession(uint32_t id);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Generates a 32-character random hex token for WCSP authentication.
std::string GenerateTermToken();

} // namespace pc
