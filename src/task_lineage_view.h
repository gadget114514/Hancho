#pragma once

#include "nlohmann/json.hpp"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pc {

enum class TaskDisplayRowKind {
    TaskHeading,
    Job,
    MoreJobs
};

struct TaskDisplayRow {
    TaskDisplayRowKind kind = TaskDisplayRowKind::Job;
    std::string taskId;
    std::string jobId;
    int indent = 0;
    bool expanded = false;
    bool isSuperseded = false;
    bool hasTaskRecord = false;
    std::string taskTitle;
    std::string taskState;
    std::string resultsCompact;
    int jobCount = 0;
    int hiddenJobCount = 0;
    std::string supersedes;
    std::string supersededBy;
    nlohmann::json jobJson;
};

struct TaskViewCounts {
    int totalTasks = 0;
    int runningTasks = 0;
    int waitingTasks = 0;
    int succeededTasks = 0;
    int failedTasks = 0;
    int cancelledTasks = 0;
    int closedTasks = 0;

    int totalJobs = 0;
    int runningJobs = 0;
    int waitingJobs = 0;
    int succeededJobs = 0;
    int failedJobs = 0;
    int cancelledJobs = 0;
};

inline bool IsVirtualTaskRowId(const std::string& id) {
    return id.rfind("group:", 0) == 0 || id.rfind("btplan:", 0) == 0;
}

inline std::string ShortJobRef(const std::string& jobId) {
    if (jobId.empty()) return "";
    std::string tail = jobId;
    if (tail.rfind("job_", 0) == 0 || tail.rfind("job-", 0) == 0) {
        tail = tail.substr(4);
    }
    bool allDigits = !tail.empty();
    for (unsigned char c : tail) {
        if (!std::isdigit(c)) {
            allDigits = false;
            break;
        }
    }
    if (allDigits) return "#" + tail;
    return jobId;
}

inline std::string JobStateSymbol(const std::string& state) {
    if (state == "succeeded") return "\xE2\x9C\x93"; // ✓
    if (state == "failed" || state == "blocked") return "\xE2\x9C\x97"; // ✗
    if (state == "running" || state == "waitingForLlm" || state == "paused") return "\xE2\x96\xB6"; // ▶
    if (state == "cancelled" || state == "closed" || state == "takenOver") return "\xE2\x88\x92"; // −
    return "\xC2\xB7"; // · (pending, waiting, planned, etc.)
}

// Formats chronological job states as a compact result string:
// e.g. "✗✗✓" when <= maxRawLen, or "✗×6 ✓" when longer.
inline std::string FormatTaskResultsCompact(const std::vector<std::string>& jobStates, size_t maxRawLen = 8) {
    if (jobStates.empty()) return "";
    if (jobStates.size() <= maxRawLen) {
        std::string out;
        for (const auto& s : jobStates) out += JobStateSymbol(s);
        return out;
    }
    std::string out;
    size_t i = 0;
    while (i < jobStates.size()) {
        const std::string sym = JobStateSymbol(jobStates[i]);
        size_t j = i + 1;
        while (j < jobStates.size() && JobStateSymbol(jobStates[j]) == sym) ++j;
        const size_t run = j - i;
        if (!out.empty()) out += ' ';
        out += sym;
        if (run > 1) {
            out += "\xC3\x97" + std::to_string(run); // ×N
        }
        i = j;
    }
    return out;
}

inline bool DefaultTaskExpanded(const std::string& taskState) {
    return taskState == "running" || taskState == "failed" || taskState == "open";
}

inline std::string ComputeImplicitTaskState(const std::vector<nlohmann::json>& jobs) {
    if (jobs.empty()) return "open";
    std::unordered_set<std::string> byIds;
    for (const auto& j : jobs) {
        const std::string id = j.value("id", std::string());
        if (!id.empty()) byIds.insert(id);
    }
    bool anyRunning = false;
    bool anyFailed = false;
    bool anyCancelled = false;
    for (const auto& j : jobs) {
        const std::string supBy = j.value("superseded_by", std::string());
        if (!supBy.empty() && byIds.count(supBy)) continue;
        const std::string st = j.value("state", std::string());
        if (st == "pending" || st == "waiting" || st == "running" || st == "planned" ||
            st == "waitingForPermit" || st == "waitingForLlm" || st == "paused") {
            anyRunning = true;
        } else if (st == "takenOver") {
            std::string res;
            if (j.contains("takeover") && j["takeover"].is_object()) {
                res = j["takeover"].value("result", std::string());
            }
            if (res.empty()) anyRunning = true;
            else if (res == "failed") anyFailed = true;
        } else if (st == "failed" || st == "blocked" || st == "closed") {
            anyFailed = true;
        } else if (st == "cancelled") {
            anyCancelled = true;
        }
    }
    if (anyRunning) return "running";
    if (anyFailed) return "failed";
    if (anyCancelled) return "cancelled";
    return "succeeded";
}

inline int TaskStateSortRank(const std::string& state) {
    if (state == "running") return 0;
    if (state == "failed") return 1;
    if (state == "open") return 2;
    if (state == "cancelled") return 3;
    if (state == "succeeded") return 4;
    if (state == "closed") return 5;
    return 6;
}

inline std::vector<TaskDisplayRow> BuildTaskDisplayRows(
    const std::vector<nlohmann::json>& jobs,
    const std::vector<nlohmann::json>& tasks,
    bool taskViewMode,
    const std::unordered_map<std::string, bool>& expandedOverrides,
    const std::unordered_set<std::string>& showAllJobsTasks,
    TaskViewCounts& outCounts,
    size_t maxVisibleJobsPerTask = 8) {
    outCounts = TaskViewCounts{};

    // Index explicit task records from task_list.
    struct TaskMeta {
        std::string id;
        std::string title;
        std::string state;
        std::string sortTs;
        bool hasRecord = false;
        std::vector<nlohmann::json> jobs;
    };
    std::unordered_map<std::string, TaskMeta> taskMap;
    std::vector<std::string> taskOrder;

    auto ensureTask = [&](const std::string& id) -> TaskMeta& {
        auto it = taskMap.find(id);
        if (it == taskMap.end()) {
            TaskMeta m;
            m.id = id;
            auto inserted = taskMap.emplace(id, std::move(m));
            taskOrder.push_back(id);
            return inserted.first->second;
        }
        return it->second;
    };

    for (const auto& t : tasks) {
        if (!t.is_object()) continue;
        const std::string id = t.value("id", std::string());
        if (id.empty()) continue;
        TaskMeta& m = ensureTask(id);
        m.hasRecord = true;
        m.title = t.value("title", std::string());
        m.state = t.value("state", std::string());
        m.sortTs = t.value("updatedAt", t.value("createdAt", std::string()));
    }

    // Build supersedes -> root lookup for implicit lineage grouping.
    std::unordered_map<std::string, std::string> supersedesMap;
    std::unordered_map<std::string, std::string> explicitTaskMap;
    for (const auto& j : jobs) {
        if (!j.is_object()) continue;
        const std::string jid = j.value("id", std::string());
        const std::string sup = j.value("supersedes", std::string());
        const std::string tid = j.value("task_id", std::string());
        if (!jid.empty() && !sup.empty()) supersedesMap[jid] = sup;
        if (!jid.empty() && !tid.empty()) explicitTaskMap[jid] = tid;
    }
    auto resolveEffectiveTaskId = [&](const nlohmann::json& j) -> std::string {
        const std::string jid = j.value("id", std::string());
        const std::string tid = j.value("task_id", std::string());
        if (!tid.empty()) return tid;
        std::string cur = jid;
        for (int step = 0; step < 256; ++step) {
            auto eit = explicitTaskMap.find(cur);
            if (eit != explicitTaskMap.end()) return eit->second;
            auto sit = supersedesMap.find(cur);
            if (sit == supersedesMap.end() || sit->second.empty() || sit->second == cur) break;
            cur = sit->second;
        }
        if (j.contains("task") && j["task"].is_object()) {
            const std::string embeddedTid = j["task"].value("id", std::string());
            if (!embeddedTid.empty() && !IsVirtualTaskRowId(jid)) {
                // Only use embedded task id if there was no earlier supersedes root found.
                if (cur == jid) return embeddedTid;
            }
        }
        return cur;
    };

    // Assign jobs to tasks and compute job-level counts.
    for (const auto& j : jobs) {
        if (!j.is_object()) continue;
        const std::string jid = j.value("id", std::string());
        const bool isVirtual = IsVirtualTaskRowId(jid);
        if (!isVirtual) {
            ++outCounts.totalJobs;
            const std::string st = j.value("state", std::string());
            if (st == "running" || st == "waitingForLlm" || st == "paused") ++outCounts.runningJobs;
            else if (st == "pending" || st == "waiting") ++outCounts.waitingJobs;
            else if (st == "succeeded") ++outCounts.succeededJobs;
            else if (st == "failed" || st == "blocked") ++outCounts.failedJobs;
            else if (st == "cancelled") ++outCounts.cancelledJobs;
        }

        const std::string effTaskId = resolveEffectiveTaskId(j);
        TaskMeta& m = ensureTask(effTaskId);
        if (!m.hasRecord && j.contains("task") && j["task"].is_object()) {
            if (m.title.empty()) m.title = j["task"].value("title", std::string());
            if (m.state.empty()) m.state = j["task"].value("state", std::string());
        }
        const std::string jTs = j.value("updated_at", j.value("created_at", std::string()));
        if (jTs > m.sortTs) m.sortTs = jTs;
        m.jobs.push_back(j);
    }

    // Compute task-level states & counts.
    for (const auto& id : taskOrder) {
        TaskMeta& m = taskMap[id];
        std::vector<nlohmann::json> realJobs;
        for (const auto& j : m.jobs) {
            if (!IsVirtualTaskRowId(j.value("id", std::string()))) realJobs.push_back(j);
        }
        if (m.state.empty()) {
            if (!realJobs.empty() || m.jobs.empty()) {
                m.state = ComputeImplicitTaskState(realJobs);
            } else {
                m.state = m.jobs[0].value("state", std::string("open"));
            }
        }
        if (!m.hasRecord && realJobs.empty()) continue; // pure virtual BT/btplan row
        ++outCounts.totalTasks;
        if (m.state == "running") ++outCounts.runningTasks;
        else if (m.state == "open") ++outCounts.waitingTasks;
        else if (m.state == "succeeded") ++outCounts.succeededTasks;
        else if (m.state == "failed") ++outCounts.failedTasks;
        else if (m.state == "cancelled") ++outCounts.cancelledTasks;
        else if (m.state == "closed") ++outCounts.closedTasks;
    }

    std::vector<TaskDisplayRow> out;
    if (!taskViewMode) {
        out.reserve(jobs.size());
        for (const auto& j : jobs) {
            if (!j.is_object()) continue;
            TaskDisplayRow row;
            row.kind = TaskDisplayRowKind::Job;
            row.jobId = j.value("id", std::string());
            row.taskId = resolveEffectiveTaskId(j);
            const auto it = taskMap.find(row.taskId);
            if (it != taskMap.end()) {
                row.hasTaskRecord = it->second.hasRecord;
                row.taskTitle = it->second.title;
                row.taskState = it->second.state;
            }
            row.supersedes = j.value("supersedes", std::string());
            row.supersededBy = j.value("superseded_by", std::string());
            row.isSuperseded = !row.supersededBy.empty();
            row.jobJson = j;
            out.push_back(std::move(row));
        }
        return out;
    }

    // Sort tasks by state rank, then newest timestamp descending.
    // Pure virtual BT/btplan rows (no task record and virtual id) sort after tasks.
    std::stable_sort(taskOrder.begin(), taskOrder.end(), [&](const std::string& aId, const std::string& bId) {
        const TaskMeta& a = taskMap[aId];
        const TaskMeta& b = taskMap[bId];
        const bool aVirt = !a.hasRecord && IsVirtualTaskRowId(a.id);
        const bool bVirt = !b.hasRecord && IsVirtualTaskRowId(b.id);
        if (aVirt != bVirt) return !aVirt;
        const int ra = TaskStateSortRank(a.state);
        const int rb = TaskStateSortRank(b.state);
        if (ra != rb) return ra < rb;
        if (a.sortTs != b.sortTs) return a.sortTs > b.sortTs;
        return a.id < b.id;
    });

    for (const auto& id : taskOrder) {
        const TaskMeta& m = taskMap[id];
        // Implicit task with 1 job (no task record): no heading row.
        if (!m.hasRecord && m.jobs.size() == 1) {
            const auto& j = m.jobs[0];
            TaskDisplayRow row;
            row.kind = TaskDisplayRowKind::Job;
            row.taskId = id;
            row.jobId = j.value("id", std::string());
            row.indent = 0;
            row.hasTaskRecord = false;
            row.taskTitle = m.title;
            row.taskState = m.state;
            row.supersedes = j.value("supersedes", std::string());
            row.supersededBy = j.value("superseded_by", std::string());
            row.isSuperseded = !row.supersededBy.empty();
            row.jobJson = j;
            out.push_back(std::move(row));
            continue;
        }

        // Chronological order (oldest -> newest) for compact results string.
        std::vector<nlohmann::json> chronoJobs = m.jobs;
        std::stable_sort(chronoJobs.begin(), chronoJobs.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
            return a.value("created_at", std::string()) < b.value("created_at", std::string());
        });
        std::vector<std::string> realStates;
        for (const auto& j : chronoJobs) {
            if (!IsVirtualTaskRowId(j.value("id", std::string()))) {
                realStates.push_back(j.value("state", std::string()));
            }
        }

        bool expanded = DefaultTaskExpanded(m.state);
        auto ovIt = expandedOverrides.find(id);
        if (ovIt != expandedOverrides.end()) expanded = ovIt->second;

        TaskDisplayRow hdr;
        hdr.kind = TaskDisplayRowKind::TaskHeading;
        hdr.taskId = id;
        hdr.indent = 0;
        hdr.expanded = expanded;
        hdr.hasTaskRecord = m.hasRecord;
        hdr.taskTitle = m.title;
        hdr.taskState = m.state;
        hdr.resultsCompact = FormatTaskResultsCompact(realStates);
        hdr.jobCount = static_cast<int>(realStates.size());
        out.push_back(std::move(hdr));

        if (!expanded || m.jobs.empty()) continue;

        // Display child jobs newest-first.
        std::vector<nlohmann::json> descJobs(chronoJobs.rbegin(), chronoJobs.rend());
        const bool showAll = (maxVisibleJobsPerTask == 0) ||
                             (descJobs.size() <= maxVisibleJobsPerTask) ||
                             (showAllJobsTasks.count(id) > 0);
        const size_t visibleCount = showAll ? descJobs.size() : maxVisibleJobsPerTask;
        for (size_t i = 0; i < visibleCount; ++i) {
            const auto& j = descJobs[i];
            TaskDisplayRow child;
            child.kind = TaskDisplayRowKind::Job;
            child.taskId = id;
            child.jobId = j.value("id", std::string());
            child.indent = 1;
            child.hasTaskRecord = m.hasRecord;
            child.taskTitle = m.title;
            child.taskState = m.state;
            child.supersedes = j.value("supersedes", std::string());
            child.supersededBy = j.value("superseded_by", std::string());
            child.isSuperseded = !child.supersededBy.empty();
            child.jobJson = j;
            out.push_back(std::move(child));
        }
        if (!showAll && descJobs.size() > visibleCount) {
            TaskDisplayRow more;
            more.kind = TaskDisplayRowKind::MoreJobs;
            more.taskId = id;
            more.indent = 1;
            more.hasTaskRecord = m.hasRecord;
            more.taskTitle = m.title;
            more.taskState = m.state;
            more.hiddenJobCount = static_cast<int>(descJobs.size() - visibleCount);
            out.push_back(std::move(more));
        }
    }
    return out;
}

} // namespace pc
