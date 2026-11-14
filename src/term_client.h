#pragma once

#include <string>

namespace pc {

// Options for the standalone WinConsoleServer (WCSP) client window.
struct TermClientOptions {
    bool autoConnect = false;
    // When true (and host/port are not given) the client resolves the local
    // core through the named-pipe `term_endpoint` action.
    bool local = true;
    std::string host;
    int port = 0;
    std::string token;
    // WCSP shell field: "powershell", "cmd", "bash" or "attach:<id>".
    std::string target;
    // When true (default) the client attaches to newly discovered local
    // ConPTY jobs, matching the old dashboard behavior.
    bool autoAttach = true;
};

// Runs the tabbed WCSP terminal client (HanchoTerm.exe). Each tab is an
// independent WCSP session, so several consoles can be connected at once.
int RunTerminalClient(const TermClientOptions& options);

// Window class of the terminal client window. It is derived from the data
// directory so two Hancho instances do not focus each other's client.
const std::wstring& TerminalClientWindowClass();

// Brings an existing client window to the front, or starts HanchoTerm.exe.
// Safe to call from the dashboard.
bool LaunchTerminalClientWindow();

} // namespace pc
