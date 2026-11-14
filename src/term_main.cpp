// HanchoTerm.exe: standalone WinConsoleServer (WCSP) terminal client.
//
// This used to be `Hancho.exe terminal`. It is now its own executable so the
// terminal client can be built, shipped and started independently from Hancho.

#include "term_client.h"

#include "common.h"

#include <windows.h>
#include <shellapi.h>

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#ifndef PC_VERSION
#define PC_VERSION "0.0.0"
#endif

namespace {

void ShowUsage(HWND owner) {
    const wchar_t* text =
        L"HanchoTerm " PC_VERSION L" - WCSP ターミナル\n\n"
        L"オプション:\n"
        L"  --host <host> --port <port> --token <token>  リモートコアに接続\n"
        L"  --shell <shell>    powershell / cmd / bash (既定: powershell)\n"
        L"  --target <target>  \"attach:<ジョブID>\" で既存ジョブを表示\n"
        L"  --attach <id>      既存ジョブに接続\n"
        L"  --local            ローカルコアへの接続を強制\n"
        L"  --no-auto-attach   ローカル ConPTY ジョブへ自動接続しない\n"
        L"  --no-connect       タブを開かない (自動接続も無効)\n"
        L"  --data-dir <path>  ローカルコアのデータディレクトリ\n";
    MessageBoxW(owner, text, L"HanchoTerm", MB_OK | MB_ICONINFORMATION);
}

} // namespace

using namespace pc;

int main() {
    int argc = 0;
    LPWSTR* argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    if (argvW) {
        for (int i = 0; i < argc; ++i) args.push_back(WideToUtf8(argvW[i]));
        LocalFree(argvW);
    }

    std::vector<std::string> filtered;
    filtered.push_back(args.empty() ? "HanchoTerm.exe" : args[0]);
    std::string dataDirArg;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--data-dir" && i + 1 < args.size()) {
            dataDirArg = args[++i];
        } else if (args[i].rfind("--data-dir=", 0) == 0) {
            dataDirArg = args[i].substr(11);
        } else {
            filtered.push_back(args[i]);
        }
    }
    args.swap(filtered);
    if (!dataDirArg.empty()) {
        SetDataDirectoryOverride(Utf8ToWide(dataDirArg));
    }

    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--help" || args[i] == "-h") {
            ShowUsage(nullptr);
            return 0;
        }
        if (args[i] == "--version" || args[i] == "-V") {
            std::cout << "HanchoTerm " PC_VERSION << std::endl;
            return 0;
        }
    }

    pc::TermClientOptions opt;
    bool sawRemote = false;
    bool forceLocal = false;
    bool anyConnectOption = false;
    for (size_t i = 1; i < args.size(); ++i) {
        const std::string& a = args[i];
        auto next = [&](std::string& out) {
            if (i + 1 < args.size()) {
                out = args[++i];
                return true;
            }
            return false;
        };
        if (a == "--host") {
            std::string v;
            if (next(v)) { opt.host = v; sawRemote = true; anyConnectOption = true; }
        } else if (a.rfind("--host=", 0) == 0) {
            opt.host = a.substr(7);
            sawRemote = true;
            anyConnectOption = true;
        } else if (a == "--port") {
            std::string v;
            if (next(v)) { opt.port = std::atoi(v.c_str()); sawRemote = true; anyConnectOption = true; }
        } else if (a.rfind("--port=", 0) == 0) {
            opt.port = std::atoi(a.substr(7).c_str());
            sawRemote = true;
            anyConnectOption = true;
        } else if (a == "--token") {
            std::string v;
            if (next(v)) opt.token = v;
        } else if (a.rfind("--token=", 0) == 0) {
            opt.token = a.substr(8);
        } else if (a == "--shell") {
            std::string v;
            if (next(v)) { opt.target = v; anyConnectOption = true; }
        } else if (a.rfind("--shell=", 0) == 0) {
            opt.target = a.substr(8);
            anyConnectOption = true;
        } else if (a == "--target") {
            std::string v;
            if (next(v)) { opt.target = v; anyConnectOption = true; }
        } else if (a.rfind("--target=", 0) == 0) {
            opt.target = a.substr(9);
            anyConnectOption = true;
        } else if (a == "--attach") {
            std::string v;
            if (next(v)) { opt.target = "attach:" + v; anyConnectOption = true; }
        } else if (a.rfind("--attach=", 0) == 0) {
            opt.target = "attach:" + a.substr(9);
            anyConnectOption = true;
        } else if (a == "--local") {
            forceLocal = true;
            anyConnectOption = true;
        }
    }
    opt.local = forceLocal || !sawRemote;
    opt.autoConnect = anyConnectOption;
    for (size_t i = 1; i < args.size(); ++i) {
        if (args[i] == "--no-connect") {
            opt.autoConnect = false;
            opt.autoAttach = false;
        } else if (args[i] == "--no-auto-attach") {
            opt.autoAttach = false;
        }
    }
    if (opt.autoConnect && opt.target.empty()) opt.target = "powershell";
    if (opt.port > 0 && opt.host.empty()) opt.host = "127.0.0.1";
    return pc::RunTerminalClient(opt);
}
