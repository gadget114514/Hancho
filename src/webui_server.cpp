#include "webui_server.h"

#include "webui_assets.h"
#include "common.h"
#include "nlohmann/json.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <random>
#include <iomanip>
#include <algorithm>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace pc {

namespace {

std::string GenerateSecureToken() {
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::ostringstream oss;
    oss << std::hex << std::setfill('0')
        << std::setw(16) << gen()
        << std::setw(16) << gen();
    return oss.str();
}

std::string GetHeaderValue(const std::string& headers, const std::string& key) {
    std::string lowerHeaders = headers;
    for (char& c : lowerHeaders) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    std::string lowerKey = key;
    for (char& c : lowerKey) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (!lowerKey.empty() && lowerKey.back() != ':') lowerKey += ':';

    size_t pos = lowerHeaders.find(lowerKey);
    if (pos == std::string::npos) return "";

    size_t valStart = pos + lowerKey.size();
    size_t valEnd = headers.find("\r\n", valStart);
    std::string val = headers.substr(valStart, valEnd == std::string::npos ? std::string::npos : valEnd - valStart);
    val.erase(0, val.find_first_not_of(" \t"));
    val.erase(val.find_last_not_of(" \t\r\n") + 1);
    return val;
}

std::string ExtractQueryParam(const std::string& uri, const std::string& paramName) {
    size_t q = uri.find('?');
    if (q == std::string::npos) return "";
    std::string query = uri.substr(q + 1);

    std::string search = paramName + "=";
    size_t p = query.find(search);
    if (p == std::string::npos) return "";

    size_t valStart = p + search.size();
    size_t valEnd = query.find('&', valStart);
    return query.substr(valStart, valEnd == std::string::npos ? std::string::npos : valEnd - valStart);
}

std::string ExtractCookieParam(const std::string& cookieHeader, const std::string& paramName) {
    std::string search = paramName + "=";
    size_t p = cookieHeader.find(search);
    if (p == std::string::npos) return "";

    size_t valStart = p + search.size();
    size_t valEnd = cookieHeader.find(';', valStart);
    std::string val = cookieHeader.substr(valStart, valEnd == std::string::npos ? std::string::npos : valEnd - valStart);
    val.erase(0, val.find_first_not_of(" \t"));
    val.erase(val.find_last_not_of(" \t") + 1);
    return val;
}

bool IsValidHostHeader(const std::string& hostVal) {
    if (hostVal.empty()) return true; // allow if absent on some simple clients
    std::string h = hostVal;
    size_t colon = h.find(':');
    if (colon != std::string::npos) h = h.substr(0, colon);
    for (char& c : h) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return (h == "localhost" || h == "127.0.0.1" || h == "[::1]" || h == "::1");
}

bool IsValidOriginHeader(const std::string& originVal) {
    if (originVal.empty() || originVal == "null") return true;
    std::string o = originVal;
    for (char& c : o) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (o.rfind("http://localhost:", 0) == 0 || o == "http://localhost") return true;
    if (o.rfind("http://127.0.0.1:", 0) == 0 || o == "http://127.0.0.1") return true;
    if (o.rfind("http://[::1]:", 0) == 0 || o == "http://[::1]") return true;
    if (o.rfind("https://localhost:", 0) == 0 || o == "https://localhost") return true;
    if (o.rfind("https://127.0.0.1:", 0) == 0 || o == "https://127.0.0.1") return true;
    return false;
}

void RestrictFilePermissions(const std::string& path) {
#ifndef _WIN32
    chmod(path.c_str(), S_IRUSR | S_IWUSR);
#else
    (void)path;
#endif
}

} // namespace

WebUiServer::WebUiServer(IpcHandler ipcHandler)
    : ipcHandler_(std::move(ipcHandler)) {}

WebUiServer::~WebUiServer() {
    Stop();
}

bool WebUiServer::Start(uint16_t port, const std::string& token,
                       const std::string& webuiDir, std::string* outError) {
    if (IsRunning()) {
        if (outError) *outError = "WebUiServer is already running";
        return false;
    }

    const char* disabled = std::getenv("HANCHO_WEBUI_DISABLE");
    if (disabled && std::string(disabled) == "1") {
        return true;
    }

    uint16_t listenPort = port;
    if (listenPort == 0) {
        const char* envPort = std::getenv("HANCHO_WEBUI_PORT");
        if (envPort && *envPort) {
            listenPort = static_cast<uint16_t>(std::atoi(envPort));
        }
    }
    if (listenPort == 0) {
        listenPort = 9138; // Default WebUI port
    }
    port_ = listenPort;

    std::string sessionToken = token;
    if (sessionToken.empty()) {
        const char* envToken = std::getenv("HANCHO_TOKEN");
        if (envToken && *envToken) {
            sessionToken = envToken;
        } else {
            sessionToken = GenerateSecureToken();
        }
    }
    token_ = sessionToken;

    if (!webuiDir.empty()) {
        webuiDir_ = webuiDir;
    } else {
        const char* envDir = std::getenv("HANCHO_WEBUI_DIR");
        if (envDir && *envDir) webuiDir_ = envDir;
    }

    // Write token and port files into data directory
    std::wstring dataDir = GetEffectiveDataDirectoryWide();
    tokenFilePath_ = WideToUtf8(PathJoinWide(dataDir, L".webui_token"));
    portFilePath_ = WideToUtf8(PathJoinWide(dataDir, L".webui_port"));

    {
        std::ofstream tokenFile(tokenFilePath_, std::ios::trunc | std::ios::binary);
        if (tokenFile) {
            tokenFile << token_ << "\n";
            tokenFile.close();
            RestrictFilePermissions(tokenFilePath_);
        }
        std::ofstream portFile(portFilePath_, std::ios::trunc | std::ios::binary);
        if (portFile) {
            portFile << port_ << "\n";
            portFile.close();
            RestrictFilePermissions(portFilePath_);
        }
    }

    auto listener = std::make_unique<remote::RemoteListener>();

    // Configure HTTP request handler
    listener->SetHttpHandler([this](const std::string& method,
                                    const std::string& uri,
                                    const std::string& headers,
                                    const std::string& body,
                                    const std::string& peerAddress,
                                    std::string& outResponse) {
        return HandleHttpRequest(method, uri, headers, body, peerAddress, outResponse);
    });

    // Configure WebSocket / IPC handler
    listener->SetHandler([this](const std::string& requestJson, const std::string& peer) {
        (void)peer;
        if (ipcHandler_) {
            return ipcHandler_(requestJson);
        }
        return std::string("{\"ok\":false,\"error\":\"no IPC handler\"}");
    });

    std::string err;
    if (!listener->Start("127.0.0.1", port_, token_, err)) {
        if (outError) *outError = err;
        Stop();
        return false;
    }

    listener_ = std::move(listener);
    WriteAuditLog("WEBUI_LISTEN", GetUrl());
    return true;
}

void WebUiServer::Stop() {
    if (listener_) {
        listener_->Stop();
        listener_.reset();
    }
    if (!tokenFilePath_.empty()) {
        std::remove(tokenFilePath_.c_str());
        tokenFilePath_.clear();
    }
    if (!portFilePath_.empty()) {
        std::remove(portFilePath_.c_str());
        portFilePath_.clear();
    }
    port_ = 0;
}

bool WebUiServer::IsRunning() const {
    return listener_ && listener_->Running();
}

std::string WebUiServer::GetUrl() const {
    if (port_ == 0) return "";
    std::string url = "http://127.0.0.1:" + std::to_string(port_) + "/";
    if (!token_.empty()) {
        url += "?token=" + token_;
    }
    return url;
}

bool WebUiServer::HandleHttpRequest(const std::string& method,
                                    const std::string& uri,
                                    const std::string& headers,
                                    const std::string& body,
                                    const std::string& peerAddress,
                                    std::string& outResponse) {
    (void)body;
    (void)peerAddress;

    // 1. Host header verification (DNS Rebinding prevention)
    std::string hostVal = GetHeaderValue(headers, "Host");
    if (!IsValidHostHeader(hostVal)) {
        outResponse = "HTTP/1.1 403 Forbidden\r\n"
                      "Content-Type: text/plain; charset=utf-8\r\n"
                      "Connection: close\r\n"
                      "Content-Length: 23\r\n\r\n"
                      "Forbidden: invalid Host\n";
        return true;
    }

    // 2. Origin header verification (CSRF prevention)
    std::string originVal = GetHeaderValue(headers, "Origin");
    if (!IsValidOriginHeader(originVal)) {
        outResponse = "HTTP/1.1 403 Forbidden\r\n"
                      "Content-Type: text/plain; charset=utf-8\r\n"
                      "Connection: close\r\n"
                      "Content-Length: 25\r\n\r\n"
                      "Forbidden: invalid Origin\n";
        return true;
    }

    // Parse path
    std::string path = uri;
    size_t q = path.find('?');
    if (q != std::string::npos) path = path.substr(0, q);
    if (path.empty() || path == "/") path = "/index.html";

    // 3. Token verification
    std::string tokenParam = ExtractQueryParam(uri, "token");
    std::string authHeader = GetHeaderValue(headers, "Authorization");
    std::string bearerToken;
    if (authHeader.rfind("Bearer ", 0) == 0 || authHeader.rfind("bearer ", 0) == 0) {
        bearerToken = authHeader.substr(7);
        bearerToken.erase(0, bearerToken.find_first_not_of(" \t"));
        bearerToken.erase(bearerToken.find_last_not_of(" \t\r\n") + 1);
    }
    std::string cookieHeader = GetHeaderValue(headers, "Cookie");
    std::string cookieToken = ExtractCookieParam(cookieHeader, "hancho_token");

    bool authenticated = false;
    if (token_.empty()) {
        authenticated = true;
    } else {
        if (!tokenParam.empty() && tokenParam == token_) authenticated = true;
        else if (!bearerToken.empty() && bearerToken == token_) authenticated = true;
        else if (!cookieToken.empty() && cookieToken == token_) authenticated = true;
    }

    // Handle /api/ endpoints
    if (path.rfind("/api/", 0) == 0) {
        if (!authenticated) {
            outResponse = "HTTP/1.1 401 Unauthorized\r\n"
                          "Content-Type: application/json; charset=utf-8\r\n"
                          "Connection: close\r\n"
                          "Content-Length: 43\r\n\r\n"
                          "{\"ok\":false,\"error\":\"authentication required\"}";
            return true;
        }

        std::string jsonResp;
        if (path == "/api/jobs" || path == "/api/taskqueue") {
            if (ipcHandler_) {
                jsonResp = ipcHandler_("{\"action\":\"taskqueue_list\"}");
            } else {
                jsonResp = "{\"ok\":false,\"error\":\"no IPC handler\"}";
            }
        } else if (path == "/api/status" || path == "/api/info") {
            nlohmann::json info;
            info["ok"] = true;
            info["version"] = PC_VERSION;
            info["port"] = port_;
            info["headless"] = true;
            jsonResp = info.dump();
        } else {
            jsonResp = "{\"ok\":false,\"error\":\"not found\"}";
        }

        outResponse = "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json; charset=utf-8\r\n"
                      "Connection: close\r\n"
                      "Content-Length: " + std::to_string(jsonResp.size()) + "\r\n\r\n" +
                      jsonResp;
        return true;
    }

    // For the main page (/ or /index.html), require authentication
    if (path == "/index.html") {
        if (!authenticated) {
            std::string authBody = "<!DOCTYPE html><html><head><meta charset=\"UTF-8\"><title>Hancho - Unauthorized</title></head>"
                                   "<body style=\"font-family:sans-serif;background:#121217;color:#f0f0f6;display:flex;align-items:center;justify-content:center;height:90vh;\">"
                                   "<div style=\"text-align:center;\"><h2>401 Unauthorized</h2><p>Authentication token required. Launch using <code>hancho-cli web</code> or pass <code>?token=...</code></p></div></body></html>";
            outResponse = "HTTP/1.1 401 Unauthorized\r\n"
                          "Content-Type: text/html; charset=utf-8\r\n"
                          "Connection: close\r\n"
                          "Content-Length: " + std::to_string(authBody.size()) + "\r\n\r\n" +
                          authBody;
            return true;
        }
    }

    // 4. Serve static assets
    std::string content, mimeType;
    if (!GetWebUiAsset(path, webuiDir_, content, mimeType)) {
        outResponse = "HTTP/1.1 404 Not Found\r\n"
                      "Content-Type: text/plain; charset=utf-8\r\n"
                      "Connection: close\r\n"
                      "Content-Length: 9\r\n\r\n"
                      "Not Found";
        return true;
    }

    std::ostringstream ss;
    ss << "HTTP/1.1 200 OK\r\n"
       << "Content-Type: " << mimeType << "\r\n"
       << "Content-Length: " << content.size() << "\r\n"
       << "Cache-Control: no-cache\r\n"
       << "X-Content-Type-Options: nosniff\r\n";

    // Set cookie if authenticated via query param on index page
    if (path == "/index.html" && !token_.empty() && tokenParam == token_) {
        ss << "Set-Cookie: hancho_token=" << token_ << "; Path=/; SameSite=Strict; HttpOnly\r\n";
    }

    ss << "Connection: close\r\n\r\n"
       << content;

    outResponse = ss.str();
    return true;
}

} // namespace pc
