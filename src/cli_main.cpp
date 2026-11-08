#include "cli.h"
#include "common.h"
#include <windows.h>
#include <shellapi.h>
#include <string>
#include <vector>

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

    return pc::RunCli(args);
}
