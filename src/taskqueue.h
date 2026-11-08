#pragma once

#include "process_manager.h"
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>

namespace pc {

// ---------------------------------------------------------------------------
// TaskQueue state machine
// ---------------------------------------------------------------------------

enum class TaskQueueState {
    Pending = 0,
    Running = 1,
    Succeeded = 2,
    Failed = 3,
    Blocked = 4,
    Cancelled = 5,
};

std::string TaskQueueStateToString(TaskQueueState state);
TaskQueueState TaskQueueStateFromString(const std::string& s);

struct TaskQueueCounts {
    int running = 0;
    int waiting = 0;   // Ready to run (state == "pending", dependencies satisfied)
    int pending = 0;   // Waiting for dependencies (state == "pending", dependencies not satisfied)
    int blocked = 0;
    int succeeded = 0;
    int failed = 0;
    int cancelled = 0;
    int total = 0;
    int64_t minRemainingSec = -1;
    int64_t sumRemainingSec = -1;
};

// ---------------------------------------------------------------------------
// TaskQueue data model
// ---------------------------------------------------------------------------

struct TaskQueueInstructions {
    std::string source;
    int rev = 1;
    std::string sha256;
    std::string text;
    std::string capturedAt;
};

struct TaskQueueIdle {
    int outputSec = 0;
    int cpuSec = 0;
};

struct TaskQueueSuccess {
    int exitCode = 0;
    std::vector<std::string> filesNewer;
    std::string outputRegex;
};

struct TaskQueueDependency {
    std::string id;
    std::string on;
};

// Declared resources (doc/resource-locks.md). "@name" is a named lock; anything else is a
// path or glob, resolved against the job's cwd when relative. Jobs whose resources conflict
// (write/write or write/read) never run at the same time.
struct TaskQueueResources {
    std::vector<std::string> write;
    std::vector<std::string> read;
    bool empty() const { return write.empty() && read.empty(); }
};

// Resource helpers, exposed for tests.
std::string NormalizeResource(const std::string& resource, const std::string& cwd);
bool ResourcesOverlap(const std::string& a, const std::string& b); // both normalized
bool FindResourceConflict(const TaskQueueResources& a, const std::string& cwdA,
                          const TaskQueueResources& b, const std::string& cwdB,
                          std::string& outResource);

struct TaskQueueAttempt {
    int n = 1;
    int processId = 0;
    std::string start;
    std::string end;
    std::string reason;
    int exitCode = -1;
    int instructionsRev = 1;
    std::string lastLinesFile;
    std::string checkDetail;
    // Transient (not persisted): launch wall clock used by filesNewer checks.
    uint64_t startFiletime = 0;
};

// An attempt that ends with one of exitCodes means "nothing ran, try again later" (75 =
// EX_TEMPFAIL, e.g. ww-run refusing an overlapping card): the job goes back to pending and
// starts again after delaySec, without using up maxAttempts. After limit deferrals (0 = no
// limit) the exit code counts as an ordinary failure.
struct TaskQueueDefer {
    std::vector<int> exitCodes{75};
    int delaySec = 60;
    int limit = 60;
};

struct TaskQueueBtRef {
    std::string id;
    std::string node;
    int run = 0;
};

// Who added a job, so jobs from several agent sessions can be told apart.
// Filled by hancho-cli (MCP server or CLI) from its environment and the MCP clientInfo.
struct TaskQueueOrigin {
    std::string label;        // free text from the caller (taskqueue_add originLabel)
    std::string client;       // MCP clientInfo "name version", "hancho-cli-cli" or "gui"
    std::string session;      // agent session id (e.g. CLAUDE_CODE_SESSION_ID)
    std::string hostSession;  // host app session id (e.g. CLAUDE_CODE_HOST_SESSION_ID)
    std::string cwd;          // working folder of the caller
    int64_t pid = 0;          // agent process that started hancho-cli
    bool empty() const {
        return label.empty() && client.empty() && session.empty() && hostSession.empty() && cwd.empty() && pid == 0;
    }
};

enum TaskQueueCaller {
    Gui = 0,
    External = 1,
};

struct TaskQueueJob {
    std::string id;
    std::string taskqueue;
    int num = 0; // transient numeric id (process correlation)
    std::string name;
    std::string summary;
    TaskQueueInstructions instructions;
    std::string command;
    std::string cwd;
    std::map<std::string, std::string> env;
    std::vector<TaskQueueDependency> dependsOn;
    int timeoutSec = 0;
    TaskQueueIdle idle;
    int maxAttempts = 2;
    int retryDelaySec = 0;
    TaskQueueDefer defer;
    TaskQueueSuccess success;
    std::vector<std::string> killExclude;
    TaskQueueResources resources;
    int priority = 0;
    std::string state;
    std::vector<TaskQueueAttempt> attempts;
    std::string blockedBy;
    std::string createdAt;
    std::string updatedAt;
    std::string notes;
    int64_t retryAtMs = 0;
    TaskQueueBtRef bt;
    TaskQueueOrigin origin;
};

struct TaskQueue {
    std::string name;
    int concurrency = 1;
    bool paused = false;
    int version = 0;
    std::string notifyFile;
    int maxTimeoutSec = 7200;
    int defaultMaxAttempts = 2;
    std::vector<TaskQueueJob> jobs;
};

// ---------------------------------------------------------------------------
// TaskQueueManager: task queue store + scheduler + notifier
//
// The TaskQueue layer never touches OS processes directly; it always goes
// through ProcessManager (which reports completion via the finished callback).
// ---------------------------------------------------------------------------

class TaskQueueManager {
public:
    TaskQueueManager();
    ~TaskQueueManager();

    using ProcessFinishedCallback = std::function<void(int processId, int exitCode, const std::string& endKind)>;
    using JobFinishedCallback = std::function<void(const TaskQueueJob& snapshot, const std::string& finalState)>;
    using BtRetryCallback = std::function<bool(const TaskQueueBtRef& ref, bool resetCounters, std::string& outError)>;

    void SetProcessFinishedCallback(ProcessFinishedCallback cb);
    void SetJobFinishedCallback(JobFinishedCallback cb);
    void SetBtRetryCallback(BtRetryCallback cb);
    void SetProcessManager(ProcessManager* pm);

    // Task queue operations. Each returns a complete JSON response string
    // ({"ok":true,...} or {"ok":false,"error":"..."}).
    // originJson: optional {"label","client","session","hostSession","cwd","pid"} recorded on every added job.
    std::string TaskQueueAdd(const std::string& taskqueue, const std::string& jobsJson, int64_t concurrency, const std::string& notifyFile, std::string& outError,
                             const std::string& originJson = "");
    // Internal entry point used only by the BT layer to add a BT-managed job.
    // The job inherits the origin recorded for its tree with SetBtOrigin.
    std::string AddBtJob(const std::string& taskqueue, const std::string& jobJson, const TaskQueueBtRef& ref, std::string& outError);
    // Remember who started a behavior tree, so the jobs it adds carry the same origin.
    void SetBtOrigin(const std::string& btId, const std::string& originJson);
    // sinceSec > 0 keeps only jobs whose updatedAt is within the last sinceSec seconds.
    // origin (optional) keeps only jobs whose origin label, session or host session matches
    // (session ids also match by prefix); "pid:<n>" matches the origin process id.
    std::string TaskQueueList(const std::string& taskqueue, const std::string& state, int64_t sinceSec, std::string& outError,
                              const std::string& origin = "");
    static TaskQueueOrigin ParseOriginJson(const std::string& json);
    static std::string OriginToJson(const TaskQueueOrigin& origin);
    static bool OriginMatches(const TaskQueueOrigin& origin, const std::string& filter);
    std::string TaskQueueGet(const std::string& id, bool includeInstructions, int rev, std::string& outError);
    bool GetJobSnapshot(const std::string& id, TaskQueueJob& outJob) const;
    std::string TaskQueueRetry(const std::string& id, bool resetAttempts, bool refreshInstructions, std::string& outError);
    std::string TaskQueueRetry(const std::string& id, bool resetAttempts, bool refreshInstructions, TaskQueueCaller caller, std::string& outError);
    // Retries every failed job (optionally limited to one queue and to jobs whose updatedAt is
    // within the last sinceSec seconds; sinceSec <= 0 means no time limit). Returns the retried ids.
    std::string TaskQueueRetryFailed(const std::string& taskqueue, int64_t sinceSec, bool resetAttempts, bool refreshInstructions, std::string& outError);
    std::string TaskQueueRetryFailed(const std::string& taskqueue, int64_t sinceSec, bool resetAttempts, bool refreshInstructions, TaskQueueCaller caller, std::string& outError);
    std::string TaskQueueUpdate(const std::string& id, const std::string& patchJson, std::string& outError);
    std::string TaskQueueUpdate(const std::string& id, const std::string& patchJson, TaskQueueCaller caller, std::string& outError);
    std::string TaskQueueCancel(const std::string& id, std::string& outError);
    std::string TaskQueuePause(const std::string& taskqueue, bool paused, std::string& outError);
    std::string TaskQueueWait(const std::string& taskqueue, int sinceVersion, int timeoutMs, std::string& outError);
    std::string TaskQueuePurge(const std::string& taskqueue, const std::string& statesJson, std::string& outError);
    // Removes finished jobs (succeeded/failed/cancelled) whose updatedAt is older than maxAgeSec.
    // Empty taskqueue applies to all queues.
    std::string TaskQueuePurgeOlderThan(const std::string& taskqueue, int64_t maxAgeSec, std::string& outError);
    std::string TaskQueueLink(const std::string& job, const std::string& dependsOn, const std::string& on, std::string& outError);
    std::string TaskQueueUnlink(const std::string& job, const std::string& dependsOn, std::string& outError);
    std::string TaskQueueGraph(const std::string& taskqueue, const std::string& root, std::string& outError);

    // Scheduler hooks
    void OnProcessFinished(int processId, int exitCode, const std::string& endKind);
    void RequestSchedulerTick();
    void ShutdownTaskQueueManager();

    int64_t GetTaskQueueVersion(const std::string& taskqueue) const;

    // Resource locks (doc/resource-locks.md). On by default; when off, declared resources are
    // kept but ignored by the scheduler.
    void SetResourceLocksEnabled(bool enabled);
    bool GetResourceLocksEnabled() const { return resourceLocksEnabled_.load(); }

    TaskQueueCounts GetJobCounts() const;

    // Returns true and fills outJobId when an unfinished job (pending/running/
    // blocked/failed) uses exactly the given command.
    bool FindUnfinishedJobByCommand(const std::string& command, std::string& outJobId) const;

    static std::wstring GetTaskQueuesDirWide();
    static std::wstring GetTaskQueueFileWide(const std::string& name);
    static std::wstring GetInstructionsDirWide(const std::string& name);
    static std::wstring GetInstructionsFileWide(const std::string& name, const std::string& jobId, int rev);
    static std::wstring GetTaskEventsFileWide(const std::string& name);
    static std::wstring GetAttemptLogFileWide(const std::string& jobId, int n);

    // Event notification (events.jsonl + notifyFile). Public for the GUI layer.
    void EmitTaskQueueEvent(const std::string& taskqueue, const std::string& type, const std::string& jobId, int64_t attempt);

private:
    struct LaunchItem {
        std::string taskqueue;
        std::string jobId;
        int attemptN = 0;
        StartProcessRequest req;
    };

    void LoadQueues();
    void SaveQueue(const TaskQueue& tq);
    void SaveAllLocked();
    void SchedulerLoop();
    void ProcessSchedulerTick();
    void FlushFinishedCallbacks();

    TaskQueueJob* FindJob(const std::string& id, TaskQueue** outQueue);
    const TaskQueueJob* FindJobConst(const std::string& id) const;
    // Requeues one job (caller holds mutex_, has validated the state, and saves the queue).
    void RequeueJobLocked(TaskQueue& tq, TaskQueueJob& job, bool resetAttempts, bool refreshInstructions);

    void ApplyAttemptOutcome(TaskQueue& tq, TaskQueueJob& job, bool success,
                             const std::string& reason, int exitCode, const std::string& checkDetail);
    void ReevaluateAll();
    bool ReevaluateJob(TaskQueue& tq, TaskQueueJob& job);
    void ReevaluateDependents(const std::string& jobId);
    void CheckQueueNotices(TaskQueue& tq);

    static bool IsDepSatisfied(const TaskQueueJob& dep, const std::string& on);
    // A running job (any queue) whose resources conflict with job's; fills holder and resource.
    bool FindRunningResourceConflictLocked(const TaskQueueJob& job, std::string& outHolder,
                                           std::string& outResource) const;
    // A ready job that starts before job (RunsBefore) and conflicts with it: job waits behind it.
    bool FindQueuedResourceConflictLocked(const TaskQueueJob& job, int64_t nowMs, std::string& outHolder,
                                          std::string& outResource) const;
    bool IsReadyLocked(const TaskQueueJob& job, int64_t nowMs) const;
    static bool RunsBefore(const TaskQueueJob& a, const TaskQueueJob& b);
    bool HasDependencyCycle(const TaskQueue& tq, const std::string& jobId,
                            const std::vector<TaskQueueDependency>& deps,
                            const std::string& ignoreDependency);
    bool BuildLaunchRequest(TaskQueue& tq, TaskQueueJob& job, int attemptN, LaunchItem& out);
    bool EvaluateSuccess(const TaskQueueJob& job, TaskQueueAttempt& attempt,
                         const std::string& endKind, int exitCode,
                         const std::string& logFile, std::string& detail);

    void EmitEventLocked(TaskQueue& tq, const std::string& type, const std::string& jobId, int64_t attempt);
    std::string JobToJson(const TaskQueueJob& job, bool includeInstructions) const;
    static std::string BuildError(const std::string& msg);

    ProcessManager* pm_ = nullptr;
    ProcessFinishedCallback finishedCb_;
    JobFinishedCallback jobFinishedCb_;
    // Job-finished callbacks running right now (guarded by mutex_). SetJobFinishedCallback(nullptr)
    // waits for this to reach 0 so the callback's owner can be destroyed safely afterwards.
    int jobFinishedCbInFlight_ = 0;
    std::condition_variable jobFinishedCbIdleCv_;
    BtRetryCallback btRetryCb_;
    std::vector<std::pair<TaskQueueJob, std::string>> pendingFinished_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::string, TaskQueue> queues_;
    std::map<std::string, TaskQueueOrigin> btOrigins_; // behavior tree id -> who started it
    std::map<std::string, std::string> lastNotice_; // taskqueue -> last emitted notice type
    int nextJobNum_ = 1;
    std::atomic<bool> schedulerRunning_{false};
    std::atomic<bool> resourceLocksEnabled_{true};
    std::thread schedulerThread_;
};

} // namespace pc
