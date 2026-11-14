#include "triage.h"

#include "common.h"
#include "worker_engines.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <vector>

namespace pc {
namespace {

using json = nlohmann::json;

std::string String(const json& value, const char* key, const std::string& fallback = "") {
    const auto it = value.find(key);
    return it != value.end() && it->is_string() ? it->get<std::string>() : fallback;
}

bool Bool(const json& value, const char* key, bool fallback = false) {
    const auto it = value.find(key);
    return it != value.end() && it->is_boolean() ? it->get<bool>() : fallback;
}

long long Number(const json& value, const char* key, long long fallback = 0) {
    const auto it = value.find(key);
    if (it == value.end() || !it->is_number()) return fallback;
    try { return it->get<long long>(); } catch (...) { return fallback; }
}

json Object(const json& value, const char* key) {
    const auto it = value.find(key);
    return it != value.end() && it->is_object() ? *it : json::object();
}

json Array(const json& value, const char* key) {
    const auto it = value.find(key);
    return it != value.end() && it->is_array() ? *it : json::array();
}

bool ParseTime(const std::string& value, std::time_t& out);

bool HasValue(const json& value) {
    if (value.is_null()) return false;
    if (value.is_string()) return !value.get<std::string>().empty();
    if (value.is_array() || value.is_object()) return !value.empty();
    return true;
}

void AddItem(json& section, const char* key, const json& value, const std::string& text) {
    if (!HasValue(value)) return;
    section.push_back({{"k", key}, {"v", value}, {"text", text}});
}

std::string Clip(std::string value, size_t maxChars = 300) {
    size_t offset = 0;
    size_t chars = 0;
    while (offset < value.size() && chars < maxChars) {
        const unsigned char c = static_cast<unsigned char>(value[offset]);
        offset += c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
        ++chars;
    }
    if (offset < value.size()) value.resize(offset);
    return value;
}

std::string TrimWhitespace(std::string value) {
    const size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    const size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> ReadLines(const std::string& path) {
    std::vector<std::string> lines;
    if (path.empty()) return lines;
    std::ifstream input(Utf8ToWide(path), std::ios::binary);
    if (!input) return lines;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

json EvidenceAt(const std::string& path, long long lineNumber) {
    if (path.empty() || lineNumber <= 0) return json();
    const auto lines = ReadLines(path);
    if (static_cast<size_t>(lineNumber) > lines.size()) return json();
    const size_t match = static_cast<size_t>(lineNumber - 1);
    const size_t first = match > 3 ? match - 3 : 0;
    const size_t last = std::min(lines.size(), match + 4);
    json context = json::array();
    for (size_t i = first; i < last; ++i) context.push_back(Clip(lines[i]));
    return {{"file", path}, {"line", lineNumber}, {"startLine", first + 1}, {"lines", context}};
}

json TailOutput(const json& attempt) {
    std::string path = String(attempt, "log_file");
    if (path.empty()) path = String(attempt, "logFile");
    auto lines = ReadLines(path);
    if (lines.empty()) {
        path = String(attempt, "last_lines_file");
        if (path.empty()) path = String(attempt, "lastLinesFile");
        lines = ReadLines(path);
    }
    if (lines.empty()) return json();
    json output = json::array();
    const size_t first = lines.size() > 8 ? lines.size() - 8 : 0;
    for (size_t i = first; i < lines.size(); ++i) output.push_back(Clip(lines[i]));
    return {{"file", path}, {"lines", output}};
}

bool AttemptRange(const json& attempt, std::time_t& start, std::time_t& end) {
    return ParseTime(String(attempt, "start"), start) && ParseTime(String(attempt, "end"), end) && end >= start;
}

json ConcurrencyAtFailure(const json& jobs, const std::string& jobId, const json& attempt) {
    std::time_t failedStart = 0, failedEnd = 0;
    if (!AttemptRange(attempt, failedStart, failedEnd)) return json();
    std::vector<std::string> overlapping;
    std::unordered_set<std::string> seen;
    for (const auto& other : jobs) {
        const std::string id = String(other, "id");
        if (!other.is_object() || id.empty() || id == jobId || !seen.insert(id).second) continue;
        for (const auto& otherAttempt : Array(other, "attempts")) {
            std::time_t start = 0, end = 0;
            if (AttemptRange(otherAttempt, start, end) && start <= failedEnd && end >= failedStart) {
                overlapping.push_back(id);
                break;
            }
        }
    }
    json ids = json::array();
    for (size_t i = 0; i < std::min<size_t>(5, overlapping.size()); ++i) ids.push_back(overlapping[i]);
    return {{"count", overlapping.size()}, {"ids", ids}};
}

long long DurationSeconds(const json& attempt) {
    std::time_t start = 0, end = 0;
    return AttemptRange(attempt, start, end) ? static_cast<long long>(end - start) : -1;
}

std::string InfoText(const json& item) {
    if (item.is_string()) return item.get<std::string>();
    return item.is_object() && item.contains("text") && item["text"].is_string()
        ? item["text"].get<std::string>() : std::string();
}

json InfoValue(const json& item) {
    return item.is_object() && item.contains("v") ? item["v"] : json();
}

std::string Lower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return value;
}

bool ParseTime(const std::string& value, std::time_t& out) {
    std::tm tm{};
    std::istringstream input(value);
    input >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (input.fail()) return false;
    tm.tm_isdst = -1;
    out = std::mktime(&tm);
    return out != static_cast<std::time_t>(-1);
}

std::string FailureClass(const json& failure) {
    return Lower(String(failure, "class"));
}

json LastAttempt(const json& job) {
    const json attempts = Array(job, "attempts");
    return attempts.empty() ? json::object() : attempts.back();
}

json FailureFor(const json& job) {
    json failure = Object(job, "failure");
    if (!failure.empty()) return failure;
    return Object(LastAttempt(job), "failure");
}

std::string AttemptReason(const json& attempt) {
    return Lower(String(attempt, "reason"));
}

bool TimedOut(const json& attempt) {
    return AttemptReason(attempt) == "timed_out" ||
           FailureClass(Object(attempt, "failure")) == "timeout";
}

std::string Confidence(const json& failure) {
    std::string value = Lower(String(failure, "confidence", "low"));
    return value == "high" || value == "medium" || value == "low" ? value : "low";
}

json Candidate(const char* id, const char* label, const char* effect, bool safe,
               json params = json::object()) {
    return {{"id", id}, {"label", label}, {"effect", effect}, {"safe", safe},
            {"params", std::move(params)}};
}

void AddCandidate(json& candidates, const char* id, const char* label,
                  const char* effect, bool safe, json params = json::object()) {
    candidates.push_back(Candidate(id, label, effect, safe, std::move(params)));
}

std::string PreviousFailureClass(const json& job) {
    const json attempts = Array(job, "attempts");
    if (attempts.size() < 2) return "";
    return FailureClass(Object(attempts[attempts.size() - 2], "failure"));
}

json BuildJobFailureCard(const json& job, const TriageInputs& in) {
    const json attempts = Array(job, "attempts");
    const json last = LastAttempt(job);
    const json failure = FailureFor(job);
    const std::string jobId = String(job, "id");
    const std::string failureClass = FailureClass(failure);
    const std::string attemptReason = AttemptReason(last);
    const std::string attemptCount = attempts.empty()
        ? std::to_string(Number(job, "attempt_count", 0))
        : std::to_string(Number(last, "n", static_cast<long long>(attempts.size())));
    const std::string key = "job:" + jobId + "#" + attemptCount;
    const long long timeoutSec = Number(job, "timeout_sec", 0);
    const long long estimateSec = Number(job, "estimate_sec", 0);
    const json idle = Object(job, "idle");
    const json resources = Object(job, "resources");
    const json conditions = Object(job, "success");
    const json activity = Object(failure, "lastActivity");

    json info = {{"cause", json::array()}, {"background", json::array()},
                 {"environment", json::array()}, {"conditions", json::array()},
                 {"reason", json::array()}};
    const std::string failureText = failureClass.empty() ? "unknown" : failureClass;
    AddItem(info["cause"], "failureClass", {{"class", failureText}, {"confidence", Confidence(failure)}},
            "failure class: " + failureText + " (" + Confidence(failure) + " confidence)");
    const std::string summary = String(failure, "summary");
    if (!summary.empty()) AddItem(info["cause"], "summary", {{"class", failureText}, {"summary", summary}}, summary);
    const int actualExit = static_cast<int>(Number(last, "exit_code", -1));
    const int expectedExit = static_cast<int>(Number(conditions, "exit_code", 0));
    if (last.contains("exit_code") && last["exit_code"].is_number() && actualExit != expectedExit)
        AddItem(info["cause"], "exitCode", {{"actual", actualExit}, {"expected", expectedExit}},
                "exit code " + std::to_string(actualExit) + " (expected " + std::to_string(expectedExit) + ")");
    const std::string checkDetail = String(last, "check_detail", String(failure, "check_detail"));
    AddItem(info["cause"], "checkDetail", checkDetail, "success check failed: " + checkDetail);
    const std::string pattern = TriageTimeoutPattern(job);
    if (!pattern.empty()) {
        const std::string patternText = pattern == "progressing" ? "timeout pattern: progressing near timeout" :
            pattern == "stalled" ? "timeout pattern: stalled" :
            pattern == "repeated" ? "timeout pattern: repeated timeout" : "timeout pattern: unknown";
        AddItem(info["cause"], "timeoutPattern", pattern, patternText);
    }
    const json evidence = Array(failure, "evidence");
    int evidenceCount = 0;
    for (const auto& item : evidence) {
        if (evidenceCount >= 3) break;
        const std::string log = String(item, "log");
        const long long atLine = Number(item, "line", 0);
        if (!log.empty() && atLine > 0) {
            json context = EvidenceAt(log, atLine);
            if (!context.is_null()) {
                AddItem(info["cause"], "evidence", context,
                        "evidence " + log + ":" + std::to_string(atLine) + " — " +
                        context["lines"].dump());
                ++evidenceCount;
            }
        } else if (item.contains("seq") && item["seq"].is_number()) {
            AddItem(info["cause"], "eventSequence", Number(item, "seq", 0),
                    "event sequence: " + std::to_string(Number(item, "seq", 0)));
        }
    }
    json lastOutput = TailOutput(last);
    if (!lastOutput.is_null())
        AddItem(info["cause"], "lastOutput", lastOutput, "last output lines: " + lastOutput["lines"].dump());
    if (failure.contains("llmGuess") && HasValue(failure["llmGuess"]))
        AddItem(info["cause"], "llmGuess", failure["llmGuess"],
                "task queue LLM guess (reference only): " + failure["llmGuess"].dump());
    if (!activity.empty()) {
        json value = activity;
        std::string text = "last activity: " + String(activity, "activity");
        if (!String(activity, "target").empty()) text += " (" + String(activity, "target") + ")";
        const long long since = Number(activity, "sinceSec", -1);
        if (since >= 0) text += ", " + std::to_string(since) + "s ago";
        AddItem(info["cause"], "lastActivity", value, text);
    }

    const std::string name = String(job, "name", jobId);
    const std::string jobSummary = String(job, "summary");
    AddItem(info["background"], "jobName", name, "name: " + name);
    AddItem(info["background"], "jobSummary", jobSummary, jobSummary);
    AddItem(info["background"], "command", String(job, "command"), "command: " + String(job, "command"));
    AddItem(info["background"], "queue", String(job, "taskqueue"), "queue: " + String(job, "taskqueue"));
    const json bt = Object(job, "bt");
    if (!String(bt, "id").empty()) {
        json btValue = {{"id", String(bt, "id")}};
        if (!String(bt, "node").empty()) btValue["node"] = String(bt, "node");
        AddItem(info["background"], "behaviorTree", btValue,
                "BT " + String(bt, "id") + (String(bt, "node").empty() ? "" : " / node " + String(bt, "node")));
    }
    json attemptInfo = json::object();
    const long long currentAttempt = Number(last, "n", Number(job, "attempt_count", 0));
    if (currentAttempt > 0) attemptInfo["n"] = currentAttempt;
    if (Number(job, "max_attempts") > 0) attemptInfo["maxAttempts"] = Number(job, "max_attempts");
    AddItem(info["background"], "attempt", attemptInfo,
            "attempt " + attemptCount + "/" + std::to_string(Number(job, "max_attempts", 0)));
    json origin = Object(job, "origin");
    const std::string originLabel = String(origin, "label");
    if (!originLabel.empty()) {
        AddItem(info["background"], "origin", {{"label", originLabel}}, "origin: " + originLabel);
    } else {
        const std::string client = String(origin, "client");
        const std::string folder = String(origin, "folder", String(origin, "cwd"));
        if (!client.empty() || !folder.empty()) {
            json originValue = json::object();
            if (!client.empty()) originValue["client"] = client;
            if (!folder.empty()) originValue["folder"] = folder;
            AddItem(info["background"], "origin", originValue,
                    "origin: " + (client.empty() ? "" : client) +
                    (folder.empty() ? "" : (client.empty() ? "" : " + ") + folder));
        }
    }
    json attemptHistory = json::array();
    for (const auto& attempt : attempts) {
        json value = json::object();
        const long long n = Number(attempt, "n");
        if (n > 0) value["n"] = n;
        const std::string attemptStart = String(attempt, "start");
        if (!attemptStart.empty()) value["start"] = attemptStart;
        const long long duration = DurationSeconds(attempt);
        if (duration >= 0) value["durationSec"] = duration;
        const std::string reason = String(attempt, "reason");
        if (!reason.empty()) value["reason"] = reason;
        const std::string attemptClass = FailureClass(Object(attempt, "failure"));
        if (!attemptClass.empty()) value["class"] = attemptClass;
        attemptHistory.push_back(std::move(value));
    }
    AddItem(info["background"], "attemptHistory", attemptHistory,
            "attempt history: " + attemptHistory.dump());
    json dependents = json::array();
    for (const auto& other : in.jobs) {
        if (!other.is_object() || String(other, "blocked_by") != jobId) continue;
        dependents.push_back(String(other, "id"));
    }
    if (!dependents.empty())
        AddItem(info["background"], "blockedDependents", {{"count", dependents.size()}, {"ids", dependents}},
                "dependent blocked: " + dependents.dump());
    const std::string host = String(job, "host");
    if (!host.empty()) AddItem(info["environment"], "host", host, "host: " + host);
    else AddItem(info["environment"], "host", "local", "host: local");
    AddItem(info["environment"], "cwd", String(job, "cwd"), "cwd: " + String(job, "cwd"));
    for (const char* access : {"write", "read"}) {
        json lockNames = Array(resources, access);
        if (!lockNames.empty())
            AddItem(info["environment"], access, lockNames,
                    std::string("resources ") + access + ": " + lockNames.dump());
    }
    const std::string resourceWait = String(job, "resource_wait");
    if (!resourceWait.empty() && resourceWait != "null") {
        std::string holderId = resourceWait.substr(0, resourceWait.find(':'));
        const size_t space = holderId.find(" (queued ahead)");
        if (space != std::string::npos) holderId.resize(space);
        json holder = json::object();
        for (const auto& other : in.jobs) if (String(other, "id") == holderId) { holder = other; break; }
        std::string lockMode = "unknown";
        const size_t colon = resourceWait.find(':');
        const std::string resourceName = TrimWhitespace(colon == std::string::npos ? resourceWait : resourceWait.substr(colon + 1));
        for (const char* access : {"write", "read"}) {
            for (const auto& lock : Array(resources, access))
                if ((lock.is_string() && lock.get<std::string>() == resourceName) ||
                    (lock.is_object() && String(lock, "name") == resourceName)) lockMode = access;
        }
        json value = {{"resource", resourceWait}, {"holderId", holderId}, {"lockMode", lockMode}};
        const std::string holderState = String(holder, "state");
        if (!holderState.empty()) value["holderState"] = holderState;
        const json holderAttempts = Array(holder, "attempts");
        if (!holderAttempts.empty()) {
            std::time_t holderStart = 0;
            if (ParseTime(String(holderAttempts.back(), "start"), holderStart))
                value["holderRunningSec"] = std::max<long long>(0, static_cast<long long>(std::time(nullptr) - holderStart));
        }
        AddItem(info["environment"], "resourceHolder", value,
                "resource wait: " + resourceWait + " (" + holderId + ", " + lockMode + " lock" +
                (holderState.empty() ? "" : ", " + holderState) + ")");
        std::time_t queued = 0, now = 0;
        if (ParseTime(String(job, "created_at"), queued) && ParseTime(in.now, now))
            AddItem(info["conditions"], "waitingSince", {{"at", String(job, "created_at")},
                    {"elapsedSec", std::max<long long>(0, static_cast<long long>(now - queued))}},
                    "waiting since: " + String(job, "created_at"));
    }

    if (timeoutSec > 0) AddItem(info["conditions"], "timeoutSec", timeoutSec, "timeoutSec: " + std::to_string(timeoutSec));
    const std::string startText = String(last, "start");
    const std::string endText = String(last, "end");
    long long elapsed = -1;
    elapsed = DurationSeconds(last);
    if (elapsed >= 0) AddItem(info["conditions"], "attemptDurationSec", elapsed,
                              "last attempt elapsedSec: " + std::to_string(elapsed));
    if (estimateSec > 0) AddItem(info["conditions"], "estimateSec", estimateSec,
                                 "estimateSec: " + std::to_string(estimateSec));
    if (estimateSec > 0 && elapsed >= 0) {
        std::ostringstream ratio;
        ratio << std::fixed << std::setprecision(2) << static_cast<double>(elapsed) / estimateSec;
        AddItem(info["conditions"], "elapsedEstimateRatio", static_cast<double>(elapsed) / estimateSec,
                "elapsed/estimate ratio: " + ratio.str());
    }
    if (Number(idle, "output_sec") > 0 || Number(idle, "cpu_sec") > 0)
        AddItem(info["conditions"], "idle", idle, "idle monitoring: " + idle.dump());
    if (expectedExit != 0) AddItem(info["conditions"], "successExitCode", expectedExit,
                                    "success exit code: " + std::to_string(expectedExit));
    const json filesNewer = Array(conditions, "files_newer");
    AddItem(info["conditions"], "successFilesNewer", filesNewer, "success filesNewer: " + filesNewer.dump());
    AddItem(info["conditions"], "successOutputRegex", String(conditions, "output_regex"),
            "success outputRegex: " + String(conditions, "output_regex"));
    AddItem(info["conditions"], "successTestReport", String(conditions, "test_report"),
            "success testReport: " + String(conditions, "test_report"));
    const long long maxAttempts = Number(job, "max_attempts");
    if (maxAttempts > 0) AddItem(info["conditions"], "maxAttempts", maxAttempts,
                                 "max attempts: " + std::to_string(maxAttempts));
    const long long retryDelay = Number(job, "retry_delay_sec");
    if (retryDelay > 0) AddItem(info["conditions"], "retryDelaySec", retryDelay,
                                 "retry delay: " + std::to_string(retryDelay) + "s");
    const json concurrency = ConcurrencyAtFailure(in.jobs, jobId, last);
    AddItem(info["environment"], "concurrencyAtFailure", concurrency,
            "concurrent jobs at failure: " + std::to_string(concurrency.value("count", 0)) +
            " (" + concurrency.value("ids", json::array()).dump() + ")");

    AddItem(info["reason"], "attemptReason", attemptReason, "attempt reason: " + attemptReason);
    const json detail = Object(failure, "detail");
    AddItem(info["reason"], "failureRule", String(detail, "rule"), "failure rule: " + String(detail, "rule"));

    json candidates = json::array();
    AddCandidate(candidates, "retry", "Retry", "Run this job again.", true);
    AddCandidate(candidates, "retry_raise_timeout", "Retry with a longer timeout",
                 "Double the timeout, then retry (bounded by original and queue limits).", true,
                 {{"multiplier", 2}, {"originalTimeoutSec", timeoutSec}});
    AddCandidate(candidates, "fix_spec", "Ask the supervisor to fix the specification",
                 "Launch the supervisor with this job's failure information.", false);
    AddCandidate(candidates, "cancel", "Cancel", "Cancel this job.", false);
    AddCandidate(candidates, "ignore", "Ignore", "Record that this failure needs no action.", false);
    AddCandidate(candidates, "investigate", "Investigate", "Inspect the cause before taking action.", false);

    std::string candidateId;
    std::string recConfidence = "low";
    std::string rationale;
    std::string rationaleKey = "noRecommendation";
    json rationaleParams = json::object();
    bool needsHuman = false;
    if (failureClass == "timeout" || attemptReason == "timed_out") {
        if (pattern == "progressing") {
            candidateId = "retry_raise_timeout"; recConfidence = "high";
            rationale = "The job was still making progress near its timeout; retry with more time.";
            rationaleKey = "timeoutProgressing"; rationaleParams = {{"timeoutSec", timeoutSec}};
        } else if (pattern == "stalled") {
            candidateId = "investigate"; recConfidence = "medium";
            rationale = "The job had stopped making progress well before the timeout; investigate first.";
            rationaleKey = "timeoutStalled";
        } else if (pattern == "repeated") {
            rationale = "Consecutive attempts timed out; gather more information before retrying.";
            rationaleKey = "timeoutRepeated";
        } else {
            candidateId = "retry_raise_timeout";
            rationale = "Timeout progress is unknown; a longer timeout is a cautious possibility.";
            rationaleKey = "timeoutUnknown";
        }
    } else if (failureClass == "idle_stall") {
        candidateId = "investigate"; recConfidence = "medium";
        rationale = "The job stalled without activity; investigate the cause first.";
        rationaleKey = "idleStall";
    } else if (String(failure, "suggestedAction") == "retry_after") {
        candidateId = "retry"; recConfidence = "high";
        rationale = "The failure is rate-limited; retry after " + String(failure, "retryAt", "the indicated delay") + ".";
        rationaleKey = "retryAfter"; rationaleParams = {{"retryAt", String(failure, "retryAt", "the indicated delay")}};
    } else if (String(failure, "suggestedAction") == "raise_timeout") {
        candidateId = "retry_raise_timeout"; recConfidence = "high";
        rationale = "The failure classifier recommends raising the timeout.";
        rationaleKey = "raiseTimeout";
    } else if (String(failure, "suggestedAction") == "fix_spec") {
        candidateId = "fix_spec"; recConfidence = "high";
        rationale = "The failure classifier recommends correcting the job specification.";
        rationaleKey = "fixSpecification";
    } else if (String(failure, "suggestedAction") == "retry") {
        candidateId = "retry";
        // Only a transient failure (Hancho restarted under the attempt, a crash) is likely to pass on an
        // unchanged rerun. An ordinary non-zero exit or failed success check usually needs a fix first,
        // so it stays medium and is never retried automatically.
        const bool transient = failureClass == "killed" || failureClass == "crash";
        recConfidence = transient && Confidence(failure) == "high" ? "high" : "medium";
        rationale = transient
            ? "The failure looks transient; an unchanged retry is likely to pass."
            : "The failure classifier suggests a retry, but an unchanged rerun may fail the same way; check the cause first.";
        rationaleKey = transient ? "transientRetry" : "retryCheckCause";
        rationaleParams = {{"class", failureClass}};
    } else {
        rationale = "The failure does not have a safe, deterministic recommendation.";
    }
    if (failureClass == "unknown" || Confidence(failure) == "low" ||
        String(failure, "suggestedAction") == "human" ||
        String(failure, "suggestedAction") == "escalate") {
        candidateId.clear();
        recConfidence = "low";
    }
    if (String(failure, "suggestedAction") == "human" || failureClass == "auth_error" ||
        failureClass == "cancelled") needsHuman = true;
    if (candidateId.empty() && rationale.empty()) rationale = "No recommendation is available.";
    AddItem(info["reason"], "recommendationRationale", {{"key", rationaleKey}, {"params", rationaleParams}},
            "recommendation: " + rationale);

    const json btInfo = Object(job, "bt");
    json target = {{"jobId", jobId}, {"queue", String(job, "taskqueue")}, {"name", name},
                   {"originalTimeoutSec", timeoutSec}};
    if (!String(btInfo, "id").empty()) target["btId"] = String(btInfo, "id");
    json card = {{"id", TriageCardIdFromKey(key)}, {"key", key}, {"kind", "job_failure"},
                 {"target", std::move(target)}, {"createdAt", in.now}, {"updatedAt", in.now},
                  {"state", "pending"}, {"info", std::move(info)}, {"candidates", std::move(candidates)},
                  {"recommendation", {{"source", "hancho"}, {"candidateId", candidateId},
                                      {"confidence", recConfidence}, {"rationale", rationale},
                                      {"rationaleKey", rationaleKey}, {"rationaleParams", rationaleParams}}},
                 {"advice", nullptr}, {"adviceState", "none"}, {"decision", nullptr},
                 {"flags", {{"needsHuman", needsHuman}, {"timeoutPattern", pattern}}}};
    return card;
}

bool IsJobInsideEscalation(const json& job, const json& failures) {
    const json bt = Object(job, "bt");
    const std::string btId = String(bt, "id");
    const std::string nodeId = String(bt, "node");
    for (const auto& failure : failures) {
        if (String(failure, "btId") != btId || btId.empty()) continue;
        const std::string failedNode = String(failure, "nodeId");
        if (failedNode == nodeId) return true;
        const json detail = Object(failure, "detail");
        for (const char* field : {"jobIds", "jobs"}) {
            const json ids = Array(detail, field);
            for (const auto& id : ids) {
                if ((id.is_string() && id.get<std::string>() == String(job, "id")) ||
                    (id.is_object() && String(id, "id") == String(job, "id"))) return true;
            }
        }
    }
    return false;
}

json BuildSimpleCard(const std::string& key, const std::string& kind,
                     const json& target, const json& info, const json& candidates,
                     const std::string& candidate, const std::string& confidence,
                     const std::string& rationale, const std::string& now) {
    const std::string rationaleKey = kind == "bt_escalation_failure" ? "btEscalationFailure" :
        kind == "process_failure" ? "processFailure" :
        kind == "timing_divergence" ? "timingDivergence" :
        kind == "resource_wait" ? "resourceWait" : "recommendation";
    return {{"id", TriageCardIdFromKey(key)}, {"key", key}, {"kind", kind},
            {"target", target}, {"createdAt", now}, {"updatedAt", now},
            {"state", "pending"}, {"info", info}, {"candidates", candidates},
            {"recommendation", {{"source", "hancho"}, {"candidateId", candidate},
                                {"confidence", confidence}, {"rationale", rationale},
                                {"rationaleKey", rationaleKey}, {"rationaleParams", json::object()}}},
            {"advice", nullptr}, {"adviceState", "none"}, {"decision", nullptr},
            {"flags", {{"needsHuman", false}, {"timeoutPattern", ""}}}};
}

const json* FindCandidate(const json& card, const std::string& candidateId) {
    const auto candidates = card.find("candidates");
    if (candidates == card.end() || !candidates->is_array()) return nullptr;
    for (const auto& candidate : *candidates)
        if (String(candidate, "id") == candidateId) return &candidate;
    return nullptr;
}

std::string WhyNot(const std::string& value) {
    return value.empty() ? "automatic action is not allowed" : value;
}

} // namespace

std::string TriageCardIdFromKey(const std::string& key) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : key) {
        hash ^= c;
        hash *= 16777619u;
    }
    std::ostringstream out;
    out << 'c' << std::hex << std::nouppercase << std::setw(8) << std::setfill('0') << hash;
    return out.str();
}

std::string TriageTimeoutPattern(const nlohmann::json& job) {
    const json failure = FailureFor(job);
    const json attempts = Array(job, "attempts");
    const json last = LastAttempt(job);
    if (FailureClass(failure) == "idle_stall") return "stalled";
    if (FailureClass(failure) != "timeout" && AttemptReason(last) != "timed_out") return "";
    if (attempts.size() >= 2 && TimedOut(attempts[attempts.size() - 2])) return "repeated";
    const json activity = Object(failure, "lastActivity");
    long long since = Number(activity, "sinceSec", -1);
    if (since < 0) since = Number(Object(last, "failure"), "lastActivitySinceSec", -1);
    const long long timeout = Number(job, "timeout_sec", 0);
    if (since < 0) return "";
    return since <= std::max(30LL, timeout / 10) ? "progressing" : "stalled";
}

nlohmann::json BuildTriageCandidates(const TriageInputs& in) {
    json fresh = json::array();
    const json jobs = in.jobs.is_array() ? in.jobs : json::array();
    const json btFailures = in.btFailures.is_array() ? in.btFailures : json::array();
    const json btStates = in.btStates.is_object() ? in.btStates : json::object();

    for (const auto& job : jobs) {
        if (!job.is_object() || Bool(job, "virtual") || String(job, "taskqueue") == "_triage") continue;
        const std::string jobId = String(job, "id");
        if (jobId.empty()) continue;
        const json bt = Object(job, "bt");
        const std::string btId = String(bt, "id");
        bool btFailed = true;
        if (!btId.empty()) {
            const auto state = btStates.find(btId);
            if (state != btStates.end() && state->is_string()) btFailed = state->get<std::string>() == "failed";
        }
        if (String(job, "state") == "failed" && btFailed && !IsJobInsideEscalation(job, btFailures))
            fresh.push_back(BuildJobFailureCard(job, in));

        const json timing = Object(job, "timing_feedback");
        const std::string accuracy = String(timing, "accuracy");
        if (accuracy == "diverged_over" || accuracy == "diverged_under") {
            const std::string key = "timing:" + jobId;
            const long long duration = Number(timing, "duration_sec", 0);
            const long long estimate = Number(timing, "estimate_sec", Number(job, "estimate_sec", 0));
            json info = {{"cause", json::array()}, {"background", json::array()},
                         {"environment", json::array()}, {"conditions", json::array()},
                         {"reason", json::array()}};
            AddItem(info["cause"], "timingAccuracy", accuracy, "timing accuracy: " + accuracy);
            AddItem(info["background"], "jobName", String(job, "name", jobId), "name: " + String(job, "name", jobId));
            AddItem(info["background"], "jobSummary", String(job, "summary"), String(job, "summary"));
            AddItem(info["background"], "queue", String(job, "taskqueue"), "queue: " + String(job, "taskqueue"));
            if (!String(job, "host").empty()) AddItem(info["environment"], "host", String(job, "host"), "host: " + String(job, "host"));
            else AddItem(info["environment"], "host", "local", "host: local");
            AddItem(info["environment"], "cwd", String(job, "cwd"), "cwd: " + String(job, "cwd"));
            AddItem(info["conditions"], "actualDurationSec", duration, "actual durationSec: " + std::to_string(duration));
            AddItem(info["conditions"], "estimateSec", estimate, "estimateSec: " + std::to_string(estimate));
            AddItem(info["conditions"], "ratio", timing.value("ratio", json()),
                    "ratio: " + timing.value("ratio", json()).dump());
            const double defaultThreshold = accuracy == "diverged_over" ? 2.0 : 0.33;
            const std::string thresholdKey = accuracy == "diverged_over"
                ? "audit_timing_divergence_threshold" : "audit_timing_under_threshold";
            double threshold = defaultThreshold;
            if (in.config.is_object() && in.config.contains(thresholdKey.c_str()) && in.config[thresholdKey.c_str()].is_number())
                threshold = in.config[thresholdKey.c_str()].get<double>();
            AddItem(info["conditions"], "divergenceThreshold", threshold,
                    "divergence threshold: " + std::to_string(threshold));
            const std::string jobName = String(job, "name", jobId);
            int affected = 0;
            for (const auto& candidate : jobs) {
                const std::string state = String(candidate, "state");
                if (String(candidate, "taskqueue") == String(job, "taskqueue") &&
                    String(candidate, "name").rfind(jobName, 0) == 0 && state != "succeeded" &&
                    state != "failed" && state != "cancelled") ++affected;
            }
            AddItem(info["conditions"], "recalibrateCount", affected,
                    "unfinished same-name jobs recalibrate would change: " + std::to_string(affected));
            AddItem(info["reason"], "timingAdvice", String(timing, "advice"), String(timing, "advice"));
            json candidates = json::array();
            AddCandidate(candidates, "recalibrate", "Recalibrate estimates",
                         "Update estimates of unfinished similar jobs using this actual duration.", true,
                         {{"actualDurationSec", duration}, {"name", String(job, "name")},
                          {"queue", String(job, "taskqueue")}});
            AddCandidate(candidates, "ignore", "Ignore", "Record that this divergence needs no action.", false);
            fresh.push_back(BuildSimpleCard(key, "timing_divergence",
                {{"jobId", jobId}, {"queue", String(job, "taskqueue")},
                 {"name", String(job, "name", jobId)}, {"actualDurationSec", duration}},
                info, candidates, "recalibrate", "medium",
                "Use the measured duration to recalibrate unfinished jobs of the same kind and queue.", in.now));
        }

        const std::string resourceWait = String(job, "resource_wait");
        if (!resourceWait.empty() && resourceWait != "null") {
            const std::string key = "res:" + jobId + "=" + resourceWait;
            json info = {{"cause", json::array()}, {"background", json::array()},
                         {"environment", json::array()}, {"conditions", json::array()},
                         {"reason", json::array()}};
            AddItem(info["cause"], "resourceWait", resourceWait, "waiting for resource: " + resourceWait);
            AddItem(info["background"], "jobName", String(job, "name", jobId), "name: " + String(job, "name", jobId));
            AddItem(info["background"], "jobSummary", String(job, "summary"), String(job, "summary"));
            AddItem(info["background"], "queue", String(job, "taskqueue"), "queue: " + String(job, "taskqueue"));
            const std::string host = String(job, "host");
            AddItem(info["environment"], "host", host.empty() ? json("local") : json(host), "host: " + (host.empty() ? "local" : host));
            AddItem(info["environment"], "cwd", String(job, "cwd"), "cwd: " + String(job, "cwd"));
            std::string holderId = resourceWait.substr(0, resourceWait.find(':'));
            const size_t queuedMark = holderId.find(" (queued ahead)");
            if (queuedMark != std::string::npos) holderId.resize(queuedMark);
            json holder = json::object();
            for (const auto& other : jobs) if (String(other, "id") == holderId) { holder = other; break; }
            const std::string holderState = String(holder, "state");
            std::string lockMode = "unknown";
            const std::string resourceName = TrimWhitespace(resourceWait.find(':') == std::string::npos ? resourceWait : resourceWait.substr(resourceWait.find(':') + 1));
            for (const char* mode : {"write", "read"}) for (const auto& lock : Array(Object(job, "resources"), mode))
                if (lock.is_string() && lock.get<std::string>() == resourceName) lockMode = mode;
            json holderValue = {{"lockMode", lockMode}, {"resource", resourceWait}};
            if (!holderId.empty()) holderValue["jobId"] = holderId;
            if (!holderState.empty()) holderValue["state"] = holderState;
            const json holderAttempts = Array(holder, "attempts");
            if (!holderAttempts.empty()) {
                std::time_t started = 0;
                if (ParseTime(String(holderAttempts.back(), "start"), started))
                    holderValue["runningSec"] = std::max<long long>(0, static_cast<long long>(std::time(nullptr) - started));
            }
            AddItem(info["environment"], "resourceHolder", holderValue,
                    "holder job " + holderId + (holderState.empty() ? "" : " [" + holderState + "]") + " (" + lockMode + " lock)");
            std::time_t created = 0, now = 0;
            if (ParseTime(String(job, "created_at"), created) && ParseTime(in.now, now)) {
                const long long waited = std::max<long long>(0, static_cast<long long>(now - created));
                AddItem(info["conditions"], "waitingSince", {{"at", String(job, "created_at")}, {"elapsedSec", waited}},
                        "waiting since " + String(job, "created_at") + " (approximately " + std::to_string(waited) + "s)");
            }
            AddItem(info["reason"], "resourceConflict", {{"lockMode", lockMode}, {"resource", resourceWait}},
                    lockMode == "write" ? "A write lock conflicts with another lock." :
                    lockMode == "read" ? "A read lock is waiting for a conflicting write lock." :
                    "The job is waiting for a resource lock.");
            json candidates = json::array();
            AddCandidate(candidates, "wait", "Wait", "Record the wait and let resource scheduling continue.", true);
            AddCandidate(candidates, "show_holder", "Show resource holder", "Inspect which job holds the resource.", false);
            AddCandidate(candidates, "fix_spec", "Ask the supervisor to fix the specification", "Launch the supervisor with this wait information.", false);
            AddCandidate(candidates, "ignore", "Ignore", "Record that this wait needs no action.", false);
            fresh.push_back(BuildSimpleCard(key, "resource_wait",
                {{"jobId", jobId}, {"queue", String(job, "taskqueue")},
                 {"name", String(job, "name", jobId)}, {"resourceWait", resourceWait}},
                info, candidates, "wait", "medium", "Let the resource scheduler continue while recording the wait.", in.now));
        }
    }

    for (const auto& failure : btFailures) {
        if (!failure.is_object()) continue;
        const std::string btId = String(failure, "btId");
        const std::string nodeId = String(failure, "nodeId");
        if (btId.empty() || nodeId.empty()) continue;
        const std::string key = "esc:" + btId + "/" + nodeId;
        const std::string detail = String(failure, "detail", "escalation failed");
        json info = {{"cause", json::array()}, {"background", json::array()},
                     {"environment", json::array()}, {"conditions", json::array()},
                     {"reason", json::array()}};
        AddItem(info["cause"], "escalationFailure", detail, "behavior tree escalation failed: " + detail);
        AddItem(info["background"], "treeId", btId, "BT: " + btId);
        AddItem(info["background"], "treeName", String(failure, "treeName"), "tree: " + String(failure, "treeName"));
        AddItem(info["background"], "treeState", String(failure, "treeState"), "tree state: " + String(failure, "treeState"));
        AddItem(info["background"], "nodeId", nodeId, "node: " + nodeId);
        AddItem(info["background"], "question", String(failure, "question"), "question: " + String(failure, "question"));
        AddItem(info["background"], "options", Array(failure, "options"), "options: " + Array(failure, "options").dump());
        AddItem(info["background"], "guidance", String(failure, "guidance"), "guidance: " + String(failure, "guidance"));
        AddItem(info["background"], "relatedJobs", Array(failure, "jobs"), "related jobs: " + Array(failure, "jobs").dump());
        AddItem(info["environment"], "queue", String(failure, "queue"), "queue: " + String(failure, "queue"));
        if (Number(failure, "timeoutSec") > 0)
            AddItem(info["conditions"], "escalationTimeoutSec", Number(failure, "timeoutSec"),
                    "escalation timeout: " + std::to_string(Number(failure, "timeoutSec")) + "s");
        AddItem(info["reason"], "treeFailureReason", detail, detail);
        json candidates = json::array();
        AddCandidate(candidates, "bt_retry", "Retry behavior tree", "Retry the failed escalation node.", true);
        AddCandidate(candidates, "bt_resume", "Resume with an answer", "Supply an answer to the escalation.", false);
        AddCandidate(candidates, "bt_cancel", "Cancel behavior tree", "Cancel this behavior tree.", false);
        AddCandidate(candidates, "fix_spec", "Ask the supervisor to fix the specification", "Launch the supervisor with the failure information.", false);
        AddCandidate(candidates, "ignore", "Ignore", "Record that this failure needs no action.", false);
        fresh.push_back(BuildSimpleCard(key, "bt_escalation_failure",
            {{"btId", btId}, {"nodeId", nodeId}, {"treeName", String(failure, "treeName")}}, info, candidates, "", "low",
            "A human should inspect the failed decision request.", in.now));
    }
    return fresh;
}

nlohmann::json BuildProcessTriageCandidates(const nlohmann::json& processes) {
    json fresh = json::array();
    if (!processes.is_array()) return fresh;
    for (const auto& process : processes) {
        if (!process.is_object()) continue;
        const std::string name = String(process, "name");
        if (Lower(name).rfind("triage-llm", 0) == 0) continue;
        if (!String(process, "job_key").empty() || Number(process, "job_id", 0) > 0 ||
            Number(process, "attempt", 0) > 0) continue;
        const std::string state = Lower(String(process, "state"));
        // "killed" means someone stopped the process on purpose (kill command, dashboard button,
        // closing a supervisor terminal); that needs no decision, so only Hancho-detected ends count.
        if (state != "failed" && state != "timed_out" && state != "idle_timed_out") continue;
        const std::string id = process.contains("id") && process["id"].is_string()
            ? process["id"].get<std::string>()
            : (process.contains("id") && process["id"].is_number()
                ? std::to_string(Number(process, "id", 0)) : std::string());
        const std::string started = String(process, "start_time");
        if (id.empty() || started.empty()) continue;
        const std::string key = "proc:" + id + "@" + started;
        const std::string command = String(process, "command");
        const std::string cwd = String(process, "cwd");
        std::string mode = String(process, "mode", "pipe");
        if (mode == "conpty") mode = "pty";
        const long long elapsedMs = Number(process, "elapsed_ms", 0);
        const long long timeoutSec = Number(process, "timeout_sec", 0);
        const long long idleTimeoutSec = Number(process, "idle_timeout_sec", 0);
        json info = {{"cause", json::array()}, {"background", json::array()},
                     {"environment", json::array()}, {"conditions", json::array()},
                     {"reason", json::array()}};
        AddItem(info["cause"], "endState", state, "state: " + state);
        if (process.contains("exit_code") && process["exit_code"].is_number() && Number(process, "exit_code", -1) >= 0)
            AddItem(info["cause"], "exitCode", Number(process, "exit_code", -1),
                    "exit code: " + std::to_string(Number(process, "exit_code", -1)));
        AddItem(info["background"], "processName", name, "name: " + name);
        AddItem(info["background"], "command", command, "command: " + command);
        AddItem(info["background"], "startedAt", started, "started at: " + started);
        AddItem(info["environment"], "cwd", cwd, "cwd: " + cwd);
        AddItem(info["environment"], "mode", mode, "mode: " + mode);
        if (timeoutSec > 0) AddItem(info["conditions"], "timeoutSec", timeoutSec, "timeoutSec: " + std::to_string(timeoutSec));
        if (idleTimeoutSec > 0) AddItem(info["conditions"], "idleTimeoutSec", idleTimeoutSec,
                                        "idleTimeoutSec: " + std::to_string(idleTimeoutSec));
        if (elapsedMs > 0) AddItem(info["conditions"], "durationSec", elapsedMs / 1000,
                                   "elapsedSec: " + std::to_string(elapsedMs / 1000));
        AddItem(info["reason"], "processEndReason", state, "process ended in state " + state);
        const std::string log = String(process, "log_file");
        if (!log.empty()) {
            auto lines = ReadLines(log);
            if (!lines.empty()) {
                json tail = json::array();
                const size_t first = lines.size() > 8 ? lines.size() - 8 : 0;
                for (size_t i = first; i < lines.size(); ++i) tail.push_back(Clip(lines[i]));
                AddItem(info["cause"], "lastOutput", {{"file", log}, {"lines", tail}},
                        "last log lines: " + tail.dump());
            }
            AddItem(info["cause"], "logFile", log, "log: " + log);
        }
        json candidates = json::array();
        AddCandidate(candidates, "restart", "Restart", "Start the same command again with the same name and working directory.", false);
        AddCandidate(candidates, "ack", "Acknowledge", "Record that this process failure was reviewed.", false);
        AddCandidate(candidates, "investigate", "Investigate", "Record that the process needs investigation.", false);
        fresh.push_back(BuildSimpleCard(key, "process_failure",
            {{"processId", id}, {"name", name}, {"command", command}, {"cwd", cwd},
             {"mode", mode}, {"timeoutSec", timeoutSec}, {"idleTimeoutSec", idleTimeoutSec}},
            info, candidates, "", "low", "Hancho cannot determine whether a plain process should be restarted.",
            String(process, "start_time")));
    }
    return fresh;
}

std::vector<std::string> MergeTriageCards(nlohmann::json& store,
                                          const nlohmann::json& fresh,
                                          const std::string& now) {
    if (!store.is_object()) store = json::object();
    if (String(store, "schema").empty()) store["schema"] = "hancho.triage/1";
    if (!store.contains("cards") || !store["cards"].is_array()) store["cards"] = json::array();
    if (!store.contains("autoRetries") || !store["autoRetries"].is_array()) store["autoRetries"] = json::array();

    json& cards = store["cards"];
    std::unordered_set<std::string> keys;
    if (fresh.is_array()) {
        for (const auto& card : fresh) {
            if (!card.is_object()) continue;
            const std::string key = String(card, "key");
            if (!key.empty()) keys.insert(key);
        }
    }
    for (auto& card : cards) {
        if (!card.is_object() || String(card, "state") != "pending") continue;
        const std::string key = String(card, "key");
        if (!key.empty() && keys.find(key) == keys.end()) {
            card["state"] = "resolved";
            card["updatedAt"] = now;
        }
    }

    std::vector<std::string> added;
    if (fresh.is_array()) {
        for (const auto& candidate : fresh) {
            if (!candidate.is_object()) continue;
            const std::string key = String(candidate, "key");
            if (key.empty()) continue;
            auto found = std::find_if(cards.begin(), cards.end(), [&](const json& card) {
                return String(card, "key") == key;
            });
            if (found == cards.end()) {
                json item = candidate;
                item["state"] = "pending";
                item["updatedAt"] = now;
                cards.push_back(std::move(item));
                added.push_back(String(candidate, "id", TriageCardIdFromKey(key)));
            } else if (String(*found, "state") == "resolved" &&
                       String(*found, "resolvedReason") == "kind_disabled") {
                json item = candidate;
                item["state"] = "pending";
                item["updatedAt"] = now;
                item.erase("resolvedReason");
                *found = std::move(item);
                added.push_back(String(candidate, "id", TriageCardIdFromKey(key)));
            } else if (String(*found, "state") == "pending") {
                const json advice = Object(*found, "advice");
                const json decision = Object(*found, "decision");
                const json handoff = Object(*found, "handoff");
                const json manual = Object(*found, "manual");
                const json outcome = Object(*found, "outcome");
                const json previousCardId = (*found).value("previousCardId", json());
                const json lifecycle = (*found).value("lifecycle", json());
                const std::string adviceState = String(*found, "adviceState", "none");
                const std::string createdAt = String(*found, "createdAt", String(candidate, "createdAt", now));
                // Leave the card untouched (and its updatedAt stable) when nothing Hancho derives has changed.
                if (Object(*found, "info") == Object(candidate, "info") &&
                    (*found).value("candidates", json()) == candidate.value("candidates", json()) &&
                    Object(*found, "recommendation") == Object(candidate, "recommendation") &&
                    Object(*found, "flags") == Object(candidate, "flags")) {
                    continue;
                }
                *found = candidate;
                (*found)["createdAt"] = createdAt;
                (*found)["updatedAt"] = now;
                (*found)["advice"] = advice.empty() ? json(nullptr) : advice;
                (*found)["adviceState"] = adviceState;
                (*found)["decision"] = decision.empty() ? json(nullptr) : decision;
                if (!handoff.empty()) (*found)["handoff"] = handoff;
                if (!manual.empty()) (*found)["manual"] = manual;
                if (!outcome.empty()) (*found)["outcome"] = outcome;
                if (!previousCardId.is_null()) (*found)["previousCardId"] = previousCardId;
                if (!lifecycle.is_null()) (*found)["lifecycle"] = lifecycle;
            }
        }
    }

    while (cards.size() > 200) {
        auto oldestDone = cards.end();
        for (auto it = cards.begin(); it != cards.end(); ++it) {
            const std::string state = String(*it, "state");
            const std::string kind = String(*it, "kind");
            const json outcome = Object(*it, "outcome");
            const bool tracksOutcome = kind == "job_failure" || kind == "bt_escalation_failure";
            const bool done = String(*it, "lifecycle") == "done" || state == "ignored" ||
                (outcome.is_object() && Bool(outcome, "complete")) ||
                (!tracksOutcome && (state == "acted" || state == "resolved"));
            if (done && (oldestDone == cards.end() ||
                          String(*it, "createdAt") < String(*oldestDone, "createdAt"))) oldestDone = it;
        }
        // Open cards are never discarded to enforce the soft cap. If there are no closed
        // cards to evict, retain the additional open card until one can be safely removed.
        if (oldestDone == cards.end()) break;
        cards.erase(oldestDone);
    }
    return added;
}

void ApplyTriageKindSettings(nlohmann::json& store, nlohmann::json& fresh,
                             const nlohmann::json& config, const std::string& now) {
    const json kinds = Object(config, "triage_kinds");
    auto enabled = [&](const std::string& kind) {
        const auto it = kinds.find(kind);
        if (it == kinds.end() || !it->is_boolean()) return kind != "resource_wait";
        return it->get<bool>();
    };
    if (fresh.is_array()) {
        fresh.erase(std::remove_if(fresh.begin(), fresh.end(), [&](const json& card) {
            return !enabled(String(card, "kind"));
        }), fresh.end());
    }
    if (!store.is_object() || !store.contains("cards") || !store["cards"].is_array()) return;
    for (auto& card : store["cards"]) {
        if (String(card, "state") != "pending" || enabled(String(card, "kind"))) continue;
        card["state"] = "resolved";
        card["resolvedReason"] = "kind_disabled";
        card["updatedAt"] = now;
    }
}

bool TriageNeedsLlm(const nlohmann::json& card, const std::string& mode,
                    const std::string& llmMode) {
    if (Lower(mode) == "auto") return true;
    if (Lower(llmMode) == "manual" || Lower(llmMode) == "always") return true;
    if (Lower(llmMode) != "uncertain") return false;
    if (Lower(String(Object(card, "recommendation"), "confidence", "low")) == "low") return true;
    const json flags = Object(card, "flags");
    const json info = Object(card, "info");
    const json background = Array(info, "background");
    std::string currentClass;
    for (const auto& item : Array(info, "cause")) {
        if (item.is_object() && String(item, "k") == "failureClass") {
            currentClass = String(InfoValue(item), "class");
            break;
        }
        const std::string text = InfoText(item);
        const std::string prefix = "failure class: ";
        if (text.rfind(prefix, 0) == 0) {
            const size_t end = text.find(" (", prefix.size());
            currentClass = text.substr(prefix.size(), end == std::string::npos ? std::string::npos : end - prefix.size());
            break;
        }
    }
    // Hancho cannot judge a plain process; in "uncertain" mode it is only sent to the LLM on request.
    if (String(card, "kind") == "process_failure") return false;
    bool timedOut = currentClass == "timeout";
    for (const auto& item : Array(info, "reason")) {
        if (item.is_object() && String(item, "k") == "attemptReason" && InfoValue(item) == "timed_out") timedOut = true;
        const std::string text = InfoText(item);
        if (text == "attempt reason: timed_out") timedOut = true;
    }
    const std::string pattern = String(flags, "timeoutPattern");
    if (timedOut && (pattern == "repeated" || pattern.empty())) return true;
    for (const auto& line : background) {
        if (line.is_object() && String(line, "k") == "attemptHistory") {
            const json history = InfoValue(line);
            if (history.is_array() && history.size() > 1) {
                for (size_t i = 0; i + 1 < history.size(); ++i)
                    if (String(history[i], "class") == currentClass && !currentClass.empty()) return true;
            }
        }
        const std::string value = InfoText(line);
        if (value.rfind("previous attempt classes: ", 0) == 0) {
            const std::string previous = value.substr(26);
            if (!currentClass.empty() && previous.find(currentClass) != std::string::npos) return true;
        }
    }
    return false;
}

std::string TriageAutoChoice(const nlohmann::json& store, const nlohmann::json& card,
                             const std::string& mode, std::string& outBy,
                             std::string& outWhyNot) {
    outBy.clear();
    outWhyNot.clear();
    const std::string normalizedMode = Lower(mode);
    if (normalizedMode != "semi" && normalizedMode != "auto") {
        outWhyNot = "manual mode never executes automatically";
        return "";
    }
    if (String(card, "state") != "pending") { outWhyNot = "card is not pending"; return ""; }
    if (Bool(Object(card, "flags"), "needsHuman")) { outWhyNot = "card requires human judgment"; return ""; }
    const json advice = Object(card, "advice");
    if (String(card, "adviceState") == "failed" || String(advice, "error").size() > 0) {
        outWhyNot = "LLM advice failed"; return "";
    }
    if (!advice.empty() && Lower(String(advice, "confidence")) == "low") {
        outWhyNot = "LLM advice confidence is low"; return "";
    }

    std::string candidateId;
    if (normalizedMode == "semi") {
        const json recommendation = Object(card, "recommendation");
        if (Lower(String(recommendation, "confidence")) != "high") {
            outWhyNot = "Hancho recommendation is not high confidence"; return "";
        }
        candidateId = String(recommendation, "candidateId");
        outBy = "hancho-auto";
    } else {
        if (advice.empty()) { outWhyNot = "LLM advice is not available"; return ""; }
        const std::string confidence = Lower(String(advice, "confidence"));
        if (confidence != "high" && confidence != "medium") {
            outWhyNot = "LLM advice confidence is not high or medium"; return "";
        }
        candidateId = String(advice, "candidateId");
        outBy = "llm-auto";
    }
    const json* candidate = FindCandidate(card, candidateId);
    if (!candidate) { outBy.clear(); outWhyNot = "candidate is not on this card"; return ""; }
    if (!Bool(*candidate, "safe")) { outBy.clear(); outWhyNot = "candidate is not marked safe"; return ""; }
    if (candidateId == "retry" || candidateId == "retry_raise_timeout") {
        const json target = Object(card, "target");
        const json info = Object(card, "info");
        std::string failureClass;
        for (const auto& line : Array(info, "cause")) {
            if (line.is_object() && String(line, "k") == "failureClass") {
                failureClass = String(InfoValue(line), "class");
                break;
            }
            const std::string text = InfoText(line);
            const std::string prefix = "failure class: ";
            if (text.rfind(prefix, 0) == 0) {
                const size_t end = text.find(' ', prefix.size());
                failureClass = text.substr(prefix.size(), end == std::string::npos ? std::string::npos : end - prefix.size());
                break;
            }
        }
        const std::string retryKey = String(target, "jobId") + "|" + failureClass;
        const json retries = Array(store, "autoRetries");
        for (const auto& retry : retries) {
            if (retry.is_string() && retry.get<std::string>() == retryKey) {
                outBy.clear(); outWhyNot = "the one automatic retry for this job and failure class was already used";
                return "";
            }
        }
    }
    return candidateId;
}

std::string BuildTriageAdvicePacket(const nlohmann::json& card, const std::string& language) {
    json packet = {{"schema", "hancho.triage-advice/1"},
                   {"language", language.empty() ? "en" : language},
                   {"card", {{"id", String(card, "id")}, {"key", String(card, "key")},
                              {"kind", String(card, "kind")}, {"target", Object(card, "target")},
                              {"info", Object(card, "info")}}},
                   {"candidates", json::array()},
                   {"recommendation", Object(card, "recommendation")}};
    for (const auto& candidate : Array(card, "candidates")) {
        packet["candidates"].push_back({{"id", String(candidate, "id")},
                                        {"label", String(candidate, "label")},
                                        {"effect", String(candidate, "effect")},
                                        {"safe", Bool(candidate, "safe")}});
    }
    std::ostringstream out;
    out << "# Hancho triage advice\n\n"
        << "Read the card information and choose exactly one listed candidate id. Reply with a JSON object matching the required schema.\n\n"
        << "Do not run any command or edit any file. Only read the listed log files if needed. Choose exactly one candidate id, explain why in 2-4 sentences, and state what the human should watch out for. "
        << "Write rationale, caution and infoNeeded in the language given by the \"language\" field (for example ja = Japanese); keep candidateId and confidence exactly as specified.\n\n"
        << "```json\n" << packet.dump(2) << "\n```\n";
    return out.str();
}

namespace {

bool DependsOn(const json& job, const std::string& id) {
    for (const auto& dep : Array(job, "depends_on")) {
        if (dep.is_string() && dep.get<std::string>() == id) return true;
        if (dep.is_object() && String(dep, "id") == id) return true;
    }
    return false;
}

long long TriageCardAttempt(const json& card) {
    const std::string key = String(card, "key");
    const size_t hash = key.rfind('#');
    if (hash == std::string::npos) return 0;
    try { return std::stoll(key.substr(hash + 1)); } catch (...) { return 0; }
}

json TriageOutcomeForJob(const json& card, const json& job, const std::string& cardState) {
    const std::string jobId = String(job, "id");
    const long long cardAttempt = TriageCardAttempt(card);
    const json attempts = Array(job, "attempts");
    const long long attemptsCount = static_cast<long long>(attempts.size());
    const long long currentAttempt = attempts.empty()
        ? Number(job, "attempt_count", attemptsCount)
        : Number(attempts.back(), "n", attemptsCount);
    const std::string jobState = String(job, "state");
    if (jobState == "cancelled")
        return {{"state", "cancelled"}, {"detail", "the job was cancelled"}};
    if (jobState == "running" || jobState == "waiting" || jobState == "pending" || jobState == "blocked")
        return {{"state", "rerunning"}, {"detail", "attempt " + std::to_string(currentAttempt) + " " + jobState}};
    if (currentAttempt <= cardAttempt) {
        if (cardState == "acted")
            return {{"state", "not_rerun"}, {"detail", "no attempt ran after the decision"}};
        return nullptr;
    }
    if (jobState == "succeeded")
        return {{"state", "succeeded"}, {"detail", "attempt " + std::to_string(currentAttempt) + " succeeded"}};
    if (jobState == "failed")
        return {{"state", "failed_again"}, {"detail", "attempt " + std::to_string(currentAttempt) + " failed"},
                {"nextKey", "job:" + jobId + "#" + std::to_string(currentAttempt)}};
    return {{"state", "rerunning"}, {"detail", jobState}};
}

// Jobs downstream of jobId (direct and indirect dependents): the branch its outcome leads to.
json TriageBranch(const std::string& jobId, const json& jobs) {
    std::vector<std::string> frontier = {jobId};
    std::unordered_set<std::string> seen = {jobId};
    json list = json::array();
    int succeeded = 0, running = 0, failed = 0, blocked = 0, cancelled = 0;
    while (!frontier.empty()) {
        const std::string current = frontier.back();
        frontier.pop_back();
        for (const auto& job : jobs.is_array() ? jobs : json::array()) {
            const std::string id = String(job, "id");
            if (id.empty() || seen.count(id) || !DependsOn(job, current)) continue;
            seen.insert(id);
            frontier.push_back(id);
            const std::string state = String(job, "state");
            if (state == "succeeded") ++succeeded;
            else if (state == "failed") ++failed;
            else if (state == "blocked") ++blocked;
            else if (state == "cancelled") ++cancelled;
            else ++running;  // running, waiting, pending, planned
            json item = {{"id", id}, {"state", state}};
            if (state == "failed") {
                const json attempts = Array(job, "attempts");
                const long long attempt = attempts.empty()
                    ? Number(job, "attempt_count", 0)
                    : Number(attempts.back(), "n", static_cast<long long>(attempts.size()));
                item["key"] = "job:" + id + "#" + std::to_string(attempt);
            }
            list.push_back(std::move(item));
        }
    }
    const int total = static_cast<int>(list.size());
    // Failed dependents are reported here; the manager checks their own cards before closing this branch.
    return {{"total", total}, {"succeeded", succeeded}, {"running", running}, {"failed", failed},
            {"blocked", blocked}, {"cancelled", cancelled}, {"jobs", list},
            {"done", running == 0 && blocked == 0 && failed == 0}};
}

} // namespace

nlohmann::json TriageOutcome(const nlohmann::json& card, const nlohmann::json& jobs,
                             const nlohmann::json& btStates) {
    const json target = Object(card, "target");
    const std::string jobId = String(target, "jobId");
    const std::string kind = String(card, "kind");
    const std::string state = String(card, "state");
    const json handoff = Object(card, "handoff");
    const json manual = Object(card, "manual");
    // Only follow what someone acted on: a decision other than ignore, a natural resolution, or a card
    // being handled (batch supervisor or by hand) that has not been decided yet.
    if (state == "ignored") return nullptr;
    if (state == "pending" && handoff.empty() && manual.empty()) return nullptr;

    // BT escalation: the tree is the lifecycle.
    if (kind == "bt_escalation_failure") {
        const std::string btId = String(target, "btId");
        const std::string treeState = btStates.is_object() ? String(btStates, btId.c_str()) : std::string();
        json out = {{"bt", {{"id", btId}, {"state", treeState}}}};
        if (treeState.empty()) { out["state"] = "gone"; out["detail"] = "the behavior tree no longer exists"; out["complete"] = true; }
        else if (treeState == "succeeded") { out["state"] = "succeeded"; out["detail"] = "tree succeeded"; out["complete"] = true; }
        else if (treeState == "cancelled") { out["state"] = "cancelled"; out["detail"] = "tree cancelled"; out["complete"] = true; }
        else if (treeState == "failed") {
            if (state != "acted") return nullptr;
            out["state"] = "not_rerun";
            out["detail"] = "tree failed"; out["complete"] = false;
        } else { out["state"] = "rerunning"; out["detail"] = "tree " + treeState; out["complete"] = false; }
        return out;
    }

    if (jobId.empty() || kind != "job_failure") return nullptr;
    json job;
    for (const auto& candidate : jobs.is_array() ? jobs : json::array())
        if (String(candidate, "id") == jobId) { job = candidate; break; }
    if (!job.is_object() || job.empty())
        return {{"state", "gone"}, {"detail", "the job no longer exists"}, {"complete", true}};
    json out = TriageOutcomeForJob(card, job, state);
    if (out.is_null()) return out;
    const json branch = TriageBranch(jobId, jobs);
    if (branch["total"].get<int>() > 0) out["branch"] = branch;
    const json bt = Object(job, "bt");
    if (!bt.empty()) {
        const std::string btId = String(bt, "id");
        out["bt"] = {{"id", btId}, {"state", btStates.is_object() ? String(btStates, btId.c_str()) : std::string()}};
    }
    const std::string outState = String(out, "state");
    const bool terminal = outState == "succeeded" || outState == "cancelled" || outState == "gone";
    out["complete"] = terminal && branch["done"].get<bool>();
    return out;
}

std::string BuildTriageHandoffInstructions(const nlohmann::json& cards, const std::string& cli,
                                           const std::string& language, bool interactive) {
    json list = json::array();
    for (const auto& card : cards.is_array() ? cards : json::array()) {
        json candidates = json::array();
        for (const auto& candidate : Array(card, "candidates")) {
            candidates.push_back({{"id", String(candidate, "id")}, {"label", String(candidate, "label")},
                                  {"effect", String(candidate, "effect")}});
        }
        list.push_back({{"id", String(card, "id")}, {"kind", String(card, "kind")},
                        {"target", Object(card, "target")}, {"info", Object(card, "info")},
                        {"candidates", std::move(candidates)},
                        {"recommendation", Object(card, "recommendation")},
                        {"llmAdvice", card.contains("advice") ? card["advice"] : json(nullptr)}});
    }
    std::ostringstream out;
    out << "# Hancho triage hand-off\n\n";
    if (interactive) {
        out << "You are helping a person handle the Hancho triage card(s) below, in a conversation. Explain what "
               "you find, propose the candidate you would choose, and act once the person agrees.\n\n";
    } else {
        out << "You are the supervisor. A person handed you the pending triage cards below. Work through every "
               "card, then exit. You run non-interactively: nobody will answer questions.\n\n";
    }
    out
        << "For each card:\n"
        << "1. Read its information (cause, background, environment, conditions, reason). Read the listed log "
           "files when you need more detail. `" << cli << " triage show <id>` prints the current card and "
           "`" << cli << " taskqueue why <jobId>` the failure diagnosis.\n"
        << "2. Choose exactly one candidate id from that card.\n"
        << "3. If the job's command, configuration or source has to change first (candidate `fix_spec`), make the "
           "smallest fix in the job's working directory, then choose the candidate that reruns it (usually `retry` "
           "or `retry_raise_timeout`).\n"
        << "4. Record the decision with `" << cli << " triage decide <id> <candidateId> --reason \"<one or two "
           "sentences>\"` (add `--multiplier <1.5-4>` for retry_raise_timeout). The decide command performs the "
           "action (retry, cancel, ...); do not run it a second way.\n"
        << "5. `ignore` and `cancel` need a clear reason. If you cannot decide a card safely, leave it pending and "
           "say why in your final message.\n\n"
        << "Do not start unrelated work, do not commit, and do not change files outside what a fix needs. Write the "
           "reasons and notes in the language \"" << (language.empty() ? "en" : language) << "\" (ja = Japanese).\n\n";
    if (!interactive) {
        out << "Before you exit, write a JSON object to the file named by the PC_TRIAGE_RESULT_FILE environment "
               "variable, one entry per card, so the person sees what happened on each card:\n"
            << "```json\n{\"cards\": [{\"id\": \"<card id>\", \"outcome\": \"decided\" | \"left_pending\", "
               "\"note\": \"<what you did, or why you left it pending and what the person should decide>\"}]}\n```\n\n";
    }
    out
        << "```json\n" << list.dump(2) << "\n```\n";
    return out.str();
}

const char* TriageAdviceOutputSchema() {
    return R"({"type":"object","required":["candidateId","confidence","rationale"],"properties":{"candidateId":{"type":"string"},"confidence":{"type":"string"},"rationale":{"type":"string"},"caution":{"type":"string"},"infoNeeded":{"type":"string"}}})";
}

bool ParseTriageAdvice(const std::string& resultJson, const nlohmann::json& card,
                       nlohmann::json& outAdvice, std::string& err) {
    err.clear();
    if (!ValidateOutput(resultJson, TriageAdviceOutputSchema(), err)) return false;
    json parsed = json::parse(resultJson, nullptr, false);
    if (!parsed.is_object()) { err = "LLM result is not a JSON object"; return false; }
    const std::string candidateId = String(parsed, "candidateId");
    if (!FindCandidate(card, candidateId)) { err = "candidateId is not present on this card"; return false; }
    const std::string confidence = Lower(String(parsed, "confidence"));
    if (confidence != "high" && confidence != "medium" && confidence != "low") {
        err = "confidence must be high, medium, or low"; return false;
    }
    outAdvice = {{"source", "llm"}, {"candidateId", candidateId}, {"confidence", confidence},
                 {"rationale", String(parsed, "rationale")}, {"caution", String(parsed, "caution")},
                 {"infoNeeded", String(parsed, "infoNeeded")}, {"jobId", ""}, {"at", ""}};
    return true;
}

} // namespace pc
