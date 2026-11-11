#pragma once

// Core Behavior Tree (BT) engine for Hancho (doc/behavior-tree-plan.md).
// Pure logic only: no GUI, no IPC, no process launching, no dependency on TaskQueueManager.

#include "worker_engines.h"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
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
};

struct BtJobSuccess {
    int exitCode = 0;
    std::vector<std::string> filesNewer;
    std::string outputRegex;
    std::string testReport;
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

struct BtLimits {
    int maxNodeRuns = 200;
    int maxWallSec = 86400;
};

struct BtWorkerDefaults {
    std::string engine;
    std::string model;
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
    const std::string& TaskQueueName() const { return taskqueue_; }
    const std::string& Cwd() const { return cwd_; }
    const std::string& NotifyFile() const { return notifyFile_; }
    const std::string& State() const { return state_; }
    const std::string& Detail() const { return detail_; }
    const std::string& CreatedAt() const { return createdAt_; }
    const std::string& UpdatedAt() const { return updatedAt_; }
    int NodeRuns() const { return nodeRuns_; }
    const BtLimits& Limits() const { return limits_; }
    const std::map<std::string, std::string>& Vars() const { return vars_; }

    std::vector<std::string> RunningJobIds() const;
    bool IsJobRunning(const std::string& jobId) const;

    std::string GetVar(const std::string& name, bool* outFound = nullptr) const;
    std::string NodeState(const std::string& nodeKey) const;
    std::string NodeDetail(const std::string& nodeKey) const;
    int NodeRunCount(const std::string& nodeKey) const;
    std::vector<std::string> NodeJobs(const std::string& nodeKey) const;

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

    std::string id_;
    std::string summary_;
    std::string intent_;
    std::string taskqueue_;
    std::string cwd_;
    std::string notifyFile_;
    BtLimits limits_;
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
