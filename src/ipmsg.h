#pragma once

// IP Messenger (IPMsg) compatible messaging for the Hancho core.
//
// The node speaks the classic IPMsg protocol (UDP for presence/messages,
// TCP on the same port number for file transfer), so a stock IPMsg client
// on the LAN can see Hancho as a host and exchange messages and files with it.
// The default port is NOT 2425: Hancho uses kIpmsgDefaultPort so it never
// collides with a desktop IPMsg running on the same PC. Point an IPMsg client
// at that port to talk to Hancho.
//
// LLM to LLM messaging: messages carry an optional Hancho extension (after the
// visible text and the file list, behind a second NUL) with from_agent /
// to_agent / reply_to. Stock clients ignore it. A message with no extension
// whose text starts with "@name " is addressed to agent "name". The extension
// also carries "audience" (human | agent), decided by the sending LLM, and
// "hops" (loop guard for LLM-to-LLM reply chains).
//
// Intended for a trusted (secure) LAN only: nothing is encrypted, and packets
// are accepted only from loopback / private / link-local IPv4 addresses
// (optionally narrowed further by allow_from).

#include <atomic>
#include <functional>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "nlohmann/json.hpp"

namespace pc {

constexpr uint16_t kIpmsgDefaultPort = 2426;

struct IpmsgConfig {
    bool enabled = false;
    uint16_t port = kIpmsgDefaultPort;
    std::string bind = "0.0.0.0";
    std::string nickname;          // default: "Hancho@<host>"
    std::string group = "Hancho";
    // Extra entry targets: broadcast addresses or unicast hosts ("a.b.c.d" or
    // "a.b.c.d:port"). 255.255.255.255 is always used.
    std::vector<std::string> hosts;
    // Accepted source networks ("192.168.1.0/24", "10.0.0.5"). Empty = every
    // private / loopback / link-local address. Public addresses are never accepted.
    std::vector<std::string> allowFrom;
    // Directory for received files. Empty = <data-dir>/ipmsg/files.
    std::string downloadDir;
    bool autoDownload = false;
    int64_t maxFileBytes = 1024ll * 1024 * 1024;  // per file, for downloads
    size_t maxMessages = 2000;                     // kept in memory / on disk

    static IpmsgConfig FromJson(const nlohmann::json& j);
    nlohmann::json ToJson() const;
};

class IpmsgNode {
public:
    IpmsgNode();
    ~IpmsgNode();

    bool Start(const IpmsgConfig& cfg, std::string* outError = nullptr);
    void Stop();
    bool IsRunning() const { return running_.load(); }
    IpmsgConfig GetConfig() const;

    // JSON API used by the IPC "ipmsg_*" actions. Every reply has "ok".
    nlohmann::json Status() const;
    nlohmann::json Peers() const;
    nlohmann::json Refresh();
    // to: peer key ("ip:port"), ip, nickname, "user@host", "*" (all peers),
    // or "" / "local" for agents on this node only.
    // audience: "human" (show to a person, never fed to an LLM handler),
    // "agent" (for Hancho-internal / LLM processing) or "" (unspecified; treated
    // like a human-written message). hops counts LLM-generated replies in a chain.
    nlohmann::json Send(const std::string& to, const std::string& text,
                        const std::string& fromAgent, const std::string& toAgent,
                        uint64_t replyTo, const std::vector<std::string>& files,
                        const std::string& audience = std::string(), int hops = 0);
    // One message as JSON ({"ok":false} when not found).
    nlohmann::json Get(uint64_t id) const;
    // Called (outside the node's lock) for every newly received or locally
    // delivered message, with the message JSON. Used by the LLM handler and UI.
    void SetOnMessage(std::function<void(const nlohmann::json&)> cb);
    // Conversation log (<data-dir>/ipmsg/log/ipmsg-YYYY-MM-DD.jsonl), kept apart
    // from the capped message history. Events: recv, send, local, status,
    // file_sent, file_received, plus handler events written through LogEvent.
    // Returns the newest `limit` matching entries, oldest first.
    // since/until: "YYYY-MM-DD", "today" or a duration such as "7d" (default 7d).
    nlohmann::json Log(const std::string& since, const std::string& until,
                       const std::string& query, const std::string& peer,
                       const std::string& agent, const std::string& event,
                       size_t limit) const;
    void LogEvent(const std::string& event, const nlohmann::json& body);
    // Messages with id > sinceId. agent: "" = all; otherwise messages addressed
    // to that agent, plus unaddressed ones when includeUnaddressed.
    nlohmann::json Inbox(uint64_t sinceId, const std::string& agent, bool includeUnaddressed,
                         bool unreadOnly, bool includeOutgoing, size_t limit, bool markRead);
    nlohmann::json Wait(uint64_t sinceId, const std::string& agent, bool includeUnaddressed,
                        int timeoutMs, bool markRead);
    nlohmann::json MarkRead(const std::vector<uint64_t>& ids, bool all);
    nlohmann::json Download(uint64_t messageId, int64_t fileId, const std::string& dir,
                            bool wait, int timeoutMs);
    nlohmann::json Clear();
    // Monotonic counter bumped on every change (dashboard polling).
    uint64_t Version() const { return version_.load(); }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> version_{1};
};

} // namespace pc
