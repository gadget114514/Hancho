#pragma once

// Worker permission list (<effective data dir>/permits.json), doc/obsoleted/HanchoExpectations.md 8.8.
// Pure logic only: no GUI, no IPC, no process launching. Callers pass a path so this
// module stays trivially testable, mirroring src/llm_config.h (091-3).

#include "common.h"

#include "nlohmann/json.hpp"

#include <string>
#include <vector>

namespace pc {
namespace permit {

using nlohmann::json;

constexpr const char* kGlobal = "global";
constexpr const char* kProposed = "proposed";
constexpr const char* kApproved = "approved";
constexpr const char* kRejected = "rejected";
constexpr const char* kRevoked = "revoked";

bool IsValidKind(const std::string& kind);

// Stable id used by the CLI / MCP / dashboard. Derived from the declaration itself so
// an auto-request from a failed job can find a proposal made by a different caller.
std::string MakeId(const std::string& kind, const std::string& target, const std::string& scope);

struct Entry {
    std::string id;
    std::string kind;
    std::string target;
    std::vector<std::string> engines;
    std::string scope = kGlobal;
    std::string status = kProposed;
    std::string approvedVia;
    std::string approvedAt;
    std::string userSaid;
    std::string note;

    json ToJson() const;
    static Entry FromJson(const json& j);
    bool Usable() const { return status == kApproved; }
    // Empty engines means all engines.
    bool CoversEngine(const std::string& engine) const;
};

struct HistoryEntry {
    std::string at;
    std::string by;
    std::string action;
    json entry = json::object();
    std::string reason;

    json ToJson() const;
    static HistoryEntry FromJson(const json& j);
};

struct Config {
    std::vector<Entry> permits;
    std::vector<HistoryEntry> history;

    json ToJson() const;
};

struct Result {
    bool ok = false;
    std::string error;
    json data = json::object();
};

// <effective data dir>/permits.json
std::wstring DefaultPath();

// A missing file loads as an empty config (ok=true).
bool Load(const std::wstring& path, Config& out, std::string& err);
bool Save(const std::wstring& path, const Config& cfg, std::string& err);

// Lists rows plus a synthetic "proposed" array, like llm::List().
Result List(const std::wstring& path, const std::string& scope);

// Mutations. `by` is "cli", "dashboard", an MCP client name or "worker" (auto-request).
// Every mutation appends a history entry with at/by/action/reason.
// Request creates or merges a proposed row. Empty kind/target is rejected.
Result Request(const std::wstring& path, const std::string& kind, const std::string& target,
               const std::vector<std::string>& engines, const std::string& scope,
               const std::string& reason, const std::string& by);
Result Approve(const std::wstring& path, const std::string& id,
               const std::string& userSaid, const std::string& by);
Result ApproveViaDashboard(const std::wstring& path, const std::string& id, const std::string& by);
Result Reject(const std::wstring& path, const std::string& id,
              const std::string& reason, const std::string& by);
Result Revoke(const std::wstring& path, const std::string& id,
              const std::string& reason, const std::string& by);

// True when an approved row covers this kind/target/engine/scope. glob '*' and '?' are
// supported; matching is case-insensitive on Windows. scope "global" matches any scope.
bool IsApproved(const Config& cfg, const std::string& kind, const std::string& target,
                const std::string& engine, const std::string& scope);
std::vector<Entry> Approved(const Config& cfg, const std::string& engine, const std::string& scope);

// Expand %TEMP%, %USERPROFILE% and a leading ~ for use in engine config. The stored
// target keeps the original spelling.
std::string ExpandTarget(const std::string& target);

} // namespace permit
} // namespace pc
