#include "behavior_tree_internal.h"
#include "common.h"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace pc {

namespace {

std::string TruncateUtf8Bytes(std::string s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t n = maxBytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
    s.resize(n);
    return s;
}

std::string XmlAttrValue(const std::string& tag, const std::string& name) {
    size_t pos = 0;
    while ((pos = tag.find(name, pos)) != std::string::npos) {
        const bool startOk = pos > 0 && std::isspace(static_cast<unsigned char>(tag[pos - 1]));
        size_t eq = pos + name.size();
        while (eq < tag.size() && std::isspace(static_cast<unsigned char>(tag[eq]))) ++eq;
        if (!startOk || eq >= tag.size() || tag[eq] != '=') { pos += name.size(); continue; }
        size_t q = eq + 1;
        while (q < tag.size() && std::isspace(static_cast<unsigned char>(tag[q]))) ++q;
        if (q >= tag.size() || (tag[q] != '"' && tag[q] != '\'')) return "";
        size_t end = tag.find(tag[q], q + 1);
        if (end == std::string::npos) return "";
        std::string v = tag.substr(q + 1, end - q - 1);
        auto replaceAll = [&](const std::string& from, const std::string& to) {
            size_t p = 0;
            while ((p = v.find(from, p)) != std::string::npos) {
                v.replace(p, from.size(), to);
                p += to.size();
            }
        };
        replaceAll("&lt;", "<");
        replaceAll("&gt;", ">");
        replaceAll("&quot;", "\"");
        replaceAll("&apos;", "'");
        replaceAll("&amp;", "&");
        return v;
    }
    return "";
}

std::vector<std::string> ExtractFailedTestNamesFromXml(const std::string& xml) {
    std::vector<std::string> out;
    size_t pos = 0;
    bool inCase = false;
    bool caseFailed = false;
    std::string caseName;
    auto closeCase = [&]() {
        if (!inCase) return;
        if (caseFailed) out.push_back(caseName.empty() ? "(unnamed)" : caseName);
        inCase = false;
    };
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        size_t end = xml.find('>', pos);
        if (end == std::string::npos) break;
        std::string tag = xml.substr(pos + 1, end - pos - 1);
        pos = end + 1;
        if (tag.empty() || tag[0] == '?' || tag[0] == '!') continue;
        size_t n = 0;
        while (n < tag.size() && !std::isspace(static_cast<unsigned char>(tag[n])) && tag[n] != '/') ++n;
        std::string name = tag.substr(0, n);
        const bool selfClosing = !tag.empty() && tag.back() == '/';
        if (name == "testcase") {
            closeCase();
            std::string cls = XmlAttrValue(tag, "classname");
            std::string nm = XmlAttrValue(tag, "name");
            caseName = cls.empty() ? nm : (cls + "." + nm);
            inCase = true;
            caseFailed = false;
            if (selfClosing) closeCase();
        } else if (name == "/testcase") {
            closeCase();
        } else if (inCase && (name == "failure" || name == "error")) {
            caseFailed = true;
        } else if (name == "test-case") {
            std::string full = XmlAttrValue(tag, "fullname");
            if (full.empty()) full = XmlAttrValue(tag, "name");
            std::string res = XmlAttrValue(tag, "result");
            for (char& c : res) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            if (res == "failed" || res == "failure" || res == "error") {
                out.push_back(full.empty() ? "(unnamed)" : full);
            }
        }
    }
    closeCase();
    return out;
}

std::vector<std::string> BuildJobFailSignature(const BtNode& node,
                                               const RunningJobInfo& info,
                                               const std::string& btCwd,
                                               const std::string& logFile,
                                               const std::string& logTail) {
    const std::string effCwd = !info.effectiveCwd.empty()
        ? info.effectiveCwd
        : (!node.cwd.empty() ? node.cwd : btCwd);
    const std::string testReport = !info.expandedTestReport.empty()
        ? info.expandedTestReport
        : node.success.testReport;
    const std::string contentFile = !info.expandedContentFile.empty()
        ? info.expandedContentFile
        : node.success.contentFile;

    std::vector<std::string> sig;
    if (!testReport.empty()) {
        std::string xml = ReadFileUtf8Wide(ResolveBtPathWide(effCwd, testReport));
        if (!xml.empty()) sig = ExtractFailedTestNamesFromXml(xml);
    } else {
        std::string text;
        if (!contentFile.empty()) {
            text = ReadFileUtf8Wide(ResolveBtPathWide(effCwd, contentFile));
        } else {
            if (!logFile.empty()) text = ReadFileUtf8Wide(Utf8ToWide(logFile));
            if (text.empty()) text = logTail;
        }
        if (!text.empty()) {
            const std::string pattern = node.failLineRegex.empty()
                ? std::string("^\\[FAIL\\].*")
                : node.failLineRegex;
            try {
                std::regex re(pattern);
                std::istringstream iss(text);
                std::string line;
                while (std::getline(iss, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (std::regex_search(line, re)) {
                        sig.push_back(TruncateUtf8Bytes(std::move(line), 300));
                        if (sig.size() >= 200) break;
                    }
                }
            } catch (const std::regex_error&) {
            }
        }
    }
    std::sort(sig.begin(), sig.end());
    sig.erase(std::unique(sig.begin(), sig.end()), sig.end());
    return sig;
}

bool IsEnvFailureClass(const std::string& cls) {
    return cls == "resource_conflict" || cls == "permission_denied" ||
           cls == "rate_limited" || cls == "provider_down" || cls == "auth_error";
}

json ExtractWorkerHandoff(const std::string& resultJson) {
    if (resultJson.empty()) return json();
    json resParsed = json::parse(resultJson, nullptr, false);
    if (!resParsed.is_object() || !resParsed.contains("handoff") || !resParsed["handoff"].is_object()) {
        return json();
    }
    json out = json::object();
    for (auto hit = resParsed["handoff"].begin(); hit != resParsed["handoff"].end(); ++hit) {
        std::string val;
        if (hit.value().is_string()) {
            val = hit.value().get<std::string>();
        } else if (!hit.value().is_null()) {
            val = hit.value().dump();
        } else {
            continue;
        }
        if (val.size() > 2000) {
            size_t n = 2000;
            while (n > 0 && (static_cast<unsigned char>(val[n]) & 0xC0) == 0x80) n--;
            val.resize(n);
        }
        out[hit.key()] = std::move(val);
    }
    return out.empty() ? json() : out;
}

void RecordWorkerAttempt(BtNode& node, const std::string& result,
                         const std::string& failureClass, const std::string& detail,
                         const json& handoff) {
    if (node.engineCandidates.empty() && (!handoff.is_object() || handoff.empty())) return;
    BtEngineAttempt attempt;
    attempt.engine = node.lastEngine;
    attempt.model = node.lastModel;
    attempt.result = result;
    attempt.failureClass = failureClass;
    attempt.detail = detail;
    if (!node.engineCandidates.empty() &&
        node.engineCandidateIndex < static_cast<int>(node.engineCandidates.size())) {
        const auto& c = node.engineCandidates[node.engineCandidateIndex];
        attempt.list = c.list;
        attempt.entry = c.entry;
    }
    if (handoff.is_object() && !handoff.empty()) attempt.handoff = handoff;
    node.engineAttempts.push_back(std::move(attempt));
}

BtNode* FindDeepestFailedNode(BtNode* cur) {
    if (!cur || cur->state != "failed") return nullptr;
    for (auto it = cur->children.rbegin(); it != cur->children.rend(); ++it) {
        if (BtNode* f = FindDeepestFailedNode(it->get())) return f;
    }
    if (BtNode* f = FindDeepestFailedNode(cur->elseChild.get())) return f;
    if (BtNode* f = FindDeepestFailedNode(cur->thenChild.get())) return f;
    if (BtNode* f = FindDeepestFailedNode(cur->condChild.get())) return f;
    if (BtNode* f = FindDeepestFailedNode(cur->untilChild.get())) return f;
    if (BtNode* f = FindDeepestFailedNode(cur->singleChild.get())) return f;
    if (BtNode* f = FindDeepestFailedNode(cur->otherwiseChild.get())) return f;
    for (const auto& h : cur->catchHandlers) {
        if (h.doNode) {
            if (BtNode* f = FindDeepestFailedNode(h.doNode.get())) return f;
        }
    }
    return cur;
}

// Lists a node's children in the order they are actually ticked. This is the order used to
// decide which children are "after" the one containing a retry target: their results must be
// discarded so the branch is re-evaluated from the target onwards.
void CollectExecOrderChildren(BtNode& node, std::vector<BtNode*>& out) {
    const std::string& t = node.type;
    if (t == "sequence" || t == "fallback" || t == "parallel" || t == "foreach") {
        for (const auto& c : node.children) if (c) out.push_back(c.get());
        return;
    }
    if (t == "if") {
        if (node.condChild) out.push_back(node.condChild.get());
        if (node.thenChild) out.push_back(node.thenChild.get());
        if (node.elseChild) out.push_back(node.elseChild.get());
        return;
    }
    if (t == "catch") {
        if (node.singleChild) out.push_back(node.singleChild.get());
        for (const auto& h : node.catchHandlers) if (h.doNode) out.push_back(h.doNode.get());
        if (node.otherwiseChild) out.push_back(node.otherwiseChild.get());
        return;
    }
    if (t == "repeat") {
        if (node.singleChild) out.push_back(node.singleChild.get());
        if (node.untilChild) out.push_back(node.untilChild.get());
        return;
    }
    // retry, invert, forceSuccess, forceFailure, timeout, and any other single-child node.
    if (node.singleChild) out.push_back(node.singleChild.get());
    for (const auto& c : node.children) if (c) out.push_back(c.get());
    if (node.condChild) out.push_back(node.condChild.get());
    if (node.thenChild) out.push_back(node.thenChild.get());
    if (node.elseChild) out.push_back(node.elseChild.get());
    if (node.untilChild) out.push_back(node.untilChild.get());
    if (node.otherwiseChild) out.push_back(node.otherwiseChild.get());
    for (const auto& h : node.catchHandlers) if (h.doNode) out.push_back(h.doNode.get());
}

// Restores the role-specific state of an ancestor so ticking resumes at the child holding
// the target rather than replaying a decision made in the previous run.
void SetReopenRoleState(BtNode& node, BtNode* matched) {
    if (node.type == "if") {
        if (matched == node.condChild.get()) node.ifBranch = 0;
        else if (matched == node.thenChild.get()) node.ifBranch = 1;
        else node.ifBranch = 2;
    } else if (node.type == "catch") {
        if (matched == node.otherwiseChild.get()) {
            node.catchPhase = 3;
        } else if (matched == node.singleChild.get()) {
            node.catchPhase = 0;
            node.catchHandler.clear();
            node.catchHandlerIndex = -1;
        } else {
            node.catchPhase = 1;
            for (size_t i = 0; i < node.catchHandlers.size(); ++i) {
                if (node.catchHandlers[i].doNode.get() == matched) {
                    node.catchHandlerIndex = static_cast<int>(i);
                    break;
                }
            }
        }
    } else if (node.type == "repeat") {
        node.repeatWaitingUntil = (matched == node.untilChild.get());
    }
}

} // namespace

bool BehaviorTree::Impl::ReopenAncestorsToTarget(BehaviorTree& bt, BtNode* cur,
                                                 BtNode* target, bool resetCounters) {
    if (!cur) return false;
    if (cur == target) return true;

    std::vector<BtNode*> ordered;
    CollectExecOrderChildren(*cur, ordered);

    int targetIdx = -1;
    for (size_t i = 0; i < ordered.size(); ++i) {
        if (ReopenAncestorsToTarget(bt, ordered[i], target, resetCounters)) {
            targetIdx = static_cast<int>(i);
            break;
        }
    }
    if (targetIdx < 0) return false;

    // Everything after the target's child must be re-evaluated: later sequence/fallback/
    // parallel siblings, an if's other branch, a catch handler/otherwise, a repeat's until,
    // and — crucially — an escalate gate that ran only because an earlier branch failed.
    for (size_t i = static_cast<size_t>(targetIdx) + 1; i < ordered.size(); ++i) {
        ResetSubtreeForRun(bt, *ordered[i], resetCounters);
    }

    cur->state = "running";
    cur->detail.clear();
    if (resetCounters && (cur->type == "retry" || cur->type == "repeat")) {
        cur->runs = 1;
    }
    SetReopenRoleState(*cur, ordered[static_cast<size_t>(targetIdx)]);
    return true;
}

void BehaviorTree::Impl::CollectNextWakeMs(const BtNode& node, int64_t& minWakeMs) const {
    auto consider = [&](int64_t t) {
        if (t > 0 && (minWakeMs == 0 || t < minWakeMs)) minWakeMs = t;
    };

    if (node.waitingForEngine) consider(node.engineWaitUntilMs);

    if (node.state == "running") {
        consider(node.waitDeadlineMs);
        consider(node.timeoutDeadlineMs);
        consider(node.retryDeadlineMs);
        consider(node.escalateDeadlineMs);
        if (!node.fallbackLaunched) consider(node.fallbackDeadlineMs);
    }

    for (const auto& c : node.children) CollectNextWakeMs(*c, minWakeMs);
    if (node.condChild) CollectNextWakeMs(*node.condChild, minWakeMs);
    if (node.thenChild) CollectNextWakeMs(*node.thenChild, minWakeMs);
    if (node.elseChild) CollectNextWakeMs(*node.elseChild, minWakeMs);
    if (node.singleChild) CollectNextWakeMs(*node.singleChild, minWakeMs);
    if (node.untilChild) CollectNextWakeMs(*node.untilChild, minWakeMs);
    if (node.otherwiseChild) CollectNextWakeMs(*node.otherwiseChild, minWakeMs);
    for (const auto& h : node.catchHandlers) {
        if (h.doNode) CollectNextWakeMs(*h.doNode, minWakeMs);
    }
}

void BehaviorTree::Impl::CheckTimers(BehaviorTree& bt, BtNode& node, int64_t nowMs, bool& anyFired) {
    if (node.state != "running") return;

    // 091-6: a held leaf wakes when its engine may have recovered. All candidates restart
    // from the original one, so a node that had switched away returns to its first choice.
    if (node.waitingForEngine) {
        if (nowMs >= node.engineWaitUntilMs) {
            const bool stillDown = bt.health_ && bt.health_->IsDown(node.lastEngine, node.lastModel);
            if (stillDown) {
                const int64_t next = bt.health_->NextProbeAtMs(node.lastEngine, node.lastModel);
                node.engineWaitUntilMs = next > nowMs ? next : (nowMs + 1000);
            } else {
                node.waitingForEngine = false;
                node.engineWaitUntilMs = 0;
                node.state = "idle";
                node.detail.clear();
                if (!node.engineCandidates.empty()) {
                    node.engineCandidateIndex = 0;
                    node.switchFailCounts.clear();
                    node.recentFailSignatures.clear();
                }
                bt.EmitEvent(nowMs, "engineRecovered", node.nodeKey, node.state,
                             "engine recovered: " + node.lastEngine + "/" + node.lastModel, node.runs);
            }
            anyFired = true;
        }
        return;
    }

    if (node.type == "wait" && node.waitDeadlineMs > 0 && nowMs >= node.waitDeadlineMs) {
        node.waitDeadlineMs = 0;
        node.state = "succeeded";
        node.detail.clear();
        bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, "", node.runs);
        anyFired = true;
        return;
    }

    if (node.type == "timeout" && node.timeoutDeadlineMs > 0 && nowMs >= node.timeoutDeadlineMs) {
        node.timeoutDeadlineMs = 0;
        if (node.singleChild) {
            CancelSubtreeJobs(bt, *node.singleChild, true);
            node.singleChild->state = "failed";
            node.singleChild->detail = "timeout";
        }
        node.state = "failed";
        node.detail = "timeout";
        bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
        anyFired = true;
        return;
    }

    if (node.type == "retry" && node.retryDeadlineMs > 0 && nowMs >= node.retryDeadlineMs) {
        node.retryDeadlineMs = 0;
        anyFired = true;
    }

    if (node.type == "escalate") {
        if (node.escalateDeadlineMs > 0 && nowMs >= node.escalateDeadlineMs) {
            CancelSubtreeJobs(bt, node, false);
            node.state = "failed";
            node.detail = "escalationTimeout";
            bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
            anyFired = true;
            return;
        }
        if (!node.fallbackLaunched && node.fallbackDeadlineMs > 0 && nowMs >= node.fallbackDeadlineMs) {
            LaunchEscalateFallback(bt, node, nowMs);
            anyFired = true;
        }
    }

    for (const auto& c : node.children) CheckTimers(bt, *c, nowMs, anyFired);
    if (node.condChild) CheckTimers(bt, *node.condChild, nowMs, anyFired);
    if (node.thenChild) CheckTimers(bt, *node.thenChild, nowMs, anyFired);
    if (node.elseChild) CheckTimers(bt, *node.elseChild, nowMs, anyFired);
    if (node.singleChild) CheckTimers(bt, *node.singleChild, nowMs, anyFired);
    if (node.untilChild) CheckTimers(bt, *node.untilChild, nowMs, anyFired);
    if (node.otherwiseChild) CheckTimers(bt, *node.otherwiseChild, nowMs, anyFired);
    for (const auto& h : node.catchHandlers) {
        if (h.doNode) CheckTimers(bt, *h.doNode, nowMs, anyFired);
    }
}

void BehaviorTree::Tick(int64_t nowMs) {
    if (!impl_ || !impl_->root) return;
    if (state_ == "succeeded" || state_ == "failed" || state_ == "cancelled" || state_ == "paused") return;

    impl_->TickNode(*this, *impl_->root, nowMs);
    if (state_ == "failed" || state_ == "cancelled" || state_ == "succeeded") return;

    if (impl_->root->state == "succeeded") {
        state_ = "succeeded";
        detail_.clear();
        EmitEvent(nowMs, "btSucceeded", id_, state_, "", nodeRuns_);
    } else if (impl_->root->state == "failed") {
        state_ = "failed";
        detail_ = impl_->root->detail;
        EmitEvent(nowMs, "btFailed", id_, state_, detail_, nodeRuns_);
    } else {
        state_ = impl_->HasWaitingEscalation(*impl_->root) ? "waitingForLlm" : "running";
        BumpVersion(nowMs);
    }
}

void BehaviorTree::Start(int64_t nowMs, bool paused) {
    if (!impl_ || !impl_->root) return;
    startMs_ = nowMs > 0 ? nowMs : 1000;
    if (createdAt_.empty()) createdAt_ = FormatEpochMsIso(startMs_);
    updatedAt_ = FormatEpochMsIso(startMs_);
    state_ = paused ? "paused" : "running";
    detail_.clear();
    EmitEvent(nowMs, "btStarted", id_, state_, "", nodeRuns_);
    if (!paused) Tick(nowMs);
}

void BehaviorTree::OnJobFinished(const std::string& jobId,
                                 bool succeeded,
                                 int exitCode,
                                 const std::string& logFile,
                                 const std::string& logTail,
                                 int64_t nowMs) {
    OnJobFinished(jobId, succeeded, exitCode, logFile, logTail, nowMs, "", "", "");
}

void BehaviorTree::OnJobFinished(const std::string& jobId,
                                 bool succeeded,
                                 int exitCode,
                                 const std::string& logFile,
                                 const std::string& logTail,
                                 const std::string& finalResponse,
                                 const std::string& resultFileJson,
                                 int64_t nowMs,
                                 const std::string& failDetail) {
    OnJobFinished(jobId, succeeded, exitCode, logFile, logTail, nowMs, finalResponse, resultFileJson, failDetail);
}

void BehaviorTree::OnJobFinished(const std::string& jobId,
                                 bool succeeded,
                                 int exitCode,
                                 const std::string& logFile,
                                 const std::string& logTail,
                                 int64_t nowMs,
                                 const std::string& finalResponse,
                                 const std::string& resultFileJson,
                                 const std::string& failDetail) {
    OnJobFinished(jobId, succeeded, exitCode, logFile, logTail, nowMs,
                  finalResponse, resultFileJson, failDetail, "");
}

void BehaviorTree::OnJobFinished(const std::string& jobId,
                                 bool succeeded,
                                 int exitCode,
                                 const std::string& logFile,
                                 const std::string& logTail,
                                 int64_t nowMs,
                                 const std::string& finalResponse,
                                 const std::string& resultFileJson,
                                 const std::string& failDetail,
                                 const std::string& failureDiagnosticJson) {
    if (!impl_ || !impl_->root) return;
    auto it = impl_->runningJobs.find(jobId);
    if (it == impl_->runningJobs.end()) return;

    RunningJobInfo info = it->second;
    impl_->runningJobs.erase(it);
    BtNode& node = *info.node;

    std::string resolvedResultJson = resultFileJson;
    if (!resultFileJson.empty()) {
        size_t firstNonWs = resultFileJson.find_first_not_of(" \t\r\n");
        if (firstNonWs != std::string::npos &&
            resultFileJson[firstNonWs] != '{' &&
            resultFileJson[firstNonWs] != '[') {
            std::string prefix = !node.id.empty() ? node.id : node.nodeKey;
            vars_[prefix + ".resultFile"] = resultFileJson;
            resolvedResultJson = ReadFileUtf8Wide(Utf8ToWide(resultFileJson));
        }
    }

    if (info.isFallback) {
        node.fallbackJobId.clear();
        if (node.state != "running") return;
        if (succeeded) {
            std::string payload = !resolvedResultJson.empty()
                ? resolvedResultJson
                : (!finalResponse.empty() ? finalResponse : logTail);
            json j = json::parse(payload, nullptr, false);
            if (!j.is_discarded() && j.is_object()) {
                std::string ans = j.value("answer", std::string());
                std::string rat = j.value("rationale", std::string());
                std::map<std::string, std::string> extraVars;
                if (j.contains("vars") && j["vars"].is_object()) {
                    for (auto vit = j["vars"].begin(); vit != j["vars"].end(); ++vit) {
                        if (vit.value().is_string()) extraVars[vit.key()] = vit.value().get<std::string>();
                        else extraVars[vit.key()] = vit.value().dump();
                    }
                }
                std::string err;
                if (!impl_->ApplyEscalateAnswer(*this, node, ans, rat, extraVars, nowMs, err)) {
                    node.state = "failed";
                    node.detail = err.empty() ? "outputInvalid" : err;
                    node.escalateDeadlineMs = 0;
                    EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
                }
            } else {
                node.state = "failed";
                node.detail = "outputInvalid";
                node.escalateDeadlineMs = 0;
                EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
            }
        } else {
            // 091-6: a down proxy worker is recorded and the next approved consult row is tried.
            std::string proxyClass;
            json proxyDiag = json::parse(failureDiagnosticJson, nullptr, false);
            if (proxyDiag.is_object()) proxyClass = proxyDiag.value("class", std::string());
            if (!node.lastEngine.empty() && health_ && health_->IsDownWorthy(proxyClass)) {
                health_->MarkDown(node.lastEngine, node.lastModel, proxyClass, failDetail, nowMs);
            }
            if (!node.fallbackCandidates.empty() &&
                node.fallbackCandidateIndex + 1 < node.fallbackCandidates.size()) {
                node.fallbackCandidateIndex++;
                node.fallbackLaunched = false;
                node.fallbackDeadlineMs = nowMs + 1000;
                EmitEvent(nowMs, "engineSwitched", node.nodeKey, "waitingForLlm",
                          "consult fallback: " + proxyClass + ", next candidate", node.runs);
                return;  // escalation stays active while another proxy answer is tried
            }
            node.state = "failed";
            node.detail = !failDetail.empty() ? failDetail : "fallbackFailed";
            node.escalateDeadlineMs = 0;
            EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
        }
        if (state_ == "waitingForLlm") state_ = "running";
        Tick(nowMs);
        return;
    }

    node.activeJobId.clear();
    node.hasExitCode = true;
    node.lastExitCode = exitCode;
    node.lastLogFile = logFile;
    node.lastLogTail = logTail;

    impl_->ApplyCaptures(*this, node, logTail, nowMs);

    // Only worker nodes have a final response and a result file. For a plain job the runner's
    // fallback text is the process log, which must not become ${<id>.result}.
    json workerHandoff;
    if (info.isWorker) {
        workerHandoff = ExtractWorkerHandoff(resolvedResultJson);
        if (workerHandoff.is_object() && !workerHandoff.empty()) {
            node.lastHandoff = workerHandoff;
        } else if (succeeded) {
            node.lastHandoff = json();
        }
        impl_->StoreWorkerOutputs(*this, node, finalResponse, resolvedResultJson, nowMs);
        if (info.isWorker && succeeded && !node.outputSchema.empty()) {
            std::string schemaErr;
            if (!ValidateOutput(resolvedResultJson, node.outputSchema, schemaErr)) {
                node.state = "failed";
                // Keep the reason: without it the supervisor has to dig through the result file.
                node.detail = schemaErr.empty() ? "outputInvalid"
                                                : "outputInvalid: " + BtVarPreview(schemaErr);
                impl_->ApplyFailureDiagnostic(*this, node, "", false, nowMs);
                RecordWorkerAttempt(node, "failed", "outputInvalid", node.detail, workerHandoff);
                EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
                Tick(nowMs);
                return;
            }
        }
    }

    if (succeeded) {
        node.state = "succeeded";
        node.detail.clear();
    } else {
        node.state = "failed";
        node.detail = !failDetail.empty() ? failDetail : ("exit " + std::to_string(exitCode));
    }
    impl_->ApplyFailureDiagnostic(*this, node, failureDiagnosticJson, succeeded, nowMs);
    EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);

    std::string failureClass;
    if (!succeeded && node.hasFailureDiagnostic && node.failureDiagnostic.is_object()) {
        failureClass = node.failureDiagnostic.value("class", std::string());
    }

    // 101-3: a job node with `blame: "<worker id>"` attributes its non-environmental failure
    // to the blamed worker node, classifies progress vs stall/oscillation, and advances the
    // worker's engine candidate for its next run when `switchEngineOn` threshold is met.
    if (!succeeded && !info.isWorker && !node.blame.empty() && !IsEnvFailureClass(failureClass)) {
        BtNode* blamed = impl_->FindNode(node.blame, id_);
        if (blamed && blamed->type == "agent") {
            std::vector<std::string> sig = BuildJobFailSignature(node, info, cwd_, logFile, logTail);
            std::string fixClass = "fix_failed";
            if (!sig.empty() && !blamed->recentFailSignatures.empty()) {
                if (blamed->recentFailSignatures.back() == sig) {
                    fixClass = "fix_no_progress";
                } else {
                    for (const auto& prev : blamed->recentFailSignatures) {
                        if (prev == sig) {
                            fixClass = "fix_oscillating";
                            break;
                        }
                    }
                }
            }
            blamed->switchFailCounts[fixClass]++;
            if (!sig.empty()) {
                blamed->recentFailSignatures.push_back(sig);
                if (blamed->recentFailSignatures.size() > 3) {
                    blamed->recentFailSignatures.erase(blamed->recentFailSignatures.begin());
                }
            }
            if (!blamed->engineAttempts.empty()) {
                std::string curEng = blamed->lastEngine;
                std::string curMod = blamed->lastModel;
                if (!blamed->engineCandidates.empty() &&
                    blamed->engineCandidateIndex < static_cast<int>(blamed->engineCandidates.size())) {
                    curEng = blamed->engineCandidates[blamed->engineCandidateIndex].engine;
                    curMod = blamed->engineCandidates[blamed->engineCandidateIndex].model;
                }
                auto& lastAtt = blamed->engineAttempts.back();
                if (lastAtt.engine == curEng && lastAtt.model == curMod) {
                    lastAtt.failSignature = sig;
                }
            }
            if (!impl_->AdvanceEngineCandidate(*this, *blamed, fixClass, node.detail, nowMs,
                                               json(), sig, false)) {
                if (!blamed->switchStoppedReason.empty() &&
                    node.detail.find(blamed->switchStoppedReason) == std::string::npos) {
                    if (node.detail.empty()) {
                        node.detail = "engineSwitchStopped: " + blamed->switchStoppedReason;
                    } else {
                        node.detail += " (engineSwitchStopped: " + blamed->switchStoppedReason + ")";
                    }
                }
            }
        }
    }

    // 091-6: share the failure with the rest of Hancho so other jobs on this engine wait
    // instead of failing too, and the core can probe for recovery.
    if (!succeeded && !node.lastEngine.empty() && health_ && health_->IsDownWorthy(failureClass)) {
        if (health_) health_->MarkDown(node.lastEngine, node.lastModel, failureClass,
                                       node.detail, nowMs);
    }

    // Engine switching (091-3): a switchable failure on a worker with more approved
    // candidates is not terminal. Record the attempt, emit `engineSwitched`, and relaunch
    // the same node with the next candidate.
    bool switched = false;
    if (!succeeded && !node.engineCandidates.empty()) {
        switched = impl_->TrySwitchEngine(*this, node, failureClass, node.detail, nowMs, workerHandoff) ||
                   impl_->TrySwitchFromDownEngine(*this, node, failureClass, node.detail, nowMs,
                                                  workerHandoff);
    }
    if (!switched && info.isWorker) {
        RecordWorkerAttempt(node, succeeded ? "succeeded" : "failed",
                            succeeded ? "" : failureClass,
                            succeeded ? "" : node.detail,
                            workerHandoff);
    }
    if (switched) {
        Tick(nowMs);
        return;
    }

    // No usable candidate left: keep the node waiting while the engine is down. The task
    // queue retries within maxAttempts; after that this node resumes once recovery probes OK.
    if (!succeeded && impl_->HoldForEngine(*this, node, node.lastEngine, node.lastModel, nowMs)) {
        return;
    }
    Tick(nowMs);
}

void BehaviorTree::OnTimer(int64_t nowMs) {
    if (!impl_ || !impl_->root) return;
    if (state_ == "succeeded" || state_ == "failed" || state_ == "cancelled" || state_ == "paused") return;

    if (limits_.maxWallSec > 0 && startMs_ > 0 &&
        (nowMs - startMs_) >= static_cast<int64_t>(limits_.maxWallSec) * 1000) {
        impl_->CancelSubtreeJobs(*this, *impl_->root, true);
        state_ = "failed";
        detail_ = "limitExceeded";
        EmitEvent(nowMs, "btFailed", id_, state_, detail_, nodeRuns_);
        return;
    }

    bool anyFired = false;
    impl_->CheckTimers(*this, *impl_->root, nowMs, anyFired);
    if (anyFired) {
        if (state_ == "waitingForLlm") state_ = "running";
        Tick(nowMs);
    }
}

int64_t BehaviorTree::NextWakeMs() const {
    if (!impl_ || !impl_->root) return 0;
    if (state_ != "running" && state_ != "waitingForLlm") return 0;
    int64_t minWake = 0;
    impl_->CollectNextWakeMs(*impl_->root, minWake);
    if (limits_.maxWallSec > 0 && startMs_ > 0) {
        int64_t wallDeadline = startMs_ + static_cast<int64_t>(limits_.maxWallSec) * 1000;
        if (minWake == 0 || wallDeadline < minWake) minWake = wallDeadline;
    }
    return minWake;
}

void BehaviorTree::Cancel(int64_t nowMs) {
    if (!impl_ || !impl_->root) return;
    // A failed tree can still be closed: it stays listed until it is retried or closed.
    if (state_ == "succeeded" || state_ == "cancelled") return;
    impl_->CancelSubtreeJobs(*this, *impl_->root, true);
    state_ = "cancelled";
    detail_ = "cancelled";
    EmitEvent(nowMs, "btCancelled", id_, state_, detail_, nodeRuns_);
}

namespace {

// True when `target` lies under a retry node whose attempt budget is already spent
// (runs >= maxCount). Such a node has no attempt left to give the target without a counter reset.
bool FindUnderSpentRetry(BtNode& cur, const BtNode* target, bool spentAbove, bool& spent) {
    if (&cur == target) {
        spent = spentAbove;
        return true;
    }
    const bool spentHere = spentAbove || (cur.type == "retry" && cur.runs >= cur.maxCount);
    std::vector<BtNode*> kids;
    CollectExecOrderChildren(cur, kids);
    for (BtNode* k : kids) {
        if (k && FindUnderSpentRetry(*k, target, spentHere, spent)) return true;
    }
    return false;
}

} // namespace

bool BehaviorTree::Retry(const std::string& fromNodeKey,
                         bool resetCounters,
                         int64_t nowMs,
                         std::string& err) {
    err.clear();
    if (!impl_ || !impl_->root) {
        err = "behavior tree is not loaded";
        return false;
    }
    if (state_ == "running" || state_ == "waitingForLlm") {
        err = "cannot retry while behavior tree is running";
        return false;
    }

    BtNode* target = nullptr;
    if (fromNodeKey.empty()) {
        target = FindDeepestFailedNode(impl_->root.get());
        if (!target) target = impl_->root.get();
    } else {
        target = impl_->FindNode(fromNodeKey, id_);
        if (!target) {
            err = "node not found: " + fromNodeKey;
            return false;
        }
    }

    // Without a counter reset, a retry whose attempts are all spent must not start another
    // attempt of its child: that would run past max.
    if (!resetCounters) {
        bool spent = false;
        if (FindUnderSpentRetry(*impl_->root, target, false, spent) && spent) {
            err = "retry budget is spent; retry with resetCounters=true to start new attempts";
            return false;
        }
    }

    impl_->ResetSubtreeForRun(*this, *target, resetCounters);
    impl_->ReopenAncestorsToTarget(*this, impl_->root.get(), target, resetCounters);
    if (resetCounters) {
        nodeRuns_ = 0;
    }
    startMs_ = nowMs > 0 ? nowMs : 1000;
    state_ = "running";
    detail_.clear();
    Tick(nowMs);
    return true;
}

bool BehaviorTree::SetPaused(bool paused, int64_t nowMs, std::string& err) {
    err.clear();
    if (!impl_ || !impl_->root) {
        err = "behavior tree is not loaded";
        return false;
    }
    if (state_ == "succeeded" || state_ == "failed" || state_ == "cancelled") {
        err = "cannot pause or resume a finished behavior tree";
        return false;
    }
    if (paused) {
        if (state_ != "paused") {
            state_ = "paused";
            BumpVersion(nowMs);
        }
    } else {
        if (state_ == "paused") {
            state_ = impl_->HasWaitingEscalation(*impl_->root) ? "waitingForLlm" : "running";
            Tick(nowMs);
        }
    }
    return true;
}

std::vector<std::string> BehaviorTree::RunningJobIds() const {
    std::vector<std::string> out;
    if (!impl_) return out;
    for (const auto& [jobId, info] : impl_->runningJobs) {
        (void)info;
        out.push_back(jobId);
    }
    return out;
}

bool BehaviorTree::IsJobRunning(const std::string& jobId) const {
    return impl_ && impl_->runningJobs.find(jobId) != impl_->runningJobs.end();
}

} // namespace pc
