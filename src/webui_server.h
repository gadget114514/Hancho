#pragma once

#include "remote_client.h"
#include <string>
#include <memory>
#include <functional>
#include <cstdint>

namespace pc {

class WebUiServer {
public:
    using IpcHandler = std::function<std::string(const std::string& requestJson)>;

    explicit WebUiServer(IpcHandler ipcHandler);
    ~WebUiServer();

    // Starts WebUI HTTP + WebSocket server on localhost (127.0.0.1).
    // If port is 0, uses config/env or default 9138.
    // If token is empty, generates random token or uses HANCHO_TOKEN.
    bool Start(uint16_t port = 0, const std::string& token = "",
               const std::string& webuiDir = "", std::string* outError = nullptr);
    void Stop();

    bool IsRunning() const;
    uint16_t GetPort() const { return port_; }
    const std::string& GetToken() const { return token_; }
    std::string GetUrl() const;

private:
    bool HandleHttpRequest(const std::string& method,
                           const std::string& uri,
                           const std::string& headers,
                           const std::string& body,
                           const std::string& peerAddress,
                           std::string& outResponse);

    IpcHandler ipcHandler_;
    std::unique_ptr<remote::RemoteListener> listener_;
    uint16_t port_ = 0;
    std::string token_;
    std::string webuiDir_;
    std::string tokenFilePath_;
    std::string portFilePath_;
};

} // namespace pc
