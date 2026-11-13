#pragma once

#include <string>

namespace pc {

// Suspends the machine (sleep by default, hibernate when requested).
// Returns false and fills outError when the platform refused the request.
bool SuspendSystem(bool hibernate, std::string& outError);

} // namespace pc
