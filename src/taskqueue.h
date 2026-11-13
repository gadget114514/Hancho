#pragma once

#include "process_manager.h"
#include "nlohmann/json.hpp"
#include <string>
#include <vector>
#include <map>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <functional>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <time.h>
#include <filesystem>
#include <fstream>
#include <sstream>

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
    int neverRunnablePending = 0; // Pending, but a dependency can never be satisfied
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
    // JUnit or NUnit XML written by the job (relative to cwd). It must be newer than the
    // attempt start, contain at least one test case, and have no failed ones.
    std::string testReport;
};

// Result of reading a test report (JUnit <testcase>, NUnit 2/3 <test-case>).
struct TestReportSummary {
    int total = 0;
    int failed = 0;
    int skipped = 0;
    std::vector<std::string> failedNames; // first few failed test names
};
// Pure parser, exposed for tests. False when the text has no recognizable test cases.
bool ParseTestReport(const std::string& xml, TestReportSummary& out);

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
// A named lock may contain {cwd} (the job's cwd) or {root} (the git top level above cwd,
// found by walking up to a .git dir or file; cwd itself when there is none, or for a
// remote host). "@editor:{root}" is then one lock per checkout or worktree.
std::string NormalizeResource(const std::string& resource, const std::string& cwd);
std::string NormalizeResource(const std::string& resource, const std::string& cwd,
                              const std::string& host);
bool ResourcesOverlap(const std::string& a, const std::string& b); // both normalized
bool FindResourceConflict(const TaskQueueResources& a, const std::string& cwdA,
                          const TaskQueueResources& b, const std::string& cwdB,
                          std::string& outResource);
// Same, but a resource on hostA never conflicts with the same path on hostB
// (doc/design-v3.md 2.5: locks are per host).
bool FindResourceConflict(const TaskQueueResources& a, const std::string& cwdA,
                          const std::string& hostA,
                          const TaskQueueResources& b, const std::string& cwdB,
                          const std::string& hostB,
                          std::string& outResource);

// ---------------------------------------------------------------------------
// Failure diagnostics (doc/design-v3.md 4.2-4.4)
//
// One record per attempt. The classifier is an ordered rule chain evaluated from
// the top: the first matching rule wins, so the same input always yields the same
// record. Every record carries evidence that points back at an original normalized
// event (events + seq) or an attempt log line (log + line).
// ---------------------------------------------------------------------------

struct TaskQueueFailureEvidence {
    std::string events; // normalized .events.jsonl the seq belongs to
    uint64_t seq = 0;
    std::string log;    // attempt log file
    int line = 0;       // 1-based line in that log
    bool IsEvent() const { return !events.empty(); }
    bool IsLog() const { return !log.empty(); }
};

struct TaskQueueFailureActivity {
    std::string activity; // read, edit, search, command, web, subagent, plan, tool
    std::string target;
    int64_t sinceSec = -1;
    bool Present() const { return !activity.empty() || !target.empty(); }
};

// An LLM guess is advisory only (doc/design-v3.md 4.4). It is kept apart from the
// deterministic fields and is never consulted by DecideTaskQueueRetry.
struct TaskQueueFailureLlmGuess {
    std::string failureClass;
    std::string confidence; // always "low"
    std::string summary;
    std::string suggestedAction;
    std::string rationale;
    std::string source; // "escalate"
    bool Present() const { return !failureClass.empty(); }
};

struct TaskQueueFailureDiagnostic {
    std::string failureClass; // one of the 4.2 classes, "unknown" when nothing matched
    std::string confidence;   // high / medium / low
    std::string summary;
    nlohmann::json detail = nlohmann::json::object();
    std::vector<TaskQueueFailureEvidence> evidence;
    TaskQueueFailureActivity lastActivity;
    std::string suggestedAction; // retry / retry_after / raise_timeout / fix_spec / escalate / human
    std::string retryAt;         // ISO 8601 local time, empty when not applicable
    TaskQueueFailureLlmGuess llmGuess; // reference only; empty unless an escalate answered
    bool Empty() const { return failureClass.empty(); }
};

// One rule from <data-dir>/failure-rules.json (doc/design-v3.md 4.4). Rules are evaluated
// in file order before the built-in chain, so an external rule can add a class or take
// precedence over a built-in one with the same class. Every condition in `when` must hold.
struct TaskQueueFailureRule {
    std::string failureClass;
    std::string confidence = "high";
    std::string summary;
    std::string suggestedAction;
    nlohmann::json detail = nlohmann::json::object();
    std::vector<int> exitCodes;
    std::vector<std::string> jobNames;   // case-insensitive exact match
    std::vector<std::string> taskqueues; // case-insensitive exact match
    std::vector<std::string> endKinds;
    std::string logRegex;   // last matching log line becomes the evidence
    std::string eventRegex; // matched against normalized event data (or its message)
    std::string eventKind;  // restrict eventRegex (or use alone) to this kind
    bool launchFailedSet = false;
    bool launchFailed = false;
    bool cancelledSet = false;
    bool cancelled = false;
    bool interruptedSet = false;
    bool interrupted = false;
    bool HasConditions() const {
        return !exitCodes.empty() || !jobNames.empty() || !taskqueues.empty() ||
               !endKinds.empty() || !logRegex.empty() || !eventRegex.empty() ||
               !eventKind.empty() || launchFailedSet || cancelledSet || interruptedSet;
    }
};

// Everything the classifier may consult. Keeping the inputs explicit makes the rules
// deterministic and lets tests reproduce a diagnostic without running a process.
struct TaskQueueFailureInput {
    std::string jobId;
    std::string jobName;
    std::string taskqueue;
    int attempt = 1;
    int maxAttempts = 1;
    bool launchFailed = false; // the process could not be started
    bool interrupted = false;  // Hancho restarted while the attempt was running
    bool cancelled = false;    // a caller stopped the job
    std::string endKind;       // completed, failed, timed_out, idle_timed_out, killed
    int exitCode = 0;
    int expectedExitCode = 0;
    bool success = false;      // EvaluateSuccess result
    std::string checkDetail;   // EvaluateSuccess detail
    bool filesNewerConfigured = false;
    bool outputRegexConfigured = false;
    int timeoutSec = 0;
    int idleOutputSec = 0;
    int idleCpuSec = 0;
    int64_t elapsedSec = 0;
    std::string blockedBy;     // dependency job id or resource holder
    std::string blockKind;     // "dependency" or "resource" ("" for an attempt failure)
    std::string blockedResource;
    std::string lastActivity;  // activity from the process snapshot when there are no events
    std::string lastActivityTarget;
    int64_t lastActivitySinceSec = -1;
    std::string eventsFile;    // normalized proc events file
    std::vector<nlohmann::json> events;
    std::string logFile;       // attempt log file
    std::string logText;       // tail of the attempt log
    int logFirstLine = 1;      // 1-based number of logText's first line
    int logLastLine = 0;       // 1-based number of the log's last line
    std::string fallbackEventsFile; // task queue events file used when no log/event exists
    uint64_t fallbackEventSeq = 0;
    int64_t nowEpochSec = 0;   // clocks are an input too, so tests stay deterministic
    // Rules loaded from <data-dir>/failure-rules.json. Kept as an explicit input so the
    // classifier itself stays pure and deterministic.
    std::vector<TaskQueueFailureRule> externalRules;
    // Optional answer already returned by an escalate node. It is applied only when the
    // deterministic rules leave the failure undecided and is stored as advisory only.
    std::string llmGuessJson;
};

// <data-dir>/failure-rules.json. A missing or malformed file yields no rules.
std::string TaskQueueFailureRulesPath();
std::vector<TaskQueueFailureRule> LoadTaskQueueFailureRules(const std::string& path);

// Deterministic, ordered rules. Returns a record with suggestedAction and evidence filled in.
TaskQueueFailureDiagnostic ClassifyTaskQueueFailure(const TaskQueueFailureInput& input);
// The JSON shape stored on an attempt and attached to job events (doc/design-v3.md 4.3).
std::string TaskQueueFailureDiagnosticToJson(const TaskQueueFailureDiagnostic& diagnostic);

// True when the deterministic rules left the failure undecided (unknown / low confidence).
bool TaskQueueFailureNeedsLlmGuess(const TaskQueueFailureDiagnostic& diagnostic);
// Builds the evidence-only packet handed to an existing `escalate` node. Returns "" when
// the deterministic rules already decided. The packet never includes instructions, env,
// command text or anything else the LLM should not see (doc/design-v3.md 4.4).
std::string BuildTaskQueueFailureEscalationPacket(const TaskQueueFailureInput& input,
                                                  const TaskQueueFailureDiagnostic& diagnostic);
// Stores an escalate answer in the advisory `llmGuess` field with confidence "low".
// It never changes failureClass, confidence, suggestedAction or retryAt. Invalid or
// inapplicable answers leave the diagnostic untouched and return false.
bool ApplyTaskQueueFailureLlmGuess(TaskQueueFailureDiagnostic& diagnostic,
                                   const std::string& answerJson, std::string* outError = nullptr);

struct TaskQueueAttempt {
    int n = 1;
    int processId = 0;
    // Set for a job dispatched to a remote host. processId is then unused and
    // remoteProcessId is the process id on that host.
    std::string remoteHost;
    int remoteProcessId = 0;
    // Remote job tracking (doc/design-v3.md 2.5, 056-1).
    std::string remoteJobId;
    int remoteAttempt = 0;
    std::string start;
    std::string end;
    std::string reason;
    int exitCode = -1;
    int instructionsRev = 1;
    std::string logFile;
    std::string lastLinesFile;
    std::string checkDetail;
    // Memory ids handed to this attempt (memory_brief). Empty when injection is off.
    std::vector<std::string> memoryRefs;
    // Per-attempt diagnosis (doc/design-v3.md 4.3). Empty for a succeeded attempt.
    TaskQueueFailureDiagnostic failure;
    // Transient (not persisted): launch wall clock used by filesNewer checks.
    uint64_t startFiletime = 0;
    // Transient (not persisted): seq of the jobStarted event, used as launch_error evidence.
    uint64_t startEventSeq = 0;
    // Transient (not persisted): last time a remote host was polled.
    uint64_t lastRemotePollMs = 0;
    // Transient (not persisted): set to true when loaded on restart while running,
    // indicating reconnect is pending.
    bool needsRemoteReconnect = false;
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

// Retry policy by failure class (doc/design-v3.md 4.5). When the job omits "retry",
// configured stays false and every failure retries up to maxAttempts (legacy behaviour).
// With "retry", "never" wins over "on"; an empty "on" allows every class that is not in
// "never". rate_limited waits for the reset like TaskQueueDefer and does not use up
// maxAttempts while waitForRateLimit is on.
struct TaskQueueRetry {
    bool configured = false;
    std::vector<std::string> on;
    std::vector<std::string> never;
    bool waitForRateLimit = true;
};

enum class TaskQueueRetryAction {
    Retry,            // start the next attempt after retryDelaySec
    WaitForRateLimit, // back to pending until the rate-limit reset, without using an attempt
    GiveUp,           // the job is failed
};

struct TaskQueueRetryDecision {
    TaskQueueRetryAction action = TaskQueueRetryAction::GiveUp;
    bool consumesAttempt = true;
};

// Memory injection (doc/design-v3.md 3.9). Off unless inject is true.
struct TaskQueueMemory {
    bool inject = false;
    int budgetChars = 1500;
    std::vector<std::string> scopes;
    bool includeAgentNotes = true;
    bool empty() const { return !inject; }
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

// Minimal state of an execution outside a task queue (doc/design-v3.md §7, card 072-1).
struct PlainRunState {
    std::string jobId;
    int attempt = 1;
    std::string taskqueue = "plain";
    std::string name;
    std::string summary;
    std::string command;
    std::string cwd;
    std::string start;
    std::string end;
    int exitCode = -1;
    std::string reason;
    int processId = 0;
    std::string logFile;
    std::string lastLinesFile;
    std::string checkDetail;
};

// Generates a unique jobId for a standalone run (e.g. "j_plain_<name>_<pid>_<seq>").
inline std::string IssuePlainJobId(int processId = 0, const std::string& name = "") {
    static std::atomic<uint64_t> s_plainSeq{1};
    uint64_t seq = s_plainSeq.fetch_add(1);
    std::string base = "j_plain_";
    if (!name.empty() && name != "plain") {
        base += SanitizeFileNameComponent(name) + "_";
    }
    if (processId > 0) {
        base += std::to_string(processId) + "_";
    }
    base += std::to_string(seq);
    return base;
}

// Persists a plain run's state directly into logs/taskqueues/<taskqueue>.json without
// requiring an active TaskQueueManager instance.
inline bool SavePlainRunStateDirect(const PlainRunState& state, std::string& outError) {
    outError.clear();
    std::string qName = state.taskqueue.empty() ? "plain" : state.taskqueue;
    std::wstring tqDir = PathJoinWide(GetLogsDirectoryWide(), L"taskqueues");
    EnsureDirectoryWide(tqDir);
    std::wstring filePath = PathJoinWide(tqDir, Utf8ToWide(SanitizeFileNameComponent(qName)) + L".json");

    nlohmann::json root;
    std::string existing = ReadFileUtf8Wide(filePath);
    if (!existing.empty()) {
        root = nlohmann::json::parse(existing, nullptr, false);
    }
    if (root.is_discarded() || !root.is_object()) {
        root = nlohmann::json::object();
        root["name"] = qName;
        root["concurrency"] = 0;
        root["paused"] = false;
        root["version"] = 1;
        root["notify_file"] = "";
        root["max_timeout_sec"] = 0;
        root["default_max_attempts"] = 1;
        root["jobs"] = nlohmann::json::array();
    }

    std::string jobId = state.jobId;
    if (jobId.empty()) {
        jobId = IssuePlainJobId(state.processId, state.name);
    }

    std::string startTs = !state.start.empty() ? state.start : GetCurrentTimestampStr();
    std::string endTs = !state.end.empty() ? state.end : GetCurrentTimestampStr();
    bool ok = (state.exitCode == 0);

    nlohmann::json attObj;
    attObj["n"] = state.attempt <= 0 ? 1 : state.attempt;
    attObj["process_id"] = state.processId;
    attObj["start"] = startTs;
    attObj["end"] = endTs;
    attObj["reason"] = !state.reason.empty() ? state.reason : (ok ? "ok" : "failed");
    attObj["exit_code"] = state.exitCode;
    attObj["instructions_rev"] = 1;
    if (!state.logFile.empty()) attObj["log_file"] = state.logFile;
    attObj["last_lines_file"] = state.lastLinesFile;
    attObj["check_detail"] = state.checkDetail;

    if (!root.contains("jobs") || !root["jobs"].is_array()) {
        root["jobs"] = nlohmann::json::array();
    }
    bool found = false;
    for (auto& j : root["jobs"]) {
        if (j.is_object() && j.value("id", "") == jobId) {
            found = true;
            j["state"] = ok ? "succeeded" : "failed";
            j["updated_at"] = endTs;
            if (!state.command.empty()) j["command"] = state.command;
            if (!state.cwd.empty()) j["cwd"] = state.cwd;
            if (!state.summary.empty()) j["summary"] = state.summary;
            if (!j.contains("attempts") || !j["attempts"].is_array()) {
                j["attempts"] = nlohmann::json::array();
            }
            j["attempts"].push_back(attObj);
            break;
        }
    }
    if (!found) {
        nlohmann::json jobObj;
        jobObj["id"] = jobId;
        jobObj["taskqueue"] = qName;
        jobObj["name"] = !state.name.empty() ? state.name : "plain";
        jobObj["summary"] = state.summary;
        jobObj["instructions"] = {{"source", ""}, {"rev", 1}, {"sha256", ""}, {"text", ""}, {"captured_at", ""}};
        jobObj["command"] = state.command;
        jobObj["cwd"] = state.cwd;
        jobObj["host"] = "";
        jobObj["env"] = nlohmann::json::object();
        jobObj["depends_on"] = nlohmann::json::array();
        jobObj["timeout_sec"] = 0;
        jobObj["estimate_sec"] = 0;
        jobObj["idle"] = {{"output_sec", 0}, {"cpu_sec", 0}};
        jobObj["max_attempts"] = 1;
        jobObj["retry_delay_sec"] = 0;
        jobObj["defer"] = {{"exit_codes", nlohmann::json::array()}, {"delay_sec", 60}, {"limit", 60}};
        jobObj["success"] = {{"exit_code", 0}, {"files_newer", nlohmann::json::array()}, {"output_regex", ""}, {"test_report", ""}};
        jobObj["kill_exclude"] = nlohmann::json::array();
        jobObj["resources"] = {{"write", nlohmann::json::array()}, {"read", nlohmann::json::array()}};
        jobObj["memory"] = {{"inject", false}, {"budget_chars", 1500}, {"scopes", nlohmann::json::array()}, {"include_agent_notes", true}};
        jobObj["priority"] = 0;
        jobObj["state"] = ok ? "succeeded" : "failed";
        jobObj["blocked_by"] = nullptr;
        jobObj["created_at"] = startTs;
        jobObj["updated_at"] = endTs;
        jobObj["notes"] = "";
        jobObj["attempts"] = nlohmann::json::array({attObj});
        root["jobs"].push_back(std::move(jobObj));
    }

    int version = root.value("version", 0) + 1;
    root["version"] = version;

    std::string text = root.dump(2) + "\r\n";
    WriteFileAtomicWide(filePath, text);

    if (!state.logFile.empty()) {
        std::wstring dstLog = PathJoinWide(GetLogsDirectoryWide(), L"taskqueue");
        EnsureDirectoryWide(dstLog);
        dstLog = PathJoinWide(dstLog, Utf8ToWide(SanitizeFileNameComponent(jobId)) + L"." +
                                      std::to_wstring(attObj["n"].get<int>()) + L".log");
        std::error_code ec;
        if (!std::filesystem::exists(std::filesystem::path(dstLog), ec) &&
            std::filesystem::exists(std::filesystem::path(Utf8ToWide(state.logFile)), ec)) {
            std::filesystem::copy_file(std::filesystem::path(Utf8ToWide(state.logFile)),
                                       std::filesystem::path(dstLog),
                                       std::filesystem::copy_options::overwrite_existing, ec);
        }
    }
    return true;
}

struct TaskQueueJob {
    std::string id;
    std::string taskqueue;
    int num = 0; // transient numeric id (process correlation)
    std::string name;
    std::string summary;
    TaskQueueInstructions instructions;
    std::string command;
    std::string cwd;
    // Optional remote host name from <data-dir>/hosts.json. Empty runs locally.
    std::string host;
    std::map<std::string, std::string> env;
    std::vector<TaskQueueDependency> dependsOn;
    int timeoutSec = 0;
    int estimateSec = 0;  // optional expected duration, used for ETA display (0 = use timeoutSec)
    TaskQueueIdle idle;
    int maxAttempts = 2;
    int retryDelaySec = 0;
    TaskQueueRetry retry;
    TaskQueueDefer defer;
    TaskQueueMemory memory;
    TaskQueueSuccess success;
    std::vector<std::string> killExclude;
    TaskQueueResources resources;
    int priority = 0;
    // Internal jobs (for example the Chat handler) stay in the queue but are hidden from lists.
    bool hidden = false;
    // Armed by `hancho-cli power suspend --after-job <id>` or a job's suspendWhenDone.
    // The core suspends after this job finishes and all other work becomes idle.
    bool suspendWhenDone = false;
    int suspendDelaySec = 0;       // 0 = default grace period
    bool suspendHibernate = false; // false = sleep
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

// ---------------------------------------------------------------------------
// One-shot CI runs (doc/design-v3.md 2.3)
//
// `hancho-cli run --file queue.json --exit-on-drain --report junit.xml` waits until the
// queue has no pending/running jobs, then maps the final states to one exit code and a
// JUnit report. These helpers are pure so the mapping can be tested without a server.
// ---------------------------------------------------------------------------

struct TaskQueueRunJob {
    std::string id;
    std::string name;
    std::string summary;
    std::string state; // succeeded, failed, cancelled, blocked, pending, running
    std::string reason; // last attempt reason (failed, timed_out, interrupted, ...)
    int exitCode = -1;
    std::string failureClass;
    std::string failureSummary;
    std::string blockedBy;
    bool interrupted = false;

    bool Unfinished() const {
        return state.empty() || state == "pending" || state == "running";
    }
    bool Cancelled() const {
        return state == "cancelled" || (interrupted && state != "failed" && state != "succeeded");
    }
    bool Failed() const { return state == "failed" || state == "blocked"; }
};

inline bool TaskQueueRunDrained(const std::vector<TaskQueueRunJob>& jobs) {
    for (const auto& job : jobs) {
        if (job.Unfinished()) return false;
    }
    return true;
}

// 0 = every job succeeded (an empty queue counts as success)
// 1 = at least one job failed or was blocked
// 2 = a job was cancelled, or an interrupted attempt left it unfinished
inline int TaskQueueRunExitCode(const std::vector<TaskQueueRunJob>& jobs) {
    bool failure = false;
    for (const auto& job : jobs) {
        if (job.Cancelled()) return 2;
        if (job.Failed() || job.Unfinished()) failure = true;
    }
    return failure ? 1 : 0;
}

inline std::string TaskQueueRunXmlEscape(const std::string& text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) {
        switch (c) {
        case '&': out += "&amp;"; break;
        case '<': out += "&lt;"; break;
        case '>': out += "&gt;"; break;
        case '"': out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default: out += c; break;
        }
    }
    return out;
}

inline std::string TaskQueueRunFailureMessage(const TaskQueueRunJob& job) {
    std::ostringstream oss;
    if (job.state == "blocked") {
        oss << "blocked";
        if (!job.blockedBy.empty()) oss << " by " << job.blockedBy;
    } else {
        oss << (job.state.empty() ? "unfinished" : job.state);
        if (!job.reason.empty()) oss << ": reason=" << job.reason;
    }
    if (job.exitCode != -1) oss << " exitCode=" << job.exitCode;
    if (!job.failureClass.empty()) oss << " class=" << job.failureClass;
    if (!job.failureSummary.empty()) oss << ": " << job.failureSummary;
    return oss.str();
}

// Minimal JUnit XML: one testcase per job. Failed/blocked jobs get <failure message>,
// cancelled or unfinished jobs get <skipped message>. Everything is escaped.
inline std::string TaskQueueRunJUnitXml(const std::string& suiteName,
                                        const std::vector<TaskQueueRunJob>& jobs) {
    size_t failures = 0;
    size_t skipped = 0;
    for (const auto& job : jobs) {
        if (job.Cancelled() || job.Unfinished()) ++skipped;
        else if (job.Failed()) ++failures;
    }
    std::ostringstream oss;
    oss << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
    oss << "<testsuites name=\"" << TaskQueueRunXmlEscape(suiteName) << "\""
        << " tests=\"" << jobs.size() << "\" failures=\"" << failures << "\""
        << " errors=\"0\" skipped=\"" << skipped << "\" time=\"0.000\">\n";
    oss << "  <testsuite name=\"" << TaskQueueRunXmlEscape(suiteName) << "\""
        << " tests=\"" << jobs.size() << "\" failures=\"" << failures << "\""
        << " errors=\"0\" skipped=\"" << skipped << "\" time=\"0.000\">\n";
    for (const auto& job : jobs) {
        const std::string name = job.id.empty() ? job.name : job.id;
        oss << "    <testcase classname=\"" << TaskQueueRunXmlEscape(suiteName) << "\""
            << " name=\"" << TaskQueueRunXmlEscape(name) << "\" time=\"0.000\"";
        if (job.state == "succeeded") {
            oss << "/>\n";
            continue;
        }
        const std::string message = TaskQueueRunXmlEscape(TaskQueueRunFailureMessage(job));
        if (job.Cancelled() || job.Unfinished()) {
            oss << ">\n      <skipped message=\"" << message << "\"/>\n    </testcase>\n";
        } else {
            oss << ">\n      <failure message=\"" << message << "\">" << message
                << "</failure>\n    </testcase>\n";
        }
    }
    oss << "  </testsuite>\n</testsuites>\n";
    return oss.str();
}

inline TaskQueueRunJob TaskQueueRunJobFromJson(const nlohmann::json& job) {
    TaskQueueRunJob out;
    if (!job.is_object()) return out;
    out.id = job.value("id", std::string());
    out.name = job.value("name", out.id);
    out.summary = job.value("summary", std::string());
    out.state = job.value("state", std::string());
    if (job.contains("blocked_by") && job["blocked_by"].is_string()) {
        out.blockedBy = job["blocked_by"].get<std::string>();
    }
    if (job.contains("attempts") && job["attempts"].is_array() && !job["attempts"].empty()) {
        const auto& attempt = job["attempts"].back();
        if (attempt.is_object()) {
            out.reason = attempt.value("reason", std::string());
            if (attempt.contains("exit_code") && attempt["exit_code"].is_number()) {
                out.exitCode = attempt["exit_code"].get<int>();
            }
            const auto failure = attempt.find("failure");
            if (failure != attempt.end() && failure->is_object()) {
                out.failureClass = failure->value("class", std::string());
                out.failureSummary = failure->value("summary", std::string());
            }
        }
    }
    // An interrupted attempt is re-queued. Only surface it as an interruption when it
    // did not later become a normal success or failure.
    out.interrupted = out.reason == "interrupted" && out.state != "succeeded" && out.state != "failed";
    return out;
}

// Deterministic retry decision for one classified failure (doc/design-v3.md 4.5).
// countedAttempts excludes interrupted/deferred attempts. allowRateLimitWait=false is
// used once defer.limit has been reached, so the class counts as an ordinary failure.
TaskQueueRetryDecision DecideTaskQueueRetry(const TaskQueueJob& job, const std::string& failureClass,
                                            int countedAttempts, bool allowRateLimitWait = true);

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

// Remote dispatch (doc/design-v3.md 2.5). The IPC layer installs these; the task
// queue itself stays free of networking so it can be tested headlessly and the
// same scheduler works on Windows and POSIX. Without callbacks a job with a
// host fails to launch instead of silently running locally.
struct RemotePollResult {
    bool finished = false;
    int exitCode = -1;
    std::string endKind = "completed";
    std::string output;      // mirrored into the local attempt log
    std::string eventsJsonl; // mirrored into <attempt>.events.jsonl
    std::string doneContent; // mirrored into the local .done file
};

using RemoteStartCallback = std::function<int(const std::string& host,
                                              const StartProcessRequest& request,
                                              std::string& outError)>;
using RemotePollCallback = std::function<bool(const std::string& host, int remoteProcessId,
                                              RemotePollResult& outResult, std::string& outError)>;
using RemoteKillCallback = std::function<bool(const std::string& host, int remoteProcessId,
                                              std::string& outError)>;

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
    // Invoked on the scheduler thread when a flagged job and all other work are idle.
    using SuspendCallback = std::function<void(const std::string& jobId, const std::string& state,
                                               bool hibernate)>;

    void SetProcessFinishedCallback(ProcessFinishedCallback cb);
    void SetJobFinishedCallback(JobFinishedCallback cb);
    void SetBtRetryCallback(BtRetryCallback cb);
    void SetSuspendCallback(SuspendCallback cb);
    void SetProcessManager(ProcessManager* pm);
    // Installed by IpcServer. Passing empty callbacks makes remote jobs fail to launch.
    void SetRemoteCallbacks(RemoteStartCallback start, RemotePollCallback poll,
                            RemoteKillCallback kill);
    // Reconnect to remote jobs that were running when Hancho restarted (doc/design-v3.md 2.5).
    void ReconnectRemoteJobs();

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
                              const std::string& origin = "", const std::string& btId = "",
                              bool includeHidden = false);
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

    // Suspend-after-job support (hancho-cli power suspend / MCP power_suspend).
    // Arming a terminal job starts the countdown as soon as all other work is idle.
    std::string SetJobSuspendWhenDone(const std::string& id, bool hibernate, int delaySec,
                                      std::string& outError);
    std::string ClearSuspendWhenDone(std::string& outError);
    std::string SuspendStatusJson() const;

    // Plain runs outside a taskqueue (doc/design-v3.md §7, card 072-1).
    static std::string IssuePlainJobId(int processId = 0, const std::string& name = "") {
        return pc::IssuePlainJobId(processId, name);
    }
    bool RecordPlainRun(const PlainRunState& state, std::string& outError);
    bool RecordPlainRunLocked(const PlainRunState& state, std::string& outError);

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

    // Polls running jobs on remote hosts (network I/O happens without mutex_) and
    // finishes the attempts whose remote process is done.
    void PollRemoteJobs();

    std::string JobToJson(const TaskQueueJob& job, bool includeInstructions = true) const;

private:
    struct LaunchItem {
        std::string taskqueue;
        std::string jobId;
        int attemptN = 0;
        std::vector<std::string> memoryRefs;
        StartProcessRequest req;
    };

    void LoadQueues();
    void SaveQueue(const TaskQueue& tq);
    void SaveAllLocked();
    void SchedulerLoop();
    void ProcessSchedulerTick();
    void FlushFinishedCallbacks();
    void CheckSuspendWhenDone();
    bool HasActiveOtherWork();

    TaskQueueJob* FindJob(const std::string& id, TaskQueue** outQueue);
    const TaskQueueJob* FindJobConst(const std::string& id) const;
    // Requeues one job (caller holds mutex_, has validated the state, and saves the queue).
    void RequeueJobLocked(TaskQueue& tq, TaskQueueJob& job, bool resetAttempts, bool refreshInstructions);

    void ApplyAttemptOutcome(TaskQueue& tq, TaskQueueJob& job, bool success,
                             const std::string& reason, int exitCode, const std::string& checkDetail,
                             const TaskQueueFailureInput* failureInput = nullptr);
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
    // resource_wait text for a job ("holder: resource" or "holder (queued ahead): resource"); empty when none.
    // Only pending jobs can wait. allSat: all dependencies are satisfied. Caller holds mutex_.
    std::string ResourceWaitLocked(const TaskQueueJob& job, bool allSat, int64_t nowMs) const;
    bool IsReadyLocked(const TaskQueueJob& job, int64_t nowMs) const;
    static bool RunsBefore(const TaskQueueJob& a, const TaskQueueJob& b);
    bool HasDependencyCycle(const TaskQueue& tq, const std::string& jobId,
                            const std::vector<TaskQueueDependency>& deps,
                            const std::string& ignoreDependency);
    bool BuildLaunchRequest(TaskQueue& tq, TaskQueueJob& job, int attemptN, LaunchItem& out);
    bool FinishRemoteAttemptLocked(TaskQueue& tq, TaskQueueJob& job, const RemotePollResult& result);
    bool EvaluateSuccess(const TaskQueueJob& job, TaskQueueAttempt& attempt,
                         const std::string& endKind, int exitCode,
                         const std::string& logFile, std::string& detail);

    void EmitEventLocked(TaskQueue& tq, const std::string& type, const std::string& jobId, int64_t attempt,
                         const TaskQueueFailureDiagnostic* failure = nullptr);
    static std::string BuildError(const std::string& msg);

    ProcessManager* pm_ = nullptr;
    RemoteStartCallback remoteStartCb_;
    RemotePollCallback remotePollCb_;
    RemoteKillCallback remoteKillCb_;
    ProcessFinishedCallback finishedCb_;
    JobFinishedCallback jobFinishedCb_;
    // Job-finished callbacks running right now (guarded by mutex_). SetJobFinishedCallback(nullptr)
    // waits for this to reach 0 so the callback's owner can be destroyed safely afterwards.
    int jobFinishedCbInFlight_ = 0;
    std::condition_variable jobFinishedCbIdleCv_;
    BtRetryCallback btRetryCb_;
    SuspendCallback suspendCb_;
    // Suspend monitor state, guarded by mutex_.
    std::string suspendPhase_; // empty/idle, waiting_target, waiting_others, countdown
    int64_t suspendDueMs_ = 0;
    int suspendDelaySec_ = 0;
    bool suspendHibernate_ = false;
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

// ---------------------------------------------------------------------------
// Automatic L2 notes written by Hancho (doc/design-v3.md 3.4)
//
// Evaluates 3 deterministic rules after each attempt finishes:
// 1. Same job name failed 3 times consecutively with the same failure classification.
// 2. Duration exceeds 80% of timeoutSec across consecutive attempts.
// 3. Success after failure: records diff of filesEdited as candidate fix files.
// Returns the number of new notes written to memory/notes.jsonl.
// ---------------------------------------------------------------------------
int CheckAndRecordAutoNotes(const TaskQueueJob& job, const TaskQueueAttempt& attempt,
                           const TaskQueueFailureInput& fin);
int CheckAndRecordAutoNotes(const std::string& jobName, int timeoutSec = 0);

// ---------------------------------------------------------------------------
// Memory injection effect (doc/design-v3.md 3.12)
//
// Joins L1 run records (memory/records/*.jsonl) with the memoryRefs recorded on
// task queue attempts, then compares runs with and without injected memory:
// success rate, retry success rate, failure-class recurrence and token/cost deltas.
// Header-only on purpose: hancho-cli does not link taskqueue.cpp, and the GUI
// needs the same numbers. Filters are optional; empty means "all".
// ---------------------------------------------------------------------------
namespace memory_inject_detail {

inline std::string LowerAsciiCopy(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

inline bool IEquals(const std::string& a, const std::string& b) {
    return LowerAsciiCopy(a) == LowerAsciiCopy(b);
}

inline int64_t ParseIsoEpoch(const std::string& s) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    char sep = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d%c%d:%d:%d", &y, &mo, &d, &sep, &h, &mi, &se) < 7) return 0;
    if (sep != 'T' && sep != ' ') return 0;
    std::tm tmv = {};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = se;
    return static_cast<int64_t>(_mkgmtime64(&tmv));
}

inline int64_t SinceEpoch(const std::string& since) {
    if (since.empty()) return 0;
    int64_t secs = 0;
    if (ParseDurationToSec(since, secs)) {
        return static_cast<int64_t>(std::time(nullptr)) - secs;
    }
    int y = 0, mo = 0, d = 0;
    if (std::sscanf(since.c_str(), "%d-%d-%d", &y, &mo, &d) == 3) {
        std::tm tmv = {};
        tmv.tm_year = y - 1900;
        tmv.tm_mon = mo - 1;
        tmv.tm_mday = d;
        return static_cast<int64_t>(_mkgmtime64(&tmv));
    }
    return 0;
}

inline std::string DumpBucket(const std::string& name, int64_t runs, int64_t successes,
                              int64_t retries, int64_t retrySuccesses,
                              int64_t repeatOpps, int64_t repeats,
                              int64_t tokens, double cost) {
    nlohmann::json o;
    o["runs"] = runs;
    o["successes"] = successes;
    o["successRate"] = runs > 0 ? std::round((static_cast<double>(successes) / runs) * 10000.0) / 10000.0 : 0.0;
    o["retries"] = retries;
    o["retrySuccesses"] = retrySuccesses;
    o["retrySuccessRate"] = retries > 0 ? std::round((static_cast<double>(retrySuccesses) / retries) * 10000.0) / 10000.0 : 0.0;
    o["failureRepeats"] = repeats;
    o["failureRepeatOpportunities"] = repeatOpps;
    o["failureRepeatRate"] = repeatOpps > 0 ? std::round((static_cast<double>(repeats) / repeatOpps) * 10000.0) / 10000.0 : 0.0;
    o["totalTokens"] = tokens;
    o["avgTokensPerRun"] = runs > 0 ? std::round((static_cast<double>(tokens) / runs) * 100.0) / 100.0 : 0.0;
    o["costUsd"] = std::round(cost * 10000.0) / 10000.0;
    o["avgCostPerRun"] = runs > 0 ? std::round((cost / runs) * 10000.0) / 10000.0 : 0.0;
    o["name"] = name;
    return o.dump();
}

} // namespace memory_inject_detail

inline std::string MemoryInjectCompareJson(const std::string& jobName,
                                           const std::string& taskqueue,
                                           const std::string& since) {
    using json = nlohmann::json;
    namespace fs = std::filesystem;
    using memory_inject_detail::IEquals;
    using memory_inject_detail::ParseIsoEpoch;
    using memory_inject_detail::SinceEpoch;

    try {
        const int64_t sinceEpoch = SinceEpoch(since);
        std::error_code ec;

        // jobId|attempt -> whether that attempt carried injected memory ids.
        std::map<std::string, bool> refByAttempt;
        fs::path tqDir = fs::path(GetEffectiveDataDirectoryWide()) / L"taskqueues";
        if (fs::is_directory(tqDir, ec)) {
            for (const auto& ent : fs::directory_iterator(tqDir, ec)) {
                if (ec) break;
                if (!ent.is_regular_file(ec)) continue;
                if (ent.path().extension() != L".json") continue;
                json tq = json::parse(ReadFileUtf8Wide(ent.path().wstring()), nullptr, false);
                if (tq.is_discarded() || !tq.is_object()) continue;
                auto jobs = tq.find("jobs");
                if (jobs == tq.end() || !jobs->is_array()) continue;
                for (const auto& job : *jobs) {
                    if (!job.is_object()) continue;
                    std::string id = job.value("id", std::string());
                    auto attempts = job.find("attempts");
                    if (attempts == job.end() || !attempts->is_array()) continue;
                    for (const auto& a : *attempts) {
                        if (!a.is_object()) continue;
                        int n = a.value("n", 0);
                        auto refs = a.find("memory_refs");
                        bool has = refs != a.end() && refs->is_array() && !refs->empty();
                        refByAttempt[id + "|" + std::to_string(n)] = has;
                    }
                }
            }
        }

        struct Rec {
            std::string jobId;
            std::string failureClass;
            std::string outcome;
            int attempt = 0;
            bool injected = false;
            bool success = false;
            int64_t tokens = 0;
            double cost = 0.0;
            int64_t epoch = 0;
        };
        std::vector<Rec> recs;
        int64_t unmatched = 0;

        fs::path recDir = fs::path(GetEffectiveDataDirectoryWide()) / L"memory" / L"records";
        if (fs::is_directory(recDir, ec)) {
            for (const auto& ent : fs::directory_iterator(recDir, ec)) {
                if (ec) break;
                if (!ent.is_regular_file(ec)) continue;
                if (ent.path().extension() != L".jsonl") continue;
                std::ifstream in(ent.path(), std::ios::binary);
                std::string line;
                while (std::getline(in, line)) {
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.empty()) continue;
                    json j = json::parse(line, nullptr, false);
                    if (j.is_discarded() || !j.is_object()) continue;
                    if (!jobName.empty() && !IEquals(j.value("jobName", std::string()), jobName)) continue;
                    if (!taskqueue.empty() && !IEquals(j.value("taskqueue", std::string()), taskqueue)) continue;

                    Rec r;
                    r.jobId = j.value("jobId", std::string());
                    r.outcome = j.value("outcome", std::string());
                    r.attempt = j.value("attempt", 0);
                    r.epoch = ParseIsoEpoch(j.value("start", std::string()));
                    if (sinceEpoch > 0 && r.epoch > 0 && r.epoch < sinceEpoch) continue;

                    auto failure = j.find("failure");
                    if (failure != j.end() && failure->is_object()) {
                        r.failureClass = failure->value("class", std::string());
                    }
                    auto usage = j.find("usage");
                    if (usage != j.end() && usage->is_object()) {
                        r.tokens = usage->value("totalTokens", static_cast<int64_t>(0));
                        r.cost = usage->value("costUsd", 0.0);
                    }
                    if (r.tokens == 0) r.tokens = j.value("totalTokens", static_cast<int64_t>(0));

                    std::string lo = memory_inject_detail::LowerAsciiCopy(r.outcome);
                    bool hasExit = j.contains("exitCode") && j["exitCode"].is_number_integer();
                    int64_t exitCode = hasExit ? j["exitCode"].get<int64_t>() : 0;
                    r.success = lo == "success" || lo == "succeeded" || lo == "completed" ||
                                lo == "ok" || lo == "passed" ||
                                (lo.empty() && hasExit && exitCode == 0);

                    auto ref = refByAttempt.find(r.jobId + "|" + std::to_string(r.attempt));
                    if (ref == refByAttempt.end()) {
                        ++unmatched;
                    } else {
                        r.injected = ref->second;
                    }
                    recs.push_back(r);
                }
            }
        }

        std::sort(recs.begin(), recs.end(), [](const Rec& a, const Rec& b) {
            if (a.jobId != b.jobId) return a.jobId < b.jobId;
            return a.attempt < b.attempt;
        });

        struct Bucket {
            int64_t runs = 0, successes = 0, retries = 0, retrySuccesses = 0;
            int64_t repeatOpps = 0, repeats = 0, tokens = 0;
            double cost = 0.0;
        };
        Bucket injected, plain;

        for (size_t i = 0; i < recs.size(); ++i) {
            const Rec& r = recs[i];
            Bucket& b = r.injected ? injected : plain;
            ++b.runs;
            if (r.success) ++b.successes;
            b.tokens += r.tokens;
            b.cost += r.cost;
            if (r.attempt > 1) {
                ++b.retries;
                if (r.success) ++b.retrySuccesses;
                if (i > 0 && recs[i - 1].jobId == r.jobId &&
                    recs[i - 1].attempt == r.attempt - 1 && !recs[i - 1].failureClass.empty()) {
                    ++b.repeatOpps;
                    if (IEquals(recs[i - 1].failureClass, r.failureClass)) ++b.repeats;
                }
            }
        }

        json injectedJson = json::parse(memory_inject_detail::DumpBucket(
            "injected", injected.runs, injected.successes, injected.retries, injected.retrySuccesses,
            injected.repeatOpps, injected.repeats, injected.tokens, injected.cost));
        json plainJson = json::parse(memory_inject_detail::DumpBucket(
            "notInjected", plain.runs, plain.successes, plain.retries, plain.retrySuccesses,
            plain.repeatOpps, plain.repeats, plain.tokens, plain.cost));
        injectedJson.erase("name");
        plainJson.erase("name");

        double tokenDelta = 0.0;
        double costDelta = 0.0;
        double successDelta = 0.0;
        double repeatDelta = 0.0;
        if (injected.runs > 0 && plain.runs > 0) {
            tokenDelta = injectedJson.value("avgTokensPerRun", 0.0) - plainJson.value("avgTokensPerRun", 0.0);
            costDelta = injectedJson.value("avgCostPerRun", 0.0) - plainJson.value("avgCostPerRun", 0.0);
            successDelta = injectedJson.value("successRate", 0.0) - plainJson.value("successRate", 0.0);
            repeatDelta = injectedJson.value("failureRepeatRate", 0.0) - plainJson.value("failureRepeatRate", 0.0);
        }

        json out;
        out["ok"] = true;
        out["compareInject"] = true;
        out["jobName"] = jobName;
        out["taskqueue"] = taskqueue;
        out["since"] = since;
        out["injected"] = injectedJson;
        out["notInjected"] = plainJson;
        out["tokenDeltaPerRun"] = std::round(tokenDelta * 100.0) / 100.0;
        out["costDeltaPerRun"] = std::round(costDelta * 10000.0) / 10000.0;
        out["successRateDelta"] = std::round(successDelta * 10000.0) / 10000.0;
        out["failureRepeatRateDelta"] = std::round(repeatDelta * 10000.0) / 10000.0;
        out["runsWithoutAttemptMatch"] = unmatched;
        out["insufficientData"] = injected.runs == 0 || plain.runs == 0;
        out["injectionDefault"] = "off";
        out["note"] = "A run counts as injected only when the matching task queue attempt recorded memory_refs. "
                      "Injection stays off by default until this comparison shows a benefit.";
        return out.dump();
    } catch (const std::exception& e) {
        json out;
        out["ok"] = false;
        out["error"] = std::string("memory compare failed: ") + e.what();
        return out.dump();
    }
}

} // namespace pc
