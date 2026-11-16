#pragma once

#include <string>

namespace pc {

// Suspends the machine (sleep by default, hibernate when requested).
// Returns false and fills outError when the platform refused the request.
bool SuspendSystem(bool hibernate, std::string& outError);

// Powers the machine off. Returns false and fills outError when the platform
// refused the request (usually a missing shutdown privilege).
bool ShutdownSystem(std::string& outError);

// True when this process can enable the privileges that suspend and shutdown need.
// On Windows this enables SE_SHUTDOWN_NAME (a harmless no-op for the later call);
// refusals explain why, so Settings can show the state before a job tries to use it.
bool HasPowerPrivilege(std::string& outError);

} // namespace pc
