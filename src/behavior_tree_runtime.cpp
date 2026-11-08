#include "behavior_tree_internal.h"
#include "common.h"

#include <algorithm>

namespace pc {

namespace {

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
    return cur;
}

bool ReopenAncestorsToTarget(BtNode* cur, BtNode* target, bool resetCounters) {
    if (!cur) return false;
    if (cur == target) return true;

    auto checkChild = [&](BtNode* c) -> bool {
        if (c && ReopenAncestorsToTarget(c, target, resetCounters)) {
            cur->state = "running";
            cur->detail.clear();
            if (resetCounters && (cur->type == "retry" || cur->type == "repeat")) {
                cur->runs = 1;
            }
            return true;
        }
        return false;
    };

    for (const auto& c : cur->children) {
        if (checkChild(c.get())) return true;
    }
    if (checkChild(cur->condChild.get())) { cur->ifBranch = 0; return true; }
    if (checkChild(cur->thenChild.get())) { cur->ifBranch = 1; return true; }
    if (checkChild(cur->elseChild.get())) { cur->ifBranch = 2; return true; }
    if (checkChild(cur->singleChild.get())) return true;
    if (checkChild(cur->untilChild.get())) { cur->repeatWaitingUntil = true; return true; }
    return false;
}

} // namespace

void BehaviorTree::Impl::CollectNextWakeMs(const BtNode& node, int64_t& minWakeMs) const {
    auto consider = [&](int64_t t) {
        if (t > 0 && (minWakeMs == 0 || t < minWakeMs)) minWakeMs = t;
    };

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
}

void BehaviorTree::Impl::CheckTimers(BehaviorTree& bt, BtNode& node, int64_t nowMs, bool& anyFired) {
    if (node.state != "running") return;

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

void BehaviorTree::Start(int64_t nowMs) {
    if (!impl_ || !impl_->root) return;
    startMs_ = nowMs > 0 ? nowMs : 1000;
    if (createdAt_.empty()) createdAt_ = FormatEpochMsIso(startMs_);
    updatedAt_ = FormatEpochMsIso(startMs_);
    state_ = "running";
    detail_.clear();
    EmitEvent(nowMs, "btStarted", id_, state_, "", nodeRuns_);
    Tick(nowMs);
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
    if (info.isWorker) {
        impl_->StoreWorkerOutputs(*this, node, finalResponse, resolvedResultJson, nowMs);
        if (info.isWorker && succeeded && !node.outputSchema.empty()) {
            std::string schemaErr;
            if (!ValidateOutput(resolvedResultJson, node.outputSchema, schemaErr)) {
                node.state = "failed";
                // Keep the reason: without it the supervisor has to dig through the result file.
                node.detail = schemaErr.empty() ? "outputInvalid"
                                                : "outputInvalid: " + BtVarPreview(schemaErr);
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
    EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
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
    if (state_ == "succeeded" || state_ == "failed" || state_ == "cancelled") return;
    impl_->CancelSubtreeJobs(*this, *impl_->root, true);
    state_ = "cancelled";
    detail_ = "cancelled";
    EmitEvent(nowMs, "btCancelled", id_, state_, detail_, nodeRuns_);
}

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

    impl_->ResetSubtreeForRun(*target, resetCounters);
    ReopenAncestorsToTarget(impl_->root.get(), target, resetCounters);
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
