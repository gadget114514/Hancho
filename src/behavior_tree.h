#pragma once

// Core Behavior Tree (BT) engine for Hancho (doc/obsoleted/behavior-tree-plan.md).
// Pure logic only: no GUI, no IPC, no process launching, no dependency on TaskQueueManager.

#include "worker_engines.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace pc {

// Variable values shown to an LLM (tool replies, escalation packets) are cut to this
// many bytes; a worker's ${<id>.result} alone can be 64 KB. bt_get returns them in full.
constexpr size_t kBtVarPreviewBytes = 200;

// Returns v unchanged when it fits in maxBytes; otherwise a UTF-8-safe prefix followed
// by " ...[truncated, N bytes]".
std::string BtVarPreview(const std::string& v, size_t maxBytes = kBtVarPreviewBytes);

struct BtJobIdle {
    int outputSec = 0;
    int cpuSec = 0;
    int firstEventSec = 0;
    bool configured = false;
};

struct BtJobSuccess {
    int exitCode = 0;
    std::vector<std::string> filesNewer;
    std::string outputRegex;
    std::string testReport;
    std::string contentFile;
    long long minBytes = 0;
    std::string contentRegex;
    bool contentFileOnly = false;
    bool engineResult = false;
};

struct BtJobResources {
    std::vector<std::string> write;
    std::vector<std::string> read;
    bool empty() const { return write.empty() && read.empty(); }
};

struct BtJobSpec {
    std::string btId;
    std::string nodeId;
    std::string nodeKey;
    int run = 1;
    std::string proposedJobId;
    std::string queueName;  // empty: the tree's queue
    std::string command;
    std::string summary;
    std::string cwd;
    int timeoutSec = 0;
    int estimateSec = 0;  // optional expected duration for ETA display (0 = use timeoutSec)
    std::map<std::string, std::string> env;
    BtJobSuccess success;
    BtJobIdle idle;
    BtJobResources resources;
    std::string instructionsText;
    int maxAttempts = 1;
};

struct BtWorkerSpec {
    std::string btId;
    std::string nodeId;
    std::string nodeKey;
    int run = 1;
    std::string proposedJobId;
    std::string queueName;  // empty: the tree's queue
    std::string engine;
    std::string model;
    std::string prompt;
    std::string summary;
    std::string cwd;
    int timeoutSec = 0;
    int estimateSec = 0;  // optional expected duration for ETA display (0 = use timeoutSec)
    std::map<std::string, std::string> env;
    BtJobSuccess success;
    BtJobIdle idle;
    BtJobResources resources;
    std::string outputSchema;
    int maxAttempts = 1;
};

class IBtJobRunner {
public:
    virtual ~IBtJobRunner() = default;
    // Returns the assigned job id (typically spec.proposedJobId), or "" when launch failed.
    virtual std::string StartJob(const BtJobSpec& spec) = 0;
    virtual std::string StartWorker(const BtWorkerSpec& spec) {
        (void)spec;
        return "";
    }
    virtual void CancelJob(const std::string& jobId) = 0;
};

// Result of a health probe (091-3). A failed probe carries a failure class that can be
// handled by `switchEngineOn` ("probe_failed", "startup_hang", "auth_error").
struct BtProbeResult {
    bool ok = false;
    int elapsedMs = 0;
    std::string failureClass;
    std::string detail;
    bool cached = false;
};

// Injected health prober. The BT core stays process-agnostic; BtManager wires the real
// engine-registry based prober and tests use a fake.
class IBtProber {
public:
    virtual ~IBtProber() = default;
    virtual BtProbeResult Probe(const std::string& engine, const std::string& model,
                                int timeoutSec) = 0;
};

// Shared engine health (091-6). The BT core stays storage-agnostic: BtManager wires the
// file-backed registry and tests use a fake.
class IBtEngineHealth {
public:
    virtual ~IBtEngineHealth() = default;
    virtual bool IsDown(const std::string& /*engine*/, const std::string& /*model*/) const { return false; }
    virtual void MarkDown(const std::string& /*engine*/, const std::string& /*model*/,
                          const std::string& /*failureClass*/, const std::string& /*detail*/,
                          int64_t /*nowMs*/) {}
    virtual void MarkHealthy(const std::string& /*engine*/, const std::string& /*model*/,
                             const std::string& /*reason*/, int64_t /*nowMs*/) {}
    virtual int64_t NextProbeAtMs(const std::string& /*engine*/, const std::string& /*model*/) const { return 0; }
    virtual bool IsDownWorthy(const std::string& failureClass) const {
        return failureClass == "startup_hang" || failureClass == "probe_failed" ||
               failureClass == "startup_timed_out" || failureClass == "idle_stall" ||
               failureClass == "rate_limited" || failureClass == "quota";
    }
    // Approved candidates for a role, sorted by rank (consult / supervisor fallbacks).
    virtual std::vector<std::pair<std::string, std::string>> Candidates(
        const std::string& /*list*/) const { return {}; }
};

// A job/worker leaf that is still idle and will start later if the tree keeps its current
// branch. The task queue treats these as provisional rows; they are re-derived from the tree
// state on every query, so a branch change (retry, fallback, if, cancel) drops stale rows
// without any explicit unregister step.
struct BtPlannedTask {
    std::string btId;
    std::string nodeKey;
    std::string type;       // "exec" or "agent"
    std::string summary;
    std::string queueName;
    std::string proposedJobId;  // job id the leaf will get when it starts
    int estimateSec = 0;    // estimateSec when set, otherwise timeoutSec
};

struct BtLimits {
    int maxNodeRuns = 200;
    int maxWallSec = 86400;
};

struct BtSwitchRule {
    std::string failureClass;
    int after = 1;
};

struct BtSwitchLimits {
    int maxCandidates = 3;
    int maxSec = 0;
    bool stopOnSameSymptom = true;
    bool configured = false;
};

struct BtWorkerDefaults {
    std::string engine;
    std::string model;
};

// Tree-level defaults (doc/obsoleted/HanchoExpectations.md 8.2). A node's explicit values always win,
// including an explicit 0. `configured` on the idle fields means the default block was present.
struct BtDefaults {
    BtWorkerDefaults worker;       // engine/model from the top-level "worker" block
    BtJobIdle workerIdle;
    int workerMaxAttempts = 0;     // 0 = not specified
    bool workerProbe = false;
    bool hasWorkerProbe = false;
    std::vector<BtSwitchRule> workerSwitchEngineOn;
    BtSwitchLimits workerSwitchLimits;
    BtJobIdle jobIdle;

    bool hasWorkerIdle() const { return workerIdle.configured; }
    bool hasJobIdle() const { return jobIdle.configured; }
};

// A non-fatal lint finding from `bt validate` (doc/obsoleted/HanchoExpectations.md 8.4). Warnings never
// fail validation; --strict turns them into errors.
struct BtWarning {
    std::string path;
    std::string code;
    std::string alias;
    std::string message;
    std::string suggestion;
};

struct BtEvent {
    uint64_t seq = 0;
    int64_t timestampMs = 0;
    std::string type;      // "nodeStarted", "nodeFinished", "varSet", "escalated", "btFinished"
    std::string btId;
    std::string nodeKey;
    std::string state;
    std::string detail;
    std::string varName;
    std::string varValue;
    int run = 0;
};

struct BtEscalationPacket {
    std::string btId;
    std::string nodeKey;
    int run = 0;
    std::string intent;
    std::string question;
    std::string guidance;
    std::vector<std::string> expect;
    std::string json;
    std::string markdown;
};

class BehaviorTree {
public:
    BehaviorTree();
    ~BehaviorTree();

    BehaviorTree(BehaviorTree&&) noexcept;
    BehaviorTree& operator=(BehaviorTree&&) noexcept;
    BehaviorTree(const BehaviorTree&) = delete;
    BehaviorTree& operator=(const BehaviorTree&) = delete;

    // Validates a BT JSON definition without running it. All errors include the offending JSON path.
    static bool ValidateJson(const std::string& jsonText,
                             const IBtEngineCatalog* catalog,
                             std::vector<std::string>& outErrors);
    static bool ValidateJson(const std::string& jsonText,
                             const IBtEngineCatalog* catalog,
                             std::string& outError);
    // Same validation plus the hang-protection lint warnings. Errors and warnings are
    // independent: a tree can be valid (no errors) but still carry warnings.
    static bool ValidateJson(const std::string& jsonText,
                             const IBtEngineCatalog* catalog,
                             std::vector<std::string>& outErrors,
                             std::vector<BtWarning>& outWarnings);

    // JSON response shared by the CLI/IPC/MCP `bt validate` entry points. With strict=true,
    // warnings become errors and the response reports ok=false / valid=false.
    static std::string ValidateResponse(const std::string& jsonText,
                                        const IBtEngineCatalog* catalog,
                                        bool strict);

    // Parses and validates a fresh BT definition.
    bool LoadDefinition(const std::string& jsonText,
                        const IBtEngineCatalog* catalog,
                        std::string& err);

    // Restores a tree from its persisted state JSON (section 7) so ticking can continue after restart.
    bool LoadFromStateJson(const std::string& jsonText,
                           const IBtEngineCatalog* catalog,
                           int64_t nowMs,
                           std::string& err);
    bool LoadFromStateJson(const std::string& jsonText,
                           const IBtEngineCatalog* catalog,
                           std::string& err);

    void SetJobRunner(IBtJobRunner* runner) { runner_ = runner; }
    void SetEngineCatalog(const IBtEngineCatalog* catalog) { catalog_ = catalog; }
    void SetProber(IBtProber* prober) { prober_ = prober; }
    void SetEngineHealth(IBtEngineHealth* health) { health_ = health; }

    // Execution entry points (event-driven tick, section 5.2).
    void Start(int64_t nowMs, bool paused = false);

    void OnJobFinished(const std::string& jobId,
                       bool succeeded,
                       int exitCode,
                       const std::string& logFile,
                       const std::string& logTail,
                       int64_t nowMs);

    void OnJobFinished(const std::string& jobId,
                       bool succeeded,
                       int exitCode,
                       const std::string& logFile,
                       const std::string& logTail,
                       int64_t nowMs,
                       const std::string& finalResponse,
                       const std::string& resultFileJson,
                       const std::string& failDetail = "");

    void OnJobFinished(const std::string& jobId,
                       bool succeeded,
                       int exitCode,
                       const std::string& logFile,
                       const std::string& logTail,
                       const std::string& finalResponse,
                       const std::string& resultFileJson,
                       int64_t nowMs,
                       const std::string& failDetail = "");

    // As above, with the taskqueue failure diagnostic JSON for the finished attempt. The
    // diagnostic fields become ${<node>.failureClass} etc. (doc/obsoleted/HanchoExpectations.md 8.1).
    void OnJobFinished(const std::string& jobId,
                       bool succeeded,
                       int exitCode,
                       const std::string& logFile,
                       const std::string& logTail,
                       int64_t nowMs,
                       const std::string& finalResponse,
                       const std::string& resultFileJson,
                       const std::string& failDetail,
                       const std::string& failureDiagnosticJson);

    void OnTimer(int64_t nowMs);
    void Tick(int64_t nowMs);

    // Earliest active timer deadline in ms (wait, timeout, escalate fallback/timeout), or 0 when none.
    int64_t NextWakeMs() const;

    bool Resume(const std::string& nodeKey,
                const std::string& answer,
                const std::string& rationale,
                const std::map<std::string, std::string>& vars,
                int64_t nowMs,
                std::string& err);

    void Cancel(int64_t nowMs);

    bool Retry(const std::string& fromNodeKey,
               bool resetCounters,
               int64_t nowMs,
               std::string& err);

    bool UpdateNode(const std::string& nodeKey,
                    const std::string& nodeJsonText,
                    int64_t nowMs,
                    std::string& err);

    bool UpdateDefinition(const std::string& jsonText,
                          const std::string& targetNodeKey,
                          int64_t nowMs,
                          std::string& err);

    bool UpdateDefinition(const std::string& jsonText,
                          int64_t nowMs,
                          std::string& err);

    bool SetPaused(bool paused, int64_t nowMs, std::string& err);

    // State and getters
    const std::string& Id() const { return id_; }
    const std::string& Summary() const { return summary_; }
    const std::string& Intent() const { return intent_; }
    // Task (lineage) this BT belongs to. Empty when the BT was started without --task.
    const std::string& TaskId() const { return taskId_; }
    const std::string& DirectionId() const { return directionId_; }
    void SetDirectionId(const std::string& id) { directionId_ = id; }
    // Marks a freshly registered tree as planned: it is persisted and visible, but no
    // jobs are created until Start/Tick runs it.
    void MarkPlanned(int64_t nowMs);
    const std::string& TaskQueueName() const { return taskqueue_; }
    const std::string& Cwd() const { return cwd_; }
    const std::string& NotifyFile() const { return notifyFile_; }
    const std::string& State() const { return state_; }
    const std::string& Detail() const { return detail_; }
    const std::string& CreatedAt() const { return createdAt_; }
    const std::string& UpdatedAt() const { return updatedAt_; }
    int NodeRuns() const { return nodeRuns_; }
    const BtLimits& Limits() const { return limits_; }
    const BtDefaults& Defaults() const { return defaults_; }
    const std::map<std::string, std::string>& Vars() const { return vars_; }

    std::vector<std::string> RunningJobIds() const;
    bool IsJobRunning(const std::string& jobId) const;

    std::string GetVar(const std::string& name, bool* outFound = nullptr) const;
    std::string NodeState(const std::string& nodeKey) const;
    std::string NodeDetail(const std::string& nodeKey) const;
    int NodeRunCount(const std::string& nodeKey) const;
    std::vector<std::string> NodeJobs(const std::string& nodeKey) const;

    // Estimated seconds of work left in job/worker leaves that have not started yet. Jobs that
    // are already running are counted by the task queue, so they contribute 0 here. The value
    // follows the current branch of the tree (retry/repeat multiply future attempts, parallel
    // takes the longest child, sequence sums, fallback/if take the active path).
    int64_t PendingEstimateSec() const;
    // Job/worker leaves still idle on the current execution path, in the order they will start.
    std::vector<BtPlannedTask> PlannedTasks() const;

    bool GetEscalationPacket(const std::string& nodeKey, BtEscalationPacket& out) const;
    BtEscalationPacket GetCurrentEscalationPacket() const;
    std::string GetEscalationPacketJson(const std::string& nodeKey = "") const;
    std::string GetEscalationPacketMarkdown(const std::string& nodeKey = "") const;

    // Persisted state format (section 7), compatible with ParseBtDebugTree.
    std::string ToStateJson() const;

    const std::vector<BtEvent>& Events() const { return events_; }
    std::vector<BtEvent> GetEventsSince(uint64_t sinceSeq) const;
    uint64_t Version() const { return version_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    IBtJobRunner* runner_ = nullptr;
    const IBtEngineCatalog* catalog_ = nullptr;
    IBtProber* prober_ = nullptr;
    IBtEngineHealth* health_ = nullptr;

    std::string id_;
    std::string summary_;
    std::string intent_;
    std::string taskId_;
    std::string directionId_;
    std::string taskqueue_;
    std::string cwd_;
    std::string notifyFile_;
    BtLimits limits_;
    BtDefaults defaults_;
    BtWorkerDefaults workerDefaults_;
    std::string state_ = "idle";
    std::string detail_;
    int nodeRuns_ = 0;
    int nextJobSeq_ = 1;
    int64_t startMs_ = 0;
    std::string createdAt_;
    std::string updatedAt_;
    std::map<std::string, std::string> vars_;
    std::vector<BtEvent> events_;
    uint64_t nextEventSeq_ = 1;
    uint64_t version_ = 0;

    void EmitEvent(int64_t nowMs,
                   const std::string& type,
                   const std::string& nodeKey = "",
                   const std::string& state = "",
                   const std::string& detail = "",
                   int run = 0,
                   const std::string& varName = "",
                   const std::string& varValue = "");
    void BumpVersion(int64_t nowMs);
};

} // namespace pc
