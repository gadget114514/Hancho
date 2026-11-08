#include "behavior_tree_internal.h"
#include "common.h"

#include <algorithm>
#include <sstream>

namespace pc {

namespace {

std::string TruncateUtf8Bytes(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t n = maxBytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
    return s.substr(0, n) + " ...[truncated, " + std::to_string(s.size()) + " bytes]";
}

bool FindPathToNode(const BtNode* cur, const BtNode* target, std::vector<const BtNode*>& path) {
    if (!cur) return false;
    path.push_back(cur);
    if (cur == target) return true;
    for (const auto& c : cur->children) {
        if (FindPathToNode(c.get(), target, path)) return true;
    }
    if (FindPathToNode(cur->condChild.get(), target, path)) return true;
    if (FindPathToNode(cur->thenChild.get(), target, path)) return true;
    if (FindPathToNode(cur->elseChild.get(), target, path)) return true;
    if (FindPathToNode(cur->singleChild.get(), target, path)) return true;
    if (FindPathToNode(cur->untilChild.get(), target, path)) return true;
    path.pop_back();
    return false;
}

void CollectVisitedNodes(const BtNode* cur, std::vector<const BtNode*>& out) {
    if (!cur) return;
    if (cur->hasStatus) out.push_back(cur);
    for (const auto& c : cur->children) CollectVisitedNodes(c.get(), out);
    CollectVisitedNodes(cur->condChild.get(), out);
    CollectVisitedNodes(cur->thenChild.get(), out);
    CollectVisitedNodes(cur->elseChild.get(), out);
    CollectVisitedNodes(cur->singleChild.get(), out);
    CollectVisitedNodes(cur->untilChild.get(), out);
}

const BtNode* FindActiveEscalation(const BtNode* cur, const std::string& nodeKey, const std::string& btId) {
    if (!cur) return nullptr;
    if (cur->type == "escalate" && cur->state == "running") {
        if (nodeKey.empty() ||
            cur->id == nodeKey || cur->pathKey == nodeKey || cur->nodeKey == nodeKey ||
            btId + "/" + cur->id == nodeKey || btId + "/" + cur->pathKey == nodeKey ||
            btId + "/" + cur->nodeKey == nodeKey) {
            return cur;
        }
    }
    for (const auto& c : cur->children) {
        if (const BtNode* f = FindActiveEscalation(c.get(), nodeKey, btId)) return f;
    }
    if (const BtNode* f = FindActiveEscalation(cur->condChild.get(), nodeKey, btId)) return f;
    if (const BtNode* f = FindActiveEscalation(cur->thenChild.get(), nodeKey, btId)) return f;
    if (const BtNode* f = FindActiveEscalation(cur->elseChild.get(), nodeKey, btId)) return f;
    if (const BtNode* f = FindActiveEscalation(cur->singleChild.get(), nodeKey, btId)) return f;
    if (const BtNode* f = FindActiveEscalation(cur->untilChild.get(), nodeKey, btId)) return f;
    return nullptr;
}

} // namespace

void BehaviorTree::Impl::StartEscalateNode(BehaviorTree& bt, BtNode& node, int64_t nowMs) {
    std::string expandedQuestion, expandedGuidance, expandedIntent, undefVar;
    if (!ExpandString(bt, node.question, false, expandedQuestion, undefVar) ||
        !ExpandString(bt, node.guidance, false, expandedGuidance, undefVar) ||
        !ExpandString(bt, bt.intent_, false, expandedIntent, undefVar)) {
        node.hasStatus = true;
        node.state = "failed";
        node.detail = "undefinedVar: " + undefVar;
        bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
        return;
    }

    node.hasStatus = true;
    node.state = "running";
    node.runs++;
    node.detail.clear();
    node.answer.clear();
    node.rationale.clear();
    node.lastStartMs = nowMs;
    node.lastStartTimestamp = FormatEpochMsIso(nowMs);
    node.escalateDeadlineMs = nowMs + static_cast<int64_t>(node.escalateTimeoutSec) * 1000;
    node.fallbackLaunched = false;
    if (node.fallback.enabled && node.fallback.afterSec > 0) {
        node.fallbackDeadlineMs = nowMs + static_cast<int64_t>(node.fallback.afterSec) * 1000;
    } else {
        node.fallbackDeadlineMs = 0;
    }

    // Build decision packet (section 5.6)
    BtEscalationPacket pkt;
    pkt.btId = bt.id_;
    pkt.nodeKey = node.nodeKey;
    pkt.run = node.runs;
    pkt.intent = expandedIntent;
    pkt.question = expandedQuestion;
    pkt.guidance = expandedGuidance;
    pkt.expect = node.expect;

    std::vector<const BtNode*> pathNodes;
    FindPathToNode(root.get(), &node, pathNodes);
    std::vector<const BtNode*> visitedNodes;
    CollectVisitedNodes(root.get(), visitedNodes);

    json pathArr = json::array();
    for (const BtNode* pn : visitedNodes) {
        json entry;
        entry["node"] = pn->nodeKey;
        entry["path"] = pn->pathKey;
        entry["type"] = pn->type;
        entry["state"] = pn->state;
        entry["runs"] = pn->runs;
        if (pn->maxCount > 0) entry["max"] = pn->maxCount;
        if (!pn->detail.empty()) entry["detail"] = pn->detail;
        pathArr.push_back(std::move(entry));
    }

    json logsObj = json::object();
    int logLines = node.include.logLines > 0 ? node.include.logLines : 100;
    for (const std::string& logNodeRef : node.include.logs) {
        const BtNode* ln = FindNodeConst(logNodeRef, bt.id_);
        std::string tail;
        if (ln) {
            tail = ln->lastLogTail;
            if (tail.empty() && !ln->lastLogFile.empty()) {
                tail = ReadFileUtf8Wide(Utf8ToWide(ln->lastLogFile));
            }
            tail = GetTailLines(tail, logLines);
        }
        logsObj[logNodeRef] = tail;
    }

    json incVarsObj = json::object();
    for (const std::string& vn : node.include.vars) {
        std::string v;
        if (LookupVar(bt, vn, v)) incVarsObj[vn] = v;
    }

    json filesObj = json::object();
    size_t fileMaxBytes = node.include.fileMaxBytes > 0 ? node.include.fileMaxBytes : 16384;
    for (const std::string& relFile : node.include.files) {
        std::string expandedRel, u;
        if (!ExpandString(bt, relFile, false, expandedRel, u)) expandedRel = relFile;
        std::wstring fp = ResolveBtPathWide(bt.cwd_, expandedRel);
        filesObj[relFile] = TruncateUtf8Bytes(ReadFileUtf8Wide(fp), fileMaxBytes);
    }

    // Every variable, cut short: a worker's ${<id>.result} can be 64 KB and the packet is
    // read by an LLM. Values the decision really needs go in include.vars, in full.
    json allVarsObj = json::object();
    for (const auto& [k, v] : bt.vars_) allVarsObj[k] = BtVarPreview(v);

    json pktJson;
    pktJson["btId"] = pkt.btId;
    pktJson["node"] = pkt.nodeKey;
    pktJson["run"] = pkt.run;
    pktJson["intent"] = pkt.intent;
    pktJson["question"] = pkt.question;
    pktJson["guidance"] = pkt.guidance;
    pktJson["expect"] = pkt.expect;
    pktJson["path"] = pathArr;
    pktJson["include"] = {
        {"logs", logsObj},
        {"vars", incVarsObj},
        {"files", filesObj}
    };
    pktJson["vars"] = allVarsObj;
    pktJson["howToAnswer"] = {
        {"tool", "bt_resume"},
        {"bt", pkt.btId},
        {"node", pkt.nodeKey},
        {"required", json::array({"answer", "rationale"})}
    };
    pkt.json = pktJson.dump(2, ' ', false, json::error_handler_t::replace);

    std::ostringstream md;
    md << "# Escalation Decision Packet: " << pkt.btId << " / " << pkt.nodeKey << "\n\n";
    if (!pkt.intent.empty()) md << "## Intent\n" << pkt.intent << "\n\n";
    md << "## Question\n" << pkt.question << "\n\n";
    if (!pkt.guidance.empty()) md << "## Guidance\n" << pkt.guidance << "\n\n";
    if (!pkt.expect.empty()) {
        md << "## Allowed Answers (`expect`)\n";
        for (const auto& ex : pkt.expect) md << "- `" << ex << "`\n";
        md << "\n";
    }
    md << "## Execution Path & Node History\n";
    for (const BtNode* pn : visitedNodes) {
        md << "- `" << pn->nodeKey << "` (" << pn->type << "): " << pn->state;
        if (pn->maxCount > 0) md << " " << pn->runs << "/" << pn->maxCount;
        else if (pn->runs > 0) md << " (" << pn->runs << " runs)";
        if (!pn->detail.empty()) md << " - " << pn->detail;
        md << "\n";
    }
    md << "\n";
    if (!logsObj.empty()) {
        md << "## Included Logs\n";
        for (auto it = logsObj.begin(); it != logsObj.end(); ++it) {
            md << "### " << it.key() << "\n```\n" << it.value().get<std::string>() << "\n```\n\n";
        }
    }
    if (!filesObj.empty()) {
        md << "## Included Files\n";
        for (auto it = filesObj.begin(); it != filesObj.end(); ++it) {
            md << "### " << it.key() << "\n```\n" << it.value().get<std::string>() << "\n```\n\n";
        }
    }
    if (!allVarsObj.empty()) {
        md << "## Variables\n";
        for (auto it = allVarsObj.begin(); it != allVarsObj.end(); ++it) {
            md << "- `" << it.key() << "` = `" << it.value().get<std::string>() << "`\n";
        }
        md << "\n";
    }
    md << "## How to Answer\n";
    md << "Call `bt_resume` with `bt=\"" << pkt.btId << "\"`, `node=\"" << pkt.nodeKey
       << "\"`, `answer`, and a non-empty `rationale` (or write `{\"answer\":...,\"rationale\":...}` to `PC_BT_RESULT_FILE`).\n";
    pkt.markdown = md.str();

    node.currentPacket = std::move(pkt);
    bt.EmitEvent(nowMs, "nodeStarted", node.nodeKey, node.state, "", node.runs);
    bt.EmitEvent(nowMs, "escalated", node.nodeKey, "waitingForLlm", expandedQuestion, node.runs);
}

void BehaviorTree::Impl::LaunchEscalateFallback(BehaviorTree& bt, BtNode& node, int64_t nowMs) {
    node.fallbackLaunched = true;
    node.fallbackDeadlineMs = 0;
    if (!bt.runner_) return;

    std::string proposedId = bt.id_ + "-" + std::to_string(bt.nextJobSeq_++);
    BtWorkerSpec wspec;
    wspec.btId = bt.id_;
    wspec.nodeId = node.id;
    wspec.nodeKey = node.nodeKey;
    wspec.run = node.runs;
    wspec.proposedJobId = proposedId;
    wspec.engine = !node.fallback.engine.empty()
        ? node.fallback.engine
        : (!bt.workerDefaults_.engine.empty() ? bt.workerDefaults_.engine
                                              : (bt.catalog_ ? bt.catalog_->DefaultEngine() : "agy"));
    wspec.model = !node.fallback.model.empty()
        ? node.fallback.model
        : ((node.fallback.engine.empty() || node.fallback.engine == bt.workerDefaults_.engine)
               ? bt.workerDefaults_.model
               : "");
    wspec.prompt = node.currentPacket.markdown;
    wspec.summary = "Fallback escalation for " + node.nodeKey;
    wspec.cwd = bt.cwd_;
    wspec.timeoutSec = node.escalateTimeoutSec > 0 ? node.escalateTimeoutSec : 1800;
    wspec.outputSchema = R"({"type":"object","required":["answer","rationale"],"properties":{"answer":{"type":"string"},"rationale":{"type":"string"}}})";
    std::string jobId = bt.runner_->StartWorker(wspec);

    if (!jobId.empty()) {
        node.fallbackJobId = jobId;
        node.jobs.push_back(jobId);
        runningJobs[jobId] = RunningJobInfo{&node, true, true};
        bt.BumpVersion(nowMs);
    }
}

bool BehaviorTree::Impl::ApplyEscalateAnswer(BehaviorTree& bt,
                                             BtNode& node,
                                             const std::string& answer,
                                             const std::string& rationale,
                                             const std::map<std::string, std::string>& extraVars,
                                             int64_t nowMs,
                                             std::string& err) {
    // Check non-empty rationale (trim whitespace)
    bool rationaleNonEmpty = false;
    for (unsigned char c : rationale) {
        if (!std::isspace(c)) { rationaleNonEmpty = true; break; }
    }
    if (!rationaleNonEmpty) {
        err = "rationale must not be empty";
        return false;
    }
    if (answer.empty()) {
        err = "answer must not be empty";
        return false;
    }
    if (!node.expect.empty()) {
        if (std::find(node.expect.begin(), node.expect.end(), answer) == node.expect.end()) {
            err = "answer '" + answer + "' is not in allowed expect list";
            return false;
        }
    }

    // Cancel fallback job if still running
    if (!node.fallbackJobId.empty()) {
        std::string fbId = node.fallbackJobId;
        node.fallbackJobId.clear();
        runningJobs.erase(fbId);
        if (bt.runner_) bt.runner_->CancelJob(fbId);
    }

    node.escalateDeadlineMs = 0;
    node.fallbackDeadlineMs = 0;
    node.answer = answer;
    node.rationale = rationale;
    node.state = "succeeded";
    node.detail.clear();

    std::string ansVar = (!node.id.empty() ? node.id : node.nodeKey) + ".answer";
    bt.vars_[ansVar] = answer;
    bt.EmitEvent(nowMs, "varSet", node.nodeKey, node.state, "", node.runs, ansVar, answer);

    for (const auto& [k, v] : extraVars) {
        bt.vars_[k] = v;
        bt.EmitEvent(nowMs, "varSet", node.nodeKey, node.state, "", node.runs, k, v);
    }

    bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, "", node.runs);
    return true;
}

bool BehaviorTree::Resume(const std::string& nodeKey,
                          const std::string& answer,
                          const std::string& rationale,
                          const std::map<std::string, std::string>& vars,
                          int64_t nowMs,
                          std::string& err) {
    err.clear();
    if (!impl_ || !impl_->root) {
        err = "behavior tree is not loaded";
        return false;
    }
    BtNode* target = nullptr;
    if (nodeKey.empty()) {
        target = const_cast<BtNode*>(FindActiveEscalation(impl_->root.get(), "", id_));
    } else {
        target = impl_->FindNode(nodeKey, id_);
    }
    if (!target || target->type != "escalate") {
        err = "escalate node not found: " + nodeKey;
        return false;
    }
    if (target->state != "running") {
        err = "escalate node '" + target->nodeKey + "' is not waiting for an answer";
        return false;
    }

    if (!impl_->ApplyEscalateAnswer(*this, *target, answer, rationale, vars, nowMs, err)) {
        return false;
    }

    if (state_ == "waitingForLlm") state_ = "running";
    Tick(nowMs);
    return true;
}

bool BehaviorTree::GetEscalationPacket(const std::string& nodeKey, BtEscalationPacket& out) const {
    if (!impl_ || !impl_->root) return false;
    const BtNode* n = FindActiveEscalation(impl_->root.get(), nodeKey, id_);
    if (!n && !nodeKey.empty()) {
        n = impl_->FindNodeConst(nodeKey, id_);
    }
    if (!n || n->type != "escalate" || n->currentPacket.json.empty()) return false;
    out = n->currentPacket;
    return true;
}

BtEscalationPacket BehaviorTree::GetCurrentEscalationPacket() const {
    BtEscalationPacket out;
    GetEscalationPacket("", out);
    return out;
}

std::string BehaviorTree::GetEscalationPacketJson(const std::string& nodeKey) const {
    BtEscalationPacket out;
    return GetEscalationPacket(nodeKey, out) ? out.json : "";
}

std::string BehaviorTree::GetEscalationPacketMarkdown(const std::string& nodeKey) const {
    BtEscalationPacket out;
    return GetEscalationPacket(nodeKey, out) ? out.markdown : "";
}

} // namespace pc
