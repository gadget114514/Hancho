#pragma once

#include <string>
#include <vector>

namespace pc {

// Run a console CLI command. args[0] is the executable name, args[1] the subcommand.
// Outputs human-friendly text by default, or raw JSON when --json / -j is present.
int RunCli(const std::vector<std::string>& args);

// Print the console edition (hancho-cli.exe) usage.
void PrintCliUsage();

} // namespace pc
