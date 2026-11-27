#pragma once

#include "nlohmann/json.hpp"
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pc {

enum class TaskDisplayRowKind {
    TaskHeading,
    DirectionHeading,
    Job,
    MoreJobs,
    LoadMoreFinished
};

struct TaskDisplayRow {
    TaskDisplayRowKind kind = TaskDisplayRowKind::Job;
    std::string taskId;
    std::string directionId;
    std::string directionKind;
    std::string directionTitle;
    std::string directionState;
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
    int hiddenFinishedCount = 0;
    std::string supersedes;
    std::string supersededBy;
    bool isCard = false;
    std::string cardNumber;
    std::vector<std::string> waitingFor;
    nlohmann::json jobJson;
    std::string startTime;
    std::string endTime;
    std::string runningDetail;
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
    return id.rfind("direction:", 0) == 0 || id.rfind("group:", 0) == 0 || id.rfind("btplan:", 0) == 0;
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
    return taskState == "running" || taskState == "failed" || taskState == "open" ||
           taskState == "waitingForLlm" || taskState == "paused";
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

inline bool CompareCardNumberNatural(const std::string& a, const std::string& b) {
    size_t ia = 0;
    size_t ib = 0;
    while (ia < a.size() && ib < b.size()) {
        const bool da = std::isdigit(static_cast<unsigned char>(a[ia])) != 0;
        const bool db = std::isdigit(static_cast<unsigned char>(b[ib])) != 0;
        if (da && db) {
            size_t za = ia;
            while (za < a.size() && a[za] == '0') ++za;
            size_t ea = za;
            while (ea < a.size() && std::isdigit(static_cast<unsigned char>(a[ea]))) ++ea;

            size_t zb = ib;
            while (zb < b.size() && b[zb] == '0') ++zb;
            size_t eb = zb;
            while (eb < b.size() && std::isdigit(static_cast<unsigned char>(b[eb]))) ++eb;

            const size_t la = ea - za;
            const size_t lb = eb - zb;
            if (la != lb) return la < lb;
            const int cmp = a.compare(za, la, b, zb, lb);
            if (cmp != 0) return cmp < 0;
            if ((ea - ia) != (eb - ib)) return (ea - ia) < (eb - ib);
            ia = ea;
            ib = eb;
        } else {
            if (a[ia] != b[ib]) return static_cast<unsigned char>(a[ia]) < static_cast<unsigned char>(b[ib]);
            ++ia;
            ++ib;
        }
    }
    return a.size() < b.size();
}

inline void InitializeTaskExpandedOverrides(
    std::unordered_map<std::string, bool>& expandedOverrides,
    const std::vector<nlohmann::json>& jobs,
    const std::vector<nlohmann::json>& tasks,
    const std::vector<nlohmann::json>& directions = {}) {
    for (const auto& t : tasks) {
        if (!t.is_object()) continue;
        const std::string id = t.value("id", std::string());
        if (id.empty()) continue;
        if (expandedOverrides.find(id) == expandedOverrides.end()) {
            expandedOverrides[id] = DefaultTaskExpanded(t.value("state", std::string()));
        }
    }
    for (const auto& d : directions) {
        if (!d.is_object()) continue;
        const std::string did = d.value("id", std::string());
        if (did.empty()) continue;
        const std::string key = (did.rfind("direction:", 0) == 0) ? did : ("direction:" + did);
        if (expandedOverrides.find(key) == expandedOverrides.end()) {
            expandedOverrides[key] = DefaultTaskExpanded(d.value("state", std::string()));
        }
    }

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

    auto resolveEffTaskId = [&](const nlohmann::json& j) -> std::string {
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
                if (cur == jid) return embeddedTid;
            }
        }
        const std::string dirId = j.value("direction_id", std::string());
        if (!dirId.empty() && cur == jid) return dirId;
        if (jid.rfind("direction:", 0) == 0 && cur == jid) return jid.substr(10);
        return cur;
    };

    std::unordered_map<std::string, std::vector<nlohmann::json>> implicitTaskJobs;
    std::unordered_map<std::string, std::string> implicitTaskState;
    std::unordered_map<std::string, std::vector<nlohmann::json>> implicitDirJobs;
    std::unordered_map<std::string, std::string> implicitDirState;

    for (const auto& j : jobs) {
        if (!j.is_object()) continue;
        const std::string jid = j.value("id", std::string());

        // Direction
        std::string did = j.value("direction_id", std::string());
        if (did.empty()) {
            if (jid.rfind("direction:", 0) == 0) did = jid.substr(10);
            else if (jid.rfind("group:", 0) == 0) did = jid;
            else did = "job:" + jid;
        }
        if (!did.empty()) {
            const std::string dirKey = (did.rfind("direction:", 0) == 0) ? did : ("direction:" + did);
            if (expandedOverrides.find(dirKey) == expandedOverrides.end()) {
                implicitDirJobs[dirKey].push_back(j);
                const std::string st = j.value("state", std::string());
                if (st == "running" || st == "failed" || st == "waitingForLlm" || st == "paused" || implicitDirState[dirKey].empty()) {
                    implicitDirState[dirKey] = st;
                }
            }
        }

        // Task
        const std::string effTid = resolveEffTaskId(j);
        if (!effTid.empty() && expandedOverrides.find(effTid) == expandedOverrides.end()) {
            implicitTaskJobs[effTid].push_back(j);
            std::string st;
            if (j.contains("task") && j["task"].is_object()) {
                st = j["task"].value("state", std::string());
            }
            if (st.empty()) st = j.value("state", std::string());
            if (st == "running" || st == "failed" || st == "waitingForLlm" || st == "paused" || implicitTaskState[effTid].empty()) {
                implicitTaskState[effTid] = st;
            }
        }
    }

    for (const auto& kv : implicitDirJobs) {
        if (expandedOverrides.find(kv.first) == expandedOverrides.end()) {
            std::string st = implicitDirState[kv.first];
            if (st.empty()) st = ComputeImplicitTaskState(kv.second);
            expandedOverrides[kv.first] = DefaultTaskExpanded(st);
        }
    }
    for (const auto& kv : implicitTaskJobs) {
        if (expandedOverrides.find(kv.first) == expandedOverrides.end()) {
            std::string st = implicitTaskState[kv.first];
            if (st.empty()) st = ComputeImplicitTaskState(kv.second);
            expandedOverrides[kv.first] = DefaultTaskExpanded(st);
        }
    }
}

inline std::vector<TaskDisplayRow> BuildTaskDisplayRows(
    const std::vector<nlohmann::json>& jobs,
    const std::vector<nlohmann::json>& tasks,
    bool taskViewMode,
    const std::unordered_map<std::string, bool>& expandedOverrides,
    const std::unordered_set<std::string>& showAllJobsTasks,
    TaskViewCounts& outCounts,
    size_t maxVisibleJobsPerTask = 8,
    const std::vector<nlohmann::json>& directions = {}) {
    outCounts = TaskViewCounts{};

    // Index explicit task records from task_list.
    struct TaskMeta {
        std::string id;
        std::string title;
        std::string state;
        std::string sortTs;
        bool hasRecord = false;
        nlohmann::json taskRecord;
        std::vector<nlohmann::json> jobs;
        std::vector<nlohmann::json> directions;
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
        m.taskRecord = t;
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
        const std::string dirId = j.value("direction_id", std::string());
        if (!dirId.empty() && cur == jid) return dirId;
        if (jid.rfind("direction:", 0) == 0 && cur == jid) return jid.substr(10);
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

    // Attach explicit direction/card records to their tasks.
    for (const auto& d : directions) {
        if (!d.is_object()) continue;
        if (d.value("implicit", false)) continue;
        const std::string did = d.value("id", std::string());
        const std::string tid = d.value("taskId", std::string());
        if (did.empty() || tid.empty()) continue;
        TaskMeta& m = ensureTask(tid);
        const std::string dTs = d.value("updatedAt", d.value("createdAt", std::string()));
        if (dTs > m.sortTs) m.sortTs = dTs;
        m.directions.push_back(d);
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

        // Group jobs in this task by direction (card 103-3: 3-level task -> direction -> job).
        struct DirMeta {
            std::string id;
            std::string kind;
            std::string title;
            std::string state;
            std::string sortTs;
            bool isVirtualBt = false;
            bool isLegacyGroup = false;
            bool hasHeading = false;
            bool isCard = false;
            std::string cardNumber;
            std::string supersedes;
            std::string supersededBy;
            std::vector<std::string> waitingFor;
            nlohmann::json dirRecord;
            nlohmann::json virtualJob;
            std::vector<nlohmann::json> realJobs;
            std::vector<std::string> realStates;
            std::string startTime;
            std::string endTime;
            std::string runningDetail;
        };
        std::unordered_map<std::string, DirMeta> dirMap;
        std::vector<std::string> dirOrder;
        auto ensureDir = [&](const std::string& did) -> DirMeta& {
            auto it = dirMap.find(did);
            if (it == dirMap.end()) {
                DirMeta d;
                d.id = did;
                auto inserted = dirMap.emplace(did, std::move(d));
                dirOrder.push_back(did);
                return inserted.first->second;
            }
            return it->second;
        };

        for (const auto& dirRec : m.directions) {
            const std::string did = dirRec.value("id", std::string());
            if (did.empty()) continue;
            DirMeta& d = ensureDir(did);
            d.dirRecord = dirRec;
            const std::string k = dirRec.value("kind", std::string());
            if (!k.empty()) d.kind = k;
            const std::string t = dirRec.value("title", std::string());
            if (!t.empty()) d.title = t;
            const std::string st = dirRec.value("state", std::string());
            if (!st.empty()) d.state = st;
            d.supersedes = dirRec.value("supersedes", std::string());
            d.supersededBy = dirRec.value("supersededBy", std::string());
            if (dirRec.contains("card") && dirRec["card"].is_object()) {
                d.isCard = true;
                d.cardNumber = dirRec["card"].value("number", std::string());
                if (d.title.empty()) d.title = dirRec["card"].value("title", std::string());
            }
            if (dirRec.contains("waitingFor") && dirRec["waitingFor"].is_array()) {
                for (const auto& w : dirRec["waitingFor"]) {
                    if (w.is_string()) d.waitingFor.push_back(w.get<std::string>());
                }
            }
            const std::string dTs = dirRec.value("updatedAt", dirRec.value("createdAt", std::string()));
            if (dTs > d.sortTs) d.sortTs = dTs;
        }

        for (const auto& j : m.jobs) {
            const std::string jid = j.value("id", std::string());
            std::string did = j.value("direction_id", std::string());
            if (did.empty()) {
                if (jid.rfind("direction:", 0) == 0) did = jid.substr(10);
                else if (jid.rfind("group:", 0) == 0) did = jid;
                else did = "job:" + jid;
            }
            DirMeta& d = ensureDir(did);
            if (jid.rfind("direction:", 0) == 0 || jid.rfind("group:", 0) == 0) {
                d.isVirtualBt = true;
                d.isLegacyGroup = (jid.rfind("group:", 0) == 0);
                d.virtualJob = j;
                d.kind = "bt";
                if (d.title.empty()) d.title = j.value("summary", std::string());
                d.state = j.value("state", std::string());
                const std::string stTs = j.value("start_time", j.value("created_at", std::string()));
                if (stTs > d.sortTs) d.sortTs = stTs;
            } else {
                d.realJobs.push_back(j);
                if (d.kind.empty() && j.contains("bt") && j["bt"].is_object()) d.kind = "bt";
                if (d.kind.empty() && !j.value("direction_id", std::string()).empty()) d.kind = "jobs";
                const std::string jTs = j.value("updated_at", j.value("created_at", std::string()));
                if (jTs > d.sortTs) d.sortTs = jTs;
            }
        }

        int totalRealJobs = 0;
        std::vector<std::string> dirStatesChrono;
        for (const auto& did : dirOrder) {
            DirMeta& d = dirMap[did];
            std::stable_sort(d.realJobs.begin(), d.realJobs.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
                return a.value("created_at", std::string()) < b.value("created_at", std::string());
            });
            for (const auto& j : d.realJobs) {
                d.realStates.push_back(j.value("state", std::string()));
            }
            totalRealJobs += static_cast<int>(d.realJobs.size());
            if (d.title.empty() && !d.realJobs.empty()) {
                d.title = d.realJobs.front().value("summary", std::string());
            }
            if (d.state.empty()) {
                d.state = ComputeImplicitTaskState(d.realJobs);
            }
            if (d.kind.empty()) d.kind = "jobs";
            const bool hasExplicitRecord = !d.dirRecord.empty() && !d.dirRecord.value("implicit", false);
            d.hasHeading = d.isCard || (d.state == "planned") ||
                           (d.isVirtualBt && (!d.isLegacyGroup || !d.realJobs.empty())) ||
                           hasExplicitRecord ||
                           (d.kind == "jobs" && d.realJobs.size() >= 2) ||
                           (d.kind == "bt" && d.realJobs.size() >= 2);
            dirStatesChrono.push_back(d.state);

            // Compute startTime for DirectionHeading:
            std::string dirEarliestStart;
            auto checkAttemptStart = [&](const nlohmann::json& jobObj) {
                if (!jobObj.is_object() || !jobObj.contains("attempts") || !jobObj["attempts"].is_array()) return;
                for (const auto& a : jobObj["attempts"]) {
                    if (!a.is_object()) continue;
                    const std::string st = a.value("start", std::string());
                    if (!st.empty() && (dirEarliestStart.empty() || st < dirEarliestStart)) {
                        dirEarliestStart = st;
                    }
                }
            };
            for (const auto& j : d.realJobs) {
                checkAttemptStart(j);
            }
            checkAttemptStart(d.virtualJob);

            if (dirEarliestStart.empty()) {
                if (d.state == "planned") {
                    std::string sAt;
                    if (d.virtualJob.is_object()) {
                        sAt = d.virtualJob.value("started_at", d.virtualJob.value("start_time", std::string()));
                    }
                    if (sAt.empty() && d.dirRecord.is_object()) {
                        sAt = d.dirRecord.value("startedAt", d.dirRecord.value("started_at", std::string()));
                    }
                    dirEarliestStart = sAt;
                } else {
                    std::string sAt;
                    if (d.virtualJob.is_object()) {
                        sAt = d.virtualJob.value("started_at", d.virtualJob.value("start_time", std::string()));
                    }
                    if (sAt.empty() && d.dirRecord.is_object()) {
                        sAt = d.dirRecord.value("startedAt", d.dirRecord.value("started_at", std::string()));
                    }
                    if (sAt.empty() && d.virtualJob.is_object()) {
                        sAt = d.virtualJob.value("created_at", std::string());
                    }
                    if (sAt.empty() && d.dirRecord.is_object()) {
                        sAt = d.dirRecord.value("createdAt", d.dirRecord.value("created_at", std::string()));
                    }
                    dirEarliestStart = sAt;
                }
            }
            d.startTime = dirEarliestStart;

            // Compute endTime for DirectionHeading:
            const bool dirFinished = (d.state == "succeeded" || d.state == "failed" ||
                                      d.state == "cancelled" || d.state == "closed");
            if (dirFinished) {
                std::string closedAt;
                if (d.dirRecord.is_object() && d.dirRecord.contains("closed") && d.dirRecord["closed"].is_object()) {
                    closedAt = d.dirRecord["closed"].value("at", std::string());
                }
                if (closedAt.empty() && d.virtualJob.is_object() && d.virtualJob.contains("closed") && d.virtualJob["closed"].is_object()) {
                    closedAt = d.virtualJob["closed"].value("at", std::string());
                }
                if (!closedAt.empty()) {
                    d.endTime = closedAt;
                } else {
                    std::string dirLatestEnd;
                    auto checkJobEnd = [&](const nlohmann::json& jobObj) {
                        if (!jobObj.is_object()) return;
                        if (jobObj.contains("attempts") && jobObj["attempts"].is_array()) {
                            for (const auto& a : jobObj["attempts"]) {
                                if (!a.is_object()) continue;
                                const std::string e = a.value("end", std::string());
                                if (!e.empty() && (dirLatestEnd.empty() || e > dirLatestEnd)) {
                                    dirLatestEnd = e;
                                }
                            }
                        }
                        const std::string up = jobObj.value("updated_at", jobObj.value("updatedAt", std::string()));
                        if (!up.empty() && (dirLatestEnd.empty() || up > dirLatestEnd)) {
                            dirLatestEnd = up;
                        }
                    };
                    for (const auto& j : d.realJobs) {
                        checkJobEnd(j);
                    }
                    checkJobEnd(d.virtualJob);
                    if (dirLatestEnd.empty() && d.dirRecord.is_object()) {
                        dirLatestEnd = d.dirRecord.value("updatedAt", d.dirRecord.value("updated_at", std::string()));
                    }
                    d.endTime = dirLatestEnd;
                }
            } else {
                d.endTime = "";
            }

            // Compute runningDetail for DirectionHeading:
            std::vector<std::string> activeJobDescs;
            auto checkActiveJob = [&](const nlohmann::json& jobObj) {
                if (!jobObj.is_object()) return;
                const std::string st = jobObj.value("state", std::string());
                if (st == "running" || st == "waitingForPermit" || st == "waitingForLlm") {
                    std::string jid = jobObj.value("id", std::string());
                    std::string sum = jobObj.value("summary", std::string());
                    if (!sum.empty()) {
                        activeJobDescs.push_back(jid + ": " + sum);
                    } else if (!jid.empty()) {
                        activeJobDescs.push_back(jid);
                    }
                }
            };
            for (const auto& j : d.realJobs) {
                checkActiveJob(j);
            }
            if (activeJobDescs.empty()) {
                checkActiveJob(d.virtualJob);
            }
            std::string dRunningDetail;
            for (size_t ai = 0; ai < activeJobDescs.size(); ++ai) {
                if (ai > 0) dRunningDetail += ", ";
                dRunningDetail += activeJobDescs[ai];
            }
            d.runningDetail = dRunningDetail;
        }

        // Sort non-card directions chronologically by earliest real job or sortTs,
        // and card directions in natural card number order.
        std::sort(dirOrder.begin(), dirOrder.end(), [&](const std::string& a, const std::string& b) {
            const DirMeta& da = dirMap[a];
            const DirMeta& db = dirMap[b];
            if (da.isCard != db.isCard) return !da.isCard;
            if (da.isCard && db.isCard) {
                if (da.cardNumber != db.cardNumber) return CompareCardNumberNatural(da.cardNumber, db.cardNumber);
                return a < b;
            }
            const std::string ta = !da.realJobs.empty() ? da.realJobs.front().value("created_at", std::string()) : da.sortTs;
            const std::string tb = !db.realJobs.empty() ? db.realJobs.front().value("created_at", std::string()) : db.sortTs;
            if (ta != tb) return ta < tb;
            return a < b;
        });
        dirStatesChrono.clear();
        for (const auto& did : dirOrder) {
            dirStatesChrono.push_back(dirMap[did].state);
        }

        // Implicit single job (or legacy single group:<btId> with 0 jobs) and no task record: render flat.
        if (!m.hasRecord && m.jobs.size() == 1 && !dirMap[dirOrder[0]].hasHeading) {
            const auto& j = m.jobs[0];
            TaskDisplayRow row;
            row.kind = TaskDisplayRowKind::Job;
            row.taskId = id;
            row.directionId = dirOrder[0];
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

        // Implicit single direction with heading (e.g., taskqueue add with >= 2 jobs or BT direction without task):
        // omit redundant TaskHeading and render DirectionHeading at indent 0.
        const bool emitTaskHeading = m.hasRecord || dirOrder.size() > 1;
        bool taskExpanded = true;
        if (emitTaskHeading) {
            taskExpanded = DefaultTaskExpanded(m.state);
            auto ovIt = expandedOverrides.find(id);
            if (ovIt != expandedOverrides.end()) taskExpanded = ovIt->second;

            // Compute startTime for TaskHeading:
            std::string taskEarliestStart;
            for (const auto& did : dirOrder) {
                const DirMeta& d = dirMap[did];
                if (!d.startTime.empty() && (taskEarliestStart.empty() || d.startTime < taskEarliestStart)) {
                    taskEarliestStart = d.startTime;
                }
            }
            for (const auto& j : m.jobs) {
                if (!j.is_object()) continue;
                if (j.contains("attempts") && j["attempts"].is_array()) {
                    for (const auto& a : j["attempts"]) {
                        if (!a.is_object()) continue;
                        const std::string st = a.value("start", std::string());
                        if (!st.empty() && (taskEarliestStart.empty() || st < taskEarliestStart)) {
                            taskEarliestStart = st;
                        }
                    }
                }
            }
            if (taskEarliestStart.empty() && (m.state != "open" || !m.jobs.empty())) {
                if (m.taskRecord.is_object()) {
                    taskEarliestStart = m.taskRecord.value("createdAt", m.taskRecord.value("created_at", std::string()));
                }
            }

            // Compute endTime for TaskHeading:
            std::string taskLatestEnd;
            const bool taskFinished = (m.state == "succeeded" || m.state == "failed" ||
                                       m.state == "cancelled" || m.state == "closed");
            if (taskFinished) {
                for (const auto& did : dirOrder) {
                    const DirMeta& d = dirMap[did];
                    if (!d.endTime.empty() && (taskLatestEnd.empty() || d.endTime > taskLatestEnd)) {
                        taskLatestEnd = d.endTime;
                    }
                }
                for (const auto& j : m.jobs) {
                    if (!j.is_object()) continue;
                    if (j.contains("attempts") && j["attempts"].is_array()) {
                        for (const auto& a : j["attempts"]) {
                            if (!a.is_object()) continue;
                            const std::string e = a.value("end", std::string());
                            if (!e.empty() && (taskLatestEnd.empty() || e > taskLatestEnd)) {
                                taskLatestEnd = e;
                            }
                        }
                    }
                    const std::string up = j.value("updated_at", j.value("updatedAt", std::string()));
                    if (!up.empty() && (taskLatestEnd.empty() || up > taskLatestEnd)) {
                        taskLatestEnd = up;
                    }
                }
                std::string closedAt;
                if (m.taskRecord.is_object() && m.taskRecord.contains("closed") && m.taskRecord["closed"].is_object()) {
                    closedAt = m.taskRecord["closed"].value("at", std::string());
                }
                if (!closedAt.empty()) {
                    if (taskLatestEnd.empty() || closedAt > taskLatestEnd) {
                        taskLatestEnd = closedAt;
                    }
                }
                if (taskLatestEnd.empty() && m.taskRecord.is_object()) {
                    taskLatestEnd = m.taskRecord.value("updatedAt", m.taskRecord.value("updated_at", std::string()));
                }
            }

            // Compute runningDetail for TaskHeading. A finished or closed task clears the column:
            // a stale running direction must not keep showing work that is no longer active.
            std::vector<std::string> activeDirDescs;
            if (!taskFinished) {
                for (const auto& did : dirOrder) {
                    const DirMeta& d = dirMap[did];
                    if (d.state == "running" || d.state == "waitingForLlm" || d.state == "paused") {
                        std::string label = d.cardNumber.empty() ? (d.id.rfind("job:", 0) == 0 ? d.id.substr(4) : d.id) : d.cardNumber;
                        if (!d.title.empty()) label += ": " + d.title;
                        activeDirDescs.push_back(label);
                    }
                }
            }
            std::string tRunningDetail;
            for (size_t di = 0; di < activeDirDescs.size(); ++di) {
                if (di > 0) tRunningDetail += ", ";
                tRunningDetail += activeDirDescs[di];
            }

            TaskDisplayRow hdr;
            hdr.kind = TaskDisplayRowKind::TaskHeading;
            hdr.taskId = id;
            hdr.indent = 0;
            hdr.expanded = taskExpanded;
            hdr.hasTaskRecord = m.hasRecord;
            hdr.taskTitle = m.title;
            hdr.taskState = m.state;
            hdr.resultsCompact = FormatTaskResultsCompact(dirStatesChrono);
            hdr.jobCount = totalRealJobs;
            hdr.startTime = taskEarliestStart;
            hdr.endTime = taskLatestEnd;
            hdr.runningDetail = tRunningDetail;

            if (m.taskRecord.is_object()) {
                hdr.jobJson = m.taskRecord;
            } else {
                hdr.jobJson = nlohmann::json::object();
            }
            if (!hdr.jobJson.contains("origin")) {
                nlohmann::json taskOrigin;
                if (m.taskRecord.is_object() && m.taskRecord.contains("origin")) {
                    taskOrigin = m.taskRecord["origin"];
                } else {
                    for (const auto& j : m.jobs) {
                        if (j.is_object() && j.contains("origin")) {
                            taskOrigin = j["origin"];
                            break;
                        }
                    }
                }
                if (!taskOrigin.is_null() && !taskOrigin.empty()) {
                    hdr.jobJson["origin"] = taskOrigin;
                }
            }
            out.push_back(std::move(hdr));

            if (!taskExpanded || (m.jobs.empty() && dirOrder.empty())) continue;
        }

        const int baseIndent = emitTaskHeading ? 1 : 0;
        const bool showAllTask = (maxVisibleJobsPerTask == 0) || (showAllJobsTasks.count(id) > 0);
        size_t emittedSingleDirJobs = 0;
        int hiddenSingleDirJobs = 0;
        // Display non-card directions newest-first and card directions in natural card number order.
        std::vector<std::string> dispDirOrder;
        dispDirOrder.reserve(dirOrder.size());
        for (auto it = dirOrder.rbegin(); it != dirOrder.rend(); ++it) {
            if (!dirMap[*it].isCard) dispDirOrder.push_back(*it);
        }
        for (const auto& did : dirOrder) {
            if (dirMap[did].isCard) dispDirOrder.push_back(did);
        }
        for (const auto& did : dispDirOrder) {
            const DirMeta& d = dirMap[did];
            if (!d.hasHeading) {
                for (auto it = d.realJobs.rbegin(); it != d.realJobs.rend(); ++it) {
                    if (!showAllTask && emittedSingleDirJobs >= maxVisibleJobsPerTask) {
                        ++hiddenSingleDirJobs;
                        continue;
                    }
                    ++emittedSingleDirJobs;
                    const auto& j = *it;
                    TaskDisplayRow child;
                    child.kind = TaskDisplayRowKind::Job;
                    child.taskId = id;
                    child.directionId = did;
                    child.jobId = j.value("id", std::string());
                    child.indent = baseIndent;
                    child.hasTaskRecord = m.hasRecord;
                    child.taskTitle = m.title;
                    child.taskState = m.state;
                    child.supersedes = j.value("supersedes", std::string());
                    child.supersededBy = j.value("superseded_by", std::string());
                    child.isSuperseded = !child.supersededBy.empty();
                    child.jobJson = j;
                    out.push_back(std::move(child));
                }
                continue;
            }

            bool dirExpanded = DefaultTaskExpanded(d.state);
            auto dirOvIt = expandedOverrides.find("direction:" + did);
            if (dirOvIt == expandedOverrides.end()) dirOvIt = expandedOverrides.find(did);
            if (dirOvIt != expandedOverrides.end()) dirExpanded = dirOvIt->second;

            int redoDepth = 0;
            {
                std::string curSup = d.supersedes;
                std::unordered_set<std::string> seenSup;
                while (!curSup.empty() && seenSup.insert(curSup).second && redoDepth < 16) {
                    auto sit = dirMap.find(curSup);
                    if (sit == dirMap.end()) break;
                    ++redoDepth;
                    curSup = sit->second.supersedes;
                }
            }
            const int dirIndent = baseIndent + redoDepth;

            TaskDisplayRow dhdr;
            dhdr.kind = TaskDisplayRowKind::DirectionHeading;
            dhdr.taskId = id;
            dhdr.directionId = did;
            dhdr.directionKind = d.kind;
            dhdr.directionTitle = d.title;
            dhdr.directionState = d.state;
            dhdr.indent = dirIndent;
            dhdr.expanded = dirExpanded;
            dhdr.isSuperseded = !d.supersededBy.empty();
            dhdr.hasTaskRecord = m.hasRecord;
            dhdr.taskTitle = m.title;
            dhdr.taskState = m.state;
            dhdr.resultsCompact = FormatTaskResultsCompact(d.realStates);
            dhdr.jobCount = static_cast<int>(d.realStates.size());
            dhdr.supersedes = d.supersedes;
            dhdr.supersededBy = d.supersededBy;
            dhdr.isCard = d.isCard;
            dhdr.cardNumber = d.cardNumber;
            dhdr.waitingFor = d.waitingFor;
            dhdr.startTime = d.startTime;
            dhdr.endTime = d.endTime;
            dhdr.runningDetail = d.runningDetail;

            if (d.virtualJob.is_object()) {
                dhdr.jobJson = d.virtualJob;
            } else if (d.dirRecord.is_object()) {
                dhdr.jobJson = d.dirRecord;
            } else {
                dhdr.jobJson = nlohmann::json::object();
            }
            if (!dhdr.jobJson.contains("origin")) {
                nlohmann::json dirOrigin;
                if (d.virtualJob.is_object() && d.virtualJob.contains("origin")) {
                    dirOrigin = d.virtualJob["origin"];
                } else if (d.dirRecord.is_object() && d.dirRecord.contains("origin")) {
                    dirOrigin = d.dirRecord["origin"];
                } else {
                    for (const auto& j : d.realJobs) {
                        if (j.is_object() && j.contains("origin")) {
                            dirOrigin = j["origin"];
                            break;
                        }
                    }
                }
                if (!dirOrigin.is_null() && !dirOrigin.empty()) {
                    dhdr.jobJson["origin"] = dirOrigin;
                }
            }
            out.push_back(std::move(dhdr));

            if (!dirExpanded || d.realJobs.empty()) continue;

            std::vector<nlohmann::json> descJobs(d.realJobs.rbegin(), d.realJobs.rend());
            const bool showAll = (maxVisibleJobsPerTask == 0) ||
                                 (descJobs.size() <= maxVisibleJobsPerTask) ||
                                 (showAllJobsTasks.count(id) > 0) ||
                                 (showAllJobsTasks.count("direction:" + did) > 0);
            const size_t visibleCount = showAll ? descJobs.size() : maxVisibleJobsPerTask;
            for (size_t i = 0; i < visibleCount; ++i) {
                const auto& j = descJobs[i];
                TaskDisplayRow child;
                child.kind = TaskDisplayRowKind::Job;
                child.taskId = id;
                child.directionId = did;
                child.jobId = j.value("id", std::string());
                child.indent = dirIndent + 1;
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
                more.directionId = did;
                more.indent = dirIndent + 1;
                more.hasTaskRecord = m.hasRecord;
                more.taskTitle = m.title;
                more.taskState = m.state;
                more.hiddenJobCount = static_cast<int>(descJobs.size() - visibleCount);
                out.push_back(std::move(more));
            }
        }
        if (hiddenSingleDirJobs > 0) {
            TaskDisplayRow more;
            more.kind = TaskDisplayRowKind::MoreJobs;
            more.taskId = id;
            more.indent = baseIndent;
            more.hasTaskRecord = m.hasRecord;
            more.taskTitle = m.title;
            more.taskState = m.state;
            more.hiddenJobCount = hiddenSingleDirJobs;
            out.push_back(std::move(more));
        }
    }
    return out;
}

// 102-6: finished-row limiting and query filtering, independent of Win32 / the GUI.
inline bool IsFinishedTaskState(const std::string& state) {
    return state == "succeeded" || state == "failed" || state == "cancelled" ||
           state == "closed" || state == "takenOver";
}

inline std::string TaskDisplayRowState(const TaskDisplayRow& row) {
    if (row.kind == TaskDisplayRowKind::TaskHeading) return row.taskState;
    if (row.kind == TaskDisplayRowKind::DirectionHeading) return row.directionState;
    if (row.kind == TaskDisplayRowKind::Job) {
        if (row.jobJson.is_object()) return row.jobJson.value("state", row.taskState);
        return row.taskState;
    }
    return std::string();
}

inline std::string TaskDisplayRowQueryText(const TaskDisplayRow& row) {
    std::string out;
    auto add = [&](const std::string& value) {
        if (value.empty()) return;
        if (!out.empty()) out += ' ';
        out += value;
    };
    add(row.taskId);
    add(row.taskTitle);
    add(row.taskState);
    add(row.directionId);
    add(row.directionTitle);
    add(row.directionKind);
    add(row.cardNumber);
    add(row.jobId);
    add(row.resultsCompact);
    if (row.jobJson.is_object()) {
        add(row.jobJson.value("summary", std::string()));
        add(row.jobJson.value("state", std::string()));
        add(row.jobJson.value("taskqueue", std::string()));
    }
    return out;
}

inline bool TaskDisplayRowMatchesQuery(const TaskDisplayRow& row, const std::string& query) {
    if (query.empty()) return true;
    std::string hay = TaskDisplayRowQueryText(row);
    std::string needle = query;
    for (char& c : hay) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : needle) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return hay.find(needle) != std::string::npos;
}

struct TaskDisplayFilterOptions {
    size_t visibleFinishedCount = 30;
    std::string query;
};

struct TaskDisplayFilterResult {
    std::vector<TaskDisplayRow> rows;
    size_t totalFinished = 0;
    size_t shownFinished = 0;
    bool queryActive = false;
    bool hasMore = false;
};

inline TaskDisplayFilterResult FilterTaskDisplayRows(
    const std::vector<TaskDisplayRow>& input,
    const TaskDisplayFilterOptions& options) {
    TaskDisplayFilterResult result;
    result.queryActive = !options.query.empty();
    const size_t limit = options.visibleFinishedCount;

    std::vector<TaskDisplayRow> matched;
    matched.reserve(input.size());
    if (!result.queryActive) {
        for (const auto& row : input) matched.push_back(row);
    } else {
        std::unordered_set<std::string> keepTasks;
        for (const auto& row : input) {
            if (!TaskDisplayRowMatchesQuery(row, options.query)) continue;
            if (!row.taskId.empty()) keepTasks.insert(row.taskId);
            if (!row.directionId.empty()) keepTasks.insert("direction:" + row.directionId);
        }
        for (const auto& row : input) {
            const bool direct = TaskDisplayRowMatchesQuery(row, options.query);
            const bool sameTask = !row.taskId.empty() && keepTasks.count(row.taskId) > 0;
            const bool sameDir = !row.directionId.empty() && keepTasks.count("direction:" + row.directionId) > 0;
            if (direct || sameTask || sameDir) matched.push_back(row);
        }
    }

    for (const auto& row : matched) {
        const bool finished = IsFinishedTaskState(TaskDisplayRowState(row));
        if (finished) ++result.totalFinished;
        if (finished && result.shownFinished >= limit) {
            result.hasMore = true;
            continue;
        }
        if (finished) ++result.shownFinished;
        result.rows.push_back(row);
    }
    if (result.hasMore) {
        TaskDisplayRow more;
        more.kind = TaskDisplayRowKind::LoadMoreFinished;
        more.hiddenFinishedCount = static_cast<int>(result.totalFinished - result.shownFinished);
        result.rows.push_back(std::move(more));
    }
    return result;
}

inline TaskDisplayFilterResult FilterTaskDisplayRows(
    const std::vector<TaskDisplayRow>& input,
    size_t visibleFinishedCount,
    const std::string& query) {
    TaskDisplayFilterOptions options;
    options.visibleFinishedCount = visibleFinishedCount;
    options.query = query;
    return FilterTaskDisplayRows(input, options);
}


// Translated display text for the dashboard (card 106-3). The dashboard is a separate process, so
// translations come from the core (translate_get) and are kept here as (lang, field, original) ->
// translation. Drawing never asks for a translation: a text missing from the table is recorded as a
// miss, the caller fetches all misses once per list build, and a change of the core's revision
// drops the table so the next build asks again.
class DisplayTextCache {
public:
    struct Miss {
        std::string lang;
        std::string field;
        std::string text;
    };

    // The text to show. English, an empty text, or a text the config does not translate
    // (translatable false) gives the original. A cached translation gives the translation.
    // Otherwise the original is shown and the text is queued as a miss.
    std::string Display(const std::string& text, const std::string& lang, const std::string& field,
                        bool translatable) {
        if (!translatable || text.empty() || lang.empty() || lang == "en") return text;
        const std::string key = Key(lang, field, text);
        auto it = table_.find(key);
        if (it == table_.end()) {
            misses_.emplace(key, Miss{lang, field, text});
            return text;
        }
        return it->second.state == State::Translated ? it->second.translated : text;
    }

    // The cached translation of text, or "" when there is none. The search box matches these.
    std::string CachedTranslation(const std::string& text, const std::string& lang,
                                  const std::string& field) const {
        auto it = table_.find(Key(lang, field, text));
        if (it == table_.end() || it->second.state != State::Translated) return std::string();
        return it->second.translated;
    }

    // Records the core's answer for one text. waiting: the core has it queued or it failed, so it is
    // not asked again until the revision changes. An empty translation, or one equal to the text,
    // means that no translation applies.
    void Store(const std::string& text, const std::string& lang, const std::string& field,
               const std::string& translated, bool waiting) {
        const std::string key = Key(lang, field, text);
        Entry entry;
        if (waiting) {
            entry.state = State::Waiting;
        } else if (!translated.empty() && translated != text) {
            entry.state = State::Translated;
            entry.translated = translated;
        } else {
            entry.state = State::Original;
        }
        table_[key] = std::move(entry);
        misses_.erase(key);
    }

    // The misses queued since the last call. The caller fetches them.
    std::vector<Miss> TakeMisses() {
        std::vector<Miss> out;
        out.reserve(misses_.size());
        for (auto& item : misses_) out.push_back(std::move(item.second));
        misses_.clear();
        return out;
    }

    // True while some text waits for the core, so the revision is worth checking.
    bool HasWaiting() const {
        for (const auto& item : table_) {
            if (item.second.state == State::Waiting) return true;
        }
        return false;
    }

    // Sets the core's revision. Returns true when it changed from a known revision. The table is
    // dropped then, so every text is asked again.
    bool SetRevision(uint64_t revision) {
        if (known_ && revision == revision_) return false;
        const bool changed = known_;
        known_ = true;
        revision_ = revision;
        table_.clear();
        return changed;
    }

    void Clear() {
        table_.clear();
        misses_.clear();
    }

private:
    enum class State { Waiting, Original, Translated };
    struct Entry {
        State state = State::Original;
        std::string translated;
    };
    static std::string Key(const std::string& lang, const std::string& field, const std::string& text) {
        return lang + '\x1f' + field + '\x1f' + text;
    }
    std::map<std::string, Entry> table_;
    std::map<std::string, Miss> misses_;
    uint64_t revision_ = 0;
    bool known_ = false;
};

} // namespace pc
