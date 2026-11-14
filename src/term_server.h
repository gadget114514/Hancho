#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace pc {

class ProcessManager;

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

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pc
