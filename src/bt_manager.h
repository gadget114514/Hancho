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

class BtManager : public IBtJobRunner, public IBtEngineCatalog {
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

    // IBtJobRunner
    std::string StartJob(const BtJobSpec& spec) override;
    std::string StartWorker(const BtWorkerSpec& spec) override;
    void CancelJob(const std::string& jobId) override;

    // JSON API (stage 2 & stage 3)
    std::string Validate(const std::string& json);
    std::string Start(const std::string& json);
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
    std::string Pause(const std::string& id, bool paused);

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
    void DeliverJobOutcomeLocked(TreeEntry& entry,
                                 const TaskQueueJob& job,
                                 const std::string& finalState,
                                 int64_t nowMs,
                                 const AgentExtractedResult* preExtracted);
    void PersistEntryLocked(TreeEntry& entry);
    void SaveStateFileLocked(const TreeEntry& entry);
    std::string LookupQueueNameForBtLocked(const std::string& btId) const;
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
