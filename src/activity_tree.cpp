#include "activity_tree.h"

#include <algorithm>
#include <map>
#include <set>

namespace pc {

namespace {

// Kinds that never merge with their neighbours: each thought or reply is its own row.
bool IsMergeableKind(const std::string& kind) {
    return kind != "thinking" && kind != "replying" && kind != "error" && kind != "done" &&
           kind != "subagent" && kind != "waiting_model";
}

// A child of one agent: an activity row, or a sub-agent branch (branchIndex into the sub-agent list).
struct TreeItem {
    int64_t sortMs = 0;
    int branchIndex = -1;
    ActivityTreeRow row;
};

class TreeBuilder {
public:
    TreeBuilder(const std::vector<LlmSubagentState>& subagents, int64_t nowMs, bool running,
                const ActivityTreeOverrides& overrides)
        : subs_(subagents), nowMs_(nowMs), running_(running), overrides_(overrides) {
        for (const auto& s : subs_) {
            known_.insert(s.id);
            if (!s.toolUseId.empty()) spawnedBy_.insert(s.toolUseId);
        }
    }

    void AddActivities(const std::vector<LlmStreamEvent>& events) {
        for (int i = 0; i < static_cast<int>(events.size()); ++i) {
            const auto& ev = events[i];
            if (ev.type == "subagent_start" || ev.type == "subagent_end") continue;
            std::string kind, target;
            // The Agent call and its result are the sub-agent's branch row.
            if (!ev.toolUseId.empty() && spawnedBy_.count(ev.toolUseId)) continue;
            if (!DescribeLlmActivity(ev, kind, target)) continue;
            const std::string owner = ev.agentId;
            if (!owner.empty() && !known_.count(owner)) continue;  // its sub-agent left the list
            auto& list = children_[owner];
            // The previous step ran until now. "Waiting for the model" and a turn's end only show
            // while they are the newest step; otherwise they would split every run of tool calls
            // and claim "done" in the middle of a run.
            if (!list.empty()) {
                list.back().row.endEpochMs = ev.recvEpochMs;
                if (list.back().row.kind == "waiting_model" || list.back().row.kind == "done") list.pop_back();
            }
            if (kind == "done" && running_ && AnySubagentRunning()) continue;  // the run goes on
            if (IsMergeableKind(kind) && !list.empty() && list.back().branchIndex < 0 &&
                list.back().row.kind == kind) {
                list.back().row.count++;
                list.back().row.target = target;
                list.back().row.seq = ev.seq;
                list.back().row.lastEvent = i;
                continue;
            }
            TreeItem item;
            item.sortMs = ev.recvEpochMs;
            item.row.isBranch = false;
            item.row.agentId = owner;
            item.row.kind = kind;
            item.row.target = target;
            item.row.description = DescribeLlmNote(ev);
            item.row.sinceEpochMs = ev.recvEpochMs;
            item.row.endEpochMs = ev.recvEpochMs;
            item.row.seq = ev.seq;
            item.row.firstSeq = ev.seq;
            item.row.firstEvent = item.row.lastEvent = i;
            list.push_back(item);
        }
    }

    void AddBranches() {
        for (int i = 0; i < static_cast<int>(subs_.size()); ++i) {
            TreeItem item;
            item.sortMs = subs_[i].startedEpochMs;
            item.branchIndex = i;
            children_[OwnerOf(subs_[i])].push_back(item);
        }
        for (auto& entry : children_) {
            std::stable_sort(entry.second.begin(), entry.second.end(),
                             [](const TreeItem& a, const TreeItem& b) { return a.sortMs < b.sortMs; });
        }
    }

    std::vector<ActivityTreeRow> Emit() {
        EmitChildren("", 0);
        return std::move(out_);
    }

private:
    bool AnySubagentRunning() const {
        for (const auto& s : subs_) {
            if (s.status == "running") return true;
        }
        return false;
    }

    // The sub-agent that spawned this one, or "" for the root agent (also when that parent left the list).
    std::string OwnerOf(const LlmSubagentState& s) const {
        return known_.count(s.parentAgentId) ? s.parentAgentId : std::string();
    }

    // Any sub-agent below this one is still running or did not complete.
    bool HasActiveDescendant(const std::string& id, int depth) const {
        if (depth > 64) return false;
        for (const auto& s : subs_) {
            if (OwnerOf(s) != id) continue;
            if (s.status != "completed") return true;
            if (HasActiveDescendant(s.id, depth + 1)) return true;
        }
        return false;
    }

    bool IsOpen(const LlmSubagentState& s) const {
        auto ov = overrides_.find(s.id);
        if (ov != overrides_.end()) return ov->second;
        if (s.status != "completed") return true;  // running, failed, killed: stay open
        if (HasActiveDescendant(s.id, 0)) return true;
        if (s.endedEpochMs <= 0) return true;
        return nowMs_ - s.endedEpochMs < kActivityTreeCloseDelayMs;
    }

    void EmitChildren(const std::string& owner, int depth) {
        auto it = children_.find(owner);
        if (it == children_.end()) return;
        for (const auto& item : it->second) {
            if (item.branchIndex < 0) {
                // The tree shows sub-agents only: the root agent's own steps are left out.
                if (owner.empty()) continue;
                ActivityTreeRow row = item.row;
                row.depth = depth;
                out_.push_back(row);
                continue;
            }
            const LlmSubagentState& s = subs_[item.branchIndex];
            ActivityTreeRow row;
            row.depth = depth;
            row.isBranch = true;
            row.agentId = s.id;
            row.kind = (s.status == "running" && !running_) ? std::string("stopped") : s.status;
            row.target = s.description;
            row.subagentType = s.subagentType;
            row.sinceEpochMs = s.startedEpochMs;
            row.endEpochMs = s.endedEpochMs;
            row.tokens = s.totalTokens;
            row.toolUses = s.toolUses;
            row.open = IsOpen(s);
            row.manual = overrides_.count(s.id) > 0;
            out_.push_back(row);
            if (row.open && depth < 64) EmitChildren(s.id, depth + 1);
        }
    }

    const std::vector<LlmSubagentState>& subs_;
    int64_t nowMs_;
    bool running_;
    const ActivityTreeOverrides& overrides_;
    std::set<std::string> known_;
    std::set<std::string> spawnedBy_;  // tool_use ids of the Agent calls that started a listed sub-agent
    std::map<std::string, std::vector<TreeItem>> children_;  // owner agent id -> children, by time
    std::vector<ActivityTreeRow> out_;
};

}  // namespace

std::vector<ActivityTreeRow> BuildActivityTree(const std::vector<LlmStreamEvent>& events,
                                               const std::vector<LlmSubagentState>& subagents,
                                               int64_t nowMs, bool running,
                                               const ActivityTreeOverrides& overrides) {
    TreeBuilder builder(subagents, nowMs, running, overrides);
    builder.AddActivities(events);
    builder.AddBranches();
    return builder.Emit();
}

}  // namespace pc
