#pragma once

#include <string>

namespace htest {

struct ProcessResult {
    int exitCode = -1;
    bool timedOut = false;
    std::string stdoutText;
    std::string stderrText;
    std::string error;
};

std::string ExecutableDir();
std::string ExpandExeDir(const std::string& text);

ProcessResult RunProcess(const std::string& command, const std::string& workingDir,
                         int timeoutSec);

} // namespace htest
