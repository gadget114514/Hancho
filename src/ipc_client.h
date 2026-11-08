#pragma once

#include <string>

namespace pc {

// Client helper: auto-starts the GUI Dashboard if not running, sends request, returns JSON response
bool SendIpcRequest(const std::string& reqJson, std::string& outRespJson, std::string& outError, bool autoStartGui = true);

} // namespace pc
