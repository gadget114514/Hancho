#include "bt_manager.h"
#include "common.h"
#include "nlohmann/json.hpp"

#include <sstream>

namespace pc {

namespace {

using json = nlohmann::json;

std::string MakeErrorJson(const std::string& msg) {
    json j;
    j["ok"] = false;
    j["error"] = msg;
    return j.dump();
}

std::string StrField(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return "";
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

std::string CompactOneLine(std::string s, size_t maxLen = 80) {
    for (char& c : s) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    }
    if (s.size() > maxLen) {
        size_t cut = maxLen - 3;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) cut--;
        s = s.substr(0, cut) + "...";
    }
    return s;
}

std::string CompactConditionText(const json& n) {
    std::ostringstream ss;
    for (const char* k : {"file", "path", "node", "name", "var", "than", "regex"}) {
        std::string v = StrField(n, k);
        if (!v.empty()) ss << ' ' << v;
    }
    for (const char* op : {"eq", "ne", "in", "gt", "lt", "exists", "state"}) {
        auto it = n.find(op);
        if (it != n.end()) ss << ' ' << op << ' ' << (it->is_string() ? it->get<std::string>() : it->dump());
    }
    return ss.str();
}

std::string FormatCompactNodeLine(const json& n, const std::string& role) {
    if (!n.is_object()) return "[?] (not an object)";
    std::string type = StrField(n, "type");
    if (type.empty()) type = "?";
    json status = n.value("status", json());
    std::string state = status.is_object() ? StrField(status, "state") : "";
    if (state.empty()) state = "idle";

    std::ostringstream ss;
    ss << '[' << state << "] ";
    if (!role.empty() && role != "child") ss << role << ": ";
    ss << type;
    std::string id = StrField(n, "id");
    if (!id.empty()) ss << ' ' << id;

    int runs = status.is_object() ? status.value("runs", 0) : 0;
    if (type == "retry" || type == "repeat") {
        ss << " (" << runs << '/' << n.value("max", 0) << ')';
    } else if (type == "job" || type == "worker") {
        if (type == "worker") {
            std::string engine = StrField(n, "engine");
            std::string model = StrField(n, "model");
            if (!engine.empty() || !model.empty()) {
                ss << " [" << (engine.empty() ? "default" : engine);
                if (!model.empty()) ss << ' ' << model;
                ss << ']';
            }
        }
        std::string summary = StrField(n, "summary");
        if (!summary.empty()) ss << " \"" << CompactOneLine(summary, 60) << '"';
        if (runs > 1) ss << " x" << runs;
    } else if (type == "escalate") {
        std::string q = StrField(n, "question");
        if (!q.empty()) ss << " \"" << CompactOneLine(q, 60) << '"';
        std::string answer = status.is_object() ? StrField(status, "answer") : "";
        if (!answer.empty()) ss << " -> " << CompactOneLine(answer, 30);
    } else if (type == "wait" || type == "timeout") {
        ss << ' ' << n.value("sec", 0) << 's';
    } else if (type == "parallel") {
        auto it = n.find("success");
        if (it != n.end()) ss << " success=" << (it->is_string() ? it->get<std::string>() : it->dump());
    } else if (type == "set") {
        std::string var = StrField(n, "var");
        if (var.empty()) var = StrField(n, "name");
        if (!var.empty()) ss << ' ' << var << " = " << CompactOneLine(StrField(n, "value"), 40);
    } else if (!n.contains("children") && !n.contains("child") && !n.contains("cond")) {
        std::string c = CompactConditionText(n);
        if (!c.empty()) ss << CompactOneLine(c, 70);
    }

    if (status.is_object()) {
        auto jobs = status.find("jobs");
        if (jobs != status.end() && jobs->is_array() && !jobs->empty() && jobs->back().is_string()) {
            ss << "  job " << jobs->back().get<std::string>();
        }
        std::string detail = StrField(status, "detail");
        if (!detail.empty()) ss << "  - " << CompactOneLine(detail, 60);
    }
    return ss.str();
}

void AppendCompactNodeLines(const json& n, const std::string& role, int depth, std::ostringstream& out) {
    if (!n.is_object() || depth > 64) return;
    out << std::string(static_cast<size_t>(depth) * 2, ' ') << FormatCompactNodeLine(n, role) << '\n';
    for (const char* slot : {"cond", "then", "else", "child", "until"}) {
        auto it = n.find(slot);
        if (it != n.end() && it->is_object() && it->contains("type")) {
            AppendCompactNodeLines(*it, slot, depth + 1, out);
        }
    }
    auto kids = n.find("children");
    if (kids != n.end() && kids->is_array()) {
        for (const auto& c : *kids) {
            AppendCompactNodeLines(c, "", depth + 1, out);
        }
    }
}

std::string FormatCompactTreeText(const json& bt) {
    if (!bt.is_object()) return "";
    std::ostringstream ss;
    std::string state = StrField(bt, "state");
    if (state.empty()) state = "idle";
    ss << '[' << state << "] " << StrField(bt, "id");
    std::string summary = StrField(bt, "summary");
    if (!summary.empty()) ss << " \"" << CompactOneLine(summary, 70) << '"';
    if (bt.contains("nodeRuns")) {
        ss << "  runs " << bt.value("nodeRuns", 0);
        auto limits = bt.find("limits");
        if (limits != bt.end() && limits->is_object() && limits->contains("maxNodeRuns")) {
            ss << '/' << limits->value("maxNodeRuns", 0);
        }
    }
    std::string updated = StrField(bt, "updatedAt");
    if (!updated.empty()) ss << "  " << updated;
    ss << '\n';
    auto root = bt.find("root");
    if (root != bt.end() && root->is_object()) {
        AppendCompactNodeLines(*root, "", 1, ss);
    }
    std::string res = ss.str();
    if (!res.empty() && res.back() == '\n') res.pop_back();
    return res;
}

void FindWaitingEscalationNodeKey(const json& node, const std::string& btId, std::string& outKey) {
    if (!outKey.empty() || !node.is_object()) return;
    if (node.value("type", "") == "escalate" &&
        node.contains("status") && node["status"].is_object() &&
        node["status"].value("state", "") == "running") {
        std::string id = node.value("id", "");
        if (!id.empty()) {
            outKey = btId + "/" + id;
            return;
        }
    }
    if (node.contains("children") && node["children"].is_array()) {
        for (const auto& c : node["children"]) {
            FindWaitingEscalationNodeKey(c, btId, outKey);
            if (!outKey.empty()) return;
        }
    }
    for (const char* k : {"cond", "then", "else", "child", "until"}) {
        if (node.contains(k) && node[k].is_object()) {
            FindWaitingEscalationNodeKey(node[k], btId, outKey);
            if (!outKey.empty()) return;
        }
    }
}

void TrimNodeDefinition(json& n) {
    if (!n.is_object()) return;
    for (const char* k : {"prompt", "instructionsText"}) {
        auto it = n.find(k);
        if (it != n.end() && it->is_string()) {
            *it = BtVarPreview(it->get<std::string>());
        }
    }
    for (const char* slot : {"cond", "then", "else", "child", "until"}) {
        auto it = n.find(slot);
        if (it != n.end() && it->is_object()) {
            TrimNodeDefinition(*it);
        }
    }
    auto kids = n.find("children");
    if (kids != n.end() && kids->is_array()) {
        for (auto& c : *kids) {
            TrimNodeDefinition(c);
        }
    }
}

// How much of the tree a reply carries. Every reply is read by an LLM, so only bt_get
// (without compact) pays for the whole state document.
enum class TreeView {
    Summary,        // id/state/version/nodeRuns, the compact text and any escalation; no "tree"
    TrimmedVars,    // plus "tree", with long variable values and prompts cut to kBtVarPreviewBytes
    Full,           // plus "tree", with every variable in full
};

json BuildTreeResponseJson(const BehaviorTree& tree, TreeView view) {
    json stateDoc = json::parse(tree.ToStateJson(), nullptr, false);
    if (stateDoc.is_discarded()) stateDoc = json::object();

    json out;
    out["ok"] = true;
    out["id"] = tree.Id();
    out["state"] = tree.State();
    if (!tree.Detail().empty()) out["detail"] = tree.Detail();
    out["version"] = tree.Version();
    out["nodeRuns"] = tree.NodeRuns();
    out["taskqueue"] = tree.TaskQueueName();
    out["compact"] = FormatCompactTreeText(stateDoc);

    if (tree.State() == "waitingForLlm") {
        std::string escNode;
        const auto& events = tree.Events();
        for (auto it = events.rbegin(); it != events.rend(); ++it) {
            if (it->type == "escalated" && !it->nodeKey.empty()) {
                escNode = it->nodeKey;
                break;
            }
        }
        if (escNode.empty() && stateDoc.contains("root")) {
            FindWaitingEscalationNodeKey(stateDoc["root"], tree.Id(), escNode);
        }
        if (!escNode.empty()) {
            std::string pktStr = tree.GetEscalationPacketJson(escNode);
            json pkt = json::parse(pktStr, nullptr, false);
            if (!pkt.is_discarded()) {
                out["escalation"] = std::move(pkt);
            }
        }
    }

    if (view != TreeView::Summary) {
        if (view == TreeView::TrimmedVars) {
            auto vars = stateDoc.find("vars");
            if (vars != stateDoc.end() && vars->is_object()) {
                for (auto& v : *vars) {
                    if (v.is_string()) v = BtVarPreview(v.get<std::string>());
                }
            }
            auto rootIt = stateDoc.find("root");
            if (rootIt != stateDoc.end() && rootIt->is_object()) {
                TrimNodeDefinition(*rootIt);
            }
        }
        out["tree"] = std::move(stateDoc);
    }
    return out;
}

} // namespace

std::string BtManager::Validate(const std::string& jsonText) {
    std::string err;
    std::lock_guard<std::mutex> lock(mutex_);
    if (!BehaviorTree::ValidateJson(jsonText, this, err)) {
        json out;
        out["ok"] = false;
        out["valid"] = false;
        out["error"] = err;
        return out.dump();
    }
    json out;
    out["ok"] = true;
    out["valid"] = true;
    return out.dump();
}

std::string BtManager::Start(const std::string& jsonText) {
    std::string err;
    std::lock_guard<std::mutex> lock(mutex_);

    auto tree = std::make_unique<BehaviorTree>();
    if (!tree->LoadDefinition(jsonText, this, err)) {
        return MakeErrorJson(err);
    }

    const std::string btId = tree->Id();
    auto existing = trees_.find(btId);
    if (existing != trees_.end() && existing->second && existing->second->tree) {
        const std::string st = existing->second->tree->State();
        if (st == "running" || st == "waitingForLlm" || st == "paused") {
            return MakeErrorJson("behavior tree '" + btId + "' is already active");
        }
    }

    tree->SetJobRunner(this);
    auto entry = std::make_unique<TreeEntry>();
    entry->tree = std::move(tree);
    TreeEntry* entryPtr = entry.get();
    trees_[btId] = std::move(entry);

    DeleteFileW(GetBtEventsFileWide(btId).c_str());

    activeLaunchingQueueName_ = entryPtr->tree->TaskQueueName();
    entryPtr->tree->Start(NowEpochMs());
    activeLaunchingQueueName_.clear();

    PersistEntryLocked(*entryPtr);
    return BuildTreeResponseJson(*entryPtr->tree, TreeView::Summary).dump();
}

std::string BtManager::Get(const std::string& id, bool compact) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = trees_.find(id);
    if (it == trees_.end() || !it->second || !it->second->tree) {
        return MakeErrorJson("behavior tree not found: " + id);
    }
    return BuildTreeResponseJson(*it->second->tree, compact ? TreeView::Summary : TreeView::Full).dump();
}

std::string BtManager::List() {
    std::lock_guard<std::mutex> lock(mutex_);
    json arr = json::array();
    for (const auto& [id, entry] : trees_) {
        (void)id;
        if (!entry || !entry->tree) continue;
        const BehaviorTree& t = *entry->tree;
        json item;
        item["id"] = t.Id();
        item["summary"] = t.Summary();
        item["intent"] = t.Intent();
        item["state"] = t.State();
        if (!t.Detail().empty()) item["detail"] = t.Detail();
        item["version"] = t.Version();
        item["nodeRuns"] = t.NodeRuns();
        item["taskqueue"] = t.TaskQueueName();
        item["createdAt"] = t.CreatedAt();
        item["updatedAt"] = t.UpdatedAt();
        arr.push_back(std::move(item));
    }
    json out;
    out["ok"] = true;
    out["trees"] = std::move(arr);
    return out.dump();
}

std::string BtManager::Wait(const std::string& id, uint64_t sinceVersion, int timeoutMs, bool compact) {
    uint64_t startTick = GetNowTickMs();
    bool timedOut = false;
    std::unique_lock<std::mutex> lock(mutex_);
    while (!stopped_.load()) {
        auto it = trees_.find(id);
        if (it == trees_.end() || !it->second || !it->second->tree) {
            return MakeErrorJson("behavior tree not found: " + id);
        }
        const BehaviorTree& tree = *it->second->tree;
        const std::string st = tree.State();
        const uint64_t ver = tree.Version();

        const bool isTerminal = (st == "succeeded" || st == "failed" || st == "cancelled");
        const bool isWaiting = (st == "waitingForLlm" || st == "paused");
        if (isTerminal) break;
        if (isWaiting && ver != sinceVersion) break;

        if (timeoutMs >= 0) {
            int64_t elapsed = static_cast<int64_t>(GetNowTickMs() - startTick);
            if (elapsed >= timeoutMs) { timedOut = true; break; }
            int64_t remain = std::min<int64_t>(100, timeoutMs - elapsed);
            waitCv_.wait_for(lock, std::chrono::milliseconds(remain));
        } else {
            waitCv_.wait_for(lock, std::chrono::milliseconds(100));
        }
    }

    auto it = trees_.find(id);
    if (it == trees_.end() || !it->second || !it->second->tree) {
        return MakeErrorJson("behavior tree not found: " + id);
    }
    const BehaviorTree& tree = *it->second->tree;
    if (timedOut) {
        // Nothing for the caller to act on: keep the reply small so a supervisor
        // LLM that loops on bt_wait does not pay for the whole tree every poll.
        // bt_get returns the full tree when it is actually needed.
        json out;
        out["ok"] = true;
        out["id"] = tree.Id();
        out["state"] = tree.State();
        if (!tree.Detail().empty()) out["detail"] = tree.Detail();
        out["version"] = tree.Version();
        out["nodeRuns"] = tree.NodeRuns();
        out["timedOut"] = true;
        out["hint"] = "Still running; call bt_wait again with this version. Use bt_get for node details.";
        return out.dump();
    }
    return BuildTreeResponseJson(tree, compact ? TreeView::Summary : TreeView::TrimmedVars).dump();
}

std::string BtManager::Resume(const std::string& id,
                              const std::string& node,
                              const std::string& answer,
                              const std::string& rationale,
                              const std::string& varsJson) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = trees_.find(id);
    if (it == trees_.end() || !it->second || !it->second->tree) {
        return MakeErrorJson("behavior tree not found: " + id);
    }

    std::map<std::string, std::string> extraVars;
    if (!varsJson.empty()) {
        json vj = json::parse(varsJson, nullptr, false);
        if (vj.is_discarded() || !vj.is_object()) {
            return MakeErrorJson("varsJson must be a JSON object");
        }
        for (auto vit = vj.begin(); vit != vj.end(); ++vit) {
            if (vit.value().is_string()) extraVars[vit.key()] = vit.value().get<std::string>();
            else extraVars[vit.key()] = vit.value().dump();
        }
    }

    TreeEntry& entry = *it->second;
    std::string err;
    activeLaunchingQueueName_ = entry.tree->TaskQueueName();
    bool ok = entry.tree->Resume(node, answer, rationale, extraVars, NowEpochMs(), err);
    activeLaunchingQueueName_.clear();
    if (!ok) {
        return MakeErrorJson(err);
    }

    PersistEntryLocked(entry);
    return BuildTreeResponseJson(*entry.tree, TreeView::Summary).dump();
}

std::string BtManager::Retry(const std::string& id,
                             const std::string& fromNode,
                             bool resetCounters) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = trees_.find(id);
    if (it == trees_.end() || !it->second || !it->second->tree) {
        return MakeErrorJson("behavior tree not found: " + id);
    }

    TreeEntry& entry = *it->second;
    std::string err;
    activeLaunchingQueueName_ = entry.tree->TaskQueueName();
    bool ok = entry.tree->Retry(fromNode, resetCounters, NowEpochMs(), err);
    activeLaunchingQueueName_.clear();
    if (!ok) {
        return MakeErrorJson(err);
    }

    PersistEntryLocked(entry);
    return BuildTreeResponseJson(*entry.tree, TreeView::Summary).dump();
}

std::string BtManager::Update(const std::string& id,
                              const std::string& node,
                              const std::string& jsonText) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = trees_.find(id);
    if (it == trees_.end() || !it->second || !it->second->tree) {
        return MakeErrorJson("behavior tree not found: " + id);
    }

    TreeEntry& entry = *it->second;
    std::string err;
    if (!entry.tree->UpdateDefinition(jsonText, node, NowEpochMs(), err)) {
        return MakeErrorJson(err);
    }

    PersistEntryLocked(entry);
    return BuildTreeResponseJson(*entry.tree, TreeView::Summary).dump();
}

std::string BtManager::Update(const std::string& id,
                              const std::string& jsonText) {
    return Update(id, "", jsonText);
}

std::string BtManager::Cancel(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = trees_.find(id);
    if (it == trees_.end() || !it->second || !it->second->tree) {
        return MakeErrorJson("behavior tree not found: " + id);
    }

    TreeEntry& entry = *it->second;
    entry.tree->Cancel(NowEpochMs());
    PersistEntryLocked(entry);
    return BuildTreeResponseJson(*entry.tree, TreeView::Summary).dump();
}

std::string BtManager::Pause(const std::string& id, bool paused) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = trees_.find(id);
    if (it == trees_.end() || !it->second || !it->second->tree) {
        return MakeErrorJson("behavior tree not found: " + id);
    }

    TreeEntry& entry = *it->second;
    std::string err;
    activeLaunchingQueueName_ = entry.tree->TaskQueueName();
    bool ok = entry.tree->SetPaused(paused, NowEpochMs(), err);
    activeLaunchingQueueName_.clear();
    if (!ok) {
        return MakeErrorJson(err);
    }

    std::string tqErr;
    tqm_.TaskQueuePause(entry.tree->TaskQueueName(), paused, tqErr);

    PersistEntryLocked(entry);
    return BuildTreeResponseJson(*entry.tree, TreeView::Summary).dump();
}

} // namespace pc
