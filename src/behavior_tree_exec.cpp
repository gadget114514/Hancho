#include "behavior_tree_internal.h"
#include "common.h"

#ifdef _WIN32
#include <windows.h>
#endif
#include <algorithm>

namespace pc {

namespace {

void ReplaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}


uint64_t CaptureStartFiletimeWithGuard() {
    SleepMs(2);
    uint64_t value = GetSystemTimeAsFiletime100ns();
    SleepMs(2);
    return value;
}

bool IsConditionType(const std::string& t) {
    return t == "fileExists" || t == "fileNewer" || t == "outputMatches" ||
           t == "exitCode" || t == "jsonValue" || t == "var" || t == "nodeState" ||
           t == "failure";
}

void CollectFailedLeaves(const BtNode& node, std::vector<const BtNode*>& out, int depth) {
    if (depth > 64) return;
    bool leaf = node.children.empty() && !node.condChild && !node.thenChild && !node.elseChild &&
                !node.singleChild && !node.untilChild && !node.otherwiseChild && node.catchHandlers.empty();
    if (leaf && node.state == "failed") out.push_back(&node);
    for (const auto& c : node.children) CollectFailedLeaves(*c, out, depth + 1);
    if (node.condChild) CollectFailedLeaves(*node.condChild, out, depth + 1);
    if (node.thenChild) CollectFailedLeaves(*node.thenChild, out, depth + 1);
    if (node.elseChild) CollectFailedLeaves(*node.elseChild, out, depth + 1);
    if (node.singleChild) CollectFailedLeaves(*node.singleChild, out, depth + 1);
    if (node.untilChild) CollectFailedLeaves(*node.untilChild, out, depth + 1);
    if (node.otherwiseChild) CollectFailedLeaves(*node.otherwiseChild, out, depth + 1);
    for (const auto& h : node.catchHandlers) {
        if (h.doNode) CollectFailedLeaves(*h.doNode, out, depth + 1);
    }
}

// The failure a catch handler inspects: the last failed job/worker leaf under the child.
const BtNode* CatchFailureLeaf(const BtNode& child) {
    std::vector<const BtNode*> leaves;
    CollectFailedLeaves(child, leaves, 0);
    for (auto it = leaves.rbegin(); it != leaves.rend(); ++it) {
        if ((*it)->hasFailureDiagnostic &&
            ((*it)->type == "job" || (*it)->type == "worker")) return *it;
    }
    for (auto it = leaves.rbegin(); it != leaves.rend(); ++it) {
        if ((*it)->type == "job" || (*it)->type == "worker") return *it;
    }
    return leaves.empty() ? nullptr : leaves.back();
}

bool HandlerClassMatches(const BtCatchHandler& h, const std::string& failureClass) {
    if (h.classes.empty()) return true;
    for (const auto& c : h.classes) {
        if (c == "*" || c == failureClass) return true;
    }
    return false;
}

bool HandlerCauseMatches(const BtCatchHandler& h, const json& diag) {
    if (h.cause.empty()) return true;
    if (!diag.is_object()) return false;
    auto it = diag.find("likelyCauses");
    if (it == diag.end() || !it->is_array()) return false;
    for (const auto& c : *it) {
        if (c.is_object() && c.value("id", std::string()) == h.cause) return true;
    }
    return false;
}

int MatchCatchHandler(const BtNode& node, const BtNode* failedLeaf) {
    std::string failureClass;
    json diag = json::object();
    if (failedLeaf) {
        if (failedLeaf->hasFailureDiagnostic) diag = failedLeaf->failureDiagnostic;
        if (diag.is_object()) failureClass = diag.value("class", std::string());
    }
    for (size_t i = 0; i < node.catchHandlers.size(); ++i) {
        const BtCatchHandler& h = node.catchHandlers[i];
        if (HandlerClassMatches(h, failureClass) && HandlerCauseMatches(h, diag)) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::string CatchHandlerLabel(const BtCatchHandler& h, int index) {
    std::string out = "handler[" + std::to_string(index) + "]";
    if (!h.classes.empty()) {
        out += " class=";
        for (size_t i = 0; i < h.classes.size(); ++i) {
            if (i > 0) out += ",";
            out += h.classes[i];
        }
    }
    if (!h.cause.empty()) out += " cause=" + h.cause;
    return out;
}
} // namespace

// 091-6: keep a leaf waiting (never failed) while its engine is down.
bool BehaviorTree::Impl::HoldForEngine(BehaviorTree& bt, BtNode& node,
                                       const std::string& engine,
                                       const std::string& model, int64_t nowMs) {
    if (!bt.health_ || engine.empty() || !bt.health_->IsDown(engine, model)) return false;
    node.waitingForEngine = true;
    node.state = "running";
    node.lastEngine = engine;
    node.lastModel = model;
    node.detail = "engine down: " + engine + "/" + model;
    const int64_t next = bt.health_->NextProbeAtMs(engine, model);
    node.engineWaitUntilMs = next > nowMs ? next : (nowMs + 1000);
    bt.EmitEvent(nowMs, "nodeHeldForEngine", node.nodeKey, node.state, node.detail, node.runs);
    return true;
}

bool BehaviorTree::Impl::ShouldSwitchEngine(const BtNode& node, const std::string& failureClass) const {
    if (node.engineCandidates.empty()) return false;
    if (node.engineCandidateIndex + 1 >= static_cast<int>(node.engineCandidates.size())) return false;
    return std::find(node.switchEngineOn.begin(), node.switchEngineOn.end(), failureClass) !=
           node.switchEngineOn.end();
}

bool BehaviorTree::Impl::TrySwitchEngine(BehaviorTree& bt, BtNode& node,
                                         const std::string& failureClass,
                                         const std::string& detail, int64_t nowMs) {
    if (!ShouldSwitchEngine(node, failureClass)) return false;
    const BtEngineChoice& from = node.engineCandidates[node.engineCandidateIndex];
    BtEngineAttempt attempt;
    attempt.engine = from.engine;
    attempt.model = from.model;
    attempt.result = "failed";
    attempt.failureClass = failureClass;
    attempt.detail = detail;
    attempt.list = from.list;
    attempt.entry = from.entry;
    node.engineAttempts.push_back(std::move(attempt));

    node.engineCandidateIndex++;
    const BtEngineChoice& to = node.engineCandidates[node.engineCandidateIndex];
    node.lastEngine = to.engine;
    node.lastModel = to.model;
    node.state = "idle";
    node.activeJobId.clear();
    node.detail.clear();

    std::string reason = from.engine + "/" + from.model + " -> " + to.engine + "/" + to.model;
    if (!failureClass.empty()) reason += " (" + failureClass + ")";
    if (!to.list.empty()) reason += " [" + to.list + "]";
    if (!to.entry.empty()) reason += " " + to.entry;
    if (!detail.empty()) reason += " " + BtVarPreview(detail, 120);
    bt.EmitEvent(nowMs, "engineSwitched", node.nodeKey, "running", reason, node.runs,
                 "engine", to.engine);
    const std::string prefix = !node.id.empty() ? node.id : node.nodeKey;
    bt.vars_[prefix + ".engine"] = to.engine;
    bt.vars_[prefix + ".model"] = to.model;
    bt.EmitEvent(nowMs, "varSet", node.nodeKey, "running", "", node.runs,
                 "engine", to.engine);
    bt.EmitEvent(nowMs, "varSet", node.nodeKey, "running", "", node.runs,
                 "model", to.model);
    return true;
}

void BehaviorTree::Impl::RunProbeLeaf(BehaviorTree& bt, BtNode& node, int64_t nowMs) {
    node.hasStatus = true;
    node.runs++;
    bt.EmitEvent(nowMs, "nodeStarted", node.nodeKey, "running", "", node.runs);

    std::string eng = !node.engine.empty()
        ? node.engine
        : (!bt.workerDefaults_.engine.empty() ? bt.workerDefaults_.engine
                                              : (bt.catalog_ ? bt.catalog_->DefaultEngine() : "agy"));
    std::string mod = node.model;
    node.lastEngine = eng;
    node.lastModel = mod;

    bool ok = false;
    std::string classOut = "probe_failed";
    std::string detail = "no prober configured";
    int elapsed = 0;
    bool cached = false;
    if (bt.prober_) {
        BtProbeResult pr = bt.prober_->Probe(eng, mod, node.timeoutSec > 0 ? node.timeoutSec : 60);
        ok = pr.ok;
        elapsed = pr.elapsedMs;
        cached = pr.cached;
        if (!pr.failureClass.empty()) classOut = pr.failureClass;
        detail = pr.detail;
    }
    if (ok) {
        node.state = "succeeded";
        node.detail = detail.empty() ? ("probe ok " + eng + "/" + mod) : detail;
        ApplyFailureDiagnostic(bt, node, "", true, nowMs);
    } else {
        node.state = "failed";
        // Always surface `probe_failed` as the BT failure class; keep the engine's own
        // classification (startup_hang / auth_error / ...) in the diagnostic detail.
        std::string inner = classOut;
        json diag = json{{"class", "probe_failed"},
                         {"summary", "probe failed for " + eng + "/" + mod},
                         {"detail", inner + (detail.empty() ? "" : (": " + detail))}};
        node.detail = "probe_failed: " + inner + (detail.empty() ? "" : (": " + detail));
        ApplyFailureDiagnostic(bt, node, diag.dump(), false, nowMs);
    }
    (void)elapsed;
    (void)cached;
    bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
}


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
    if (node.waitingForEngine) return;

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

    if (t == "probe") {
        if (node.state == "idle") {
            if (!checkNodeRunLimit()) return;
            RunProbeLeaf(bt, node, nowMs);
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

        node.launchSeq++;
        std::string proposedId = bt.id_ + "-" + node.nodeKey + "-" + std::to_string(node.launchSeq);
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

            node.isWorkerJob = false;
            node.lastEngine.clear();
            node.lastModel.clear();
            // `{engine}` / `{model}` placeholders let wrapper commands (e.g. ww-run) be
            // switched by the same approved candidate list as worker nodes. A command with
            // no placeholders is simply left unchanged.
            std::string jobEngine = node.engine;
            std::string jobModel = node.model;
            if (!node.engineCandidates.empty()) {
                if (node.engineCandidateIndex >= static_cast<int>(node.engineCandidates.size())) {
                    node.state = "failed";
                    node.detail = "noUsableEngine";
                    bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
                    return;
                }
                const BtEngineChoice& choice = node.engineCandidates[node.engineCandidateIndex];
                if (bt.catalog_ && !bt.catalog_->IsApprovedForList("worker", choice.engine, choice.model)) {
                    node.state = "failed";
                    node.detail = "engineNotApproved";
                    bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
                    return;
                }
                jobEngine = choice.engine;
                jobModel = choice.model;
            }
            if (!jobEngine.empty()) ReplaceAll(expCmd, "{engine}", jobEngine);
            if (!jobModel.empty()) ReplaceAll(expCmd, "{model}", jobModel);
            node.lastEngine = jobEngine;
            node.lastModel = jobModel;
            if (HoldForEngine(bt, node, jobEngine, jobModel, nowMs)) return;

            BtJobSpec spec{bt.id_, node.id, node.nodeKey, node.runs, proposedId, node.queueName,
                           expCmd, expSummary,
                           expCwd, node.timeoutSec, node.estimateSec, std::move(expEnv), node.success, node.idle,
                           std::move(expRes), expInstr, node.maxAttempts};
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

            std::string effEng;
            std::string effMod;
            if (!node.engineCandidates.empty()) {
                bool resolved = false;
                while (node.engineCandidateIndex < static_cast<int>(node.engineCandidates.size())) {
                    const BtEngineChoice& choice = node.engineCandidates[node.engineCandidateIndex];
                    // Re-check the approved list immediately before use: a candidate removed,
                    // disabled or returned to proposed while the tree was running is never used.
                    if (bt.catalog_ && !bt.catalog_->IsApprovedForList("worker", choice.engine, choice.model)) {
                        node.engineAttempts.push_back({choice.engine, choice.model, "skipped",
                                                       "notApproved", "candidate is not approved and enabled"});
                        node.engineCandidateIndex++;
                        continue;
                    }
                    // 091-6: skip a down engine and start from the next approved candidate.
                    if (bt.health_ && bt.health_->IsDown(choice.engine, choice.model)) {
                        node.engineAttempts.push_back({choice.engine, choice.model, "skipped",
                                                       "engineDown", "engine is down; waiting for recovery"});
                        node.engineCandidateIndex++;
                        continue;
                    }
                    if (node.probe && bt.prober_) {
                        BtProbeResult pr = bt.prober_->Probe(choice.engine, choice.model,
                                                            node.probeTimeoutSec > 0 ? node.probeTimeoutSec : 60);
                        if (!pr.ok) {
                            std::string inner = pr.failureClass.empty() ? "probe_failed" : pr.failureClass;
                            if (TrySwitchEngine(bt, node, "probe_failed", inner, nowMs)) continue;
                            node.state = "failed";
                            node.lastEngine = choice.engine;
                            node.lastModel = choice.model;
                            node.detail = "probe_failed: " + inner +
                                          (pr.detail.empty() ? "" : (": " + pr.detail));
                            json diag = json{{"class", "probe_failed"},
                                             {"summary", "preflight probe failed"},
                                             {"detail", inner}};
                            ApplyFailureDiagnostic(bt, node, diag.dump(), false, nowMs);
                            bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
                            return;
                        }
                    }
                    effEng = choice.engine;
                    effMod = choice.model;
                    resolved = true;
                    break;
                }
                if (!resolved) {
                    node.state = "failed";
                    node.detail = "noUsableEngine";
                    bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
                    return;
                }
            } else {
                effEng = !node.engine.empty()
                    ? node.engine
                    : (!bt.workerDefaults_.engine.empty() ? bt.workerDefaults_.engine
                                                          : (bt.catalog_ ? bt.catalog_->DefaultEngine() : "agy"));
                effMod = !node.model.empty()
                    ? node.model
                    : ((node.engine.empty() || node.engine == bt.workerDefaults_.engine) ? bt.workerDefaults_.model : "");
            }

            if (HoldForEngine(bt, node, effEng, effMod, nowMs)) return;

            BtWorkerSpec wspec{bt.id_, node.id, node.nodeKey, node.runs, proposedId, node.queueName,
                               effEng, effMod,
                               expPrompt, expSummary, expCwd, node.timeoutSec, node.estimateSec, std::move(expEnv),
                                node.success, node.idle, std::move(expRes), node.outputSchema,
                                node.maxAttempts};
            node.isWorkerJob = true;
            node.lastEngine = effEng;
            node.lastModel = effMod;
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

    if (t == "catch") {
        if (node.state == "idle") {
            node.hasStatus = true;
            node.state = "running";
            node.catchPhase = 0;
            node.catchRetries = 0;
            node.catchHandlerIndex = -1;
            node.catchHandler.clear();
            node.runs++;
            bt.EmitEvent(nowMs, "nodeStarted", node.nodeKey, node.state, "", node.runs);
        }
        auto finishCatch = [&](bool ok, const std::string& detail) {
            node.state = ok ? "succeeded" : "failed";
            node.detail = detail;
            bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
        };
        auto startOtherwise = [&]() -> bool {
            if (!node.otherwiseChild) return false;
            node.catchPhase = 3;
            ResetSubtreeForRun(bt, *node.otherwiseChild, false);
            return true;
        };
        while (node.state == "running") {
            if (node.catchPhase == 0) {
                if (!node.singleChild) { finishCatch(false, "missing child"); return; }
                TickNode(bt, *node.singleChild, nowMs);
                if (bt.state_ == "failed" || bt.state_ == "cancelled" ||
                    node.singleChild->state == "running") return;
                if (node.singleChild->state == "succeeded") {
                    finishCatch(true, node.catchHandler);
                    return;
                }
                int idx = MatchCatchHandler(node, CatchFailureLeaf(*node.singleChild));
                if (idx >= 0 && node.catchHandlers[static_cast<size_t>(idx)].doNode) {
                    node.catchHandlerIndex = idx;
                    node.catchHandler = CatchHandlerLabel(node.catchHandlers[static_cast<size_t>(idx)], idx);
                    node.detail = node.catchHandler;
                    node.catchPhase = 1;
                    ResetSubtreeForRun(bt, *node.catchHandlers[static_cast<size_t>(idx)].doNode, false);
                    continue;
                }
                if (startOtherwise()) continue;
                finishCatch(false, node.singleChild->detail);
                return;
            }
            if (node.catchPhase == 1) {
                size_t idx = static_cast<size_t>(node.catchHandlerIndex);
                if (node.catchHandlerIndex < 0 || idx >= node.catchHandlers.size() ||
                    !node.catchHandlers[idx].doNode) {
                    finishCatch(false, "invalid catch handler");
                    return;
                }
                const BtCatchHandler& h = node.catchHandlers[idx];
                TickNode(bt, *h.doNode, nowMs);
                if (bt.state_ == "failed" || bt.state_ == "cancelled" || h.doNode->state == "running") return;
                if (h.doNode->state == "failed") {
                    finishCatch(false, node.catchHandler + " do failed: " + h.doNode->detail);
                    return;
                }
                if (!h.thenRetry) {
                    finishCatch(true, node.catchHandler);
                    return;
                }
                if (node.catchRetries >= node.catchMaxRetries) {
                    if (startOtherwise()) continue;
                    finishCatch(false, node.catchHandler + " maxRetries exceeded");
                    return;
                }
                node.catchRetries++;
                node.catchPhase = 0;
                ResetSubtreeForRun(bt, *node.singleChild, false);
                continue;
            }
            if (node.catchPhase == 3 && node.otherwiseChild) {
                TickNode(bt, *node.otherwiseChild, nowMs);
                if (bt.state_ == "failed" || bt.state_ == "cancelled" ||
                    node.otherwiseChild->state == "running") return;
                bool ok = node.otherwiseChild->state == "succeeded";
                std::string detail = node.otherwiseChild->detail.empty()
                    ? node.catchHandler
                    : (node.catchHandler.empty() ? node.otherwiseChild->detail
                                                 : node.catchHandler + ": " + node.otherwiseChild->detail);
                finishCatch(ok, detail);
                return;
            }
            finishCatch(false, "invalid catch phase");
            return;
        }
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
                ResetSubtreeForRun(bt, *node.singleChild, false);
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
                    ResetSubtreeForRun(bt, *node.untilChild, false);
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
                ResetSubtreeForRun(bt, *node.singleChild, false);
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
