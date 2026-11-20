#include "routine.h"

#include "behavior_tree_internal.h"
#include "common.h"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

namespace pc {

using json = nlohmann::json;

namespace {

std::string LowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string BaseNameFromPath(const std::wstring& path) {
    return WideToUtf8(std::filesystem::path(path).stem().wstring());
}

std::wstring ParentDir(const std::wstring& path) {
    return std::filesystem::path(path).parent_path().wstring();
}

json OkJson() {
    return json{{"ok", true}};
}

json ErrorJson(const std::string& msg) {
    return json{{"ok", false}, {"error", msg}};
}

std::tm ToLocalTm(std::time_t t) {
    std::tm local = {};
#ifdef _WIN32
    localtime_s(&local, &t);
#else
    localtime_r(&t, &local);
#endif
    return local;
}

std::vector<std::string> JsonStrings(const json& value) {
    std::vector<std::string> out;
    if (!value.is_array()) return out;
    for (const auto& item : value) {
        if (item.is_string()) out.push_back(item.get<std::string>());
    }
    return out;
}

json StringArray(const std::vector<std::string>& values) {
    json out = json::array();
    for (const auto& value : values) out.push_back(value);
    return out;
}

// Collect every foreach glob in the BT. A file appearing/disappearing under one of these
// globs is absorbed by the run itself, so it must not make the routine stale.
void CollectForeachGlobs(const json& node,
                         std::vector<std::pair<std::string, std::vector<std::string>>>& out) {
    if (node.is_array()) {
        for (const auto& child : node) CollectForeachGlobs(child, out);
        return;
    }
    if (!node.is_object()) return;
    if (node.value("type", std::string()) == "foreach" && node.contains("glob") &&
        node["glob"].is_string() && !node["glob"].get<std::string>().empty()) {
        out.emplace_back(node["glob"].get<std::string>(), JsonStrings(node.value("exclude", json::array())));
    }
    for (auto it = node.begin(); it != node.end(); ++it) CollectForeachGlobs(it.value(), out);
}

// Expand the routine's input globs into path -> sha256.
json BuildInputsSnapshot(const std::string& cwd, const json& inputs) {
    json snapshot = json::object();
    for (const auto& input : inputs) {
        if (!input.is_string()) continue;
        std::vector<std::string> matches;
        if (!ExpandBtGlob(cwd, input.get<std::string>(), {}, matches)) continue;
        for (const auto& rel : matches) {
            const std::wstring full = ResolveBtPathWide(cwd, rel);
            snapshot[rel] = Sha256Hex(ReadFileUtf8Wide(full));
        }
    }
    return snapshot;
}

// Return the set of files currently matched by any foreach glob in the definition.
std::set<std::string> ForeachMatches(const std::string& cwd,
                                     const std::vector<std::pair<std::string, std::vector<std::string>>>& globs) {
    std::set<std::string> out;
    for (const auto& glob : globs) {
        std::vector<std::string> matches;
        if (ExpandBtGlob(cwd, glob.first, glob.second, matches)) {
            for (const auto& rel : matches) out.insert(rel);
        }
    }
    return out;
}

json CompareInputs(const json& def, const std::string& cwd) {
    json snapshot = def.contains("inputsSnapshot") && def["inputsSnapshot"].is_object()
                        ? def["inputsSnapshot"] : json::object();
    const json current = BuildInputsSnapshot(cwd, def.value("inputs", json::array()));

    std::vector<std::string> added, removed, changed, absorbed;
    std::vector<std::pair<std::string, std::vector<std::string>>> globs;
    CollectForeachGlobs(def.value("bt", json::object()), globs);
    const std::set<std::string> currentForeach = ForeachMatches(cwd, globs);
    const std::set<std::string> savedForeachSet = [&]() {
        std::set<std::string> set;
        for (const auto& path : JsonStrings(def.value("foreachSnapshot", json::array()))) set.insert(path);
        return set;
    }();

    for (auto it = current.begin(); it != current.end(); ++it) {
        const auto found = snapshot.find(it.key());
        if (found == snapshot.end()) {
            if (currentForeach.count(it.key())) absorbed.push_back(it.key());
            else added.push_back(it.key());
        } else if (found->is_string() && found->get<std::string>() != it.value().get<std::string>()) {
            changed.push_back(it.key());
        }
    }
    for (auto it = snapshot.begin(); it != snapshot.end(); ++it) {
        if (!current.contains(it.key())) {
            if (savedForeachSet.count(it.key())) absorbed.push_back(it.key());
            else removed.push_back(it.key());
        }
    }

    json out = json::object();
    out["state"] = (added.empty() && removed.empty() && changed.empty()) ? "ok" : "stale";
    out["added"] = StringArray(added);
    out["removed"] = StringArray(removed);
    out["changed"] = StringArray(changed);
    out["absorbed"] = StringArray(absorbed);
    return out;
}

void ApplyDiff(json& row, const json& def, const std::string& cwd) {
    const json diff = CompareInputs(def, cwd);
    for (auto it = diff.begin(); it != diff.end(); ++it) row[it.key()] = it.value();
}

// 093-5: a saved BT state file may carry live `status` fields; a routine definition must not.
void StripRuntimeStatus(json& node) {
    if (node.is_array()) {
        for (auto& child : node) StripRuntimeStatus(child);
        return;
    }
    if (!node.is_object()) return;
    node.erase("status");
    for (auto it = node.begin(); it != node.end(); ++it) StripRuntimeStatus(it.value());
}

} // namespace

bool RoutineManager::ValidName(const std::string& name) {
    if (name.empty() || name.size() > 80) return false;
    if (name.find("..") != std::string::npos) return false;
    for (char c : name) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (std::isalnum(u) || c == '_' || c == '-' || c == '.') continue;
        return false;
    }
    return true;
}

std::string RoutineManager::DataDirUtf8() {
    return WideToUtf8(GetEffectiveDataDirectoryWide());
}

std::wstring RoutineManager::RoutineDirWide(const std::string& scope, const std::string& cwd) {
    if (LowerAscii(scope) == "data") {
        return PathJoinWide(Utf8ToWide(DataDirUtf8()), L"routines");
    }
    const std::wstring root = cwd.empty() ? GetCurrentDirectoryWide()
                                          : ResolveAbsolutePathWide(Utf8ToWide(cwd));
    return PathJoinWide(PathJoinWide(root, L".hancho"), L"routines");
}

std::wstring RoutineManager::DefinitionPathWide(const std::string& name, const std::string& scope,
                                                const std::string& cwd, std::string* outScope,
                                                bool* outFound) {
    const std::string normalized = LowerAscii(scope);
    const std::wstring file = Utf8ToWide(name) + L".json";
    const std::wstring projectPath = PathJoinWide(RoutineDirWide("project", cwd), file);
    const std::wstring dataPath = PathJoinWide(RoutineDirWide("data", cwd), file);

    std::wstring chosen;
    std::string chosenScope;
    if (normalized == "project") {
        chosen = projectPath;
        chosenScope = "project";
    } else if (normalized == "data") {
        chosen = dataPath;
        chosenScope = "data";
    } else if (PathExistsWide(projectPath)) {
        chosen = projectPath;
        chosenScope = "project";
    } else {
        chosen = dataPath;
        chosenScope = "data";
    }
    if (outScope) *outScope = chosenScope;
    if (outFound) *outFound = PathExistsWide(chosen);
    return chosen;
}

std::wstring RoutineManager::ProposalDirWide(const std::string& scope, const std::string& cwd) {
    return PathJoinWide(RoutineDirWide(scope, cwd), L".proposals");
}

std::wstring RoutineManager::ProposalPathWide(const std::string& name, const std::string& scope,
                                              const std::string& cwd, std::string* outScope,
                                              bool* outFound) {
    const std::string normalized = LowerAscii(scope);
    const std::wstring file = Utf8ToWide(name) + L".json";
    const std::wstring projectPath = PathJoinWide(ProposalDirWide("project", cwd), file);
    const std::wstring dataPath = PathJoinWide(ProposalDirWide("data", cwd), file);
    std::wstring chosen;
    std::string chosenScope;
    if (normalized == "project") {
        chosen = projectPath;
        chosenScope = "project";
    } else if (normalized == "data") {
        chosen = dataPath;
        chosenScope = "data";
    } else if (PathExistsWide(projectPath)) {
        chosen = projectPath;
        chosenScope = "project";
    } else if (PathExistsWide(dataPath)) {
        chosen = dataPath;
        chosenScope = "data";
    } else {
        chosen = projectPath;
        chosenScope = "project";
    }
    if (outScope) *outScope = chosenScope;
    if (outFound) *outFound = PathExistsWide(chosen);
    return chosen;
}

json RoutineManager::ReadProposal(const std::string& name, const std::string& cwd,
                                  std::string* outScope, bool* outFound) {
    std::string scope;
    bool found = false;
    const std::wstring path = ProposalPathWide(name, "", cwd, &scope, &found);
    if (outScope) *outScope = scope;
    if (outFound) *outFound = found;
    if (!found) return json();
    json proposal = json::parse(ReadFileUtf8Wide(path), nullptr, false);
    if (proposal.is_discarded() || !proposal.is_object()) return json();
    return proposal;
}

bool RoutineManager::HasProposal(const std::string& name, const std::string& cwd) {
    bool found = false;
    ProposalPathWide(name, "", cwd, nullptr, &found);
    return found;
}

std::wstring RoutineManager::RunsPathWide(const std::string& name) {
    return PathJoinWide(RoutineDirWide("data", ""), Utf8ToWide(name) + L".runs.json");
}

json RoutineManager::ReadRuns(const std::string& name) {
    const std::wstring path = RunsPathWide(name);
    if (!PathExistsWide(path)) return json::array();
    json arr = json::parse(ReadFileUtf8Wide(path), nullptr, false);
    if (arr.is_discarded() || !arr.is_array()) return json::array();
    return arr;
}

std::string RoutineManager::MakeRunId(const std::string& name, const std::string& runsJson) {
    json runs = json::parse(runsJson, nullptr, false);
    std::set<std::string> used;
    if (!runs.is_discarded() && runs.is_array()) {
        for (const auto& r : runs) {
            if (r.is_object() && r.contains("runId") && r["runId"].is_string()) {
                used.insert(r["runId"].get<std::string>());
            }
        }
    }
    const std::wstring btDir = PathJoinWide(Utf8ToWide(DataDirUtf8()), L"behaviortrees");
    const std::time_t now = std::time(nullptr);
    for (int offset = 0; offset < 100000; ++offset) {
        const std::time_t candidateTime = now + offset;
        const std::tm local = ToLocalTm(candidateTime);
        char stamp[32] = {};
        std::snprintf(stamp, sizeof(stamp), "%04d%02d%02d-%02d%02d%02d",
                      local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
                      local.tm_hour, local.tm_min, local.tm_sec);
        const std::string runId = name + "-" + stamp;
        if (used.count(runId)) continue;
        if (PathExistsWide(PathJoinWide(btDir, Utf8ToWide(SanitizeFileNameComponent(runId))) + L".json")) continue;
        return runId;
    }
    return name + "-" + GetCurrentTimestampStr();
}

std::string RoutineManager::Save(const std::string& name,
                                 const std::string& btJson,
                                 const std::vector<std::string>& inputs,
                                 const std::string& summary,
                                 const std::string& notes,
                                 const std::string& scope,
                                 const std::string& cwd,
                                 const std::string& by,
                                 const std::string& reason) {
    if (!ValidName(name)) return ErrorJson("invalid routine name (use letters, digits, _, - or .)").dump();

    json bt = json::parse(btJson, nullptr, false);
    if (bt.is_discarded() || !bt.is_object()) return ErrorJson("invalid BT JSON").dump();

    if (validator_) {
        std::string validationError;
        if (!validator_(btJson, validationError)) {
            return ErrorJson("BT validation failed: " + (validationError.empty() ? "invalid definition" : validationError)).dump();
        }
    } else if (!bt.contains("root") || !bt["root"].is_object()) {
        return ErrorJson("BT validation failed: root is required").dump();
    }

    const std::string normalizedScope = LowerAscii(scope);
    if (normalizedScope != "project" && normalizedScope != "data") {
        return ErrorJson("scope must be 'project' or 'data'").dump();
    }

    const std::wstring path = DefinitionPathWide(name, normalizedScope, cwd);
    json existing = json::parse(ReadFileUtf8Wide(path), nullptr, false);
    json history = json::array();
    if (!existing.is_discarded() && existing.is_object() && existing.contains("history") &&
        existing["history"].is_array()) {
        history = existing["history"];
    }
    history.push_back(json{{"at", GetCurrentTimestampStr()},
                           {"by", by.empty() ? "hancho-cli" : by},
                           {"reason", reason}});

    json inputArray = json::array();
    for (const auto& input : inputs) inputArray.push_back(input);

    const std::string effectiveCwd = normalizedScope == "data"
                                         ? DataDirUtf8()
                                         : ResolveAbsolutePathUtf8(cwd.empty() ? "." : cwd);
    json snapshot = BuildInputsSnapshot(effectiveCwd, inputArray);

    std::vector<std::pair<std::string, std::vector<std::string>>> globs;
    CollectForeachGlobs(bt, globs);
    const std::set<std::string> foreachSet = ForeachMatches(effectiveCwd, globs);
    json foreachSnapshot = json::array();
    for (auto it = snapshot.begin(); it != snapshot.end(); ++it) {
        if (foreachSet.count(it.key())) foreachSnapshot.push_back(it.key());
    }

    json def;
    def["name"] = name;
    def["summary"] = summary;
    def["cwd"] = effectiveCwd;
    def["bt"] = std::move(bt);
    def["inputs"] = std::move(inputArray);
    def["notes"] = notes;
    def["inputsSnapshot"] = std::move(snapshot);
    def["foreachSnapshot"] = std::move(foreachSnapshot);
    def["history"] = std::move(history);

    EnsureDirectoryWide(ParentDir(path));
    WriteFileAtomicWide(path, def.dump(2));

    json out = OkJson();
    out["name"] = name;
    out["scope"] = normalizedScope;
    out["path"] = WideToUtf8(path);
    return out.dump();
}

std::string RoutineManager::SaveProposal(const std::string& name,
                                         const std::string& btJson,
                                         const std::vector<std::string>& inputs,
                                         const std::string& summary,
                                         const std::string& notes,
                                         const std::string& scope,
                                         const std::string& cwd,
                                         const std::string& by,
                                         const std::string& reason,
                                         const json& proposalMeta) {
    if (!ValidName(name)) return ErrorJson("invalid routine name (use letters, digits, _, - or .)").dump();

    json bt = json::parse(btJson, nullptr, false);
    if (bt.is_discarded() || !bt.is_object()) return ErrorJson("invalid BT JSON").dump();
    if (validator_) {
        std::string validationError;
        if (!validator_(btJson, validationError)) {
            return ErrorJson("BT validation failed: " +
                             (validationError.empty() ? "invalid definition" : validationError)).dump();
        }
    } else if (!bt.contains("root") || !bt["root"].is_object()) {
        return ErrorJson("BT validation failed: root is required").dump();
    }

    const std::string normalizedScope = LowerAscii(scope);
    if (normalizedScope != "project" && normalizedScope != "data") {
        return ErrorJson("scope must be 'project' or 'data'").dump();
    }

    const std::wstring path = ProposalPathWide(name, normalizedScope, cwd);
    json existing = json::parse(ReadFileUtf8Wide(DefinitionPathWide(name, normalizedScope, cwd)),
                                nullptr, false);
    if (existing.is_discarded() || !existing.is_object()) {
        existing = json::parse(ReadFileUtf8Wide(path), nullptr, false);
    }
    json history = json::array();
    if (!existing.is_discarded() && existing.is_object() && existing.contains("history") &&
        existing["history"].is_array()) {
        history = existing["history"];
    }
    history.push_back(json{{"at", GetCurrentTimestampStr()},
                           {"by", by.empty() ? "llm" : by},
                           {"reason", reason},
                           {"status", "proposed"}});

    json inputArray = json::array();
    for (const auto& input : inputs) inputArray.push_back(input);
    const std::string effectiveCwd = normalizedScope == "data"
                                         ? DataDirUtf8()
                                         : ResolveAbsolutePathUtf8(cwd.empty() ? "." : cwd);
    json snapshot = BuildInputsSnapshot(effectiveCwd, inputArray);
    std::vector<std::pair<std::string, std::vector<std::string>>> globs;
    CollectForeachGlobs(bt, globs);
    const std::set<std::string> foreachSet = ForeachMatches(effectiveCwd, globs);
    json foreachSnapshot = json::array();
    for (auto it = snapshot.begin(); it != snapshot.end(); ++it) {
        if (foreachSet.count(it.key())) foreachSnapshot.push_back(it.key());
    }

    json def;
    def["name"] = name;
    def["summary"] = summary;
    def["cwd"] = effectiveCwd;
    def["bt"] = std::move(bt);
    def["inputs"] = std::move(inputArray);
    def["notes"] = notes;
    def["inputsSnapshot"] = std::move(snapshot);
    def["foreachSnapshot"] = std::move(foreachSnapshot);
    def["history"] = std::move(history);
    def["status"] = "proposed";
    def["proposal"] = proposalMeta.is_object() ? proposalMeta : json::object();

    EnsureDirectoryWide(ParentDir(path));
    WriteFileAtomicWide(path, def.dump(2));

    json out = OkJson();
    out["name"] = name;
    out["scope"] = normalizedScope;
    out["path"] = WideToUtf8(path);
    out["status"] = "proposed";
    return out.dump();
}

std::string RoutineManager::Approve(const std::string& name, const std::string& cwd,
                                    const std::string& userSaid, const std::string& by,
                                    const std::string& via) {
    if (!ValidName(name)) return ErrorJson("invalid routine name").dump();
    const std::string normalizedVia = LowerAscii(via.empty() ? std::string("chat") : via);
    if (normalizedVia == "chat" && userSaid.empty()) {
        return ErrorJson("--user-said must not be empty; an empty value means reject").dump();
    }

    std::string pScope;
    bool pFound = false;
    const std::wstring pPath = ProposalPathWide(name, "", cwd, &pScope, &pFound);
    if (!pFound) return ErrorJson("no pending proposal for routine: " + name).dump();
    json def = json::parse(ReadFileUtf8Wide(pPath), nullptr, false);
    if (def.is_discarded() || !def.is_object()) {
        return ErrorJson("invalid proposal for routine: " + name).dump();
    }
    def.erase("proposal");
    def["status"] = "approved";
    json history = def.value("history", json::array());
    if (!history.is_array()) history = json::array();
    json approval = {{"at", GetCurrentTimestampStr()},
                     {"by", by.empty() ? "user" : by},
                     {"reason", "approved via " + normalizedVia},
                     {"approvedVia", normalizedVia},
                     {"status", "approved"}};
    if (!userSaid.empty()) approval["userSaid"] = userSaid;
    history.push_back(std::move(approval));
    def["history"] = std::move(history);

    const std::wstring approvedPath = DefinitionPathWide(name, pScope, cwd);
    EnsureDirectoryWide(ParentDir(approvedPath));
    WriteFileAtomicWide(approvedPath, def.dump(2));
    RemoveFileWide(pPath);

    json out = OkJson();
    out["name"] = name;
    out["scope"] = pScope;
    out["status"] = "approved";
    out["approvedVia"] = normalizedVia;
    out["path"] = WideToUtf8(approvedPath);
    return out.dump();
}

std::string RoutineManager::Reject(const std::string& name, const std::string& cwd,
                                   const std::string& reason, const std::string& by) {
    if (!ValidName(name)) return ErrorJson("invalid routine name").dump();
    std::string pScope;
    bool pFound = false;
    const std::wstring pPath = ProposalPathWide(name, "", cwd, &pScope, &pFound);
    if (!pFound) return ErrorJson("no pending proposal for routine: " + name).dump();

    const std::wstring approvedPath = DefinitionPathWide(name, pScope, cwd);
    if (PathExistsWide(approvedPath)) {
        json def = json::parse(ReadFileUtf8Wide(approvedPath), nullptr, false);
        if (!def.is_discarded() && def.is_object()) {
            json history = def.value("history", json::array());
            if (!history.is_array()) history = json::array();
            history.push_back(json{{"at", GetCurrentTimestampStr()},
                                   {"by", by.empty() ? "user" : by},
                                   {"reason", "proposal rejected: " + reason},
                                   {"status", "rejected"}});
            def["history"] = std::move(history);
            WriteFileAtomicWide(approvedPath, def.dump(2));
        }
    }
    RemoveFileWide(pPath);

    json out = OkJson();
    out["name"] = name;
    out["status"] = "rejected";
    out["reason"] = reason;
    return out.dump();
}

std::string RoutineManager::SaveFromBt(const std::string& name, const std::string& btId,
                                       const std::string& scope, const std::string& cwd,
                                       const std::string& by, bool proposed,
                                       const std::string& reason) {
    if (!ValidName(name)) return ErrorJson("invalid routine name").dump();
    if (btId.empty()) return ErrorJson("bt id is required for --from-bt").dump();

    const std::wstring path = PathJoinWide(PathJoinWide(Utf8ToWide(DataDirUtf8()), L"behaviortrees"),
                                           Utf8ToWide(SanitizeFileNameComponent(btId))) + L".json";
    if (!PathExistsWide(path)) return ErrorJson("BT not found: " + btId).dump();
    json doc = json::parse(ReadFileUtf8Wide(path), nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return ErrorJson("invalid BT file: " + btId).dump();

    json bt;
    if (doc.contains("definition") && doc["definition"].is_object()) bt = doc["definition"];
    else if (doc.contains("tree") && doc["tree"].is_object()) bt = doc["tree"];
    else if (doc.contains("root")) bt = doc;
    else return ErrorJson("BT file has no definition: " + btId).dump();
    StripRuntimeStatus(bt);

    std::vector<std::string> inputs;
    if (bt.contains("inputs") && bt["inputs"].is_array()) inputs = JsonStrings(bt["inputs"]);
    const std::string summary = bt.value("summary", std::string("from BT ") + btId);
    const std::string notes = std::string("registered from BT ") + btId;
    const std::string btText = bt.dump();
    if (proposed) {
        return SaveProposal(name, btText, inputs, summary, notes, scope, cwd, by, reason,
                            json{{"request", "routine save --from-bt " + btId},
                                 {"rationale", "Registered from a previously successful BT."},
                                 {"fromBt", btId}});
    }
    return Save(name, btText, inputs, summary, notes, scope, cwd, by, reason);
}

std::string RoutineManager::List(const std::string& cwd) const {
    std::map<std::string, json> byName;
    for (const char* scope : {"project", "data"}) {
        const std::wstring dir = RoutineDirWide(scope, cwd);
        for (const auto& path : ListDirectoryWide(dir, L"*.json")) {
            const std::string name = BaseNameFromPath(path);
            if (!ValidName(name) || byName.count(name)) continue;
            json def = json::parse(ReadFileUtf8Wide(path), nullptr, false);
            if (def.is_discarded() || !def.is_object()) continue;

            json runs = ReadRuns(name);
            json lastRun;
            if (!runs.empty() && runs.back().is_object()) lastRun = runs.back();

            json row;
            row["name"] = name;
            row["summary"] = def.value("summary", std::string());
            row["scope"] = scope;
            row["path"] = WideToUtf8(path);
            row["lastRun"] = std::move(lastRun);
            row["status"] = def.value("status", std::string("approved"));
            row["hasProposal"] = HasProposal(name, cwd);
            const std::string defCwd = def.value("cwd", std::string());
            ApplyDiff(row, def, defCwd.empty() ? cwd : defCwd);
            byName[name] = std::move(row);
        }
    }

    // 093-5: proposals for brand-new routines have no approved file yet, so they only live
    // under <dir>/.proposals. Approved routines with a pending proposal are annotated above.
    for (const char* scope : {"project", "data"}) {
        const std::wstring dir = ProposalDirWide(scope, cwd);
        for (const auto& path : ListDirectoryWide(dir, L"*.json")) {
            const std::string name = BaseNameFromPath(path);
            if (!ValidName(name) || byName.count(name)) continue;
            json proposal = json::parse(ReadFileUtf8Wide(path), nullptr, false);
            if (proposal.is_discarded() || !proposal.is_object()) continue;

            json runs = ReadRuns(name);
            json lastRun;
            if (!runs.empty() && runs.back().is_object()) lastRun = runs.back();
            json row;
            row["name"] = name;
            row["summary"] = proposal.value("summary", std::string());
            row["scope"] = scope;
            row["path"] = WideToUtf8(path);
            row["lastRun"] = std::move(lastRun);
            row["status"] = "proposed";
            row["hasProposal"] = true;
            const std::string defCwd = proposal.value("cwd", std::string());
            ApplyDiff(row, proposal, defCwd.empty() ? cwd : defCwd);
            byName[name] = std::move(row);
        }
    }

    json rows = json::array();
    for (auto& kv : byName) rows.push_back(std::move(kv.second));
    json out = OkJson();
    out["routines"] = std::move(rows);
    return out.dump();
}

std::string RoutineManager::Show(const std::string& name, const std::string& cwd) const {
    std::string scope;
    bool found = false;
    const std::wstring path = DefinitionPathWide(name, "", cwd, &scope, &found);
    const json proposal = ReadProposal(name, cwd);
    const bool hasProposal = !proposal.is_null();

    json def;
    if (found) {
        def = json::parse(ReadFileUtf8Wide(path), nullptr, false);
        if (def.is_discarded() || !def.is_object()) {
            return ErrorJson("invalid routine definition: " + name).dump();
        }
    } else if (hasProposal) {
        def = proposal;
    } else {
        return ErrorJson("routine not found: " + name).dump();
    }

    json runs = ReadRuns(name);
    json out = OkJson();
    out["name"] = name;
    out["scope"] = scope.empty() ? proposal.value("scope", std::string()) : scope;
    out["path"] = WideToUtf8(found ? path : ProposalPathWide(name, "", cwd));
    const std::string defCwd = def.value("cwd", std::string());
    ApplyDiff(out, def, defCwd.empty() ? cwd : defCwd);
    out["definition"] = def;
    out["approved"] = found;
    out["status"] = def.value("status", std::string(found ? "approved" : "proposed"));
    out["hasProposal"] = hasProposal;
    out["runs"] = runs;
    out["lastRun"] = runs.empty() ? json() : runs.back();
    if (hasProposal) {
        json p = proposal.value("proposal", json::object());
        p["definition"] = proposal;
        p["scope"] = proposal.value("scope", std::string());
        out["dryRun"] = p.value("dryRun", json::object());
        out["proposal"] = std::move(p);
    }
    return out.dump();
}

std::string RoutineManager::Check(const std::string& name, const std::string& cwd) const {
    std::string scope;
    bool found = false;
    const std::wstring path = DefinitionPathWide(name, "", cwd, &scope, &found);
    if (!found) return ErrorJson("routine not found: " + name).dump();
    json def = json::parse(ReadFileUtf8Wide(path), nullptr, false);
    if (def.is_discarded() || !def.is_object()) {
        return ErrorJson("invalid routine definition: " + name).dump();
    }
    const std::string defCwd = def.value("cwd", std::string());
    json out = OkJson();
    out["name"] = name;
    out["scope"] = scope;
    ApplyDiff(out, def, defCwd.empty() ? cwd : defCwd);
    return out.dump();
}

std::string RoutineManager::PrepareRun(const std::string& name, const std::string& cwd) const {
    std::string scope;
    bool found = false;
    const std::wstring path = DefinitionPathWide(name, "", cwd, &scope, &found);
    if (!found) {
        if (HasProposal(name, cwd)) {
            return ErrorJson("routine '" + name +
                             "' is proposed but not approved; run `routine approve` first").dump();
        }
        return ErrorJson("routine not found: " + name).dump();
    }

    json def = json::parse(ReadFileUtf8Wide(path), nullptr, false);
    if (def.is_discarded() || !def.is_object()) {
        return ErrorJson("invalid routine definition: " + name).dump();
    }
    if (def.value("status", std::string("approved")) == "proposed") {
        return ErrorJson("routine '" + name + "' is proposed but not approved").dump();
    }
    json bt = def.value("bt", json::object());
    if (!bt.is_object() || !bt.contains("root")) {
        return ErrorJson("routine '" + name + "' has no valid BT definition").dump();
    }

    const json runs = ReadRuns(name);
    const std::string runId = MakeRunId(name, runs.dump());
    bt["id"] = runId;
    bt["taskqueue"] = "routine-" + name;
    const std::string defCwd = def.value("cwd", std::string());
    if (!defCwd.empty() && (!bt.contains("cwd") || !bt["cwd"].is_string() || bt["cwd"].get<std::string>().empty())) {
        bt["cwd"] = defCwd;
    }

    json out = OkJson();
    out["name"] = name;
    out["scope"] = scope;
    out["runId"] = runId;
    out["taskqueue"] = "routine-" + name;
    out["cwd"] = defCwd;
    ApplyDiff(out, def, defCwd.empty() ? cwd : defCwd);
    out["bt"] = bt.dump();
    return out.dump();
}

bool RoutineManager::AppendRun(const std::string& name, const json& run, std::string& outError) const {
    if (!ValidName(name)) {
        outError = "invalid routine name";
        return false;
    }
    json runs = ReadRuns(name);
    runs.push_back(run);
    while (runs.size() > 50) runs.erase(runs.begin());

    const std::wstring path = RunsPathWide(name);
    std::error_code ec;
    std::filesystem::create_directories(ParentDir(path), ec);
    WriteFileAtomicWide(path, runs.dump(2));
    if (!PathExistsWide(path)) {
        outError = "failed to write " + WideToUtf8(path);
        return false;
    }
    return true;
}

bool RoutineManager::UpdateLatestRun(const std::string& name, const json& patch, std::string& outError) const {
    json runs = ReadRuns(name);
    if (runs.empty() || !runs.back().is_object()) {
        outError = "no run record to update";
        return false;
    }
    for (auto it = patch.begin(); it != patch.end(); ++it) {
        runs.back()[it.key()] = it.value();
    }
    const std::wstring path = RunsPathWide(name);
    std::error_code ec;
    std::filesystem::create_directories(ParentDir(path), ec);
    WriteFileAtomicWide(path, runs.dump(2));
    return true;
}

bool RoutineManager::RefreshInputsSnapshot(const std::string& name, const std::string& cwd,
                                           const std::string& by, std::string& outError) const {
    std::string scope;
    bool found = false;
    const std::wstring path = DefinitionPathWide(name, "", cwd, &scope, &found);
    if (!found) {
        outError = "routine not found: " + name;
        return false;
    }
    json def = json::parse(ReadFileUtf8Wide(path), nullptr, false);
    if (def.is_discarded() || !def.is_object()) {
        outError = "invalid routine definition: " + name;
        return false;
    }
    const std::string defCwd = def.value("cwd", std::string());
    const std::string effectiveCwd = defCwd.empty() ? cwd : defCwd;
    json snapshot = BuildInputsSnapshot(effectiveCwd, def.value("inputs", json::array()));
    std::vector<std::pair<std::string, std::vector<std::string>>> globs;
    CollectForeachGlobs(def.value("bt", json::object()), globs);
    const std::set<std::string> foreachSet = ForeachMatches(effectiveCwd, globs);
    json foreachSnapshot = json::array();
    for (auto it = snapshot.begin(); it != snapshot.end(); ++it) {
        if (foreachSet.count(it.key())) foreachSnapshot.push_back(it.key());
    }
    def["inputsSnapshot"] = std::move(snapshot);
    def["foreachSnapshot"] = std::move(foreachSnapshot);

    json history = def.value("history", json::array());
    if (!history.is_array()) history = json::array();
    history.push_back(json{{"at", GetCurrentTimestampStr()},
                           {"by", by.empty() ? "routine-run" : by},
                           {"reason", "inputs snapshot refreshed after successful routine run"}});
    def["history"] = std::move(history);

    WriteFileAtomicWide(path, def.dump(2));
    if (!PathExistsWide(path)) {
        outError = "failed to write " + WideToUtf8(path);
        return false;
    }
    return true;
}

} // namespace pc
