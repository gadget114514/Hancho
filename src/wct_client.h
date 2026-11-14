#pragma once

#ifdef _WIN32

#include <string>

namespace pc {

// Options for the wct CUI client. It connects to a WCSP endpoint and relays
// the remote ConPTY bytes to the current console unchanged.
struct WctOptions {
    bool local = true;
    std::string host;
    // Remote WCSP port. 0 means the default (9139) when host is set.
    int port = 0;
    std::string token;
    // WCSP shell field: "powershell", "cmd", "bash" or "attach:<id>".
    std::string target = "powershell";
};

// Runs wct in the current console and returns a process exit code.
// Ctrl+] detaches (the ConPTY job keeps running for a later `wct <id>`);
// Ctrl+\ (or Ctrl+] twice) ends the session (WCSP CLOSE terminates the job).
int RunWct(const WctOptions& options);

} // namespace pc

#endif // _WIN32
