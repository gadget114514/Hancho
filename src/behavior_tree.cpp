#include "behavior_tree_internal.h"
#include "common.h"

#ifdef _WIN32
#include <windows.h>
#endif
#include <algorithm>
#include <cstdio>
#include <ctime>

namespace pc {

std::wstring ResolveBtPathWide(const std::string& cwdUtf8, const std::string& relOrAbsUtf8) {
    if (relOrAbsUtf8.empty()) return ResolveAbsolutePathWide(Utf8ToWide(cwdUtf8));
    std::wstring p = Utf8ToWide(relOrAbsUtf8);
#ifdef _WIN32
    if (p.size() >= 2 && (p[1] == L':' || (p[0] == L'\\' && p[1] == L'\\') || (p[0] == L'/' && p[1] == L'/'))) {
        return ResolveAbsolutePathWide(p);
    }
#else
    if (!p.empty() && p[0] == L'/') {
        return ResolveAbsolutePathWide(p);
    }
#endif
    if (!cwdUtf8.empty()) {
        std::wstring base = Utf8ToWide(cwdUtf8);
        if (!base.empty() && base.back() != L'\\' && base.back() != L'/') {
#ifdef _WIN32
            base.push_back(L'\\');
#else
            base.push_back(L'/');
#endif
        }
        return ResolveAbsolutePathWide(base + p);
    }
    return ResolveAbsolutePathWide(p);
}

std::string FormatEpochMsIso(int64_t nowMs) {
    if (nowMs <= 0) return "1970-01-01T00:00:00";
    // If nowMs looks like a small synthetic test clock (< 10^11 ms, ~1973), format deterministically in UTC.
    time_t sec = static_cast<time_t>(nowMs / 1000);
    std::tm tmv = {};
#ifdef _WIN32
    gmtime_s(&tmv, &sec);
#else
    gmtime_r(&sec, &tmv);
#endif
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

bool NavigateJsonPath(const json& root, const std::string& path, json& outVal) {
    if (path.empty() || path == "$") {
        outVal = root;
        return true;
    }
    std::string clean = path;
    if (clean.rfind("$.", 0) == 0) clean = clean.substr(2);
    else if (clean.rfind("$", 0) == 0) clean = clean.substr(1);

    const json* cur = &root;
    size_t i = 0;
    while (i < clean.size()) {
        while (i < clean.size() && clean[i] == '.') i++;
        if (i >= clean.size()) break;

        size_t nextDot = clean.find('.', i);
        size_t nextBracket = clean.find('[', i);
        size_t tokenEnd = std::min(nextDot, nextBracket);
        if (tokenEnd > i) {
            std::string key = clean.substr(i, tokenEnd - i);
            if (!cur->is_object()) return false;
            auto it = cur->find(key);
            if (it == cur->end()) return false;
            cur = &(*it);
            i = tokenEnd;
        }
        while (i < clean.size() && clean[i] == '[') {
            size_t closeBracket = clean.find(']', i + 1);
            if (closeBracket == std::string::npos) return false;
            std::string idxStr = clean.substr(i + 1, closeBracket - (i + 1));
            if (!cur->is_array()) return false;
            int idx = -1;
            try {
                size_t used = 0;
                idx = std::stoi(idxStr, &used);
                if (used != idxStr.size()) return false;
            } catch (...) {
                return false;
            }
            if (idx < 0 || static_cast<size_t>(idx) >= cur->size()) return false;
            cur = &((*cur)[static_cast<size_t>(idx)]);
            i = closeBracket + 1;
        }
    }
    outVal = *cur;
    return true;
}

namespace {

BtNode* FindNodeRecursive(BtNode* node, const std::string& key, const std::string& btId) {
    if (!node) return nullptr;
    if ((!node->id.empty() && (key == node->id || key == btId + "/" + node->id)) ||
        key == node->pathKey || key == btId + "/" + node->pathKey ||
        key == node->nodeKey || key == btId + "/" + node->nodeKey) {
        return node;
    }
    for (const auto& child : node->children) {
        if (BtNode* f = FindNodeRecursive(child.get(), key, btId)) return f;
    }
    if (BtNode* f = FindNodeRecursive(node->condChild.get(), key, btId)) return f;
    if (BtNode* f = FindNodeRecursive(node->thenChild.get(), key, btId)) return f;
    if (BtNode* f = FindNodeRecursive(node->elseChild.get(), key, btId)) return f;
    if (BtNode* f = FindNodeRecursive(node->singleChild.get(), key, btId)) return f;
    if (BtNode* f = FindNodeRecursive(node->untilChild.get(), key, btId)) return f;
    if (BtNode* f = FindNodeRecursive(node->otherwiseChild.get(), key, btId)) return f;
    for (const auto& h : node->catchHandlers) {
        if (h.doNode) {
            if (BtNode* f = FindNodeRecursive(h.doNode.get(), key, btId)) return f;
        }
    }
    return nullptr;
}

} // namespace

BtNode* BehaviorTree::Impl::FindNode(const std::string& key, const std::string& btId) const {
    if (key.empty() || !root) return nullptr;
    return FindNodeRecursive(root.get(), key, btId);
}

const BtNode* BehaviorTree::Impl::FindNodeConst(const std::string& key, const std::string& btId) const {
    return FindNode(key, btId);
}

BehaviorTree::BehaviorTree() : impl_(std::make_unique<Impl>()) {}
BehaviorTree::~BehaviorTree() = default;
BehaviorTree::BehaviorTree(BehaviorTree&&) noexcept = default;
BehaviorTree& BehaviorTree::operator=(BehaviorTree&&) noexcept = default;

void BehaviorTree::BumpVersion(int64_t nowMs) {
    version_++;
    if (nowMs > 0) {
        updatedAt_ = FormatEpochMsIso(nowMs);
        if (createdAt_.empty()) createdAt_ = updatedAt_;
    }
}

void BehaviorTree::EmitEvent(int64_t nowMs,
                             const std::string& type,
                             const std::string& nodeKey,
                             const std::string& state,
                             const std::string& detail,
                             int run,
                             const std::string& varName,
                             const std::string& varValue) {
    BtEvent ev;
    ev.seq = nextEventSeq_++;
    ev.timestampMs = nowMs;
    ev.type = type;
    ev.btId = id_;
    ev.nodeKey = nodeKey;
    ev.state = state;
    ev.detail = detail;
    ev.run = run;
    ev.varName = varName;
    ev.varValue = varValue;
    events_.push_back(std::move(ev));
    BumpVersion(nowMs);
}

std::string BehaviorTree::GetVar(const std::string& name, bool* outFound) const {
    std::string val;
    bool ok = impl_ && impl_->LookupVar(*this, name, val);
    if (outFound) *outFound = ok;
    return ok ? val : "";
}

std::string BehaviorTree::NodeState(const std::string& nodeKey) const {
    const BtNode* n = impl_ ? impl_->FindNodeConst(nodeKey, id_) : nullptr;
    return n ? n->state : "";
}

std::string BehaviorTree::NodeDetail(const std::string& nodeKey) const {
    const BtNode* n = impl_ ? impl_->FindNodeConst(nodeKey, id_) : nullptr;
    return n ? n->detail : "";
}

int BehaviorTree::NodeRunCount(const std::string& nodeKey) const {
    const BtNode* n = impl_ ? impl_->FindNodeConst(nodeKey, id_) : nullptr;
    return n ? n->runs : 0;
}

std::vector<std::string> BehaviorTree::NodeJobs(const std::string& nodeKey) const {
    const BtNode* n = impl_ ? impl_->FindNodeConst(nodeKey, id_) : nullptr;
    return n ? n->jobs : std::vector<std::string>{};
}

std::vector<BtEvent> BehaviorTree::GetEventsSince(uint64_t sinceSeq) const {
    std::vector<BtEvent> out;
    for (const auto& e : events_) {
        if (e.seq > sinceSeq) out.push_back(e);
    }
    return out;
}

void BehaviorTree::Impl::ResetSubtreeForRun(BehaviorTree& bt, BtNode& node, bool resetHistory) {
    node.state = "idle";
    node.detail.clear();
    node.activeJobId.clear();
    node.fallbackJobId.clear();
    node.waitDeadlineMs = 0;
    node.timeoutDeadlineMs = 0;
    node.retryDeadlineMs = 0;
    node.escalateDeadlineMs = 0;
    node.fallbackDeadlineMs = 0;
    node.fallbackLaunched = false;
    node.ifBranch = 0;
    node.repeatWaitingUntil = false;
    // Stale decision / diagnostic state must not survive a retry: an answered escalate gate
    // would otherwise keep its old `answer` and `${gate.answer}` while the branch is re-run.
    node.answer.clear();
    node.rationale.clear();
    node.takenOver = false;
    node.takeover = json::object();
    node.currentPacket = BtEscalationPacket{};
    node.hasFailureDiagnostic = false;
    node.failureDiagnostic = json::object();
    // The proxy chain is derived from engine health at launch time and walked by index. A
    // re-run must rebuild it from the first candidate, not resume at the last failed one.
    if (node.type == "escalate") {
        node.fallbackCandidates.clear();
        node.fallbackCandidateIndex = 0;
    }
    if (resetHistory) {
        node.runs = 0;
        node.engineCandidateIndex = 0;
    } else if (node.type == "retry" || node.type == "repeat" ||
               node.type == "sequence" || node.type == "fallback" ||
               node.type == "parallel" || node.type == "if" || node.type == "catch") {
        node.runs = 0;
    }
    if (node.type == "catch") {
        node.catchPhase = 0;
        node.catchRetries = 0;
        node.catchHandlerIndex = -1;
        node.catchHandler.clear();
    }

    // Remove variables written by this node in the previous run (answer, result, output.*,
    // failure*, engine, model, ...). Variables belonging to siblings we keep are untouched.
    const std::string prefix = node.id.empty() ? node.nodeKey : node.id;
    if (!prefix.empty()) {
        const std::string dotPrefix = prefix + ".";
        for (auto it = bt.vars_.begin(); it != bt.vars_.end(); ) {
            if (it->first.rfind(dotPrefix, 0) == 0) it = bt.vars_.erase(it);
            else ++it;
        }
    }
    if (!node.setVar.empty()) bt.vars_.erase(node.setVar);
    for (const auto& [varName, rule] : node.capture) {
        (void)rule;
        bt.vars_.erase(varName);
    }

    for (const auto& c : node.children) ResetSubtreeForRun(bt, *c, resetHistory);
    if (node.condChild) ResetSubtreeForRun(bt, *node.condChild, resetHistory);
    if (node.thenChild) ResetSubtreeForRun(bt, *node.thenChild, resetHistory);
    if (node.elseChild) ResetSubtreeForRun(bt, *node.elseChild, resetHistory);
    if (node.singleChild) ResetSubtreeForRun(bt, *node.singleChild, resetHistory);
    if (node.untilChild) ResetSubtreeForRun(bt, *node.untilChild, resetHistory);
    if (node.otherwiseChild) ResetSubtreeForRun(bt, *node.otherwiseChild, resetHistory);
    for (const auto& h : node.catchHandlers) {
        if (h.doNode) ResetSubtreeForRun(bt, *h.doNode, resetHistory);
    }
}

void BehaviorTree::Impl::CancelSubtreeJobs(BehaviorTree& bt, BtNode& node, bool markSkipped) {
    if (!node.activeJobId.empty()) {
        std::string jid = node.activeJobId;
        node.activeJobId.clear();
        runningJobs.erase(jid);
        if (bt.runner_) bt.runner_->CancelJob(jid);
    }
    if (!node.fallbackJobId.empty()) {
        std::string jid = node.fallbackJobId;
        node.fallbackJobId.clear();
        runningJobs.erase(jid);
        if (bt.runner_) bt.runner_->CancelJob(jid);
    }
    node.waitDeadlineMs = 0;
    node.timeoutDeadlineMs = 0;
    node.retryDeadlineMs = 0;
    node.escalateDeadlineMs = 0;
    node.fallbackDeadlineMs = 0;

    if (markSkipped && node.state == "running") {
        node.state = "skipped";
        node.detail = "cancelled";
    }

    for (const auto& c : node.children) CancelSubtreeJobs(bt, *c, markSkipped);
    if (node.condChild) CancelSubtreeJobs(bt, *node.condChild, markSkipped);
    if (node.thenChild) CancelSubtreeJobs(bt, *node.thenChild, markSkipped);
    if (node.elseChild) CancelSubtreeJobs(bt, *node.elseChild, markSkipped);
    if (node.singleChild) CancelSubtreeJobs(bt, *node.singleChild, markSkipped);
    if (node.untilChild) CancelSubtreeJobs(bt, *node.untilChild, markSkipped);
    if (node.otherwiseChild) CancelSubtreeJobs(bt, *node.otherwiseChild, markSkipped);
    for (const auto& h : node.catchHandlers) {
        if (h.doNode) CancelSubtreeJobs(bt, *h.doNode, markSkipped);
    }
}

bool BehaviorTree::Impl::HasWaitingEscalation(const BtNode& node) const {
    if (node.type == "escalate" && node.state == "running") return true;
    for (const auto& c : node.children) {
        if (HasWaitingEscalation(*c)) return true;
    }
    if (node.condChild && HasWaitingEscalation(*node.condChild)) return true;
    if (node.thenChild && HasWaitingEscalation(*node.thenChild)) return true;
    if (node.elseChild && HasWaitingEscalation(*node.elseChild)) return true;
    if (node.singleChild && HasWaitingEscalation(*node.singleChild)) return true;
    if (node.untilChild && HasWaitingEscalation(*node.untilChild)) return true;
    if (node.otherwiseChild && HasWaitingEscalation(*node.otherwiseChild)) return true;
    for (const auto& h : node.catchHandlers) {
        if (h.doNode && HasWaitingEscalation(*h.doNode)) return true;
    }
    return false;
}

namespace {

// Estimates are display hints, not scheduling input. Cap them so a repeated retry cannot
// produce an absurd countdown; one year is far beyond any BT wall-clock limit.
constexpr int64_t kMaxEstimateSec = 366LL * 24 * 3600;

int64_t ClampEstimateSec(int64_t sec) {
    if (sec <= 0) return 0;
    return sec > kMaxEstimateSec ? kMaxEstimateSec : sec;
}

bool IsFinalNodeState(const BtNode& node) {
    return node.state == "succeeded" || node.state == "failed" || node.state == "skipped";
}

int64_t LeafBudgetSec(const BtNode& node) {
    if (node.type == "job" || node.type == "worker") {
        return node.estimateSec > 0 ? node.estimateSec : node.timeoutSec;
    }
    if (node.type == "probe") return node.timeoutSec;
    if (node.type == "wait") return node.sec;
    return 0;
}

int64_t FullBudgetSec(const BtNode& node);

int64_t RepeatBudgetSec(const BtNode& node, int64_t childBudget) {
    int count = node.maxCount > 0 ? node.maxCount : 1;
    int64_t total = childBudget * count;
    if (node.type == "retry" && node.delaySec > 0) {
        total += static_cast<int64_t>(node.delaySec) * (count - 1);
    }
    return ClampEstimateSec(total);
}

// Best-case duration of one complete run of the node, used for future retry/repeat attempts.
int64_t FullBudgetSec(const BtNode& node) {
    const std::string& t = node.type;
    if (t == "job" || t == "worker" || t == "probe" || t == "wait") return ClampEstimateSec(LeafBudgetSec(node));
    if (t == "sequence") {
        int64_t sum = 0;
        for (const auto& c : node.children) sum += FullBudgetSec(*c);
        return ClampEstimateSec(sum);
    }
    if (t == "fallback") {
        // A fallback normally succeeds on its first child; later children only run on failure.
        for (const auto& c : node.children) return FullBudgetSec(*c);
        return 0;
    }
    if (t == "parallel") {
        int64_t longest = 0;
        for (const auto& c : node.children) longest = (std::max)(longest, FullBudgetSec(*c));
        return ClampEstimateSec(longest);
    }
    if (t == "if") {
        if (node.thenChild) return FullBudgetSec(*node.thenChild);
        if (node.elseChild) return FullBudgetSec(*node.elseChild);
        return 0;
    }
    if (t == "retry" || t == "repeat") {
        return node.singleChild ? RepeatBudgetSec(node, FullBudgetSec(*node.singleChild)) : 0;
    }
    if (t == "catch") {
        int64_t childBudget = node.singleChild ? FullBudgetSec(*node.singleChild) : 0;
        int64_t handlerBudget = 0;
        for (const auto& h : node.catchHandlers) {
            if (h.doNode) handlerBudget = (std::max)(handlerBudget, FullBudgetSec(*h.doNode));
        }
        int retries = node.catchMaxRetries > 0 ? node.catchMaxRetries : 1;
        return ClampEstimateSec(childBudget * (retries + 1) + handlerBudget * retries);
    }
    if (t == "timeout") {
        int64_t childBudget = node.singleChild ? FullBudgetSec(*node.singleChild) : 0;
        if (node.sec <= 0) return childBudget;
        if (childBudget <= 0 || node.sec < childBudget) return ClampEstimateSec(node.sec);
        return childBudget;
    }
    if (t == "invert" || t == "forceSuccess" || t == "forceFailure") {
        return node.singleChild ? FullBudgetSec(*node.singleChild) : 0;
    }
    return 0;
}

// Work left before the tree finishes this branch, counting only leaves that have not started.
int64_t PendingNodeSec(const BtNode& node) {
    if (IsFinalNodeState(node)) return 0;
    const std::string& t = node.type;
    if (t == "job" || t == "worker" || t == "probe" || t == "wait") {
        // Running jobs are already part of the task queue's remaining time.
        return node.state == "idle" ? ClampEstimateSec(LeafBudgetSec(node)) : 0;
    }
    if (t == "sequence") {
        int64_t sum = 0;
        for (const auto& c : node.children) {
            if (c->state == "failed") return 0;
            sum += PendingNodeSec(*c);
        }
        return ClampEstimateSec(sum);
    }
    if (t == "fallback") {
        for (const auto& c : node.children) {
            if (c->state == "succeeded") return 0;
            if (c->state == "failed" || c->state == "skipped") continue;
            return PendingNodeSec(*c);
        }
        return 0;
    }
    if (t == "parallel") {
        int64_t longest = 0;
        for (const auto& c : node.children) longest = (std::max)(longest, PendingNodeSec(*c));
        return ClampEstimateSec(longest);
    }
    if (t == "if") {
        const BtNode* branch = (node.ifBranch == 2) ? node.elseChild.get() : node.thenChild.get();
        if (!branch) branch = node.elseChild.get();
        return branch ? PendingNodeSec(*branch) : 0;
    }
    if (t == "retry" || t == "repeat") {
        if (!node.singleChild) return 0;
        if (node.state == "idle") return FullBudgetSec(node);
        int count = node.maxCount > 0 ? node.maxCount : 1;
        int runs = node.runs > 0 ? node.runs : 0;
        int64_t futureAttempts = (count > runs) ? static_cast<int64_t>(count - runs) : 0;
        int64_t childBudget = FullBudgetSec(*node.singleChild);
        int64_t total = PendingNodeSec(*node.singleChild) + futureAttempts * childBudget;
        if (t == "retry" && node.delaySec > 0) {
            total += static_cast<int64_t>(node.delaySec) * futureAttempts;
        }
        return ClampEstimateSec(total);
    }
    if (t == "invert" || t == "forceSuccess" || t == "forceFailure" || t == "timeout") {
        return node.singleChild ? PendingNodeSec(*node.singleChild) : 0;
    }
    if (t == "catch") {
        if (node.catchPhase == 3 && node.otherwiseChild) return PendingNodeSec(*node.otherwiseChild);
        int64_t pending = 0;
        bool willRetry = true;
        if (node.catchPhase == 1 && node.catchHandlerIndex >= 0 &&
            static_cast<size_t>(node.catchHandlerIndex) < node.catchHandlers.size()) {
            const auto& h = node.catchHandlers[static_cast<size_t>(node.catchHandlerIndex)];
            if (h.doNode) pending += PendingNodeSec(*h.doNode);
            willRetry = h.thenRetry;
        }
        if (node.singleChild) pending += PendingNodeSec(*node.singleChild);
        int retries = node.catchMaxRetries > 0 ? node.catchMaxRetries : 1;
        int remaining = willRetry ? (retries - node.catchRetries) : 0;
        if (remaining <= 0) {
            return ClampEstimateSec(pending + (node.otherwiseChild ? PendingNodeSec(*node.otherwiseChild) : 0));
        }
        int64_t childBudget = node.singleChild ? FullBudgetSec(*node.singleChild) : 0;
        int64_t handlerBudget = 0;
        for (const auto& h : node.catchHandlers) {
            if (h.doNode) handlerBudget = (std::max)(handlerBudget, FullBudgetSec(*h.doNode));
        }
        return ClampEstimateSec(pending + static_cast<int64_t>(remaining) * (childBudget + handlerBudget));
    }
    return 0;
}

void CollectPlannedTasks(const BtNode& node, const BehaviorTree& bt,
                         std::vector<BtPlannedTask>& out) {
    if (IsFinalNodeState(node)) return;
    const std::string& t = node.type;
    if (t == "job" || t == "worker") {
        if (node.state != "idle") return;
        BtPlannedTask task;
        task.btId = bt.Id();
        task.nodeKey = node.nodeKey;
        task.type = t;
        task.summary = node.summary;
        task.queueName = node.queueName.empty() ? bt.TaskQueueName() : node.queueName;
        task.proposedJobId = bt.Id() + "-" + node.nodeKey + "-" + std::to_string(node.launchSeq + 1);
        task.estimateSec = static_cast<int>(ClampEstimateSec(LeafBudgetSec(node)));
        out.push_back(std::move(task));
        return;
    }
    if (t == "sequence") {
        for (const auto& c : node.children) CollectPlannedTasks(*c, bt, out);
        return;
    }
    if (t == "fallback") {
        // Only the first child that can still run is provisional; later children wait for it
        // to fail, and a branch change drops them from the next query.
        for (const auto& c : node.children) {
            if (c->state == "succeeded") return;
            if (c->state == "failed" || c->state == "skipped") continue;
            CollectPlannedTasks(*c, bt, out);
            return;
        }
        return;
    }
    if (t == "parallel") {
        for (const auto& c : node.children) CollectPlannedTasks(*c, bt, out);
        return;
    }
    if (t == "if") {
        const BtNode* branch = (node.ifBranch == 2) ? node.elseChild.get() : node.thenChild.get();
        if (!branch) branch = node.elseChild.get();
        if (branch) CollectPlannedTasks(*branch, bt, out);
        return;
    }
    if (t == "catch") {
        if (node.catchPhase == 3 && node.otherwiseChild) {
            CollectPlannedTasks(*node.otherwiseChild, bt, out);
            return;
        }
        if (node.catchPhase == 1 && node.catchHandlerIndex >= 0 &&
            static_cast<size_t>(node.catchHandlerIndex) < node.catchHandlers.size()) {
            const auto& h = node.catchHandlers[static_cast<size_t>(node.catchHandlerIndex)];
            if (h.doNode) CollectPlannedTasks(*h.doNode, bt, out);
            return;
        }
        if (node.singleChild) CollectPlannedTasks(*node.singleChild, bt, out);
        return;
    }
    if (node.singleChild) {
        CollectPlannedTasks(*node.singleChild, bt, out);
    }
}

} // namespace

int64_t BehaviorTree::PendingEstimateSec() const {
    if (!impl_ || !impl_->root) return 0;
    return PendingNodeSec(*impl_->root);
}

std::vector<BtPlannedTask> BehaviorTree::PlannedTasks() const {
    std::vector<BtPlannedTask> out;
    if (!impl_ || !impl_->root) return out;
    CollectPlannedTasks(*impl_->root, *this, out);
    return out;
}

} // namespace pc
