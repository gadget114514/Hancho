#include "cli.h"

#ifndef _WIN32

#include <string>
#include <vector>

int main(int argc, char** argv) {
    std::vector<std::string> args;
    args.reserve(static_cast<size_t>(argc));
    for (int i = 0; i < argc; ++i) args.emplace_back(argv[i]);
    return pc::RunCli(args);
}

#endif // !_WIN32
