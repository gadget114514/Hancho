#include "../src/taskqueue.h"
#include "../src/task_lineage_view.h"
#include "../src/chain_guard.h"
#include "../src/common.h"
#include "../src/llm_stream.h"
#include "../src/llm_config.h"
#include "../src/memory_search.h"
#include <windows.h>
#include <ctime>
#include <cstdio>
#include <cwctype>
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

    // --- Dashboard window title ----------------------------------------------------------
    {
        const std::wstring defaultDir = GetDefaultLogsDirectoryWide();
        const std::wstring customDir = dir;

        Check(DashboardWindowTitle(defaultDir) == L"Hancho - " + defaultDir,
              "title includes the default data dir");
        Check(DashboardWindowTitle(customDir) == L"Hancho - " + customDir,
              "title includes the --data-dir override");

        const std::wstring title = DashboardWindowTitle(customDir);
        Check(DashboardTitleMatches(title, customDir), "title matches its data dir");
        Check(DashboardTitleMatches(title, customDir + L"\\"), "trailing separator is ignored");

        std::wstring slashed = customDir;
        for (wchar_t& ch : slashed) {
            if (ch == L'\\') ch = L'/';
        }
        Check(DashboardTitleMatches(title, slashed), "/ and \\ are equivalent");
        Check(DashboardTitleMatches(DashboardWindowTitle(slashed) + L"/", customDir),
              "title separators and trailing separator are ignored");

        std::wstring lowered = slashed, raised = slashed;
        for (wchar_t& ch : lowered) ch = static_cast<wchar_t>(std::towlower(ch));
        for (wchar_t& ch : raised) ch = static_cast<wchar_t>(std::towupper(ch));
        Check(DashboardTitleMatches(DashboardWindowTitle(lowered), raised),
              "matching ignores case");

        Check(!DashboardTitleMatches(title, defaultDir), "different data dir does not match");
        Check(DashboardTitleMatches(L"Hancho", defaultDir), "legacy title matches default data dir");
        Check(!DashboardTitleMatches(L"Hancho", customDir),
              "legacy title does not match an explicit data dir");
    }

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

    // --- Failure classifier (doc/obsoleted/design-v3.md 4.2-4.4) -------------------------------------
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
            in.events.push_back(event("tool.call", 1, R"({"activity":"build","target":"app"})"));
            in.lastActivity = "build";
            in.lastActivitySinceSec = 5;
            auto d = classified(in, "timeout", "raise_timeout");
            Check(d.evidence[0].IsLog() && d.evidence[0].line == 102, "timeout evidence is the last log line");
        }
        {
            // timeout stalled mid-run (sinceSec 50 > threshold 30) -> timeout with retry and nextSteps
            TaskQueueFailureInput in = makeBase();
            in.endKind = "timed_out";
            in.timeoutSec = 90;
            in.elapsedSec = 91;
            in.events.push_back(event("tool.call", 1, R"({"activity":"build","target":"app","tool":"compiler"})"));
            in.lastActivity = "build";
            in.lastActivitySinceSec = 50;
            auto d = classified(in, "timeout", "retry");
            Check(d.detail.value("stalled", false), "timeout stalled mid-run sets stalled detail");
            Check(d.detail.value("lastTool", "") == "compiler", "timeout stalled detail has lastTool");
            Check(!d.nextSteps.empty(), "timeout stalled has nextSteps");
            Check(d.evidence[0].IsEvent() && d.evidence[0].seq == 1, "timeout stalled evidence is the last event");
        }
        {
            TaskQueueFailureInput in = makeBase();
            in.endKind = "idle_timed_out";
            in.idleOutputSec = 60;
            in.idleCpuSec = 30;
            in.events.push_back(event("tool.call", 1, R"({"activity":"build","target":"app"})"));
            classified(in, "idle_stall", "retry");
        }
        // 090-1 hang diagnosis tests
        {
            // 0 events timeout -> startup_hang and NOT raise_timeout
            TaskQueueFailureInput in = makeBase();
            in.endKind = "timed_out";
            in.timeoutSec = 3600;
            in.elapsedSec = 3600;
            auto d = classified(in, "startup_hang", "retry_with_change");
            Check(d.suggestedAction != "raise_timeout", "0-event timeout does not suggest raise_timeout");
            Check(d.summary.find("3600 s") != std::string::npos, "startup_hang summary includes elapsed time");
            Check(d.confidence == "high", "startup_hang has high confidence");
            Check(!d.supervisorNote.empty(), "startup_hang carries supervisorNote");
            Check(d.supervisorNote.find("takeover") != std::string::npos, "supervisorNote mentions takeover");
        }
        {
            // opencode without --model -> no_model in likelyCauses
            TaskQueueFailureInput in = makeBase();
            in.endKind = "timed_out";
            in.timeoutSec = 60;
            in.command = "opencode run \"Execute card.md\"";
            auto d = classified(in, "startup_hang", "retry_with_change");
            bool foundNoModel = false;
            for (const auto& c : d.likelyCauses) {
                if (c.id == "no_model") foundNoModel = true;
            }
            Check(foundNoModel, "opencode without --model reports no_model likelyCause");
        }
        {
            // nextSteps head changes with attempt count:
            // 1st attempt: retry
            TaskQueueFailureInput in1 = makeBase();
            in1.endKind = "timed_out";
            in1.attempt = 1;
            auto d1 = classified(in1, "startup_hang", "retry_with_change");
            Check(!d1.nextSteps.empty() && d1.nextSteps[0].action == "retry" && d1.nextSteps[0].change.is_null(),
                  "attempt 1 nextSteps head is retry unchanged");

            // 2nd attempt: retry with change and needsApproval
            TaskQueueFailureInput in2 = makeBase();
            in2.endKind = "timed_out";
            in2.attempt = 2;
            in2.command = "opencode run \"card\"";
            auto d2 = classified(in2, "startup_hang", "retry_with_change");
            Check(!d2.nextSteps.empty() && d2.nextSteps[0].action == "retry" &&
                  d2.nextSteps[0].change.is_object() && d2.nextSteps[0].needsApproval,
                  "attempt 2 nextSteps head is retry with change and needsApproval");

            // 3rd attempt: escalate
            TaskQueueFailureInput in3 = makeBase();
            in3.endKind = "timed_out";
            in3.attempt = 3;
            auto d3 = classified(in3, "startup_hang", "retry_with_change");
            Check(!d3.nextSteps.empty() && d3.nextSteps[0].action == "escalate" && !d3.nextSteps[0].question.empty(),
                  "attempt 3 nextSteps head is escalate with question");
        }
        {
            // 090-2: startup_timed_out classified as startup_hang with detail.idleFirstEventSec and processTree
            TaskQueueFailureInput in = makeBase();
            in.endKind = "startup_timed_out";
            in.idleFirstEventSec = 180;
            in.processTree = nlohmann::json::array({
                {{"name", "claude.exe"}, {"pid", 1234}, {"cpuSec", 0.5}}
            });
            auto d = classified(in, "startup_hang", "retry_with_change");
            Check(d.summary.find("180 s") != std::string::npos, "startup_timed_out summary mentions 180 s");
            Check(d.detail.value("endKind", "") == "startup_timed_out", "diagnostic detail carries endKind");
            Check(d.detail.value("idleFirstEventSec", 0) == 180, "diagnostic detail carries idleFirstEventSec");
            Check(d.detail.contains("processTree") && d.detail["processTree"].is_array() && d.detail["processTree"].size() == 1,
                  "diagnostic detail carries processTree snapshot");
            Check(d.detail["processTree"][0]["name"] == "claude.exe", "processTree snapshot item preserved");
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
        // External rules from failure-rules.json (doc/obsoleted/design-v3.md 4.4).
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
        // The LLM guess is advisory only (doc/obsoleted/design-v3.md 4.4).
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
        // 091-7 (1): a denied tool call outranks a rate-limit log line (the 090-2/090-3
        // misdiagnosis), and its evidence names the tool, target and engine.
        auto base = []() {
            TaskQueueFailureInput in;
            in.jobId = "0917";
            in.jobName = "worker";
            in.taskqueue = "q";
            in.attempt = 1;
            in.maxAttempts = 2;
            in.expectedExitCode = 0;
            in.eventsFile = "C:/logs/proc_0917.events.jsonl";
            in.logFile = "C:/logs/0917.1.log";
            in.logText = "usage limit reached; too many requests (429)\n";
            in.logFirstLine = 10;
            in.logLastLine = 10;
            in.fallbackEventsFile = "C:/logs/taskqueue-q.events.jsonl";
            in.fallbackEventSeq = 9;
            in.nowEpochSec = 1794288000;
            return in;
        };
        auto ev = [](const std::string& kind, uint64_t seq, const std::string& data,
                     const std::string& agent = "") {
            std::string s = "{\"schema\":\"hancho.event/1\",\"seq\":" + std::to_string(seq) +
                            ",\"ts\":\"2026-11-19T09:00:00Z\",\"kind\":\"" + kind +
                            "\",\"data\":" + data;
            if (!agent.empty()) s += ",\"agent\":{\"sessionId\":\"" + agent + "\"}";
            return nlohmann::json::parse(s + "}", nullptr, false);
        };
        TaskQueueFailureInput in = base();
        in.engine = "opencode";
        in.events.push_back(ev("tool.call", 1,
            R"({"tool":"read","toolUseId":"u1","activity":"read","target":"C:/Users/x/.config/opencode/AGENTS.md"})",
            "sess-1"));
        in.events.push_back(ev("tool.result", 2,
            R"({"toolUseId":"u1","ok":false,"error":"Error: The user has specified a rule which prevents you from using this specific tool call."})"));
        auto d = ClassifyTaskQueueFailure(in);
        Check(d.failureClass == "permission_denied",
              "opencode denial beats the rate-limit log line (got " + d.failureClass + ")");
        Check(d.detail.value("source", std::string()) == "event", "denial source is the tool result");
        Check(d.detail["permission"].value("tool", std::string()) == "read", "evidence tool recorded");
        Check(d.detail["permission"].value("target", std::string()).find("AGENTS.md") != std::string::npos,
              "evidence target recorded");
        Check(d.detail["permission"].value("engine", std::string()) == "opencode", "evidence engine recorded");
        Check(!d.evidence.empty() && d.evidence[0].tool == "read" &&
                  d.evidence[0].target.find("AGENTS.md") != std::string::npos &&
                  d.evidence[0].engine == "opencode",
              "evidence carries {tool, target, engine}");
        bool hasFix = false, hasPermit = false, hasEscalate = false;
        for (const auto& s : d.nextSteps) {
            if (s.action == "fix_spec") hasFix = true;
            if (s.action == "permit_request") hasPermit = true;
            if (s.action == "escalate") hasEscalate = true;
        }
        Check(hasFix && hasPermit && hasEscalate, "three nextSteps: fix / permit request / escalate");
        Check(TaskQueueFailureDiagnosticToJson(d).find("permit_request") != std::string::npos,
              "nextSteps are serialized for BT / triage");

        // The 2026-11-19 literal wording from the design is recognized too.
        TaskQueueFailureInput other = base();
        other.events.push_back(ev("tool.result", 3,
            R"({"toolUseId":"u2","ok":false,"error":"The user rejected permission to use this specific tool call."})"));
        Check(ClassifyTaskQueueFailure(other).failureClass == "permission_denied",
              "design's rejection wording is recognized");

        // 101-2: HANCHO-RESULT: root_cause_not_found at line start only.
        TaskQueueFailureInput rcnf = base();
        rcnf.endKind = "failed";
        rcnf.exitCode = 1;
        rcnf.logText = "investigating...\nHANCHO-RESULT: root_cause_not_found\n";
        auto rcnfDiag = ClassifyTaskQueueFailure(rcnf);
        Check(rcnfDiag.failureClass == "root_cause_not_found",
              "101-2: marker line classifies as root_cause_not_found");
        Check(rcnfDiag.confidence == "high" && rcnfDiag.suggestedAction == "retry",
              "101-2: root_cause_not_found has high confidence and retry action");

        TaskQueueFailureInput rcnfJson = base();
        rcnfJson.endKind = "failed";
        rcnfJson.exitCode = 1;
        rcnfJson.logText = R"({"type":"assistant","text":"HANCHO-RESULT: root_cause_not_found"})" "\n";
        Check(ClassifyTaskQueueFailure(rcnfJson).failureClass == "exit_nonzero",
              "101-2: marker inside JSON string does not trigger root_cause_not_found");

        // 091-7 (5)/(6): the pure hold/resume decision.
        Check(PlanPermissionHandling(d, "sess-1", 0, 2, "q", "").resume,
              "first denial resumes the same session");
        PermissionHoldPlan held = PlanPermissionHandling(d, "sess-1", 2, 2, "q", "");
        Check(held.wait && held.kind == "read_path" && held.scope == "taskqueue:q" &&
                  !held.permitId.empty(),
              "after the resume limit the denial waits for a permit");
        Check(!PlanPermissionHandling(d, "sess-1", 0, 2, "q", "fail").resume &&
                  !PlanPermissionHandling(d, "sess-1", 0, 2, "q", "fail").wait,
              "onPermissionDenied=fail neither resumes nor holds");
        Check(TaskQueueStateFromString("waitingForPermit") == TaskQueueState::WaitingForPermit &&
                  TaskQueueStateToString(TaskQueueState::WaitingForPermit) == "waitingForPermit",
              "waitingForPermit round-trips through the state enum");
        TaskQueueRunJob runJob;
        runJob.id = "0917"; runJob.state = "waitingForPermit";
        Check(runJob.Unfinished(), "waitingForPermit is not a terminal run state");
    }

    {
        // Retry policy by failure class (doc/obsoleted/design-v3.md 4.5).
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
        Check(decide(job, "startup_hang", 1).action == TaskQueueRetryAction::Retry,
              "startup_hang retries like idle_stall when retry.on lists idle_stall");
        {
            TaskQueueJob neverHang = job;
            neverHang.retry.on.clear();
            neverHang.retry.never = {"idle_stall"};
            Check(decide(neverHang, "startup_hang", 1).action == TaskQueueRetryAction::GiveUp,
                  "retry.never on idle_stall also blocks startup_hang");
        }
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

    {
        // hidden is a caller's choice: the queue must preserve it (the dashboard has a
        // "Show hidden tasks" option). Internal jobs use the same field.
        std::string hiddenErr;
        std::string hiddenResp = tqm.TaskQueueAdd("hiddenq",
            R"([{"id":"HID","command":"cmd /c exit 0","summary":"hidden job","timeout_sec":30,"hidden":true}])",
            0, "", hiddenErr);
        std::string hiddenJob = tqm.TaskQueueGet("HID", false, 0, hiddenErr);
        Check(JsonGetBool(hiddenResp, "ok", false) &&
              hiddenJob.find("\"hidden\":true") != std::string::npos,
              "taskqueue_add keeps hidden=true");
    }

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
        std::string hangRetry = R"([{"id":"RTH","command":"cmd /c exit 0","summary":"hang retry","timeoutSec":10,
          "retry":{"on":["startup_hang"]}}])";
        Check(JsonGetBool(tqm.TaskQueueAdd("retryq", hangRetry, 0, "", err), "ok", false),
              "startup_hang is a valid retry.on class: " + err);
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

    // success.contentFile/contentRegex/minBytes: a report that only got its headings is not
    // success, and success.engineResult catches an engine that reported an error while
    // still exiting 0.
    WriteFileAtomicWide(workDir + L"/ct-headings.src", "# Report\r\n\r\n## Result\r\n\r\n## Notes\r\n");
    WriteFileAtomicWide(workDir + L"/ct-complete.src",
                        "# Report\r\n\r\n## Result\r\nAll done.\r\n\r\n<!-- hancho:report-complete -->\r\n");
    WriteFileAtomicWide(workDir + L"/ct-after.src", "<!-- hancho:report-complete -->\r\ntrailing text\r\n");
    WriteFileAtomicWide(workDir + L"/ct-small.src", "ok\r\n");
    WriteFileAtomicWide(workDir + L"/ct-engine-error.src",
                        "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":true,"
                        "\"result\":\"Failed to authenticate\"}\r\n");
    WriteFileAtomicWide(workDir + L"/ct-engine-ok.src",
                        "{\"type\":\"result\",\"subtype\":\"success\",\"is_error\":false,\"result\":\"ok\"}\r\n");
    {
        // std::regex treats "$" as end-of-line, so the end of file is written as a lookahead.
        const std::string markerRegex = "<!--\\s*hancho:report-complete\\s*-->\\s*(?![\\s\\S])";
        auto makeFileJob = [&](const std::string& id, const std::string& src, const std::string& out,
                               nlohmann::json success) {
            return nlohmann::json{
                {"id", id},
                {"command", "cmd /c type " + src + ">" + out},
                {"summary", id},
                {"timeoutSec", 30},
                {"maxAttempts", 1},
                {"cwd", workUtf8},
                {"success", std::move(success)}};
        };
        auto makeStdoutJob = [&](const std::string& id, const std::string& src,
                                 nlohmann::json success) {
            return nlohmann::json{
                {"id", id},
                {"command", "cmd /c type " + src},
                {"summary", id},
                {"timeoutSec", 30},
                {"maxAttempts", 1},
                {"cwd", workUtf8},
                {"success", std::move(success)}};
        };
        nlohmann::json ctJobs = nlohmann::json::array();
        ctJobs.push_back(makeFileJob("CT1", "ct-headings.src", "ct1.md",
                                     {{"exitCode", 0}, {"contentFile", "ct1.md"}, {"contentRegex", markerRegex}}));
        ctJobs.push_back(makeFileJob("CT2", "ct-complete.src", "ct2.md",
                                     {{"exitCode", 0}, {"contentFile", "ct2.md"}, {"contentRegex", markerRegex}}));
        ctJobs.push_back(makeFileJob("CT3", "ct-after.src", "ct3.md",
                                     {{"exitCode", 0}, {"contentFile", "ct3.md"}, {"contentRegex", markerRegex}}));
        ctJobs.push_back(makeStdoutJob("CT4", "ct-engine-error.src", {{"exitCode", 0}, {"engineResult", true}}));
        ctJobs.push_back(makeStdoutJob("CT5", "ct-engine-error.src", {{"exitCode", 0}, {"engineResult", false}}));
        ctJobs.push_back(makeStdoutJob("CT6", "ct-engine-ok.src", {{"exitCode", 0}, {"engineResult", true}}));
        ctJobs.push_back(makeFileJob("CT7", "ct-small.src", "ct7.md",
                                     {{"exitCode", 0}, {"contentFile", "ct7.md"}, {"minBytes", 1000}}));
        std::string addCt = tqm.TaskQueueAdd("ctq", ctJobs.dump(), 0, "", err);
        Check(JsonGetBool(addCt, "ok", false), "content-check jobs added: " + err);
        Check(WaitForState(tqm, "CT1", "failed", 20000), "CT1 failed: headings only");
        Check(WaitForState(tqm, "CT2", "succeeded", 20000), "CT2 succeeded: report ends with the marker");
        Check(WaitForState(tqm, "CT3", "failed", 20000), "CT3 failed: marker is not the last line");
        Check(WaitForState(tqm, "CT4", "failed", 20000), "CT4 failed: engine reported an error");
        Check(WaitForState(tqm, "CT5", "succeeded", 20000), "CT5 succeeded without engineResult");
        Check(WaitForState(tqm, "CT6", "succeeded", 20000), "CT6 succeeded with a good engine result");
        Check(WaitForState(tqm, "CT7", "failed", 20000), "CT7 failed: content file below minBytes");

        auto failureOf = [&](const std::string& id) {
            auto j = ParseJobJson(tqm.TaskQueueGet(id, false, 0, err));
            if (j.contains("attempts") && j["attempts"].is_array() && !j["attempts"].empty()) {
                const auto& a = j["attempts"][0];
                if (a.contains("failure") && a["failure"].is_object()) {
                    return a["failure"];
                }
            }
            return nlohmann::json::object();
        };
        auto conditionOf = [&](const std::string& id) {
            auto f = failureOf(id);
            if (f.contains("detail") && f["detail"].is_object()) {
                return f["detail"].value("condition", std::string());
            }
            return std::string();
        };
        Check(conditionOf("CT1") == "content", "CT1 diagnosis names content: " + conditionOf("CT1"));
        Check(conditionOf("CT3") == "content", "CT3 diagnosis names content: " + conditionOf("CT3"));
        Check(conditionOf("CT7") == "content", "CT7 diagnosis names content: " + conditionOf("CT7"));
        Check(conditionOf("CT4") == "engineResult",
              "CT4 diagnosis names engineResult: " + conditionOf("CT4"));
        auto ct4Detail = failureOf("CT4");
        Check(ct4Detail.contains("detail") && ct4Detail["detail"].is_object() &&
                  ct4Detail["detail"].value("detail", std::string()).find("Failed to authenticate") !=
                      std::string::npos,
              "CT4 detail carries the engine error");
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
            auto j = nlohmann::json::parse(resp, nullptr, false);
            if (!j.is_object() || !j.contains("jobs") || !j["jobs"].is_array()) return -1;
            return static_cast<int>(j["jobs"].size());
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
        // Per-job pause: a queued job is not scheduled; a running job is suspended
        // and its elapsed clock freezes until it is resumed.
        std::string pauseJobs = "["
            "{\"id\":\"PJ1\",\"command\":\"ping -n 5 127.0.0.1\",\"summary\":\"PJ1 blocker\",\"timeoutSec\":30},"
            "{\"id\":\"PJ2\",\"command\":\"ping -n 20 127.0.0.1\",\"summary\":\"PJ2 paused\",\"timeoutSec\":60}"
            "]";
        Check(JsonGetBool(tqm.TaskQueueAdd("pauseq", pauseJobs, 1, "", err), "ok", false),
              "pauseq jobs added: " + err);
        Check(WaitForState(tqm, "PJ1", "running", 5000), "PJ1 starts");
        Check(GetState(tqm, "PJ2") != "running", "PJ2 waits behind PJ1");

        std::string pauseResp = tqm.TaskQueuePauseJob("PJ2", true, err);
        Check(JsonGetBool(pauseResp, "ok", false) && JsonGetBool(pauseResp, "paused", false),
              "TaskQueuePauseJob pauses the queued job: " + err);

        Check(WaitForState(tqm, "PJ1", "succeeded", 20000), "PJ1 succeeded");
        Sleep(600);
        Check(GetState(tqm, "PJ2") == "pending", "paused queued job is not started");

        std::string resumeResp = tqm.TaskQueuePauseJob("PJ2", false, err);
        Check(JsonGetBool(resumeResp, "ok", false) && !JsonGetBool(resumeResp, "paused", true),
              "TaskQueuePauseJob resumes the queued job: " + err);
        Check(WaitForState(tqm, "PJ2", "running", 5000), "PJ2 starts after resume");
        Sleep(300);

        auto jobJson = ParseJobJson(tqm.TaskQueueGet("PJ2", false, 0, err));
        int pid = 0;
        if (jobJson.contains("attempts") && jobJson["attempts"].is_array() &&
            !jobJson["attempts"].empty()) {
            pid = static_cast<int>(jobJson["attempts"].back().value("process_id", 0));
        }
        Check(pid > 0, "PJ2 has a process id");
        Check(JsonGetBool(tqm.TaskQueuePauseJob("PJ2", true, err), "ok", false),
              "TaskQueuePauseJob pauses the running job: " + err);

        ProcessSnapshot snapPaused;
        Check(pid > 0 && pm.GetSnapshot(pid, snapPaused) && snapPaused.suspended,
              "paused running job has a suspended process tree");
        const uint64_t elapsedAtPause = snapPaused.elapsedMs;
        const uint64_t idleAtPause = snapPaused.idleMs;
        Sleep(900);
        ProcessSnapshot snapStill;
        Check(pm.GetSnapshot(pid, snapStill), "paused process snapshot is available");
        Check(snapStill.elapsedMs <= elapsedAtPause + 50,
              "elapsed time is frozen while paused");
        Check(snapStill.idleMs <= idleAtPause + 50,
              "idle time is frozen while paused");

        Check(JsonGetBool(tqm.TaskQueuePauseJob("PJ2", false, err), "ok", false),
              "TaskQueuePauseJob resumes the running job: " + err);
        Sleep(700);
        ProcessSnapshot snapResumed;
        Check(pm.GetSnapshot(pid, snapResumed) && !snapResumed.suspended,
              "resumed job is no longer suspended");
        Check(snapResumed.elapsedMs > elapsedAtPause + 300,
              "elapsed time advances again after resume");

        Check(JsonGetBool(tqm.TaskQueueCancel("PJ2", err), "ok", false), "PJ2 cancelled");
        Check(WaitForState(tqm, "PJ2", "cancelled", 5000), "PJ2 is cancelled");
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

    // --- Model changes require an explicit human approval (2026-11-17 incident) ------------
    {
        std::string addErr;
        const std::string modelJobs =
            R"([{"id":"MG1","command":"cmd /c exit 1","summary":"model gate","timeoutSec":30}])";
        Check(JsonGetBool(tqm.TaskQueueAdd("modelq", modelJobs, 0, "", addErr), "ok", false),
              "model gate job added: " + addErr);
        std::string gateState;
        for (int i = 0; i < 200; ++i) {
            gateState = GetState(tqm, "MG1");
            if (gateState == "failed" || gateState == "succeeded") break;
            Sleep(100);
        }
        Check(gateState == "failed", "model gate job failed (state=" + gateState + ")");
        std::string err1;
        const std::string noApproval = tqm.TaskQueueUpdate(
            "MG1", R"({"command":"cmd /c exit 1 --model opencode-go/kimi-k3.1-pro"})", err1);
        Check(!JsonGetBool(noApproval, "ok", false), "model change without approval is rejected");
        std::string err2;
        const std::string approved = tqm.TaskQueueUpdate(
            "MG1",
            R"({"command":"cmd /c exit 1 --model opencode-go/kimi-k3.1-pro","approved_by":"human","approval_reason":"test"})",
            err2);
        Check(JsonGetBool(approved, "ok", false), "model change with approval is accepted: " + err2);
        Check(tqm.TaskQueueGet("MG1", false, 0, err2).find("[model]") != std::string::npos,
              "model approval is recorded in notes");
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

    // Remote reconnect after restart (056-1, doc/obsoleted/design-v3.md 2.5).
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

    // Remote success.filesNewer evaluation (057, doc/obsoleted/design-v3-todo.md 6).
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
        // CI one-shot run (doc/obsoleted/design-v3.md 2.3): the three exit codes and the JUnit report.
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

        // 100-6: superseded jobs are excluded by default; --count-superseded includes them.
        auto parsedSup = TaskQueueRunJobFromJson(nlohmann::json::parse(
            R"({"id":"A","state":"failed","superseded_by":"A2"})", nullptr, false));
        Check(parsedSup.supersededBy == "A2",
              "100-6: TaskQueueRunJobFromJson reads superseded_by");

        TaskQueueRunJob supFailed;
        supFailed.id = "A";
        supFailed.state = "failed";
        supFailed.supersededBy = "A2";
        TaskQueueRunJob supSucceeded;
        supSucceeded.id = "A2";
        supSucceeded.state = "succeeded";
        const std::vector<TaskQueueRunJob> lineageOk = {supFailed, supSucceeded};
        Check(TaskQueueRunDrained(lineageOk), "100-6: A failed (superseded) + A2 succeeded is drained");
        Check(TaskQueueRunExitCode(lineageOk) == 0,
              "100-6: A failed -> A2 succeeded exits 0 by default");
        Check(TaskQueueRunExitCode(lineageOk, true) == 1,
              "100-6: A failed -> A2 succeeded exits 1 with countSuperseded=true");

        const std::string lineageXmlDefault = TaskQueueRunJUnitXml("q", lineageOk);
        Check(lineageXmlDefault.find("tests=\"1\"") != std::string::npos &&
                  lineageXmlDefault.find("failures=\"0\"") != std::string::npos &&
                  lineageXmlDefault.find("name=\"A2\"") != std::string::npos &&
                  lineageXmlDefault.find("name=\"A\"") == std::string::npos,
              "100-6: JUnit excludes superseded job A by default: " + lineageXmlDefault);

        const std::string lineageXmlAll = TaskQueueRunJUnitXml("q", lineageOk, true);
        Check(lineageXmlAll.find("tests=\"2\"") != std::string::npos &&
                  lineageXmlAll.find("failures=\"1\"") != std::string::npos &&
                  lineageXmlAll.find("name=\"A\"") != std::string::npos &&
                  lineageXmlAll.find("name=\"A2\"") != std::string::npos,
              "100-6: JUnit includes superseded job A with countSuperseded=true: " + lineageXmlAll);

        TaskQueueRunJob supCancelled;
        supCancelled.id = "C";
        supCancelled.state = "cancelled";
        supCancelled.supersededBy = "C2";
        TaskQueueRunJob supCancelledOk;
        supCancelledOk.id = "C2";
        supCancelledOk.state = "succeeded";
        Check(TaskQueueRunExitCode({supCancelled, supCancelledOk}) == 0,
              "100-6: cancelled job superseded by succeeded exits 0 by default");
        Check(TaskQueueRunExitCode({supCancelled, supCancelledOk}, true) == 2,
              "100-6: cancelled job superseded by succeeded exits 2 with countSuperseded=true");
    }

    {
        // Resource overlap rules (doc/obsoleted/resource-locks.md).
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
        // Hancho auto-notes test (doc/obsoleted/design-v3.md 3.4, task 053)
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

        TaskQueueManager powerTqm;
        ProcessManager powerPm;
        powerTqm.SetProcessManager(&powerPm);
        powerPm.SetProcessFinishedCallback([&powerTqm](int pid, int ec, const std::string& kind) {
            powerTqm.OnProcessFinished(pid, ec, kind);
        });
        std::atomic<int> fired{0};
        std::atomic<int> shutDowns{0};
        powerTqm.SetSuspendCallback([&fired, &shutDowns](const std::string&, const std::string&,
                                                         const std::string& action) {
            fired.fetch_add(1);
            if (action == "shutdown") shutDowns.fetch_add(1);
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
        std::string cancelResp = powerTqm.ClearPowerWhenDone(err);
        Check(JsonGetInt(cancelResp, "cleared", 0) == 1, "power: cancel clears the armed flag");
        Check(!JsonGetBool(powerTqm.PowerStatusJson(), "armed", true),
              "power: status reports not armed");
        int firedBefore = fired.load();
        Check(WaitForState(powerTqm, "PW2", "succeeded", 15000), "power: cancel job finished");
        Sleep(1500);
        Check(fired.load() == firedBefore, "power: cancel prevents suspend");

        // A shutdown request arms the same monitor but runs the shutdown action.
        const std::string shutJob =
            R"([{"id":"PW3","command":"cmd /c exit 0","summary":"shutdown job","timeoutSec":30,"shutdownWhenDone":true,"shutdownDelaySec":1}])";
        Check(JsonGetBool(powerTqm.TaskQueueAdd("powerq3", shutJob, 0, "", err), "ok", false),
              "power: shutdown job added");
        Check(WaitForState(powerTqm, "PW3", "succeeded", 15000), "power: shutdown job finished");
        uint64_t shutStart = GetNowTickMs();
        while (shutDowns.load() == 0 && GetNowTickMs() - shutStart < 8000) Sleep(100);
        Check(shutDowns.load() == 1, "power: shutdown fires with the shutdown action");
        const std::string shutStatus = powerTqm.PowerStatusJson();
        Check(!JsonGetBool(shutStatus, "armed", true), "power: shutdown flag cleared");

        powerTqm.SetSuspendCallback(nullptr);
        powerPm.SetProcessFinishedCallback(nullptr);
        powerTqm.ShutdownTaskQueueManager();
        powerTqm.SetProcessManager(nullptr);
        SetDataDirectoryOverride(dir);
    }

    // --- 090-2 Hang Detection tests ------------------------------------------
    {
        TaskQueueManager hangTqm;
        ProcessManager hangPm;
        hangTqm.SetProcessManager(&hangPm);
        hangPm.SetProcessFinishedCallback([&hangTqm](int pid, int ec, const std::string& kind) {
            hangTqm.OnProcessFinished(pid, ec, kind);
        });

        // Test 3: Worker idle defaults: dummy worker without explicit idle receives default 180s/600s
        std::string defWorkerJson =
            R"([{"id":"DW_DEF","command":"cmd /c exit 0","summary":"default idle worker","agent":"claude","timeoutSec":30}])";
        Check(JsonGetBool(hangTqm.TaskQueueAdd("defq", defWorkerJson, 0, "", err), "ok", false),
              "default worker added: " + err);
        std::string dwGet = hangTqm.TaskQueueGet("DW_DEF", false, 0, err);
        Check(dwGet.find("\"first_event_sec\":180") != std::string::npos,
              "worker without idle receives default first_event_sec 180: " + dwGet);
        Check(dwGet.find("\"output_sec\":600") != std::string::npos,
              "worker without idle receives default output_sec 600: " + dwGet);

        // Dummy worker with idle: 0 explicitly set does NOT inherit defaults (remains 0)
        std::string zeroWorkerJson =
            R"([{"id":"DW_ZERO","command":"cmd /c exit 0","summary":"disabled idle worker","agent":"claude","idle":0,"timeoutSec":30}])";
        Check(JsonGetBool(hangTqm.TaskQueueAdd("defq", zeroWorkerJson, 0, "", err), "ok", false),
              "zero worker added: " + err);
        std::string zwGet = hangTqm.TaskQueueGet("DW_ZERO", false, 0, err);
        Check(zwGet.find("\"first_event_sec\":0") != std::string::npos,
              "worker with idle: 0 keeps first_event_sec 0: " + zwGet);
        Check(zwGet.find("\"output_sec\":0") != std::string::npos,
              "worker with idle: 0 keeps output_sec 0: " + zwGet);

        // Test 4: Legacy job JSON compatibility: non-worker or job without first_event_sec parses with firstEventSec == 0
        std::string legJson =
            R"([{"id":"DW_LEG","command":"cmake --build .","summary":"cmake build","timeoutSec":30}])";
        Check(JsonGetBool(hangTqm.TaskQueueAdd("defq", legJson, 0, "", err), "ok", false),
              "legacy non-worker added: " + err);
        std::string legGet = hangTqm.TaskQueueGet("DW_LEG", false, 0, err);
        Check(legGet.find("\"first_event_sec\":0") != std::string::npos,
              "non-worker keeps first_event_sec 0: " + legGet);

        // Test with config.json override for worker_idle_defaults
        {
            std::wstring cfgFile = PathJoinWide(GetEffectiveDataDirectoryWide(), L"config.json");
            std::string oldCfg = ReadFileUtf8Wide(cfgFile);
            WriteFileAtomicWide(cfgFile,
                R"({"taskqueue":{"worker_idle_defaults":{"first_event_sec":75,"output_sec":350}}})");
            std::string cfgWorkerJson =
                R"([{"id":"DW_CFG","command":"cmd /c exit 0","summary":"cfg idle worker","agent":"claude","timeoutSec":30}])";
            Check(JsonGetBool(hangTqm.TaskQueueAdd("defq", cfgWorkerJson, 0, "", err), "ok", false),
                  "cfg worker added: " + err);
            std::string cfgGet = hangTqm.TaskQueueGet("DW_CFG", false, 0, err);
            Check(cfgGet.find("\"first_event_sec\":75") != std::string::npos,
                  "worker idle defaults honored from config.json first_event_sec: " + cfgGet);
            Check(cfgGet.find("\"output_sec\":350") != std::string::npos,
                  "worker idle defaults honored from config.json output_sec: " + cfgGet);
            if (!oldCfg.empty()) {
                WriteFileAtomicWide(cfgFile, oldCfg);
            } else {
                DeleteFileW(cfgFile.c_str());
            }
        }

        // Test 1: Startup hang triggers: sleep dummy worker without emitting JSON terminates via firstEventSec as startup_timed_out
        std::string hang1Json =
            R"JSON([{"id":"HANG1","name":"claude","command":"cmd /c ping -n 10 127.0.0.1 >nul","summary":"hang worker","idle":{"first_event_sec":2},"timeoutSec":30,"maxAttempts":1}])JSON";
        Check(JsonGetBool(hangTqm.TaskQueueAdd("hangq", hang1Json, 0, "", err), "ok", false),
              "hang1 added: " + err);
        Check(WaitForState(hangTqm, "HANG1", "failed", 10000), "hang1 failed via startup_timed_out");
        std::string h1Resp = hangTqm.TaskQueueGet("HANG1", false, 0, err);
        auto h1Json = ParseJobJson(h1Resp);
        Check(h1Json.is_object() && !h1Json["attempts"].empty(), "hang1 has attempt record");
        if (h1Json.is_object() && !h1Json["attempts"].empty()) {
            const auto& att = h1Json["attempts"][0];
            Check(att.value("reason", "") == "startup_timed_out",
                  "hang1 attempt reason is startup_timed_out (got " + att.value("reason", "") + ")");
            Check(att.contains("failure") && att["failure"].is_object(), "hang1 has failure diagnostic");
            if (att.contains("failure") && att["failure"].is_object()) {
                Check(att["failure"].value("class", "") == "startup_hang",
                      "hang1 failure class is startup_hang (got " + att["failure"].value("class", "") + ")");
                const auto& detail = att["failure"].value("detail", nlohmann::json::object());
                Check(detail.value("endKind", "") == "startup_timed_out",
                      "hang1 failure detail endKind is startup_timed_out");
                Check(detail.value("idleFirstEventSec", 0) == 2,
                      "hang1 failure detail idleFirstEventSec is 2");
                Check(detail.contains("processTree") && detail["processTree"].is_array(),
                      "hang1 failure detail contains processTree snapshot");
                if (detail.contains("processTree") && detail["processTree"].is_array()) {
                    Check(!detail["processTree"].empty(),
                          "hang1 processTree snapshot captured running processes");
                }
            }
        }

        // Test 2: Raw output does not prevent startup hang: emitting raw non-JSON text still terminates via firstEventSec
        std::wstring chatterBat = dir + L"\\chatter.bat";
        WriteFileAtomicWide(chatterBat, "@echo off\r\n:loop\r\necho chatter\r\nping -n 2 127.0.0.1 >nul\r\ngoto loop\r\n");
        std::string chatterCmd = "cmd /c \"" + WideToUtf8(chatterBat) + "\"";
        std::string hang2Json =
            R"JSON([{"id":"HANG2","name":"claude","command":")JSON" + JsonEscape(chatterCmd) +
            R"JSON( --json-stream","summary":"raw chatter worker","idle":{"first_event_sec":2},"timeoutSec":30,"maxAttempts":1}])JSON";
        Check(JsonGetBool(hangTqm.TaskQueueAdd("hangq", hang2Json, 0, "", err), "ok", false),
              "hang2 added: " + err);
        Check(WaitForState(hangTqm, "HANG2", "failed", 10000), "hang2 failed despite raw non-JSON output");
        std::string h2Resp = hangTqm.TaskQueueGet("HANG2", false, 0, err);
        auto h2Json = ParseJobJson(h2Resp);
        if (h2Json.is_object() && !h2Json["attempts"].empty()) {
            const auto& att = h2Json["attempts"][0];
            Check(att.value("reason", "") == "startup_timed_out",
                  "hang2 attempt reason is startup_timed_out with raw output");
            if (att.contains("failure") && att["failure"].is_object()) {
                Check(att["failure"].value("class", "") == "startup_hang",
                      "hang2 failure class is startup_hang");
            }
        }

        hangPm.SetProcessFinishedCallback(nullptr);
        hangTqm.ShutdownTaskQueueManager();
        hangTqm.SetProcessManager(nullptr);
    }

    // 090-3: takeover / complete with recorded decision and dependent propagation.
    {
        TaskQueueManager toTqm;
        ProcessManager toPm;
        toTqm.SetProcessManager(&toPm);
        toPm.SetProcessFinishedCallback([&toTqm](int pid, int ec, const std::string& kind) {
            toTqm.OnProcessFinished(pid, ec, kind);
        });
        // A separate queue lets us hold jobs pending while the takeover record is made.
        Check(JsonGetBool(toTqm.TaskQueueAdd("tkq",
              R"([{"id":"TK-INIT","command":"cmd /c exit 0","summary":"init","timeoutSec":30,"maxAttempts":1}])",
              0, "", err), "ok", false), "tkq init job added: " + err);
        Check(JsonGetBool(toTqm.TaskQueuePause("tkq", true, err), "ok", false), "tkq paused: " + err);
        std::string tkJobs = R"([
            {"id":"TK-A","command":"cmd /c exit 0","summary":"taken over A","timeoutSec":30,"maxAttempts":1},
            {"id":"TK-B","command":"cmd /c exit 0","summary":"dependent B","timeoutSec":30,"maxAttempts":1,"depends_on":["TK-A"]},
            {"id":"TK-C","command":"cmd /c exit 0","summary":"taken over C","timeoutSec":30,"maxAttempts":1},
            {"id":"TK-D","command":"cmd /c exit 0","summary":"dependent D","timeoutSec":30,"maxAttempts":1,"depends_on":["TK-C"]}
        ])";
        Check(JsonGetBool(toTqm.TaskQueueAdd("tkq", tkJobs, 0, "", err), "ok", false),
              "takeover jobs added: " + err);
        Check(GetState(toTqm, "TK-A") == "pending", "TK-A starts pending");
        Check(GetState(toTqm, "TK-B") == "pending", "TK-B starts pending");

        // A succeeded job cannot be taken over.
        Check(JsonGetBool(toTqm.TaskQueueAdd("tkokq",
              R"([{"id":"TK-OK","command":"cmd /c exit 0","summary":"ok","timeoutSec":30,"maxAttempts":1}])",
              0, "", err), "ok", false), "TK-OK added: " + err);
        Check(WaitForState(toTqm, "TK-OK", "succeeded", 10000), "TK-OK succeeded");
        std::string errSucceeded;
        std::string respSucceeded = toTqm.TaskQueueTakeover("TK-OK", "cli", "because", errSucceeded);
        Check(!JsonGetBool(respSucceeded, "ok", false) && !errSucceeded.empty(),
              "takeover rejects a succeeded job: " + errSucceeded);

        // reason is mandatory.
        std::string errNoReason;
        std::string respNoReason = toTqm.TaskQueueTakeover("TK-A", "cli", "", errNoReason);
        Check(!JsonGetBool(respNoReason, "ok", false) && !errNoReason.empty(),
              "takeover rejects a missing reason: " + errNoReason);

        // Take over TK-A and inspect the recorded decision.
        std::string errTake;
        std::string respTake = toTqm.TaskQueueTakeover("TK-A", "cli", "supervisor handles this", errTake);
        Check(JsonGetBool(respTake, "ok", false) && errTake.empty(), "takeover TK-A: " + errTake);
        Check(GetState(toTqm, "TK-A") == "takenOver", "TK-A is takenOver");
        auto tkA = ParseJobJson(toTqm.TaskQueueGet("TK-A", false, 0, err));
        Check(tkA.contains("takeover") && tkA["takeover"].value("by", "") == "cli" &&
              tkA["takeover"].value("reason", "") == "supervisor handles this" &&
              !tkA["takeover"].value("at", "").empty(),
              "takeover record has by/reason/at");

        // Complete is required with a reason; dependents must not advance before it.
        std::string errNoCompleteReason;
        std::string respNoCompleteReason =
            toTqm.TaskQueueComplete("TK-A", "", "succeeded", "", errNoCompleteReason);
        Check(!JsonGetBool(respNoCompleteReason, "ok", false) && !errNoCompleteReason.empty(),
              "complete rejects a missing reason: " + errNoCompleteReason);
        Sleep(300);
        Check(GetState(toTqm, "TK-B") == "pending", "TK-B stays pending before complete");

        std::string errComplete;
        std::string respComplete = toTqm.TaskQueueComplete(
            "TK-A", "reports/tk-a.md", "succeeded", "done by supervisor", errComplete);
        Check(JsonGetBool(respComplete, "ok", false) && errComplete.empty(), "complete TK-A: " + errComplete);
        tkA = ParseJobJson(toTqm.TaskQueueGet("TK-A", false, 0, err));
        Check(GetState(toTqm, "TK-A") == "takenOver", "TK-A stays takenOver after complete");
        Check(tkA["takeover"].value("result", "") == "succeeded" &&
              !tkA["takeover"].value("completed_at", "").empty() &&
              tkA["takeover"].value("report", "") == "reports/tk-a.md" &&
              tkA["takeover"].value("completion_reason", "") == "done by supervisor",
              "complete record has result/completed_at/report/completion_reason");

        // A failed takeover result blocks its dependent.
        Check(JsonGetBool(toTqm.TaskQueueTakeover("TK-C", "cli", "supervisor stops delegation", errTake),
                          "ok", false), "takeover TK-C: " + errTake);
        Check(JsonGetBool(toTqm.TaskQueueComplete("TK-C", "", "failed", "unresolved", errComplete),
                          "ok", false), "complete TK-C failed: " + errComplete);

        // Resume the queue: B advances on the succeeded takeover, D is blocked by the failed one.
        Check(JsonGetBool(toTqm.TaskQueuePause("tkq", false, err), "ok", false), "tkq resumed: " + err);
        Check(WaitForState(toTqm, "TK-B", "succeeded", 10000),
              "TK-B succeeded after takeover completed with succeeded result");
        Check(WaitForState(toTqm, "TK-D", "blocked", 10000),
              "TK-D blocked after takeover completed with failed result");

        TaskQueueCounts counts = toTqm.GetJobCounts();
        Check(counts.takenOver >= 2, "counts include takenOver jobs");
        toPm.SetProcessFinishedCallback(nullptr);
        toTqm.ShutdownTaskQueueManager();
        toTqm.SetProcessManager(nullptr);
    }

    // ------------------------------------------------------------------
    // 091-6: shared engine health / circuit breaker
    // ------------------------------------------------------------------
    {
        namespace eh = engine_health;
        const std::wstring dataDir = PathJoinWide(GetCurrentDirectoryWide(), L".hancho\\0916-smoke");
        EnsureDirectoryWide(dataDir);
        SetDataDirectoryOverride(dataDir);
        WriteFileAtomicWide(PathJoinWide(dataDir, L"config.json"),
            R"({"engine_health":{"probe_initial_sec":1,"probe_max_sec":2,"probe_timeout_sec":3}})");
        WriteFileAtomicWide(PathJoinWide(dataDir, L"engines.json"),
            R"({"engines":{"okdummy":{"command":"cmd /c echo OK","stream":"dummy","models":["m1","m2","m4","m9"]},)"
            R"("hangdummy":{"command":"cmd /c ping -n 5 127.0.0.1 >nul","stream":"dummy","models":["m1"]}},)"
            R"("default":"okdummy"})");
        const std::wstring healthPath = eh::DefaultPath();
        RemoveFileWide(healthPath);
        RemoveFileWide(eh::EventsPath());
        RemoveFileWide(PathJoinWide(dataDir, L"probes.json"));
        RemoveFileWide(PathJoinWide(PathJoinWide(dataDir, L"taskqueues"), L"eh.json"));
        const eh::Settings settings = eh::LoadSettings();
        Check(settings.probeInitialSec == 1 && settings.probeMaxSec == 2 &&
              settings.probeTimeoutSec == 3, "0916: shortened config keys are read");
        const int64_t t0 = eh::NowEpochMs();

        // (1) startup_hang marks an engine down and writes engineDown.
        Check(eh::MarkDown(healthPath, "hangdummy", "m1", "startup_hang", "no events", t0, 0, settings),
              "0916: startup_hang marks engine down");
        eh::Store store;
        std::string errH;
        Check(eh::Load(healthPath, store, errH), "0916: state file loads: " + errH);
        Check(eh::IsDown(store, "hangdummy", "m1"), "0916: engine is down");
        const eh::Entry* downEntry = eh::Find(store, "hangdummy", "m1");
        Check(downEntry && downEntry->reason == "startup_hang" && downEntry->probeCount == 0 &&
              downEntry->nextProbeAtMs >= t0 + 900, "0916: down fields since/reason/nextProbeAt");
        Check(!eh::ProbeDue(*downEntry, t0) && eh::ProbeDue(*downEntry, t0 + 1100),
              "0916: nextProbeAt gates the first probe");
        Check(ReadFileUtf8Wide(eh::EventsPath()).find("\"engineDown\"") != std::string::npos,
              "0916: engineDown event is written");

        // (4) backoff doubles from 1s to 2s after a failed probe.
        Check(eh::RecordFailedProbe(healthPath, "hangdummy", "m1", "startup_hang", "still silent",
                                    t0 + 1100, settings), "0916: failed probe recorded");
        Check(eh::Load(healthPath, store, errH), "0916: state reloads after probe");
        downEntry = eh::Find(store, "hangdummy", "m1");
        Check(downEntry && downEntry->probeCount == 1 &&
              downEntry->nextProbeAtMs >= t0 + 1100 + 1900, "0916: backoff interval doubles");

        // (5) explicit retryAt holds probing even after nextProbeAt.
        Check(eh::MarkDown(healthPath, "okdummy", "m2", "rate_limited", "quota exhausted",
                           t0, t0 + 60000, settings), "0916: rate_limited marks engine down");
        Check(eh::Load(healthPath, store, errH), "0916: state reloads for retryAt");
        const eh::Entry* quotaEntry = eh::Find(store, "okdummy", "m2");
        Check(quotaEntry && quotaEntry->retryAtMs == t0 + 60000 &&
              !eh::ProbeDue(*quotaEntry, t0 + 5000), "0916: no probe before retryAt");

        // (7) a fresh Load sees the persisted state.
        eh::Store restored;
        Check(eh::Load(healthPath, restored, errH) && eh::IsDown(restored, "hangdummy", "m1") &&
              eh::IsDown(restored, "okdummy", "m2"), "0916: state survives a fresh load");
        Check(ReadFileUtf8Wide(healthPath).find("\"nextProbeAt\"") != std::string::npos,
              "0916: state file has the documented fields");

        // (6) manual --reset equivalent.
        Check(eh::Reset(healthPath, "okdummy", "m2", eh::NowEpochMs()), "0916: reset succeeds");
        Check(eh::Load(healthPath, store, errH) && !eh::IsDown(store, "okdummy", "m2"),
              "0916: reset returns the engine to healthy");
        Check(ReadFileUtf8Wide(eh::EventsPath()).find("\"engineRecovered\"") != std::string::npos,
              "0916: engineRecovered event is written");

        // Diagnosis context: recent success + another 0-event failure => quota_suspected.
        TaskQueueFailureInput diagIn;
        diagIn.engine = "opencode";
        diagIn.model = "opencode-go/deepseek-v4.1-flash";
        diagIn.endKind = "startup_timed_out";
        diagIn.recentSuccess = true;
        diagIn.otherZeroEventFailure = true;
        diagIn.engineDown = true;
        TaskQueueFailureDiagnostic diag = ClassifyTaskQueueFailure(diagIn);
        bool hasQuota = false, hasEngineDown = false;
        for (const auto& c : diag.likelyCauses) if (c.id == "quota_suspected") hasQuota = true;
        for (const auto& s : diag.nextSteps) if (s.action == "engine_down") hasEngineDown = true;
        Check(diag.failureClass == "startup_hang" && hasQuota,
              "0916: quota_suspected added to likelyCauses");
        Check(hasEngineDown, "0916: engine_down added to nextSteps");

        // (1/2) held job does not start; a different engine is unaffected.
        TaskQueueManager ehTqm;
        ProcessManager ehPm;
        ehTqm.SetProcessManager(&ehPm);
        ehPm.SetProcessFinishedCallback([&ehTqm](int pid, int ec, const std::string& kind) {
            ehTqm.OnProcessFinished(pid, ec, kind);
        });
        eh::MarkDown(healthPath, "hangdummy", "m1", "startup_hang", "no events",
                     eh::NowEpochMs(), 0, settings);
        std::string errAdd;
        const std::string addHeld = ehTqm.TaskQueueAdd("eh",
            R"([{"id":"EH-HELD","summary":"held","command":"cmd /c echo OK -Engine hangdummy -Model m1","timeout_sec":10,"max_attempts":2}])",
            1, "", errAdd);
        Check(JsonGetBool(addHeld, "ok", false), "0916: held job added: " + errAdd + " " + addHeld);
        const std::string addOther = ehTqm.TaskQueueAdd("eh",
            R"([{"id":"EH-OTHER","summary":"other","command":"cmd /c echo OK -Engine okdummy -Model m9","timeout_sec":10,"max_attempts":1}])",
            1, "", errAdd);
        Check(JsonGetBool(addOther, "ok", false), "0916: other-engine job added: " + errAdd + " " + addOther);
        Sleep(700);
        Check(GetState(ehTqm, "EH-HELD") == "pending", "0916: down engine's job stays pending");
        std::string heldJson = ehTqm.TaskQueueGet("EH-HELD", false, 0, errAdd);
        Check(heldJson.find("engine down: hangdummy/m1") != std::string::npos,
              "0916: list shows the hold reason");
        Check(WaitForState(ehTqm, "EH-OTHER", "succeeded", 8000),
              "0916: other engine's job succeeds");
        Check(eh::Reset(healthPath, "hangdummy", "m1", eh::NowEpochMs()),
              "0916: manual recovery of hangdummy");
        Check(WaitForState(ehTqm, "EH-HELD", "succeeded", 8000),
              "0916: held job starts after recovery");

        // (3) core auto-probe: down engine recovers and the waiting job is released.
        eh::MarkDown(healthPath, "okdummy", "m4", "startup_hang", "no events",
                     eh::NowEpochMs(), 0, settings);
        const std::string addProbe = ehTqm.TaskQueueAdd("eh",
            R"([{"id":"EH-PROBE","summary":"probe","command":"cmd /c echo OK -Engine okdummy -Model m4","timeout_sec":10,"max_attempts":1}])",
            1, "", errAdd);
        Check(JsonGetBool(addProbe, "ok", false), "0916: auto-probe job added: " + errAdd + " " + addProbe);
        Check(WaitForState(ehTqm, "EH-PROBE", "succeeded", 12000),
              "0916: core probe restores health and starts the waiting job");
        Check(eh::Load(healthPath, store, errH) && !eh::IsDown(store, "okdummy", "m4"),
              "0916: engine is healthy after the successful probe");

        // (18) approved worker-list switch on a down engine, and engine_switch:false.
        const std::wstring llmPath = llm::DefaultPath();
        RemoveFileWide(llmPath);
        llm::Add(llmPath, "worker", "swA", "mA", "", 1, true, "", "dashboard", "");
        llm::Add(llmPath, "worker", "swB", "mB", "", 2, true, "", "dashboard", "");
        llm::Add(llmPath, "worker", "swC", "mC", "", 3, true, "", "dashboard", "");
        llm::Add(llmPath, "worker", "swD", "mD", "", 4, true, "", "dashboard", "");
        eh::MarkDown(healthPath, "swA", "mA", "startup_hang", "no events",
                     eh::NowEpochMs(), 0, settings);
        const std::string addSwitch = ehTqm.TaskQueueAdd("eh",
            R"([{"id":"EH-SWITCH","summary":"switch","command":"cmd /c echo OK -Engine swA -Model mA","timeout_sec":10,"max_attempts":1}])",
            1, "", errAdd);
        Check(JsonGetBool(addSwitch, "ok", false), "0916: switch job added: " + errAdd + " " + addSwitch);
        Check(WaitForState(ehTqm, "EH-SWITCH", "succeeded", 10000),
              "0916: job switches to the next approved worker-list candidate");
        const nlohmann::json switched = ParseJobJson(ehTqm.TaskQueueGet("EH-SWITCH", false, 0, errAdd));
        Check(switched.value("command", "").find("swB") != std::string::npos &&
              switched.value("notes", "").find("worker list #2") != std::string::npos,
              "0916: switch records the approving list row in job notes");

        eh::MarkDown(healthPath, "swC", "mC", "startup_hang", "no events",
                     eh::NowEpochMs(), 0, settings);
        const std::string addNoSwitch = ehTqm.TaskQueueAdd("eh",
            R"([{"id":"EH-NOSWITCH","summary":"no switch","command":"cmd /c echo OK -Engine swC -Model mC","timeout_sec":10,"max_attempts":1,"engine_switch":false}])",
            1, "", errAdd);
        Check(JsonGetBool(addNoSwitch, "ok", false), "0916: no-switch job added: " + errAdd + " " + addNoSwitch);
        Sleep(700);
        Check(GetState(ehTqm, "EH-NOSWITCH") == "pending",
              "0916: engine_switch:false keeps the job held on its down engine");

        // Plain commands with no engine/model are never held.
        const std::string addPlain = ehTqm.TaskQueueAdd("eh",
            R"([{"id":"EH-PLAIN","summary":"plain","command":"cmd /c echo OK","timeout_sec":10,"max_attempts":1}])",
            1, "", errAdd);
        Check(JsonGetBool(addPlain, "ok", false), "0916: plain job added: " + errAdd + " " + addPlain);
        Check(WaitForState(ehTqm, "EH-PLAIN", "succeeded", 8000),
              "0916: plain job without engine/model is unaffected");
        ehPm.SetProcessFinishedCallback(nullptr);
        ehTqm.ShutdownTaskQueueManager();
        ehTqm.SetProcessManager(nullptr);
        SetDataDirectoryOverride(dir);
    }

    // Task lineage data layer (100-1): `supersedes` links a redo to the failed job it replaces.
    {
        const std::string lineageState =
            "{\"name\":\"lineageq\",\"concurrency\":1,\"paused\":false,\"version\":1,"
            "\"notify_file\":\"\",\"max_timeout_sec\":7200,\"default_max_attempts\":1,\"jobs\":["
            "{\"id\":\"LA\",\"taskqueue\":\"lineageq\",\"summary\":\"a\",\"command\":\"cmd /c exit 1\","
            "\"timeout_sec\":30,\"max_attempts\":1,\"state\":\"failed\"},"
            "{\"id\":\"LS\",\"taskqueue\":\"lineageq\",\"summary\":\"s\",\"command\":\"cmd /c exit 0\","
            "\"timeout_sec\":30,\"max_attempts\":1,\"state\":\"succeeded\"},"
            "{\"id\":\"LP\",\"taskqueue\":\"lineageq\",\"summary\":\"p\",\"command\":\"cmd /c exit 0\","
            "\"timeout_sec\":30,\"max_attempts\":1,\"state\":\"pending\",\"paused\":true},"
            "{\"id\":\"LBT\",\"taskqueue\":\"lineageq\",\"summary\":\"bt\",\"command\":\"cmd /c exit 1\","
            "\"timeout_sec\":30,\"max_attempts\":1,\"state\":\"failed\",\"bt\":{\"id\":\"btx\",\"node\":\"n\",\"run\":1}}"
            "]}";
        WriteFileAtomicWide(TaskQueueManager::GetTaskQueueFileWide("lineageq"), lineageState);
        std::string errL;
        TaskQueueManager lq;
        ProcessManager lqPm;
        lq.SetProcessManager(&lqPm);
        lqPm.SetProcessFinishedCallback([&lq](int pid, int ec, const std::string& kind) {
            lq.OnProcessFinished(pid, ec, kind);
        });
        auto addRaw = [&](TaskQueueManager& target, const std::string& jobs) {
            std::string e;
            return target.TaskQueueAdd("lineageq", jobs, 1, "", e);
        };

        // Without `supersedes` the three lineage keys stay out of the output.
        const std::string addPlain = addRaw(lq,
            R"J([{"id":"LN","summary":"n","command":"cmd /c exit 1","timeout_sec":30,"max_attempts":1}])J");
        Check(JsonGetBool(addPlain, "ok", false) && addPlain.find("\"lineage\"") == std::string::npos,
              "100-1: plain add has no lineage in its response");
        const nlohmann::json plainJob = ParseJobJson(lq.TaskQueueGet("LN", false, 0, errL));
        Check(plainJob.is_object() && !plainJob.contains("supersedes") && !plainJob.contains("task_id") &&
                  !plainJob.contains("superseded_by"),
              "100-1: plain job JSON has no lineage keys");

        // A camelCase `supersedes` redoes the failed job LA. task_id / superseded_by are ignored.
        const std::string addRedo = addRaw(lq,
            R"J([{"id":"LB","summary":"redo a","command":"cmd /c exit 1","timeout_sec":30,"max_attempts":1,)J"
            R"J("supersedes":"LA","task_id":"ZZ","superseded_by":"ZZ"}])J");
        Check(JsonGetBool(addRedo, "ok", false), "100-1: redo add accepted: " + addRedo);
        const nlohmann::json redoResp = nlohmann::json::parse(addRedo, nullptr, false);
        Check(redoResp.is_object() && redoResp.contains("lineage") && redoResp["lineage"].size() == 1 &&
                  redoResp["lineage"][0].value("id", "") == "LB" &&
                  redoResp["lineage"][0].value("supersedes", "") == "LA" &&
                  redoResp["lineage"][0].value("task_id", "") == "LA",
              "100-1: add response reports the lineage link");
        Check(WaitForState(lq, "LB", "failed", 8000), "100-1: redo job LB finishes");
        const nlohmann::json laJob = ParseJobJson(lq.TaskQueueGet("LA", false, 0, errL));
        const nlohmann::json lbJob = ParseJobJson(lq.TaskQueueGet("LB", false, 0, errL));
        Check(lbJob.value("supersedes", "") == "LA" && lbJob.value("task_id", "") == "LA" &&
                  !lbJob.contains("superseded_by"),
              "100-1: LB records supersedes LA and task_id LA, ignoring caller task_id/superseded_by");
        Check(laJob.value("superseded_by", "") == "LB" && !laJob.contains("supersedes"),
              "100-1: LA records superseded_by LB");

        // The lineage survives a save and reload.
        lqPm.SetProcessFinishedCallback(nullptr);
        lq.ShutdownTaskQueueManager();
        lq.SetProcessManager(nullptr);
        TaskQueueManager reloadedL;
        ProcessManager reloadedPm;
        reloadedL.SetProcessManager(&reloadedPm);
        reloadedPm.SetProcessFinishedCallback([&reloadedL](int pid, int ec, const std::string& kind) {
            reloadedL.OnProcessFinished(pid, ec, kind);
        });
        const nlohmann::json laReloaded = ParseJobJson(reloadedL.TaskQueueGet("LA", false, 0, errL));
        const nlohmann::json lbReloaded = ParseJobJson(reloadedL.TaskQueueGet("LB", false, 0, errL));
        Check(laReloaded.value("superseded_by", "") == "LB" &&
                  lbReloaded.value("supersedes", "") == "LA" && lbReloaded.value("task_id", "") == "LA",
              "100-1: lineage survives save and reload");

        // Superseding the older LA takes over the newest job LB: the chain stays one line.
        const std::string addChain = addRaw(reloadedL,
            R"J([{"id":"LC","summary":"redo again","command":"cmd /c exit 1","timeout_sec":30,"max_attempts":1,"supersedes":"LA"}])J");
        Check(JsonGetBool(addChain, "ok", false), "100-1: chained redo accepted: " + addChain);
        const nlohmann::json chainResp = nlohmann::json::parse(addChain, nullptr, false);
        Check(chainResp.is_object() && chainResp.contains("lineage") &&
                  chainResp["lineage"][0].value("supersedes", "") == "LB" &&
                  chainResp["lineage"][0].value("task_id", "") == "LA",
              "100-1: stale target is rewritten to the newest job of the lineage");
        Check(WaitForState(reloadedL, "LC", "failed", 8000), "100-1: chained job LC finishes");
        const nlohmann::json lbChained = ParseJobJson(reloadedL.TaskQueueGet("LB", false, 0, errL));
        const nlohmann::json lcChained = ParseJobJson(reloadedL.TaskQueueGet("LC", false, 0, errL));
        Check(lbChained.value("superseded_by", "") == "LC" && lcChained.value("supersedes", "") == "LB" &&
                  lcChained.value("task_id", "") == "LA",
              "100-1: LB.superseded_by == LC, LC.supersedes == LB, task_id stays LA");

        // Invalid links are rejected, and nothing is added.
        const char* errorCases[][2] = {
            {R"J([{"id":"LE1","summary":"e","command":"cmd /c exit 1","timeout_sec":30,"max_attempts":1,"supersedes":"LS"}])J",
             "can only supersede"},
            {R"J([{"id":"LE1","summary":"e","command":"cmd /c exit 1","timeout_sec":30,"max_attempts":1,"supersedes":"LP"}])J",
             "can only supersede"},
            {R"J([{"id":"LE1","summary":"e","command":"cmd /c exit 1","timeout_sec":30,"max_attempts":1,"supersedes":"NOPE"}])J",
             "unknown supersedes job"},
            {R"J([{"id":"LE1","summary":"e","command":"cmd /c exit 1","timeout_sec":30,"max_attempts":1,"supersedes":"LBT"}])J",
             "use bt_retry"},
            {R"J([{"id":"LE1","summary":"e","command":"cmd /c exit 1","timeout_sec":30,"max_attempts":1},)J"
             R"J({"id":"LE2","summary":"e","command":"cmd /c exit 1","timeout_sec":30,"max_attempts":1,"supersedes":"LE1"}])J",
             "must name an existing job"},
            {R"J([{"id":"LE1","summary":"e","command":"cmd /c exit 1","timeout_sec":30,"max_attempts":1,"supersedes":"LC"},)J"
             R"J({"id":"LE2","summary":"e","command":"cmd /c exit 1","timeout_sec":30,"max_attempts":1,"supersedes":"LC"}])J",
             "already superseded"},
        };
        for (const auto& errCase : errorCases) {
            const std::string resp = addRaw(reloadedL, errCase[0]);
            Check(!JsonGetBool(resp, "ok", true) && resp.find(errCase[1]) != std::string::npos,
                  std::string("100-1: rejected with '") + errCase[1] + "': " + resp);
        }
        Check(GetState(reloadedL, "LE1").empty() && GetState(reloadedL, "LE2").empty(),
              "100-1: rejected links add no jobs");
        Check(ParseJobJson(reloadedL.TaskQueueGet("LA", false, 0, errL)).value("superseded_by", "") == "LB",
              "100-1: rejected links do not change the lineage");

        // The redo reason (100-1 5b): kept on the job, reported in the response, survives a reload.
        const std::string addReason = addRaw(reloadedL,
            R"J([{"id":"LD","summary":"redo with reason","command":"cmd /c exit 1","timeout_sec":30,"max_attempts":1,"supersedes":"LC","supersede_reason":"the path was wrong"}])J");
        Check(JsonGetBool(addReason, "ok", false), "100-1: redo with reason accepted: " + addReason);
        const nlohmann::json reasonResp = nlohmann::json::parse(addReason, nullptr, false);
        Check(reasonResp.is_object() && reasonResp.contains("lineage") &&
                  reasonResp["lineage"][0].value("reason", "") == "the path was wrong" &&
                  reasonResp.contains("warnings") && reasonResp["warnings"].empty(),
              "100-1: lineage response carries the reason and no warning");
        Check(WaitForState(reloadedL, "LD", "failed", 8000), "100-1: reason job LD finishes");
        reloadedPm.SetProcessFinishedCallback(nullptr);
        reloadedL.ShutdownTaskQueueManager();
        reloadedL.SetProcessManager(nullptr);
        TaskQueueManager finalL;
        ProcessManager finalPm;
        finalL.SetProcessManager(&finalPm);
        finalPm.SetProcessFinishedCallback([&finalL](int pid, int ec, const std::string& kind) {
            finalL.OnProcessFinished(pid, ec, kind);
        });
        Check(ParseJobJson(finalL.TaskQueueGet("LD", false, 0, errL)).value("supersede_reason", "") ==
                  "the path was wrong",
              "100-1: supersede_reason survives save and reload");

        // Redoing LD without a reason still succeeds, with a warning.
        const std::string addNoReason = addRaw(finalL,
            R"J([{"id":"LE3","summary":"redo no reason","command":"cmd /c exit 1","timeout_sec":30,"max_attempts":1,"supersedes":"LD"}])J");
        Check(JsonGetBool(addNoReason, "ok", false), "100-1: redo without reason still accepted: " + addNoReason);
        const nlohmann::json noReasonResp = nlohmann::json::parse(addNoReason, nullptr, false);
        bool sawNoReasonWarning = false;
        if (noReasonResp.is_object() && noReasonResp.contains("warnings")) {
            for (const auto& w : noReasonResp["warnings"]) {
                if (w.value("job", "") == "LE3" && w.value("warning", "") == "supersede_without_reason")
                    sawNoReasonWarning = true;
            }
        }
        Check(sawNoReasonWarning, "100-1: missing reason gives a supersede_without_reason warning");

        // A reason without `supersedes` is dropped silently, even when it is longer than the limit.
        const std::string addDropped = addRaw(finalL,
            R"J([{"id":"LF","summary":"f","command":"cmd /c exit 1","timeout_sec":30,"max_attempts":1,"supersede_reason":")J" +
            std::string(2001, 'x') + R"J("}])J");
        Check(JsonGetBool(addDropped, "ok", false) && addDropped.find("\"lineage\"") == std::string::npos,
              "100-1: reason without supersedes is dropped, not an error");
        Check(!ParseJobJson(finalL.TaskQueueGet("LF", false, 0, errL)).contains("supersede_reason"),
              "100-1: dropped reason is not stored");

        // A reason over 2000 bytes on a redo is rejected, and nothing is added.
        const std::string addLong = addRaw(finalL,
            R"J([{"id":"LG","summary":"g","command":"cmd /c exit 1","timeout_sec":30,"max_attempts":1,"supersedes":"LD","supersede_reason":")J" +
            std::string(2001, 'x') + R"J("}])J");
        Check(!JsonGetBool(addLong, "ok", true) && addLong.find("supersede_reason exceeds 2000 bytes") != std::string::npos,
              "100-1: reason over 2000 bytes is rejected");
        Check(GetState(finalL, "LG").empty(), "100-1: rejected long reason adds no job");
        finalPm.SetProcessFinishedCallback(nullptr);
        finalL.ShutdownTaskQueueManager();
        finalL.SetProcessManager(nullptr);
    }

    // Task records (card 100-2a): a task is a Hancho record with its own id, state and log.
    {
        const std::string ts = std::to_string(GetNowEpochSec());
        const std::string tq = "taskq" + ts;
        std::string e;
        TaskQueueManager tm;
        ProcessManager tPm;
        tm.SetProcessManager(&tPm);
        tPm.SetProcessFinishedCallback([&tm](int pid, int ec, const std::string& kind) {
            tm.OnProcessFinished(pid, ec, kind);
        });
        auto parse = [](const std::string& s) { return nlohmann::json::parse(s, nullptr, false); };
        auto jobJson = [](const std::string& id, const std::string& command, const std::string& extra) {
            return "[{\"id\":\"" + id + "\",\"summary\":\"s\",\"command\":\"" + command +
                   "\",\"timeout_sec\":30,\"max_attempts\":1" + extra + "}]";
        };
        auto addTask = [&](const std::string& jobs, const std::string& task) {
            std::string err;
            return tm.TaskQueueAdd(tq, jobs, 1, "", err, "", task);
        };

        // Create: automatic and explicit ids, and the id and size checks.
        const nlohmann::json autoT = parse(tm.TaskCreate("first", "intent", "", "", e));
        Check(autoT.value("ok", false) && autoT.value("id", std::string()).rfind("task_", 0) == 0,
              "100-2a: task create picks an automatic task_ id");
        const std::string tA = "task_a" + ts;
        const std::string tB = "task_b" + ts;
        const std::string tC = "task_c" + ts;
        Check(parse(tm.TaskCreate("A", "", tA, "", e)).value("ok", false), "100-2a: task create with --id");
        Check(!parse(tm.TaskCreate("A again", "", tA, "", e)).value("ok", true), "100-2a: duplicate task id is rejected");
        Check(parse(tm.TaskCreate("B", "", tB, "", e)).value("ok", false), "100-2a: second task created");
        Check(parse(tm.TaskCreate("C", "", tC, "", e)).value("ok", false), "100-2a: third task created");
        Check(!parse(tm.TaskCreate("bad", "", "nope", "", e)).value("ok", true), "100-2a: task id must start with task_");
        Check(!parse(tm.TaskCreate("bad", "", "task_a b", "", e)).value("ok", true), "100-2a: task id rejects spaces");
        Check(!parse(tm.TaskCreate("bad", "", "task_" + std::string(60, 'x'), "", e)).value("ok", true),
              "100-2a: task id over 64 characters is rejected");
        Check(!parse(tm.TaskCreate(std::string(201, 'x'), "", "", "", e)).value("ok", true),
              "100-2a: title over 200 bytes is rejected");
        Check(!parse(tm.TaskCreate("big", std::string(16 * 1024 + 1, 'x'), "", "", e)).value("ok", true),
              "100-2a: intent over 16 KB is rejected");
        Check(tm.TaskState(tA).value("state", std::string()) == "open", "100-2a: a task with no jobs is open");

        // add --task: a failing job, then a redo that succeeds. The lineage decides the task state.
        const std::string j1 = "TK1_" + ts;
        const std::string j2 = "TK2_" + ts;
        Check(parse(addTask(jobJson(j1, "cmd /c exit 1", ""), tA)).value("ok", false), "100-2a: add --task to an open task");
        Check(WaitForState(tm, j1, "failed", 8000), "100-2a: first job of task A fails");
        Check(tm.TaskState(tA).value("state", std::string()) == "failed", "100-2a: a task whose latest job failed is failed");
        Check(parse(addTask(jobJson(j2, "cmd /c exit 0", ",\"supersedes\":\"" + j1 + "\",\"supersede_reason\":\"retry\""), tA))
                  .value("ok", false),
              "100-2a: redo joins task A");
        Check(WaitForState(tm, j2, "succeeded", 8000), "100-2a: redo in task A succeeds");
        Check(tm.TaskState(tA).value("state", std::string()) == "succeeded",
              "100-2a: the newest job of the lineage decides: succeeded");

        // A redo may not cross tasks: j4 belongs to C, and a redo of it under A is rejected.
        const std::string j4 = "TK4_" + ts;
        Check(parse(addTask(jobJson(j4, "cmd /c exit 1", ""), tC)).value("ok", false), "100-2a: job for task C added");
        Check(WaitForState(tm, j4, "failed", 8000), "100-2a: job of task C fails");
        const nlohmann::json cross = parse(addTask(jobJson("TK3_" + ts, "cmd /c exit 0",
                                                           ",\"supersedes\":\"" + j4 + "\""), tA));
        Check(!cross.value("ok", true) && cross.value("error", std::string()).find("belongs to task") != std::string::npos,
              "100-2a: supersedes a job of another task is rejected");

        // Closed, hidden, reserved and missing tasks take no job.
        Check(parse(tm.TaskClose(tB, "failed", "gave up", e)).value("ok", false), "100-2a: task B closed");
        Check(tm.TaskState(tB).value("state", std::string()) == "closed" &&
                  tm.TaskState(tB).value("result", std::string()) == "failed",
              "100-2a: a closed task reports its result");
        Check(!parse(addTask(jobJson("TK5_" + ts, "cmd /c exit 0", ""), tB)).value("ok", true),
              "100-2a: a closed task takes no new job");
        Check(!parse(tm.TaskClose(tB, "failed", "again", e)).value("ok", true), "100-2a: a closed task cannot close again");
        Check(parse(tm.TaskReopen(tB, "more work", e)).value("ok", false) && tm.TaskState(tB).value("state", std::string()) == "open",
              "100-2a: reopen returns the task to its computed state");
        Check(!parse(addTask(jobJson("TK6_" + ts, "cmd /c exit 0", ",\"hidden\":true"), tA)).value("ok", true),
              "100-2a: a hidden job cannot join a task");
        Check(!parse(addTask(jobJson("task_x" + ts, "cmd /c exit 0", ""), "")).value("ok", true),
              "100-2a: a job id starting with task_ is reserved");
        Check(!parse(addTask(jobJson("TK7_" + ts, "cmd /c exit 0", ""), "task_missing" + ts)).value("ok", true),
              "100-2a: a missing task takes no job");

        // Close A: the result is recorded, and a reopen goes back to the computed state.
        Check(parse(tm.TaskClose(tA, "succeeded", "all done", e)).value("ok", false), "100-2a: task A closed");
        Check(tm.TaskState(tA).value("state", std::string()) == "closed" &&
                  tm.TaskState(tA).value("result", std::string()) == "succeeded",
              "100-2a: closed task A reports succeeded");
        Check(parse(tm.TaskReopen(tA, "found more", e)).value("ok", false) &&
                  tm.TaskState(tA).value("state", std::string()) == "succeeded",
              "100-2a: reopened task A is succeeded again");

        // Removing the newest job keeps the state: the removed job stays on the task record.
        Check(parse(tm.TaskQueueRemove(j2, false, e)).value("ok", false), "100-2a: newest job removed");
        Check(tm.TaskState(tA).value("state", std::string()) == "succeeded",
              "100-2a: state survives removal of the newest job");
        const nlohmann::json shownA = parse(tm.TaskShow(tA, 0, e));
        Check(shownA.value("ok", false) && shownA["task"].value("removedJobs", nlohmann::json::array()).size() == 1,
              "100-2a: removed job is recorded on the task");

        // The log: the kinds written automatically, and the note from the supervisor.
        Check(parse(tm.TaskNote(tA, "checked the redo", "sess-1", e)).value("ok", false), "100-2a: task note accepted");
        const nlohmann::json logA = parse(tm.TaskShow(tA, 0, e));
        std::vector<std::string> kinds;
        for (const auto& entry : logA["log"]) kinds.push_back(entry.value("kind", std::string()));
        auto hasKind = [&kinds](const std::string& k) {
            return std::find(kinds.begin(), kinds.end(), k) != kinds.end();
        };
        Check(hasKind("created") && hasKind("job_added") && hasKind("superseded") && hasKind("note") &&
                  hasKind("closed") && hasKind("reopened"),
              "100-2a: log holds created, job_added, superseded, note, closed and reopened");

        // Task history: each attempt's end is logged with its result, and a job that gives up
        // is logged as job_failed (j1 failed with exit 1, then j2 succeeded).
        bool j1Failed = false, j1GaveUp = false, j2Ok = false;
        for (const auto& entry : logA["log"]) {
            const std::string kind = entry.value("kind", std::string());
            const std::string job = entry.value("job", std::string());
            if (kind == "attempt_finished" && job == j1 && entry.value("attempt", 0) == 1 &&
                entry.value("result", std::string()) == "failed" && entry.value("exitCode", -1) == 1 &&
                entry.value("text", std::string()).find("exit 1") != std::string::npos) {
                j1Failed = true;
            }
            if (kind == "job_failed" && job == j1) j1GaveUp = true;
            if (kind == "attempt_finished" && job == j2 && entry.value("result", std::string()) == "succeeded") {
                j2Ok = true;
            }
        }
        Check(j1Failed, "history: attempt_finished records the failed attempt with its exit code");
        Check(j1GaveUp, "history: job_failed records the job that gave up");
        Check(j2Ok, "history: attempt_finished records the succeeded attempt");

        // Save and reload: the state and the log come back.
        tPm.SetProcessFinishedCallback(nullptr);
        tm.ShutdownTaskQueueManager();
        tm.SetProcessManager(nullptr);
        TaskQueueManager reloaded;
        ProcessManager reloadPm;
        reloaded.SetProcessManager(&reloadPm);
        reloadPm.SetProcessFinishedCallback([&reloaded](int pid, int ec, const std::string& kind) {
            reloaded.OnProcessFinished(pid, ec, kind);
        });
        Check(reloaded.TaskState(tA).value("state", std::string()) == "succeeded" &&
                  parse(reloaded.TaskShow(tA, 0, e))["log"].size() == logA["log"].size(),
              "100-2a: task state and log survive save and reload");

        // Purge: only closed tasks older than the limit go.
        Check(parse(reloaded.TaskClose(tB, "failed", "old", e)).value("ok", false), "100-2a: task B closed for purge");
        Sleep(2100);
        Check(parse(reloaded.TaskPurgeOlderThan(1, e)).value("purged", 0) >= 1 &&
                  !parse(reloaded.TaskShow(tB, 0, e)).value("ok", true),
              "100-2a: purge removes an old closed task");
        Check(parse(reloaded.TaskShow(tA, 0, e)).value("ok", false), "100-2a: purge keeps an open task");
        reloadPm.SetProcessFinishedCallback(nullptr);
        reloaded.ShutdownTaskQueueManager();
        reloaded.SetProcessManager(nullptr);
    }

    {
        // 100-2b: a BT job of a task carries task_id; events carry it; the task log takes BT entries;
        // external taskqueue_add ignores a task_id in the job JSON; a hidden job cannot join a task.
        const std::string ts2 = std::to_string(GetCurrentProcessId());
        TaskQueueManager bm;
        ProcessManager bPm;
        bm.SetProcessManager(&bPm);
        bPm.SetProcessFinishedCallback([&bm](int pid, int ec, const std::string& kind) {
            bm.OnProcessFinished(pid, ec, kind);
        });
        std::string bErr;
        const std::string taskB = "task_bt" + ts2;
        Check(JsonGetBool(bm.TaskCreate("bt task", "", taskB, "", bErr), "ok", false),
              "100-2b: task for a BT created: " + bErr);

        TaskQueueBtRef btRef;
        btRef.id = "bt-task-" + ts2;
        btRef.node = "n1";
        btRef.run = 1;
        const std::string btJobId = "BTT1-" + ts2;
        const std::string btQueue = "btq-task-" + ts2;
        const std::string btJob = "{\"id\":\"" + btJobId + "\",\"command\":\"cmd /c exit 0\",\"summary\":\"bt task job\","
                                  "\"timeoutSec\":30,\"task_id\":\"" + taskB + "\"}";
        Check(JsonGetBool(bm.AddBtJob(btQueue, btJob, btRef, bErr), "ok", false),
              "100-2b: BT job with task_id added: " + bErr);
        Check(bm.TaskQueueGet(btJobId, false, 0, bErr).find("\"task_id\":\"" + taskB + "\"") != std::string::npos,
              "100-2b: the BT job belongs to the task");
        Check(WaitForState(bm, btJobId, "succeeded", 8000), "100-2b: BT job of the task succeeds");
        const std::string btEvents = ReadFileUtf8Wide(TaskQueueManager::GetTaskEventsFileWide(btQueue));
        Check(btEvents.find("\"task_id\":\"" + taskB + "\"") != std::string::npos,
              "100-2b: job events carry task_id");

        // The BT entries of the task log: written by TaskLog for a task with a record only.
        bm.TaskLog(taskB, "bt_started", "", btRef.id, "running", "");
        bm.TaskLog(taskB, "bt_finished", "", btRef.id, "succeeded", "");
        bm.TaskLog("task_nosuch" + ts2, "bt_started", "", btRef.id, "running", "");
        const nlohmann::json shownB = nlohmann::json::parse(bm.TaskShow(taskB, 0, bErr), nullptr, false);
        std::string btKinds;
        for (const auto& entry : shownB["log"]) btKinds += entry.value("kind", std::string()) + " ";
        Check(btKinds.find("bt_started") != std::string::npos && btKinds.find("bt_finished") != std::string::npos,
              "100-2b: task log holds bt_started and bt_finished");
        Check(!JsonGetBool(bm.TaskShow("task_nosuch" + ts2, 0, bErr), "ok", true),
              "100-2b: TaskLog does not create a record");

        // External add: task_id in the JSON is ignored (only the task argument links a job).
        const std::string extJob = "[{\"id\":\"EXT-" + ts2 + "\",\"command\":\"cmd /c exit 0\",\"summary\":\"x\","
                                   "\"timeout_sec\":30,\"max_attempts\":1,\"task_id\":\"" + taskB + "\"}]";
        Check(JsonGetBool(bm.TaskQueueAdd("btq-ext-" + ts2, extJob, 1, "", bErr), "ok", false),
              "100-2b: external job with a task_id JSON field added");
        Check(bm.TaskQueueGet("EXT-" + ts2, false, 0, bErr).find(taskB) == std::string::npos,
              "100-2b: external taskqueue_add ignores task_id");

        // A hidden job cannot belong to a task.
        const std::string hiddenJob = "{\"id\":\"HID-" + ts2 + "\",\"command\":\"cmd /c exit 0\",\"summary\":\"h\","
                                      "\"timeoutSec\":30,\"hidden\":true,\"task_id\":\"" + taskB + "\"}";
        TaskQueueBtRef hiddenRef = btRef;
        hiddenRef.id = "bt-hidden-" + ts2;
        Check(!JsonGetBool(bm.AddBtJob("btq-hid-" + ts2, hiddenJob, hiddenRef, bErr), "ok", true),
              "100-2b: a hidden BT job with a task_id is rejected");

        bPm.SetProcessFinishedCallback(nullptr);
        bm.ShutdownTaskQueueManager();
        bm.SetProcessManager(nullptr);
    }

    {
        // 100-3: automatic task log entries for takeover, takeover_completed, cancel, and close.
        auto parse = [](const std::string& raw) {
            nlohmann::json j = nlohmann::json::parse(raw, nullptr, false);
            return j.is_object() ? j : nlohmann::json::object();
        };
        auto countKind = [](const nlohmann::json& shown, const std::string& kind) {
            int n = 0;
            for (const auto& entry : shown.value("log", nlohmann::json::array())) {
                if (entry.value("kind", std::string()) == kind) ++n;
            }
            return n;
        };
        const std::string ts3 = std::to_string(GetCurrentProcessId());
        TaskQueueManager lm;
        ProcessManager lPm;
        lm.SetProcessManager(&lPm);
        lPm.SetProcessFinishedCallback([&lm](int pid, int ec, const std::string& kind) {
            lm.OnProcessFinished(pid, ec, kind);
        });
        std::string lErr;
        const std::string taskL = "task_log1003_" + ts3;
        const std::string qL = "lq-1003-" + ts3;
        Check(parse(lm.TaskCreate("100-3 log test", "", taskL, "", lErr)).value("ok", false),
              "100-3: task created");

        const std::string jTake = "L1003-TAKE-" + ts3;
        const std::string jCancel = "L1003-CAN-" + ts3;
        const std::string jClose = "L1003-CLS-" + ts3;
        const std::string jobsJson = "["
            "{\"id\":\"" + jTake + "\",\"command\":\"cmd /c exit 1\",\"summary\":\"take\",\"timeout_sec\":30,\"max_attempts\":1},"
            "{\"id\":\"" + jCancel + "\",\"command\":\"cmd /c exit 0\",\"summary\":\"cancel\",\"timeout_sec\":30,\"max_attempts\":1,\"depends_on\":[\"" + jTake + "\"]},"
            "{\"id\":\"" + jClose + "\",\"command\":\"cmd /c exit 0\",\"summary\":\"close\",\"timeout_sec\":30,\"max_attempts\":1,\"depends_on\":[\"" + jTake + "\"]}"
            "]";
        Check(parse(lm.TaskQueueAdd(qL, jobsJson, 1, "", lErr, "", taskL)).value("ok", false),
              "100-3: jobs added to task");
        Check(WaitForState(lm, jTake, "failed", 8000), "100-3: takeover target job fails");

        const nlohmann::json beforeTake = parse(lm.TaskShow(taskL, 0, lErr));
        Check(countKind(beforeTake, "takeover") == 0 && countKind(beforeTake, "takeover_completed") == 0 &&
                  countKind(beforeTake, "job_cancelled") == 0,
              "100-3: no takeover/cancel log entries yet");

        Check(parse(lm.TaskQueueCancel(jCancel, lErr)).value("ok", false), "100-3: job cancel succeeds");
        const nlohmann::json afterCancel = parse(lm.TaskShow(taskL, 0, lErr));
        Check(countKind(afterCancel, "job_cancelled") == 1,
              "100-3: job cancel logs 1 job_cancelled entry");

        Check(parse(lm.TaskQueueClose(jClose, lErr)).value("ok", false), "100-3: job close succeeds");
        const nlohmann::json afterClose = parse(lm.TaskShow(taskL, 0, lErr));
        Check(countKind(afterClose, "job_cancelled") == 2,
              "100-3: job close logs 1 more job_cancelled entry");

        Check(parse(lm.TaskQueueTakeover(jTake, "alice", "manual fix", lErr)).value("ok", false),
              "100-3: takeover succeeds");
        const nlohmann::json afterTake = parse(lm.TaskShow(taskL, 0, lErr));
        Check(countKind(afterTake, "takeover") == 1 &&
                  afterTake["log"].back().value("text", std::string()).find("alice") != std::string::npos &&
                  afterTake["log"].back().value("text", std::string()).find("manual fix") != std::string::npos,
              "100-3: takeover logs 1 takeover entry with by and reason");

        Check(parse(lm.TaskQueueComplete(jTake, "done", "succeeded", "fixed by hand", lErr)).value("ok", false),
              "100-3: takeover complete succeeds");
        const nlohmann::json afterComplete = parse(lm.TaskShow(taskL, 0, lErr));
        Check(countKind(afterComplete, "takeover_completed") == 1 &&
                  afterComplete["log"].back().value("text", std::string()).find("succeeded") != std::string::npos &&
                  afterComplete["log"].back().value("text", std::string()).find("fixed by hand") != std::string::npos,
              "100-3: takeover complete logs 1 takeover_completed entry with result and reason");

        lPm.SetProcessFinishedCallback(nullptr);
        lm.ShutdownTaskQueueManager();
        lm.SetProcessManager(nullptr);
    }

    {
        // 100-4: dependency resolution across supersedes + task summary & task filter in TaskQueueList/Get.
        auto parse = [](const std::string& raw) {
            nlohmann::json j = nlohmann::json::parse(raw, nullptr, false);
            return j.is_object() ? j : nlohmann::json::object();
        };
        const std::string ts4 = std::to_string(GetCurrentProcessId());
        TaskQueueManager dm;
        ProcessManager dPm;
        dm.SetProcessManager(&dPm);
        dPm.SetProcessFinishedCallback([&dm](int pid, int ec, const std::string& kind) {
            dm.OnProcessFinished(pid, ec, kind);
        });
        std::string dErr;
        const std::string taskNamed = "task_1004_" + ts4;
        const std::string qD = "dq-1004-" + ts4;
        Check(parse(dm.TaskCreate("100-4 named task", "test list & deps", taskNamed, "", dErr)).value("ok", false),
              "100-4: named task created");

        const std::string jA = "D1004-A-" + ts4;
        const std::string jB = "D1004-B-" + ts4;
        const std::string jA2 = "D1004-A2-" + ts4;
        const std::string jImp = "D1004-IMP-" + ts4;

        const std::string abJson = "["
            "{\"id\":\"" + jA + "\",\"command\":\"cmd /c exit 1\",\"summary\":\"A fails\",\"timeout_sec\":30,\"max_attempts\":1},"
            "{\"id\":\"" + jB + "\",\"command\":\"cmd /c exit 0\",\"summary\":\"B waits on A\",\"timeout_sec\":30,\"max_attempts\":1,\"depends_on\":[\"" + jA + "\"]}"
            "]";
        Check(parse(dm.TaskQueueAdd(qD, abJson, 1, "", dErr, "", taskNamed)).value("ok", false),
              "100-4: A and B added to named task");
        Check(WaitForState(dm, jA, "failed", 8000), "100-4: A fails");
        Check(WaitForState(dm, jB, "blocked", 8000), "100-4: B becomes blocked after A fails");

        // Add A2 superseding A; B should unblock and succeed once A2 succeeds.
        const std::string a2Json = "["
            "{\"id\":\"" + jA2 + "\",\"command\":\"cmd /c exit 0\",\"summary\":\"A2 succeeds\",\"timeout_sec\":30,\"max_attempts\":1,"
            "\"supersedes\":\"" + jA + "\",\"supersede_reason\":\"fix A\"}"
            "]";
        Check(parse(dm.TaskQueueAdd(qD, a2Json, 1, "", dErr, "", taskNamed)).value("ok", false),
              "100-4: A2 superseding A added");
        Check(WaitForState(dm, jA2, "succeeded", 8000), "100-4: A2 succeeds");
        Check(WaitForState(dm, jB, "succeeded", 8000), "100-4: B unblocks and succeeds after A2 succeeds");

        // Add an implicit-task job (no task argument).
        const std::string impJson = "[{\"id\":\"" + jImp + "\",\"command\":\"cmd /c exit 0\",\"summary\":\"implicit\",\"timeout_sec\":30,\"max_attempts\":1}]";
        Check(parse(dm.TaskQueueAdd(qD, impJson, 1, "", dErr)).value("ok", false),
              "100-4: implicit job added");
        Check(WaitForState(dm, jImp, "succeeded", 8000), "100-4: implicit job succeeds");

        // TaskQueueGet includes task object: with title for named task, without title for implicit task.
        const nlohmann::json getB = parse(dm.TaskQueueGet(jB, false, 0, dErr));
        Check(getB.value("ok", false) && getB.contains("job") && getB["job"].contains("task") &&
                  getB["job"]["task"].value("id", std::string()) == taskNamed &&
                  getB["job"]["task"].value("title", std::string()) == "100-4 named task" &&
                  getB["job"]["task"].value("state", std::string()) == "succeeded",
              "100-4: TaskQueueGet named task job carries task{id,title,state}");

        const nlohmann::json getImp = parse(dm.TaskQueueGet(jImp, false, 0, dErr));
        Check(getImp.value("ok", false) && getImp.contains("job") && getImp["job"].contains("task") &&
                  getImp["job"]["task"].value("id", std::string()) == jImp &&
                  !getImp["job"]["task"].contains("title") &&
                  getImp["job"]["task"].value("state", std::string()) == "succeeded",
              "100-4: TaskQueueGet implicit task job carries task{id,state} without title");

        // TaskQueueList with task filter returns only that task's jobs, each carrying task{...}.
        const nlohmann::json listNamed = parse(dm.TaskQueueList("", "", 0, dErr, "", "", false, taskNamed));
        Check(listNamed.value("ok", false) && listNamed["jobs"].size() == 3,
              "100-4: TaskQueueList task filter returns 3 jobs for named task");
        bool allNamedOk = true;
        for (const auto& j : listNamed["jobs"]) {
            if (!j.contains("task") || j["task"].value("id", std::string()) != taskNamed ||
                j["task"].value("title", std::string()) != "100-4 named task" ||
                j["task"].value("state", std::string()) != "succeeded") {
                allNamedOk = false;
            }
        }
        Check(allNamedOk, "100-4: TaskQueueList named task jobs all carry matching task field");

        const nlohmann::json listImp = parse(dm.TaskQueueList("", "", 0, dErr, "", "", false, jImp));
        Check(listImp.value("ok", false) && listImp["jobs"].size() == 1 &&
                  listImp["jobs"][0].value("id", std::string()) == jImp &&
                  listImp["jobs"][0].contains("task") &&
                  !listImp["jobs"][0]["task"].contains("title"),
              "100-4: TaskQueueList task filter returns only the implicit task job without title");

        dPm.SetProcessFinishedCallback(nullptr);
        dm.ShutdownTaskQueueManager();
        dm.SetProcessManager(nullptr);
    }

    // ------------------------------------------------------------------
    // Card 100-5: Dashboard task-lineage view pure row builder & TaskList version
    // ------------------------------------------------------------------
    {
        // 1. Compact result formatting
        Check(FormatTaskResultsCompact({"failed", "failed", "succeeded"}) == "\xE2\x9C\x97\xE2\x9C\x97\xE2\x9C\x93",
              "100-5: FormatTaskResultsCompact short (✗✗✓)");
        std::vector<std::string> many(6, "failed");
        many.push_back("succeeded");
        many.push_back("failed");
        many.push_back("succeeded");
        Check(FormatTaskResultsCompact(many, 8) == "\xE2\x9C\x97\xC3\x97" "6 \xE2\x9C\x93 \xE2\x9C\x97 \xE2\x9C\x93",
              "100-5: FormatTaskResultsCompact run-length (✗×6 ✓ ✗ ✓)");

        // 2. BuildTaskDisplayRows: 2-level Task view, auto-expand/collapse, implicit single job, superseded dimming, MoreJobs
        std::vector<nlohmann::json> tasks = {
            {{"id", "task_run"}, {"title", "Running task"}, {"state", "running"}, {"updatedAt", "2026-11-23T10:05:00Z"}},
            {{"id", "task_done"}, {"title", "Done task"}, {"state", "succeeded"}, {"updatedAt", "2026-11-23T10:04:00Z"}}
        };
        std::vector<nlohmann::json> jobs = {
            {{"id", "j_run"}, {"task_id", "task_run"}, {"state", "running"}, {"created_at", "2026-11-23T10:05:00Z"}},
            {{"id", "j_done_2"}, {"task_id", "task_done"}, {"state", "succeeded"}, {"supersedes", "j_done_1"}, {"created_at", "2026-11-23T10:04:00Z"}},
            {{"id", "j_done_1"}, {"task_id", "task_done"}, {"state", "failed"}, {"superseded_by", "j_done_2"}, {"created_at", "2026-11-23T10:03:00Z"}},
            {{"id", "j_imp"}, {"state", "succeeded"}, {"created_at", "2026-11-23T10:02:00Z"}},
            {{"id", "group:bt_1"}, {"state", "running"}, {"created_at", "2026-11-23T10:01:00Z"}}
        };

        TaskViewCounts counts;
        std::unordered_map<std::string, bool> overrides;
        std::unordered_set<std::string> showAll;
        auto rows = BuildTaskDisplayRows(jobs, tasks, /*taskViewMode=*/true, overrides, showAll, counts, 8);
        Check(counts.totalTasks == 3 && counts.runningTasks == 1 && counts.succeededTasks == 2,
              "100-5: TaskViewCounts counts explicit + implicit tasks (excluding pure virtual BT)");
        Check(counts.totalJobs == 4 && counts.runningJobs == 1 && counts.succeededJobs == 2 && counts.failedJobs == 1,
              "100-5: TaskViewCounts counts real jobs");

        // Expected order: task_run (heading, expanded) -> j_run (indent 1) -> task_done (heading, collapsed) -> j_imp (implicit, no heading, indent 0) -> group:bt_1 (indent 0)
        Check(rows.size() == 5, "100-5: collapsed succeeded task hides children; implicit single job has no heading");
        Check(rows[0].kind == TaskDisplayRowKind::TaskHeading && rows[0].taskId == "task_run" && rows[0].expanded,
              "100-5: running task auto-expanded");
        Check(rows[1].kind == TaskDisplayRowKind::Job && rows[1].jobId == "j_run" && rows[1].indent == 1,
              "100-5: child job indented");
        Check(rows[2].kind == TaskDisplayRowKind::TaskHeading && rows[2].taskId == "task_done" && !rows[2].expanded &&
                  rows[2].resultsCompact == "\xE2\x9C\x97\xE2\x9C\x93" && rows[2].jobCount == 2,
              "100-5: succeeded task auto-collapsed with compact result ✗✓ and jobCount=2");
        Check(rows[3].kind == TaskDisplayRowKind::Job && rows[3].jobId == "j_imp" && rows[3].indent == 0,
              "100-5: implicit single job renders flat without heading");
        Check(rows[4].kind == TaskDisplayRowKind::Job && rows[4].jobId == "group:bt_1" && rows[4].indent == 0,
              "100-5: virtual BT row renders flat");

        // Expand task_done via override: superseded job is marked isSuperseded=true
        overrides["task_done"] = true;
        auto expandedRows = BuildTaskDisplayRows(jobs, tasks, true, overrides, showAll, counts, 8);
        bool foundSuperseded = false;
        for (const auto& r : expandedRows) {
            if (r.kind == TaskDisplayRowKind::Job && r.jobId == "j_done_1" && r.isSuperseded &&
                r.supersededBy == "j_done_2" && r.indent == 1) {
                foundSuperseded = true;
            }
        }
        Check(foundSuperseded, "100-5: expanded task marks superseded failure with isSuperseded & supersededBy");

        // 3. 103-3: 3-level hierarchy (task -> direction -> job), DirectionHeading for >=2 jobs or BT,
        //    no DirectionHeading for 1-job jobs direction, task resultsCompact per direction.
        {
            std::vector<nlohmann::json> dTasks = {
                {{"id", "t_3lvl"}, {"title", "3-level task"}, {"state", "running"}, {"updatedAt", "2026-11-24T10:10:00Z"}}
            };
            std::vector<nlohmann::json> dJobs = {
                // Single-job jobs direction -> no DirectionHeading, indent = 1
                {{"id", "j_single"}, {"task_id", "t_3lvl"}, {"direction_id", "dir_single"}, {"state", "succeeded"}, {"created_at", "2026-11-24T10:01:00Z"}},
                // Multi-job jobs direction -> DirectionHeading at indent 1, jobs at indent 2
                {{"id", "j_multi_1"}, {"task_id", "t_3lvl"}, {"direction_id", "dir_multi"}, {"state", "failed"}, {"summary", "Batch 1"}, {"created_at", "2026-11-24T10:02:00Z"}},
                {{"id", "j_multi_2"}, {"task_id", "t_3lvl"}, {"direction_id", "dir_multi"}, {"state", "succeeded"}, {"summary", "Batch 2"}, {"created_at", "2026-11-24T10:03:00Z"}},
                // BT direction -> DirectionHeading at indent 1, child job at indent 2
                {{"id", "direction:dir_bt"}, {"task_id", "t_3lvl"}, {"direction_id", "dir_bt"}, {"state", "running"}, {"summary", "My BT"}, {"virtual", true}, {"created_at", "2026-11-24T10:04:00Z"}},
                {{"id", "j_bt_1"}, {"task_id", "t_3lvl"}, {"direction_id", "dir_bt"}, {"state", "running"}, {"bt", {{"id", "bt_x"}}}, {"created_at", "2026-11-24T10:05:00Z"}},
                // Implicit taskqueue add (2 jobs, no task_id) -> DirectionHeading at indent 0, jobs at indent 1
                {{"id", "j_imp_1"}, {"direction_id", "dir_imp"}, {"state", "running"}, {"summary", "Imp 1"}, {"created_at", "2026-11-24T10:06:00Z"}},
                {{"id", "j_imp_2"}, {"direction_id", "dir_imp"}, {"state", "pending"}, {"summary", "Imp 2"}, {"created_at", "2026-11-24T10:07:00Z"}}
            };
            TaskViewCounts dCounts;
            std::unordered_map<std::string, bool> dOv;
            std::unordered_set<std::string> dAll;
            auto dRows = BuildTaskDisplayRows(dJobs, dTasks, true, dOv, dAll, dCounts, 8);
            bool sawTask = false, sawSingle = false, sawMultiDir = false, sawMultiJob = false, sawBtDir = false, sawBtJob = false, sawImpDir = false, sawImpJob = false;
            for (const auto& r : dRows) {
                if (r.kind == TaskDisplayRowKind::TaskHeading && r.taskId == "t_3lvl") {
                    // 3 directions: dir_single (✓), dir_multi (✗), dir_bt (▶) -> ✓✗▶
                    sawTask = (r.indent == 0 && r.resultsCompact == "\xE2\x9C\x93\xE2\x9C\x97\xE2\x96\xB6");
                } else if (r.kind == TaskDisplayRowKind::Job && r.jobId == "j_single") {
                    sawSingle = (r.indent == 1);
                } else if (r.kind == TaskDisplayRowKind::DirectionHeading && r.directionId == "dir_multi") {
                    sawMultiDir = (r.indent == 1 && r.resultsCompact == "\xE2\x9C\x97\xE2\x9C\x93" && r.jobCount == 2);
                } else if (r.kind == TaskDisplayRowKind::Job && r.jobId == "j_multi_1") {
                    sawMultiJob = (r.indent == 2);
                } else if (r.kind == TaskDisplayRowKind::DirectionHeading && r.directionId == "dir_bt") {
                    sawBtDir = (r.indent == 1 && r.directionKind == "bt");
                } else if (r.kind == TaskDisplayRowKind::Job && r.jobId == "j_bt_1") {
                    sawBtJob = (r.indent == 2);
                } else if (r.kind == TaskDisplayRowKind::DirectionHeading && r.directionId == "dir_imp") {
                    sawImpDir = (r.indent == 0 && r.jobCount == 2);
                } else if (r.kind == TaskDisplayRowKind::Job && r.jobId == "j_imp_1") {
                    sawImpJob = (r.indent == 1);
                }
            }
            Check(sawTask, "103-3: TaskHeading has indent 0 and per-direction resultsCompact");
            Check(sawSingle, "103-3: single-job jobs direction omits DirectionHeading (indent 1)");
            Check(sawMultiDir && sawMultiJob, "103-3: multi-job jobs direction has DirectionHeading (indent 1) and jobs (indent 2)");
            Check(sawBtDir && sawBtJob, "103-3: BT direction has DirectionHeading (indent 1) and jobs (indent 2)");
            Check(sawImpDir && sawImpJob, "103-3: implicit multi-job add has DirectionHeading (indent 0) and jobs (indent 1)");
        }

        // Older jobs summary ("MoreJobs") when exceeding maxVisibleJobsPerTask=1
        auto cappedRows = BuildTaskDisplayRows(jobs, tasks, true, overrides, showAll, counts, 1);
        bool foundMore = false;
        for (const auto& r : cappedRows) {
            if (r.kind == TaskDisplayRowKind::MoreJobs && r.taskId == "task_done" && r.hiddenJobCount == 1 && r.indent == 1) {
                foundMore = true;
            }
        }
        Check(foundMore, "100-5: MoreJobs row summarizes older hidden jobs");

        // Job view (flat)
        auto flatRows = BuildTaskDisplayRows(jobs, tasks, /*taskViewMode=*/false, overrides, showAll, counts, 8);
        Check(flatRows.size() == jobs.size() && flatRows[0].indent == 0 && flatRows[0].taskTitle == "Running task",
              "100-5: Job view is flat with taskTitle populated");

        // 3. TaskList version & since_version
        auto parse = [](const std::string& raw) {
            nlohmann::json j = nlohmann::json::parse(raw, nullptr, false);
            return j.is_object() ? j : nlohmann::json::object();
        };
        std::error_code vEc;
        std::filesystem::create_directories(dir + L"\\v1005", vEc);
        SetDataDirectoryOverride(dir + L"\\v1005");
        ProcessManager vPm;
        TaskQueueManager vTm;
        vTm.SetProcessManager(&vPm);
        std::string vErr;
        const nlohmann::json cRes = parse(vTm.TaskCreate("v test", "intent", "", "", vErr));
        Check(cRes.value("ok", false), "100-5: TaskCreate for version check");
        const nlohmann::json l1 = parse(vTm.TaskList("", 0, vErr));
        Check(l1.value("ok", false) && l1.contains("version") && l1["version"].is_number_integer(),
              "100-5: TaskList returns version");
        const int64_t ver1 = l1.value("version", int64_t(-1));
        const nlohmann::json l2 = parse(vTm.TaskList("", 0, vErr, ver1));
        Check(l2.value("ok", false) && l2.value("unchanged", false) && l2.value("version", int64_t(-1)) == ver1,
              "100-5: TaskList with matching since_version returns unchanged:true");
        vTm.TaskNote(cRes.value("id", std::string()), "bump version", "", vErr);
        const nlohmann::json l3 = parse(vTm.TaskList("", 0, vErr, ver1));
        Check(l3.value("ok", false) && !l3.value("unchanged", false) && l3.value("version", int64_t(-1)) > ver1,
              "100-5: TaskList with stale since_version returns updated tasks and higher version");
        vTm.ShutdownTaskQueueManager();
        vTm.SetProcessManager(nullptr);
    }

    // Card 100-7: TaskQueueSupersede (cloning, id numbering, BT rejection, task inheritance).
    {
        auto parse = [](const std::string& raw) {
            nlohmann::json j = nlohmann::json::parse(raw, nullptr, false);
            return j.is_object() ? j : nlohmann::json::object();
        };
        TaskQueueManager supTm;
        ProcessManager supPm;
        supTm.SetProcessManager(&supPm);
        supPm.SetProcessFinishedCallback([&supTm](int pid, int ec, const std::string& kind) {
            supTm.OnProcessFinished(pid, ec, kind);
        });

        std::string sErr;
        const nlohmann::json tc = parse(supTm.TaskCreate("100-7 supersede task", "test supersede", "", "", sErr));
        Check(tc.value("ok", false), "100-7: TaskCreate succeeds");
        const std::string tId = tc.value("id", std::string());

        const std::string jobsJson = R"([{
            "id": "sup-orig",
            "command": "cmd /c exit 1",
            "summary": "original summary",
            "timeoutSec": 45,
            "maxAttempts": 1,
            "priority": 3,
            "env": {"FOO": "bar"}
        }])";
        Check(parse(supTm.TaskQueueAdd("q100-7", jobsJson, 1, "", sErr, "", tId)).value("ok", false),
              "100-7: TaskQueueAdd orig succeeds");
        Check(WaitForState(supTm, "sup-orig", "failed", 8000), "100-7: sup-orig fails");

        // Reject missing reason or >2000 byte reason.
        Check(!parse(supTm.TaskQueueSupersede("sup-orig", "", "", "{}", "", sErr)).value("ok", true),
              "100-7: empty reason rejected");
        Check(!parse(supTm.TaskQueueSupersede("sup-orig", std::string(2001, 'x'), "", "{}", "", sErr)).value("ok", true),
              "100-7: >2000 byte reason rejected");

        // Supersede with command & timeout override; default id -> sup-orig-s1.
        const nlohmann::json sup1 = parse(supTm.TaskQueueSupersede(
            "sup-orig", "fix command and timeout", "",
            R"({"command":"cmd /c exit 0","timeoutSec":60})",
            R"({"session":"sess-sup-1"})", sErr));
        Check(sup1.value("ok", false), "100-7: TaskQueueSupersede succeeds: " + sErr);
        Check(sup1.value("id", std::string()) == "sup-orig-s1", "100-7: default id is sup-orig-s1");
        Check(sup1.value("supersedes", std::string()) == "sup-orig", "100-7: supersedes points to sup-orig");
        Check(sup1.value("taskqueue", std::string()) == "q100-7", "100-7: same queue preserved");
        Check(sup1.value("task_id", std::string()) == tId, "100-7: task_id inherited");
        const nlohmann::json j1 = sup1.value("job", nlohmann::json::object());
        Check(j1.value("summary", std::string()) == "original summary", "100-7: summary cloned");
        Check(j1.value("priority", 0) == 3, "100-7: priority cloned");
        Check(j1.value("max_attempts", 0) == 1, "100-7: max_attempts cloned");
        Check(j1.contains("env") && j1["env"].value("FOO", std::string()) == "bar", "100-7: env cloned");
        Check(j1.value("timeout_sec", 0) == 60, "100-7: timeoutSec overridden");
        Check(j1.value("command", std::string()) == "cmd /c exit 0", "100-7: command overridden");
        Check(j1.value("supersede_reason", std::string()) == "fix command and timeout",
              "100-7: supersede_reason recorded");
        Check(WaitForState(supTm, "sup-orig-s1", "succeeded", 8000), "100-7: sup-orig-s1 succeeds");

        // Superseding a succeeded job is rejected.
        Check(!parse(supTm.TaskQueueSupersede("sup-orig-s1", "again", "", "{}", "", sErr)).value("ok", true),
              "100-7: cannot supersede succeeded job");

        // ID collision numbering: sup-num -> sup-num-s1 (fails) -> sup-num-s2 (superseding sup-num again).
        const std::string numJobs = R"([{"id":"sup-num","command":"cmd /c exit 1","summary":"num","timeoutSec":30,"maxAttempts":1}])";
        Check(parse(supTm.TaskQueueAdd("q100-7", numJobs, 1, "", sErr)).value("ok", false),
              "100-7: implicit task job added");
        Check(WaitForState(supTm, "sup-num", "failed", 8000), "100-7: sup-num fails");
        const nlohmann::json n1 = parse(supTm.TaskQueueSupersede("sup-num", "try 2", "", "{}", "", sErr));
        Check(n1.value("ok", false) && n1.value("id", std::string()) == "sup-num-s1" &&
                  n1.value("task_id", std::string()) == "sup-num",
              "100-7: first implicit supersede gets -s1 and inherits implicit task_id");
        Check(WaitForState(supTm, "sup-num-s1", "failed", 8000), "100-7: sup-num-s1 fails");
        const nlohmann::json n2 = parse(supTm.TaskQueueSupersede("sup-num", "try 3", "", R"({"command":"cmd /c exit 0"})", "", sErr));
        Check(n2.value("ok", false) && n2.value("id", std::string()) == "sup-num-s2" &&
                  n2.value("supersedes", std::string()) == "sup-num-s1",
              "100-7: second supersede skips -s1 collision to -s2 and walks to head sup-num-s1");
        Check(WaitForState(supTm, "sup-num-s2", "succeeded", 8000), "100-7: sup-num-s2 succeeds");

        // Explicit newId starting with task_ is rejected.
        Check(!parse(supTm.TaskQueueSupersede("sup-num-s2", "bad", "task_bad", "{}", "", sErr)).value("ok", true),
              "100-7: task_ prefix for newId rejected");

        // BT-managed job is rejected.
        TaskQueueBtRef btRef;
        btRef.id = "bt-100-7";
        btRef.node = "leaf";
        btRef.run = 1;
        Check(parse(supTm.AddBtJob("q100-7",
                                   R"({"id":"sup-bt-job","command":"cmd /c exit 1","summary":"bt job","timeoutSec":30})",
                                   btRef, sErr)).value("ok", false),
              "100-7: AddBtJob succeeds");
        Check(WaitForState(supTm, "sup-bt-job", "failed", 8000), "100-7: sup-bt-job fails");
        const nlohmann::json btSup = parse(supTm.TaskQueueSupersede("sup-bt-job", "try", "", "{}", "", sErr));
        Check(!btSup.value("ok", true) && sErr.find("behavior tree") != std::string::npos,
              "100-7: BT-managed job rejected");

        supPm.SetProcessFinishedCallback(nullptr);
        supTm.ShutdownTaskQueueManager();
        supTm.SetProcessManager(nullptr);
    }

    // Card 100-8: repeatedFailures count in TaskShow and TaskQueueGet.
    {
        ProcessManager rfPm;
        TaskQueueManager rfTm;
        rfTm.SetProcessManager(&rfPm);
        rfPm.SetProcessFinishedCallback([&rfTm](int pid, int exitCode, const std::string& endKind) {
            rfTm.OnProcessFinished(pid, exitCode, endKind);
        });
        std::string rErr;
        auto parse = [](const std::string& s) { return nlohmann::json::parse(s, nullptr, false); };

        const nlohmann::json created = parse(rfTm.TaskCreate("100-8 repeat", "test repeatedFailures", "", "{}", rErr));
        const std::string taskId = created.value("id", std::string());
        Check(!taskId.empty(), "100-8: task created");
        Check(parse(rfTm.TaskShow(taskId, 0, rErr)).value("repeatedFailures", -1) == 0,
              "100-8: TaskShow repeatedFailures is 0 initially");

        Check(parse(rfTm.TaskQueueAdd("q100-8",
                                      R"([{"id":"rf-1","command":"cmd /c exit 1","summary":"rf1","timeoutSec":30,"maxAttempts":1}])",
                                      1, "", rErr, "", taskId)).value("ok", false),
              "100-8: rf-1 added");
        Check(WaitForState(rfTm, "rf-1", "failed", 8000), "100-8: rf-1 fails");
        Check(parse(rfTm.TaskShow(taskId, 0, rErr)).value("repeatedFailures", -1) == 1 &&
                  parse(rfTm.TaskQueueGet("rf-1", false, 0, rErr))["job"].value("repeatedFailures", -1) == 1,
              "100-8: repeatedFailures is 1 after 1st failure");

        Check(parse(rfTm.TaskQueueAdd("q100-8",
                                      R"([{"id":"rf-2","command":"cmd /c exit 1","summary":"rf2","timeoutSec":30,"maxAttempts":1,"supersedes":"rf-1"}])",
                                      1, "", rErr, "", taskId)).value("ok", false),
              "100-8: rf-2 added");
        Check(WaitForState(rfTm, "rf-2", "failed", 8000), "100-8: rf-2 fails");
        Check(parse(rfTm.TaskShow(taskId, 0, rErr)).value("repeatedFailures", -1) == 2 &&
                  parse(rfTm.TaskQueueGet("rf-2", false, 0, rErr))["job"].value("repeatedFailures", -1) == 2,
              "100-8: repeatedFailures is 2 after 2nd failure");

        Check(parse(rfTm.TaskQueueAdd("q100-8",
                                      R"([{"id":"rf-3","command":"cmd /c exit 1","summary":"rf3","timeoutSec":30,"maxAttempts":1,"supersedes":"rf-2"}])",
                                      1, "", rErr, "", taskId)).value("ok", false),
              "100-8: rf-3 added");
        Check(WaitForState(rfTm, "rf-3", "failed", 8000), "100-8: rf-3 fails");
        Check(parse(rfTm.TaskShow(taskId, 0, rErr)).value("repeatedFailures", -1) == 3 &&
                  parse(rfTm.TaskQueueGet("rf-3", false, 0, rErr))["job"].value("repeatedFailures", -1) == 3,
              "100-8: repeatedFailures is 3 after 3rd failure");

        rfPm.SetProcessFinishedCallback(nullptr);
        rfTm.ShutdownTaskQueueManager();
        rfTm.SetProcessManager(nullptr);
    }

    // 103-1: direction records (core) and job direction_id.
    {
        auto parse = [](const std::string& s) {
            return nlohmann::json::parse(s, nullptr, false);
        };
        const std::wstring dirDataDir = PathJoinWide(dir, L"dir_103_1");
        std::filesystem::remove_all(dirDataDir);
        std::filesystem::create_directories(dirDataDir);
        SetDataDirectoryOverride(dirDataDir);

        {
            ProcessManager dirPm;
            TaskQueueManager dirTm;
            dirTm.SetProcessManager(&dirPm);
            dirPm.SetProcessFinishedCallback([&](int pid, int code, const std::string& endKind) {
                dirTm.OnProcessFinished(pid, code, endKind);
            });
            std::string dErr;

            // 1) Multi-job add without --task: 1 direction, shared direction_id and shared implicit task_id.
            const nlohmann::json add1 = parse(dirTm.TaskQueueAdd(
                "q103-1",
                R"([{"id":"d1-a","command":"cmd /c exit 1","summary":"first job","timeoutSec":30,"maxAttempts":1},)"
                R"({"id":"d1-b","command":"cmd /c echo ok","summary":"second job","timeoutSec":30,"maxAttempts":1}])",
                1, "", dErr));
            Check(add1.value("ok", false), "103-1: multi-job add succeeds");
            const std::string dir1 = add1.value("direction_id", std::string());
            Check(dir1 == "dir_1", "103-1: first direction gets dir_1");
            const nlohmann::json jA = parse(dirTm.TaskQueueGet("d1-a", false, 0, dErr))["job"];
            const nlohmann::json jB = parse(dirTm.TaskQueueGet("d1-b", false, 0, dErr))["job"];
            Check(jA.value("direction_id", std::string()) == dir1 &&
                      jB.value("direction_id", std::string()) == dir1,
                  "103-1: both jobs share direction_id");
            Check(jA.value("task_id", std::string()) == "d1-a" &&
                      jB.value("task_id", std::string()) == "d1-a",
                  "103-1: multi-job add without --task shares implicit task_id");
            nlohmann::json dirRec1;
            Check(dirTm.DirectionGet(dir1, dirRec1) &&
                      dirRec1.value("kind", std::string()) == "jobs" &&
                      dirRec1.value("title", std::string()) == "first job" &&
                      dirRec1.value("taskId", std::string()).empty(),
                  "103-1: direction record stores kind/title/empty taskId");

            Check(WaitForState(dirTm, "d1-a", "failed", 8000), "103-1: d1-a fails");
            Check(WaitForState(dirTm, "d1-b", "succeeded", 8000), "103-1: d1-b succeeds");

            // 2) Retry does not create a new direction.
            const size_t dirCountBeforeRetry = dirTm.DirectionAll().size();
            const nlohmann::json ret = parse(dirTm.TaskQueueRetry("d1-a", true, false, dErr));
            Check(ret.value("ok", false), "103-1: retry d1-a succeeds");
            Check(dirTm.DirectionAll().size() == dirCountBeforeRetry,
                  "103-1: retry does not create a new direction");
            Check(WaitForState(dirTm, "d1-a", "failed", 8000), "103-1: retried d1-a fails");

            // 3) Supersede keeps the new job in the same direction.
            const nlohmann::json sup = parse(dirTm.TaskQueueSupersede(
                "d1-a", "fix cmd", "d1-a2", R"({"command":"cmd /c echo fixed"})", "", dErr));
            Check(sup.value("ok", false) && sup.value("direction_id", std::string()) == dir1,
                  "103-1: supersede keeps original direction_id");
            Check(WaitForState(dirTm, "d1-a2", "succeeded", 8000), "103-1: d1-a2 succeeds");

            // 4) Purge moves job id and final state into direction's removedJobs.
            const nlohmann::json pur = parse(dirTm.TaskQueuePurge("q103-1", R"(["succeeded","failed"])", dErr));
            Check(pur.value("ok", false) && pur.value("purged", 0) == 3,
                  "103-1: purged all 3 finished jobs");
            Check(dirTm.DirectionGet(dir1, dirRec1) &&
                      dirRec1.contains("removedJobs") &&
                      dirRec1["removedJobs"].size() == 3,
                  "103-1: direction removedJobs records purged jobs");

            // 5) Add with --task sets taskId on direction and writes direction_started to task log.
            const nlohmann::json tRes = parse(dirTm.TaskCreate("103-1 task", "intent", "", "{}", dErr));
            const std::string tId = tRes.value("id", std::string());
            const nlohmann::json add2 = parse(dirTm.TaskQueueAdd(
                "q103-1",
                R"([{"id":"d2-a","command":"cmd /c echo t","summary":"tasked job","timeoutSec":30}])",
                1, "", dErr, "", tId));
            const std::string dir2 = add2.value("direction_id", std::string());
            Check(add2.value("ok", false) && dir2 == "dir_2", "103-1: second direction gets dir_2");
            nlohmann::json dirRec2;
            Check(dirTm.DirectionGet(dir2, dirRec2) && dirRec2.value("taskId", std::string()) == tId,
                  "103-1: direction records taskId");
            const nlohmann::json tShow = parse(dirTm.TaskShow(tId, 20, dErr));
            bool foundDirStarted = false;
            if (tShow.contains("log") && tShow["log"].is_array()) {
                for (const auto& entry : tShow["log"]) {
                    if (entry.value("kind", std::string()) == "direction_started" &&
                        entry.value("directionId", std::string()) == dir2 &&
                        entry.value("title", std::string()) == "tasked job") {
                        foundDirStarted = true;
                        break;
                    }
                }
            }
            Check(foundDirStarted, "103-1: task log records direction_started");
            WaitForState(dirTm, "d2-a", "succeeded", 8000);

            // 6) Legacy pre-0.10 queue file without direction_id can be read cleanly.
            WriteFileAtomicWide(
                TaskQueueManager::GetTaskQueueFileWide("legacy-q"),
                "{\r\n  \"name\": \"legacy-q\",\r\n  \"concurrency\": 1,\r\n  \"jobs\": [\r\n"
                "    {\"id\":\"leg-1\",\"summary\":\"legacy\",\"command\":\"cmd /c echo ok\","
                "\"state\":\"succeeded\",\"timeout_sec\":30,\"max_attempts\":1}\r\n  ]\r\n}\r\n");

            dirPm.SetProcessFinishedCallback(nullptr);
            dirTm.ShutdownTaskQueueManager();
            dirTm.SetProcessManager(nullptr);
        }

        // Reload from disk: direction records and legacy queue load cleanly.
        {
            TaskQueueManager reloadTm;
            std::string rErr;
            nlohmann::json dirRec1;
            Check(reloadTm.DirectionGet("dir_1", dirRec1) && dirRec1["removedJobs"].size() == 3,
                  "103-1: direction record persists across restart");
            const nlohmann::json leg = parse(reloadTm.TaskQueueGet("leg-1", false, 0, rErr));
            Check(leg.value("ok", false) && !leg["job"].contains("direction_id"),
                  "103-1: pre-0.10 job without direction_id loads cleanly");
            reloadTm.ShutdownTaskQueueManager();
        }
        SetDataDirectoryOverride(dir);
    }

    // 103-2: direction state, close/reopen, direction-unit task state, purge.
    {
        auto parse = [](const std::string& s) {
            return nlohmann::json::parse(s, nullptr, false);
        };
        const std::wstring dirDataDir = PathJoinWide(dir, L"dir_103_2");
        std::filesystem::remove_all(dirDataDir);
        std::filesystem::create_directories(dirDataDir);
        SetDataDirectoryOverride(dirDataDir);

        {
            ProcessManager dPm;
            TaskQueueManager dTm;
            dTm.SetProcessManager(&dPm);
            dPm.SetProcessFinishedCallback([&](int pid, int code, const std::string& endKind) {
                dTm.OnProcessFinished(pid, code, endKind);
            });
            std::string err;

            const nlohmann::json tc = parse(dTm.TaskCreate("103-2 task", "test directions", "", "{}", err));
            const std::string taskId = tc.value("id", std::string());
            Check(tc.value("ok", false), "103-2: task created");

            // 1) Active direction cannot be closed; once finished it can be closed and reopened.
            const std::wstring slowCmd = PathJoinWide(dirDataDir, L"slow_fail.cmd");
            WriteFileAtomicWide(slowCmd, "@echo off\r\nping -n 2 127.0.0.1 >nul\r\nexit /b 1\r\n");
            const nlohmann::json addSlow = parse(dTm.TaskQueueAdd(
                "q103-2",
                nlohmann::json::array({{
                    {"id", "slow-1"},
                    {"command", "cmd /c \"" + WideToUtf8(slowCmd) + "\""},
                    {"summary", "slow fail"},
                    {"timeoutSec", 30},
                    {"maxAttempts", 1},
                }}).dump(),
                1, "", err, "", taskId));
            const std::string dirSlow = addSlow.value("direction_id", std::string());
            Check(addSlow.value("ok", false) && !dirSlow.empty(), "103-2: slow direction added");
            Check(dTm.DirectionState(dirSlow).value("state", std::string()) == "running",
                  "103-2: active direction state is running");
            Check(!parse(dTm.DirectionClose(dirSlow, "ignored", "too early", err)).value("ok", true),
                  "103-2: cannot close direction with active job");

            Check(WaitForState(dTm, "slow-1", "failed", 8000), "103-2: slow-1 fails");
            Check(dTm.DirectionState(dirSlow).value("state", std::string()) == "failed",
                  "103-2: failed direction state is failed");

            // 2) Add a second succeeding direction to the same task.
            const nlohmann::json addOk = parse(dTm.TaskQueueAdd(
                "q103-2",
                R"([{"id":"ok-1","command":"cmd /c echo ok","summary":"ok job","timeoutSec":30,"maxAttempts":1}])",
                1, "", err, "", taskId));
            const std::string dirOk = addOk.value("direction_id", std::string());
            Check(WaitForState(dTm, "ok-1", "succeeded", 8000), "103-2: ok-1 succeeds");
            Check(dTm.DirectionState(dirOk).value("state", std::string()) == "succeeded",
                  "103-2: succeeded direction state is succeeded");

            // Task state is failed while dirSlow is failed.
            Check(dTm.TaskState(taskId).value("state", std::string()) == "failed",
                  "103-2: task state is failed when one direction failed");

            // 3) Close dirSlow -> task state becomes succeeded with closedDirections=1.
            const nlohmann::json cl = parse(dTm.DirectionClose(dirSlow, "ignored", "not needed", err));
            Check(cl.value("ok", false) && cl.value("state", std::string()) == "closed" &&
                      cl.value("result", std::string()) == "ignored",
                  "103-2: direction close succeeds with ignored");
            Check(dTm.DirectionState(dirSlow).value("state", std::string()) == "closed",
                  "103-2: closed direction state is closed");
            const nlohmann::json tsClosed = dTm.TaskState(taskId);
            Check(tsClosed.value("state", std::string()) == "succeeded" &&
                      tsClosed.value("closedDirections", 0) == 1,
                  "103-2: closed direction is excluded from task state and counted in closedDirections");

            // TaskShow lists directions and task log has direction_finished & direction_closed.
            const nlohmann::json showTask = parse(dTm.TaskShow(taskId, 50, err));
            Check(showTask.contains("directions") && showTask["directions"].is_array() &&
                      showTask["directions"].size() == 2,
                  "103-2: TaskShow includes directions list");
            bool hasFinishedLog = false, hasClosedLog = false;
            for (const auto& entry : showTask["log"]) {
                if (entry.value("kind", std::string()) == "direction_finished") hasFinishedLog = true;
                if (entry.value("kind", std::string()) == "direction_closed") hasClosedLog = true;
            }
            Check(hasFinishedLog && hasClosedLog,
                  "103-2: task log records direction_finished and direction_closed");

            // 4) Reopen dirSlow -> task state returns to failed and direction_reopened is logged.
            const nlohmann::json ro = parse(dTm.DirectionReopen(dirSlow, "recheck", err));
            Check(ro.value("ok", false) && ro.value("state", std::string()) == "failed",
                  "103-2: direction reopen restores failed state");
            Check(dTm.TaskState(taskId).value("state", std::string()) == "failed",
                  "103-2: task state returns to failed after reopen");
            const nlohmann::json showAfterReopen = parse(dTm.TaskShow(taskId, 50, err));
            bool hasReopenedLog = false;
            for (const auto& entry : showAfterReopen["log"]) {
                if (entry.value("kind", std::string()) == "direction_reopened") hasReopenedLog = true;
            }
            Check(hasReopenedLog, "103-2: task log records direction_reopened");

            // 5) Cancelled job -> direction state is cancelled.
            const nlohmann::json addCan = parse(dTm.TaskQueueAdd(
                "q103-2",
                R"([{"id":"can-1","command":"cmd /c echo hi","summary":"cancel me","timeoutSec":30,"paused":true}])",
                1, "", err));
            const std::string dirCan = addCan.value("direction_id", std::string());
            Check(parse(dTm.TaskQueueCancel("can-1", err)).value("ok", false), "103-2: can-1 cancelled");
            Check(dTm.DirectionState(dirCan).value("state", std::string()) == "cancelled",
                  "103-2: cancelled direction state is cancelled");

            // 6) DirectionShow & DirectionList filters.
            const nlohmann::json dShow = parse(dTm.DirectionShow(dirSlow, err));
            Check(dShow.value("ok", false) && dShow["jobs"].size() == 1 &&
                      dShow["status"].value("state", std::string()) == "failed",
                  "103-2: DirectionShow returns record, status, and jobs");
            const nlohmann::json dListTask = parse(dTm.DirectionList(taskId, "failed", 10, err));
            Check(dListTask.value("ok", false) && dListTask["directions"].size() == 1 &&
                      dListTask["directions"][0].value("id", std::string()) == dirSlow,
                  "103-2: DirectionList filters by taskId and state");

            // 7) Implicit direction job:<id> for pre-0.10 job without direction_id.
            WriteFileAtomicWide(
                TaskQueueManager::GetTaskQueueFileWide("legacy-103-2"),
                "{\r\n  \"name\": \"legacy-103-2\",\r\n  \"concurrency\": 1,\r\n  \"jobs\": [\r\n"
                "    {\"id\":\"leg-2\",\"summary\":\"legacy 2\",\"command\":\"cmd /c echo ok\","
                "\"state\":\"succeeded\",\"timeout_sec\":30,\"max_attempts\":1}\r\n  ]\r\n}\r\n");

            // 8) DirectionPurgeOlderThan removes closed direction and completed implicit-task direction.
            Check(parse(dTm.DirectionClose(dirSlow, "failed", "done", err)).value("ok", false),
                  "103-2: close dirSlow for purge");
            Sleep(2100);
            const nlohmann::json pur = parse(dTm.DirectionPurgeOlderThan(1, err));
            Check(pur.value("ok", false) && pur.value("purged", 0) >= 2,
                  "103-2: DirectionPurgeOlderThan removes closed and completed implicit-task directions");
            Check(!parse(dTm.DirectionShow(dirSlow, err)).value("ok", true),
                  "103-2: purged closed direction is gone");
            Check(parse(dTm.DirectionShow(dirOk, err)).value("ok", false),
                  "103-2: open task's unclosed direction is kept by purge");

            dPm.SetProcessFinishedCallback(nullptr);
            dTm.ShutdownTaskQueueManager();
            dTm.SetProcessManager(nullptr);
        }

        // Reload to test implicit job:<id> DirectionState & DirectionShow.
        {
            TaskQueueManager reloadTm;
            std::string rErr;
            Check(reloadTm.DirectionState("job:leg-2").value("state", std::string()) == "succeeded",
                  "103-2: implicit direction job:<id> computes state");
            const nlohmann::json impShow = parse(reloadTm.DirectionShow("job:leg-2", rErr));
            Check(impShow.value("ok", false) && impShow["direction"].value("implicit", false),
                  "103-2: implicit direction job:<id> works with DirectionShow");
            reloadTm.ShutdownTaskQueueManager();
        }
        SetDataDirectoryOverride(dir);
    }

    std::printf("%s (%d failures)\n", g_failures == 0 ? "ALL PASSED" : "FAILURES", g_failures);
    return g_failures == 0 ? 0 : 1;
}
