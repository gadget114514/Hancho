#include "triage_manager.h"

#include "bt_manager.h"
#include "common.h"
#include "process_manager.h"
#include "taskqueue.h"
#include "triage.h"
#include "worker_engines.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <fstream>
#include <map>
#include <sstream>
#include <vector>

namespace pc {
namespace {

using json = nlohmann::json;

std::string GetString(const json& value, const char* key, const std::string& fallback = "") {
    auto it = value.find(key);
    return it != value.end() && it->is_string() ? it->get<std::string>() : fallback;
}

long long GetNumber(const json& value, const char* key, long long fallback = 0) {
    auto it = value.find(key);
    if (it == value.end() || !it->is_number()) return fallback;
    try { return it->get<long long>(); } catch (...) { return fallback; }
}

json GetObject(const json& value, const char* key) {
    auto it = value.find(key);
    return it != value.end() && it->is_object() ? *it : json::object();
}

json Error(const std::string& message) {
    return {{"ok", false}, {"error", message}};
}

bool ResponseOk(const std::string& response, std::string& error) {
    json parsed = json::parse(response, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        error = "invalid response from Hancho core";
        return false;
    }
    if (!parsed.value("ok", false)) {
        error = parsed.value("error", std::string("operation failed"));
        return false;
    }
    return true;
}

std::wstring TriageDirectory() {
    return PathJoinWide(GetLogsDirectoryWide(), L"triage");
}

std::wstring CardsPath() {
    return PathJoinWide(TriageDirectory(), L"cards.json");
}

std::wstring AdviceResultPath(const std::string& jobId) {
    return PathJoinWide(PathJoinWide(TriageDirectory(), L"results"),
                        Utf8ToWide(SanitizeFileNameComponent(jobId)) + L".json");
}

std::string ReadConfig() {
    return ReadFileUtf8Wide(PathJoinWide(GetEffectiveDataDirectoryWide(), L"config.json"));
}

void CollectEscalations(const json& node, const std::string& btId, const json& treeMeta, json& failures) {
    if (!node.is_object()) return;
    const json status = GetObject(node, "status");
    if (GetString(node, "type") == "escalate" && GetString(status, "state") == "failed") {
        const std::string nodeId = GetString(node, "id");
        if (!nodeId.empty()) {
            json relatedJobs = json::array();
            for (const char* source : {"jobs", "jobIds"}) {
                const auto values = node.find(source);
                if (values != node.end() && values->is_array())
                    for (const auto& value : *values) {
                        if (value.is_string()) relatedJobs.push_back(value);
                        else if (value.is_object() && value.contains("id")) relatedJobs.push_back(value["id"]);
                    }
                const auto statusValues = status.find(source);
                if (statusValues != status.end() && statusValues->is_array())
                    for (const auto& value : *statusValues) {
                        if (value.is_string()) relatedJobs.push_back(value);
                        else if (value.is_object() && value.contains("id")) relatedJobs.push_back(value["id"]);
                    }
            }
            json options = node.value("options", node.value("choices", json::array()));
            json item = {{"btId", btId}, {"nodeId", nodeId},
                         {"detail", GetString(status, "detail", "escalation failed")},
                         {"treeName", GetString(treeMeta, "name")},
                         {"treeState", GetString(treeMeta, "state")},
                         {"queue", GetString(treeMeta, "queue")},
                         {"question", GetString(node, "question")}, {"options", options},
                         {"guidance", GetString(node, "guidance")}, {"jobs", relatedJobs}};
            const long long timeout = GetNumber(node, "timeout_sec", GetNumber(node, "timeoutSec", 0));
            if (timeout > 0) item["timeoutSec"] = timeout;
            failures.push_back(std::move(item));
        }
    }
    auto children = node.find("children");
    if (children != node.end() && children->is_array()) {
        for (const auto& child : *children) CollectEscalations(child, btId, treeMeta, failures);
    }
    for (const char* slot : {"child", "cond", "then", "else", "until"}) {
        auto child = node.find(slot);
        if (child != node.end() && child->is_object()) CollectEscalations(*child, btId, treeMeta, failures);
    }
}

json ProcessToJson(const ProcessSnapshot& snapshot) {
    return {{"id", snapshot.id}, {"name", snapshot.name}, {"command", snapshot.commandLine},
            {"cwd", snapshot.workingDir}, {"mode", ExecModeToString(snapshot.mode)},
            {"state", ProcStateToString(snapshot.state)}, {"exit_code", snapshot.exitCode},
            {"start_time", snapshot.startTimeStr}, {"elapsed_ms", snapshot.elapsedMs},
            {"timeout_sec", snapshot.timeoutSec}, {"idle_timeout_sec", snapshot.idleTimeoutSec},
            {"log_file", snapshot.logFilePath}, {"job_key", snapshot.jobKey},
            {"job_id", snapshot.jobId}, {"attempt", snapshot.attemptN}};
}

std::string FailureClassFor(const json& card) {
    const json info = GetObject(card, "info");
    auto cause = info.find("cause");
    if (cause == info.end() || !cause->is_array()) return "unknown";
    for (const auto& line : *cause) {
        if (line.is_object() && GetString(line, "k") == "failureClass") {
            const json value = line.value("v", json::object());
            return GetString(value, "class", "unknown");
        }
        if (!line.is_string()) continue;
        const std::string text = line.get<std::string>();
        const std::string prefix = "failure class: ";
        if (text.rfind(prefix, 0) == 0) {
            const size_t end = text.find(' ', prefix.size());
            return text.substr(prefix.size(), end == std::string::npos ? std::string::npos : end - prefix.size());
        }
    }
    return "unknown";
}

std::string CardTargetJob(const json& card) {
    return GetString(GetObject(card, "target"), "jobId");
}

std::string CardTargetQueue(const json& card) {
    return GetString(GetObject(card, "target"), "queue");
}

bool TracksOutcome(const json& card) {
    const std::string kind = GetString(card, "kind");
    return kind == "job_failure" || kind == "bt_escalation_failure";
}

bool IsClosedCard(const json& card) {
    const std::string state = GetString(card, "state");
    if (state == "ignored") return true;
    if (!TracksOutcome(card)) return state == "acted" || state == "resolved";
    const json outcome = GetObject(card, "outcome");
    auto complete = outcome.find("complete");
    return complete != outcome.end() && complete->is_boolean() && complete->get<bool>();
}

std::string OutcomeJobKey(const json& job) {
    const std::string id = GetString(job, "id");
    const json attempts = job.value("attempts", json::array());
    const long long count = attempts.is_array() && !attempts.empty() && attempts.back().is_object()
        ? GetNumber(attempts.back(), "n", static_cast<long long>(attempts.size()))
        : GetNumber(job, "attempt_count", attempts.is_array() ? static_cast<long long>(attempts.size()) : 0);
    return id.empty() ? std::string() : "job:" + id + "#" + std::to_string(count);
}

const json* FindCandidate(const json& card, const std::string& id) {
    auto candidates = card.find("candidates");
    if (candidates == card.end() || !candidates->is_array()) return nullptr;
    for (const auto& candidate : *candidates) {
        if (GetString(candidate, "id") == id) return &candidate;
    }
    return nullptr;
}

} // namespace

TriageManager::TriageManager(ProcessManager& pm, TaskQueueManager& tqm, BtManager* btm)
    : pm_(pm), tqm_(tqm), btm_(btm),
      store_({{"schema", "hancho.triage/1"}, {"cards", json::array()}, {"autoRetries", json::array()}}) {
    EnsureDirectoryWide(TriageDirectory());
    json loaded = json::parse(ReadFileUtf8Wide(CardsPath()), nullptr, false);
    if (loaded.is_object()) store_ = std::move(loaded);
    if (!store_.contains("cards") || !store_["cards"].is_array()) store_["cards"] = json::array();
    if (!store_.contains("autoRetries") || !store_["autoRetries"].is_array()) store_["autoRetries"] = json::array();
}

TriageManager::~TriageManager() {
    Stop();
}

void TriageManager::Start() {
    if (running_.exchange(true)) return;
    Refresh();
    thread_ = std::thread(&TriageManager::Loop, this);
}

void TriageManager::Stop() {
    if (!running_.exchange(false)) return;
    if (thread_.joinable()) thread_.join();
}

void TriageManager::Loop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::seconds(5));
        if (!running_) break;
        try { Refresh(); }
        catch (const std::exception& e) { WriteAuditLog("TRIAGE_REFRESH_ERR", e.what()); }
        catch (...) { WriteAuditLog("TRIAGE_REFRESH_ERR", "unknown exception"); }
    }
}

void TriageManager::SaveLocked() {
    EnsureDirectoryWide(TriageDirectory());
    WriteFileAtomicWide(CardsPath(), store_.dump(2));
}

void TriageManager::Refresh() {
    std::lock_guard<std::mutex> refreshLock(refreshMutex_);
    std::string configText = ReadConfig();
    json config = json::parse(configText, nullptr, false);
    if (!config.is_object()) config = json::object();
    const std::string mode = GetString(config, "triage_mode", "semi");
    const std::string llmMode = GetString(config, "triage_llm_mode", "uncertain");
    const std::string engine = GetString(config, "triage_llm_engine");
    const std::string model = GetString(config, "triage_llm_model");
    const std::string supervisorEngine = GetString(config, "triage_supervisor_engine");
    const std::string supervisorModel = GetString(config, "triage_supervisor_model");
    const long long supervisorTimeout = GetNumber(config, "triage_supervisor_timeout_sec", 1800);
    const std::string language = GetString(config, "language", "en");

    std::string err;
    json listed = json::parse(tqm_.TaskQueueList("", "", 0, err, "", "", true), nullptr, false);
    if (!err.empty() || !listed.is_object() || !listed.value("ok", false)) {
        WriteAuditLog("TRIAGE_REFRESH_ERR", err.empty() ? "taskqueue list returned an invalid response" : err);
        return;
    }
    json jobs = listed.is_object() && listed.contains("jobs") && listed["jobs"].is_array()
        ? listed["jobs"] : json::array();
    TriageInputs inputs;
    inputs.jobs = jobs;
    inputs.config = config;
    inputs.now = GetCurrentTimestampStr();

    if (btm_) {
        json treesDoc = json::parse(btm_->List(), nullptr, false);
        if (treesDoc.is_object() && treesDoc.contains("trees") && treesDoc["trees"].is_array()) {
            inputs.btStates = json::object();
            inputs.btFailures = json::array();
            for (const auto& summary : treesDoc["trees"]) {
                const std::string btId = GetString(summary, "id");
                if (btId.empty()) continue;
                inputs.btStates[btId] = GetString(summary, "state");
                json treeDoc = json::parse(btm_->Get(btId, false), nullptr, false);
                if (!treeDoc.is_object()) continue;
                const json tree = GetObject(treeDoc, "tree");
                auto root = tree.find("root");
                if (root != tree.end()) {
                    json treeMeta = tree;
                    treeMeta["state"] = GetString(summary, "state");
                    CollectEscalations(*root, btId, treeMeta, inputs.btFailures);
                }
            }
        }
    }

    json processes = json::array();
    for (const auto& snapshot : pm_.GetAllSnapshots()) processes.push_back(ProcessToJson(snapshot));
    json fresh = BuildTriageCandidates(inputs);
    json processCards = BuildProcessTriageCandidates(processes);
    for (auto& card : processCards) {
        card["createdAt"] = inputs.now;
        card["updatedAt"] = inputs.now;
        fresh.push_back(std::move(card));
    }

    std::vector<std::tuple<std::string, std::string, std::string>> automatic;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mode_ = mode == "manual" || mode == "semi" || mode == "auto" ? mode : "semi";
        llmMode_ = llmMode == "manual" || llmMode == "always" || llmMode == "uncertain" ? llmMode : "uncertain";
        llmEngine_ = engine;
        llmModel_ = model;
        supervisorEngine_ = supervisorEngine;
        supervisorModel_ = supervisorModel;
        supervisorTimeoutSec_ = static_cast<int>(std::clamp<long long>(supervisorTimeout, 60, 7200));
        language_ = language.empty() ? "en" : language;
        const json before = store_;
        ApplyTriageKindSettings(store_, fresh, config, inputs.now);
        MergeTriageCards(store_, fresh, inputs.now);
        json supervisorJobs = json::array();
        for (const auto& job : jobs) {
            if (GetString(job, "taskqueue") == "_supervisor") supervisorJobs.push_back(job);
        }
        UpdateHandoffsLocked(supervisorJobs);
        UpdateOutcomesLocked(jobs, inputs.btStates);
        for (auto& card : store_["cards"]) {
            if (GetString(card, "state") != "pending") continue;
            const std::string cardId = GetString(card, "id");
            if (GetString(card, "adviceState", "none") == "none" &&
                !(mode_ != "auto" && llmMode_ == "manual") &&
                TriageNeedsLlm(card, mode_, llmMode_)) {
                card["adviceState"] = "requested";
                card["updatedAt"] = inputs.now;
            }
            std::string by, whyNot;
            const std::string choice = TriageAutoChoice(store_, card, mode_, by, whyNot);
            if (!choice.empty()) automatic.emplace_back(cardId, choice, by);
        }
        // Refresh runs every few seconds and on every triage_list; only write when something changed.
        if (store_ != before) SaveLocked();
    }

    for (const auto& [cardId, candidateId, by] : automatic) {
        DecideInternal(cardId, candidateId, "Automatic triage recommendation", json::object(), by);
    }
    PumpAdvice();
}

void TriageManager::PumpAdvice() {
    std::lock_guard<std::mutex> adviceLock(adviceMutex_);
    std::string err;
    const std::string queueResponse = tqm_.TaskQueueList("_triage", "", 0, err, "", "", true);
    json queue = json::parse(queueResponse, nullptr, false);
    json queueJobs = queue.is_object() && queue.contains("jobs") && queue["jobs"].is_array()
        ? queue["jobs"] : json::array();
    std::string finishedCardId;
    std::string finishedJobId;
    std::string finishedState;
    json finishedJob;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& card : store_["cards"]) {
            if (GetString(card, "adviceState") != "running") continue;
            json advice = GetObject(card, "advice");
            const std::string jobId = GetString(advice, "jobId");
            if (jobId.empty()) continue;
            auto found = std::find_if(queueJobs.begin(), queueJobs.end(), [&](const json& job) {
                return GetString(job, "id") == jobId;
            });
            if (found == queueJobs.end()) continue;
            const std::string state = GetString(*found, "state");
            if (state == "pending" || state == "running" || state == "waiting" || state == "blocked") continue;
            finishedCardId = GetString(card, "id");
            finishedJobId = jobId;
            finishedState = state;
            finishedJob = *found;
            break;
        }
    }

    std::vector<std::tuple<std::string, std::string, std::string>> automatic;
    if (!finishedCardId.empty()) {
        json parsedAdvice;
        std::string adviceError;
        const std::string resultText = ReadFileUtf8Wide(AdviceResultPath(finishedJobId));
        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto found = std::find_if(store_["cards"].begin(), store_["cards"].end(), [&](const json& card) {
                return GetString(card, "id") == finishedCardId;
            });
            if (found != store_["cards"].end()) {
                if (finishedState == "succeeded" && ParseTriageAdvice(resultText, *found, parsedAdvice, adviceError)) {
                    parsedAdvice["jobId"] = finishedJobId;
                    parsedAdvice["at"] = GetCurrentTimestampStr();
                    (*found)["advice"] = std::move(parsedAdvice);
                    (*found)["adviceState"] = "done";
                } else {
                    if (adviceError.empty()) {
                        adviceError = finishedState == "succeeded" ? "LLM result file is missing or invalid"
                                                                  : "LLM advice job ended in state " + finishedState;
                        json attempts = finishedJob.value("attempts", json::array());
                        if (!attempts.empty() && attempts.back().is_object()) {
                            const json failure = GetObject(attempts.back(), "failure");
                            const std::string summary = GetString(failure, "summary");
                            if (!summary.empty()) adviceError += ": " + summary;
                        }
                    }
                    (*found)["advice"] = {{"source", "llm"}, {"jobId", finishedJobId},
                                          {"at", GetCurrentTimestampStr()}, {"error", adviceError}};
                    (*found)["adviceState"] = "failed";
                }
                (*found)["updatedAt"] = GetCurrentTimestampStr();
                std::string by, whyNot;
                const std::string choice = TriageAutoChoice(store_, *found, mode_, by, whyNot);
                if (!choice.empty()) automatic.emplace_back(finishedCardId, choice, by);
                SaveLocked();
            }
        }
    }
    for (const auto& [cardId, candidateId, by] : automatic) {
        DecideInternal(cardId, candidateId, "Automatic triage advice", json::object(), by);
    }

    // Start the next requested card only when there is no advice worker in flight.
    json cardToStart;
    std::string jobId;
    std::string engineName;
    std::string modelName;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const bool active = std::any_of(store_["cards"].begin(), store_["cards"].end(), [](const json& card) {
            return GetString(card, "adviceState") == "running";
        });
        if (active) return;
        auto requested = std::find_if(store_["cards"].begin(), store_["cards"].end(), [](const json& card) {
            return GetString(card, "state") == "pending" && GetString(card, "adviceState") == "requested";
        });
        if (requested == store_["cards"].end()) return;
        cardToStart = *requested;
        const int sequence = static_cast<int>(GetNumber(store_, "nextAdvice", 0)) + 1;
        store_["nextAdvice"] = sequence;
        jobId = "triage-" + GetString(cardToStart, "id") + "-" + std::to_string(sequence);
        engineName = llmEngine_;
        modelName = llmModel_;
        if (engineName.empty()) engineName = btm_ ? btm_->DefaultEngine() : std::string();
    }

    WorkerEngineRegistry localEngines;
    const WorkerEngineRegistry* engines = nullptr;
    if (btm_) engines = &btm_->Engines();
    else {
        const std::string engineConfig = ReadFileUtf8Wide(PathJoinWide(GetLogsDirectoryWide(), L"engines.json"));
        if (!engineConfig.empty()) {
            std::string loadError;
            if (!localEngines.LoadFromJson(engineConfig, loadError)) {
                std::lock_guard<std::mutex> lock(mutex_);
                auto card = std::find_if(store_["cards"].begin(), store_["cards"].end(), [&](const json& item) {
                    return GetString(item, "id") == GetString(cardToStart, "id");
                });
                if (card != store_["cards"].end()) {
                    (*card)["advice"] = {{"source", "llm"}, {"error", loadError}};
                    (*card)["adviceState"] = "failed";
                    SaveLocked();
                }
                return;
            }
        }
        if (engineName.empty()) engineName = localEngines.DefaultEngine();
        engines = &localEngines;
    }
    if (!engines || engineName.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto card = std::find_if(store_["cards"].begin(), store_["cards"].end(), [&](const json& item) {
            return GetString(item, "id") == GetString(cardToStart, "id");
        });
        if (card != store_["cards"].end()) {
            (*card)["advice"] = {{"source", "llm"}, {"error", "no worker engine is configured"}};
            (*card)["adviceState"] = "failed";
            SaveLocked();
        }
        return;
    }

    const std::wstring resultPath = AdviceResultPath(jobId);
    EnsureDirectoryWide(PathJoinWide(TriageDirectory(), L"results"));
    RemoveFileWide(resultPath);
    const std::wstring instructionPath = PathJoinWide(PathJoinWide(TriageDirectory(), L"instructions"),
        Utf8ToWide(SanitizeFileNameComponent(jobId)) + L".md");
    EnsureDirectoryWide(PathJoinWide(TriageDirectory(), L"instructions"));
    // Answer in the dashboard's UI language so the advice reads naturally next to the card.
    json uiConfig = json::parse(ReadConfig(), nullptr, false);
    std::string language = uiConfig.is_object() ? GetString(uiConfig, "language", "en") : "en";
    if (language.empty()) language = "en";
    const std::string instruction = BuildTriageAdvicePacket(cardToStart, language) +
        "\nRequired result JSON Schema:\n```json\n" + TriageAdviceOutputSchema() + "\n```\n";
    WriteFileAtomicWide(instructionPath, instruction);
    const std::string launchPrompt = "Read the instructions file " + WideToUtf8(instructionPath) +
        " and write the required JSON result to the file named by PC_TRIAGE_RESULT_FILE.";
    std::string command, streamType, commandError, adapter;
    if (!engines->BuildCommand(engineName, modelName, launchPrompt, 300, command, streamType, commandError, &adapter)) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto card = std::find_if(store_["cards"].begin(), store_["cards"].end(), [&](const json& item) {
            return GetString(item, "id") == GetString(cardToStart, "id");
        });
        if (card != store_["cards"].end()) {
            (*card)["advice"] = {{"source", "llm"}, {"error", commandError}};
            (*card)["adviceState"] = "failed";
            SaveLocked();
        }
        return;
    }
    json env = {{"PC_TRIAGE_RESULT_FILE", WideToUtf8(resultPath)}};
    if (!adapter.empty()) env["HANCHO_ADAPTER"] = adapter;
    json job = {{"id", jobId}, {"name", "triage-llm"}, {"summary", "Triage advice for " + GetString(cardToStart, "id")},
                {"command", command}, {"cwd", WideToUtf8(GetEffectiveDataDirectoryWide())},
                {"instructions_text", instruction}, {"timeout_sec", 300}, {"max_attempts", 1},
                {"hidden", true}, {"agent_type", engineName}, {"env", env}};
    const std::string addResponse = tqm_.TaskQueueAdd("_triage", json::array({job}).dump(), 1, "", err);
    std::string addError;
    if (!err.empty() || !ResponseOk(addResponse, addError)) {
        if (err.empty()) err = addError;
        std::lock_guard<std::mutex> lock(mutex_);
        auto card = std::find_if(store_["cards"].begin(), store_["cards"].end(), [&](const json& item) {
            return GetString(item, "id") == GetString(cardToStart, "id");
        });
        if (card != store_["cards"].end()) {
            (*card)["advice"] = {{"source", "llm"}, {"error", err}};
            (*card)["adviceState"] = "failed";
            SaveLocked();
        }
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto card = std::find_if(store_["cards"].begin(), store_["cards"].end(), [&](const json& item) {
            return GetString(item, "id") == GetString(cardToStart, "id");
        });
        if (card != store_["cards"].end() && GetString(*card, "state") == "pending") {
            (*card)["advice"] = {{"source", "llm"}, {"jobId", jobId}, {"at", GetCurrentTimestampStr()}};
            (*card)["adviceState"] = "running";
            (*card)["updatedAt"] = GetCurrentTimestampStr();
            SaveLocked();
        }
    }
}

void TriageManager::RefreshIfStale() {
    // The dashboard asks on every refresh; rebuilding cards (all jobs, every BT) at most once a second
    // is enough, and the 5 s background loop keeps them current when nobody is asking.
    const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
    const uint64_t last = lastRefreshTickMs_.load();
    if (last != 0 && now >= last && now - last < 1000) return;
    lastRefreshTickMs_ = now;
    Refresh();
}

std::string TriageManager::List(const std::string& requestedState) {
    RefreshIfStale();
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string state = requestedState.empty() ? "pending" : requestedState;
    if (state != "pending" && state != "all" && state != "acted" && state != "ignored" && state != "resolved" &&
        state != "open" && state != "follow_up")
        return Error("state must be pending, all, acted, ignored, resolved, open, or follow_up").dump();
    json cards = json::array();
    int pendingTask = 0;
    int pendingProcess = 0;
    int pendingOther = 0;
    int autoUnreviewed = 0;
    int acted = 0;
    int ignored = 0;
    int resolved = 0;
    int withSupervisor = 0;
    int handling = 0, running = 0, waitingBranch = 0, waitingNext = 0, followUp = 0, done = 0, open = 0;
    json jobs = json::object();
    std::vector<const json*> ordered;
    for (const auto& card : store_["cards"]) ordered.push_back(&card);
    std::stable_sort(ordered.begin(), ordered.end(), [](const json* a, const json* b) {
        return GetString(*a, "createdAt") > GetString(*b, "createdAt");
    });
    for (const json* card : ordered) {
        const std::string cardState = GetString(*card, "state");
        const std::string lifecycle = GetString(*card, "lifecycle", cardState == "pending" ? "pending" : "done");
        if (lifecycle == "handling") ++handling;
        else if (lifecycle == "running") ++running;
        else if (lifecycle == "waiting_branch") ++waitingBranch;
        else if (lifecycle == "waiting_next") ++waitingNext;
        else if (lifecycle == "follow_up") ++followUp;
        else if (lifecycle == "done") ++done;
        if (lifecycle != "done") ++open;
        if (cardState == "acted") ++acted;
        else if (cardState == "ignored") ++ignored;
        else if (cardState == "resolved") ++resolved;
        const std::string handoffState = GetString(GetObject(*card, "handoff"), "state");
        if (cardState == "pending" && (handoffState == "queued" || handoffState == "running")) ++withSupervisor;
        if (cardState == "pending") {
            const std::string kind = GetString(*card, "kind");
            if (kind == "job_failure" || kind == "bt_escalation_failure") ++pendingTask;
            else if (kind == "process_failure") ++pendingProcess;
            else ++pendingOther;
        }
        const json decision = GetObject(*card, "decision");
        if (cardState == "acted" && decision.contains("reviewed") && decision["reviewed"].is_boolean() &&
            !decision["reviewed"].get<bool>()) ++autoUnreviewed;
        if (state == "all" || cardState == state || lifecycle == state || (state == "open" && lifecycle != "done"))
            cards.push_back(*card);
        const std::string jobId = CardTargetJob(*card);
        if (!jobId.empty() && !jobs.contains(jobId)) {
            json summary = {{"cardId", GetString(*card, "id")}, {"state", cardState}, {"lifecycle", lifecycle}};
            if (!handoffState.empty()) summary["handoff"] = handoffState;
            if (!decision.is_null() && decision.is_object()) {
                summary["candidateId"] = GetString(decision, "candidateId");
                summary["by"] = GetString(decision, "by");
                summary["reason"] = GetString(decision, "reason");
            }
            jobs[jobId] = std::move(summary);
        }
    }
    json counts = {{"pendingTask", pendingTask}, {"pendingProcess", pendingProcess},
                   {"pendingOther", pendingOther}, {"autoUnreviewed", autoUnreviewed},
                    {"acted", acted}, {"ignored", ignored}, {"resolved", resolved},
                    {"withSupervisor", withSupervisor}, {"handling", handling}, {"running", running},
                    {"waitingBranch", waitingBranch}, {"waitingNext", waitingNext},
                    {"followUp", followUp}, {"done", done}, {"open", open}};
    return json{{"ok", true}, {"cards", std::move(cards)}, {"counts", std::move(counts)},
                {"jobs", std::move(jobs)}, {"mode", mode_}, {"llmMode", llmMode_}}.dump();
}

std::string TriageManager::Get(const std::string& id) {
    RefreshIfStale();
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& card : store_["cards"]) {
        if (GetString(card, "id") == id) return json{{"ok", true}, {"card", card}}.dump();
    }
    return Error("triage card not found: " + id).dump();
}

std::string TriageManager::RequestAdvice(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto card = std::find_if(store_["cards"].begin(), store_["cards"].end(), [&](const json& item) {
            return GetString(item, "id") == id;
        });
        if (card == store_["cards"].end()) return Error("triage card not found: " + id).dump();
        if (GetString(*card, "state") != "pending") return Error("only pending cards can request advice").dump();
        if (GetString(*card, "adviceState") == "none" || GetString(*card, "adviceState") == "failed") {
            (*card)["adviceState"] = "requested";
            (*card)["advice"] = nullptr;
            (*card)["updatedAt"] = GetCurrentTimestampStr();
            SaveLocked();
        }
    }
    PumpAdvice();
    return Get(id);
}

std::string TriageManager::Acknowledge(const std::string& id, bool allAuto) {
    std::lock_guard<std::mutex> lock(mutex_);
    int changed = 0;
    json selected;
    for (auto& card : store_["cards"]) {
        if (allAuto) {
            json decision = GetObject(card, "decision");
            if (GetString(card, "state") != "acted" || !decision.is_object() ||
                (GetString(decision, "by") != "hancho-auto" && GetString(decision, "by") != "llm-auto") ||
                (decision.contains("reviewed") && decision["reviewed"].is_boolean() && decision["reviewed"].get<bool>())) continue;
            decision["reviewed"] = true;
            card["decision"] = decision;
            card["updatedAt"] = GetCurrentTimestampStr();
            ++changed;
        } else if (GetString(card, "id") == id) {
            json decision = GetObject(card, "decision");
            if (GetString(card, "state") != "acted" || !decision.is_object() ||
                (GetString(decision, "by") != "hancho-auto" && GetString(decision, "by") != "llm-auto"))
                return Error("card has no automatic decision to acknowledge").dump();
            decision["reviewed"] = true;
            card["decision"] = decision;
            card["updatedAt"] = GetCurrentTimestampStr();
            selected = card;
            ++changed;
            break;
        }
    }
    if (!changed) return Error(allAuto ? "no unreviewed automatic decisions" : "triage card not found: " + id).dump();
    SaveLocked();
    if (allAuto) return json{{"ok", true}, {"acknowledged", changed}}.dump();
    return json{{"ok", true}, {"card", std::move(selected)}}.dump();
}

std::string TriageManager::Decide(const std::string& id, const std::string& candidateId,
                                 const std::string& reason, const json& params, const std::string& by) {
    // A person choosing "ask the supervisor to fix the specification" hands the card to a batch
    // supervisor; the card stays pending until the supervisor records what it did.
    if (candidateId == "fix_spec" && by == "human") return Handoff({id}, false);
    return DecideInternal(id, candidateId, reason, params, by);
}

std::string TriageManager::DecideInternal(const std::string& id, const std::string& candidateId,
                                          const std::string& reason, const json& params,
                                          const std::string& by) {
    json card;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = std::find_if(store_["cards"].begin(), store_["cards"].end(), [&](const json& item) {
            return GetString(item, "id") == id;
        });
        if (found == store_["cards"].end()) return Error("triage card not found: " + id).dump();
        if (GetString(*found, "state") != "pending") return Error("triage card is not pending").dump();
        if (!FindCandidate(*found, candidateId)) return Error("candidate is not present on this card").dump();
        if (candidateId == "ignore" && reason.empty()) return Error("ignore requires a reason").dump();
        if (candidateId == "ack" && reason.empty()) return Error("ack requires a reason").dump();
        if (candidateId == "cancel" && by == "supervisor" && reason.empty())
            return Error("supervisor cancel requires a reason").dump();
        if (by != "human" && by != "supervisor" && by != "hancho-auto" && by != "llm-auto")
            return Error("by must be human, supervisor, hancho-auto, or llm-auto").dump();
        if ((by == "hancho-auto" || by == "llm-auto") && !GetString(*found, "id").empty()) {
            const json* candidate = FindCandidate(*found, candidateId);
            if (!candidate || !candidate->value("safe", false) ||
                GetObject(*found, "flags").value("needsHuman", false))
                return Error("automatic decision failed safety guards").dump();
        }
        card = *found;
    }

    const json target = GetObject(card, "target");
    std::string result = "Recorded " + candidateId + ".";
    std::string actionError;
    if (candidateId == "retry" || candidateId == "retry_raise_timeout") {
        const std::string jobId = GetString(target, "jobId");
        if (jobId.empty()) return Error("job target is missing").dump();
        if (candidateId == "retry_raise_timeout") {
            TaskQueueJob snapshot;
            if (!tqm_.GetJobSnapshot(jobId, snapshot)) return Error("task queue job not found: " + jobId).dump();
            // The 4x cap is measured from the job's first timeout, not from an already raised one.
            int original = static_cast<int>(GetNumber(target, "originalTimeoutSec", snapshot.timeoutSec));
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (!store_.contains("originalTimeouts") || !store_["originalTimeouts"].is_object())
                    store_["originalTimeouts"] = json::object();
                json& originals = store_["originalTimeouts"];
                if (originals.contains(jobId) && originals[jobId].is_number()) original = originals[jobId].get<int>();
                else originals[jobId] = original;
            }
            double multiplier = 2.0;
            if (params.contains("multiplier") && params["multiplier"].is_number()) multiplier = params["multiplier"].get<double>();
            if (!std::isfinite(multiplier) || multiplier < 1.5 || multiplier > 4.0)
                return Error("multiplier must be between 1.5 and 4").dump();
            const std::wstring queuePath = TaskQueueManager::GetTaskQueueFileWide(snapshot.taskqueue);
            json queueFile = json::parse(ReadFileUtf8Wide(queuePath), nullptr, false);
            int queueMax = queueFile.is_object() ? static_cast<int>(GetNumber(queueFile, "max_timeout_sec", 7200)) : 7200;
            int newTimeout = static_cast<int>(std::ceil(original * multiplier));
            newTimeout = std::min(newTimeout, std::min(original * 4, queueMax));
            if (original <= 0 || newTimeout <= snapshot.timeoutSec)
                return Error("timeout cannot be raised within the configured cap").dump();
            const std::string updated = tqm_.TaskQueueUpdate(jobId, json{{"timeout_sec", newTimeout}}.dump(),
                                                              TaskQueueCaller::External, actionError);
            if (!actionError.empty() || !ResponseOk(updated, actionError)) return Error(actionError).dump();
            result = "Raised timeout to " + std::to_string(newTimeout) + " seconds.";
        }
        const std::string response = tqm_.TaskQueueRetry(jobId, false, false, TaskQueueCaller::External, actionError);
        if (!actionError.empty() || !ResponseOk(response, actionError)) return Error(actionError).dump();
        result += " Retried job " + jobId + ".";
    } else if (candidateId == "cancel") {
        const std::string response = tqm_.TaskQueueCancel(GetString(target, "jobId"), actionError);
        if (!actionError.empty() || !ResponseOk(response, actionError)) return Error(actionError).dump();
        result = "Cancelled job " + GetString(target, "jobId") + ".";
    } else if (candidateId == "recalibrate") {
        std::string listError;
        json listed = json::parse(tqm_.TaskQueueList("", "", 0, listError), nullptr, false);
        const int actual = static_cast<int>(GetNumber(target, "actualDurationSec", 0));
        const std::string name = GetString(target, "name");
        const std::string queueName = GetString(target, "queue");
        int updatedCount = 0;
        if (listed.is_object() && listed.contains("jobs") && listed["jobs"].is_array()) {
            for (const auto& job : listed["jobs"]) {
                const std::string jobName = GetString(job, "name");
                const std::string state = GetString(job, "state");
                if (GetString(job, "taskqueue") != queueName || name.empty() || jobName.rfind(name, 0) != 0 ||
                    state == "succeeded" || state == "failed" || state == "cancelled") continue;
                const std::string response = tqm_.TaskQueueUpdate(GetString(job, "id"),
                    json{{"estimate_sec", actual}}.dump(), TaskQueueCaller::External, actionError);
                if (!actionError.empty() || !ResponseOk(response, actionError)) break;
                ++updatedCount;
            }
        }
        if (!actionError.empty()) return Error(actionError).dump();
        result = updatedCount ? "Recalibrated " + std::to_string(updatedCount) + " unfinished job(s)."
                              : "No unfinished matching jobs; recorded the measured duration.";
    } else if (candidateId == "bt_retry") {
        if (!btm_) return Error("behavior tree manager is unavailable").dump();
        const std::string response = btm_->Retry(GetString(target, "btId"), GetString(target, "nodeId"), false);
        if (!ResponseOk(response, actionError)) return Error(actionError).dump();
        result = "Retried behavior tree " + GetString(target, "btId") + ".";
    } else if (candidateId == "bt_cancel") {
        if (!btm_) return Error("behavior tree manager is unavailable").dump();
        const std::string response = btm_->Cancel(GetString(target, "btId"));
        if (!ResponseOk(response, actionError)) return Error(actionError).dump();
        result = "Cancelled behavior tree " + GetString(target, "btId") + ".";
    } else if (candidateId == "bt_resume") {
        if (!btm_) return Error("behavior tree manager is unavailable").dump();
        const std::string answer = GetString(params, "answer");
        if (answer.empty()) return Error("bt_resume requires params.answer").dump();
        const std::string response = btm_->Resume(GetString(target, "btId"), GetString(target, "nodeId"), answer, reason);
        if (!ResponseOk(response, actionError)) return Error(actionError).dump();
        result = "Resumed behavior tree " + GetString(target, "btId") + ".";
    } else if (candidateId == "restart") {
        StartProcessRequest request;
        request.name = GetString(target, "name");
        request.commandLine = GetString(target, "command");
        request.workingDir = GetString(target, "cwd");
        request.mode = StringToExecMode(GetString(target, "mode", "pipe"));
        request.timeoutSec = static_cast<int>(GetNumber(target, "timeoutSec", 0));
        request.idleTimeoutSec = static_cast<int>(GetNumber(target, "idleTimeoutSec", 0));
        const int processId = pm_.StartProcess(request, actionError);
        if (processId <= 0) return Error(actionError.empty() ? "failed to restart process" : actionError).dump();
        result = "Restarted process as #" + std::to_string(processId) + ".";
    } else if (candidateId == "ignore") {
        result = "Ignored: " + reason;
    } else if (candidateId == "ack") {
        result = "Acknowledged: " + reason;
    } else if (candidateId == "investigate") {
        result = reason.empty() ? "Recorded investigation request." : "Recorded investigation request: " + reason;
    } else if (candidateId == "wait") {
        result = "Recorded; the resource scheduler will continue waiting.";
    } else if (candidateId == "show_holder" || candidateId == "fix_spec") {
        result = "Recorded " + candidateId + " for the dashboard to handle.";
    } else {
        return Error("unsupported candidate: " + candidateId).dump();
    }

    const std::string jobId = CardTargetJob(card);
    if (!jobId.empty()) {
        TaskQueueJob jobSnapshot;
        if (tqm_.GetJobSnapshot(jobId, jobSnapshot)) {
            const std::string line = "[triage] " + by + " chose " + candidateId + ": " + reason;
            const std::string notes = jobSnapshot.notes.empty() ? line : jobSnapshot.notes + "\n" + line;
            const std::string noteResponse = tqm_.TaskQueueUpdate(jobId, json{{"notes", notes}}.dump(),
                                                                  TaskQueueCaller::External, actionError);
            if (!actionError.empty() || !ResponseOk(noteResponse, actionError)) result += " Notes update failed: " + actionError;
        }
    }

    json updated;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = std::find_if(store_["cards"].begin(), store_["cards"].end(), [&](const json& item) {
            return GetString(item, "id") == id;
        });
        if (found == store_["cards"].end()) return Error("triage card disappeared while deciding").dump();
        const std::string state = candidateId == "ignore" ? "ignored" : "acted";
        (*found)["state"] = state;
        (*found)["updatedAt"] = GetCurrentTimestampStr();
        (*found)["decision"] = {{"candidateId", candidateId}, {"by", by}, {"reason", reason},
                                  {"at", GetCurrentTimestampStr()}, {"result", result}, {"ok", true}};
        json manual = GetObject(*found, "manual");
        if (by == "human" && !manual.empty()) {
            (*found)["decision"]["route"] = "manual";
            manual["state"] = "done";
            manual["endedAt"] = GetCurrentTimestampStr();
            (*found)["manual"] = manual;
        }
        if (by == "hancho-auto" || by == "llm-auto") (*found)["decision"]["reviewed"] = false;
        if (by != "human" && (candidateId == "retry" || candidateId == "retry_raise_timeout")) {
            const std::string retry = CardTargetJob(*found) + "|" + FailureClassFor(*found);
            if (std::find(store_["autoRetries"].begin(), store_["autoRetries"].end(), json(retry)) == store_["autoRetries"].end())
                store_["autoRetries"].push_back(retry);
        }
        updated = *found;
        SaveLocked();
    }
    return json{{"ok", true}, {"card", std::move(updated)}, {"result", result}}.dump();
}

void TriageManager::UpdateOutcomesLocked(const json& jobs, const json& btStates) {
    const std::string now = GetCurrentTimestampStr();
    std::map<std::string, std::string> cardIdByKey;
    std::map<std::string, json> oldOutcomes;
    for (const auto& card : store_["cards"])
        if (!GetString(card, "key").empty()) cardIdByKey[GetString(card, "key")] = GetString(card, "id");
    for (const auto& card : store_["cards"])
        oldOutcomes[GetString(card, "id")] = GetObject(card, "outcome");

    std::map<std::string, std::string> previousByKey;
    for (auto& card : store_["cards"]) {
        if (!TracksOutcome(card)) continue;
        json outcome = TriageOutcome(card, jobs, btStates);
        if (!outcome.is_object()) {
            card.erase("outcome");
            continue;
        }
        if (outcome.contains("nextKey")) {
            const std::string nextKey = GetString(outcome, "nextKey");
            outcome["nextCardId"] = TriageCardIdFromKey(nextKey);
            previousByKey[nextKey] = GetString(card, "id");
        }
        json branch = GetObject(outcome, "branch");
        if (branch.contains("jobs") && branch["jobs"].is_array()) {
            for (auto& item : branch["jobs"]) {
                if (GetString(item, "state") != "failed") continue;
                const std::string key = GetString(item, "key");
                const auto cardId = cardIdByKey.find(key);
                if (cardId != cardIdByKey.end()) item["cardId"] = cardId->second;
            }
            outcome["branch"] = branch;
        }
        card["outcome"] = std::move(outcome);
    }

    // Closure depends on other cards (failed branches and retry successors), so recompute until stable.
    const size_t maxPasses = store_["cards"].size() + 1;
    for (size_t pass = 0; pass < maxPasses; ++pass) {
        bool changed = false;
        for (auto& card : store_["cards"]) {
            if (!TracksOutcome(card)) continue;
            json outcome = GetObject(card, "outcome");
            if (outcome.empty()) continue;
            const std::string outcomeState = GetString(outcome, "state");
            bool nextClosed = true;
            if (outcomeState == "failed_again") {
                const std::string nextKey = GetString(outcome, "nextKey");
                const auto next = cardIdByKey.find(nextKey);
                outcome["nextCardId"] = TriageCardIdFromKey(nextKey);
                nextClosed = next != cardIdByKey.end();
                if (nextClosed) {
                    auto nextCard = std::find_if(store_["cards"].begin(), store_["cards"].end(), [&](const json& candidate) {
                        return GetString(candidate, "id") == next->second;
                    });
                    nextClosed = nextCard != store_["cards"].end() && IsClosedCard(*nextCard);
                }
            }
            json branch = GetObject(outcome, "branch");
            bool branchClosed = true;
            if (branch.contains("jobs") && branch["jobs"].is_array()) {
                for (auto& item : branch["jobs"]) {
                    const std::string state = GetString(item, "state");
                    if (state == "failed") {
                        const std::string key = GetString(item, "key");
                        const auto cardId = cardIdByKey.find(key);
                        if (cardId == cardIdByKey.end()) { branchClosed = false; continue; }
                        item["cardId"] = cardId->second;
                        auto failedCard = std::find_if(store_["cards"].begin(), store_["cards"].end(), [&](const json& candidate) {
                            return GetString(candidate, "id") == cardId->second;
                        });
                        if (failedCard == store_["cards"].end() || !IsClosedCard(*failedCard)) branchClosed = false;
                    } else if (state != "succeeded" && state != "cancelled") {
                        branchClosed = false;
                    }
                }
                branch["done"] = branchClosed;
                outcome["branch"] = branch;
            }
            const bool rootTerminal = outcomeState == "succeeded" || outcomeState == "cancelled" ||
                                      outcomeState == "gone";
            const bool complete = (rootTerminal || (outcomeState == "failed_again" && nextClosed)) && branchClosed;
            outcome["complete"] = complete;
            if (card.value("outcome", json::object()) != outcome) {
                card["outcome"] = outcome;
                changed = true;
            }
        }
        if (!changed) break;
    }

    for (auto& card : store_["cards"]) {
        const std::string id = GetString(card, "id");
        const auto previous = previousByKey.find(GetString(card, "key"));
        if (previous != previousByKey.end()) card["previousCardId"] = previous->second;

        json manual = GetObject(card, "manual");
        if (!manual.empty() && GetString(manual, "state") == "working" &&
            GetString(card, "state") != "pending") {
            manual["state"] = "done";
            manual["endedAt"] = now;
            card["manual"] = manual;
        }

        const std::string state = GetString(card, "state");
        const json outcome = GetObject(card, "outcome");
        const std::string outcomeState = GetString(outcome, "state");
        std::string lifecycle;
        if (IsClosedCard(card)) lifecycle = "done";
        else if (state == "pending") {
            const std::string handoffState = GetString(GetObject(card, "handoff"), "state");
            lifecycle = (handoffState == "queued" || handoffState == "running" ||
                         GetString(manual, "state") == "working") ? "handling" : "pending";
        } else if (outcomeState == "rerunning") lifecycle = "running";
        else if (outcomeState == "failed_again") {
            const std::string nextId = GetString(outcome, "nextCardId");
            auto next = std::find_if(store_["cards"].begin(), store_["cards"].end(), [&](const json& candidate) {
                return !nextId.empty() && GetString(candidate, "id") == nextId;
            });
            lifecycle = next == store_["cards"].end() || !IsClosedCard(*next) ? "waiting_next" : "waiting_branch";
        } else if (outcomeState == "not_rerun") lifecycle = "follow_up";
        else if (outcome.is_object() && outcome.contains("complete") && !outcome["complete"].get<bool>())
            lifecycle = "waiting_branch";
        else lifecycle = "pending";
        if (GetString(card, "lifecycle") != lifecycle) {
            card["lifecycle"] = lifecycle;
            card["updatedAt"] = now;
        }
        if (outcome.is_object()) {
            json current = outcome;
            current.erase("at");
            json prior = oldOutcomes[id];
            prior.erase("at");
            if (current != prior) {
                card["outcome"]["at"] = now;
                card["updatedAt"] = now;
            } else if (oldOutcomes[id].contains("at")) {
                card["outcome"]["at"] = oldOutcomes[id]["at"];
            }
        }
    }
}

std::string TriageManager::MarkManual(const std::string& id) {
    json card;
    std::string language;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto found = std::find_if(store_["cards"].begin(), store_["cards"].end(), [&](const json& item) {
            return GetString(item, "id") == id;
        });
        if (found == store_["cards"].end()) return Error("triage card not found: " + id).dump();
        if (GetString(*found, "state") != "pending") return Error("only a pending card can be taken over by hand").dump();
        (*found)["manual"] = {{"state", "working"}, {"at", GetCurrentTimestampStr()}};
        (*found)["lifecycle"] = "handling";
        (*found)["updatedAt"] = GetCurrentTimestampStr();
        card = *found;
        language = language_;
        SaveLocked();
    }
    const std::string jobId = CardTargetJob(card);
    TaskQueueJob snapshot;
    if (!jobId.empty() && tqm_.GetJobSnapshot(jobId, snapshot)) {
        const std::string line = "[triage] human took card " + id + " over by hand (prompt copied)";
        std::string noteError;
        tqm_.TaskQueueUpdate(jobId, json{{"notes", snapshot.notes.empty() ? line : snapshot.notes + "\n" + line}}.dump(),
                             TaskQueueCaller::External, noteError);
    }
    std::wstring cliPath = L"hancho-cli.exe";
#ifdef _WIN32
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) > 0) {
        std::wstring dir = self;
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) cliPath = dir.substr(0, slash + 1) + L"hancho-cli.exe";
    }
#endif
    const std::string cli = "\"" + WideToUtf8(cliPath) + "\" --data-dir \"" +
                            WideToUtf8(GetEffectiveDataDirectoryWide()) + "\"";
    const std::string prompt = BuildTriageHandoffInstructions(json::array({card}), cli, language, true);
    return json{{"ok", true}, {"card", card}, {"prompt", prompt}}.dump();
}

void TriageManager::UpdateHandoffsLocked(const json& supervisorJobs) {
    for (auto& card : store_["cards"]) {
        json handoff = GetObject(card, "handoff");
        const std::string jobId = GetString(handoff, "jobId");
        const std::string old = GetString(handoff, "state");
        if (jobId.empty() || (old != "running" && old != "queued")) continue;
        std::string state;
        for (const auto& job : supervisorJobs) {
            if (GetString(job, "id") == jobId) { state = GetString(job, "state"); break; }
        }
        std::string next = old;
        if (state == "running") next = "running";
        else if (state == "succeeded") next = GetString(card, "state") == "pending" ? "done_undecided" : "done";
        else if (state == "failed" || state == "cancelled" || state == "blocked") next = "failed";
        else if (state.empty()) next = "failed";  // the job is gone (purged)
        if (next != old) {
            handoff["state"] = next;
            if (next != "running") handoff["endedAt"] = GetCurrentTimestampStr();
            if (next != "running") {
                // The supervisor's per-card note (what it did, or why it left the card pending).
                json result = json::parse(ReadFileUtf8Wide(AdviceResultPath(jobId)), nullptr, false);
                if (result.is_object() && result.contains("cards") && result["cards"].is_array()) {
                    for (const auto& entry : result["cards"]) {
                        if (GetString(entry, "id") != GetString(card, "id")) continue;
                        handoff["outcome"] = GetString(entry, "outcome");
                        handoff["note"] = GetString(entry, "note");
                    }
                }
            }
            card["handoff"] = handoff;
            card["updatedAt"] = GetCurrentTimestampStr();
        }
    }
}

std::string TriageManager::Handoff(const std::vector<std::string>& ids, bool allPending) {
    json cards = json::array();
    std::string engineName, modelName, language;
    int timeoutSec = 1800;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& card : store_["cards"]) {
            if (GetString(card, "state") != "pending") continue;
            const std::string state = GetString(GetObject(card, "handoff"), "state");
            if (state == "running" || state == "queued") continue;  // already with a supervisor
            const std::string id = GetString(card, "id");
            if (allPending || std::find(ids.begin(), ids.end(), id) != ids.end()) cards.push_back(card);
        }
        engineName = supervisorEngine_.empty() ? (btm_ ? btm_->DefaultEngine() : std::string("claude"))
                                               : supervisorEngine_;
        modelName = supervisorModel_;
        language = language_;
        timeoutSec = supervisorTimeoutSec_;
    }
    if (cards.empty()) return Error("no pending card to hand off (or it is already with a supervisor)").dump();

    // The worker must talk to this core, so give it this hancho-cli and data dir explicitly.
    std::wstring cliPath = L"hancho-cli.exe";
#ifdef _WIN32
    wchar_t self[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, self, MAX_PATH) > 0) {
        std::wstring dir = self;
        const size_t slash = dir.find_last_of(L"\\/");
        if (slash != std::wstring::npos) cliPath = dir.substr(0, slash + 1) + L"hancho-cli.exe";
    }
#endif
    const std::string cli = "\"" + WideToUtf8(cliPath) + "\" --data-dir \"" +
                            WideToUtf8(GetEffectiveDataDirectoryWide()) + "\"";

    const int sequence = [&] {
        std::lock_guard<std::mutex> lock(mutex_);
        const int next = static_cast<int>(GetNumber(store_, "nextHandoff", 0)) + 1;
        store_["nextHandoff"] = next;
        return next;
    }();
    const std::string jobId = "supervisor-triage-" + std::to_string(sequence);
    const std::string instruction = BuildTriageHandoffInstructions(cards, cli, language);
    const std::wstring instructionDir = PathJoinWide(TriageDirectory(), L"instructions");
    EnsureDirectoryWide(instructionDir);
    const std::wstring instructionPath = PathJoinWide(instructionDir, Utf8ToWide(jobId) + L".md");
    WriteFileAtomicWide(instructionPath, instruction);
    const std::string launchPrompt = "Read the instructions file " + WideToUtf8(instructionPath) +
                                     " and carry them out. You run non-interactively; finish and exit.";

    const WorkerEngineRegistry* engines = btm_ ? &btm_->Engines() : nullptr;
    WorkerEngineRegistry localEngines;
    if (!engines) engines = &localEngines;
    std::string command, streamType, commandError, adapter;
    if (!engines->BuildCommand(engineName, modelName, launchPrompt, timeoutSec, command, streamType,
                               commandError, &adapter)) {
        return Error("cannot build the supervisor command for engine '" + engineName + "': " + commandError).dump();
    }

    // Run where the work is: the job's own cwd when every card shares one.
    std::string cwd;
    bool sameCwd = true;
    for (const auto& card : cards) {
        std::string jobCwd;
        TaskQueueJob snapshot;
        const std::string targetJob = CardTargetJob(card);
        if (!targetJob.empty() && tqm_.GetJobSnapshot(targetJob, snapshot)) jobCwd = snapshot.cwd;
        if (cwd.empty() && sameCwd) cwd = jobCwd;
        else if (jobCwd != cwd) sameCwd = false;
    }
    if (!sameCwd || cwd.empty()) cwd = WideToUtf8(GetEffectiveDataDirectoryWide());

    EnsureDirectoryWide(PathJoinWide(TriageDirectory(), L"results"));
    RemoveFileWide(AdviceResultPath(jobId));
    json env = {{"PC_TRIAGE_RESULT_FILE", WideToUtf8(AdviceResultPath(jobId))}};
    if (!adapter.empty()) env["HANCHO_ADAPTER"] = adapter;
    std::string summary = "Supervisor triage hand-off: " + std::to_string(cards.size()) + " card(s)";
    json job = {{"id", jobId}, {"name", "supervisor-triage"}, {"summary", summary},
                {"command", command}, {"cwd", cwd}, {"instructions_text", instruction},
                {"timeout_sec", timeoutSec}, {"max_attempts", 1}, {"hidden", true},
                {"idle", {{"output_sec", 600}, {"cpu_sec", 600}}},
                {"agent_type", engineName}, {"env", env}};
    std::string err;
    const std::string addResponse = tqm_.TaskQueueAdd("_supervisor", json::array({job}).dump(), 1, "", err);
    std::string addError;
    if (!err.empty() || !ResponseOk(addResponse, addError)) return Error(err.empty() ? addError : err).dump();

    // Mark every handed-off card so nobody mistakes "with the supervisor" for "not looked at".
    const std::string now = GetCurrentTimestampStr();
    json handedIds = json::array();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& card : store_["cards"]) {
            const std::string id = GetString(card, "id");
            bool selected = false;
            for (const auto& c : cards) if (GetString(c, "id") == id) { selected = true; break; }
            if (!selected) continue;
            card["handoff"] = {{"jobId", jobId}, {"at", now}, {"state", "queued"}, {"engine", engineName}};
            card["updatedAt"] = now;
            handedIds.push_back(id);
        }
        SaveLocked();
    }
    for (const auto& card : cards) {
        const std::string targetJob = CardTargetJob(card);
        TaskQueueJob snapshot;
        if (targetJob.empty() || !tqm_.GetJobSnapshot(targetJob, snapshot)) continue;
        const std::string line = "[triage] handed to supervisor job " + jobId;
        std::string noteError;
        tqm_.TaskQueueUpdate(targetJob, json{{"notes", snapshot.notes.empty() ? line : snapshot.notes + "\n" + line}}.dump(),
                             TaskQueueCaller::External, noteError);
    }
    return json{{"ok", true}, {"jobId", jobId}, {"cards", handedIds}}.dump();
}

} // namespace pc
