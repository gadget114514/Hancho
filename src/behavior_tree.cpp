#include "behavior_tree_internal.h"
#include "common.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <ctime>

namespace pc {

std::wstring ResolveBtPathWide(const std::string& cwdUtf8, const std::string& relOrAbsUtf8) {
    if (relOrAbsUtf8.empty()) return ResolveAbsolutePathWide(Utf8ToWide(cwdUtf8));
    std::wstring p = Utf8ToWide(relOrAbsUtf8);
    if (p.size() >= 2 && (p[1] == L':' || (p[0] == L'\\' && p[1] == L'\\') || (p[0] == L'/' && p[1] == L'/'))) {
        return ResolveAbsolutePathWide(p);
    }
    if (!cwdUtf8.empty()) {
        std::wstring base = Utf8ToWide(cwdUtf8);
        if (!base.empty() && base.back() != L'\\' && base.back() != L'/') base.push_back(L'\\');
        return ResolveAbsolutePathWide(base + p);
    }
    return ResolveAbsolutePathWide(p);
}

std::string FormatEpochMsIso(int64_t nowMs) {
    if (nowMs <= 0) return "1970-01-01T00:00:00";
    // If nowMs looks like a small synthetic test clock (< 10^11 ms, ~1973), format deterministically in UTC.
    time_t sec = static_cast<time_t>(nowMs / 1000);
    std::tm tmv = {};
    gmtime_s(&tmv, &sec);
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

void BehaviorTree::Impl::ResetSubtreeForRun(BtNode& node, bool resetHistory) {
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
    if (resetHistory) {
        node.runs = 0;
    } else if (node.type == "retry" || node.type == "repeat" ||
               node.type == "sequence" || node.type == "fallback" ||
               node.type == "parallel" || node.type == "if") {
        node.runs = 0;
    }

    for (const auto& c : node.children) ResetSubtreeForRun(*c, resetHistory);
    if (node.condChild) ResetSubtreeForRun(*node.condChild, resetHistory);
    if (node.thenChild) ResetSubtreeForRun(*node.thenChild, resetHistory);
    if (node.elseChild) ResetSubtreeForRun(*node.elseChild, resetHistory);
    if (node.singleChild) ResetSubtreeForRun(*node.singleChild, resetHistory);
    if (node.untilChild) ResetSubtreeForRun(*node.untilChild, resetHistory);
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
    return false;
}

} // namespace pc
