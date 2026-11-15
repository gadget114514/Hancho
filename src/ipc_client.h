#pragma once

#include <string>

namespace pc {

// Client helper: auto-starts the GUI Dashboard if not running, sends request, returns JSON response
//
// timeoutMs bounds how long the client waits for the answer (0 = wait forever).
// When the wait times out, outTimedOut is set to true so the caller can tell a
// timeout apart from a connection failure and must not repeat the request.
bool SendIpcRequest(const std::string& reqJson, std::string& outRespJson, std::string& outError,
                     bool autoStartGui = true, int timeoutMs = 0, bool* outTimedOut = nullptr);

} // namespace pc
