#pragma once

#include "nlohmann/json.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace pc {

class BtManager;
class ProcessManager;
class TaskQueueManager;

class TriageManager {
public:
    TriageManager(ProcessManager& pm, TaskQueueManager& tqm, BtManager* btm);
    ~TriageManager();

    TriageManager(const TriageManager&) = delete;
    TriageManager& operator=(const TriageManager&) = delete;

    void Start();
    void Stop();
    std::string List(const std::string& state = "pending");
    std::string Get(const std::string& id);
    // Insert (or reopen) one externally produced card without touching the other cards.
    // Returns the card id, or an empty string when the card has no key.
    std::string AddCard(const nlohmann::json& card);
    std::string RequestAdvice(const std::string& id);
    std::string Decide(const std::string& id, const std::string& candidateId,
                       const std::string& reason, const nlohmann::json& params,
                       const std::string& by = "human");
    std::string Acknowledge(const std::string& id, bool allAuto = false);
    // 093-5: lets the IPC layer implement the `update_routine` candidate as a routine ask
    // (the triage manager itself owns no RoutineManager). Returns the routine_ask JSON.
    using RoutineAskHandler = std::function<std::string(const std::string& name,
                                                        const std::string& cwd,
                                                        const std::string& request)>;
    void SetRoutineAskHandler(RoutineAskHandler handler) { routineAskHandler_ = std::move(handler); }
    // Hands pending cards (the given ids, or every pending card when allPending) to a non-interactive
    // supervisor worker, run as a hidden job in queue `_supervisor` with a timeout. The cards stay pending;
    // the worker records its decisions with `hancho-cli triage decide`.
    std::string Handoff(const std::vector<std::string>& ids, bool allPending);
    // A person takes the card over by hand (e.g. in the Claude desktop app): marks it "manual, working",
    // notes it on the job, and returns a prompt for that conversation.
    std::string MarkManual(const std::string& id);

private:
    void Refresh();
    void RefreshIfStale();
    void Loop();
    void SaveLocked();
    void PumpAdvice();
    // Runs the advice engine as an ordinary worker process (not a queue job) so an answer
    // starts immediately instead of waiting behind the task queue.
    void StartAdviceProcess(const nlohmann::json& card, const std::string& adviceId,
                            const std::string& engineName, const std::string& modelName);
    void UpdateHandoffsLocked(const nlohmann::json& supervisorJobs);
    // Follows each handled card's job to the end of its lifecycle (rerunning, succeeded, failed again,
    // cancelled, not rerun) and links a card to the card that follows it.
    void UpdateOutcomesLocked(const nlohmann::json& jobs, const nlohmann::json& btStates);
    std::string DecideInternal(const std::string& id, const std::string& candidateId,
                               const std::string& reason, const nlohmann::json& params,
                               const std::string& by);

    ProcessManager& pm_;
    TaskQueueManager& tqm_;
    BtManager* btm_ = nullptr;
    std::mutex mutex_;
    std::mutex refreshMutex_;
    std::mutex adviceMutex_;
    std::mutex stopMutex_;
    std::condition_variable stopCv_;
    nlohmann::json store_;
    std::string mode_ = "semi";
    std::string llmMode_ = "uncertain";
    std::string llmEngine_;
    std::string llmModel_;
    // Actions the triage advisor may take. Default is read-only; "run_commands",
    // "write_files" or "use_worker_permissions" opt into the engine's worker permission args.
    RoutineAskHandler routineAskHandler_;
    std::vector<std::string> llmPermissions_{"read_files"};
    // The batch supervisor must act, so it defaults to the worker permissions. A person can
    // restrict it to the read-only advice flags when they only want proposals.
    std::vector<std::string> supervisorPermissions_{"use_worker_permissions"};
    // How many advice engines may run at once. The runs are independent processes, but
    // bounding them keeps a burst of new cards from starting one engine per card.
    int llmConcurrency_ = 2;
    std::string supervisorEngine_;
    std::string supervisorModel_;
    int supervisorTimeoutSec_ = 1800;
    std::string language_ = "en";
    // Captured instructions text per "jobId#rev", so the once-a-second refresh does not
    // re-fetch it from the task queue for every failed job.
    std::unordered_map<std::string, std::string> instructionsCache_;
    // Cards whose decision is in progress (action running, not yet recorded). A second decision for
    // the same card is refused, so a person's click and an automatic decision cannot both act on it.
    std::unordered_set<std::string> deciding_;
    std::atomic<bool> running_{false};
    std::atomic<uint64_t> lastRefreshTickMs_{0};
    std::thread thread_;
};

} // namespace pc
