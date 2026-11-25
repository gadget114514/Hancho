#pragma once

#include "behavior_tree.h"
#include "process_manager.h"
#include "taskqueue.h"
#include "worker_engines.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pc {

struct TaskGroupInfo {
    std::string id;
    std::string directionId;
    std::string taskId;
    std::string state;
    std::string summary;
    std::string detail;
    std::string taskqueue;
    std::string startTime;
};

struct BtRemainingInfo {
    int64_t pendingSec = 0;
    std::vector<BtPlannedTask> planned;  // provisional rows, derived from the current tree state
    std::vector<TaskGroupInfo> groups;   // active task groups, shown as virtual rows in the task list
};

class BtManager : public IBtJobRunner, public IBtEngineCatalog, public IBtProber,
                  public IBtEngineHealth {
public:
    BtManager(ProcessManager& pm, TaskQueueManager& tqm);
    ~BtManager() override;

    BtManager(const BtManager&) = delete;
    BtManager& operator=(const BtManager&) = delete;

    void Shutdown();

    bool ReloadEngines(std::string& outError);
    const WorkerEngineRegistry& Engines() const { return engines_; }

    // IBtEngineCatalog
    std::string DefaultEngine() const override;
    bool Knows(const std::string& engine, const std::string& model, std::string& err) const override;
    bool IsApprovedForList(const std::string& list, const std::string& engine,
                           const std::string& model) const override;

    // IBtProber
    BtProbeResult Probe(const std::string& engine, const std::string& model,
                         int timeoutSec) override;

    // IBtEngineHealth (091-6)
    bool IsDown(const std::string& engine, const std::string& model) const override;
    void MarkDown(const std::string& engine, const std::string& model,
                  const std::string& failureClass, const std::string& detail,
                  int64_t nowMs) override;
    void MarkHealthy(const std::string& engine, const std::string& model,
                     const std::string& reason, int64_t nowMs) override;
    int64_t NextProbeAtMs(const std::string& engine, const std::string& model) const override;
    bool IsDownWorthy(const std::string& failureClass) const override;
    std::vector<std::pair<std::string, std::string>> Candidates(
        const std::string& list) const override;

    // IBtJobRunner
    std::string StartJob(const BtJobSpec& spec) override;
    std::string StartWorker(const BtWorkerSpec& spec) override;
    void CancelJob(const std::string& jobId) override;

    // Estimated work left in active BTs plus every idle job/worker leaf. Recomputed from the
    // tree state on each call, so rows for branches that will not run are simply absent.
    BtRemainingInfo GetRemainingInfo() const;

    // JSON API (stage 2 & stage 3)
    // strict=true promotes 091-4 lint warnings to errors.
    std::string Validate(const std::string& json, bool strict = false);
    std::string Start(const std::string& json, bool paused = false, bool triage = true);
    std::string Get(const std::string& id, bool compact = false);
    std::string List();
    std::string Wait(const std::string& id, uint64_t sinceVersion, int timeoutMs, bool compact = false);
    std::string Resume(const std::string& id,
                       const std::string& node,
                       const std::string& answer,
                       const std::string& rationale,
                       const std::string& varsJson = "");
    std::string Retry(const std::string& id,
                      const std::string& fromNode = "",
                      bool resetCounters = false);
    std::string Update(const std::string& id,
                       const std::string& node,
                       const std::string& jsonText);
    std::string Update(const std::string& id,
                       const std::string& jsonText);
    std::string Cancel(const std::string& id);
    // Stops the tree, then drops it from the manager and deletes its state/events files so
    // its virtual rows (task group and planned leaves) disappear for good.
    std::string Remove(const std::string& id);
    std::string Pause(const std::string& id, bool paused);
    std::string CardRun(const std::string& taskId,
                        const std::string& number,
                        bool ready,
                        int maxConcurrent,
                        bool force,
                        const std::string& originJson = "");

    static int64_t NowEpochMs();
    static std::wstring GetBtDirectoryWide();
    static std::wstring GetBtStateFileWide(const std::string& btId);
    static std::wstring GetBtEventsFileWide(const std::string& btId);
    static std::wstring GetBtResultFileWide(const std::string& btId, const std::string& jobId);
    static std::wstring GetBtEscalationFileWide(const std::string& btId,
                                                const std::string& nodeKey,
                                                int run,
                                                const std::wstring& ext);

private:
    struct TreeEntry {
        std::unique_ptr<BehaviorTree> tree;
        uint64_t lastFlushedVersion = 0;
        size_t lastFlushedEventCount = 0;
    };

    void LoadPersistedTrees();
    void TimerLoop();
    void OnTaskQueueJobFinished(const TaskQueueJob& job, const std::string& finalState);
    bool OnTaskQueueBtRetry(const TaskQueueBtRef& ref, bool resetCounters, std::string& outError);
    // Cancels queue jobs that still belong to a BT after the tree itself was cancelled.
    // Pending/waiting leaves would otherwise stay registered and never run.
    void CancelQueuedJobsForTree(const std::string& btId);
    void DeliverJobOutcomeLocked(TreeEntry& entry,
                                 const TaskQueueJob& job,
                                 const std::string& finalState,
                                 int64_t nowMs,
                                 const AgentExtractedResult* preExtracted);
    void PersistEntryLocked(TreeEntry& entry);
    void SaveStateFileLocked(const TreeEntry& entry);
    std::string LookupQueueNameForBtLocked(const std::string& btId) const;
    std::string LookupTaskIdForBtLocked(const std::string& btId) const;
    std::string LookupDirectionIdForBtLocked(const std::string& btId) const;
    // proposedId, or proposedId-r2, -r3, ... when a job with that id is still in the queue
    // (a BT started again under an id used before).
    std::string UniqueJobId(const std::string& proposedId) const;

    ProcessManager& pm_;
    TaskQueueManager& tqm_;
    WorkerEngineRegistry engines_;

    mutable std::mutex mutex_;
    std::condition_variable waitCv_;
    std::condition_variable timerCv_;
    std::map<std::string, std::unique_ptr<TreeEntry>> trees_;
    std::string activeLaunchingQueueName_;

    std::atomic<bool> stopped_{false};
    std::thread timerThread_;
};

} // namespace pc
