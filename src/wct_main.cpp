#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "wct_client.h"

#include "common.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <string>
#include <vector>

#include <windows.h>
#include <shellapi.h>

#ifndef PC_VERSION
#define PC_VERSION "0.0.0"
#endif

namespace {

void PrintUsage() {
    std::cout <<
"wct " PC_VERSION R"( - Hancho WCSP terminal (CUI)

USAGE:
  wct [options] [job-id]

  Connects to a Hancho core over the WinConsoleServer protocol (WCSP) and
  relays the remote ConPTY to this console. Keyboard input goes to the remote
  shell unchanged. By default the local core is found through the named pipe
  (`term_endpoint`), so Hancho must be running.

  Leaving wct with Ctrl+] detaches: the remote ConPTY job keeps running and
  can be reattached with `wct <job-id>`. Ctrl+\ ends the session (WCSP CLOSE),
  which terminates the remote job. Some Windows terminals filter Ctrl+\, so
  pressing Ctrl+] twice quickly also closes the session. The session also
  ends when the remote shell exits.

OPTIONS:
  --host <host>       Connect to a remote core instead of the local one.
                      If <host> is a name from <data-dir>/hosts.json, it is
                      resolved like --remote.
  --remote <name>     Connect through a host registered in
                      <data-dir>/hosts.json. Its IPC endpoint supplies the WCSP
                      port and token.
  --port <port>       Remote WCSP port (default 9139).
  --token <token>     WCSP shared token (or the HANCHO_TERM_TOKEN env var).
  --local             Force local core discovery even if --host was given.
  --shell <name>      powershell (default), cmd, or bash.
  --attach <id>       Attach to a running headless ConPTY job instead of
                      starting a new shell.
  --data-dir <path>   Data directory of the local core.
  -h, --help          Show this help.
  -V, --version       Show the version.

A bare numeric argument is short for --attach <id>.

KEYS:
  Ctrl+]              Detach and exit wct; the remote job keeps running.
  Ctrl+\              Close the session (terminate the remote job) and exit.
  Ctrl+] twice        Same as Ctrl+\ (for terminals that filter Ctrl+\).

EXAMPLES:
  wct                          # local PowerShell
  wct 12                       # attach to local job #12
  wct --shell cmd
  wct --host 192.168.1.10 --token <token> --attach 12
  wct --remote buildbox --attach 12
)";
}

bool ParsePort(const std::string& text, int& out) {
    if (text.empty()) return false;
    for (unsigned char c : text) {
        if (!std::isdigit(c)) return false;
    }
    try {
        out = std::stoi(text);
    } catch (...) {
        return false;
    }
    return out > 0 && out <= 65535;
}

bool IsDigits(const std::string& text) {
    if (text.empty()) return false;
    return std::all_of(text.begin(), text.end(), [](unsigned char c) {
        return std::isdigit(c) != 0;
    });
}

bool IsKnownShell(const std::string& text) {
    return text == "powershell" || text == "cmd" || text == "bash" ||
           text == "pwsh" || text == "powershell.exe" || text == "cmd.exe" ||
           text == "bash.exe";
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);

    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 0; i < argc; ++i) {
        args.push_back(pc::WideToUtf8(argvW[i]));
    }
    LocalFree(argvW);

    pc::WctOptions options;
    bool forceLocal = false;
    bool sawHost = false;
    std::string dataDir;

    auto requireValue = [&](size_t& i, const std::string& flag, std::string& out) {
        if (i + 1 >= args.size()) {
            std::cerr << "wct: " << flag << " requires a value\n";
            return false;
        }
        out = args[++i];
        return true;
    };

    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "-h" || a == "--help" || a == "help") {
            PrintUsage();
            return 0;
        }
        if (a == "-V" || a == "--version" || a == "version") {
            std::cout << "wct " PC_VERSION << "\n";
            return 0;
        }
        if (a == "--host") {
            if (!requireValue(i, a, options.host)) return 1;
            sawHost = true;
        } else if (a.rfind("--host=", 0) == 0) {
            options.host = a.substr(7);
            sawHost = true;
        } else if (a == "--remote") {
            if (!requireValue(i, a, options.registryHost)) return 1;
            sawHost = true;
        } else if (a.rfind("--remote=", 0) == 0) {
            options.registryHost = a.substr(9);
            sawHost = true;
        } else if (a == "--port") {
            std::string value;
            if (!requireValue(i, a, value)) return 1;
            if (!ParsePort(value, options.port)) {
                std::cerr << "wct: invalid --port value: " << value << "\n";
                return 1;
            }
        } else if (a.rfind("--port=", 0) == 0) {
            if (!ParsePort(a.substr(7), options.port)) {
                std::cerr << "wct: invalid --port value: " << a.substr(7) << "\n";
                return 1;
            }
        } else if (a == "--token") {
            if (!requireValue(i, a, options.token)) return 1;
        } else if (a.rfind("--token=", 0) == 0) {
            options.token = a.substr(8);
        } else if (a == "--shell") {
            if (!requireValue(i, a, options.target)) return 1;
        } else if (a.rfind("--shell=", 0) == 0) {
            options.target = a.substr(8);
        } else if (a == "--attach") {
            std::string id;
            if (!requireValue(i, a, id)) return 1;
            options.target = "attach:" + id;
        } else if (a.rfind("--attach=", 0) == 0) {
            options.target = "attach:" + a.substr(9);
        } else if (a == "--local") {
            forceLocal = true;
        } else if (a == "--data-dir") {
            if (!requireValue(i, a, dataDir)) return 1;
        } else if (a.rfind("--data-dir=", 0) == 0) {
            dataDir = a.substr(11);
        } else if (a.size() > 1 && a[0] == '-') {
            std::cerr << "wct: unknown option: " << a << "\n";
            std::cerr << "Run `wct --help` for usage.\n";
            return 1;
        } else if (a.rfind("attach:", 0) == 0) {
            options.target = a;
        } else if (IsDigits(a)) {
            options.target = "attach:" + a;
        } else if (IsKnownShell(a)) {
            options.target = a;
        } else {
            std::cerr << "wct: unknown argument: " << a << "\n";
            std::cerr << "Run `wct --help` for usage.\n";
            return 1;
        }
    }

    if (!dataDir.empty()) {
        pc::SetDataDirectoryOverride(pc::Utf8ToWide(dataDir));
    }

    options.local = forceLocal || !sawHost;
    if (options.target.empty()) options.target = "powershell";

    return pc::RunWct(options);
}
