#include "llm_config.h"

#include <algorithm>
#include <cctype>
#include <functional>

namespace pc {
namespace llm {

namespace {

std::string Field(const json& j, const char* key, const std::string& def = "") {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    if (it->is_string()) return it->get<std::string>();
    return def;
}

int IntField(const json& j, const char* key, int def = 0) {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    if (it == j.end() || it->is_null() || !it->is_number_integer()) return def;
    return it->get<int>();
}

bool BoolField(const json& j, const char* key, bool def = false) {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    if (it == j.end() || it->is_null() || !it->is_boolean()) return def;
    return it->get<bool>();
}

Entry* FindEntry(Config& cfg, const std::string& list,
                 const std::string& engine, const std::string& model) {
    auto& rows = cfg.List(list);
    for (auto& e : rows) {
        if (e.engine == engine && e.model == model) return &e;
    }
    return nullptr;
}

json ResultError(const std::string& err) {
    Result r;
    r.error = err;
    return json{{"ok", false}, {"error", err}};
}

} // namespace

bool IsValidList(const std::string& list) {
    return list == kWorker || list == kConsult || list == kSupervisor;
}

json Entry::ToJson() const {
    return json{{"engine", engine},
                {"provider", provider},
                {"model", model},
                {"rank", rank},
                {"enabled", enabled},
                {"note", note},
                {"status", status},
                {"approvedVia", approvedVia},
                {"approvedAt", approvedAt},
                {"userSaid", userSaid}};
}

Entry Entry::FromJson(const json& j) {
    Entry e;
    e.engine = Field(j, "engine");
    e.provider = Field(j, "provider");
    e.model = Field(j, "model");
    e.rank = IntField(j, "rank");
    e.enabled = BoolField(j, "enabled", true);
    e.note = Field(j, "note");
    e.status = Field(j, "status", "proposed");
    e.approvedVia = Field(j, "approvedVia");
    e.approvedAt = Field(j, "approvedAt");
    e.userSaid = Field(j, "userSaid");
    return e;
}

json HistoryEntry::ToJson() const {
    return json{{"at", at}, {"by", by}, {"list", list},
                {"action", action}, {"entry", entry}, {"reason", reason}};
}

HistoryEntry HistoryEntry::FromJson(const json& j) {
    HistoryEntry h;
    h.at = Field(j, "at");
    h.by = Field(j, "by");
    h.list = Field(j, "list");
    h.action = Field(j, "action");
    h.entry = j.is_object() && j.contains("entry") ? j["entry"] : json::object();
    h.reason = Field(j, "reason");
    return h;
}

std::vector<Entry>& Config::List(const std::string& name) {
    if (name == kConsult) return consult;
    if (name == kSupervisor) return supervisor;
    return worker;
}

const std::vector<Entry>& Config::List(const std::string& name) const {
    if (name == kConsult) return consult;
    if (name == kSupervisor) return supervisor;
    return worker;
}

json Config::ToJson() const {
    json rows = json::array();
    for (const auto& e : worker) rows.push_back(e.ToJson());
    json cons = json::array();
    for (const auto& e : consult) cons.push_back(e.ToJson());
    json sup = json::array();
    for (const auto& e : supervisor) sup.push_back(e.ToJson());
    json hist = json::array();
    for (const auto& h : history) hist.push_back(h.ToJson());
    return json{{kWorker, rows}, {kConsult, cons}, {kSupervisor, sup}, {"history", hist}};
}

std::wstring DefaultPath() {
    return PathJoinWide(GetEffectiveDataDirectoryWide(), L"llms.json");
}

bool Load(const std::wstring& path, Config& out, std::string& err) {
    err.clear();
    out = Config();
    const std::string text = ReadFileUtf8Wide(path);
    if (text.empty()) return true;
    json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        err = "llms.json must be a JSON object";
        return false;
    }
    for (const char* name : {kWorker, kConsult, kSupervisor}) {
        auto it = root.find(name);
        if (it == root.end()) continue;
        if (!it->is_array()) {
            err = std::string("'") + name + "' must be an array";
            return false;
        }
        auto& rows = out.List(name);
        for (const auto& row : *it) rows.push_back(Entry::FromJson(row));
    }
    auto hit = root.find("history");
    if (hit != root.end() && hit->is_array()) {
        for (const auto& h : *hit) out.history.push_back(HistoryEntry::FromJson(h));
    }
    return true;
}

bool Save(const std::wstring& path, const Config& cfg, std::string& err) {
    err.clear();
    try {
        WriteFileAtomicWide(path, cfg.ToJson().dump(2));
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
    return true;
}

std::string ProviderFromModel(const std::string& model) {
    size_t slash = model.find('/');
    if (slash == std::string::npos || slash == 0) return "";
    return model.substr(0, slash);
}

std::vector<Entry> Usable(const Config& cfg, const std::string& list) {
    std::vector<Entry> rows;
    if (!IsValidList(list)) return rows;
    for (const auto& e : cfg.List(list)) {
        if (e.Usable()) rows.push_back(e);
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Entry& a, const Entry& b) {
        return a.rank < b.rank;
    });
    return rows;
}

bool HasUsable(const Config& cfg, const std::string& list,
               const std::string& engine, const std::string& model) {
    for (const auto& e : Usable(cfg, list)) {
        if (e.engine == engine && e.model == model) return true;
    }
    return false;
}

Result List(const std::wstring& path, const std::string& list) {
    Config cfg;
    std::string err;
    if (!Load(path, cfg, err)) return {false, err, json::object()};
    json out = json::object();
    auto emit = [&](const char* name) {
        json rows = json::array();
        for (const auto& e : cfg.List(name)) {
            json o = e.ToJson();
            o["usable"] = e.Usable();
            rows.push_back(o);
        }
        out[name] = rows;
    };
    if (list.empty()) {
        emit(kWorker);
        emit(kConsult);
        emit(kSupervisor);
    } else {
        if (!IsValidList(list)) return {false, "invalid list: " + list, json::object()};
        emit(list.c_str());
    }
    json pending = json::array();
    for (const auto& name : {kWorker, kConsult, kSupervisor}) {
        for (const auto& e : cfg.List(name)) {
            if (e.status != "approved") {
                pending.push_back(json{{"list", name}, {"engine", e.engine}, {"model", e.model}});
            }
        }
    }
    out["proposed"] = pending;
    Result r;
    r.ok = true;
    r.data = out;
    return r;
}

static Result Mutate(const std::wstring& path, const std::string& list,
                     const std::string& action, const std::string& by,
                     const std::string& reason,
                     std::function<bool(Config&, json&)> change) {
    if (!IsValidList(list)) return {false, "invalid list: " + list, json::object()};
    Config cfg;
    std::string err;
    if (!Load(path, cfg, err)) return {false, err, json::object()};
    json detail = json::object();
    if (!change(cfg, detail)) return {false, "entry not found", json::object()};
    HistoryEntry h;
    h.at = GetCurrentTimestampStr();
    h.by = by.empty() ? "cli" : by;
    h.list = list;
    h.action = action;
    h.entry = detail;
    h.reason = reason;
    cfg.history.push_back(h);
    if (!Save(path, cfg, err)) return {false, err, json::object()};
    Result r;
    r.ok = true;
    r.data = json{{"ok", true}, {"list", list}, {"entry", detail}};
    return r;
}

Result Add(const std::wstring& path, const std::string& list,
           const std::string& engine, const std::string& model,
           const std::string& provider, int rank, bool hasRank,
           const std::string& note, const std::string& by, const std::string& reason) {
    if (engine.empty() || model.empty()) return {false, "--engine and --model are required", json::object()};
    return Mutate(path, list, "add", by, reason, [&](Config& cfg, json& detail) {
        if (FindEntry(cfg, list, engine, model)) return false;
        Entry e;
        e.engine = engine;
        e.model = model;
        e.provider = provider.empty() ? ProviderFromModel(model) : provider;
        e.note = note;
        e.rank = hasRank ? rank : static_cast<int>(cfg.List(list).size()) + 1;
        if (by == "dashboard") {
            e.status = "approved";
            e.approvedVia = "dashboard";
            e.approvedAt = GetCurrentTimestampStr();
        } else {
            e.status = "proposed";
        }
        cfg.List(list).push_back(e);
        detail = e.ToJson();
        return true;
    });
}

Result Remove(const std::wstring& path, const std::string& list,
              const std::string& engine, const std::string& model,
              const std::string& by, const std::string& reason) {
    return Mutate(path, list, "remove", by, reason, [&](Config& cfg, json& detail) {
        auto& rows = cfg.List(list);
        auto it = std::find_if(rows.begin(), rows.end(), [&](const Entry& e) {
            return e.engine == engine && e.model == model;
        });
        if (it == rows.end()) return false;
        detail = it->ToJson();
        rows.erase(it);
        return true;
    });
}

Result SetEnabled(const std::wstring& path, const std::string& list,
                  const std::string& engine, const std::string& model,
                  bool enabled, const std::string& by, const std::string& reason) {
    return Mutate(path, list, enabled ? "enable" : "disable", by, reason,
                  [&](Config& cfg, json& detail) {
        Entry* e = FindEntry(cfg, list, engine, model);
        if (!e) return false;
        e->enabled = enabled;
        detail = e->ToJson();
        return true;
    });
}

Result Approve(const std::wstring& path, const std::string& list,
               const std::string& engine, const std::string& model,
               const std::string& userSaid, const std::string& by) {
    if (userSaid.empty()) return {false, "--user-said must quote the user's words", json::object()};
    return Mutate(path, list, "approve", by, userSaid, [&](Config& cfg, json& detail) {
        Entry* e = FindEntry(cfg, list, engine, model);
        if (!e) return false;
        e->status = "approved";
        e->approvedVia = "chat";
        e->approvedAt = GetCurrentTimestampStr();
        e->userSaid = userSaid;
        detail = e->ToJson();
        return true;
    });
}

Result ApproveViaDashboard(const std::wstring& path, const std::string& list,
                           const std::string& engine, const std::string& model,
                           const std::string& by) {
    return Mutate(path, list, "approve", by.empty() ? "dashboard" : by,
                  "approved from dashboard", [&](Config& cfg, json& detail) {
        Entry* e = FindEntry(cfg, list, engine, model);
        if (!e) return false;
        e->status = "approved";
        e->approvedVia = "dashboard";
        e->approvedAt = GetCurrentTimestampStr();
        detail = e->ToJson();
        return true;
    });
}

Result SetRank(const std::wstring& path, const std::string& list,
               const std::string& engine, const std::string& model,
               int rank, const std::string& by, const std::string& reason) {
    return Mutate(path, list, "rank", by, reason, [&](Config& cfg, json& detail) {
        Entry* e = FindEntry(cfg, list, engine, model);
        if (!e) return false;
        e->rank = rank;
        detail = e->ToJson();
        return true;
    });
}

Result Edit(const std::wstring& path, const std::string& list,
            const std::string& engine, const std::string& model,
            const std::string& newEngine, const std::string& newModel,
            const std::string& newProvider,
            const std::string& by, const std::string& reason) {
    return Mutate(path, list, "edit", by, reason, [&](Config& cfg, json& detail) {
        Entry* e = FindEntry(cfg, list, engine, model);
        if (!e) return false;
        if (!newEngine.empty()) e->engine = newEngine;
        if (!newModel.empty()) {
            e->model = newModel;
            if (newProvider.empty()) e->provider = ProviderFromModel(newModel);
        }
        if (!newProvider.empty()) e->provider = newProvider;
        if (e->approvedVia == "dashboard" || e->approvedVia == "chat" || e->status == "approved") {
            e->status = "proposed";
            e->approvedVia.clear();
            e->approvedAt.clear();
            e->userSaid.clear();
        }
        detail = e->ToJson();
        return true;
    });
}

} // namespace llm
} // namespace pc
