#include "behavior_tree_internal.h"
#include "common.h"

#include <algorithm>

namespace pc {

namespace {

json NodeToJson(const BtNode& node) {
    json j = node.rawDef;

    if (node.hasStatus || node.state != "idle" || node.runs > 0 || !node.jobs.empty()) {
        json st = json::object();
        st["state"] = node.state;
        if (node.runs > 0) st["runs"] = node.runs;
        if (!node.jobs.empty()) st["jobs"] = node.jobs;
        if (!node.detail.empty()) st["detail"] = node.detail;
        if (!node.answer.empty()) st["answer"] = node.answer;
        if (!node.rationale.empty()) st["rationale"] = node.rationale;
        j["status"] = std::move(st);
    } else {
        j.erase("status");
    }

    if (!node.children.empty()) {
        json kids = json::array();
        for (const auto& c : node.children) {
            kids.push_back(NodeToJson(*c));
        }
        j["children"] = std::move(kids);
    }
    if (node.condChild) j["cond"] = NodeToJson(*node.condChild);
    if (node.thenChild) j["then"] = NodeToJson(*node.thenChild);
    if (node.elseChild) j["else"] = NodeToJson(*node.elseChild);
    if (node.singleChild) j["child"] = NodeToJson(*node.singleChild);
    if (node.untilChild) j["until"] = NodeToJson(*node.untilChild);

    return j;
}

void UpdateNextJobSeqFromId(const std::string& btId, const std::string& jobId, int& nextJobSeq) {
    std::string prefix = btId + "-";
    if (jobId.rfind(prefix, 0) == 0) {
        std::string tail = jobId.substr(prefix.size());
        try {
            size_t used = 0;
            int n = std::stoi(tail, &used);
            if (used == tail.size() && n >= nextJobSeq) {
                nextJobSeq = n + 1;
            }
        } catch (...) {
        }
    }
}

} // namespace

void BehaviorTree::Impl::RestoreNodeStatus(BehaviorTree& bt,
                                           BtNode& node,
                                           const json& jNode,
                                           int64_t baseMs,
                                           int& nextJobSeq,
                                           std::map<std::string, std::string>& vars) {
    if (!jNode.is_object()) return;

    if (jNode.contains("status") && jNode["status"].is_object()) {
        const json& st = jNode["status"];
        node.hasStatus = true;
        node.state = st.value("state", "idle");
        node.runs = st.value("runs", 0);
        node.detail = st.value("detail", std::string());
        node.answer = st.value("answer", std::string());
        node.rationale = st.value("rationale", std::string());
        if (!node.answer.empty()) {
            std::string ansKey = (!node.id.empty() ? node.id : node.nodeKey) + ".answer";
            vars[ansKey] = node.answer;
        }
        if (st.contains("jobs") && st["jobs"].is_array()) {
            for (const auto& jId : st["jobs"]) {
                if (jId.is_string()) {
                    std::string s = jId.get<std::string>();
                    node.jobs.push_back(s);
                    UpdateNextJobSeqFromId(bt.Id(), s, nextJobSeq);
                }
            }
        }
    }

    if (!node.children.empty() && jNode.contains("children") && jNode["children"].is_array()) {
        size_t cnt = std::min(node.children.size(), jNode["children"].size());
        for (size_t i = 0; i < cnt; ++i) {
            RestoreNodeStatus(bt, *node.children[i], jNode["children"][i], baseMs, nextJobSeq, vars);
        }
    }
    if (node.condChild && jNode.contains("cond")) {
        RestoreNodeStatus(bt, *node.condChild, jNode["cond"], baseMs, nextJobSeq, vars);
    }
    if (node.thenChild && jNode.contains("then")) {
        RestoreNodeStatus(bt, *node.thenChild, jNode["then"], baseMs, nextJobSeq, vars);
    }
    if (node.elseChild && jNode.contains("else")) {
        RestoreNodeStatus(bt, *node.elseChild, jNode["else"], baseMs, nextJobSeq, vars);
    }
    if (node.singleChild && jNode.contains("child")) {
        RestoreNodeStatus(bt, *node.singleChild, jNode["child"], baseMs, nextJobSeq, vars);
    }
    if (node.untilChild && jNode.contains("until")) {
        RestoreNodeStatus(bt, *node.untilChild, jNode["until"], baseMs, nextJobSeq, vars);
    }

    if (node.state == "running") {
        if ((node.type == "job" || node.type == "worker") && !node.jobs.empty()) {
            node.activeJobId = node.jobs.back();
            node.isWorkerJob = (node.type == "worker");
            runningJobs[node.activeJobId] = RunningJobInfo{&node, false, node.isWorkerJob};
        } else if (node.type == "wait") {
            node.waitDeadlineMs = baseMs + static_cast<int64_t>(node.sec) * 1000;
        } else if (node.type == "timeout") {
            node.timeoutDeadlineMs = baseMs + static_cast<int64_t>(node.sec) * 1000;
        } else if (node.type == "if") {
            if (node.thenChild && node.thenChild->hasStatus) node.ifBranch = 1;
            else if (node.elseChild && node.elseChild->hasStatus) node.ifBranch = 2;
        } else if (node.type == "escalate") {
            node.escalateDeadlineMs = baseMs + static_cast<int64_t>(node.escalateTimeoutSec) * 1000;
            if (!node.jobs.empty()) {
                node.fallbackJobId = node.jobs.back();
                node.fallbackLaunched = true;
                runningJobs[node.fallbackJobId] = RunningJobInfo{&node, true, true};
            } else if (node.fallback.enabled && node.fallback.afterSec > 0) {
                node.fallbackDeadlineMs = baseMs + static_cast<int64_t>(node.fallback.afterSec) * 1000;
            }
            // Populate packet so GetEscalationPacket works after reload
            BtEscalationPacket pkt;
            pkt.btId = bt.Id();
            pkt.nodeKey = node.nodeKey;
            pkt.run = node.runs > 0 ? node.runs : 1;
            pkt.intent = bt.Intent();
            pkt.question = node.question;
            pkt.guidance = node.guidance;
            pkt.expect = node.expect;
            json pj;
            pj["btId"] = pkt.btId;
            pj["node"] = pkt.nodeKey;
            pj["run"] = pkt.run;
            pj["intent"] = pkt.intent;
            pj["question"] = pkt.question;
            pj["guidance"] = pkt.guidance;
            pj["expect"] = pkt.expect;
            pkt.json = pj.dump(2, ' ', false, json::error_handler_t::replace);
            pkt.markdown = "# Escalation Decision Packet: " + pkt.btId + " / " + pkt.nodeKey + "\n\n## Question\n" + pkt.question + "\n";
            node.currentPacket = std::move(pkt);
        }
    }
}

std::string BehaviorTree::ToStateJson() const {
    if (!impl_ || !impl_->root) return "{}";

    json out = impl_->rawRootDoc;
    out["id"] = id_;
    if (!summary_.empty()) out["summary"] = summary_;
    if (!intent_.empty()) out["intent"] = intent_;
    if (!taskqueue_.empty()) out["taskqueue"] = taskqueue_;
    if (!cwd_.empty()) out["cwd"] = cwd_;
    out["state"] = state_;
    if (!detail_.empty()) out["detail"] = detail_;
    else out.erase("detail");
    out["nodeRuns"] = nodeRuns_;
    out["limits"] = {
        {"maxNodeRuns", limits_.maxNodeRuns},
        {"maxWallSec", limits_.maxWallSec}
    };
    if (!workerDefaults_.engine.empty() || !workerDefaults_.model.empty()) {
        json w = json::object();
        if (!workerDefaults_.engine.empty()) w["engine"] = workerDefaults_.engine;
        if (!workerDefaults_.model.empty()) w["model"] = workerDefaults_.model;
        out["worker"] = std::move(w);
    }

    json varsJson = json::object();
    for (const auto& [k, v] : vars_) {
        varsJson[k] = v;
    }
    out["vars"] = std::move(varsJson);

    out["createdAt"] = createdAt_.empty() ? "1970-01-01T00:00:00" : createdAt_;
    out["updatedAt"] = updatedAt_.empty() ? out["createdAt"].get<std::string>() : updatedAt_;
    out["root"] = NodeToJson(*impl_->root);

    return out.dump(2, ' ', false, json::error_handler_t::replace);
}

bool BehaviorTree::LoadFromStateJson(const std::string& jsonText,
                                     const IBtEngineCatalog* catalog,
                                     int64_t nowMs,
                                     std::string& err) {
    if (!LoadDefinition(jsonText, catalog, err)) return false;

    json doc = json::parse(jsonText, nullptr, false);
    state_ = doc.value("state", "idle");
    detail_ = doc.value("detail", std::string());
    nodeRuns_ = doc.value("nodeRuns", 0);
    createdAt_ = doc.value("createdAt", std::string());
    updatedAt_ = doc.value("updatedAt", std::string());
    int64_t baseMs = nowMs > 0 ? nowMs : 1000;
    startMs_ = baseMs;

    if (doc.contains("vars") && doc["vars"].is_object()) {
        vars_.clear();
        for (auto it = doc["vars"].begin(); it != doc["vars"].end(); ++it) {
            if (it.value().is_string()) vars_[it.key()] = it.value().get<std::string>();
            else vars_[it.key()] = it.value().dump();
        }
    }

    if (doc.contains("root") && doc["root"].is_object() && impl_ && impl_->root) {
        impl_->RestoreNodeStatus(*this, *impl_->root, doc["root"], baseMs, nextJobSeq_, vars_);
    }

    version_ = 1;
    return true;
}

bool BehaviorTree::LoadFromStateJson(const std::string& jsonText,
                                     const IBtEngineCatalog* catalog,
                                     std::string& err) {
    return LoadFromStateJson(jsonText, catalog, 1000, err);
}

namespace {

void CollectOutsideTarget(const BtNode& cur,
                          const BtNode* target,
                          std::vector<std::string>& outIds,
                          int& outCount) {
    if (&cur == target) return;
    outCount++;
    if (!cur.id.empty()) outIds.push_back(cur.id);
    for (const auto& c : cur.children) {
        if (c) CollectOutsideTarget(*c, target, outIds, outCount);
    }
    if (cur.condChild) CollectOutsideTarget(*cur.condChild, target, outIds, outCount);
    if (cur.thenChild) CollectOutsideTarget(*cur.thenChild, target, outIds, outCount);
    if (cur.elseChild) CollectOutsideTarget(*cur.elseChild, target, outIds, outCount);
    if (cur.singleChild) CollectOutsideTarget(*cur.singleChild, target, outIds, outCount);
    if (cur.untilChild) CollectOutsideTarget(*cur.untilChild, target, outIds, outCount);
}

bool FindJsonNodeByIdOrPath(const json& cur,
                            const std::string& curPath,
                            const std::string& targetId,
                            const std::string& targetPath,
                            json& outJson) {
    if (!cur.is_object()) return false;
    std::string id = cur.value("id", "");
    if ((!targetId.empty() && id == targetId) || curPath == targetPath) {
        outJson = cur;
        return true;
    }
    if (cur.contains("children") && cur["children"].is_array()) {
        for (size_t i = 0; i < cur["children"].size(); ++i) {
            if (FindJsonNodeByIdOrPath(cur["children"][i],
                                       curPath + ".children[" + std::to_string(i) + "]",
                                       targetId, targetPath, outJson)) {
                return true;
            }
        }
    }
    for (const char* slot : {"cond", "then", "else", "child", "until"}) {
        if (cur.contains(slot) && cur[slot].is_object()) {
            if (FindJsonNodeByIdOrPath(cur[slot], curPath + "." + slot, targetId, targetPath, outJson)) {
                return true;
            }
        }
    }
    return false;
}

bool CheckJsonShapeMatchesNode(const BtNode& oldNode,
                               const json& jNew,
                               bool isExplicitTargetRoot,
                               std::string& err) {
    if (!jNew.is_object()) {
        err = oldNode.pathKey + ": node must be a JSON object";
        return false;
    }
    std::string rawType = jNew.value("type", "");
    std::string normType = (rawType == "selector") ? "fallback" : rawType;
    if (normType != oldNode.type) {
        err = oldNode.pathKey + ": cannot change node type from '" + oldNode.type + "' to '" + rawType + "'";
        return false;
    }
    std::string newId = jNew.value("id", "");
    if (isExplicitTargetRoot && ! jNew.contains("id")) {
        newId = oldNode.id;
    }
    if (newId != oldNode.id) {
        err = oldNode.pathKey + ": cannot change node id from '" + oldNode.id + "' to '" + newId + "'";
        return false;
    }

    if (!oldNode.children.empty() || jNew.contains("children")) {
        if (!jNew.contains("children") || !jNew["children"].is_array() ||
            jNew["children"].size() != oldNode.children.size()) {
            err = oldNode.pathKey + ": cannot change children count (tree shape must remain identical)";
            return false;
        }
        for (size_t i = 0; i < oldNode.children.size(); ++i) {
            if (!CheckJsonShapeMatchesNode(*oldNode.children[i], jNew["children"][i], false, err)) {
                return false;
            }
        }
    }

    auto checkSlot = [&](const char* slot, const std::unique_ptr<BtNode>& oldSlot) -> bool {
        bool hasOld = (oldSlot != nullptr);
        bool hasNew = (jNew.contains(slot) && jNew[slot].is_object());
        if (hasOld != hasNew) {
            err = oldNode.pathKey + ": cannot add or remove '" + slot + "' child (tree shape must remain identical)";
            return false;
        }
        if (hasOld) {
            return CheckJsonShapeMatchesNode(*oldSlot, jNew[slot], false, err);
        }
        return true;
    };

    if (!checkSlot("cond", oldNode.condChild)) return false;
    if (!checkSlot("then", oldNode.thenChild)) return false;
    if (!checkSlot("else", oldNode.elseChild)) return false;
    if (!checkSlot("child", oldNode.singleChild)) return false;
    if (!checkSlot("until", oldNode.untilChild)) return false;
    return true;
}

json StripChildrenAndStatus(json j) {
    if (!j.is_object()) return j;
    j.erase("status");
    j.erase("children");
    for (const char* slot : {"cond", "then", "else", "child", "until"}) {
        j.erase(slot);
    }
    return j;
}

bool CheckNotRunningForUpdate(const BtNode& oldNode,
                              const BtNode& newNode,
                              bool rejectAnyRunningInSubtree,
                              bool& outAnyChanged,
                              std::string& err) {
    bool selfChanged = (StripChildrenAndStatus(oldNode.rawDef) != StripChildrenAndStatus(newNode.rawDef));
    if (selfChanged) outAnyChanged = true;

    if (oldNode.state == "running" && (rejectAnyRunningInSubtree || selfChanged)) {
        err = "cannot update running node: " + oldNode.nodeKey;
        return false;
    }

    for (size_t i = 0; i < oldNode.children.size(); ++i) {
        if (!CheckNotRunningForUpdate(*oldNode.children[i], *newNode.children[i],
                                      rejectAnyRunningInSubtree, outAnyChanged, err)) {
            return false;
        }
    }
    if (oldNode.condChild &&
        !CheckNotRunningForUpdate(*oldNode.condChild, *newNode.condChild,
                                  rejectAnyRunningInSubtree, outAnyChanged, err)) return false;
    if (oldNode.thenChild &&
        !CheckNotRunningForUpdate(*oldNode.thenChild, *newNode.thenChild,
                                  rejectAnyRunningInSubtree, outAnyChanged, err)) return false;
    if (oldNode.elseChild &&
        !CheckNotRunningForUpdate(*oldNode.elseChild, *newNode.elseChild,
                                  rejectAnyRunningInSubtree, outAnyChanged, err)) return false;
    if (oldNode.singleChild &&
        !CheckNotRunningForUpdate(*oldNode.singleChild, *newNode.singleChild,
                                  rejectAnyRunningInSubtree, outAnyChanged, err)) return false;
    if (oldNode.untilChild &&
        !CheckNotRunningForUpdate(*oldNode.untilChild, *newNode.untilChild,
                                  rejectAnyRunningInSubtree, outAnyChanged, err)) return false;
    return true;
}

void CopyNodeDefinitionRecursive(BtNode& dst, const BtNode& src) {
    dst.rawDef = src.rawDef;
    dst.parallelSuccessMode = src.parallelSuccessMode;
    dst.parallelSuccessCount = src.parallelSuccessCount;
    dst.maxCount = src.maxCount;
    dst.delaySec = src.delaySec;
    dst.sec = src.sec;

    dst.summary = src.summary;
    dst.command = src.command;
    dst.cwd = src.cwd;
    dst.timeoutSec = src.timeoutSec;
    dst.env = src.env;
    dst.success = src.success;
    dst.idle = src.idle;
    dst.resources = src.resources;
    dst.instructionsText = src.instructionsText;
    dst.instructionsFile = src.instructionsFile;
    dst.maxAttempts = src.maxAttempts;
    dst.engine = src.engine;
    dst.model = src.model;
    dst.prompt = src.prompt;
    dst.promptFile = src.promptFile;
    dst.outputSchema = src.outputSchema;
    dst.capture = src.capture;

    dst.setVar = src.setVar;
    dst.setValue = src.setValue;
    dst.failDetail = src.failDetail;

    dst.question = src.question;
    dst.guidance = src.guidance;
    dst.expect = src.expect;
    dst.include = src.include;
    dst.escalateTimeoutSec = src.escalateTimeoutSec;
    dst.fallback = src.fallback;

    dst.condFile = src.condFile;
    dst.condPath = src.condPath;
    dst.condThan = src.condThan;
    dst.condNode = src.condNode;
    dst.condRegex = src.condRegex;
    dst.condLines = src.condLines;
    dst.condVarName = src.condVarName;
    dst.hasEq = src.hasEq;
    dst.condEq = src.condEq;
    dst.hasNe = src.hasNe;
    dst.condNe = src.condNe;
    dst.hasIn = src.hasIn;
    dst.condIn = src.condIn;
    dst.hasGt = src.hasGt;
    dst.condGt = src.condGt;
    dst.hasLt = src.hasLt;
    dst.condLt = src.condLt;
    dst.hasExists = src.hasExists;
    dst.condExists = src.condExists;

    for (size_t i = 0; i < dst.children.size() && i < src.children.size(); ++i) {
        CopyNodeDefinitionRecursive(*dst.children[i], *src.children[i]);
    }
    if (dst.condChild && src.condChild) CopyNodeDefinitionRecursive(*dst.condChild, *src.condChild);
    if (dst.thenChild && src.thenChild) CopyNodeDefinitionRecursive(*dst.thenChild, *src.thenChild);
    if (dst.elseChild && src.elseChild) CopyNodeDefinitionRecursive(*dst.elseChild, *src.elseChild);
    if (dst.singleChild && src.singleChild) CopyNodeDefinitionRecursive(*dst.singleChild, *src.singleChild);
    if (dst.untilChild && src.untilChild) CopyNodeDefinitionRecursive(*dst.untilChild, *src.untilChild);
}

} // namespace

bool BehaviorTree::UpdateDefinition(const std::string& jsonText,
                                    const std::string& targetNodeKey,
                                    int64_t nowMs,
                                    std::string& err) {
    if (!impl_ || !impl_->root) {
        err = "behavior tree is not initialized";
        return false;
    }

    json doc = json::parse(jsonText, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        err = "invalid JSON object";
        return false;
    }

    BtNode* target = nullptr;
    bool explicitNodeTarget = !targetNodeKey.empty();
    if (!explicitNodeTarget) {
        target = impl_->root.get();
    } else {
        target = impl_->FindNode(targetNodeKey, id_);
        if (!target) {
            err = "node not found: " + targetNodeKey;
            return false;
        }
    }

    json nodeJson;
    if (!explicitNodeTarget) {
        if (doc.contains("root") && doc["root"].is_object()) {
            if (doc.contains("id") && (!doc["id"].is_string() || doc["id"].get<std::string>() != id_)) {
                err = "cannot change behavior tree id";
                return false;
            }
            nodeJson = doc["root"];
        } else {
            nodeJson = doc;
        }
    } else {
        if (doc.contains("root") && doc["root"].is_object() && !doc.contains("type")) {
            if (target == impl_->root.get()) {
                nodeJson = doc["root"];
            } else if (!FindJsonNodeByIdOrPath(doc["root"], "root", target->id, target->pathKey, nodeJson)) {
                err = "node '" + targetNodeKey + "' not found in provided tree JSON";
                return false;
            }
        } else if (!doc.contains("type")) {
            nodeJson = target->rawDef;
            for (auto it = doc.begin(); it != doc.end(); ++it) {
                nodeJson[it.key()] = it.value();
            }
        } else {
            nodeJson = doc;
            if (!nodeJson.contains("id") && !target->id.empty()) {
                nodeJson["id"] = target->id;
            }
        }
    }

    if (!CheckJsonShapeMatchesNode(*target, nodeJson, explicitNodeTarget, err)) {
        return false;
    }

    std::vector<std::string> outsideIds;
    int outsideCount = 0;
    CollectOutsideTarget(*impl_->root, target, outsideIds, outsideCount);

    auto parsedNode = ParseSingleNodeJson(
        nodeJson, target->pathKey, target->role, target->depth,
        catalog_, workerDefaults_, cwd_, outsideIds, outsideCount, err);
    if (!parsedNode) {
        return false;
    }

    bool anyChanged = false;
    if (!CheckNotRunningForUpdate(*target, *parsedNode, explicitNodeTarget, anyChanged, err)) {
        return false;
    }
    if (!explicitNodeTarget && !anyChanged && target->state == "running") {
        err = "cannot update running node: " + target->nodeKey;
        return false;
    }

    CopyNodeDefinitionRecursive(*target, *parsedNode);
    BumpVersion(nowMs);
    return true;
}

bool BehaviorTree::UpdateNode(const std::string& nodeKey,
                              const std::string& nodeJsonText,
                              int64_t nowMs,
                              std::string& err) {
    return UpdateDefinition(nodeJsonText, nodeKey, nowMs, err);
}

bool BehaviorTree::UpdateDefinition(const std::string& jsonText,
                                    int64_t nowMs,
                                    std::string& err) {
    return UpdateDefinition(jsonText, "", nowMs, err);
}

} // namespace pc
