#include "bt_manager.h"
#include "behavior_tree_internal.h"
#include "common.h"
#include "nlohmann/json.hpp"

#include <windows.h>
#include <algorithm>
#include <chrono>

namespace pc {

namespace {

thread_local bool t_inBtRunnerCancel = false;

struct ScopedCancelGuard {
    bool prev;
    ScopedCancelGuard() : prev(t_inBtRunnerCancel) { t_inBtRunnerCancel = true; }
    ~ScopedCancelGuard() { t_inBtRunnerCancel = prev; }
};

std::string TrimTrailingCrLf(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
    return s;
}

std::string ShortNodeName(const std::string& btId, const std::string& nodeKey) {
    std::string prefix = btId + "/";
    if (nodeKey.rfind(prefix, 0) == 0) {
        return nodeKey.substr(prefix.size());
    }
    return nodeKey;
}

nlohmann::json BuildAddBtJobJson(const std::string& jobId,
                                 const std::string& summary,
                                 const std::string& command,
                                 const std::string& cwd,
                                 const std::string& instructionsText,
                                 int timeoutSec,
                                 int maxAttempts,
                                 const std::map<std::string, std::string>& env,
                                 const BtJobResources& resources,
                                 const BtJobSuccess& success,
                                 const BtJobIdle& idle) {
    nlohmann::json j;
    j["id"] = jobId;
    j["summary"] = summary.empty() ? jobId : summary;
    j["command"] = command;
    if (!cwd.empty()) j["cwd"] = cwd;
    if (!instructionsText.empty()) j["instructions_text"] = instructionsText;
    j["timeout_sec"] = timeoutSec > 0 ? timeoutSec : 1800;
    j["max_attempts"] = maxAttempts >= 1 ? maxAttempts : 1;
    if (!env.empty()) j["env"] = env;
    if (!resources.write.empty() || !resources.read.empty()) {
        j["resources"] = {
            {"write", resources.write},
            {"read", resources.read}
        };
    }
    nlohmann::json succObj;
    succObj["exit_code"] = success.exitCode;
    if (!success.filesNewer.empty()) succObj["files_newer"] = success.filesNewer;
    if (!success.outputRegex.empty()) succObj["output_regex"] = success.outputRegex;
    j["success"] = std::move(succObj);
    if (idle.outputSec > 0 || idle.cpuSec > 0) {
        j["idle"] = {
            {"output_sec", idle.outputSec},
            {"cpu_sec", idle.cpuSec}
        };
    }
    return j;
}

} // namespace

int64_t BtManager::NowEpochMs() {
    auto now = std::chrono::system_clock::now();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
}

std::wstring BtManager::GetBtDirectoryWide() {
    std::wstring dir = GetLogsDirectoryWide() + L"\\behaviortrees";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

std::wstring BtManager::GetBtStateFileWide(const std::string& btId) {
    return GetBtDirectoryWide() + L"\\" + Utf8ToWide(SanitizeFileNameComponent(btId)) + L".json";
}

std::wstring BtManager::GetBtEventsFileWide(const std::string& btId) {
    return GetLogsDirectoryWide() + L"\\bt-" + Utf8ToWide(SanitizeFileNameComponent(btId)) + L".events.jsonl";
}

std::wstring BtManager::GetBtResultFileWide(const std::string& btId, const std::string& jobId) {
    std::wstring btDir = GetBtDirectoryWide() + L"\\" + Utf8ToWide(SanitizeFileNameComponent(btId));
    CreateDirectoryW(btDir.c_str(), nullptr);
    std::wstring resDir = btDir + L"\\results";
    CreateDirectoryW(resDir.c_str(), nullptr);
    return resDir + L"\\" + Utf8ToWide(SanitizeFileNameComponent(jobId)) + L".json";
}

std::wstring BtManager::GetBtEscalationFileWide(const std::string& btId,
                                                const std::string& nodeKey,
                                                int run,
                                                const std::wstring& ext) {
    std::wstring btDir = GetBtDirectoryWide() + L"\\" + Utf8ToWide(SanitizeFileNameComponent(btId));
    CreateDirectoryW(btDir.c_str(), nullptr);
    std::wstring escDir = btDir + L"\\escalations";
    CreateDirectoryW(escDir.c_str(), nullptr);
    std::string shortNode = ShortNodeName(btId, nodeKey);
    return escDir + L"\\" + Utf8ToWide(SanitizeFileNameComponent(shortNode)) +
           L"." + std::to_wstring(run > 0 ? run : 1) + ext;
}

BtManager::BtManager(ProcessManager& pm, TaskQueueManager& tqm)
    : pm_(pm), tqm_(tqm) {
    std::string engErr;
    ReloadEngines(engErr);

    tqm_.SetJobFinishedCallback([this](const TaskQueueJob& job, const std::string& finalState) {
        OnTaskQueueJobFinished(job, finalState);
    });
    tqm_.SetBtRetryCallback([this](const TaskQueueBtRef& ref, bool resetCounters, std::string& outError) {
        return OnTaskQueueBtRetry(ref, resetCounters, outError);
    });

    LoadPersistedTrees();
    timerThread_ = std::thread(&BtManager::TimerLoop, this);
}

BtManager::~BtManager() {
    Shutdown();
}

void BtManager::Shutdown() {
    if (stopped_.exchange(true)) return;

    tqm_.SetJobFinishedCallback(nullptr);
    tqm_.SetBtRetryCallback(nullptr);

    timerCv_.notify_all();
    waitCv_.notify_all();
    if (timerThread_.joinable()) {
        timerThread_.join();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& [id, entry] : trees_) {
        (void)id;
        if (entry && entry->tree) {
            SaveStateFileLocked(*entry);
        }
    }
}

bool BtManager::ReloadEngines(std::string& outError) {
    outError.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    engines_.ResetToBuiltins();
    std::wstring enginesPath = GetLogsDirectoryWide() + L"\\engines.json";
    if (GetFileAttributesW(enginesPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        return true;
    }
    std::string text = ReadFileUtf8Wide(enginesPath);
    if (!engines_.LoadFromJson(text, outError)) {
        WriteAuditLog("BT_ENGINES_LOAD", "Failed to load engines.json: " + outError);
        engines_.ResetToBuiltins();
        return false;
    }
    return true;
}

std::string BtManager::DefaultEngine() const {
    return engines_.DefaultEngine();
}

bool BtManager::Knows(const std::string& engine, const std::string& model, std::string& err) const {
    return engines_.Knows(engine, model, err);
}

std::string BtManager::LookupQueueNameForBtLocked(const std::string& btId) const {
    auto it = trees_.find(btId);
    if (it != trees_.end() && it->second && it->second->tree) {
        return it->second->tree->TaskQueueName();
    }
    if (!activeLaunchingQueueName_.empty()) {
        return activeLaunchingQueueName_;
    }
    return "bt-" + btId;
}

std::string BtManager::UniqueJobId(const std::string& proposedId) const {
    TaskQueueJob existing;
    if (!tqm_.GetJobSnapshot(proposedId, existing)) return proposedId;
    for (int k = 2; k < 10000; ++k) {
        std::string candidate = proposedId + "-r" + std::to_string(k);
        if (!tqm_.GetJobSnapshot(candidate, existing)) return candidate;
    }
    return proposedId;  // give up; AddBtJob reports the clash and the node fails
}

std::string BtManager::StartJob(const BtJobSpec& spec) {
    std::string queueName = LookupQueueNameForBtLocked(spec.btId);
    const std::string jobId = UniqueJobId(spec.proposedJobId);

    nlohmann::json jobJson = BuildAddBtJobJson(
        jobId,
        spec.summary,
        spec.command,
        spec.cwd,
        spec.instructionsText,
        spec.timeoutSec,
        spec.maxAttempts,
        spec.env,
        spec.resources,
        spec.success,
        spec.idle);

    TaskQueueBtRef ref;
    ref.id = spec.btId;
    ref.node = spec.nodeKey;
    ref.run = spec.run;

    std::string err;
    tqm_.AddBtJob(queueName, jobJson.dump(), ref, err);
    if (!err.empty()) {
        WriteAuditLog("BT_JOB_ERR", "StartJob '" + jobId + "' failed: " + err);
        return "";
    }
    return jobId;
}

std::string BtManager::StartWorker(const BtWorkerSpec& spec) {
    std::string queueName = LookupQueueNameForBtLocked(spec.btId);
    const std::string jobId = UniqueJobId(spec.proposedJobId);

    std::wstring resultFileW = GetBtResultFileWide(spec.btId, jobId);
    DeleteFileW(resultFileW.c_str());
    const std::string resultPathUtf8 = WideToUtf8(resultFileW);

    // The node's outputSchema is checked when the worker exits, so the worker has to
    // see it; without it the worker guesses the shape and the node fails outputInvalid.
    std::string instructions = spec.prompt;
    if (!spec.outputSchema.empty()) {
        instructions +=
            "\n\n---\n## Required result file\n"
            "When you finish, write your result as a JSON value to the file named by the "
            "PC_BT_RESULT_FILE environment variable (" + resultPathUtf8 + "). "
            "It must match this JSON Schema:\n```json\n" + spec.outputSchema + "\n```\n";
    }

    std::wstring instrFileW = TaskQueueManager::GetInstructionsFileWide(queueName, jobId, 1);
    WriteFileAtomicWide(instrFileW, instructions);
    std::string instrPathUtf8 = WideToUtf8(instrFileW);

    std::string launchPrompt =
        "Read the instructions file " + instrPathUtf8 +
        " and carry them out. If asked for structured output, write it as JSON to the file named by the PC_BT_RESULT_FILE environment variable.";

    std::string cmd;
    std::string streamType;
    std::string err;
    int effTimeout = spec.timeoutSec > 0 ? spec.timeoutSec : 1800;
    if (!engines_.BuildCommand(spec.engine, spec.model, launchPrompt, effTimeout, cmd, streamType, err)) {
        WriteAuditLog("BT_WORKER_ERR", "BuildCommand for '" + jobId + "' failed: " + err);
        return "";
    }

    std::map<std::string, std::string> env = spec.env;
    env["PC_BT_RESULT_FILE"] = resultPathUtf8;

    nlohmann::json jobJson = BuildAddBtJobJson(
        jobId,
        spec.summary,
        cmd,
        spec.cwd,
        instructions,
        effTimeout,
        1,
        env,
        spec.resources,
        spec.success,
        spec.idle);

    TaskQueueBtRef ref;
    ref.id = spec.btId;
    ref.node = spec.nodeKey;
    ref.run = spec.run;

    tqm_.AddBtJob(queueName, jobJson.dump(), ref, err);
    if (!err.empty()) {
        WriteAuditLog("BT_WORKER_ERR", "StartWorker '" + jobId + "' failed: " + err);
        return "";
    }
    return jobId;
}

void BtManager::CancelJob(const std::string& jobId) {
    ScopedCancelGuard guard;
    std::string err;
    tqm_.TaskQueueCancel(jobId, err);
}

void BtManager::SaveStateFileLocked(const TreeEntry& entry) {
    if (!entry.tree) return;
    WriteFileAtomicWide(GetBtStateFileWide(entry.tree->Id()), entry.tree->ToStateJson());
}

void BtManager::PersistEntryLocked(TreeEntry& entry) {
    if (!entry.tree) return;
    const auto& events = entry.tree->Events();
    if (events.size() > entry.lastFlushedEventCount) {
        std::wstring evFileW = GetBtEventsFileWide(entry.tree->Id());
        for (size_t i = entry.lastFlushedEventCount; i < events.size(); ++i) {
            const BtEvent& ev = events[i];
            nlohmann::json ej;
            ej["seq"] = ev.seq;
            ej["version"] = entry.tree->Version();
            ej["type"] = ev.type;
            ej["btId"] = ev.btId;
            if (!ev.nodeKey.empty()) ej["node"] = ev.nodeKey;
            if (!ev.state.empty()) ej["state"] = ev.state;
            if (!ev.detail.empty()) ej["detail"] = ev.detail;
            if (ev.run > 0) ej["run"] = ev.run;
            if (!ev.varName.empty()) {
                ej["var"] = ev.varName;
                ej["value"] = ev.varValue;
            }
            ej["timestamp"] = FormatEpochMsIso(ev.timestampMs);

            std::string line = ej.dump();
            AppendToLogFileWide(evFileW, line + "\r\n");

            if (ev.type == "escalated" && !ev.nodeKey.empty()) {
                int run = ev.run > 0 ? ev.run : 1;
                std::string pktJson = entry.tree->GetEscalationPacketJson(ev.nodeKey);
                std::string pktMd = entry.tree->GetEscalationPacketMarkdown(ev.nodeKey);
                if (!pktJson.empty()) {
                    WriteFileAtomicWide(GetBtEscalationFileWide(entry.tree->Id(), ev.nodeKey, run, L".json"), pktJson);
                }
                if (!pktMd.empty()) {
                    WriteFileAtomicWide(GetBtEscalationFileWide(entry.tree->Id(), ev.nodeKey, run, L".md"), pktMd);
                }
            }

            if (!entry.tree->NotifyFile().empty() &&
                (ev.type == "escalated" || ev.type == "btFailed" || ev.type == "btSucceeded")) {
                std::wstring notifyPathW = ResolveBtPathWide(entry.tree->Cwd(), entry.tree->NotifyFile());
                WriteFileAtomicWide(notifyPathW, line);
            }
        }
        entry.lastFlushedEventCount = events.size();
    }

    if (entry.tree->Version() != entry.lastFlushedVersion) {
        SaveStateFileLocked(entry);
        entry.lastFlushedVersion = entry.tree->Version();
        waitCv_.notify_all();
        timerCv_.notify_all();
    }
}

void BtManager::DeliverJobOutcomeLocked(TreeEntry& entry,
                                        const TaskQueueJob& job,
                                        const std::string& finalState,
                                        int64_t nowMs,
                                        const AgentExtractedResult* preExtracted) {
    if (!entry.tree || !entry.tree->IsJobRunning(job.id)) return;

    const bool succeeded = (finalState == "succeeded");
    const int exitCode = job.attempts.empty() ? -1 : job.attempts.back().exitCode;
    const int attemptN = job.attempts.empty() ? 1 : job.attempts.back().n;
    const std::string logFile = WideToUtf8(TaskQueueManager::GetAttemptLogFileWide(job.id, attemptN));

    std::string lastLines;
    if (!job.attempts.empty() && !job.attempts.back().lastLinesFile.empty()) {
        lastLines = ReadFileUtf8Wide(Utf8ToWide(job.attempts.back().lastLinesFile));
    }
    if (lastLines.empty() && !logFile.empty()) {
        lastLines = GetTailLines(ReadFileUtf8Wide(Utf8ToWide(logFile)), 50);
    }

    std::string resultFile;
    std::wstring resFileW = GetBtResultFileWide(entry.tree->Id(), job.id);
    if (GetFileAttributesW(resFileW.c_str()) != INVALID_FILE_ATTRIBUTES) {
        resultFile = WideToUtf8(resFileW);
    }

    std::string resultText;
    if (preExtracted && preExtracted->found && !preExtracted->result.empty()) {
        resultText = TrimTrailingCrLf(preExtracted->result);
    } else if (!lastLines.empty()) {
        resultText = TrimTrailingCrLf(lastLines);
    }

    std::string failDetail;
    if (finalState == "cancelled") {
        failDetail = "cancelled";
    } else if (!succeeded && !job.attempts.empty() && !job.attempts.back().checkDetail.empty()) {
        failDetail = job.attempts.back().checkDetail;
    }

    activeLaunchingQueueName_ = entry.tree->TaskQueueName();
    entry.tree->OnJobFinished(job.id,
                              succeeded,
                              exitCode,
                              logFile,
                              lastLines,
                              resultText,
                              resultFile,
                              nowMs,
                              failDetail);
    activeLaunchingQueueName_.clear();
}

void BtManager::OnTaskQueueJobFinished(const TaskQueueJob& job, const std::string& finalState) {
    if (t_inBtRunnerCancel || stopped_.load() || job.bt.id.empty()) return;

    AgentExtractedResult extracted;
    const AgentExtractedResult* extractedPtr = nullptr;
    if (!job.attempts.empty() && job.attempts.back().processId > 0) {
        extracted = ExtractAgentResult(pm_, job.attempts.back().processId, true, 15);
        extractedPtr = &extracted;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = trees_.find(job.bt.id);
    if (it == trees_.end() || !it->second || !it->second->tree) return;

    DeliverJobOutcomeLocked(*it->second, job, finalState, NowEpochMs(), extractedPtr);
    PersistEntryLocked(*it->second);
}

bool BtManager::OnTaskQueueBtRetry(const TaskQueueBtRef& ref, bool resetCounters, std::string& outError) {
    outError.clear();
    if (ref.id.empty()) {
        outError = "invalid behavior tree reference";
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = trees_.find(ref.id);
    if (it == trees_.end() || !it->second || !it->second->tree) {
        outError = "behavior tree not found: " + ref.id;
        return false;
    }
    TreeEntry& entry = *it->second;
    activeLaunchingQueueName_ = entry.tree->TaskQueueName();
    bool ok = entry.tree->Retry(ref.node, resetCounters, NowEpochMs(), outError);
    activeLaunchingQueueName_.clear();
    if (ok) {
        PersistEntryLocked(entry);
    }
    return ok;
}

void BtManager::LoadPersistedTrees() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::wstring pattern = GetBtDirectoryWide() + L"\\*.json";
    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW(pattern.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return;

    int64_t nowMs = NowEpochMs();
    do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) continue;
        std::wstring filePath = GetBtDirectoryWide() + L"\\" + fd.cFileName;
        std::string text = ReadFileUtf8Wide(filePath);
        if (text.empty()) continue;

        auto tree = std::make_unique<BehaviorTree>();
        std::string err;
        if (!tree->LoadFromStateJson(text, this, nowMs, err)) {
            WriteAuditLog("BT_LOAD_ERR", WideToUtf8(filePath) + ": " + err);
            continue;
        }
        tree->SetJobRunner(this);

        auto entry = std::make_unique<TreeEntry>();
        entry->lastFlushedVersion = tree->Version();
        entry->lastFlushedEventCount = tree->Events().size();
        std::string btId = tree->Id();
        entry->tree = std::move(tree);
        TreeEntry* entryPtr = entry.get();
        trees_[btId] = std::move(entry);

        const std::string st = entryPtr->tree->State();
        if (st == "running" || st == "waitingForLlm") {
            auto runningIds = entryPtr->tree->RunningJobIds();
            for (const auto& jobId : runningIds) {
                TaskQueueJob snap;
                if (!tqm_.GetJobSnapshot(jobId, snap)) {
                    activeLaunchingQueueName_ = entryPtr->tree->TaskQueueName();
                    entryPtr->tree->OnJobFinished(jobId, false, -1, "", "", "", "", nowMs, "jobMissing");
                    activeLaunchingQueueName_.clear();
                } else if (snap.state == "succeeded" || snap.state == "failed" || snap.state == "cancelled") {
                    DeliverJobOutcomeLocked(*entryPtr, snap, snap.state, nowMs, nullptr);
                }
            }
            activeLaunchingQueueName_ = entryPtr->tree->TaskQueueName();
            entryPtr->tree->Tick(nowMs);
            activeLaunchingQueueName_.clear();
            PersistEntryLocked(*entryPtr);
        }
    } while (FindNextFileW(hFind, &fd));

    FindClose(hFind);
}

void BtManager::TimerLoop() {
    while (!stopped_.load()) {
        int64_t sleepMs = 500;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            int64_t nowMs = NowEpochMs();
            int64_t earliestWake = 0;
            for (auto& [id, entry] : trees_) {
                (void)id;
                if (!entry || !entry->tree) continue;
                int64_t w = entry->tree->NextWakeMs();
                if (w > 0 && w <= nowMs) {
                    activeLaunchingQueueName_ = entry->tree->TaskQueueName();
                    entry->tree->OnTimer(nowMs);
                    activeLaunchingQueueName_.clear();
                    PersistEntryLocked(*entry);
                    w = entry->tree->NextWakeMs();
                }
                if (w > 0 && (earliestWake == 0 || w < earliestWake)) {
                    earliestWake = w;
                }
            }
            if (stopped_.load()) break;
            nowMs = NowEpochMs();
            if (earliestWake > 0) {
                int64_t diff = earliestWake - nowMs;
                sleepMs = std::max<int64_t>(10, std::min<int64_t>(diff, 500));
            }
            timerCv_.wait_for(lock, std::chrono::milliseconds(sleepMs));
        }
    }
}

} // namespace pc
