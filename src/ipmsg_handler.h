#pragma once

// LLM handler for incoming Chat messages: each received message that matches
// the handler's filter is passed to a worker LLM as a Hancho task queue job.
// Implemented by the "handler" job; see ipmsg_handler.cpp.

#include "nlohmann/json.hpp"
#include "worker_engines.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace pc {

class IpmsgNode;
class ProcessManager;
class TaskQueueManager;

// config.json "chat": { ..., "handler": { ... } }
struct IpmsgHandlerConfig {
    bool enabled = false;
    std::string engine = "agy";      // worker engine (WorkerEngineRegistry)
    std::string model;               // "" = engine default
    std::string agent;               // handle messages to this agent ("" = unaddressed ones)
    std::string cwd;                 // worker working directory ("" = data dir)
    std::string instructions;        // extra system-style instructions for the worker
    std::string taskqueue = "chat"; // task queue the jobs go to
    bool autoReply = true;           // core sends the worker's final text back as a reply
    bool skipPermissions = false;    // pass the engine's permissionArgs (dangerous with LAN input)
    int timeoutSec = 600;
    int maxHops = 4;                 // stop replying once a chain reaches this many hops
    int concurrency = 1;             // task queue concurrency for handled messages

    static IpmsgHandlerConfig FromJson(const nlohmann::json& j);
    nlohmann::json ToJson() const;
};

// Runs the worker jobs for received messages and sends their answers back.
// One instance per core; the message callback only queues work so the IpmsgNode
// callback thread is never blocked.
class IpmsgHandler {
public:
    IpmsgHandler(TaskQueueManager& tqm, ProcessManager& pm, IpmsgNode* node);
    ~IpmsgHandler();

    IpmsgHandler(const IpmsgHandler&) = delete;
    IpmsgHandler& operator=(const IpmsgHandler&) = delete;

    // Replaces the configuration and engine catalog. The helper thread is
    // started the first time and kept alive across node restarts.
    void Configure(const IpmsgHandlerConfig& cfg, const std::string& dataDir,
                   const WorkerEngineRegistry* engines);
    void Start();
    void Stop();

    // Called from the IpmsgNode callback thread (received or local messages).
    // Must return quickly.
    void OnMessage(const nlohmann::json& message);

    // Config plus queued/running/done/failed counts and the last job ids, for
    // chat_status.
    nlohmann::json StatusJson() const;

private:
    struct TrackedJob {
        std::string jobId;
        uint64_t messageId = 0;
        std::string peer;
        std::string fromAgent;
        int hops = 0;
        bool started = false;
        int64_t addedMs = 0;
        nlohmann::json message;
    };

    void ThreadLoop();
    void ProcessMessage(const nlohmann::json& message);
    void CheckJobs();
    bool ShouldProcess(const IpmsgHandlerConfig& cfg, const nlohmann::json& message,
                       std::string& reason) const;
    std::string BuildPrompt(const IpmsgHandlerConfig& cfg, const nlohmann::json& message) const;
    std::string MakeJobId(uint64_t messageId) const;
    void HandleJobResult(const TrackedJob& entry, const IpmsgHandlerConfig& cfg,
                         const std::string& resultText);
    void LogEvent(const std::string& event, const nlohmann::json& body) const;

    TaskQueueManager& tqm_;
    ProcessManager& pm_;
    IpmsgNode* node_ = nullptr;
    WorkerEngineRegistry fallbackEngines_;

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<nlohmann::json> incoming_;
    std::map<std::string, TrackedJob> tracked_;
    std::deque<std::string> recentJobIds_;
    IpmsgHandlerConfig cfg_;
    std::string dataDir_;
    const WorkerEngineRegistry* engines_ = nullptr;
    int doneCount_ = 0;
    int failedCount_ = 0;
    std::atomic<bool> stop_{false};
    bool started_ = false;
    std::thread thread_;
};

} // namespace pc
