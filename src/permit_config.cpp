#include "permit_config.h"

#include <algorithm>
#include <cctype>
#include <functional>

#ifdef _WIN32
#include <windows.h>
#endif

namespace pc {
namespace permit {

namespace {

std::string Field(const json& j, const char* key, const std::string& def = "") {
    if (!j.is_object()) return def;
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return def;
    if (it->is_string()) return it->get<std::string>();
    return def;
}

std::vector<std::string> StringArray(const json& j, const char* key) {
    std::vector<std::string> out;
    if (!j.is_object()) return out;
    auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return out;
    for (const auto& v : *it) {
        if (v.is_string() && !v.get<std::string>().empty()) out.push_back(v.get<std::string>());
    }
    return out;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Simple whole-value glob: '*' (any sequence) and '?' (one char). No escaping; paths/commands
// are normalized backslashes to slashes before matching.
bool GlobMatch(const std::string& patternIn, const std::string& valueIn) {
    const std::string p = Lower(patternIn);
    const std::string v = Lower(valueIn);
    size_t pi = 0, vi = 0, star = std::string::npos, match = 0;
    while (vi < v.size()) {
        if (pi < p.size() && (p[pi] == '?' || p[pi] == v[vi])) {
            pi++; vi++;
        } else if (pi < p.size() && p[pi] == '*') {
            star = pi++;
            match = vi;
        } else if (star != std::string::npos) {
            pi = star + 1;
            vi = ++match;
        } else {
            return false;
        }
    }
    while (pi < p.size() && p[pi] == '*') pi++;
    return pi == p.size();
}

std::string Slashify(std::string s) {
    for (char& c : s) if (c == '\\') c = '/';
    return s;
}

json ResultError(const std::string& err) {
    Result r;
    r.error = err;
    return json{{"ok", false}, {"error", err}};
}

Entry* FindId(Config& cfg, const std::string& id) {
    for (auto& e : cfg.permits) {
        if (e.id == id) return &e;
    }
    return nullptr;
}

} // namespace

bool IsValidKind(const std::string& kind) {
    return kind == "read_path" || kind == "write_path" || kind == "command" || kind == "network";
}

std::string MakeId(const std::string& kind, const std::string& target, const std::string& scope) {
    const std::string s = scope.empty() ? std::string(kGlobal) : scope;
    return kind + "|" + s + "|" + target;
}

json Entry::ToJson() const {
    json engineArray = json::array();
    for (const auto& e : engines) engineArray.push_back(e);
    return json{{"id", id}, {"kind", kind}, {"target", target}, {"engines", engineArray},
                {"scope", scope}, {"status", status}, {"approvedVia", approvedVia},
                {"approvedAt", approvedAt}, {"userSaid", userSaid}, {"note", note}};
}

Entry Entry::FromJson(const json& j) {
    Entry e;
    e.id = Field(j, "id");
    e.kind = Field(j, "kind");
    e.target = Field(j, "target");
    e.engines = StringArray(j, "engines");
    e.scope = Field(j, "scope", kGlobal);
    e.status = Field(j, "status", kProposed);
    e.approvedVia = Field(j, "approvedVia");
    e.approvedAt = Field(j, "approvedAt");
    e.userSaid = Field(j, "userSaid");
    e.note = Field(j, "note");
    if (e.id.empty()) e.id = MakeId(e.kind, e.target, e.scope);
    return e;
}

bool Entry::CoversEngine(const std::string& engine) const {
    if (engines.empty()) return true;
    if (engine.empty()) return true;
    for (const auto& e : engines) {
        if (Lower(e) == Lower(engine)) return true;
    }
    return false;
}

json HistoryEntry::ToJson() const {
    return json{{"at", at}, {"by", by}, {"action", action},
                {"entry", entry}, {"reason", reason}};
}

HistoryEntry HistoryEntry::FromJson(const json& j) {
    HistoryEntry h;
    h.at = Field(j, "at");
    h.by = Field(j, "by");
    h.action = Field(j, "action");
    h.entry = j.is_object() && j.contains("entry") && j["entry"].is_object()
                  ? j["entry"] : json::object();
    h.reason = Field(j, "reason");
    return h;
}

json Config::ToJson() const {
    json rows = json::array();
    for (const auto& e : permits) rows.push_back(e.ToJson());
    json hist = json::array();
    for (const auto& h : history) hist.push_back(h.ToJson());
    return json{{"permits", std::move(rows)}, {"history", std::move(hist)}};
}

std::wstring DefaultPath() {
    return PathJoinWide(GetEffectiveDataDirectoryWide(), L"permits.json");
}

bool Load(const std::wstring& path, Config& out, std::string& err) {
    err.clear();
    out = Config();
    const std::string text = ReadFileUtf8Wide(path);
    if (text.empty()) return true;
    json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        err = "permits.json must be a JSON object";
        return false;
    }
    auto it = root.find("permits");
    if (it != root.end()) {
        if (!it->is_array()) {
            err = "'permits' must be an array";
            return false;
        }
        for (const auto& row : *it) out.permits.push_back(Entry::FromJson(row));
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

static Result Mutate(const std::wstring& path, const std::string& action, const std::string& by,
                     const std::string& reason, std::function<bool(Config&, json&)> change) {
    Config cfg;
    std::string err;
    if (!Load(path, cfg, err)) return {false, err, json::object()};
    json detail = json::object();
    if (!change(cfg, detail)) return {false, "permit not found", json::object()};
    HistoryEntry h;
    h.at = GetCurrentTimestampStr();
    h.by = by.empty() ? "cli" : by;
    h.action = action;
    h.entry = detail;
    h.reason = reason;
    cfg.history.push_back(h);
    if (!Save(path, cfg, err)) return {false, err, json::object()};
    Result r;
    r.ok = true;
    r.data = json{{"ok", true}, {"action", action}, {"entry", detail}};
    return r;
}

Result List(const std::wstring& path, const std::string& scope) {
    Config cfg;
    std::string err;
    if (!Load(path, cfg, err)) return {false, err, json::object()};
    json rows = json::array();
    json proposed = json::array();
    for (const auto& e : cfg.permits) {
        if (!scope.empty() && e.scope != scope && e.scope != kGlobal) continue;
        json o = e.ToJson();
        o["usable"] = e.Usable();
        rows.push_back(o);
        if (e.status != kApproved) proposed.push_back(o);
    }
    Result r;
    r.ok = true;
    r.data = json{{"permits", std::move(rows)}, {"proposed", std::move(proposed)}};
    return r;
}

Result Request(const std::wstring& path, const std::string& kind, const std::string& target,
               const std::vector<std::string>& engines, const std::string& scope,
               const std::string& reason, const std::string& by) {
    if (!IsValidKind(kind)) return {false, "invalid permit kind: " + kind, json::object()};
    if (target.empty()) return {false, "--target is required", json::object()};
    const std::string id = MakeId(kind, target, scope);
    return Mutate(path, "request", by, reason, [&](Config& cfg, json& detail) {
        Entry* e = FindId(cfg, id);
        if (!e) {
            Entry n;
            n.id = id;
            n.kind = kind;
            n.target = target;
            n.engines = engines;
            n.scope = scope.empty() ? std::string(kGlobal) : scope;
            n.note = reason;
            n.status = kProposed;
            cfg.permits.push_back(n);
            e = &cfg.permits.back();
        } else {
            // Merge: never downgrade an approved row, just record that it was requested again.
            for (const auto& eng : engines) {
                if (std::find(e->engines.begin(), e->engines.end(), eng) == e->engines.end()) {
                    e->engines.push_back(eng);
                }
            }
            if (!reason.empty()) e->note = reason;
        }
        detail = e->ToJson();
        return true;
    });
}

Result Approve(const std::wstring& path, const std::string& id,
               const std::string& userSaid, const std::string& by) {
    if (userSaid.empty()) return {false, "--user-said must quote the user's words", json::object()};
    return Mutate(path, "approve", by, userSaid, [&](Config& cfg, json& detail) {
        Entry* e = FindId(cfg, id);
        if (!e) return false;
        e->status = kApproved;
        e->approvedVia = "chat";
        e->approvedAt = GetCurrentTimestampStr();
        e->userSaid = userSaid;
        detail = e->ToJson();
        return true;
    });
}

Result ApproveViaDashboard(const std::wstring& path, const std::string& id, const std::string& by) {
    return Mutate(path, "approve", by.empty() ? "dashboard" : by, "approved from dashboard",
                  [&](Config& cfg, json& detail) {
        Entry* e = FindId(cfg, id);
        if (!e) return false;
        e->status = kApproved;
        e->approvedVia = "dashboard";
        e->approvedAt = GetCurrentTimestampStr();
        detail = e->ToJson();
        return true;
    });
}

Result Reject(const std::wstring& path, const std::string& id,
              const std::string& reason, const std::string& by) {
    return Mutate(path, "reject", by, reason, [&](Config& cfg, json& detail) {
        Entry* e = FindId(cfg, id);
        if (!e) return false;
        e->status = kRejected;
        e->approvedVia.clear();
        e->approvedAt.clear();
        e->userSaid.clear();
        e->note = reason;
        detail = e->ToJson();
        return true;
    });
}

Result Revoke(const std::wstring& path, const std::string& id,
              const std::string& reason, const std::string& by) {
    return Mutate(path, "revoke", by, reason, [&](Config& cfg, json& detail) {
        Entry* e = FindId(cfg, id);
        if (!e) return false;
        e->status = kRevoked;
        e->approvedVia.clear();
        e->approvedAt.clear();
        e->userSaid.clear();
        detail = e->ToJson();
        return true;
    });
}

bool IsApproved(const Config& cfg, const std::string& kind, const std::string& target,
                const std::string& engine, const std::string& scope) {
    for (const auto& e : cfg.permits) {
        if (!e.Usable() || e.kind != kind || !e.CoversEngine(engine)) continue;
        if (e.scope != kGlobal && (scope.empty() || e.scope != scope)) continue;
        if (GlobMatch(Slashify(ExpandTarget(e.target)), Slashify(ExpandTarget(target)))) return true;
    }
    return false;
}

std::vector<Entry> Approved(const Config& cfg, const std::string& engine, const std::string& scope) {
    std::vector<Entry> out;
    for (const auto& e : cfg.permits) {
        if (!e.Usable() || !e.CoversEngine(engine)) continue;
        if (e.scope != kGlobal && (scope.empty() || e.scope != scope)) continue;
        out.push_back(e);
    }
    return out;
}

std::string ExpandTarget(const std::string& target) {
    std::string out = target;
    size_t tilde = 0;
    while (tilde < out.size() && (out[tilde] == ' ' || out[tilde] == '"')) tilde++;
    if (tilde < out.size() && out[tilde] == '~' &&
        (tilde + 1 == out.size() || out[tilde + 1] == '/' || out[tilde + 1] == '\\')) {
#ifdef _WIN32
        const char* home = std::getenv("USERPROFILE");
        if (home && *home) out.replace(tilde, 1, home);
#endif
    }
#ifdef _WIN32
    const int wideLen = MultiByteToWideChar(CP_UTF8, 0, out.c_str(), -1, nullptr, 0);
    if (wideLen > 0) {
        std::wstring wide(static_cast<size_t>(wideLen), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, out.c_str(), -1, &wide[0], wideLen);
        wide.pop_back();
        DWORD n = ExpandEnvironmentStringsW(wide.c_str(), nullptr, 0);
        if (n > 0) {
            std::wstring expanded(n, L'\0');
            n = ExpandEnvironmentStringsW(wide.c_str(), &expanded[0], n);
            if (n > 0) {
                expanded.resize(n - 1);
                int utf8Len = WideCharToMultiByte(CP_UTF8, 0, expanded.c_str(), -1, nullptr, 0, nullptr, nullptr);
                if (utf8Len > 0) {
                    std::string utf8(static_cast<size_t>(utf8Len), '\0');
                    WideCharToMultiByte(CP_UTF8, 0, expanded.c_str(), -1, &utf8[0], utf8Len, nullptr, nullptr);
                    utf8.pop_back();
                    out = utf8;
                }
            }
        }
    }
#endif
    return out;
}

} // namespace permit
} // namespace pc
