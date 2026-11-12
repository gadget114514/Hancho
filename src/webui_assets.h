#pragma once

#include <string>

namespace pc {

// Attempts to resolve and return content for requestPath (e.g. "/" or "/app.js").
// If webuiOverrideDir is non-empty and contains the file, it reads from disk.
// Otherwise, it serves from embedded static assets.
// Returns true on success, false on 404 Not Found.
bool GetWebUiAsset(const std::string& requestPath,
                   const std::string& webuiOverrideDir,
                   std::string& outContent,
                   std::string& outMimeType);

std::string GetMimeTypeForPath(const std::string& path);

} // namespace pc
