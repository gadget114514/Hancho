#pragma once

#include <string>

namespace pc {

// Returns true when the command is a single command that may be launched.
// Returns false and fills outError when the command chains multiple commands
// (&&, ||, &, newline, powershell ';') or polls for another process
// (a "wait loop"). The error message includes the detected position, split
// candidates and a taskqueue_add example.
bool CheckChainGuard(const std::string& command, std::string& outError);

} // namespace pc
