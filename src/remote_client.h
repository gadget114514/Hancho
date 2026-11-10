#pragma once

// TCP (WebSocket) transport for the Hancho IPC protocol (doc/design-v3.md 2.5).
//
// The request shape is unchanged: one JSON object in, one JSON object out. The
// listener binds to loopback by default. Loopback clients need no token; a
// connection from any other address is refused unless a token is configured
// and the request carries it. TLS is deliberately not implemented: use an SSH
// tunnel when the traffic has to leave the machine.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "remote_hosts.h"

namespace pc {
namespace remote {

// One request/response over a fresh WebSocket connection.
bool WebSocketRequest(const std::string& address, const std::string& token,
                      const std::string& requestJson, std::string& outResponse,
                      std::string& outError, int timeoutMs = 15000);

// Looks the host up in <data-dir>/hosts.json, resolves its token and calls it.
bool CallHost(const std::string& hostName, const std::string& requestJson,
              std::string& outResponse, std::string& outError, int timeoutMs = 15000);

// Same, with an explicit host entry (its token is still resolved).
bool CallHost(const RemoteHost& host, const std::string& requestJson,
              std::string& outResponse, std::string& outError, int timeoutMs = 15000);

// Local remote listener. Handler receives the request JSON and the peer address.
class RemoteListener {
public:
    using Handler = std::function<std::string(const std::string& requestJson,
                                              const std::string& peerAddress)>;

    RemoteListener();
    ~RemoteListener();

    void SetHandler(Handler handler);

    // bindHost empty or "*" means 0.0.0.0. token empty means "loopback only".
    bool Start(const std::string& bindHost, uint16_t port, const std::string& token,
               std::string& outError);
    void Stop();
    bool Running() const;
    std::string Endpoint() const;

    // Opaque shared state; public so the accept loop in the .cpp can name it.
    struct State;

private:
    std::shared_ptr<State> state_;
    std::thread acceptThread_;
};

} // namespace remote
} // namespace pc
