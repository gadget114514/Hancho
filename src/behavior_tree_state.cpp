#include "behavior_tree_internal.h"
#include "common.h"

#include <algorithm>

namespace pc {

namespace {

json NodeToJson(const BtNode& node, bool forceQueue = false) {
    json j = node.rawDef;
    if (node.type == "exec" || node.type == "agent") {
        j["type"] = node.type;
    }

    if (node.hasStatus || node.state != "idle" || node.runs > 0 || node.launchSeq > 0 || !node.jobs.empty()) {
        json st = json::object();
        st["state"] = node.state;
        if (node.runs > 0) st["runs"] = node.runs;
        if (node.launchSeq > 0) st["launchSeq"] = node.launchSeq;
        if (!node.jobs.empty()) st["jobs"] = node.jobs;
        if (!node.detail.empty()) st["detail"] = node.detail;
        if (!node.answer.empty()) st["answer"] = node.answer;
        if (!node.rationale.empty()) st["rationale"] = node.rationale;
        if (node.hasFailureDiagnostic) st["failure"] = node.failureDiagnostic;
        if (!node.lastEngine.empty()) st["engine"] = node.lastEngine;
        if (!node.lastModel.empty()) st["model"] = node.lastModel;
        if (!node.engineCandidates.empty() || !node.engineAttempts.empty()) {
            if (!node.engineCandidates.empty()) {
                st["engineCandidateIndex"] = node.engineCandidateIndex;
            }
            json attempts = json::array();
            for (const auto& a : node.engineAttempts) {
                json aj{{"engine", a.engine}, {"model", a.model},
                        {"result", a.result}, {"failureClass", a.failureClass},
                        {"detail", a.detail}, {"list", a.list}, {"entry", a.entry}};
                if (a.handoff.is_object() && !a.handoff.empty()) {
                    aj["handoff"] = a.handoff;
                }
                if (!a.failSignature.empty()) {
                    aj["failSignature"] = a.failSignature;
                }
                attempts.push_back(std::move(aj));
            }
            st["engineAttempts"] = std::move(attempts);
        }
        if (!node.switchFailCounts.empty()) {
            st["switchFailCounts"] = node.switchFailCounts;
        }
        if (node.lastHandoff.is_object() && !node.lastHandoff.empty()) {
            st["lastHandoff"] = node.lastHandoff;
        }
        if (!node.recentFailSignatures.empty()) {
            st["recentFailSignatures"] = node.recentFailSignatures;
        }
        if (node.firstStartMs > 0) {
            st["firstStartMs"] = node.firstStartMs;
        }
        if (!node.switchStoppedReason.empty()) {
            st["switchStoppedReason"] = node.switchStoppedReason;
        }
        if (node.waitingForEngine) {
            st["waitingForEngine"] = true;
            st["engineWaitUntilMs"] = node.engineWaitUntilMs;
        }
        if (node.takenOver) {
            st["takenOver"] = true;
            st["takeover"] = node.takeover.is_object() ? node.takeover : json::object();
        }
        if (node.type == "catch") {
            st["catchPhase"] = node.catchPhase;
            st["catchRetries"] = node.catchRetries;
            st["catchHandlerIndex"] = node.catchHandlerIndex;
            if (!node.catchHandler.empty()) st["catchHandler"] = node.catchHandler;
        }
        if (node.type == "foreach" && node.foreachExpanded) st["foreachExpanded"] = true;
        if (forceQueue && !node.queueName.empty()) st["queueName"] = node.queueName;
        j["status"] = std::move(st);
    } else {
        j.erase("status");
    }

    if (!node.children.empty()) {
        json kids = json::array();
        for (const auto& c : node.children) {
            kids.push_back(NodeToJson(*c, node.type == "foreach"));
        }
        j["children"] = std::move(kids);
    }
    if (node.condChild) j["cond"] = NodeToJson(*node.condChild);
    if (node.thenChild) j["then"] = NodeToJson(*node.thenChild);
    if (node.elseChild) j["else"] = NodeToJson(*node.elseChild);
    if (node.singleChild) j["child"] = NodeToJson(*node.singleChild);
    if (node.untilChild) j["until"] = NodeToJson(*node.untilChild);
    if (!node.catchHandlers.empty() && j.contains("handlers") && j["handlers"].is_array()) {
        json handlers = json::array();
        for (size_t i = 0; i < node.catchHandlers.size() && i < j["handlers"].size(); ++i) {
            const BtCatchHandler& h = node.catchHandlers[i];
            json hj = h.rawDef;
            if (hj.is_object() && h.doNode) hj["do"] = NodeToJson(*h.doNode);
            handlers.push_back(std::move(hj));
        }
        j["handlers"] = std::move(handlers);
    }
    if (node.otherwiseChild && j.contains("otherwise") && j["otherwise"].is_object()) {
        j["otherwise"] = NodeToJson(*node.otherwiseChild);
    }

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

json IdleToJson(const BtJobIdle& idle) {
    return {
        {"outputSec", idle.outputSec},
        {"cpuSec", idle.cpuSec},
        {"firstEventSec", idle.firstEventSec}
    };
}

json DefaultsToJson(const BtDefaults& d) {
    json out = json::object();
    json worker = json::object();
    if (d.hasWorkerIdle()) worker["idle"] = IdleToJson(d.workerIdle);
    if (d.workerMaxAttempts > 0) worker["maxAttempts"] = d.workerMaxAttempts;
    if (d.hasWorkerProbe) worker["probe"] = d.workerProbe;
    if (d.workerSwitchLimits.configured) {
        worker["switchLimits"] = {
            {"maxCandidates", d.workerSwitchLimits.maxCandidates},
            {"maxSec", d.workerSwitchLimits.maxSec},
            {"stopOnSameSymptom", d.workerSwitchLimits.stopOnSameSymptom}
        };
    }
    if (!worker.empty()) out["agent"] = std::move(worker);
    if (d.hasJobIdle()) out["exec"] = {{"idle", IdleToJson(d.jobIdle)}};
    return out;
}

} // namespace

void BehaviorTree::Impl::RestoreNodeStatus(BehaviorTree& bt,
                                           BtNode& node,
                                           const json& jNode,
                                           int64_t baseMs,
                                           int& nextJobSeq,
                                           std::map<std::string, std::string>& vars) {
    if (!jNode.is_object()) return;
    bool foreachExpanded = false;

    if (jNode.contains("status") && jNode["status"].is_object()) {
        const json& st = jNode["status"];
        foreachExpanded = st.value("foreachExpanded", false);
        node.hasStatus = true;
        node.state = st.value("state", "idle");
        node.runs = st.value("runs", 0);
        // Older saves have no launchSeq; fall back to runs so new IDs still don't collide with them.
        node.launchSeq = st.value("launchSeq", node.runs);
        node.detail = st.value("detail", std::string());
        node.answer = st.value("answer", std::string());
        node.rationale = st.value("rationale", std::string());
        if (st.contains("failure") && st["failure"].is_object()) {
            node.hasFailureDiagnostic = true;
            node.failureDiagnostic = st["failure"];
        }
        node.lastEngine = st.value("engine", std::string());
        node.lastModel = st.value("model", std::string());
        node.engineCandidateIndex = st.value("engineCandidateIndex", 0);
        if (st.contains("switchFailCounts") && st["switchFailCounts"].is_object()) {
            for (auto it = st["switchFailCounts"].begin(); it != st["switchFailCounts"].end(); ++it) {
                if (it.value().is_number_integer()) {
                    node.switchFailCounts[it.key()] = it.value().get<int>();
                }
            }
        }
        node.waitingForEngine = st.value("waitingForEngine", false);
        node.engineWaitUntilMs = st.value("engineWaitUntilMs", static_cast<int64_t>(0));
        if (st.contains("engineAttempts") && st["engineAttempts"].is_array()) {
            for (const auto& a : st["engineAttempts"]) {
                if (!a.is_object()) continue;
                BtEngineAttempt att;
                att.engine = a.value("engine", std::string());
                att.model = a.value("model", std::string());
                att.result = a.value("result", std::string());
                att.failureClass = a.value("failureClass", std::string());
                att.detail = a.value("detail", std::string());
                att.list = a.value("list", std::string());
                att.entry = a.value("entry", std::string());
                if (a.contains("handoff") && a["handoff"].is_object()) {
                    att.handoff = a["handoff"];
                }
                if (a.contains("failSignature") && a["failSignature"].is_array()) {
                    for (const auto& sig : a["failSignature"]) {
                        if (sig.is_string()) att.failSignature.push_back(sig.get<std::string>());
                    }
                }
                node.engineAttempts.push_back(std::move(att));
            }
        }
        if (st.contains("lastHandoff") && st["lastHandoff"].is_object()) {
            node.lastHandoff = st["lastHandoff"];
        }
        if (st.contains("recentFailSignatures") && st["recentFailSignatures"].is_array()) {
            for (const auto& sig : st["recentFailSignatures"]) {
                if (!sig.is_array()) continue;
                std::vector<std::string> vec;
                for (const auto& item : sig) {
                    if (item.is_string()) vec.push_back(item.get<std::string>());
                }
                node.recentFailSignatures.push_back(std::move(vec));
            }
        }
        node.firstStartMs = st.value("firstStartMs", static_cast<int64_t>(0));
        node.switchStoppedReason = st.value("switchStoppedReason", std::string());
        node.takenOver = st.value("takenOver", false);
        if (st.contains("takeover") && st["takeover"].is_object()) node.takeover = st["takeover"];
        if (node.type == "catch") {
            node.catchPhase = st.value("catchPhase", 0);
            node.catchRetries = st.value("catchRetries", 0);
            node.catchHandlerIndex = st.value("catchHandlerIndex", -1);
            node.catchHandler = st.value("catchHandler", std::string());
        }
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
    // A foreach node parses from its authored definition without children; rebuild the
    // concrete children from the saved expanded state so bt get / retry keep working.
    if (node.type == "foreach" && foreachExpanded && jNode.contains("children") &&
        jNode["children"].is_array()) {
        node.foreachExpanded = true;
        node.children.clear();
        size_t i = 0;
        for (const auto& childJson : jNode["children"]) {
            std::string childPath = node.pathKey + ".children[" + std::to_string(i) + "]";
            std::string err;
            auto child = ParseSingleNodeJson(childJson, childPath, "", node.depth + 1, nullptr,
                                             bt.defaults_, bt.cwd_, {}, 0, bt.taskqueue_,
                                             node.queueName, err);
            if (child) {
                child->isForeachChild = true;
                if (childJson.is_object() && childJson.contains("status") &&
                    childJson["status"].is_object()) {
                    const json& cs = childJson["status"];
                    if (cs.contains("queueName") && cs["queueName"].is_string()) {
                        child->queueName = cs["queueName"].get<std::string>();
                    }
                }
                RestoreNodeStatus(bt, *child, childJson, baseMs, nextJobSeq, vars);
                node.children.push_back(std::move(child));
            }
            ++i;
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
    if (node.otherwiseChild && jNode.contains("otherwise") && jNode["otherwise"].is_object()) {
        RestoreNodeStatus(bt, *node.otherwiseChild, jNode["otherwise"], baseMs, nextJobSeq, vars);
    }
    if (jNode.contains("handlers") && jNode["handlers"].is_array()) {
        for (size_t i = 0; i < node.catchHandlers.size() && i < jNode["handlers"].size(); ++i) {
            const BtCatchHandler& h = node.catchHandlers[i];
            const json& hj = jNode["handlers"][i];
            if (h.doNode && hj.is_object() && hj.contains("do")) {
                RestoreNodeStatus(bt, *h.doNode, hj["do"], baseMs, nextJobSeq, vars);
            }
        }
    }

    if (node.state == "running") {
        if ((node.type == "exec" || node.type == "agent") && !node.jobs.empty()) {
            node.activeJobId = node.jobs.back();
            node.isWorkerJob = (node.type == "agent");
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
    if (!taskId_.empty()) out["taskId"] = taskId_;
    if (!directionId_.empty()) out["directionId"] = directionId_;
    if (!supersedes_.empty()) out["supersedes"] = supersedes_;
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
    json defaults = DefaultsToJson(defaults_);
    if (!defaults.empty()) out["defaults"] = std::move(defaults);

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
    if (cur.otherwiseChild) CollectOutsideTarget(*cur.otherwiseChild, target, outIds, outCount);
    for (const auto& h : cur.catchHandlers) {
        if (h.doNode) CollectOutsideTarget(*h.doNode, target, outIds, outCount);
    }
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
    for (const char* slot : {"cond", "then", "else", "child", "until", "otherwise"}) {
        if (cur.contains(slot) && cur[slot].is_object()) {
            if (FindJsonNodeByIdOrPath(cur[slot], curPath + "." + slot, targetId, targetPath, outJson)) {
                return true;
            }
        }
    }
    if (cur.contains("handlers") && cur["handlers"].is_array()) {
        size_t i = 0;
        for (const auto& h : cur["handlers"]) {
            if (h.is_object() && h.contains("do") && h["do"].is_object()) {
                if (FindJsonNodeByIdOrPath(h["do"], curPath + ".handlers[" + std::to_string(i) + "].do",
                                           targetId, targetPath, outJson)) return true;
            }
            ++i;
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
    std::string normType = rawType;
    if (rawType == "selector") normType = "fallback";
    else if (rawType == "job") normType = "exec";
    else if (rawType == "worker") normType = "agent";
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

    // A foreach node's children are runtime-expanded copies, not authored shape: updates may
    // change the glob/do definition and the children are rebuilt on the next expansion.
    if (oldNode.type != "foreach" &&
        (!oldNode.children.empty() || jNew.contains("children"))) {
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
    if (oldNode.type == "catch") {
        if (!checkSlot("otherwise", oldNode.otherwiseChild)) return false;
        bool hasHandlers = jNew.contains("handlers") && jNew["handlers"].is_array();
        if (!oldNode.catchHandlers.empty() || hasHandlers) {
            if (!hasHandlers || jNew["handlers"].size() != oldNode.catchHandlers.size()) {
                err = oldNode.pathKey + ": cannot change handlers count (tree shape must remain identical)";
                return false;
            }
            for (size_t i = 0; i < oldNode.catchHandlers.size(); ++i) {
                const BtCatchHandler& oldH = oldNode.catchHandlers[i];
                const json& jh = jNew["handlers"][i];
                bool hasDo = jh.is_object() && jh.contains("do") && jh["do"].is_object();
                if (static_cast<bool>(oldH.doNode) != hasDo) {
                    err = oldNode.pathKey + ": cannot add or remove a handler's 'do' child";
                    return false;
                }
                if (hasDo && !CheckJsonShapeMatchesNode(*oldH.doNode, jh["do"], false, err)) return false;
            }
        }
    }
    return true;
}

json StripChildrenAndStatus(json j) {
    if (!j.is_object()) return j;
    j.erase("status");
    j.erase("children");
    for (const char* slot : {"cond", "then", "else", "child", "until", "otherwise", "handlers"}) {
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
    if (oldNode.type == "catch" && newNode.type == "catch") {
        auto handlerMeta = [](const BtNode& n) {
            json arr = json::array();
            for (const auto& h : n.catchHandlers) {
                json hj = json::object();
                if (!h.classes.empty()) hj["class"] = h.classes;
                if (!h.cause.empty()) hj["cause"] = h.cause;
                if (h.thenRetry) hj["thenRetry"] = true;
                arr.push_back(std::move(hj));
            }
            return arr;
        };
        if (handlerMeta(oldNode) != handlerMeta(newNode)) selfChanged = true;
    }
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
    if (oldNode.otherwiseChild && newNode.otherwiseChild &&
        !CheckNotRunningForUpdate(*oldNode.otherwiseChild, *newNode.otherwiseChild,
                                  rejectAnyRunningInSubtree, outAnyChanged, err)) return false;
    for (size_t i = 0; i < oldNode.catchHandlers.size() && i < newNode.catchHandlers.size(); ++i) {
        const auto& oh = oldNode.catchHandlers[i];
        const auto& nh = newNode.catchHandlers[i];
        if (oh.doNode && nh.doNode &&
            !CheckNotRunningForUpdate(*oh.doNode, *nh.doNode,
                                      rejectAnyRunningInSubtree, outAnyChanged, err)) return false;
    }
    return true;
}

void CopyNodeDefinitionRecursive(BtNode& dst, const BtNode& src) {
    dst.rawDef = src.rawDef;
    dst.queueName = src.queueName;
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
    dst.maxAttemptsConfigured = src.maxAttemptsConfigured;
    dst.probe = src.probe;
    dst.probeConfigured = src.probeConfigured;
    dst.engine = src.engine;
    dst.model = src.model;
    dst.engineCandidates = src.engineCandidates;
    dst.switchEngineOn = src.switchEngineOn;
    dst.switchLimits = src.switchLimits;
    dst.switchFailCounts = src.switchFailCounts;
    dst.engineCandidateIndex = src.engineCandidateIndex;
    dst.engineAttempts = src.engineAttempts;
    dst.lastHandoff = src.lastHandoff;
    dst.onSwitch = src.onSwitch;
    dst.blame = src.blame;
    dst.failLineRegex = src.failLineRegex;
    dst.recentFailSignatures = src.recentFailSignatures;
    dst.firstStartMs = src.firstStartMs;
    dst.switchStoppedReason = src.switchStoppedReason;
    dst.probeTimeoutSec = src.probeTimeoutSec;
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
    dst.condFailureClasses = src.condFailureClasses;
    dst.condFailureCause = src.condFailureCause;

    dst.foreachGlob = src.foreachGlob;
    dst.foreachAs = src.foreachAs;
    dst.foreachMode = src.foreachMode;
    dst.foreachExclude = src.foreachExclude;
    dst.foreachMinMatches = src.foreachMinMatches;
    dst.foreachExpanded = false;
    dst.children.clear();
    if (dst.type == "foreach" && src.rawDef.contains("do")) {
        std::string err;
        dst.doChild = ParseSingleNodeJson(src.rawDef["do"], dst.pathKey + ".do", "do", dst.depth + 1,
                                          nullptr, BtDefaults{}, std::string(), {}, 0,
                                          std::string(), dst.queueName, err);
    }

    dst.catchMaxRetries = src.catchMaxRetries;
    for (size_t i = 0; i < dst.catchHandlers.size() && i < src.catchHandlers.size(); ++i) {
        BtCatchHandler& dh = dst.catchHandlers[i];
        const BtCatchHandler& sh = src.catchHandlers[i];
        dh.rawDef = sh.rawDef;
        dh.classes = sh.classes;
        dh.cause = sh.cause;
        dh.thenRetry = sh.thenRetry;
        if (dh.doNode && sh.doNode) CopyNodeDefinitionRecursive(*dh.doNode, *sh.doNode);
    }

    for (size_t i = 0; i < dst.children.size() && i < src.children.size(); ++i) {
        CopyNodeDefinitionRecursive(*dst.children[i], *src.children[i]);
    }
    if (dst.condChild && src.condChild) CopyNodeDefinitionRecursive(*dst.condChild, *src.condChild);
    if (dst.thenChild && src.thenChild) CopyNodeDefinitionRecursive(*dst.thenChild, *src.thenChild);
    if (dst.elseChild && src.elseChild) CopyNodeDefinitionRecursive(*dst.elseChild, *src.elseChild);
    if (dst.singleChild && src.singleChild) CopyNodeDefinitionRecursive(*dst.singleChild, *src.singleChild);
    if (dst.untilChild && src.untilChild) CopyNodeDefinitionRecursive(*dst.untilChild, *src.untilChild);
    if (dst.otherwiseChild && src.otherwiseChild) {
        CopyNodeDefinitionRecursive(*dst.otherwiseChild, *src.otherwiseChild);
    }
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
        catalog_, defaults_, cwd_, outsideIds, outsideCount,
        taskqueue_, target->queueName, err);
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
