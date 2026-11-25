#include "bt_manager.h"
#include "bt_templates.h"
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
    for (const char* k : {"file", "path", "node", "name", "var", "than", "regex", "class", "cause"}) {
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
    } else if (type == "job" || type == "worker" || type == "exec" || type == "agent") {
        if (type == "worker" || type == "agent") {
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
        if (status.is_object() && status.value("takenOver", false)) ss << " [takenOver]";
    } else if (type == "wait" || type == "timeout") {
        ss << ' ' << n.value("sec", 0) << 's';
    } else if (type == "parallel") {
        auto it = n.find("success");
        if (it != n.end()) ss << " success=" << (it->is_string() ? it->get<std::string>() : it->dump());
    } else if (type == "catch") {
        auto hs = n.find("handlers");
        ss << " handlers=" << (hs != n.end() && hs->is_array() ? hs->size() : 0)
           << " maxRetries=" << n.value("maxRetries", 1);
        std::string handler = status.is_object() ? StrField(status, "catchHandler") : "";
        if (!handler.empty()) ss << " -> " << CompactOneLine(handler, 50);
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
    for (const char* slot : {"cond", "then", "else", "child", "until", "otherwise"}) {
        auto it = n.find(slot);
        if (it != n.end() && it->is_object() && it->contains("type")) {
            AppendCompactNodeLines(*it, slot, depth + 1, out);
        }
    }
    auto hs = n.find("handlers");
    if (hs != n.end() && hs->is_array()) {
        for (const auto& h : *hs) {
            if (h.is_object() && h.contains("do") && h["do"].is_object() && h["do"].contains("type")) {
                AppendCompactNodeLines(h["do"], "do", depth + 1, out);
            }
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
    for (const char* k : {"cond", "then", "else", "child", "until", "otherwise"}) {
        if (node.contains(k) && node[k].is_object()) {
            FindWaitingEscalationNodeKey(node[k], btId, outKey);
            if (!outKey.empty()) return;
        }
    }
    if (node.contains("handlers") && node["handlers"].is_array()) {
        for (const auto& h : node["handlers"]) {
            if (h.is_object() && h.contains("do") && h["do"].is_object()) {
                FindWaitingEscalationNodeKey(h["do"], btId, outKey);
                if (!outKey.empty()) return;
            }
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
    for (const char* slot : {"cond", "then", "else", "child", "until", "otherwise"}) {
        auto it = n.find(slot);
        if (it != n.end() && it->is_object()) {
            TrimNodeDefinition(*it);
        }
    }
    auto hs = n.find("handlers");
    if (hs != n.end() && hs->is_array()) {
        for (auto& h : *hs) {
            if (h.is_object() && h.contains("do") && h["do"].is_object()) {
                TrimNodeDefinition(h["do"]);
            }
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
    if (!tree.DirectionId().empty()) out["directionId"] = tree.DirectionId();
    out["state"] = tree.State();
    if (!tree.Detail().empty()) out["detail"] = tree.Detail();
    out["version"] = tree.Version();
    out["nodeRuns"] = tree.NodeRuns();
    out["taskqueue"] = tree.TaskQueueName();
    out["pendingEstimateSec"] = tree.PendingEstimateSec();
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

    json planned = json::array();
    for (const auto& task : tree.PlannedTasks()) {
        json item;
        item["node"] = task.nodeKey;
        item["type"] = task.type;
        item["summary"] = task.summary;
        item["taskqueue"] = task.queueName;
        item["jobId"] = task.proposedJobId;
        item["estimateSec"] = task.estimateSec;
        planned.push_back(std::move(item));
    }
    out["plannedTasks"] = std::move(planned);
    return out;
}

} // namespace

std::string BtManager::Validate(const std::string& jsonText, bool strict) {
    std::lock_guard<std::mutex> lock(mutex_);
    return BehaviorTree::ValidateResponse(jsonText, this, strict);
}

std::string BtManager::Start(const std::string& jsonText, bool paused, bool triage) {
    std::string err;
    std::lock_guard<std::mutex> lock(mutex_);

    std::vector<std::string> lintErrors;
    std::vector<BtWarning> warnings;
    BehaviorTree::ValidateJson(jsonText, this, lintErrors, warnings);

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

    bool effTriage = triage;
    if (effTriage) {
        auto parsedDef = json::parse(jsonText, nullptr, false);
        if (parsedDef.is_object() && parsedDef.contains("triage") && parsedDef["triage"].is_boolean()) {
            effTriage = parsedDef["triage"].get<bool>();
        }
    }

    std::string dirId;
    std::string dirErr;
    const std::string btTitle = tree->Summary().empty() ? btId : tree->Summary();
    if (!tqm_.EnsureBtDirection(btId, tree->DirectionId(), tree->TaskId(), btTitle,
                                tqm_.BtOriginJson(btId), dirId, dirErr, effTriage)) {
        return MakeErrorJson(dirErr);
    }
    tree->SetDirectionId(dirId);

    tree->SetJobRunner(this);
    tree->SetProber(this);
    tree->SetEngineHealth(this);
    auto entry = std::make_unique<TreeEntry>();
    entry->tree = std::move(tree);
    TreeEntry* entryPtr = entry.get();
    trees_[btId] = std::move(entry);

    RemoveFileWide(GetBtEventsFileWide(btId));

    activeLaunchingQueueName_ = entryPtr->tree->TaskQueueName();
    entryPtr->tree->Start(NowEpochMs(), paused);
    activeLaunchingQueueName_.clear();
    if (paused) {
        std::string tqErr;
        tqm_.TaskQueuePause(entryPtr->tree->TaskQueueName(), true, tqErr);
    }

    PersistEntryLocked(*entryPtr);
    json resp = BuildTreeResponseJson(*entryPtr->tree, TreeView::Summary);
    json warningArray = json::array();
    for (const auto& w : warnings) {
        json wj{{"path", w.path}, {"code", w.code},
                {"message", w.message}, {"suggestion", w.suggestion}};
        if (!w.alias.empty()) wj["alias"] = w.alias;
        warningArray.push_back(std::move(wj));
    }
    resp["warnings"] = std::move(warningArray);
    return resp.dump();
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
        if (!t.DirectionId().empty()) item["directionId"] = t.DirectionId();
        item["summary"] = t.Summary();
        item["intent"] = t.Intent();
        item["state"] = t.State();
        if (!t.Detail().empty()) item["detail"] = t.Detail();
        item["version"] = t.Version();
        item["nodeRuns"] = t.NodeRuns();
        item["taskqueue"] = t.TaskQueueName();
        item["pendingEstimateSec"] = t.PendingEstimateSec();
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
    // Task log (100-3): record escalation answer on the BT's task if it has a record.
    if (!entry.tree->TaskId().empty()) {
        std::string text = answer;
        if (!rationale.empty()) {
            text += (text.empty() ? "" : ": ") + rationale;
        }
        tqm_.TaskLog(entry.tree->TaskId(), "escalate_answered", "", entry.tree->Id(), text, "");
    }
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
    const std::string btId = entry.tree->Id();
    entry.tree->Cancel(NowEpochMs());
    PersistEntryLocked(entry);
    // The tree drops its active jobs itself; any other queued/provisional jobs that still
    // point at this tree have to be cancelled here or they would stay pending forever.
    CancelQueuedJobsForTree(btId);
    return BuildTreeResponseJson(*entry.tree, TreeView::Summary).dump();
}

std::string BtManager::Remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = trees_.find(id);
    if (it == trees_.end() || !it->second || !it->second->tree) {
        return MakeErrorJson("behavior tree not found: " + id);
    }
    const std::string btId = it->second->tree->Id();
    // Stop any running work first so no process is left untracked.
    it->second->tree->Cancel(NowEpochMs());
    CancelQueuedJobsForTree(btId);
    // Drop the tree's jobs from the queues too, so its rows leave the task list.
    std::string tqErr;
    tqm_.RemoveJobsForTree(btId, tqErr);
    if (!tqErr.empty()) {
        WriteAuditLog("BT_REMOVE_ERR", "remove jobs for '" + btId + "' failed: " + tqErr);
    }
    trees_.erase(it);
    RemoveFileWide(GetBtStateFileWide(btId));
    RemoveFileWide(GetBtEventsFileWide(btId));
    return "{\"ok\":true,\"id\":\"" + JsonEscape(btId) + "\"}";
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

namespace {

void ReplaceAllInString(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

void ApplyCardParamsToNode(nlohmann::json& node,
                           const std::string& cardNumber,
                           const std::string& cardTitle,
                           const std::string& cardBody,
                           const nlohmann::json& effectiveParams) {
    if (!node.is_object()) return;
    const std::string type = node.value("type", std::string());
    const std::string id = node.value("id", std::string());

    if (node.contains("summary") && node["summary"].is_string()) {
        std::string s = node["summary"].get<std::string>();
        ReplaceAllInString(s, "<card summary>", cardTitle);
        ReplaceAllInString(s, "<task summary>", cardTitle);
        node["summary"] = s;
    }

    if ((type == "agent" || type == "worker") && id == "impl") {
        std::string prompt = node.value("prompt", std::string());
        if (prompt.find("<card body>") != std::string::npos) {
            ReplaceAllInString(prompt, "<card body>", cardBody);
        } else if (prompt.find("<agent prompt>") != std::string::npos) {
            ReplaceAllInString(prompt, "<agent prompt>", cardBody);
        } else if (prompt.find("<worker prompt>") != std::string::npos) {
            ReplaceAllInString(prompt, "<worker prompt>", cardBody);
        } else {
            prompt = cardBody;
        }
        node["prompt"] = prompt;

        if (effectiveParams.contains("engines") && effectiveParams["engines"].is_array() &&
            !effectiveParams["engines"].empty()) {
            node["engines"] = effectiveParams["engines"];
        } else {
            node.erase("engines");
        }
        if (!node.contains("engines") || !node["engines"].is_array() || node["engines"].size() < 2) {
            node.erase("switchEngineOn");
        }
    }

    if (type == "escalate") {
        node["question"] = "Card " + cardNumber + " (" + cardTitle + ") failed. What should the supervisor decide?";
    }

    if (node.contains("child") && node["child"].is_object()) {
        ApplyCardParamsToNode(node["child"], cardNumber, cardTitle, cardBody, effectiveParams);
    }

    if (node.contains("children") && node["children"].is_array()) {
        nlohmann::json filtered = nlohmann::json::array();
        for (auto& ch : node["children"]) {
            if (!ch.is_object()) continue;
            const std::string chType = ch.value("type", std::string());
            const std::string chId = ch.value("id", std::string());
            if ((chType == "exec" || chType == "job") && chId == "build") {
                const std::string buildCmd = effectiveParams.value("build", std::string());
                if (buildCmd.empty()) continue;
                ch["command"] = buildCmd;
            } else if ((chType == "exec" || chType == "job") && chId == "test") {
                const std::string testCmd = effectiveParams.value("test", std::string());
                if (testCmd.empty()) continue;
                ch["command"] = testCmd;
            } else {
                ApplyCardParamsToNode(ch, cardNumber, cardTitle, cardBody, effectiveParams);
            }
            filtered.push_back(std::move(ch));
        }
        node["children"] = std::move(filtered);
    }
}

bool BuildCardBtDefinition(const std::string& taskId,
                           const nlohmann::json& taskCardDefaults,
                           const nlohmann::json& showDoc,
                           nlohmann::json& outBtDef,
                           std::string& outBtId,
                           std::string& outDirId,
                           std::string& outError) {
    outError.clear();
    const nlohmann::json dir = showDoc.value("direction", nlohmann::json::object());
    const nlohmann::json card = showDoc.value("card", nlohmann::json::object());
    outDirId = dir.value("id", std::string());
    const std::string number = card.value("number", std::string());
    const std::string title = card.value("title", dir.value("title", std::string()));
    const std::string body = card.value("body", std::string());
    std::string tmplName = card.value("template", std::string("card"));
    if (tmplName.empty()) tmplName = "card";

    nlohmann::json btDef = BtTemplateDefinition(tmplName);
    if (btDef.is_null() || !btDef.is_object()) {
        outError = "unknown bt template: " + tmplName;
        return false;
    }

    nlohmann::json effectiveParams = nlohmann::json::object();
    if (taskCardDefaults.is_object()) {
        for (auto it = taskCardDefaults.begin(); it != taskCardDefaults.end(); ++it) {
            effectiveParams[it.key()] = it.value();
        }
    }
    if (card.contains("params") && card["params"].is_object()) {
        const auto& cp = card["params"];
        for (auto it = cp.begin(); it != cp.end(); ++it) {
            effectiveParams[it.key()] = it.value();
        }
    }

    outBtId = "card-" + taskId + "-" + number;
    btDef["id"] = outBtId;
    btDef["taskId"] = taskId;
    btDef["directionId"] = outDirId;
    btDef["summary"] = number + ": " + title;
    btDef["intent"] = title;
    if (effectiveParams.contains("cwd") && effectiveParams["cwd"].is_string() &&
        !effectiveParams["cwd"].get<std::string>().empty()) {
        btDef["cwd"] = effectiveParams["cwd"];
    }
    if (btDef.contains("root") && btDef["root"].is_object()) {
        ApplyCardParamsToNode(btDef["root"], number, title, body, effectiveParams);
    }
    outBtDef = std::move(btDef);
    return true;
}

} // namespace

std::string BtManager::CardRun(const std::string& taskId,
                               const std::string& number,
                               bool ready,
                               int maxConcurrent,
                               bool force,
                               const std::string& originJson) {
    std::string err;
    if (!tqm_.ValidateTaskActive(taskId, err)) {
        return MakeErrorJson(err);
    }
    nlohmann::json taskRec;
    if (!tqm_.TaskGet(taskId, taskRec)) {
        return MakeErrorJson("task not found: " + taskId);
    }
    const nlohmann::json cardDefaults =
        (taskRec.contains("cardDefaults") && taskRec["cardDefaults"].is_object())
            ? taskRec["cardDefaults"]
            : nlohmann::json::object();

    auto startOneCard = [&](const nlohmann::json& showDoc, nlohmann::json& outStarted,
                            std::string& outErr) -> bool {
        nlohmann::json btDef;
        std::string btId;
        std::string dirId;
        if (!BuildCardBtDefinition(taskId, cardDefaults, showDoc, btDef, btId, dirId, outErr)) {
            return false;
        }
        if (!originJson.empty()) {
            tqm_.SetBtOrigin(btId, originJson);
        }
        const json startResp = json::parse(Start(btDef.dump(), false), nullptr, false);
        if (!startResp.is_object() || !startResp.value("ok", false)) {
            outErr = startResp.is_object() ? startResp.value("error", std::string("failed to start card BT"))
                                           : std::string("failed to start card BT");
            return false;
        }
        const std::string num = showDoc["card"].value("number", std::string());
        outStarted = {
            {"number", num},
            {"directionId", dirId},
            {"btId", btId},
            {"state", startResp.value("state", std::string("running"))}
        };
        return true;
    };

    if (!ready) {
        if (number.empty()) {
            return MakeErrorJson("card number or --ready is required");
        }
        const std::string showRaw = tqm_.CardShow(number, taskId, err);
        const json showDoc = json::parse(showRaw, nullptr, false);
        if (!showDoc.is_object() || !showDoc.value("ok", false)) {
            return showRaw.empty() ? MakeErrorJson(err) : showRaw;
        }
        const json dir = showDoc.value("direction", json::object());
        const json status = showDoc.value("status", json::object());
        const std::string state = status.value("state", std::string());
        if (dir.value("kind", std::string()) != "card" || state != "planned") {
            return MakeErrorJson("card '" + number + "' can only be run while planned (current state: " + state + ")");
        }
        const json waiting = showDoc.value("waitingFor", json::array());
        if (!force && waiting.is_array() && !waiting.empty()) {
            std::string deps;
            for (size_t i = 0; i < waiting.size(); ++i) {
                if (i > 0) deps += ", ";
                deps += waiting[i].get<std::string>();
            }
            return MakeErrorJson("card '" + number + "' is waiting on dependencies: " + deps);
        }
        json startedItem;
        if (!startOneCard(showDoc, startedItem, err)) {
            return MakeErrorJson(err);
        }
        startedItem["ok"] = true;
        startedItem["taskId"] = taskId;
        return startedItem.dump();
    }

    if (!number.empty()) {
        return MakeErrorJson("cannot specify both card number and --ready");
    }
    if (maxConcurrent < 1) {
        return MakeErrorJson("max must be >= 1");
    }
    const std::string listRaw = tqm_.CardList(taskId, err);
    const json listDoc = json::parse(listRaw, nullptr, false);
    if (!listDoc.is_object() || !listDoc.value("ok", false)) {
        return listRaw.empty() ? MakeErrorJson(err) : listRaw;
    }
    const json cards = listDoc.value("cards", json::array());
    int activeCount = 0;
    for (const auto& c : cards) {
        const std::string st = c.value("state", std::string());
        if (st == "running" || st == "waitingForLlm" || st == "paused") {
            ++activeCount;
        }
    }

    json started = json::array();
    for (const auto& c : cards) {
        if (activeCount + static_cast<int>(started.size()) >= maxConcurrent) break;
        if (c.value("state", std::string()) != "planned") continue;
        const json waiting = c.value("waitingFor", json::array());
        if (!force && waiting.is_array() && !waiting.empty()) continue;

        const std::string num = c.value("number", std::string());
        const json showDoc = json::parse(tqm_.CardShow(num, taskId, err), nullptr, false);
        if (!showDoc.is_object() || !showDoc.value("ok", false)) {
            return MakeErrorJson(err.empty() ? ("failed to load card " + num) : err);
        }
        json startedItem;
        if (!startOneCard(showDoc, startedItem, err)) {
            return MakeErrorJson(err);
        }
        started.push_back(std::move(startedItem));
    }

    return json{
        {"ok", true},
        {"taskId", taskId},
        {"started", started},
        {"activeCount", activeCount + static_cast<int>(started.size())},
        {"max", maxConcurrent}
    }.dump();
}

} // namespace pc
