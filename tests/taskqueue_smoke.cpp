#include "../src/taskqueue.h"
#include "../src/chain_guard.h"
#include "../src/common.h"
#include "../src/llm_stream.h"
#include "../src/memory_search.h"
#include <windows.h>
#include <ctime>
#include <cstdio>
#include <string>

namespace pc {
using RemoteFilesCheckCallback = std::function<bool(
    const std::string& host,
    const std::string& cwd,
    int remoteProcessId,
    const std::vector<std::string>& files,
    uint64_t startFiletime,
    std::string& outDetail)>;

void SetRemoteFilesCheckCallback(RemoteFilesCheckCallback cb);
}

using namespace pc;

static int g_failures = 0;

static void Check(bool cond, const std::string& label) {
    std::printf("%s %s\n", cond ? "[PASS]" : "[FAIL]", label.c_str());
    if (!cond) g_failures++;
}

static nlohmann::json ParseJobJson(const std::string& raw) {
    auto j = nlohmann::json::parse(raw, nullptr, false);
    if (j.is_object() && j.contains("job") && j["job"].is_object()) {
        return j["job"];
    }
    return j;
}

static std::string GetState(TaskQueueManager& tqm, const std::string& id) {
    std::string err;
    std::string resp = tqm.TaskQueueGet(id, false, 0, err);
    return JsonGetString(resp, "state", "");
}

static bool WaitForState(TaskQueueManager& tqm, const std::string& id, const std::string& want, int timeoutMs) {
    uint64_t start = GetNowTickMs();
    while (static_cast<int>(GetNowTickMs() - start) < timeoutMs) {
        if (GetState(tqm, id) == want) return true;
        Sleep(100);
    }
    return GetState(tqm, id) == want;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    wchar_t tmp[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmp);
    // PID plus start tick: Windows reuses PIDs, and a reused folder holds the queues of an older run.
    std::wstring dir = std::wstring(tmp) + L"pc_tq_smoke_" + std::to_wstring(GetCurrentProcessId()) +
                       L"_" + std::to_wstring(GetNowTickMs());
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring env = L"HANCHO_LOG_DIR=" + dir;
    _wputenv(env.c_str());
    SetDataDirectoryOverride(dir);

    ProcessManager pm;
    TaskQueueManager tqm;
    tqm.SetProcessManager(&pm);
    pm.SetProcessFinishedCallback([&tqm](int pid, int ec, const std::string& kind) {
        tqm.OnProcessFinished(pid, ec, kind);
    });

    std::string guardMsg;
    Check(!CheckChainGuard("A && B", guardMsg), "chain guard rejects &&");
    Check(!CheckChainGuard("cmd /c \"A && B\"", guardMsg), "chain guard rejects cmd /c chain");
    Check(CheckChainGuard("A | B", guardMsg), "chain guard allows single pipe");
    Check(CheckChainGuard("run.exe \"x && y\"", guardMsg), "chain guard allows quoted operator");
    Check(!CheckChainGuard("while(-not (Test-Path X)){Start-Sleep 20}", guardMsg), "chain guard rejects wait loop");
    Check(!CheckChainGuard("powershell -Command \"a; b\"", guardMsg), "chain guard rejects powershell semicolon");

    // --- Failure classifier (doc/design-v3.md 4.2-4.4) -------------------------------------
    // Rules are ordered and deterministic; every record must point at an event seq or log line.
    {
        auto makeBase = []() {
            TaskQueueFailureInput in;
            in.jobId = "J1";
            in.jobName = "build";
            in.taskqueue = "q";
            in.attempt = 1;
            in.maxAttempts = 2;
            in.expectedExitCode = 0;
            in.logFile = "C:/logs/J1.1.log";
            in.logText = "starting\nbuilding\nfinished\n";
            in.logFirstLine = 100;
            in.logLastLine = 102;
            in.eventsFile = "C:/logs/proc_1_build.events.jsonl";
            in.fallbackEventsFile = "C:/logs/taskqueue-q.events.jsonl";
            in.fallbackEventSeq = 42;
            in.nowEpochSec = 1794288000;
            return in;
        };
        auto event = [](const std::string& kind, uint64_t seq, const std::string& data) {
            return nlohmann::json::parse("{\"schema\":\"hancho.event/1\",\"seq\":" + std::to_string(seq) +
                                             ",\"ts\":\"2026-11-10T09:00:00Z\",\"kind\":\"" + kind +
                                             "\",\"data\":" + data + "}",
                                         nullptr, false);
        };
        auto classified = [&](const TaskQueueFailureInput& in, const std::string& cls,
                              const std::string& action) {
            TaskQueueFailureDiagnostic d = ClassifyTaskQueueFailure(in);
            Check(d.failureClass == cls, "classifier " + cls + " (got " + d.failureClass + ")");
            if (!action.empty()) {
                Check(d.suggestedAction == action,
                      "classifier " + cls + " action " + action + " (got " + d.suggestedAction + ")");
            }
            Check(!d.evidence.empty(), "classifier " + cls + " has evidence");
            bool points = true;
            for (const auto& e : d.evidence) {
                if (e.IsEvent()) {
                    if (e.events.empty() || e.seq == 0) points = false;
                } else if (e.IsLog()) {
                    if (e.log.empty() || e.line <= 0) points = false;
                } else {
                    points = false;
                }
            }
            Check(points, "classifier " + cls + " evidence names a seq or log line");
            Check(TaskQueueFailureDiagnosticToJson(d) ==
                      TaskQueueFailureDiagnosticToJson(ClassifyTaskQueueFailure(in)),
                  "classifier " + cls + " is deterministic");
            return d;
        };

        {
            TaskQueueFailureInput in = makeBase();
            in.launchFailed = true;
            in.logFile.clear();
            in.logText.clear();
            in.logLastLine = 0;
            in.checkDetail = "CreateProcess failed";
            auto d = classified(in, "launch_error", "fix_spec");
            Check(d.evidence.size() == 1 && d.evidence[0].IsEvent() && d.evidence[0].seq == 42,
                  "launch_error evidence falls back to the jobStarted event");
        }
        {
            TaskQueueFailureInput in = makeBase();
            in.endKind = "timed_out";
            in.timeoutSec = 90;
            in.elapsedSec = 91;
            auto d = classified(in, "timeout", "raise_timeout");
            Check(d.evidence[0].IsLog() && d.evidence[0].line == 102, "timeout evidence is the last log line");
        }
        {
            TaskQueueFailureInput in = makeBase();
            in.endKind = "idle_timed_out";
            in.idleOutputSec = 60;
            in.idleCpuSec = 30;
            classified(in, "idle_stall", "retry");
        }
        {
            TaskQueueFailureInput in = makeBase();
            in.cancelled = true;
            in.endKind = "killed";
            classified(in, "killed", "human");
        }
        {
            TaskQueueFailureInput in = makeBase();
            in.interrupted = true;
            in.endKind = "killed";
            classified(in, "killed", "retry");
        }
        {
            TaskQueueFailureInput in = makeBase();
            in.endKind = "failed";
            in.exitCode = 1;
            in.events.push_back(event("rate_limit", 77,
                R"({"status":"rejected","window":"five_hour","utilization":1.0,"resetsAt":1794290000})"));
            auto d = classified(in, "rate_limited", "retry_after");
            Check(!d.retryAt.empty(), "rate_limited carries retryAt");
            Check(d.detail.value("window", std::string()) == "five_hour", "rate_limited detail names the window");
            Check(d.evidence[0].IsEvent() && d.evidence[0].seq == 77, "rate_limited evidence is the event seq");
        }
        {
            TaskQueueFailureInput in = makeBase();
            in.endKind = "failed";
            in.exitCode = 1;
            in.events.push_back(event("error", 78, R"({"message":"Invalid API key provided"})"));
            auto d = classified(in, "auth_error", "human");
            Check(d.evidence[0].IsEvent() && d.evidence[0].seq == 78, "auth_error evidence is the event seq");
        }
        {
            TaskQueueFailureInput in = makeBase();
            in.endKind = "completed";
            in.events.push_back(event("run.ended", 79, R"({"status":"error","permissionDenials":3})"));
            classified(in, "permission_denied", "fix_spec");
        }
        {
            TaskQueueFailureInput in = makeBase();
            in.endKind = "completed";
            in.events.push_back(event("run.ended", 80, R"({"status":"completed","stopReason":"max_turns"})"));
            classified(in, "turn_or_context_limit", "escalate");
        }
        {
            TaskQueueFailureInput in = makeBase();
            in.endKind = "failed";
            in.exitCode = static_cast<int>(0xC0000005u);
            auto d = classified(in, "crash", "retry");
            Check(d.detail.value("exitCodeHex", std::string()) == "0xC0000005", "crash detail names the exit code");
        }
        {
            TaskQueueFailureInput in = makeBase();
            in.endKind = "failed";
            in.exitCode = 1;
            for (int i = 0; i < 3; ++i) {
                std::string id = "t" + std::to_string(i);
                in.events.push_back(event("tool.call", 90 + i * 2,
                    "{\"tool\":\"Bash\",\"activity\":\"command\",\"toolUseId\":\"" + id + "\"}"));
                in.events.push_back(event("tool.result", 91 + i * 2,
                    "{\"ok\":false,\"toolUseId\":\"" + id + "\",\"error\":\"boom\"}"));
            }
            auto d = classified(in, "tool_error_loop", "escalate");
            Check(d.detail.value("tool", std::string()) == "Bash", "tool_error_loop names the tool");
        }
        {
            TaskQueueFailureInput in = makeBase();
            in.endKind = "completed";
            in.success = false;
            in.checkDetail = "missing file: report.md";
            in.events.push_back(event("plan", 96,
                R"({"items":[{"content":"a","status":"completed"},{"content":"b","status":"pending"}]})"));
            classified(in, "incomplete", "retry");
        }
        {
            TaskQueueFailureInput in = makeBase();
            in.endKind = "completed";
            in.success = false;
            in.checkDetail = "missing file: report.md";
            auto d = classified(in, "success_check_failed", "retry");
            Check(d.detail.value("condition", std::string()) == "filesNewer",
                  "success_check_failed names the failed condition");
        }
        {
            TaskQueueFailureInput in = makeBase();
            in.endKind = "failed";
            in.exitCode = 3;
            auto d = classified(in, "exit_nonzero", "retry");
            Check(d.detail.value("exitCode", 0) == 3, "exit_nonzero detail carries the exit code");
        }
        {
            TaskQueueFailureInput in = makeBase();
            in.blockKind = "dependency";
            in.blockedBy = "A";
            classified(in, "dependency_blocked", "human");
        }
        {
            TaskQueueFailureInput in = makeBase();
            in.blockKind = "resource";
            in.blockedBy = "W1";
            in.blockedResource = "shared.txt";
            classified(in, "resource_conflict", "human");
        }
        {
            TaskQueueFailureInput in = makeBase();
            classified(in, "unknown", "retry");
        }
        // External rules from failure-rules.json (doc/design-v3.md 4.4).
        {
            TaskQueueFailureInput in = makeBase();
            in.jobName = "lint";
            in.endKind = "failed";
            in.exitCode = 2;
            in.logText = "lint: 3 problems\n";
            in.logFirstLine = 100;
            in.logLastLine = 100;
            TaskQueueFailureRule rule;
            rule.failureClass = "lint_error";
            rule.summary = "lint reported problems";
            rule.suggestedAction = "fix_spec";
            rule.jobNames = {"lint"};
            rule.exitCodes = {2};
            rule.logRegex = "(?i)lint: \\d+ problems";
            in.externalRules.push_back(rule);
            auto d = classified(in, "lint_error", "fix_spec");
            Check(d.confidence == "high", "external rule keeps its default high confidence");
            Check(d.summary == "lint reported problems", "external rule summary is used");
            Check(d.detail.value("external", false), "external rule is marked in detail");
            Check(d.evidence.size() == 1 && d.evidence[0].IsLog() && d.evidence[0].line == 100,
                  "external logRegex names the matched log line");
        }
        {
            // An external rule with the same class wins over the built-in chain.
            TaskQueueFailureInput in = makeBase();
            in.jobName = "lint";
            in.endKind = "failed";
            in.exitCode = 2;
            TaskQueueFailureRule rule;
            rule.failureClass = "exit_nonzero";
            rule.summary = "lint stop";
            rule.suggestedAction = "fix_spec";
            rule.jobNames = {"lint"};
            rule.exitCodes = {2};
            in.externalRules.push_back(rule);
            auto d = classified(in, "exit_nonzero", "fix_spec");
            Check(d.summary == "lint stop", "external rule overrides a built-in class");
            Check(d.detail.value("rule", std::string()) == "external.exit_nonzero",
                  "override records the external rule name");
        }
        {
            // Content rules can look at normalized events, and a bad regex simply misses.
            TaskQueueFailureInput in = makeBase();
            in.endKind = "failed";
            in.exitCode = 1;
            in.events.push_back(event("error", 88, R"({"message":"Quota exceeded for this window"})"));
            TaskQueueFailureRule good;
            good.failureClass = "quota_window";
            good.suggestedAction = "human";
            good.eventKind = "error";
            good.eventRegex = "(?i)quota exceeded";
            TaskQueueFailureRule bad;
            bad.failureClass = "never";
            bad.logRegex = "(";
            in.externalRules = {bad, good};
            auto d = classified(in, "quota_window", "human");
            Check(d.evidence.size() == 1 && d.evidence[0].IsEvent() && d.evidence[0].seq == 88,
                  "external eventRegex points at the event seq");

            TaskQueueFailureInput fallback = makeBase();
            fallback.endKind = "failed";
            fallback.exitCode = 3;
            fallback.externalRules = {bad};
            classified(fallback, "exit_nonzero", "retry");
        }
        // The LLM guess is advisory only (doc/design-v3.md 4.4).
        {
            TaskQueueFailureInput in = makeBase();
            auto d = ClassifyTaskQueueFailure(in);
            Check(TaskQueueFailureNeedsLlmGuess(d), "unknown asks for an optional LLM guess");
            std::string packet = BuildTaskQueueFailureEscalationPacket(in, d);
            Check(!packet.empty() && packet.find("\"log\"") != std::string::npos &&
                      packet.find("building") != std::string::npos,
                  "escalation packet carries the log tail");
            Check(packet.find("\"attempt\"") != std::string::npos &&
                      packet.find("\"events\"") != std::string::npos,
                  "escalation packet carries attempt info and the event tail");
            std::string err;
            Check(ApplyTaskQueueFailureLlmGuess(d,
                      R"({"class":"flaky_network","summary":"DNS hiccup","action":"human","rationale":"resolver timeout"})",
                      &err),
                  "a valid escalate answer is applied");
            Check(d.failureClass == "unknown" && d.confidence == "low" && d.suggestedAction == "retry",
                  "the LLM guess does not replace the deterministic fields");
            Check(d.llmGuess.Present() && d.llmGuess.confidence == "low" &&
                      d.llmGuess.failureClass == "flaky_network" && d.llmGuess.source == "escalate",
                  "the LLM guess lives in the low-confidence reference field");
            std::string json = TaskQueueFailureDiagnosticToJson(d);
            Check(json.find("\"llmGuess\"") != std::string::npos &&
                      json.find("\"confidence\":\"low\"") != std::string::npos,
                  "the reference field is serialized as low confidence");
            std::string before = json;
            Check(!ApplyTaskQueueFailureLlmGuess(d, "not json", &err),
                  "an invalid escalate answer is ignored");
            Check(TaskQueueFailureDiagnosticToJson(d) == before,
                  "an invalid escalate answer leaves the diagnostic unchanged");

            TaskQueueFailureInput decided = makeBase();
            decided.endKind = "failed";
            decided.exitCode = 3;
            auto high = ClassifyTaskQueueFailure(decided);
            Check(!TaskQueueFailureNeedsLlmGuess(high), "a decided failure does not ask the LLM");
            Check(BuildTaskQueueFailureEscalationPacket(decided, high).empty(),
                  "a decided failure has no escalation packet");
            Check(!ApplyTaskQueueFailureLlmGuess(high, R"({"class":"crash"})", &err),
                  "an LLM guess is refused when the deterministic rules decided");

            TaskQueueFailureInput withGuess = makeBase();
            withGuess.llmGuessJson = R"({"class":"flaky_network","summary":"DNS hiccup"})";
            auto attached = ClassifyTaskQueueFailure(withGuess);
            Check(attached.failureClass == "unknown" && attached.llmGuess.Present() &&
                      attached.llmGuess.confidence == "low",
                  "an answer supplied on the input feeds only the advisory field");
        }
    }

    {
        // Retry policy by failure class (doc/design-v3.md 4.5).
        auto decide = [](const TaskQueueJob& job, const std::string& cls, int counted,
                         bool allowWait = true) {
            return DecideTaskQueueRetry(job, cls, counted, allowWait);
        };
        TaskQueueJob job;
        job.maxAttempts = 3;
        job.retry.configured = true;
        job.retry.on = {"idle_stall", "exit_nonzero", "incomplete"};
        job.retry.never = {"auth_error", "permission_denied"};
        job.retry.waitForRateLimit = true;
        Check(decide(job, "timeout", 1).action == TaskQueueRetryAction::GiveUp,
              "retry.on excludes timeout, so it does not retry");
        Check(decide(job, "exit_nonzero", 1).action == TaskQueueRetryAction::Retry,
              "retry.on allows exit_nonzero");
        Check(decide(job, "auth_error", 1).action == TaskQueueRetryAction::GiveUp,
              "retry.never stops auth_error");
        Check(decide(job, "auth_error", 1).consumesAttempt, "a refused class still counts");
        {
            auto d = decide(job, "rate_limited", 1);
            Check(d.action == TaskQueueRetryAction::WaitForRateLimit && !d.consumesAttempt,
                  "rate_limited waits until the reset and does not use up maxAttempts");
        }
        job.retry.never.push_back("rate_limited");
        Check(decide(job, "rate_limited", 1).action == TaskQueueRetryAction::GiveUp,
              "retry.never wins over waitForRateLimit");
        job.retry.never.pop_back();
        job.retry.waitForRateLimit = false;
        Check(decide(job, "rate_limited", 1).action == TaskQueueRetryAction::GiveUp,
              "without waitForRateLimit an ungranted rate_limited fails");
        Check(decide(job, "rate_limited", 1, false).action == TaskQueueRetryAction::GiveUp,
              "after defer.limit an ungranted rate_limited fails");
        job.retry.on.push_back("rate_limited");
        Check(decide(job, "rate_limited", 1).action == TaskQueueRetryAction::Retry,
              "rate_limited may be retried immediately when retry.on grants it");
        Check(decide(job, "rate_limited", 1, false).action == TaskQueueRetryAction::Retry,
              "after defer.limit rate_limited follows retry.on");
        Check(decide(job, "exit_nonzero", 3).action == TaskQueueRetryAction::GiveUp,
              "retry.on still stops at maxAttempts");
        {
            TaskQueueJob onlyNever;
            onlyNever.maxAttempts = 2;
            onlyNever.retry.configured = true;
            onlyNever.retry.never = {"auth_error"};
            Check(decide(onlyNever, "timeout", 1).action == TaskQueueRetryAction::Retry,
                  "an empty retry.on allows every class except retry.never");
            Check(decide(onlyNever, "auth_error", 1).action == TaskQueueRetryAction::GiveUp,
                  "retry.never alone still blocks auth_error");
        }
        {
            TaskQueueJob legacy;
            legacy.maxAttempts = 2;
            Check(decide(legacy, "timeout", 1).action == TaskQueueRetryAction::Retry,
                  "without retry, timeout retries like before");
            Check(decide(legacy, "auth_error", 1).action == TaskQueueRetryAction::Retry,
                  "without retry, auth_error retries like before");
            Check(decide(legacy, "rate_limited", 1).action == TaskQueueRetryAction::Retry &&
                      decide(legacy, "rate_limited", 1).consumesAttempt,
                  "without retry, rate_limited is an ordinary retry");
            Check(decide(legacy, "timeout", 2).action == TaskQueueRetryAction::GiveUp,
                  "without retry, maxAttempts still applies");
        }
        {
            // 040 + 041: the classifier's rate_limited class drives the wait decision even
            // when the attempt count has already reached maxAttempts.
            TaskQueueFailureInput in;
            in.jobId = "RL";
            in.jobName = "build";
            in.taskqueue = "q";
            in.attempt = 1;
            in.maxAttempts = 1;
            in.expectedExitCode = 0;
            in.endKind = "failed";
            in.exitCode = 1;
            in.nowEpochSec = 1794288000;
            in.eventsFile = "C:/logs/proc_9_build.events.jsonl";
            in.logFile = "C:/logs/RL.1.log";
            in.logText = "rate limit rejected\n";
            in.logFirstLine = 1;
            in.logLastLine = 1;
            in.events.push_back(nlohmann::json::parse(
                R"({"schema":"hancho.event/1","seq":7,"ts":"2026-11-10T09:00:00Z","kind":"rate_limit",)"
                R"("data":{"status":"rejected","window":"five_hour","resetsAt":1794293000}})",
                nullptr, false));
            TaskQueueFailureDiagnostic diag = ClassifyTaskQueueFailure(in);
            Check(diag.failureClass == "rate_limited", "040 diagnosis feeds the retry policy");
            TaskQueueJob rlJob;
            rlJob.maxAttempts = 1;
            rlJob.retry.configured = true;
            rlJob.retry.waitForRateLimit = true;
            auto d = decide(rlJob, diag.failureClass, 1);
            Check(d.action == TaskQueueRetryAction::WaitForRateLimit && !d.consumesAttempt,
                  "a classified rate_limited waits even at maxAttempts");
        }
    }

    std::string err;
    std::string jobs = R"([
      {"id":"A","command":"cmd /c exit 0","summary":"A ok","timeout_sec":30},
      {"id":"B","command":"cmd /c exit 0","summary":"B after A","timeout_sec":30,"depends_on":["A"]},
      {"id":"C","command":"cmd /c exit 1","summary":"C fails","timeout_sec":30,"max_attempts":1}
    ])";
    std::string addResp = tqm.TaskQueueAdd("main", jobs, 0, "", err);
    Check(JsonGetBool(addResp, "ok", false), "taskqueue_add ok: " + err);

    Check(WaitForState(tqm, "A", "succeeded", 20000), "A succeeded");
    Check(WaitForState(tqm, "B", "succeeded", 20000), "B succeeded after A");
    Check(WaitForState(tqm, "C", "failed", 20000), "C failed");

    {
        // Every failed attempt carries a diagnostic that points back at its evidence.
        auto cj = ParseJobJson(tqm.TaskQueueGet("C", false, 0, err));
        bool hasAttempt = cj.is_object() && cj.contains("attempts") &&
                          cj["attempts"].is_array() && !cj["attempts"].empty();
        Check(hasAttempt, "C has an attempt record");
        if (hasAttempt) {
            const auto& attempt = cj["attempts"][0];
            Check(attempt.contains("failure") && attempt["failure"].is_object(),
                  "C attempt carries a diagnosis");
            if (attempt.contains("failure") && attempt["failure"].is_object()) {
                const auto& f = attempt["failure"];
                Check(f.value("class", std::string()) == "exit_nonzero",
                      "C attempt diagnosed as exit_nonzero: " + f.dump());
                Check(f.contains("evidence") && f["evidence"].is_array() && !f["evidence"].empty(),
                      "C diagnosis carries evidence");
                if (f.contains("evidence") && !f["evidence"].empty()) {
                    const auto& e = f["evidence"][0];
                    Check(e.contains("log") && e.value("line", 0) > 0,
                          "C evidence points at a log line: " + e.dump());
                }
            }
        }
        std::string events = ReadFileUtf8Wide(TaskQueueManager::GetTaskEventsFileWide("main"));
        Check(events.find("\"seq\":") != std::string::npos, "task queue events carry seq");
        Check(events.find("\"failure\":{\"class\":\"exit_nonzero\"") != std::string::npos,
              "job events carry the diagnosis for BT conditions");
    }

    {
        // retry is parsed from camelCase input, persisted as snake_case, and omitted when
        // the job keeps the legacy policy.
        std::string retryJobs = R"([
          {"id":"RT1","command":"cmd /c exit 0","summary":"retry policy","timeoutSec":30,
           "retry":{"on":["exit_nonzero","incomplete"],"never":["auth_error"],"waitForRateLimit":false}},
          {"id":"RT3","command":"cmd /c exit 0","summary":"legacy policy","timeoutSec":30},
          {"id":"RT5","command":"cmd /c exit 1","summary":"failed then retried","timeoutSec":30,"maxAttempts":1}
        ])";
        Check(JsonGetBool(tqm.TaskQueueAdd("retryq", retryJobs, 0, "", err), "ok", false),
              "retry jobs added: " + err);
        std::string rt1 = tqm.TaskQueueGet("RT1", false, 0, err);
        Check(rt1.find("\"retry\":{\"on\":[\"exit_nonzero\",\"incomplete\"],\"never\":[\"auth_error\"],"
                       "\"wait_for_rate_limit\":false}") != std::string::npos,
              "retry policy persisted and returned: " + rt1);
        std::string rt3 = tqm.TaskQueueGet("RT3", false, 0, err);
        Check(rt3.find("\"retry\"") == std::string::npos,
              "a job without retry keeps the legacy policy in its JSON");
        Check(WaitForState(tqm, "RT5", "failed", 20000), "retry policy failure job failed");
        std::string patch = R"({"retry":{"on":["timeout"],"never":["auth_error"]}})";
        std::string up = tqm.TaskQueueUpdate("RT5", patch, err);
        Check(JsonGetBool(up, "ok", false), "retry can be updated while failed: " + err);
        Check(tqm.TaskQueueGet("RT5", false, 0, err).find("\"retry\":{\"on\":[\"timeout\"]") !=
                  std::string::npos,
              "retry update applied");
        std::string badRetry = R"([{"id":"RT2","command":"cmd /c exit 0","summary":"bad retry","timeoutSec":10,
          "retry":{"on":["not_a_class"]}}])";
        Check(!JsonGetBool(tqm.TaskQueueAdd("retryq", badRetry, 0, "", err), "ok", false),
              "an unknown retry.on class is rejected");
        std::string badRetryType =
            R"([{"id":"RT4","command":"cmd /c exit 0","summary":"bad retry","timeoutSec":10,"retry":["x"]}])";
        Check(!JsonGetBool(tqm.TaskQueueAdd("retryq", badRetryType, 0, "", err), "ok", false),
              "retry must be an object");
        Check(WaitForState(tqm, "RT1", "succeeded", 20000), "retry policy job still runs");
    }

    std::string badJobs = R"([{"id":"X","command":"cmd /c exit 0 && cmd /c exit 0","summary":"chain","timeout_sec":10}])";
    std::string badResp = tqm.TaskQueueAdd("main", badJobs, 0, "", err);
    Check(!JsonGetBool(badResp, "ok", false), "taskqueue_add rejects chain");

    std::string noTimeout = R"([{"id":"Y","command":"cmd /c exit 0","summary":"no timeout"}])";
    std::string ntResp = tqm.TaskQueueAdd("main", noTimeout, 0, "", err);
    Check(!JsonGetBool(ntResp, "ok", false), "taskqueue_add rejects missing timeout");

    std::wstring workDir = dir + L"\\work";
    CreateDirectoryW(workDir.c_str(), nullptr);
    std::string workUtf8 = WideToUtf8(workDir);
    std::string envJobs = "[{\"id\":\"E\",\"command\":\"cmd /c echo %PC_JOB_ID%,%PC_ATTEMPT%,%PC_JOB_SUMMARY%>args.txt\","
                          "\"summary\":\"Env Check\",\"timeoutSec\":30,\"cwd\":\"" + JsonEscape(workUtf8) + "\"}]";
    std::string envResp = tqm.TaskQueueAdd("envq", envJobs, 0, "", err);
    Check(JsonGetBool(envResp, "ok", false), "env job added: " + err);
    Check(WaitForState(tqm, "E", "succeeded", 20000), "env job succeeded");
    std::string argsFile = ReadFileUtf8Wide(workDir + L"\\args.txt");
    Check(argsFile.find("E,1,Env Check") != std::string::npos,
          "PC_* env vars and summary passed: " + argsFile);

    // Memory injection: memory_brief is expanded at {memory} and its ids are recorded
    // on the attempt. Injection stays off by default (job M2).
    {
        std::wstring memDir = dir + L"\\memory";
        std::wstring recDir = memDir + L"\\records";
        CreateDirectoryW(memDir.c_str(), nullptr);
        CreateDirectoryW(recDir.c_str(), nullptr);
        std::string run =
            "{\"schema\":\"hancho.memory.run/1\",\"id\":\"rr_mem_1\",\"jobName\":\"memjob\","
            "\"taskqueue\":\"memq\",\"outcome\":\"failed\","
            "\"failure\":{\"class\":\"success_check_failed\",\"summary\":\"app.exe was not updated\"},"
            "\"start\":\"2026-11-10T09:02:11Z\",\"durationSec\":749,\"exitCode\":0,"
            "\"filesEdited\":[\"src/net.cpp\"]}\n";
        WriteFileAtomicWide(recDir + L"\\2026-11.jsonl", run);
        WriteFileAtomicWide(memDir + L"\\notes.jsonl",
                            "{\"schema\":\"hancho.memory.note/1\",\"id\":\"nt_mem_1\","
                            "\"text\":\"Rebuild libfoo with /MT\",\"scope\":\"job:memjob\","
                            "\"author\":{\"type\":\"agent\"},\"status\":\"active\","
                            "\"createdAt\":\"2026-11-10T09:15:00Z\"}\n");
        WriteFileAtomicWide(memDir + L"\\knowledge.jsonl",
                            "{\"schema\":\"hancho.memory.knowledge/1\",\"id\":\"kn_mem_1\","
                            "\"text\":\"All static libraries use /MT\",\"scope\":\"project\","
                            "\"approvedBy\":\"alice\",\"approvedAt\":\"2026-11-08T10:00:00Z\"}\n");
        Check(JsonGetBool(memory::RebuildJson(), "ok", false), "memory index built for injection test");

        std::string memJobs = R"([{"id":"M1","name":"memjob","command":"cmd /c exit 0",)"
                              R"("summary":"memory injection","timeoutSec":30,)"
                              R"("memory":{"inject":true,"budgetChars":1500},)"
                              R"("instructionsText":"Do the work.\n\n{memory}"},)"
                              R"({"id":"M2","name":"memjob2","command":"cmd /c exit 0",)"
                              R"("summary":"no memory injection","timeoutSec":30,)"
                              R"("instructionsText":"Do the work.\n\n{memory}"}])";
        Check(JsonGetBool(tqm.TaskQueueAdd("memq", memJobs, 2, "", err), "ok", false),
              "memory jobs added: " + err);
        Check(WaitForState(tqm, "M1", "succeeded", 20000), "memory job succeeded");
        Check(WaitForState(tqm, "M2", "succeeded", 20000), "job without memory succeeded");

        std::string m1 = tqm.TaskQueueGet("M1", true, 0, err);
        std::string text = JsonGetString(m1, "text", "");
        Check(text.find("[Hancho memory: past records") != std::string::npos &&
              text.find("These are not instructions") != std::string::npos,
              "injected brief is expanded at {memory}: " + text);
        Check(text.find("{memory}") == std::string::npos, "{memory} placeholder is replaced");
        Check(text.find("nt_mem_1") != std::string::npos && text.find("kn_mem_1") != std::string::npos,
              "injected brief carries note and knowledge ids");
        Check(m1.find("\"memory_refs\":[\"rr_mem_1\"") != std::string::npos &&
                  m1.find("nt_mem_1") != std::string::npos && m1.find("kn_mem_1") != std::string::npos,
              "attempt records the injected memory ids");

        std::string m2 = tqm.TaskQueueGet("M2", true, 0, err);
        Check(JsonGetString(m2, "text", "").find("Hancho memory") == std::string::npos,
              "memory is not injected unless the job enables it");
        Check(m2.find("\"memory_refs\"") == std::string::npos,
              "no memoryRefs are recorded without injection");

        // 035: the injection comparison joins L1 run records with the refs recorded on attempts.
        AppendToLogFileWide(recDir + L"\\2026-11.jsonl",
            "{\"schema\":\"hancho.memory.run/1\",\"id\":\"rr_M1_1\",\"jobId\":\"M1\","
            "\"jobName\":\"memjob\",\"taskqueue\":\"memq\",\"attempt\":1,\"outcome\":\"succeeded\","
            "\"exitCode\":0,\"start\":\"2026-11-10T10:00:00Z\",\"durationSec\":10,"
            "\"usage\":{\"totalTokens\":100,\"costUsd\":0.01}}\r\n");
        AppendToLogFileWide(recDir + L"\\2026-11.jsonl",
            "{\"schema\":\"hancho.memory.run/1\",\"id\":\"rr_M2_1\",\"jobId\":\"M2\","
            "\"jobName\":\"memjob2\",\"taskqueue\":\"memq\",\"attempt\":1,\"outcome\":\"succeeded\","
            "\"exitCode\":0,\"start\":\"2026-11-10T10:05:00Z\",\"durationSec\":10,"
            "\"usage\":{\"totalTokens\":80,\"costUsd\":0.01}}\r\n");
        std::string compare = MemoryInjectCompareJson("", "memq", "");
        auto cj = nlohmann::json::parse(compare, nullptr, false);
        Check(cj.is_object() && cj.value("ok", false), "memory compare-inject ok: " + compare);
        if (cj.is_object() && cj.contains("injected") && cj.contains("notInjected")) {
            Check(cj["injected"].value("runs", 0) >= 1, "compare counts injected runs");
            Check(cj["notInjected"].value("runs", 0) >= 1, "compare counts runs without injection");
            Check(cj.value("tokenDeltaPerRun", 0.0) > 0.0,
                  "compare reports the extra tokens per injected run");
        } else {
            Check(false, "compare response has injected/notInjected buckets");
        }
    }

    // Exit 0 is not success: D must write reportD.txt, it does not -> checkFailed.
    // B2 depends on D -> blocked once D finally fails.
    std::string jobs2 = "["
        "{\"id\":\"D\",\"command\":\"cmd /c exit 0\",\"summary\":\"D no file\",\"timeoutSec\":30,"
        "\"maxAttempts\":1,\"success\":{\"exitCode\":0,\"filesNewer\":[\"reportD.txt\"]},"
        "\"cwd\":\"" + JsonEscape(workUtf8) + "\"},"
        "{\"id\":\"B2\",\"command\":\"cmd /c exit 0\",\"summary\":\"blocked after D\",\"timeoutSec\":30,"
        "\"dependsOn\":[\"D\"],\"cwd\":\"" + JsonEscape(workUtf8) + "\"}"
        "]";
    std::string add2 = tqm.TaskQueueAdd("checkq", jobs2, 0, "", err);
    Check(JsonGetBool(add2, "ok", false), "checkq added: " + err);
    Check(WaitForState(tqm, "D", "failed", 20000), "D failed because the file was not created");
    Check(WaitForState(tqm, "B2", "blocked", 20000), "B2 blocked after D failed");
    {
        auto dj = ParseJobJson(tqm.TaskQueueGet("D", false, 0, err));
        bool hasAttempt = dj.is_object() && dj.contains("attempts") &&
                          dj["attempts"].is_array() && !dj["attempts"].empty();
        Check(hasAttempt, "D has an attempt record");
        if (hasAttempt) {
            const auto& attempt = dj["attempts"][0];
            Check(attempt.contains("failure") && attempt["failure"].is_object(),
                  "D attempt carries a diagnosis");
            if (attempt.contains("failure") && attempt["failure"].is_object()) {
                const auto& f = attempt["failure"];
                Check(f.value("class", std::string()) == "success_check_failed",
                      "D attempt diagnosed as success_check_failed: " + f.dump());
                Check(f.contains("detail") && f["detail"].is_object() &&
                          f["detail"].value("condition", std::string()) == "filesNewer",
                      "success_check_failed detail names filesNewer");
            }
        }
    }

    std::string jobs3 = "[{\"id\":\"F\",\"command\":\"cmd /c echo ok>reportF.txt\",\"summary\":\"F writes file\","
                        "\"timeoutSec\":30,\"success\":{\"exitCode\":0,\"filesNewer\":[\"reportF.txt\"]},"
                        "\"cwd\":\"" + JsonEscape(workUtf8) + "\"}]";
    std::string add3 = tqm.TaskQueueAdd("checkq", jobs3, 0, "", err);
    Check(JsonGetBool(add3, "ok", false), "F added: " + err);
    Check(WaitForState(tqm, "F", "succeeded", 20000), "F succeeded because it wrote the file");

    // success.testReport: the job writes a JUnit file; a failed test case fails the job and
    // the failed test name ends up in the diagnosis.
    WriteFileAtomicWide(workDir + L"/junit-pass.src",
        "<?xml version=\"1.0\"?><testsuite tests=\"2\"><testcase classname=\"A\" name=\"one\"/>"
        "<testcase classname=\"A\" name=\"two\"></testcase></testsuite>\n");
    WriteFileAtomicWide(workDir + L"/junit-fail.src",
        "<testsuite><testcase classname=\"A\" name=\"ok\"/><testcase classname=\"A\" name=\"broken\">"
        "<failure message=\"x\">boom</failure></testcase></testsuite>\n");
    {
        std::string jobsTr = "["
            "{\"id\":\"TRP\",\"command\":\"cmd /c type junit-pass.src>tr-pass.xml\",\"summary\":\"report passes\","
            "\"timeoutSec\":30,\"maxAttempts\":1,\"success\":{\"testReport\":\"tr-pass.xml\"},"
            "\"cwd\":\"" + JsonEscape(workUtf8) + "\"},"
            "{\"id\":\"TRF\",\"command\":\"cmd /c type junit-fail.src>tr-fail.xml\",\"summary\":\"report fails\","
            "\"timeoutSec\":30,\"maxAttempts\":1,\"success\":{\"testReport\":\"tr-fail.xml\"},"
            "\"cwd\":\"" + JsonEscape(workUtf8) + "\"},"
            "{\"id\":\"TRM\",\"command\":\"cmd /c exit 0\",\"summary\":\"report missing\","
            "\"timeoutSec\":30,\"maxAttempts\":1,\"success\":{\"testReport\":\"tr-missing.xml\"},"
            "\"cwd\":\"" + JsonEscape(workUtf8) + "\"}"
            "]";
        std::string addTr = tqm.TaskQueueAdd("trq", jobsTr, 0, "", err);
        Check(JsonGetBool(addTr, "ok", false), "testReport jobs added: " + err);
        Check(WaitForState(tqm, "TRP", "succeeded", 20000), "TRP succeeded with an all-pass JUnit report");
        Check(WaitForState(tqm, "TRF", "failed", 20000), "TRF failed because a test case failed");
        Check(WaitForState(tqm, "TRM", "failed", 20000), "TRM failed because the report is missing");
        auto fj = ParseJobJson(tqm.TaskQueueGet("TRF", false, 0, err));
        if (fj.is_object() && fj.contains("job") && fj["job"].is_object()) fj = fj["job"];
        std::string cond, detail;
        if (fj.is_object() && fj.contains("attempts") && fj["attempts"].is_array() && !fj["attempts"].empty()) {
            const auto& a = fj["attempts"][0];
            if (a.contains("failure") && a["failure"].is_object() && a["failure"].contains("detail") &&
                a["failure"]["detail"].is_object()) {
                cond = a["failure"]["detail"].value("condition", std::string());
                detail = a["failure"]["detail"].value("detail", std::string());
            }
        }
        Check(cond == "testReport", "TRF diagnosis names testReport: " + cond);
        Check(detail.find("A.broken") != std::string::npos && detail.find("1 of 2") != std::string::npos,
              "TRF diagnosis lists the failed test: " + detail);
    }

    // Instructions versioning: H fails, source changed, retry refreshes to rev 2.
    std::wstring instrPath = workDir + L"\\instr.md";
    WriteFileAtomicWide(instrPath, "# Task H\nDo the thing\n");
    std::string instrUtf8 = WideToUtf8(instrPath);
    std::string jobs4 = "[{\"id\":\"H\",\"command\":\"cmd /c exit 1\",\"summary\":\"H with instructions\","
                        "\"timeoutSec\":30,\"maxAttempts\":1,\"instructionsFile\":\"" + JsonEscape(instrUtf8) + "\"}]";
    std::string add4 = tqm.TaskQueueAdd("instrq", jobs4, 0, "", err);
    Check(JsonGetBool(add4, "ok", false), "H added: " + err);
    Check(WaitForState(tqm, "H", "failed", 20000), "H failed");
    {
        std::string r = tqm.TaskQueueGet("H", true, 0, err);
        Check(JsonGetString(r, "text", "").find("Do the thing") != std::string::npos,
              "instructions text captured");
        Check(!JsonGetBool(r, "source_changed", false), "source not changed yet");
        WriteFileAtomicWide(instrPath, "# Task H\nDo the thing (revised)\n");
        r = tqm.TaskQueueGet("H", true, 0, err);
        Check(JsonGetBool(r, "source_changed", false), "source_changed detected after edit");
    }
    {
        std::string r = tqm.TaskQueueRetry("H", true, true, err);
        Check(JsonGetBool(r, "ok", false), "retry H with refresh: " + err);
        std::string g = tqm.TaskQueueGet("H", true, 0, err);
        Check(JsonGetString(g, "rev", "0") == "2", "instructions revision bumped to 2");
    }

    Sleep(300);
    std::string retryResp = tqm.TaskQueueRetry("C", true, false, err);
    Check(JsonGetBool(retryResp, "ok", false), "retry C ok: " + err);
    Check(JsonGetString(retryResp, "state", "") == "pending", "retry C returns pending");
    Check(WaitForState(tqm, "C", "failed", 20000), "C runs and fails again after retry");

    std::wstring stateFile = TaskQueueManager::GetTaskQueueFileWide("main");
    Check(GetFileAttributesW(stateFile.c_str()) != INVALID_FILE_ATTRIBUTES, "taskqueue json persisted");

    {
        // Origin: recorded on every added job, persisted, returned and usable as a list filter.
        std::string originJson = R"({"label":"Release Work","client":"claude-code 2.1","session":"121bb3dd-40e2-4acb",)"
                                 R"("host_session":"local_9f76","cwd":"D:\\wsgit\\Hancho","pid":4242})";
        std::string oJobs = R"([{"id":"O1","command":"cmd /c exit 0","summary":"origin job","timeoutSec":30}])";
        Check(JsonGetBool(tqm.TaskQueueAdd("originq", oJobs, 0, "", err, originJson), "ok", false), "origin job added: " + err);
        std::string other = R"([{"id":"O2","command":"cmd /c exit 0","summary":"no origin","timeoutSec":30}])";
        Check(JsonGetBool(tqm.TaskQueueAdd("originq", other, 0, "", err), "ok", false), "job without origin added: " + err);

        std::string got = tqm.TaskQueueGet("O1", false, 0, err);
        Check(got.find("\"origin\":{\"label\":\"Release Work\"") != std::string::npos, "origin returned by get: " + got);
        Check(tqm.TaskQueueGet("O2", false, 0, err).find("\"origin\"") == std::string::npos, "no origin field when none given");
        std::string saved = ReadFileUtf8Wide(TaskQueueManager::GetTaskQueueFileWide("originq"));
        Check(saved.find("\"session\":\"121bb3dd-40e2-4acb\"") != std::string::npos, "origin persisted");

        auto listIds = [&](const std::string& filter) {
            std::string resp = tqm.TaskQueueList("originq", "", 0, err, filter);
            int n = 0;
            for (size_t p = resp.find("{\"id\":"); p != std::string::npos; p = resp.find("{\"id\":", p + 1)) ++n;
            return n;
        };
        Check(listIds("121bb3dd") == 1, "origin filter matches session prefix");
        Check(listIds("local_9f76") == 1, "origin filter matches host session");
        Check(listIds("release work") == 1, "origin filter matches label case-insensitively");
        Check(listIds("pid:4242") == 1, "origin filter matches pid");
        Check(listIds("Release") == 0, "origin label needs a full match");
        Check(listIds("") == 2, "no origin filter lists all");

        TaskQueueOrigin parsed = TaskQueueManager::ParseOriginJson(TaskQueueManager::OriginToJson(
            TaskQueueManager::ParseOriginJson(originJson)));
        Check(parsed.hostSession == "local_9f76" && parsed.pid == 4242 && parsed.cwd == "D:\\wsgit\\Hancho",
              "origin JSON round trip");
    }

    {
        // BT-wide list filter: taskqueue_list --bt <id> spans the tree's parallel branch queues.
        TaskQueueBtRef refA;
        refA.id = "tree-a-" + std::to_string(GetCurrentProcessId());
        refA.node = "n1";
        refA.run = 1;
        TaskQueueBtRef refB;
        refB.id = "tree-b-" + std::to_string(GetCurrentProcessId());
        refB.node = "n1";
        refB.run = 1;
        std::string jobAId = "B1-" + std::to_string(GetCurrentProcessId());
        std::string jobBId = "B2-" + std::to_string(GetCurrentProcessId());
        std::string btJobA = "{\"id\":\"" + jobAId + "\",\"command\":\"cmd /c exit 0\",\"summary\":\"bt job a\",\"timeoutSec\":30}";
        std::string btJobB = "{\"id\":\"" + jobBId + "\",\"command\":\"cmd /c exit 0\",\"summary\":\"bt job b\",\"timeoutSec\":30}";
        Check(JsonGetBool(tqm.AddBtJob("btqa", btJobA, refA, err), "ok", false), "BT job A added: " + err);
        std::string bResp = tqm.AddBtJob("btqb", btJobB, refB, err);
        Check(JsonGetBool(bResp, "ok", false), "BT job B added: " + bResp + " " + err);
        auto listHas = [&](const std::string& btId, const std::string& jobId) {
            std::string resp = tqm.TaskQueueList("", "", 0, err, "", btId);
            return resp.find("\"id\":\"" + jobId + "\"") != std::string::npos;
        };
        Check(listHas(refA.id, jobAId) && !listHas(refA.id, jobBId), "BT filter keeps only the tree's jobs");
        Check(listHas(refB.id, jobBId) && !listHas(refB.id, jobAId), "BT filter separates trees");
    }

    {
        // estimateSec (camelCase) is normalized and returned as estimate_sec for the ETA display.
        std::string suffix = std::to_string(GetCurrentProcessId());
        std::string estJobs = "[{\"id\":\"E1-" + suffix +
            "\",\"command\":\"cmd /c exit 0\",\"summary\":\"estimate job\",\"timeoutSec\":600,\"estimateSec\":300}]";
        Check(JsonGetBool(tqm.TaskQueueAdd("estq-" + suffix, estJobs, 0, "", err), "ok", false),
              "job with estimateSec added: " + err);
        std::string resp = tqm.TaskQueueList("estq-" + suffix, "", 0, err);
        Check(resp.find("\"estimate_sec\":300") != std::string::npos,
              "estimate_sec is returned by taskqueue list: " + resp);

        // Verify timing_feedback when an attempt finishes with duration diverging from estimate
        TaskQueueJob testJob;
        testJob.id = "TF-1";
        testJob.state = "succeeded";
        testJob.estimateSec = 10;
        TaskQueueAttempt att;
        att.reason = "ok";
        att.start = "2026-11-12 10:00:00";
        att.end = "2026-11-12 10:00:30"; // 30s duration vs 10s estimate -> 3.0x (diverged_over)
        testJob.attempts.push_back(att);
        std::string tfJson = tqm.JobToJson(testJob, false);
        Check(tfJson.find("\"timing_feedback\"") != std::string::npos, "timing_feedback present in JobToJson");
        Check(tfJson.find("\"accuracy\":\"diverged_over\"") != std::string::npos, "diverged_over detected");
        Check(tfJson.find("\"ratio\":3.00") != std::string::npos, "ratio 3.00 calculated");
        Check(tfJson.find("\"duration_sec\":30") != std::string::npos, "duration_sec 30 calculated");

        // Under-estimate: estimate 100s, duration 20s -> 0.2x (diverged_under)
        testJob.estimateSec = 100;
        testJob.attempts[0].end = "2026-11-12 10:00:20";
        tfJson = tqm.JobToJson(testJob, false);
        Check(tfJson.find("\"accuracy\":\"diverged_under\"") != std::string::npos, "diverged_under detected");

        // Accurate: estimate 30s, duration 30s -> 1.0x (accurate)
        testJob.estimateSec = 30;
        testJob.attempts[0].end = "2026-11-12 10:00:30";
        tfJson = tqm.JobToJson(testJob, false);
        Check(tfJson.find("\"accuracy\":\"accurate\"") != std::string::npos, "accurate detected within tolerance");

        // A failed attempt says nothing about the estimate: 2s vs estimate 100 must not be diverged_under.
        auto tfFor = [&](const std::string& jobState, const std::vector<TaskQueueAttempt>& atts, int64_t estimate) {
            TaskQueueJob j;
            j.id = "TF-X";
            j.state = jobState;
            j.estimateSec = estimate;
            j.attempts = atts;
            return tqm.JobToJson(j, false);
        };
        auto attempt = [](const std::string& reason, const std::string& start, const std::string& end) {
            TaskQueueAttempt a;
            a.reason = reason;
            a.start = start;
            a.end = end;
            return a;
        };
        std::vector<TaskQueueAttempt> failedOnly = {attempt("failed", "2026-11-12 10:00:00", "2026-11-12 10:00:02")};
        std::string failedJson = tfFor("failed", failedOnly, 100);
        Check(failedJson.find("\"timing_feedback\"") == std::string::npos,
              "failed attempt (2s vs estimate 100) produces no timing_feedback: " + failedJson);

        // Even if the job is marked succeeded, a failed attempt is never used for timing.
        std::string failedOnSucceeded = tfFor("succeeded", failedOnly, 100);
        Check(failedOnSucceeded.find("\"accuracy\":\"diverged_under\"") == std::string::npos,
              "failed attempt is not diverged_under: " + failedOnSucceeded);

        // Very short successful run: 3s vs estimate 100 is measurement noise, not diverged_under.
        std::vector<TaskQueueAttempt> shortOk = {attempt("ok", "2026-11-12 10:00:00", "2026-11-12 10:00:03")};
        std::string shortJson = tfFor("succeeded", shortOk, 100);
        Check(shortJson.find("\"accuracy\":\"diverged_under\"") == std::string::npos,
              "successful 3s attempt vs estimate 100 is not diverged_under: " + shortJson);

        // A failed attempt after a successful one: the successful attempt (30s vs 10s) is the one used.
        std::vector<TaskQueueAttempt> okThenFailed = {
            attempt("ok", "2026-11-12 10:00:00", "2026-11-12 10:00:30"),
            attempt("failed", "2026-11-12 10:01:00", "2026-11-12 10:01:02")};
        std::string okThenFailedJson = tfFor("succeeded", okThenFailed, 10);
        Check(okThenFailedJson.find("\"duration_sec\":30") != std::string::npos &&
                  okThenFailedJson.find("\"accuracy\":\"diverged_over\"") != std::string::npos,
              "successful attempt is used when a later attempt failed: " + okThenFailedJson);
    }

    {
        TaskQueueCounts cBefore = tqm.GetJobCounts();
        Check(cBefore.total > 0, "GetJobCounts returned non-empty total");
        Check(cBefore.succeeded >= 3, "GetJobCounts accurately counted succeeded jobs");

        std::string sleepJobs = "["
            "{\"id\":\"S1\",\"command\":\"ping -n 5 127.0.0.1\",\"summary\":\"S1 running\",\"timeoutSec\":30},"
            "{\"id\":\"S2\",\"command\":\"ping -n 5 127.0.0.1\",\"summary\":\"S2 waiting\",\"timeoutSec\":30},"
            "{\"id\":\"S3\",\"command\":\"ping -n 5 127.0.0.1\",\"summary\":\"S3 pending on S2\",\"timeoutSec\":30,\"dependsOn\":[\"S2\"]}"
            "]";
        std::string sResp = tqm.TaskQueueAdd("concurq", sleepJobs, 1, "", err);
        Check(JsonGetBool(sResp, "ok", false), "concurq added");
        WaitForState(tqm, "S1", "running", 5000);

        TaskQueueCounts cAfter = tqm.GetJobCounts();
        Check(cAfter.running >= 1, "GetJobCounts accurately detected running job");
        Check(cAfter.waiting >= 1, "GetJobCounts accurately detected waiting ready job");
        Check(cAfter.pending >= 1, "GetJobCounts accurately detected pending dependent job");

        // CI run: a pending dependent keeps the run waiting instead of reporting success.
        auto snapshot = nlohmann::json::parse(tqm.TaskQueueWait("concurq", 0, 0, err), nullptr, false);
        std::vector<TaskQueueRunJob> runJobs;
        if (snapshot.is_object() && snapshot.contains("jobs") && snapshot["jobs"].is_array()) {
            for (const auto& job : snapshot["jobs"]) runJobs.push_back(TaskQueueRunJobFromJson(job));
        }
        Check(!runJobs.empty() && !TaskQueueRunDrained(runJobs),
              "run does not finish while a dependency is still pending");

        tqm.TaskQueueCancel("S1", err);
        tqm.TaskQueueCancel("S2", err);
        tqm.TaskQueueCancel("S3", err);
    }

    {
        // Scheduler: W1/W2 write the same file -> serialized; R1/R2 only read it -> parallel.
        std::wstring resDir = dir + L"\\res";
        CreateDirectoryW(resDir.c_str(), nullptr);
        std::string resCwd = JsonEscape(WideToUtf8(resDir));
        std::string jobsW = "["
            "{\"id\":\"W1\",\"command\":\"ping -n 4 127.0.0.1\",\"summary\":\"W1\",\"timeoutSec\":30,\"cwd\":\"" + resCwd + "\","
            "\"resources\":{\"write\":[\"shared.txt\"]}},"
            "{\"id\":\"W2\",\"command\":\"ping -n 4 127.0.0.1\",\"summary\":\"W2\",\"timeoutSec\":30,\"cwd\":\"" + resCwd + "\","
            "\"resources\":{\"write\":[\"shared.txt\"]}}"
            "]";
        Check(JsonGetBool(tqm.TaskQueueAdd("resw", jobsW, 4, "", err), "ok", false), "resource writers added: " + err);
        Check(WaitForState(tqm, "W1", "running", 5000), "W1 starts");
        Sleep(800);
        Check(GetState(tqm, "W2") == "pending", "W2 waits while W1 holds shared.txt");
        std::string g = tqm.TaskQueueGraph("resw", "", err);
        Check(g.find("\"resource_wait\":\"W1: shared.txt\"") != std::string::npos, "graph shows resource_wait for W2");
        std::string wl = tqm.TaskQueueList("resw", "", 0, err);
        Check(wl.find("\"resource_wait\":\"W1: shared.txt\"") != std::string::npos,
              "taskqueue list shows resource_wait for W2: " + wl);
        Check(wl.find("\"id\":\"W1\"") != std::string::npos &&
                  wl.find("\"resource_wait\":null") != std::string::npos,
              "taskqueue list reports null resource_wait for the running job");
        Check(WaitForState(tqm, "W1", "succeeded", 20000), "W1 succeeded");
        Check(WaitForState(tqm, "W2", "succeeded", 20000), "W2 runs after W1 releases shared.txt");

        std::string jobsR = "["
            "{\"id\":\"RD1\",\"command\":\"ping -n 4 127.0.0.1\",\"summary\":\"RD1\",\"timeoutSec\":30,\"cwd\":\"" + resCwd + "\","
            "\"resources\":{\"read\":[\"shared.txt\"]}},"
            "{\"id\":\"RD2\",\"command\":\"ping -n 4 127.0.0.1\",\"summary\":\"RD2\",\"timeoutSec\":30,\"cwd\":\"" + resCwd + "\","
            "\"resources\":{\"read\":[\"shared.txt\"]}}"
            "]";
        Check(JsonGetBool(tqm.TaskQueueAdd("resr", jobsR, 4, "", err), "ok", false), "resource readers added: " + err);
        Check(WaitForState(tqm, "RD1", "running", 5000) && WaitForState(tqm, "RD2", "running", 5000), "readers run in parallel");
        Check(WaitForState(tqm, "RD2", "succeeded", 20000), "reader RD2 succeeded");

        // Order across queues: a writer queued behind a running reader is not overtaken by a
        // reader registered later in another queue (no writer starvation).
        auto one = [&](const std::string& id, const std::string& mode) {
            return "[{\"id\":\"" + id + "\",\"command\":\"ping -n 4 127.0.0.1\",\"summary\":\"" + id +
                   "\",\"timeoutSec\":30,\"cwd\":\"" + resCwd + "\",\"resources\":{\"" + mode + "\":[\"order.txt\"]}}]";
        };
        Check(JsonGetBool(tqm.TaskQueueAdd("ordq1", one("OA", "read"), 2, "", err), "ok", false), "OA added: " + err);
        Check(WaitForState(tqm, "OA", "running", 5000), "reader OA starts");
        Check(JsonGetBool(tqm.TaskQueueAdd("ordq2", one("OW", "write"), 2, "", err), "ok", false), "OW added: " + err);
        Check(JsonGetBool(tqm.TaskQueueAdd("ordq3", one("OB", "read"), 2, "", err), "ok", false), "OB added: " + err);
        Sleep(800);
        Check(GetState(tqm, "OW") == "pending", "writer OW waits for reader OA");
        Check(GetState(tqm, "OB") == "pending", "later reader OB in another queue waits behind writer OW");
        std::string og = tqm.TaskQueueGraph("ordq3", "", err);
        Check(og.find("OW (queued ahead): order.txt") != std::string::npos, "graph shows OB queued behind OW");
        Check(WaitForState(tqm, "OW", "running", 20000), "OW starts after OA");
        Check(GetState(tqm, "OB") == "pending", "OB still waits while OW writes");
        Check(WaitForState(tqm, "OB", "succeeded", 30000), "OB runs after OW");

        // Locks off (settings checkbox): conflicting writers run together.
        tqm.SetResourceLocksEnabled(false);
        Check(JsonGetBool(tqm.TaskQueueAdd("resoff1", one("OFF1", "write"), 2, "", err), "ok", false), "OFF1 added: " + err);
        Check(JsonGetBool(tqm.TaskQueueAdd("resoff2", one("OFF2", "write"), 2, "", err), "ok", false), "OFF2 added: " + err);
        Check(WaitForState(tqm, "OFF1", "running", 5000) && WaitForState(tqm, "OFF2", "running", 5000),
              "with resource locks off, conflicting writers run in parallel");
        Check(tqm.TaskQueueGraph("resoff1", "", err).find("\"resource_locks\":false") != std::string::npos,
              "graph reports resource_locks off");
        tqm.SetResourceLocksEnabled(true);
        Check(WaitForState(tqm, "OFF2", "succeeded", 20000), "lock-off writer finished");

        std::string bad = "[{\"id\":\"RB\",\"command\":\"cmd /c exit 0\",\"summary\":\"bad\",\"timeoutSec\":10,\"resources\":[\"x\"]}]";
        Check(!JsonGetBool(tqm.TaskQueueAdd("resw", bad, 0, "", err), "ok", false), "resources must be an object");
        std::string got = tqm.TaskQueueGet("W1", false, 0, err);
        Check(got.find("\"resources\":{\"write\":[\"shared.txt\"],\"read\":[]}") != std::string::npos, "resources returned by get");
        Check(got.find("\"defer\":{\"exit_codes\":[75],\"delay_sec\":60,\"limit\":60}") != std::string::npos,
              "defer defaults to exit code 75, 60 s, 60 times");
    }

    {
        // Deferral (exit 75 = try again later): back to pending after delaySec without using up
        // maxAttempts, until limit deferrals; then the exit code is an ordinary failure.
        auto count = [](const std::string& s, const std::string& what) {
            size_t n = 0;
            for (size_t p = s.find(what); p != std::string::npos; p = s.find(what, p + 1)) n++;
            return n;
        };
        std::string jobsD = R"([
          {"id":"D1","command":"cmd /c exit 75","summary":"always deferred","timeoutSec":30,"maxAttempts":1,
           "defer":{"delaySec":2,"limit":2}},
          {"id":"D2","command":"cmd /c exit 75","summary":"deferral off","timeoutSec":30,"maxAttempts":1,
           "defer":{"exitCodes":[]}}
        ])";
        Check(JsonGetBool(tqm.TaskQueueAdd("deferq", jobsD, 2, "", err), "ok", false), "defer jobs added: " + err);
        Check(WaitForState(tqm, "D2", "failed", 10000), "with defer.exitCodes [] exit 75 fails at once");
        Sleep(1000);
        Check(GetState(tqm, "D1") == "pending", "exit 75 puts the job back to pending (maxAttempts 1 not used up)");
        Check(tqm.TaskQueueGraph("deferq", "D1", err).find("\"retry_in_sec\":null") == std::string::npos,
              "graph shows retry_in_sec while a deferred job waits");
        Check(WaitForState(tqm, "D1", "failed", 20000), "after defer.limit deferrals exit 75 counts as a failure");
        std::string d1 = tqm.TaskQueueGet("D1", false, 0, err);
        Check(count(d1, "\"reason\":\"deferred\"") == 2 && count(d1, "\"reason\":\"failed\"") == 1,
              "two deferred attempts, then one failed attempt");
        std::string events = ReadFileUtf8Wide(TaskQueueManager::GetTaskEventsFileWide("deferq"));
        Check(count(events, "\"jobDeferred\"") == 2, "jobDeferred events emitted");
        Check(events.find("taskqueueStalled") == std::string::npos, "a queue waiting out a deferral is not stalled");

        std::string badDefer = R"([{"id":"D3","command":"cmd /c exit 0","summary":"bad","timeoutSec":10,"defer":{"exitCodes":[0]}}])";
        Check(!JsonGetBool(tqm.TaskQueueAdd("deferq", badDefer, 0, "", err), "ok", false), "defer.exitCodes may not contain the success code");
    }

    pm.SetProcessFinishedCallback(nullptr);
    tqm.ShutdownTaskQueueManager();

    // Restart while a max_attempts=1 job is running: the interrupted attempt must not
    // fail the job, and its dependent must stay pending instead of becoming blocked.
    {
        std::string restartState =
            "{\"name\":\"restartq\",\"concurrency\":1,\"paused\":false,\"version\":3,"
            "\"notify_file\":\"\",\"max_timeout_sec\":7200,\"default_max_attempts\":2,\"jobs\":["
            "{\"id\":\"R1\",\"taskqueue\":\"restartq\",\"command\":\"cmd /c exit 0\",\"timeout_sec\":30,"
            "\"max_attempts\":1,\"state\":\"running\",\"attempts\":[{\"n\":1,\"process_id\":999,"
            "\"start\":\"2026-01-01 00:00:00\",\"end\":\"\",\"reason\":\"\",\"exit_code\":-1}]},"
            "{\"id\":\"R2\",\"taskqueue\":\"restartq\",\"command\":\"cmd /c exit 0\",\"timeout_sec\":30,"
            "\"max_attempts\":1,\"state\":\"pending\",\"depends_on\":[{\"id\":\"R1\",\"on\":\"success\"}]}"
            "]}";
        WriteFileAtomicWide(TaskQueueManager::GetTaskQueueFileWide("restartq"), restartState);
        TaskQueueManager restarted;
        Check(GetState(restarted, "R1") == "pending", "interrupted running job returns to pending after restart");
        Check(GetState(restarted, "R2") == "pending", "dependent of interrupted job stays pending after restart");
        auto rj = ParseJobJson(restarted.TaskQueueGet("R1", false, 0, err));
        bool hasAttempt = rj.is_object() && rj.contains("attempts") &&
                          rj["attempts"].is_array() && !rj["attempts"].empty();
        Check(hasAttempt && rj["attempts"][0].contains("failure") &&
                  rj["attempts"][0]["failure"].is_object() &&
                  rj["attempts"][0]["failure"].value("class", std::string()) == "killed",
              "interrupted attempt diagnosed as killed");
        if (rj.is_object()) {
            TaskQueueRunJob interrupted = TaskQueueRunJobFromJson(rj);
            Check(interrupted.interrupted && TaskQueueRunExitCode({interrupted}) == 2,
                  "an interrupted run exits 2");
            std::string xml = TaskQueueRunJUnitXml("restartq", {interrupted});
            Check(xml.find("<skipped message=\"") != std::string::npos &&
                      xml.find("reason=interrupted") != std::string::npos,
                  "JUnit marks an interrupted job as skipped with the reason");
        }
        restarted.ShutdownTaskQueueManager();
    }

    // Remote reconnect after restart (056-1, doc/design-v3.md 2.5).
    {
        std::wstring hostsFile = (std::filesystem::path(GetEffectiveDataDirectoryWide()) / L"hosts.json").wstring();
        std::string prevHosts = ReadFileUtf8Wide(hostsFile);

        // Case 1: host not in hosts.json -> returns to pending (interrupted).
        {
            DeleteFileW(hostsFile.c_str());
            std::string qJson =
                "{\"name\":\"remote_reconnect_q\",\"concurrency\":1,\"paused\":false,\"version\":1,"
                "\"jobs\":[{\"id\":\"REM1\",\"taskqueue\":\"remote_reconnect_q\",\"command\":\"cmd /c exit 0\","
                "\"host\":\"box_unknown\",\"state\":\"running\",\"attempts\":[{"
                "\"n\":1,\"process_id\":0,\"remote_host\":\"box_unknown\",\"remote_process_id\":42,"
                "\"remote_job_id\":\"REM1\",\"remote_attempt\":1,"
                "\"start\":\"2026-01-01 00:00:00\",\"end\":\"\",\"reason\":\"\",\"exit_code\":-1}]}]}";
            WriteFileAtomicWide(TaskQueueManager::GetTaskQueueFileWide("remote_reconnect_q"), qJson);
            TaskQueueManager restarted;
            Check(GetState(restarted, "REM1") == "pending",
                  "remote running job whose host is not in hosts.json returns to pending on restart");
            auto rj = ParseJobJson(restarted.TaskQueueGet("REM1", false, 0, err));
            Check(rj.is_object() && rj["attempts"][0]["reason"] == "interrupted" &&
                      rj["attempts"][0].value("remote_job_id", "") == "REM1" &&
                      rj["attempts"][0].value("remote_attempt", 0) == 1,
                  "unreachable remote attempt preserved remote_job_id/remote_attempt and reason=interrupted");
            restarted.ShutdownTaskQueueManager();
        }

        // Case 2: host is in hosts.json, but reconnect fails (unreachable) -> interrupted.
        {
            std::string hostsJson =
                "{\"schema\":\"hancho.hosts/1\",\"hosts\":[{\"name\":\"box_offline\",\"address\":\"127.0.0.1:9137\",\"enabled\":true}]}";
            WriteFileAtomicWide(hostsFile, hostsJson);
            std::string qJson =
                "{\"name\":\"remote_reconnect_q\",\"concurrency\":1,\"paused\":false,\"version\":1,"
                "\"jobs\":[{\"id\":\"REM2\",\"taskqueue\":\"remote_reconnect_q\",\"command\":\"cmd /c exit 0\","
                "\"host\":\"box_offline\",\"state\":\"running\",\"attempts\":[{"
                "\"n\":1,\"process_id\":0,\"remote_host\":\"box_offline\",\"remote_process_id\":99,"
                "\"remote_job_id\":\"REM2\",\"remote_attempt\":1,"
                "\"start\":\"2026-01-01 00:00:00\",\"end\":\"\",\"reason\":\"\",\"exit_code\":-1}]}]}";
            WriteFileAtomicWide(TaskQueueManager::GetTaskQueueFileWide("remote_reconnect_q"), qJson);
            TaskQueueManager restarted;
            Check(GetState(restarted, "REM2") == "running",
                  "remote job with registered host stays running pending reconnect");
            restarted.SetRemoteCallbacks(
                nullptr,
                [](const std::string&, int, RemotePollResult&, std::string& errCb) {
                    errCb = "connection refused";
                    return false;
                },
                nullptr);
            Check(GetState(restarted, "REM2") == "pending",
                  "remote job whose host is unreachable becomes interrupted");
            auto rj = ParseJobJson(restarted.TaskQueueGet("REM2", false, 0, err));
            Check(rj.is_object() && rj["attempts"][0]["reason"] == "interrupted",
                  "failed reconnect marks attempt as interrupted");
            restarted.ShutdownTaskQueueManager();
        }

        // Case 3: host is in hosts.json and reachable (still running) -> reconnected, stays running.
        {
            std::string hostsJson =
                "{\"schema\":\"hancho.hosts/1\",\"hosts\":[{\"name\":\"box_online\",\"address\":\"127.0.0.1:9137\",\"enabled\":true}]}";
            WriteFileAtomicWide(hostsFile, hostsJson);
            std::string qJson =
                "{\"name\":\"remote_reconnect_q\",\"concurrency\":1,\"paused\":false,\"version\":1,"
                "\"jobs\":[{\"id\":\"REM3\",\"taskqueue\":\"remote_reconnect_q\",\"command\":\"cmd /c exit 0\","
                "\"host\":\"box_online\",\"state\":\"running\",\"attempts\":[{"
                "\"n\":1,\"process_id\":0,\"remote_host\":\"box_online\",\"remote_process_id\":100,"
                "\"remote_job_id\":\"REM3\",\"remote_attempt\":1,"
                "\"start\":\"2026-01-01 00:00:00\",\"end\":\"\",\"reason\":\"\",\"exit_code\":-1}]}]}";
            WriteFileAtomicWide(TaskQueueManager::GetTaskQueueFileWide("remote_reconnect_q"), qJson);
            TaskQueueManager restarted;
            bool pollCalled = false;
            restarted.SetRemoteCallbacks(
                nullptr,
                [&pollCalled](const std::string& host, int pid, RemotePollResult& res, std::string&) {
                    pollCalled = true;
                    Check(host == "box_online" && pid == 100, "reconnect targets expected host and pid");
                    res.finished = false;
                    return true;
                },
                nullptr);
            Check(pollCalled, "reconnect called remote poll");
            Check(GetState(restarted, "REM3") == "running",
                  "successfully reconnected remote job stays running");
            restarted.ShutdownTaskQueueManager();
        }

        // Case 4: host is in hosts.json and finished while Hancho was offline -> status, log, events, and .done restored.
        {
            std::string hostsJson =
                "{\"schema\":\"hancho.hosts/1\",\"hosts\":[{\"name\":\"box_online\",\"address\":\"127.0.0.1:9137\",\"enabled\":true}]}";
            WriteFileAtomicWide(hostsFile, hostsJson);
            std::string qJson =
                "{\"name\":\"remote_reconnect_q\",\"concurrency\":1,\"paused\":false,\"version\":1,"
                "\"jobs\":[{\"id\":\"REM4\",\"taskqueue\":\"remote_reconnect_q\",\"command\":\"cmd /c exit 0\","
                "\"host\":\"box_online\",\"state\":\"running\",\"attempts\":[{"
                "\"n\":1,\"process_id\":0,\"remote_host\":\"box_online\",\"remote_process_id\":101,"
                "\"remote_job_id\":\"REM4\",\"remote_attempt\":1,"
                "\"start\":\"2026-01-01 00:00:00\",\"end\":\"\",\"reason\":\"\",\"exit_code\":-1}]}]}";
            WriteFileAtomicWide(TaskQueueManager::GetTaskQueueFileWide("remote_reconnect_q"), qJson);
            TaskQueueManager restarted;
            const std::string expectedOutput = "remote task line 1\nremote task line 2\nremote task finished successfully\n";
            const std::string expectedEvents = "{\"schema\":\"hancho.event/1\",\"seq\":1,\"kind\":\"turn.started\"}\n{\"schema\":\"hancho.event/1\",\"seq\":2,\"kind\":\"turn.completed\"}\n";
            const std::string expectedDone = "{\"exit_code\":0,\"status\":\"ok\"}";
            restarted.SetRemoteCallbacks(
                nullptr,
                [&](const std::string&, int, RemotePollResult& res, std::string&) {
                    res.finished = true;
                    res.exitCode = 0;
                    res.endKind = "completed";
                    res.output = expectedOutput;
                    res.eventsJsonl = expectedEvents;
                    res.doneContent = expectedDone;
                    return true;
                },
                nullptr);
            Check(GetState(restarted, "REM4") == "succeeded",
                  "remote job that finished during restart is marked succeeded on reconnect");

            std::wstring logPath = TaskQueueManager::GetAttemptLogFileWide("REM4", 1);
            std::wstring eventsPath = logPath.substr(0, logPath.size() - 4) + L".events.jsonl";
            std::wstring donePath = logPath.substr(0, logPath.size() - 4) + L".done";
            std::wstring tailPath = logPath.substr(0, logPath.size() - 4) + L".tail.txt";

            Check(ReadFileUtf8Wide(logPath) == expectedOutput,
                  "reconnect restored remote attempt log to disk");
            Check(ReadFileUtf8Wide(eventsPath) == expectedEvents,
                  "reconnect restored remote attempt events to disk");
            Check(ReadFileUtf8Wide(donePath) == expectedDone,
                  "reconnect restored remote attempt .done file to disk");
            std::string tailContent = ReadFileUtf8Wide(tailPath);
            Check(!tailContent.empty() && tailContent.find("remote task finished successfully") != std::string::npos,
                  "reconnect restored remote attempt tail file");

            auto rj = ParseJobJson(restarted.TaskQueueGet("REM4", false, 0, err));
            Check(rj.is_object() && rj["state"] == "succeeded", "reconnected job state is succeeded in list");
            Check(rj["attempts"].is_array() && !rj["attempts"].empty(), "reconnected job has attempts");
            const auto& a0 = rj["attempts"][0];
            Check(a0.value("reason", "") == "ok" && a0.value("exit_code", -1) == 0,
                  "reconnected attempt has reason=ok and exit_code=0");
            Check(a0.value("remote_host", "") == "box_online" &&
                  a0.value("remote_job_id", "") == "REM4" &&
                  a0.value("remote_attempt", 0) == 1 &&
                  a0.value("remote_process_id", 0) == 101,
                  "reconnected attempt preserved remote host, process ID, job ID, and attempt number");
            Check(!a0.value("end", "").empty(), "reconnected attempt has recorded end timestamp");

            restarted.ShutdownTaskQueueManager();

            // Verify persistence: restart again, job and its restored state remain intact.
            {
                TaskQueueManager reloaded;
                Check(GetState(reloaded, "REM4") == "succeeded",
                      "reconnected job remains succeeded across subsequent restart");
                auto rjReloaded = ParseJobJson(reloaded.TaskQueueGet("REM4", false, 0, err));
                Check(rjReloaded.is_object() && rjReloaded["attempts"][0].value("reason", "") == "ok",
                      "persisted attempt remains intact across subsequent restart");
                reloaded.ShutdownTaskQueueManager();
            }

            DeleteFileW(logPath.c_str());
            DeleteFileW(eventsPath.c_str());
            DeleteFileW(donePath.c_str());
            DeleteFileW(tailPath.c_str());
        }

        // Case 5: remote job continues running across restart, then finishes on subsequent poll.
        {
            std::string hostsJson =
                "{\"schema\":\"hancho.hosts/1\",\"hosts\":[{\"name\":\"box_online\",\"address\":\"127.0.0.1:9137\",\"enabled\":true}]}";
            WriteFileAtomicWide(hostsFile, hostsJson);
            std::string qJson =
                "{\"name\":\"remote_reconnect_q\",\"concurrency\":1,\"paused\":false,\"version\":1,"
                "\"jobs\":[{\"id\":\"REM5\",\"taskqueue\":\"remote_reconnect_q\",\"command\":\"cmd /c exit 0\","
                "\"host\":\"box_online\",\"state\":\"running\",\"attempts\":[{"
                "\"n\":1,\"process_id\":0,\"remote_host\":\"box_online\",\"remote_process_id\":102,"
                "\"remote_job_id\":\"REM5\",\"remote_attempt\":1,"
                "\"start\":\"2026-01-01 00:00:00\",\"end\":\"\",\"reason\":\"\",\"exit_code\":-1}]}]}";
            WriteFileAtomicWide(TaskQueueManager::GetTaskQueueFileWide("remote_reconnect_q"), qJson);
            TaskQueueManager restarted;
            int pollCount = 0;
            const std::string expectedOutput = "REM5 output log\nfinished\n";
            const std::string expectedEvents = "{\"schema\":\"hancho.event/1\",\"seq\":1,\"kind\":\"turn.completed\"}\n";
            const std::string expectedDone = "{\"exit_code\":0}";
            restarted.SetRemoteCallbacks(
                nullptr,
                [&](const std::string&, int, RemotePollResult& res, std::string&) {
                    pollCount++;
                    if (pollCount == 1) {
                        res.finished = false;
                        return true;
                    }
                    res.finished = true;
                    res.exitCode = 0;
                    res.endKind = "completed";
                    res.output = expectedOutput;
                    res.eventsJsonl = expectedEvents;
                    res.doneContent = expectedDone;
                    return true;
                },
                nullptr);
            Check(GetState(restarted, "REM5") == "running",
                  "reconnected remote job that is still running remains running");
            restarted.PollRemoteJobs();
            Check(GetState(restarted, "REM5") == "succeeded",
                  "reconnected job marked succeeded after completing on subsequent poll");

            std::wstring logPath = TaskQueueManager::GetAttemptLogFileWide("REM5", 1);
            std::wstring eventsPath = logPath.substr(0, logPath.size() - 4) + L".events.jsonl";
            std::wstring donePath = logPath.substr(0, logPath.size() - 4) + L".done";
            std::wstring tailPath = logPath.substr(0, logPath.size() - 4) + L".tail.txt";

            Check(ReadFileUtf8Wide(logPath) == expectedOutput,
                  "subsequent poll restored remote attempt log to disk");
            Check(ReadFileUtf8Wide(eventsPath) == expectedEvents,
                  "subsequent poll restored remote attempt events to disk");
            Check(ReadFileUtf8Wide(donePath) == expectedDone,
                  "subsequent poll restored remote attempt .done file to disk");

            restarted.ShutdownTaskQueueManager();
            DeleteFileW(logPath.c_str());
            DeleteFileW(eventsPath.c_str());
            DeleteFileW(donePath.c_str());
            DeleteFileW(tailPath.c_str());
        }

        // Case 6: remote job that failed on the host while Hancho was offline -> restored as failed with failure logs.
        {
            std::string hostsJson =
                "{\"schema\":\"hancho.hosts/1\",\"hosts\":[{\"name\":\"box_online\",\"address\":\"127.0.0.1:9137\",\"enabled\":true}]}";
            WriteFileAtomicWide(hostsFile, hostsJson);
            std::string qJson =
                "{\"name\":\"remote_reconnect_q\",\"concurrency\":1,\"paused\":false,\"version\":1,"
                "\"jobs\":[{\"id\":\"REM6\",\"taskqueue\":\"remote_reconnect_q\",\"command\":\"cmd /c exit 0\",\"max_attempts\":1,"
                "\"host\":\"box_online\",\"state\":\"running\",\"attempts\":[{"
                "\"n\":1,\"process_id\":0,\"remote_host\":\"box_online\",\"remote_process_id\":103,"
                "\"remote_job_id\":\"REM6\",\"remote_attempt\":1,"
                "\"start\":\"2026-01-01 00:00:00\",\"end\":\"\",\"reason\":\"\",\"exit_code\":-1}]}]}";
            WriteFileAtomicWide(TaskQueueManager::GetTaskQueueFileWide("remote_reconnect_q"), qJson);
            TaskQueueManager restarted;
            const std::string failOutput = "fatal error: remote build compilation failed\n";
            restarted.SetRemoteCallbacks(
                nullptr,
                [&](const std::string&, int, RemotePollResult& res, std::string&) {
                    res.finished = true;
                    res.exitCode = 1;
                    res.endKind = "failed";
                    res.output = failOutput;
                    res.doneContent = "{\"exit_code\":1,\"status\":\"failed\"}";
                    return true;
                },
                nullptr);
            Check(GetState(restarted, "REM6") == "failed",
                  "remote job that failed during restart is restored with failed state on reconnect");
            std::wstring logPath = TaskQueueManager::GetAttemptLogFileWide("REM6", 1);
            Check(ReadFileUtf8Wide(logPath) == failOutput,
                  "reconnect restored failed remote attempt log to disk");
            auto rj = ParseJobJson(restarted.TaskQueueGet("REM6", false, 0, err));
            Check(rj.is_object() && rj["attempts"][0].value("exit_code", -1) == 1,
                  "restored failed remote attempt preserves exit code 1");
            restarted.ShutdownTaskQueueManager();
            DeleteFileW(logPath.c_str());
            std::wstring donePath = logPath.substr(0, logPath.size() - 4) + L".done";
            std::wstring tailPath = logPath.substr(0, logPath.size() - 4) + L".tail.txt";
            DeleteFileW(donePath.c_str());
            DeleteFileW(tailPath.c_str());
        }

        if (!prevHosts.empty()) {
            WriteFileAtomicWide(hostsFile, prevHosts);
        } else {
            DeleteFileW(hostsFile.c_str());
        }
        DeleteFileW(TaskQueueManager::GetTaskQueueFileWide("remote_reconnect_q").c_str());
    }

    // Remote success.filesNewer evaluation (057, doc/design-v3-todo.md 6).
    {
        std::wstring hostsFile = (std::filesystem::path(GetEffectiveDataDirectoryWide()) / L"hosts.json").wstring();
        std::string prevHosts = ReadFileUtf8Wide(hostsFile);
        std::string hostsJson =
            "{\"schema\":\"hancho.hosts/1\",\"hosts\":[{\"name\":\"box_online\",\"address\":\"127.0.0.1:9137\",\"enabled\":true}]}";
        WriteFileAtomicWide(hostsFile, hostsJson);

        // Case 1: Remote job with files_newer succeeds when host reports files are newer.
        {
            std::string qJson =
                "{\"name\":\"remote_fn_q\",\"concurrency\":1,\"paused\":false,\"version\":1,"
                "\"jobs\":[{\"id\":\"RFN1\",\"taskqueue\":\"remote_fn_q\",\"command\":\"cmd /c exit 0\","
                "\"host\":\"box_online\",\"state\":\"running\","
                "\"success\":{\"exit_code\":0,\"files_newer\":[\"artifact_ok.txt\"]},"
                "\"attempts\":[{"
                "\"n\":1,\"process_id\":0,\"remote_host\":\"box_online\",\"remote_process_id\":201,"
                "\"remote_job_id\":\"RFN1\",\"remote_attempt\":1,"
                "\"start\":\"2026-01-01 00:00:00\",\"end\":\"\",\"reason\":\"\",\"exit_code\":-1}]}]}";
            WriteFileAtomicWide(TaskQueueManager::GetTaskQueueFileWide("remote_fn_q"), qJson);
            TaskQueueManager restarted;
            bool checkCalled = false;
            std::string checkedHost;
            std::vector<std::string> checkedFiles;
            SetRemoteFilesCheckCallback([&](const std::string& host, const std::string&,
                                            int pid, const std::vector<std::string>& files,
                                            uint64_t, std::string& outDetail) -> bool {
                checkCalled = true;
                checkedHost = host;
                checkedFiles = files;
                (void)pid;
                (void)outDetail;
                return true;
            });
            restarted.SetRemoteCallbacks(
                nullptr,
                [&](const std::string&, int, RemotePollResult& res, std::string&) {
                    res.finished = true;
                    res.exitCode = 0;
                    res.endKind = "completed";
                    res.output = "build completed successfully\n";
                    res.doneContent = "{\"exit_code\":0,\"status\":\"ok\"}";
                    return true;
                },
                nullptr);
            Check(checkCalled && checkedHost == "box_online", "remote files_newer calls host check callback");
            Check(checkedFiles.size() == 1 && checkedFiles[0] == "artifact_ok.txt", "remote files_newer checks configured path");
            Check(GetState(restarted, "RFN1") == "succeeded", "remote job with satisfied files_newer succeeds");

            restarted.ShutdownTaskQueueManager();
            std::wstring logPath = TaskQueueManager::GetAttemptLogFileWide("RFN1", 1);
            DeleteFileW(logPath.c_str());
            std::wstring donePath = logPath.substr(0, logPath.size() - 4) + L".done";
            std::wstring tailPath = logPath.substr(0, logPath.size() - 4) + L".tail.txt";
            DeleteFileW(donePath.c_str());
            DeleteFileW(tailPath.c_str());
        }

        // Case 2: Remote job with files_newer fails when host reports missing file.
        {
            std::string qJson =
                "{\"name\":\"remote_fn_q\",\"concurrency\":1,\"paused\":false,\"version\":1,"
                "\"jobs\":[{\"id\":\"RFN2\",\"taskqueue\":\"remote_fn_q\",\"command\":\"cmd /c exit 0\",\"max_attempts\":1,"
                "\"host\":\"box_online\",\"state\":\"running\","
                "\"success\":{\"exit_code\":0,\"files_newer\":[\"missing_artifact.txt\"]},"
                "\"attempts\":[{"
                "\"n\":1,\"process_id\":0,\"remote_host\":\"box_online\",\"remote_process_id\":202,"
                "\"remote_job_id\":\"RFN2\",\"remote_attempt\":1,"
                "\"start\":\"2026-01-01 00:00:00\",\"end\":\"\",\"reason\":\"\",\"exit_code\":-1}]}]}";
            WriteFileAtomicWide(TaskQueueManager::GetTaskQueueFileWide("remote_fn_q"), qJson);
            TaskQueueManager restarted;
            SetRemoteFilesCheckCallback([&](const std::string&, const std::string&,
                                            int, const std::vector<std::string>& files,
                                            uint64_t, std::string& outDetail) -> bool {
                outDetail = "missing file: " + (files.empty() ? "" : files[0]);
                return false;
            });
            restarted.SetRemoteCallbacks(
                nullptr,
                [&](const std::string&, int, RemotePollResult& res, std::string&) {
                    res.finished = true;
                    res.exitCode = 0;
                    res.endKind = "completed";
                    res.output = "build finished without producing output\n";
                    res.doneContent = "{\"exit_code\":0,\"status\":\"ok\"}";
                    return true;
                },
                nullptr);
            Check(GetState(restarted, "RFN2") == "failed", "remote job fails when files_newer file is missing on host");
            std::string err;
            auto rj = ParseJobJson(restarted.TaskQueueGet("RFN2", false, 0, err));
            Check(rj.is_object() && rj["attempts"][0].value("reason", "") == "checkFailed",
                  "attempt reason is checkFailed on missing files_newer");
            if (rj.is_object() && rj.contains("diagnostic") && rj["diagnostic"].is_object()) {
                const auto& diag = rj["diagnostic"];
                Check(diag.value("class", "") == "success_check_failed",
                      "failure diagnostic class is success_check_failed");
                Check(diag.value("detail", nlohmann::json::object()).value("condition", "") == "filesNewer",
                      "failure diagnostic condition is filesNewer");
            }

            restarted.ShutdownTaskQueueManager();
            std::wstring logPath = TaskQueueManager::GetAttemptLogFileWide("RFN2", 1);
            DeleteFileW(logPath.c_str());
            std::wstring donePath = logPath.substr(0, logPath.size() - 4) + L".done";
            std::wstring tailPath = logPath.substr(0, logPath.size() - 4) + L".tail.txt";
            DeleteFileW(donePath.c_str());
            DeleteFileW(tailPath.c_str());
        }

        // Case 3: Remote job with files_newer fails when host reports file not newer than start.
        {
            std::string qJson =
                "{\"name\":\"remote_fn_q\",\"concurrency\":1,\"paused\":false,\"version\":1,"
                "\"jobs\":[{\"id\":\"RFN3\",\"taskqueue\":\"remote_fn_q\",\"command\":\"cmd /c exit 0\",\"max_attempts\":1,"
                "\"host\":\"box_online\",\"state\":\"running\","
                "\"success\":{\"exit_code\":0,\"files_newer\":[\"stale_artifact.txt\"]},"
                "\"attempts\":[{"
                "\"n\":1,\"process_id\":0,\"remote_host\":\"box_online\",\"remote_process_id\":203,"
                "\"remote_job_id\":\"RFN3\",\"remote_attempt\":1,"
                "\"start\":\"2026-01-01 00:00:00\",\"end\":\"\",\"reason\":\"\",\"exit_code\":-1}]}]}";
            WriteFileAtomicWide(TaskQueueManager::GetTaskQueueFileWide("remote_fn_q"), qJson);
            TaskQueueManager restarted;
            SetRemoteFilesCheckCallback([&](const std::string&, const std::string&,
                                            int, const std::vector<std::string>& files,
                                            uint64_t, std::string& outDetail) -> bool {
                outDetail = "file not newer than attempt start: " + (files.empty() ? "" : files[0]);
                return false;
            });
            restarted.SetRemoteCallbacks(
                nullptr,
                [&](const std::string&, int, RemotePollResult& res, std::string&) {
                    res.finished = true;
                    res.exitCode = 0;
                    res.endKind = "completed";
                    res.output = "build exited without touching stale file\n";
                    res.doneContent = "{\"exit_code\":0,\"status\":\"ok\"}";
                    return true;
                },
                nullptr);
            Check(GetState(restarted, "RFN3") == "failed", "remote job fails when files_newer file is not newer than start");
            std::string err;
            auto rj = ParseJobJson(restarted.TaskQueueGet("RFN3", false, 0, err));
            Check(rj.is_object() && rj["attempts"][0].value("reason", "") == "checkFailed",
                  "attempt reason is checkFailed on stale files_newer");

            restarted.ShutdownTaskQueueManager();
            std::wstring logPath = TaskQueueManager::GetAttemptLogFileWide("RFN3", 1);
            DeleteFileW(logPath.c_str());
            std::wstring donePath = logPath.substr(0, logPath.size() - 4) + L".done";
            std::wstring tailPath = logPath.substr(0, logPath.size() - 4) + L".tail.txt";
            DeleteFileW(donePath.c_str());
            DeleteFileW(tailPath.c_str());
        }

        // Case 4: Remote job files_newer evaluated from .done file content without callback.
        {
            std::string qJson =
                "{\"name\":\"remote_fn_q\",\"concurrency\":1,\"paused\":false,\"version\":1,"
                "\"jobs\":[{\"id\":\"RFN4\",\"taskqueue\":\"remote_fn_q\",\"command\":\"cmd /c exit 0\","
                "\"host\":\"box_online\",\"state\":\"running\","
                "\"success\":{\"exit_code\":0,\"files_newer\":[\"done_embedded.txt\"]},"
                "\"attempts\":[{"
                "\"n\":1,\"process_id\":0,\"remote_host\":\"box_online\",\"remote_process_id\":204,"
                "\"remote_job_id\":\"RFN4\",\"remote_attempt\":1,"
                "\"start\":\"2026-01-01 00:00:00\",\"end\":\"\",\"reason\":\"\",\"exit_code\":-1}]}]}";
            WriteFileAtomicWide(TaskQueueManager::GetTaskQueueFileWide("remote_fn_q"), qJson);
            TaskQueueManager restarted;
            SetRemoteFilesCheckCallback(nullptr); // No callback installed
            restarted.SetRemoteCallbacks(
                nullptr,
                [&](const std::string&, int, RemotePollResult& res, std::string&) {
                    res.finished = true;
                    res.exitCode = 0;
                    res.endKind = "completed";
                    res.output = "build done\n";
                    res.doneContent = "{\"exit_code\":0,\"files_newer\":{\"success\":true}}";
                    return true;
                },
                nullptr);
            Check(GetState(restarted, "RFN4") == "succeeded",
                  "remote job files_newer succeeded directly via .done file metadata");

            restarted.ShutdownTaskQueueManager();
            std::wstring logPath = TaskQueueManager::GetAttemptLogFileWide("RFN4", 1);
            DeleteFileW(logPath.c_str());
            std::wstring donePath = logPath.substr(0, logPath.size() - 4) + L".done";
            std::wstring tailPath = logPath.substr(0, logPath.size() - 4) + L".tail.txt";
            DeleteFileW(donePath.c_str());
            DeleteFileW(tailPath.c_str());
        }

        // Case 5: When neither callback nor .done metadata is available, remote files_newer fails gracefully.
        {
            std::string qJson =
                "{\"name\":\"remote_fn_q\",\"concurrency\":1,\"paused\":false,\"version\":1,"
                "\"jobs\":[{\"id\":\"RFN5\",\"taskqueue\":\"remote_fn_q\",\"command\":\"cmd /c exit 0\",\"max_attempts\":1,"
                "\"host\":\"box_online\",\"state\":\"running\","
                "\"success\":{\"exit_code\":0,\"files_newer\":[\"no_cb.txt\"]},"
                "\"attempts\":[{"
                "\"n\":1,\"process_id\":0,\"remote_host\":\"box_online\",\"remote_process_id\":205,"
                "\"remote_job_id\":\"RFN5\",\"remote_attempt\":1,"
                "\"start\":\"2026-01-01 00:00:00\",\"end\":\"\",\"reason\":\"\",\"exit_code\":-1}]}]}";
            WriteFileAtomicWide(TaskQueueManager::GetTaskQueueFileWide("remote_fn_q"), qJson);
            TaskQueueManager restarted;
            SetRemoteFilesCheckCallback(nullptr);
            restarted.SetRemoteCallbacks(
                nullptr,
                [&](const std::string&, int, RemotePollResult& res, std::string&) {
                    res.finished = true;
                    res.exitCode = 0;
                    res.endKind = "completed";
                    res.output = "build done without remote files check\n";
                    res.doneContent = "{\"exit_code\":0,\"status\":\"ok\"}";
                    return true;
                },
                nullptr);
            Check(GetState(restarted, "RFN5") == "failed",
                  "remote job fails when remote files check mechanism is unavailable");

            restarted.ShutdownTaskQueueManager();
            std::wstring logPath = TaskQueueManager::GetAttemptLogFileWide("RFN5", 1);
            DeleteFileW(logPath.c_str());
            std::wstring donePath = logPath.substr(0, logPath.size() - 4) + L".done";
            std::wstring tailPath = logPath.substr(0, logPath.size() - 4) + L".tail.txt";
            DeleteFileW(donePath.c_str());
            DeleteFileW(tailPath.c_str());
        }

        SetRemoteFilesCheckCallback(nullptr);
        if (!prevHosts.empty()) {
            WriteFileAtomicWide(hostsFile, prevHosts);
        } else {
            DeleteFileW(hostsFile.c_str());
        }
        DeleteFileW(TaskQueueManager::GetTaskQueueFileWide("remote_fn_q").c_str());
    }

    {
        // agy reports usage per step; totals must be the sum, not the high-water mark.
        SetUsageHistoryPersistence(true);
        int64_t since = static_cast<int64_t>(time(nullptr)) - 5;
        LlmStreamTracker tr;
        tr.Init(9001, "agy", "");
        std::string lines =
            "{\"event\":\"init\",\"init\":{\"conversation_id\":\"smoke-no-transcript\"}}\n"
            "{\"event\":\"step_update\",\"step_update\":{\"conversation_id\":\"smoke-no-transcript\",\"step_index\":1,\"state\":\"DONE\",\"step_type\":\"agent_response\","
            "\"usage\":{\"input_tokens\":7422,\"output_tokens\":677,\"thinking_tokens\":249,\"cache_read_tokens\":0,\"total_tokens\":8099}}}\n"
            "{\"event\":\"step_update\",\"step_update\":{\"conversation_id\":\"smoke-no-transcript\",\"step_index\":2,\"state\":\"DONE\",\"step_type\":\"agent_response\","
            "\"usage\":{\"input_tokens\":2675,\"output_tokens\":122,\"thinking_tokens\":48,\"cache_read_tokens\":128496,\"total_tokens\":2797}}}\n"
            "{\"event\":\"result\",\"result\":{\"conversation_id\":\"smoke-no-transcript\",\"status\":\"SUCCESS\",\"response\":\"ok\","
            "\"usage\":{\"input_tokens\":10097,\"output_tokens\":799,\"thinking_tokens\":297,\"cache_read_tokens\":128496,\"total_tokens\":10896}}}\n";
        tr.IngestChunk(lines.data(), lines.size());
        Check(tr.GetState().totalTokens == 10896, "agy per-step usage sums into state total");
        uint64_t histTotal = 0;
        for (const auto& p : GetLlmUsageHistory(since)) {
            if (p.agentType == "agy") histTotal += p.totalTokens;
        }
        Check(histTotal == 10896, "agy per-step usage recorded once per step in usage history");
        std::string hist = ReadFileUtf8Wide(dir + L"\\usage_history.jsonl");
        size_t nl = 0;
        for (char c : hist) if (c == '\n') nl++;
        Check(nl == 2 && hist.find("\\n") == std::string::npos, "usage_history.jsonl written in logs dir, one line per point");
        SetUsageHistoryPersistence(false);
    }

    {
        // Claude --include-partial-messages: chunks update the live state but are not stored, and usage
        // counts once per message (message_delta) with the result total filling in what was not reported.
        LlmStreamTracker tr;
        tr.Init(9002, "claude", "");
        std::string head =
            "{\"type\":\"stream_event\",\"event\":{\"type\":\"message_start\",\"message\":{\"model\":\"claude-test\",\"id\":\"m1\"}}}\n"
            "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}}\n"
            "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hel\"}}}\n"
            "{\"type\":\"stream_event\",\"event\":{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"lo\"}}}\n";
        tr.IngestChunk(head.data(), head.size());
        Check(tr.EventCount() == 0 && tr.GetState().lastMessage == "Hello" && tr.GetState().model == "claude-test",
              "claude partial chunks update the live state and are not stored");
        std::string rest =
            "{\"type\":\"assistant\",\"message\":{\"id\":\"m1\",\"model\":\"claude-test\",\"content\":[{\"type\":\"text\",\"text\":\"Hello\"}],"
            "\"usage\":{\"input_tokens\":2,\"output_tokens\":1}}}\n"
            "{\"type\":\"stream_event\",\"event\":{\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},"
            "\"usage\":{\"input_tokens\":2,\"output_tokens\":89,\"output_tokens_details\":{\"thinking_tokens\":40}}},\"api_message_id\":\"m1\"}\n"
            "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,\"total_cost_usd\":0.01,\"num_turns\":1,"
            "\"result\":\"Hello\",\"usage\":{\"input_tokens\":2,\"output_tokens\":89}}\n";
        tr.IngestChunk(rest.data(), rest.size());
        Check(tr.GetState().totalTokens == 91, "claude usage counts each message once and fills gaps from the result total");
        Check(tr.EventCount() == 2, "claude complete blocks are stored, partial lines are not");
    }

    {
        // CI one-shot run (doc/design-v3.md 2.3): the three exit codes and the JUnit report.
        std::vector<TaskQueueRunJob> empty;
        Check(TaskQueueRunDrained(empty), "an empty queue is already drained");
        Check(TaskQueueRunExitCode(empty) == 0, "an empty queue exits 0");
        std::string emptyXml = TaskQueueRunJUnitXml("q", empty);
        Check(emptyXml.find("tests=\"0\"") != std::string::npos &&
                  emptyXml.find("<testsuite") != std::string::npos,
              "an empty queue still writes a valid JUnit suite");

        TaskQueueRunJob ok;
        ok.id = "ok";
        ok.state = "succeeded";
        Check(TaskQueueRunDrained({ok}) && TaskQueueRunExitCode({ok}) == 0,
              "all succeeded exits 0");

        TaskQueueRunJob failed;
        failed.id = "build";
        failed.name = "build";
        failed.state = "failed";
        failed.reason = "failed";
        failed.exitCode = 1;
        failed.failureClass = "exit_nonzero";
        failed.failureSummary = "command exited with 1";
        Check(TaskQueueRunDrained({failed}), "failed jobs are terminal");
        Check(TaskQueueRunExitCode({failed}) == 1, "a failed job exits 1");
        std::string failureXml = TaskQueueRunJUnitXml("q", {failed});
        Check(failureXml.find("<failure message=\"") != std::string::npos &&
                  failureXml.find("reason=failed") != std::string::npos &&
                  failureXml.find("exitCode=1") != std::string::npos &&
                  failureXml.find("class=exit_nonzero") != std::string::npos,
              "JUnit failure message carries reason, exitCode and diagnosis class: " + failureXml);

        TaskQueueRunJob blocked;
        blocked.id = "after";
        blocked.state = "blocked";
        blocked.blockedBy = "build";
        Check(TaskQueueRunExitCode({blocked}) == 1, "a blocked dependent exits 1");
        Check(TaskQueueRunJUnitXml("q", {blocked}).find("blocked by build") != std::string::npos,
              "JUnit names the blocking job");

        TaskQueueRunJob cancelled;
        cancelled.id = "cancelled";
        cancelled.state = "cancelled";
        cancelled.reason = "killed";
        Check(TaskQueueRunExitCode({cancelled}) == 2, "a cancelled job exits 2");
        Check(TaskQueueRunJUnitXml("q", {cancelled}).find("<skipped message=\"") != std::string::npos,
              "JUnit marks a cancelled job as skipped");

        TaskQueueRunJob pending;
        pending.id = "dep";
        pending.state = "pending";
        Check(!TaskQueueRunDrained({pending}), "a pending dependency is not drained");

        TaskQueueRunJob interrupted;
        interrupted.id = "int";
        interrupted.state = "pending";
        interrupted.reason = "interrupted";
        interrupted.interrupted = true;
        Check(TaskQueueRunExitCode({interrupted}) == 2, "an interrupted job exits 2");

        auto parsed = TaskQueueRunJobFromJson(nlohmann::json::parse(
            R"({"id":"J","name":"job","summary":"s","state":"failed","attempts":)"
            R"([{"reason":"failed","exit_code":7,"failure":{"class":"exit_nonzero","summary":"boom"}}]})",
            nullptr, false));
        Check(parsed.id == "J" && parsed.exitCode == 7 && parsed.failureClass == "exit_nonzero" &&
                  parsed.reason == "failed",
              "run jobs are built from the taskqueue_get shape");

        TaskQueueRunJob amp;
        amp.id = "a&b<c>";
        amp.state = "succeeded";
        std::string escapedXml = TaskQueueRunJUnitXml("q", {amp});
        Check(escapedXml.find("a&amp;b&lt;c&gt;") != std::string::npos,
              "JUnit XML escapes the job id");
    }

    {
        // Resource overlap rules (doc/resource-locks.md).
        Check(NormalizeResource("src\\A.cpp", "C:\\Proj\\") == "c:/proj/src/a.cpp", "relative resource resolves against cwd");
        Check(NormalizeResource("./src/../doc/x.md", "c:/p") == "c:/p/doc/x.md", "resource . and .. are resolved");
        Check(NormalizeResource("@Build", "c:/p") == "@build", "named lock is not resolved against cwd");
        Check(NormalizeResource("Foo.cs", "c:/p") == "c:/p/**/foo.cs", "bare file name matches anywhere below cwd (as ww)");
        Check(NormalizeResource("*.md", "") == "**/*.md", "bare glob matches anywhere");
        Check(NormalizeResource("**", "c:/p") == "c:/p/**", "** stays the whole cwd");
        Check(ResourcesOverlap(NormalizeResource("Foo.cs", "c:/p"), NormalizeResource("Assets/x/Foo.cs", "c:/p")),
              "bare file name overlaps the same file in a subfolder");
        Check(!ResourcesOverlap(NormalizeResource("Foo.cs", "c:/p"), NormalizeResource("src/b.cpp", "c:/p")),
              "bare file name does not overlap another file");
        // Same cases as tools/ww/ww-run.selftest.ps1 (Test-PathPatternOverlap), so both agree.
        Check(!ResourcesOverlap("c:/p/src/a.cpp", "c:/p/src/b.cpp"), "different files do not overlap");
        Check(ResourcesOverlap("c:/p/**/foo.cs", "c:/p/src/x/foo.cs") && ResourcesOverlap("c:/p/**/foo.cs", "c:/p/foo.cs"),
              "bare file name overlaps itself at any depth");
        Check(!ResourcesOverlap("c:/p/**/foo.cs", "c:/p/**/bar.cs"), "two different bare file names do not overlap");
        Check(ResourcesOverlap("c:/p/**", "c:/p/x/y.cpp") && !ResourcesOverlap("c:/p/doc/**", "c:/p/src/a.cpp"),
              "** under a directory covers only that directory");
        Check(ResourcesOverlap("c:/p/src/**/*.cpp", "c:/p/src/x/a.cpp") && !ResourcesOverlap("c:/p/src/**/*.cpp", "c:/p/src/x/a.h"),
              "**/*.cpp matches only .cpp files");
        Check(ResourcesOverlap("c:/p/src", "c:/p/src/a.cpp"), "directory covers its files");
        Check(!ResourcesOverlap("c:/p/src", "c:/p/src2/a.cpp"), "sibling with common prefix does not overlap");
        Check(ResourcesOverlap("c:/p/src/**/*.cpp", "c:/p/src/x/y.cpp"), "** glob matches nested file");
        Check(!ResourcesOverlap("c:/p/src/*.cpp", "c:/p/src/a.h"), "* glob does not match other extension");
        Check(!ResourcesOverlap("c:/p/src/*.cpp", "c:/p/src/x/a.cpp"), "* glob does not cross /");
        Check(ResourcesOverlap("c:/p/**/foo.cs", "c:/p/src"), "glob that can match below a directory overlaps it");
        Check(ResourcesOverlap("c:/p/src/*", "c:/p/src/x/a.cpp"), "glob matching an ancestor directory overlaps");
        Check(ResourcesOverlap("c:/p/doc/**", "c:/p/doc/*.md"), "glob x glob with nested literal prefixes overlaps");
        Check(!ResourcesOverlap("c:/p/doc/**", "c:/p/src/*.md"), "glob x glob with disjoint prefixes does not overlap");
        Check(!ResourcesOverlap("@build", "c:/p/build"), "named lock never overlaps a path");

        TaskQueueResources w1, w2, r1, r2;
        w1.write = {"src/a.cpp"};
        w2.write = {"src/**"};
        r1.read = {"src/a.cpp"};
        r2.read = {"src/**"};
        std::string res;
        Check(FindResourceConflict(w1, "c:/p", w2, "c:/p", res), "write x write conflicts");
        Check(FindResourceConflict(w1, "c:/p", r2, "c:/p", res), "write x read conflicts");
        Check(FindResourceConflict(r1, "c:/p", w2, "c:/p", res), "read x write conflicts");
        Check(!FindResourceConflict(r1, "c:/p", r2, "c:/p", res), "read x read does not conflict");
        Check(!FindResourceConflict(w1, "c:/p", w2, "c:/p/.claude/worktrees/x", res), "same relative path in another cwd does not conflict");
    }

    {
        // Scoped named locks: @name:{root} is one lock per git checkout / worktree.
        wchar_t tmpW[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tmpW);
        std::wstring base = std::wstring(tmpW) + L"hancho-root-" + std::to_wstring(GetCurrentProcessId());
        CreateDirectoryW(base.c_str(), nullptr);
        CreateDirectoryW((base + L"/main").c_str(), nullptr);
        CreateDirectoryW((base + L"/main/.git").c_str(), nullptr);
        CreateDirectoryW((base + L"/main/sub").c_str(), nullptr);
        CreateDirectoryW((base + L"/wt").c_str(), nullptr);
        WriteFileAtomicWide(base + L"/wt/.git", "gitdir: elsewhere\n");
        CreateDirectoryW((base + L"/wt/sub").c_str(), nullptr);
        std::string mainSub = WideToUtf8(base + L"/main/sub");
        std::string mainDir = WideToUtf8(base + L"/main");
        std::string wtSub = WideToUtf8(base + L"/wt/sub");
        std::string rootMain = NormalizeResource("@Editor:{root}", mainSub);
        Check(rootMain == "@editor:" + NormalizeResource(mainDir, ""), "{root} becomes the git top level: " + rootMain);
        Check(NormalizeResource("@editor:{root}", mainDir) == rootMain, "{root} is the same from the top and a subfolder");
        Check(NormalizeResource("@editor:{root}", wtSub) != rootMain, "a worktree (.git file) is its own root");
        Check(NormalizeResource("@editor:{cwd}", mainSub) == "@editor:" + NormalizeResource(mainSub, ""), "{cwd} becomes the cwd");
        Check(NormalizeResource("@editor:{root}", "c:/no/such/dir", "remotebox") == "@editor:c:/no/such/dir",
              "{root} on a remote host stays the cwd");
        TaskQueueResources e1;
        e1.write = {"@editor:{root}"};
        std::string res;
        Check(FindResourceConflict(e1, mainSub, e1, mainDir, res), "same checkout -> scoped locks conflict");
        Check(!FindResourceConflict(e1, mainSub, e1, wtSub, res), "different worktrees -> scoped locks do not conflict");
        TaskQueueResources g1;
        g1.write = {"@editor"};
        Check(FindResourceConflict(g1, mainSub, g1, wtSub, res), "unscoped named lock still conflicts across worktrees");
    }

    {
        // Test report parser (success.testReport).
        TestReportSummary sum;
        Check(ParseTestReport("<testsuites><testsuite><testcase classname=\"a.B\" name=\"x\"/>"
                              "<testcase name=\"y\"><skipped/></testcase>"
                              "<testcase classname=\"a.B\" name=\"z &amp; w\"><error/></testcase></testsuite></testsuites>", sum) &&
              sum.total == 3 && sum.failed == 1 && sum.skipped == 1 &&
              sum.failedNames.size() == 1 && sum.failedNames[0] == "a.B.z & w",
              "JUnit: total, skipped and failed names");
        Check(ParseTestReport("<test-run><test-suite><test-case fullname=\"N.T.Pass\" result=\"Passed\"/>"
                              "<test-case fullname=\"N.T.Bad\" result=\"Failed\"><failure/></test-case>"
                              "<test-case fullname=\"N.T.Skip\" result=\"Skipped\"/></test-suite></test-run>", sum) &&
              sum.total == 3 && sum.failed == 1 && sum.skipped == 1 && sum.failedNames[0] == "N.T.Bad",
              "NUnit 3: result attribute");
        Check(ParseTestReport("<test-results><test-case name=\"Old.Ok\" executed=\"True\" result=\"Success\"/>"
                              "<test-case name=\"Old.Fail\" executed=\"True\" result=\"Failure\"/></test-results>", sum) &&
              sum.total == 2 && sum.failed == 1 && sum.failedNames[0] == "Old.Fail",
              "NUnit 2: Success / Failure");
        Check(!ParseTestReport("<testsuite tests=\"0\"></testsuite>", sum), "a report without test cases is rejected");
        Check(!ParseTestReport("not xml at all", sum), "garbage is rejected");
    }

    {
        // Hancho auto-notes test (doc/design-v3.md 3.4, task 053)
        std::wstring autoRecDir = dir + L"\\memory\\records";
        EnsureDirectoryWide(autoRecDir);

        // Rule 1: Same job name failed 3 times consecutively with same failure classification.
        AppendToLogFileWide(autoRecDir + L"\\2026-11.jsonl",
            "{\"schema\":\"hancho.memory.run/1\",\"id\":\"rr_auto_1\",\"jobId\":\"j_a1\","
            "\"jobName\":\"build\",\"taskqueue\":\"main\",\"attempt\":1,\"outcome\":\"failed\","
            "\"failure\":{\"class\":\"success_check_failed\",\"summary\":\"filesNewer missing\"},"
            "\"start\":\"2026-11-10T11:00:00Z\",\"durationSec\":10}\r\n"
            "{\"schema\":\"hancho.memory.run/1\",\"id\":\"rr_auto_2\",\"jobId\":\"j_a2\","
            "\"jobName\":\"build\",\"taskqueue\":\"main\",\"attempt\":1,\"outcome\":\"failed\","
            "\"failure\":{\"class\":\"success_check_failed\",\"summary\":\"filesNewer missing\"},"
            "\"start\":\"2026-11-10T11:05:00Z\",\"durationSec\":10}\r\n"
            "{\"schema\":\"hancho.memory.run/1\",\"id\":\"rr_auto_3\",\"jobId\":\"j_a3\","
            "\"jobName\":\"build\",\"taskqueue\":\"main\",\"attempt\":1,\"outcome\":\"failed\","
            "\"failure\":{\"class\":\"success_check_failed\",\"summary\":\"filesNewer missing\"},"
            "\"start\":\"2026-11-10T11:10:00Z\",\"durationSec\":10}\r\n");

        int n1 = CheckAndRecordAutoNotes("build", 0);
        Check(n1 == 1, "auto-notes: 3 consecutive failures trigger Rule 1");

        // Verify retrieval via search
        memory::Query q;
        q.kinds = {"note"};
        q.jobName = "build";
        std::string sres = memory::SearchJson(q);
        auto sj = nlohmann::json::parse(sres, nullptr, false);
        Check(sj.value("ok", false) && sj.value("totalMatched", 0) >= 1, "memory search finds auto note");

        // Verify note contents
        std::string noteId;
        if (sj.contains("results") && !sj["results"].empty()) {
            noteId = sj["results"][0].value("id", "");
            std::string text = sj["results"][0].value("summary", sj["results"][0].value("text", ""));
            Check(text.find("build は直近 3 回 success_check_failed（filesNewer）で失敗") != std::string::npos,
                  "Rule 1 text matches expected wording");
        }
        if (!noteId.empty()) {
            std::string noteJson = memory::GetJson(noteId, "note");
            auto nj = nlohmann::json::parse(noteJson, nullptr, false);
            std::string authorType;
            if (nj.contains("document") && nj["document"].is_object()) {
                if (nj["document"].contains("author") && nj["document"]["author"].is_object()) {
                    authorType = nj["document"]["author"].value("type", "");
                }
            } else if (nj.contains("author") && nj["author"].is_object()) {
                authorType = nj["author"].value("type", "");
            }
            Check(authorType == "hancho", "auto-note author is hancho");
            auto docObj = nj.contains("document") ? nj["document"] : nj;
            auto sources = docObj.value("sources", nlohmann::json::array());
            Check(sources.size() == 3 && sources[0] == "rr_auto_1" && sources[2] == "rr_auto_3",
                  "auto-note sources has 3 RunRecord ids");
        }

        // Rule 1 continuing: 4th consecutive failure should NOT create duplicate note
        AppendToLogFileWide(autoRecDir + L"\\2026-11.jsonl",
            "{\"schema\":\"hancho.memory.run/1\",\"id\":\"rr_auto_4\",\"jobId\":\"j_a4\","
            "\"jobName\":\"build\",\"taskqueue\":\"main\",\"attempt\":1,\"outcome\":\"failed\","
            "\"failure\":{\"class\":\"success_check_failed\",\"summary\":\"filesNewer missing\"},"
            "\"start\":\"2026-11-10T11:15:00Z\",\"durationSec\":10}\r\n");
        int n1_dup = CheckAndRecordAutoNotes("build", 0);
        Check(n1_dup == 0, "auto-notes: 4th consecutive failure does not duplicate note");

        // Rule 2: Duration exceeds 80% timeout across consecutive attempts
        AppendToLogFileWide(autoRecDir + L"\\2026-11.jsonl",
            "{\"schema\":\"hancho.memory.run/1\",\"id\":\"rr_slow_1\",\"jobId\":\"j_s1\","
            "\"jobName\":\"test\",\"taskqueue\":\"main\",\"attempt\":1,\"outcome\":\"succeeded\","
            "\"exitCode\":0,\"durationSec\":1600,\"timeoutSec\":1800,"
            "\"start\":\"2026-11-10T12:00:00Z\"}\r\n"
            "{\"schema\":\"hancho.memory.run/1\",\"id\":\"rr_slow_2\",\"jobId\":\"j_s2\","
            "\"jobName\":\"test\",\"taskqueue\":\"main\",\"attempt\":1,\"outcome\":\"succeeded\","
            "\"exitCode\":0,\"durationSec\":1700,\"timeoutSec\":1800,"
            "\"start\":\"2026-11-10T12:30:00Z\"}\r\n");

        int n2 = CheckAndRecordAutoNotes("test", 1800);
        Check(n2 == 1, "auto-notes: consecutive slow attempts trigger Rule 2");

        q.jobName = "test";
        sres = memory::SearchJson(q);
        sj = nlohmann::json::parse(sres, nullptr, false);
        if (sj.contains("results") && !sj["results"].empty()) {
            std::string text = sj["results"][0].value("summary", sj["results"][0].value("text", ""));
            Check(text.find("test は所要時間がタイムアウトに近い") != std::string::npos &&
                  text.find("上限 1800 秒") != std::string::npos,
                  "Rule 2 text matches p90 and timeout limit wording");
        }

        // Rule 2 continuing: 3rd consecutive slow attempt does not duplicate note
        AppendToLogFileWide(autoRecDir + L"\\2026-11.jsonl",
            "{\"schema\":\"hancho.memory.run/1\",\"id\":\"rr_slow_3\",\"jobId\":\"j_s3\","
            "\"jobName\":\"test\",\"taskqueue\":\"main\",\"attempt\":1,\"outcome\":\"succeeded\","
            "\"exitCode\":0,\"durationSec\":1650,\"timeoutSec\":1800,"
            "\"start\":\"2026-11-10T13:00:00Z\"}\r\n");
        int n2_dup = CheckAndRecordAutoNotes("test", 1800);
        Check(n2_dup == 0, "auto-notes: continuing slow attempts do not duplicate note");

        // Rule 3: Success after failure (diff of filesEdited)
        AppendToLogFileWide(autoRecDir + L"\\2026-11.jsonl",
            "{\"schema\":\"hancho.memory.run/1\",\"id\":\"rr_fix_1\",\"jobId\":\"j_f1\","
            "\"jobName\":\"lint\",\"taskqueue\":\"main\",\"attempt\":1,\"outcome\":\"failed\","
            "\"exitCode\":1,\"filesEdited\":[\"src/foo.cpp\"],"
            "\"failure\":{\"class\":\"exit_nonzero\",\"summary\":\"lint error\"},"
            "\"start\":\"2026-11-10T14:00:00Z\",\"durationSec\":5}\r\n"
            "{\"schema\":\"hancho.memory.run/1\",\"id\":\"rr_fix_2\",\"jobId\":\"j_f2\","
            "\"jobName\":\"lint\",\"taskqueue\":\"main\",\"attempt\":1,\"outcome\":\"succeeded\","
            "\"exitCode\":0,\"filesEdited\":[\"src/foo.cpp\",\"src/bar.cpp\"],"
            "\"start\":\"2026-11-10T14:05:00Z\",\"durationSec\":5}\r\n");

        int n3 = CheckAndRecordAutoNotes("lint", 0);
        Check(n3 == 1, "auto-notes: success after failure triggers Rule 3");

        q.jobName = "lint";
        sres = memory::SearchJson(q);
        sj = nlohmann::json::parse(sres, nullptr, false);
        if (sj.contains("results") && !sj["results"].empty()) {
            std::string text = sj["results"][0].value("summary", sj["results"][0].value("text", ""));
            Check(text.find("lint は失敗の後に成功。直した可能性のあるファイル: src/bar.cpp") != std::string::npos,
                  "Rule 3 text lists filesEdited diff");
        }

        // Verify memory_brief includes the auto-notes
        memory::BriefOptions bo;
        bo.jobName = "build";
        memory::BriefResult brief = memory::BuildBrief(bo);
        Check(brief.ok, "memory brief succeeds with auto-notes");
        Check(brief.text.find("Related notes") != std::string::npos &&
              brief.text.find("build は直近 3 回") != std::string::npos,
              "memory brief includes auto-notes in Related notes");
    }

    {
        // 072-1 Plain run state (runs outside a queue)
        std::string jid1 = TaskQueueManager::IssuePlainJobId(1234, "build");
        std::string jid2 = TaskQueueManager::IssuePlainJobId(1234, "build");
        Check(jid1.find("j_plain_") == 0 && jid1.find("build_1234_") != std::string::npos, "IssuePlainJobId formats correctly with name and pid");
        Check(jid1 != jid2, "IssuePlainJobId generates unique sequence numbers");

        // RecordPlainRun test
        TaskQueueManager plainTqm;
        PlainRunState run;
        run.jobId = "j_plain_manual_1";
        run.name = "deploy";
        run.command = "deploy.cmd --prod";
        run.cwd = "D:/wsgit/app";
        run.start = "2026-11-10 15:00:00";
        run.end = "2026-11-10 15:01:00";
        run.exitCode = 0;
        run.processId = 555;
        std::string recErr;
        bool ok = plainTqm.RecordPlainRun(run, recErr);
        Check(ok, "RecordPlainRun succeeds");
        std::wstring plainFilePath = PathJoinWide(GetLogsDirectoryWide(), L"taskqueues\\plain.json");
        Check(std::filesystem::exists(plainFilePath), "plain.json exists on disk");

        // Verify plain.json contents
        std::string plainJson = ReadFileUtf8Wide(plainFilePath);
        auto parsedPlain = nlohmann::json::parse(plainJson, nullptr, false);
        Check(!parsedPlain.is_discarded() && parsedPlain.value("name", "") == "plain", "plain.json has name 'plain'");
        bool foundJob = false;
        if (parsedPlain.contains("jobs") && parsedPlain["jobs"].is_array()) {
            for (const auto& j : parsedPlain["jobs"]) {
                if (j.value("id", "") == "j_plain_manual_1") {
                    foundJob = true;
                    Check(j.value("command", "") == "deploy.cmd --prod", "plain job command preserved");
                    Check(j.value("state", "") == "succeeded", "plain job exit 0 has state succeeded");
                    auto atts = j.value("attempts", nlohmann::json::array());
                    Check(atts.size() == 1 && atts[0].value("exit_code", -1) == 0 && atts[0].value("process_id", 0) == 555,
                          "plain job attempt facts preserved");
                    break;
                }
            }
        }
        Check(foundJob, "j_plain_manual_1 found in plain.json");

        // OnProcessFinished with an unqueued PID automatically records plain run
        plainTqm.OnProcessFinished(8888, 1, "failed");
        plainJson = ReadFileUtf8Wide(plainFilePath);
        parsedPlain = nlohmann::json::parse(plainJson, nullptr, false);
        bool foundUnqueued = false;
        if (parsedPlain.contains("jobs") && parsedPlain["jobs"].is_array()) {
            for (const auto& j : parsedPlain["jobs"]) {
                auto atts = j.value("attempts", nlohmann::json::array());
                if (!atts.empty() && atts[0].value("process_id", 0) == 8888) {
                    foundUnqueued = true;
                    Check(j.value("state", "") == "failed", "unqueued exit 1 has state failed");
                    Check(atts[0].value("exit_code", 0) == 1, "attempt exit_code 1");
                    break;
                }
            }
        }
        Check(foundUnqueued, "unqueued process finished is automatically recorded into plain.json");

        // Existing queue isolation: retry --failed ignores plain queue
        std::string retryErr;
        std::string retryResp = plainTqm.TaskQueueRetryFailed("", 0, false, false, retryErr);
        Check(retryResp.find("j_plain_") == std::string::npos, "TaskQueueRetryFailed ignores plain jobs");
    }

    // --- Suspend after a job and all other work become idle --------------------------------
    tqm.ShutdownTaskQueueManager();
    {
        std::wstring powerDir = dir + L"_power";
        CreateDirectoryW(powerDir.c_str(), nullptr);
        SetDataDirectoryOverride(powerDir);

        ProcessManager powerPm;
        TaskQueueManager powerTqm;
        powerTqm.SetProcessManager(&powerPm);
        powerPm.SetProcessFinishedCallback([&powerTqm](int pid, int ec, const std::string& kind) {
            powerTqm.OnProcessFinished(pid, ec, kind);
        });
        std::atomic<int> fired{0};
        powerTqm.SetSuspendCallback([&fired](const std::string&, const std::string&, bool) {
            fired.fetch_add(1);
        });

        std::string err;
        const std::string jobs = R"([
          {"id":"PW1","command":"cmd /c exit 0","summary":"final job","timeoutSec":30,"suspendWhenDone":true,"suspendDelaySec":1},
          {"id":"PW_HOLD","command":"cmd /c ping -n 4 127.0.0.1","summary":"other running","timeoutSec":30},
          {"id":"PW_DEP","command":"cmd /c exit 0","summary":"other pending","timeoutSec":30,"dependsOn":["PW_HOLD"]}
        ])";
        Check(JsonGetBool(powerTqm.TaskQueueAdd("powerq", jobs, 3, "", err), "ok", false),
              "power: jobs registered: " + err);
        Check(WaitForState(powerTqm, "PW1", "succeeded", 10000), "power: armed job finished");
        Sleep(1200);
        Check(fired.load() == 0, "power: waits for other jobs");
        Check(WaitForState(powerTqm, "PW_HOLD", "succeeded", 15000), "power: other job finished");
        Check(WaitForState(powerTqm, "PW_DEP", "succeeded", 15000), "power: dependent job finished");
        uint64_t powerStart = GetNowTickMs();
        while (fired.load() == 0 && GetNowTickMs() - powerStart < 8000) Sleep(100);
        Check(fired.load() == 1, "power: fires after all work is idle");
        std::string getResp = powerTqm.TaskQueueGet("PW1", false, 0, err);
        Check(!JsonGetBool(getResp, "suspend_when_done", true), "power: flag cleared after firing");

        const std::string cancelJob =
            R"([{"id":"PW2","command":"cmd /c ping -n 4 127.0.0.1","summary":"cancel me","timeoutSec":30,"suspendWhenDone":true,"suspendDelaySec":1}])";
        Check(JsonGetBool(powerTqm.TaskQueueAdd("powerq2", cancelJob, 0, "", err), "ok", false),
              "power: cancel job added");
        Check(WaitForState(powerTqm, "PW2", "running", 10000), "power: cancel job running");
        std::string cancelResp = powerTqm.ClearSuspendWhenDone(err);
        Check(JsonGetInt(cancelResp, "cleared", 0) == 1, "power: cancel clears the armed flag");
        Check(!JsonGetBool(powerTqm.SuspendStatusJson(), "armed", true),
              "power: status reports not armed");
        int firedBefore = fired.load();
        Check(WaitForState(powerTqm, "PW2", "succeeded", 15000), "power: cancel job finished");
        Sleep(1500);
        Check(fired.load() == firedBefore, "power: cancel prevents suspend");

        powerTqm.SetSuspendCallback(nullptr);
        powerTqm.ShutdownTaskQueueManager();
        SetDataDirectoryOverride(dir);
    }

    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
