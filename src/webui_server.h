#pragma once

#include "remote_client.h"
#include <string>
#include <memory>
#include <functional>
#include <cstdint>
#include <set>

namespace pc {

class WebUiServer {
public:
    using IpcHandler = std::function<std::string(const std::string& requestJson)>;

    explicit WebUiServer(IpcHandler ipcHandler);
    ~WebUiServer();

    // Starts the WebUI HTTP + WebSocket server. The bind address, exposure scope
    // and allowed hosts/origins are read from <data-dir>/config.json ("webui")
    // and can be overridden with HANCHO_WEBUI_* environment variables.
    // If port is 0, uses config/env or default 9138.
    // If token is empty, generates random token or uses HANCHO_TOKEN.
    bool Start(uint16_t port = 0, const std::string& token = "",
               const std::string& webuiDir = "", std::string* outError = nullptr);
    // Stops the server. With waitForHandlers=false the listening socket is closed
    // right away and in-flight WebSocket handlers are detached instead of waited
    // for, so the caller is never blocked by a handler that takes long. The
    // detached handlers keep the shared runtime state alive; the owner has to
    // keep this object alive until they are done (see IpcServer, which parks
    // retired servers and waits for them when the core shuts down).
    void Stop(bool waitForHandlers = true);

    bool IsRunning() const;
    uint16_t GetPort() const { return port_; }
    const std::string& GetToken() const { return token_; }
    std::string GetUrl() const;

private:
    // Runtime data handed to the HTTP and WebSocket handlers. A handler may
    // still be running after Stop() returned, so the handlers must not depend
    // on the lifetime of this object.
    struct Runtime;

    static bool HandleHttpRequest(const Runtime& runtime, const std::string& method,
                                  const std::string& uri, const std::string& headers,
                                  const std::string& body, const std::string& peerAddress,
                                  std::string& outResponse);

    static bool IsAllowedHostHeader(const Runtime& runtime, const std::string& hostVal);
    static bool IsAllowedOriginHeader(const Runtime& runtime, const std::string& originVal);

    IpcHandler ipcHandler_;
    std::unique_ptr<remote::RemoteListener> listener_;
    std::shared_ptr<Runtime> runtime_;
    uint16_t port_ = 0;
    std::string token_;
    std::string webuiDir_;
    std::string bindHost_ = "127.0.0.1";
    bool allowRemote_ = false;
    std::set<std::string> allowedHosts_;
    std::set<std::string> allowedOrigins_;
    std::string tokenFilePath_;
    std::string portFilePath_;
};

} // namespace pc
