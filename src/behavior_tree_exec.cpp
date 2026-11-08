#include "behavior_tree_internal.h"
#include "common.h"

#include <windows.h>
#include <algorithm>

namespace pc {

namespace {

uint64_t CaptureStartFiletimeWithGuard() {
    Sleep(2);
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    uint64_t val = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    Sleep(2);
    return val;
}

bool IsConditionType(const std::string& t) {
    return t == "fileExists" || t == "fileNewer" || t == "outputMatches" ||
           t == "exitCode" || t == "jsonValue" || t == "var" || t == "nodeState";
}

} // namespace

void BehaviorTree::Impl::TickNode(BehaviorTree& bt, BtNode& node, int64_t nowMs) {
    if (bt.state_ == "failed" || bt.state_ == "cancelled" || bt.state_ == "succeeded") return;

    if (bt.limits_.maxWallSec > 0 && bt.startMs_ > 0 &&
        (nowMs - bt.startMs_) >= static_cast<int64_t>(bt.limits_.maxWallSec) * 1000) {
        CancelSubtreeJobs(bt, *root, true);
        bt.state_ = "failed";
        bt.detail_ = "limitExceeded";
        bt.EmitEvent(nowMs, "btFailed", bt.id_, bt.state_, bt.detail_, bt.nodeRuns_);
        return;
    }

    if (node.state == "succeeded" || node.state == "failed" || node.state == "skipped") return;

    const std::string& t = node.type;
    if (IsConditionType(t)) {
        EvaluateCondition(bt, node, nowMs);
        return;
    }

    auto checkNodeRunLimit = [&]() -> bool {
        if (bt.nodeRuns_ + 1 > bt.limits_.maxNodeRuns) {
            node.hasStatus = true;
            node.state = "failed";
            node.detail = "limitExceeded";
            CancelSubtreeJobs(bt, *root, true);
            bt.state_ = "failed";
            bt.detail_ = "limitExceeded";
            bt.EmitEvent(nowMs, "btFailed", bt.id_, bt.state_, bt.detail_, bt.nodeRuns_);
            return false;
        }
        bt.nodeRuns_++;
        return true;
    };

    auto failNodeUndef = [&](const std::string& undefVar) {
        node.state = "failed";
        node.detail = "undefinedVar: " + undefVar;
        bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
    };

    if (t == "succeed" || t == "fail") {
        if (!checkNodeRunLimit()) return;
        node.hasStatus = true;
        node.runs++;
        node.state = (t == "succeed") ? "succeeded" : "failed";
        node.detail = (t == "succeed") ? "" : (node.failDetail.empty() ? "fail node" : node.failDetail);
        bt.EmitEvent(nowMs, "nodeStarted", node.nodeKey, "running", "", node.runs);
        bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
        return;
    }

    if (t == "set") {
        if (!checkNodeRunLimit()) return;
        node.hasStatus = true;
        node.runs++;
        std::string expandedVal, undefVar;
        if (!ExpandString(bt, node.setValue, false, expandedVal, undefVar)) {
            failNodeUndef(undefVar);
            return;
        }
        bt.vars_[node.setVar] = expandedVal;
        node.state = "succeeded";
        node.detail.clear();
        bt.EmitEvent(nowMs, "varSet", node.nodeKey, node.state, "", node.runs, node.setVar, expandedVal);
        bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, "", node.runs);
        return;
    }

    if (t == "wait") {
        if (node.state == "idle") {
            if (!checkNodeRunLimit()) return;
            node.hasStatus = true;
            node.runs++;
            if (node.sec <= 0) {
                node.state = "succeeded";
                bt.EmitEvent(nowMs, "nodeStarted", node.nodeKey, "running", "", node.runs);
                bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, "", node.runs);
                return;
            }
            node.state = "running";
            node.waitDeadlineMs = nowMs + static_cast<int64_t>(node.sec) * 1000;
            bt.EmitEvent(nowMs, "nodeStarted", node.nodeKey, node.state, "", node.runs);
        }
        return;
    }

    if (t == "escalate") {
        if (node.state == "idle") StartEscalateNode(bt, node, nowMs);
        return;
    }

    if (t == "job" || t == "worker") {
        if (node.state == "running") return;
        if (!checkNodeRunLimit()) return;

        node.hasStatus = true;
        node.runs++;
        node.detail.clear();

        std::string expSummary, expCwd, undefVar;
        if (!ExpandString(bt, node.summary, false, expSummary, undefVar) ||
            !ExpandString(bt, node.cwd.empty() ? bt.cwd_ : node.cwd, false, expCwd, undefVar)) {
            failNodeUndef(undefVar);
            return;
        }

        std::map<std::string, std::string> expEnv;
        for (const auto& [ek, ev] : node.env) {
            std::string xv;
            if (!ExpandString(bt, ev, false, xv, undefVar)) { failNodeUndef(undefVar); return; }
            expEnv[ek] = xv;
        }

        BtJobResources expRes;
        for (const auto& w : node.resources.write) {
            std::string xw;
            if (!ExpandString(bt, w, false, xw, undefVar)) { failNodeUndef(undefVar); return; }
            expRes.write.push_back(xw);
        }
        for (const auto& r : node.resources.read) {
            std::string xr;
            if (!ExpandString(bt, r, false, xr, undefVar)) { failNodeUndef(undefVar); return; }
            expRes.read.push_back(xr);
        }

        std::string proposedId = bt.id_ + "-" + std::to_string(bt.nextJobSeq_++);
        node.lastStartMs = static_cast<int64_t>(CaptureStartFiletimeWithGuard());
        node.lastStartTimestamp = FormatEpochMsIso(nowMs);

        std::string jobId;
        if (t == "job") {
            std::string rawInstr = node.instructionsText;
            if (rawInstr.empty() && !node.instructionsFile.empty()) {
                std::string relF;
                if (!ExpandString(bt, node.instructionsFile, false, relF, undefVar)) { failNodeUndef(undefVar); return; }
                rawInstr = ReadFileUtf8Wide(ResolveBtPathWide(expCwd, relF));
            }
            std::string expCmd, expInstr;
            if (!ExpandString(bt, node.command, true, expCmd, undefVar) ||
                !ExpandString(bt, rawInstr, false, expInstr, undefVar)) {
                failNodeUndef(undefVar);
                return;
            }

            BtJobSpec spec{bt.id_, node.id, node.nodeKey, node.runs, proposedId, expCmd, expSummary,
                           expCwd, node.timeoutSec, std::move(expEnv), node.success, node.idle,
                           std::move(expRes), expInstr, node.maxAttempts};
            node.isWorkerJob = false;
            if (bt.runner_) jobId = bt.runner_->StartJob(spec);
        } else {
            std::string rawPrompt = node.prompt;
            if (rawPrompt.empty() && !node.promptFile.empty()) {
                std::string relF;
                if (!ExpandString(bt, node.promptFile, false, relF, undefVar)) { failNodeUndef(undefVar); return; }
                rawPrompt = ReadFileUtf8Wide(ResolveBtPathWide(expCwd, relF));
            }
            std::string expPrompt;
            if (!ExpandString(bt, rawPrompt, false, expPrompt, undefVar)) { failNodeUndef(undefVar); return; }

            std::string effEng = !node.engine.empty()
                ? node.engine
                : (!bt.workerDefaults_.engine.empty() ? bt.workerDefaults_.engine
                                                      : (bt.catalog_ ? bt.catalog_->DefaultEngine() : "agy"));
            std::string effMod = !node.model.empty()
                ? node.model
                : ((node.engine.empty() || node.engine == bt.workerDefaults_.engine) ? bt.workerDefaults_.model : "");

            BtWorkerSpec wspec{bt.id_, node.id, node.nodeKey, node.runs, proposedId, effEng, effMod,
                               expPrompt, expSummary, expCwd, node.timeoutSec, std::move(expEnv),
                               node.success, node.idle, std::move(expRes), node.outputSchema};
            node.isWorkerJob = true;
            if (bt.runner_) jobId = bt.runner_->StartWorker(wspec);
        }

        if (jobId.empty()) {
            node.state = "failed";
            node.detail = "launchFailed";
            bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
            return;
        }
        node.state = "running";
        node.activeJobId = jobId;
        node.jobs.push_back(jobId);
        runningJobs[jobId] = RunningJobInfo{&node, false, node.isWorkerJob};
        bt.EmitEvent(nowMs, "nodeStarted", node.nodeKey, node.state, jobId, node.runs);
        return;
    }

    if (t == "sequence" || t == "fallback") {
        bool isSeq = (t == "sequence");
        if (node.state == "idle") {
            node.hasStatus = true;
            node.state = "running";
            if (!isSeq || node.pathKey != "root") node.runs++;
            bt.EmitEvent(nowMs, "nodeStarted", node.nodeKey, node.state, "", node.runs);
        }
        for (const auto& child : node.children) {
            if (isSeq && child->state == "succeeded") continue;
            if (!isSeq && child->state == "failed") continue;
            if (child->state == "idle" || child->state == "running") {
                TickNode(bt, *child, nowMs);
                if (bt.state_ == "failed" || bt.state_ == "cancelled") return;
            }
            if (child->state == "running") return;
            if (isSeq && child->state == "failed") {
                node.state = "failed";
                node.detail = child->detail;
                bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
                return;
            }
            if (!isSeq && child->state == "succeeded") {
                node.state = "succeeded";
                node.detail.clear();
                bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, "", node.runs);
                return;
            }
        }
        node.state = isSeq ? "succeeded" : "failed";
        node.detail = isSeq ? "" : (node.children.empty() ? "" : node.children.back()->detail);
        bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
        return;
    }

    if (t == "parallel") {
        if (node.state == "idle") {
            node.hasStatus = true;
            node.state = "running";
            node.runs++;
            bt.EmitEvent(nowMs, "nodeStarted", node.nodeKey, node.state, "", node.runs);
        }
        for (const auto& child : node.children) {
            if (child->state == "idle" || child->state == "running") {
                TickNode(bt, *child, nowMs);
                if (bt.state_ == "failed" || bt.state_ == "cancelled") return;
            }
        }
        int total = static_cast<int>(node.children.size());
        int reqSuccess = (node.parallelSuccessMode == "any") ? 1
                       : ((node.parallelSuccessMode == "count") ? node.parallelSuccessCount : total);

        int succ = 0, running = 0;
        std::string firstFailDetail;
        for (const auto& child : node.children) {
            if (child->state == "succeeded") succ++;
            else if (child->state == "failed" && firstFailDetail.empty()) firstFailDetail = child->detail;
            else if (child->state == "running" || child->state == "idle") running++;
        }
        if (succ >= reqSuccess || succ + running < reqSuccess) {
            for (const auto& child : node.children) {
                if (child->state == "running") CancelSubtreeJobs(bt, *child, true);
            }
            node.state = (succ >= reqSuccess) ? "succeeded" : "failed";
            node.detail = (succ >= reqSuccess) ? "" : firstFailDetail;
            bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
        }
        return;
    }

    if (t == "if") {
        if (node.state == "idle") {
            node.hasStatus = true;
            node.state = "running";
            node.ifBranch = 0;
            bt.EmitEvent(nowMs, "nodeStarted", node.nodeKey, node.state, "", node.runs);
        }
        if (node.ifBranch == 0) {
            TickNode(bt, *node.condChild, nowMs);
            if (bt.state_ == "failed" || bt.state_ == "cancelled" || node.condChild->state == "running") return;
            if (node.condChild->state == "succeeded") {
                node.ifBranch = 1;
            } else if (!node.elseChild) {
                node.state = "succeeded";
                node.detail.clear();
                bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, "", node.runs);
                return;
            } else {
                node.ifBranch = 2;
            }
        }
        BtNode* branch = (node.ifBranch == 1) ? node.thenChild.get() : node.elseChild.get();
        TickNode(bt, *branch, nowMs);
        if (bt.state_ == "failed" || bt.state_ == "cancelled" || branch->state == "running") return;
        node.state = branch->state;
        node.detail = branch->detail;
        bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
        return;
    }

    if (t == "retry") {
        if (node.state == "idle") {
            node.hasStatus = true;
            node.state = "running";
            node.runs = 1;
            bt.EmitEvent(nowMs, "nodeStarted", node.nodeKey, node.state, "", node.runs);
        }
        while (node.state == "running") {
            if (node.retryDeadlineMs > nowMs) return;
            node.retryDeadlineMs = 0;
            TickNode(bt, *node.singleChild, nowMs);
            if (bt.state_ == "failed" || bt.state_ == "cancelled" || node.singleChild->state == "running") return;
            if (node.singleChild->state == "succeeded") {
                node.state = "succeeded";
                node.detail.clear();
                bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, "", node.runs);
                return;
            }
            if (node.runs < node.maxCount) {
                node.runs++;
                ResetSubtreeForRun(*node.singleChild, false);
                if (node.delaySec > 0) {
                    node.retryDeadlineMs = nowMs + static_cast<int64_t>(node.delaySec) * 1000;
                    return;
                }
            } else {
                node.state = "failed";
                node.detail = node.singleChild->detail;
                bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
                return;
            }
        }
        return;
    }

    if (t == "repeat") {
        if (node.state == "idle") {
            node.hasStatus = true;
            node.state = "running";
            node.runs = 1;
            node.repeatWaitingUntil = false;
            bt.EmitEvent(nowMs, "nodeStarted", node.nodeKey, node.state, "", node.runs);
        }
        while (node.state == "running") {
            if (!node.repeatWaitingUntil) {
                TickNode(bt, *node.singleChild, nowMs);
                if (bt.state_ == "failed" || bt.state_ == "cancelled" || node.singleChild->state == "running") return;
                if (node.singleChild->state == "failed") {
                    node.state = "failed";
                    node.detail = node.singleChild->detail;
                    bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
                    return;
                }
                if (node.untilChild) {
                    node.repeatWaitingUntil = true;
                    ResetSubtreeForRun(*node.untilChild, false);
                }
            }
            if (node.repeatWaitingUntil && node.untilChild) {
                TickNode(bt, *node.untilChild, nowMs);
                if (bt.state_ == "failed" || bt.state_ == "cancelled" || node.untilChild->state == "running") return;
                node.repeatWaitingUntil = false;
                if (node.untilChild->state == "succeeded") {
                    node.state = "succeeded";
                    node.detail.clear();
                    bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, "", node.runs);
                    return;
                }
            }
            if (node.runs < node.maxCount) {
                node.runs++;
                ResetSubtreeForRun(*node.singleChild, false);
            } else {
                node.state = node.untilChild ? "failed" : "succeeded";
                node.detail = node.untilChild ? "until condition not satisfied after max repeats" : "";
                bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
                return;
            }
        }
        return;
    }

    if (t == "invert" || t == "forceSuccess" || t == "forceFailure") {
        if (node.state == "idle") {
            node.hasStatus = true;
            node.state = "running";
            node.runs++;
            bt.EmitEvent(nowMs, "nodeStarted", node.nodeKey, node.state, "", node.runs);
        }
        TickNode(bt, *node.singleChild, nowMs);
        if (bt.state_ == "failed" || bt.state_ == "cancelled" || node.singleChild->state == "running") return;

        if (t == "invert") {
            node.state = (node.singleChild->state == "succeeded") ? "failed" : "succeeded";
            node.detail = (node.state == "failed") ? node.singleChild->detail : "";
        } else if (t == "forceSuccess") {
            node.state = "succeeded";
            node.detail.clear();
        } else {
            node.state = "failed";
            node.detail = node.singleChild->detail.empty() ? "forceFailure" : node.singleChild->detail;
        }
        bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
        return;
    }

    if (t == "timeout") {
        if (node.state == "idle") {
            node.hasStatus = true;
            node.state = "running";
            node.runs++;
            node.timeoutDeadlineMs = nowMs + static_cast<int64_t>(node.sec) * 1000;
            bt.EmitEvent(nowMs, "nodeStarted", node.nodeKey, node.state, "", node.runs);
        }
        TickNode(bt, *node.singleChild, nowMs);
        if (bt.state_ == "failed" || bt.state_ == "cancelled" || node.singleChild->state == "running") return;
        node.timeoutDeadlineMs = 0;
        node.state = node.singleChild->state;
        node.detail = node.singleChild->detail;
        bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
        return;
    }
}

} // namespace pc
