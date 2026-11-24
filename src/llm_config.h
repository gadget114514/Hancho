#pragma once

// Per-role LLM candidate lists (doc/obsoleted/HanchoExpectations.md 8.3). Pure logic: no GUI,
// no IPC, no process launching. Callers pass a path so this module is trivially testable.

#include "common.h"

#include "nlohmann/json.hpp"

#include <string>
#include <vector>

namespace pc {
namespace llm {

using nlohmann::json;

constexpr const char* kWorker = "worker";
constexpr const char* kConsult = "consult";
constexpr const char* kSupervisor = "supervisor";

bool IsValidList(const std::string& list);

struct Entry {
    std::string engine;
    std::string provider;
    std::string model;
    int rank = 0;
    bool enabled = true;
    std::string note;
    std::string status = "proposed";
    std::string approvedVia;
    std::string approvedAt;
    std::string userSaid;

    json ToJson() const;
    static Entry FromJson(const json& j);
    bool Usable() const { return status == "approved" && enabled; }
};

struct HistoryEntry {
    std::string at;
    std::string by;
    std::string list;
    std::string action;
    json entry = json::object();
    std::string reason;

    json ToJson() const;
    static HistoryEntry FromJson(const json& j);
};

struct Config {
    std::vector<Entry> worker;
    std::vector<Entry> consult;
    std::vector<Entry> supervisor;
    std::vector<HistoryEntry> history;

    std::vector<Entry>& List(const std::string& name);
    const std::vector<Entry>& List(const std::string& name) const;
    json ToJson() const;
};

struct Result {
    bool ok = false;
    std::string error;
    json data = json::object();
};

// <effective data dir>/llms.json
std::wstring DefaultPath();

// A missing file loads as an empty config (ok=true).
bool Load(const std::wstring& path, Config& out, std::string& err);
bool Save(const std::wstring& path, const Config& cfg, std::string& err);

// Queries. list may be empty for List() meaning "all three lists".
Result List(const std::wstring& path, const std::string& list);
// Row order / usable candidates for a role (approved && enabled, sorted by rank).
std::vector<Entry> Usable(const Config& cfg, const std::string& list);
bool HasUsable(const Config& cfg, const std::string& list,
               const std::string& engine, const std::string& model);

// Mutations. `by` is "cli", "dashboard" or an MCP client name; `reason` is recorded in history.
// add: status is approved when by=="dashboard", otherwise proposed.
Result Add(const std::wstring& path, const std::string& list,
           const std::string& engine, const std::string& model,
           const std::string& provider, int rank, bool hasRank,
           const std::string& note, const std::string& by, const std::string& reason);
Result Remove(const std::wstring& path, const std::string& list,
              const std::string& engine, const std::string& model,
              const std::string& by, const std::string& reason);
Result SetEnabled(const std::wstring& path, const std::string& list,
                  const std::string& engine, const std::string& model,
                  bool enabled, const std::string& by, const std::string& reason);
Result Approve(const std::wstring& path, const std::string& list,
               const std::string& engine, const std::string& model,
               const std::string& userSaid, const std::string& by);
// Dashboard approval button: approvedVia is "dashboard" (no chat quote).
Result ApproveViaDashboard(const std::wstring& path, const std::string& list,
                           const std::string& engine, const std::string& model,
                           const std::string& by);
// Sets rank on an existing row (approved rows may be re-ranked without re-approval).
Result SetRank(const std::wstring& path, const std::string& list,
               const std::string& engine, const std::string& model,
               int rank, const std::string& by, const std::string& reason);
// Edits model/provider/engine: the edited row becomes proposed again (needs re-approval).
Result Edit(const std::wstring& path, const std::string& list,
            const std::string& engine, const std::string& model,
            const std::string& newEngine, const std::string& newModel,
            const std::string& newProvider,
            const std::string& by, const std::string& reason);

// "provider/model" -> "provider" (empty when there is no slash).
std::string ProviderFromModel(const std::string& model);

} // namespace llm
} // namespace pc
