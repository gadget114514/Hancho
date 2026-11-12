#pragma once

// Declarations for WebSocket multiplexing client and connection pool.
// Authorised edits: tests/

#include "remote_client.h"
#include "nlohmann/json.hpp"

#include <functional>
#include <memory>
#include <string>

namespace pc {
namespace remote {

class WebSocketClient {
public:
    using MessageCallback = std::function<void(const std::string& message)>;
    using StreamEventCallback = std::function<void(const std::string& subId, uint64_t seq,
                                                   const std::string& line, const nlohmann::json& doc)>;

    WebSocketClient();
    ~WebSocketClient();

    bool Connect(const std::string& address, const std::string& token = "",
                 int timeoutMs = 15000, std::string* outError = nullptr);
    bool IsConnected() const;
    void Close();
    bool SendRequest(const std::string& requestJson, std::string& outResponse,
                     std::string& outError, int timeoutMs = 15000);
    void SetMessageCallback(MessageCallback cb);
    void SetStreamCallback(StreamEventCallback cb);
    std::string Endpoint() const;

    bool Subscribe(const std::string& stream, int id, uint64_t sinceSeq,
                   std::string& outSubId, std::string& outError, int timeoutMs = 15000);
    bool SubscribeFile(const std::string& filePath, uint64_t sinceSeq,
                       std::string& outSubId, std::string& outError, int timeoutMs = 15000);
    bool Unsubscribe(const std::string& subId, std::string& outError, int timeoutMs = 15000);

private:
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

bool WebSocketMultiplexRequest(const std::string& address, const std::string& token,
                               const std::string& requestJson, std::string& outResponse,
                               std::string& outError, int timeoutMs = 15000);

void ResetWebSocketPool();

} // namespace remote
} // namespace pc
