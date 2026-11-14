#include "../src/triage.h"
#include "../src/triage_text.h"
#include "../src/common.h"

#include <windows.h>

#include <cstdio>
#include <fstream>
#include <string>

using namespace pc;
using json = nlohmann::json;

static int g_failures = 0;

static void Check(bool condition, const char* label) {
    std::printf("%s %s\n", condition ? "[PASS]" : "[FAIL]", label);
    std::fflush(stdout);
    if (!condition) ++g_failures;
}

static json TimeoutJob(int sinceSec, int attempts = 1) {
    json history = json::array();
    for (int i = 1; i <= attempts; ++i) {
        history.push_back({{"n", i}, {"reason", "timed_out"},
                           {"start", "2026-01-01 10:00:00"}, {"end", "2026-01-01 10:01:00"},
                           {"failure", {{"class", "timeout"}, {"confidence", "high"},
                                        {"summary", "Timed out"},
                                        {"lastActivity", {{"activity", "output"},
                                                           {"target", "worker"},
                                                           {"sinceSec", sinceSec}}}}}});
    }
    const json failure = {{"class", "timeout"}, {"confidence", "high"},
                          {"summary", "Timed out"}, {"suggestedAction", "retry"},
                          {"lastActivity", {{"activity", "output"}, {"target", "worker"},
                                             {"sinceSec", sinceSec}}}};
    return {{"id", "job-timeout"}, {"name", "Build"}, {"summary", "Build project"},
            {"taskqueue", "main"}, {"state", "failed"}, {"timeout_sec", 60},
            {"estimate_sec", 45}, {"max_attempts", 3}, {"attempts", history},
            {"failure", failure}, {"idle", {{"output_sec", 3}, {"cpu_sec", 2}}},
            {"success", json::object()}, {"resources", {{"write", json::array({"out"})},
                                                          {"read", json::array({"src"})}}},
            {"cwd", "C:/work"}, {"host", ""}};
}

static json FindCard(const json& cards, const std::string& key) {
    for (const auto& card : cards)
        if (card.value("key", std::string()) == key) return card;
    return json::object();
}

static json FindInfoItem(const json& card, const char* section, const char* key) {
    const json info = card.value("info", json::object());
    const json items = info.value(section, json::array());
    for (const auto& item : items)
        if (item.is_object() && item.value("k", std::string()) == key) return item;
    return json::object();
}

static bool StructuredInfo(const json& card) {
    const json info = card.value("info", json::object());
    for (const char* section : {"cause", "background", "environment", "conditions", "reason"}) {
        const json items = info.value(section, json::array());
        if (!items.is_array()) return false;
        for (const auto& item : items)
            if (!item.is_object() || !item.contains("k") || !item.contains("v") ||
                !item.contains("text") || !item["text"].is_string()) return false;
    }
    return true;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    const std::string now = "2026-11-14 12:00:00";

    // 1. Progressing timeout recommends a longer timeout and exposes the expected actions.
    {
        TriageInputs in;
        in.jobs = json::array({TimeoutJob(3)});
        in.now = now;
        const json cards = BuildTriageCandidates(in);
        const json card = FindCard(cards, "job:job-timeout#1");
        bool hasExpected = true;
        for (const char* expected : {"retry", "retry_raise_timeout", "cancel", "ignore", "investigate"}) {
            bool found = false;
            for (const auto& candidate : card.value("candidates", json::array()))
                found = found || candidate.value("id", std::string()) == expected;
            hasExpected = hasExpected && found;
        }
        const json failureClass = FindInfoItem(card, "cause", "failureClass");
        Check(TriageTimeoutPattern(TimeoutJob(3)) == "progressing" &&
              card.value("recommendation", json::object()).value("candidateId", std::string()) == "retry_raise_timeout" &&
              card.value("recommendation", json::object()).value("confidence", std::string()) == "high" &&
              card["recommendation"].value("rationaleKey", std::string()) == "timeoutProgressing" &&
              failureClass.value("v", json::object()).value("class", std::string()) == "timeout" &&
              failureClass.contains("text") && hasExpected,
              "progressing timeout recommendation and structured information");
    }

    // Structured job details include command, bounded log context, overlapping attempts, and attempt history.
    {
        wchar_t tempDir[MAX_PATH] = {};
        wchar_t logPath[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tempDir);
        GetTempFileNameW(tempDir, L"tri", 0, logPath);
        FILE* log = nullptr;
        _wfopen_s(&log, logPath, L"wb");
        if (log) {
            for (int i = 1; i <= 10; ++i) std::fprintf(log, "line %d\n", i);
            std::fclose(log);
        }
        json job = TimeoutJob(3);
        job["command"] = "build.exe --release";
        job["idle"] = {{"output_sec", 0}, {"cpu_sec", 0}};
        job["success"] = {{"output_regex", ""}, {"files_newer", json::array()}, {"test_report", ""}};
        job["failure"]["evidence"] = json::array({{{"log", WideToUtf8(logPath)}, {"line", 5}}});
        job["attempts"].back()["log_file"] = WideToUtf8(logPath);
        json other = TimeoutJob(3);
        other["id"] = "job-overlap";
        other["attempts"].back()["start"] = "2026-01-01 10:00:30";
        other["attempts"].back()["end"] = "2026-01-01 10:00:45";
        TriageInputs in;
        in.jobs = json::array({job, other});
        in.now = now;
        const json card = BuildTriageCandidates(in).front();
        const json evidence = FindInfoItem(card, "cause", "evidence");
        const json concurrency = FindInfoItem(card, "environment", "concurrencyAtFailure");
        const json history = FindInfoItem(card, "background", "attemptHistory");
        const bool evidenceOk = evidence.value("v", json::object()).value("line", 0) == 5 &&
            evidence.value("v", json::object()).value("lines", json::array()).size() == 7 &&
            evidence.value("v", json::object()).value("lines", json::array())[3] == "line 5";
        const bool unsetOmitted = FindInfoItem(card, "conditions", "successOutputRegex").empty() &&
            FindInfoItem(card, "conditions", "successFilesNewer").empty() &&
            FindInfoItem(card, "conditions", "idle").empty();
        Check(card["info"]["background"].is_array() &&
              FindInfoItem(card, "background", "command").value("v", json()) == "build.exe --release" &&
              evidenceOk && concurrency.value("v", json::object()).value("count", 0) == 1 &&
              concurrency.value("v", json::object()).value("ids", json::array())[0] == "job-overlap" &&
              history.value("v", json::array()).size() == 1 &&
              history.value("v", json::array())[0].value("class", std::string()) == "timeout" &&
              unsetOmitted, "command, evidence context, omitted defaults, concurrency and attempt history");
        DeleteFileW(logPath);
    }

    // Older cards stored plain info strings and remain eligible for uncertain LLM advice.
    {
        const json legacy = {{"kind", "job_failure"},
            {"recommendation", {{"confidence", "medium"}}},
            {"flags", {{"timeoutPattern", "repeated"}}},
            {"info", {{"cause", json::array({"failure class: timeout (high confidence)"})},
                      {"background", json::array({"previous attempt classes: timeout"})},
                      {"reason", json::array({"attempt reason: timed_out"})}}}};
        Check(TriageNeedsLlm(legacy, "semi", "uncertain"), "legacy string-only card still triggers LLM advice");
    }

    // 2. A timeout with no recent activity recommends investigation.
    {
        TriageInputs in;
        in.jobs = json::array({TimeoutJob(50)});
        in.now = now;
        const json card = BuildTriageCandidates(in).front();
        Check(TriageTimeoutPattern(TimeoutJob(50)) == "stalled" &&
              card["recommendation"]["candidateId"] == "investigate" &&
              card["recommendation"]["confidence"] == "medium",
              "stalled timeout recommends investigation");
    }

    // 3. Consecutive timeout attempts are treated as a repeated pattern.
    {
        TriageInputs in;
        in.jobs = json::array({TimeoutJob(50, 2)});
        in.now = now;
        const json card = BuildTriageCandidates(in).front();
        Check(TriageTimeoutPattern(TimeoutJob(50, 2)) == "repeated" &&
              card["recommendation"]["candidateId"] == "" &&
              card["recommendation"]["confidence"] == "low",
              "repeated timeout has no recommendation");
    }

    // 4. Human-required auth failures are never auto-executed.
    {
        json job = TimeoutJob(3);
        job["failure"] = {{"class", "auth_error"}, {"confidence", "high"},
                           {"suggestedAction", "human"}, {"summary", "Authentication required"}};
        TriageInputs in;
        in.jobs = json::array({job});
        in.now = now;
        const json card = BuildTriageCandidates(in).front();
        bool noChoice = true;
        for (const std::string mode : {"manual", "semi", "auto"}) {
            std::string by, why;
            noChoice = noChoice && TriageAutoChoice(json::object(), card, mode, by, why).empty();
        }
        Check(card["flags"]["needsHuman"] == true && noChoice,
              "auth error requires a human in every mode");
    }

    // 5. Internal, virtual, and BT-absorbed failed rows are excluded.
    {
        json internal = TimeoutJob(3); internal["id"] = "internal"; internal["taskqueue"] = "_triage";
        json planned = TimeoutJob(3); planned["id"] = "planned"; planned["virtual"] = true;
        json absorbed = TimeoutJob(3); absorbed["id"] = "absorbed";
        absorbed["bt"] = {{"id", "tree"}, {"node", "job-node"}};
        TriageInputs in;
        in.jobs = json::array({internal, planned, absorbed});
        in.btStates = {{"tree", "running"}};
        in.now = now;
        Check(BuildTriageCandidates(in).empty(), "excluded job rows produce no cards");
    }

    // 6. Merge adds pending cards, resolves vanished pending cards, and preserves acted cards.
    {
        TriageInputs in;
        in.jobs = json::array({TimeoutJob(3)});
        in.now = now;
        const json fresh = BuildTriageCandidates(in);
        json store = {{"schema", "hancho.triage/1"}, {"cards", json::array()},
                      {"autoRetries", json::array()}};
        const auto added = MergeTriageCards(store, fresh, now);
        const bool initiallyPending = added.size() == 1 && store["cards"][0]["state"] == "pending";
        MergeTriageCards(store, json::array(), "2026-11-14 12:01:00");
        const bool resolved = store["cards"][0]["state"] == "resolved";
        json acted = fresh.front();
        acted["key"] = "job:acted#1";
        acted["id"] = TriageCardIdFromKey("job:acted#1");
        acted["state"] = "acted";
        store["cards"].push_back(acted);
        MergeTriageCards(store, json::array(), "2026-11-14 12:02:00");
        Check(initiallyPending && resolved && store["cards"].back()["state"] == "acted",
              "merge pending, natural resolution, and acted preservation");
    }

    // Per-kind settings suppress new cards, resolve pending cards with a reason, and preserve decisions.
    {
        json waiting = { {"id", "waiting"}, {"state", "waiting"}, {"taskqueue", "main"},
                         {"name", "Waiting"}, {"resource_wait", "holder: @lock"} };
        TriageInputs in;
        in.jobs = json::array({TimeoutJob(3), waiting});
        in.now = now;
        json fresh = BuildTriageCandidates(in);
        const json jobCard = FindCard(fresh, "job:job-timeout#1");
        json acted = jobCard;
        acted["id"] = "acted-card";
        acted["key"] = "job:acted#1";
        acted["state"] = "acted";
        json store = {{"cards", json::array({jobCard, acted})}};
        ApplyTriageKindSettings(store, fresh, {{"triage_kinds", {{"job_failure", false}}}}, now);
        const bool disabledResolved = store["cards"][0].value("state", std::string()) == "resolved" &&
            store["cards"][0].value("resolvedReason", std::string()) == "kind_disabled";
        const bool actedPreserved = store["cards"][1].value("state", std::string()) == "acted";
        const bool freshDropped = fresh.empty(); // resource_wait defaults off; job_failure is explicitly off.
        fresh = BuildTriageCandidates(in);
        ApplyTriageKindSettings(store, fresh,
            {{"triage_kinds", {{"job_failure", true}, {"resource_wait", true}}}}, now);
        MergeTriageCards(store, fresh, now);
        Check(disabledResolved && actedPreserved && freshDropped &&
              store["cards"][0].value("state", std::string()) == "pending",
              "triage kind settings suppress, resolve, preserve, and re-enable cards");
    }

    // 7. Semi mode uses a high safe Hancho recommendation; manual and repeat retry guards block.
    {
        TriageInputs in;
        in.jobs = json::array({TimeoutJob(3)});
        in.now = now;
        const json card = BuildTriageCandidates(in).front();
        std::string by, why;
        const std::string semi = TriageAutoChoice(json::object(), card, "semi", by, why);
        const bool semiOk = semi == "retry_raise_timeout" && by == "hancho-auto";
        const bool manualBlocked = TriageAutoChoice(json::object(), card, "manual", by, why).empty();
        json store = {{"autoRetries", json::array({"job-timeout|timeout"})}};
        const bool retryBlocked = TriageAutoChoice(store, card, "semi", by, why).empty();
        Check(semiOk && manualBlocked && retryBlocked, "auto-choice mode, safety, and one-retry guards");
    }

    // 8. LLM advice must name a candidate on this card.
    {
        TriageInputs in;
        in.jobs = json::array({TimeoutJob(3)});
        in.now = now;
        const json card = BuildTriageCandidates(in).front();
        json advice;
        std::string err;
        const bool unknownRejected = !ParseTriageAdvice(
            R"({"candidateId":"not-real","confidence":"high","rationale":"No."})", card, advice, err);
        const bool validAccepted = ParseTriageAdvice(
            R"({"candidateId":"retry_raise_timeout","confidence":"medium","rationale":"The task was progressing.","caution":"Check the timeout cap."})",
            card, advice, err) && advice.value("candidateId", std::string()) == "retry_raise_timeout";
        Check(unknownRejected && validAccepted, "LLM advice candidate validation");
    }

    // 9. Timing and resource cards use stable keys and the expected kinds.
    {
        json timing = {{"id", "timed-job"}, {"state", "succeeded"}, {"taskqueue", "main"},
                       {"name", "Build"}, {"summary", "Compile"}, {"timing_feedback", {
                           {"accuracy", "diverged_over"}, {"duration_sec", 120},
                           {"estimate_sec", 40}, {"ratio", 3.0}, {"advice", "Increase estimate."}}}};
        json waiting = {{"id", "waiting-job"}, {"state", "waiting"}, {"taskqueue", "main"},
                        {"name", "Test"}, {"resource_wait", "holder: @compiler"}};
        TriageInputs in;
        in.jobs = json::array({timing, waiting});
        in.btFailures = json::array({{{"btId", "tree-1"}, {"nodeId", "ask"},
            {"detail", "answer timed out"}, {"treeName", "Review"}, {"treeState", "failed"},
            {"queue", "main"}, {"question", "Proceed?"}, {"options", json::array({"yes", "no"})},
            {"jobs", json::array({"related-job"})}, {"timeoutSec", 90}}});
        in.now = now;
        const json cards = BuildTriageCandidates(in);
        const json timingCard = FindCard(cards, "timing:timed-job");
        const json resourceCard = FindCard(cards, "res:waiting-job=holder: @compiler");
        const json escalationCard = FindCard(cards, "esc:tree-1/ask");
        Check(!timingCard.empty() && timingCard["kind"] == "timing_divergence" &&
              !resourceCard.empty() && resourceCard["kind"] == "resource_wait" &&
              !escalationCard.empty() && escalationCard["kind"] == "bt_escalation_failure" &&
              StructuredInfo(timingCard) && StructuredInfo(resourceCard) && StructuredInfo(escalationCard) &&
              FindInfoItem(escalationCard, "background", "question").value("v", json()) == "Proceed?" &&
              FindInfoItem(escalationCard, "background", "relatedJobs").value("v", json::array()).size() == 1,
              "timing, resource and BT cards use structured information");
    }

    // 10. Plain process failures become cards, while task-linked and triage LLM processes are excluded.
    {
        const json processes = json::array({
            {{"id", 9}, {"name", "worker"}, {"command", "worker.exe --run"}, {"cwd", "C:/work"},
             {"mode", "pipe"}, {"state", "timed_out"}, {"exit_code", 1460}, {"start_time", "2026-11-14 11:00:00"},
             {"elapsed_ms", 30000}, {"timeout_sec", 30}, {"idle_timeout_sec", 0}, {"job_key", ""},
             {"job_id", 0}, {"attempt", 0}},
            {{"id", 10}, {"name", "task-attempt"}, {"state", "failed"}, {"start_time", "2026-11-14 11:00:00"},
             {"job_key", "job-a"}},
            {{"id", 11}, {"name", "triage-llm-card"}, {"state", "failed"}, {"start_time", "2026-11-14 11:00:00"}},
            // Stopped by a person: not a card.
            {{"id", 12}, {"name", "supervisor"}, {"state", "killed"}, {"start_time", "2026-11-14 11:00:00"}}
        });
        const json cards = BuildProcessTriageCandidates(processes);
        Check(cards.size() == 1 && cards[0]["kind"] == "process_failure" &&
              cards[0]["key"] == "proc:9@2026-11-14 11:00:00" &&
              cards[0]["candidates"][0]["id"] == "restart" &&
              cards[0]["candidates"][0]["safe"] == false,
              "process failure card and exclusions");
        Check(StructuredInfo(cards[0]) &&
              FindInfoItem(cards[0], "background", "command").value("v", json()) == "worker.exe --run",
              "process card uses keyed information");
    }

    // 11. Batch supervisor hand-off: the instructions name the card and the exact decide command, and a
    //     refresh with changed information keeps the "with supervisor" mark on the pending card.
    {
        TriageInputs in;
        in.jobs = json::array({TimeoutJob(3)});
        in.now = now;
        const json fresh = BuildTriageCandidates(in);
        const std::string cli = "\"C:/h/hancho-cli.exe\" --data-dir \"C:/d\"";
        const std::string text = BuildTriageHandoffInstructions(fresh, cli, "ja");
        const std::string cardId = fresh[0]["id"].get<std::string>();
        const bool instructionsOk = text.find(cli + " triage decide <id> <candidateId>") != std::string::npos &&
                                    text.find(cardId) != std::string::npos &&
                                    text.find("non-interactively") != std::string::npos;
        json store = {{"schema", "hancho.triage/1"}, {"cards", json::array()}, {"autoRetries", json::array()}};
        MergeTriageCards(store, fresh, now);
        store["cards"][0]["handoff"] = {{"jobId", "supervisor-triage-1"}, {"state", "running"}};
        json changed = fresh;
        changed[0]["info"]["cause"].push_back("extra line");
        MergeTriageCards(store, changed, "2026-11-14 12:05:00");
        Check(instructionsOk && store["cards"][0]["state"] == "pending" &&
              store["cards"][0]["handoff"]["state"] == "running",
              "supervisor hand-off instructions and mark survive refresh");
    }

    // 12. Lifecycle outcomes distinguish a retry success, another failure, and an acted card with no retry.
    {
        const json original = TimeoutJob(3);
        TriageInputs in;
        in.jobs = json::array({original});
        in.now = now;
        json card = BuildTriageCandidates(in).front();
        card["state"] = "acted";
        json retried = original;
        retried["attempts"].push_back({{"n", 2}, {"reason", "completed"}});
        retried["state"] = "succeeded";
        const json success = TriageOutcome(card, json::array({retried}), json::object());

        json failedAgain = original;
        failedAgain["attempts"].push_back({{"n", 2}, {"reason", "failed"}});
        failedAgain["state"] = "failed";
        const json again = TriageOutcome(card, json::array({failedAgain}), json::object());
        const json notRerun = TriageOutcome(card, json::array({original}), json::object());
        Check(success.value("state", std::string()) == "succeeded" && success.value("complete", false) &&
              again.value("state", std::string()) == "failed_again" && again.value("nextKey", std::string()) == "job:job-timeout#2" &&
              notRerun.value("state", std::string()) == "not_rerun",
              "retry success, failed-again link, and not-rerun outcomes");
    }

    // 13. Branch activity prevents completion and failed branch jobs expose their own card key.
    {
        const json original = TimeoutJob(3);
        TriageInputs in;
        in.jobs = json::array({original});
        in.now = now;
        json card = BuildTriageCandidates(in).front();
        card["state"] = "acted";
        json retried = original;
        retried["attempts"].push_back({{"n", 2}, {"reason", "completed"}});
        retried["state"] = "succeeded";
        json blocked = {{"id", "dependent"}, {"state", "blocked"}, {"depends_on", json::array({"job-timeout"})}};
        const json pendingBranch = TriageOutcome(card, json::array({retried, blocked}), json::object());
        json failed = blocked;
        failed["state"] = "failed";
        failed["attempts"] = json::array({{{"n", 1}}});
        const json failedBranch = TriageOutcome(card, json::array({retried, failed}), json::object());
        const auto& branchJobs = failedBranch["branch"]["jobs"];
        Check(pendingBranch.value("state", std::string()) == "succeeded" && !pendingBranch.value("complete", true) &&
              branchJobs.size() == 1 && branchJobs[0].value("key", std::string()) == "job:dependent#1",
              "blocked branch delays completion and failed dependents carry a key");
    }

    // 14. BT escalation outcomes follow the tree, while a manual card remains untracked until a run starts.
    {
        json btCard = {{"id", "bt-card"}, {"key", "esc:tree/node"}, {"kind", "bt_escalation_failure"},
                       {"state", "acted"}, {"target", {{"btId", "tree"}, {"nodeId", "node"}}}};
        const json running = TriageOutcome(btCard, json::array(), json{{"tree", "running"}});
        const json succeeded = TriageOutcome(btCard, json::array(), json{{"tree", "succeeded"}});
        TriageInputs in;
        in.jobs = json::array({TimeoutJob(3)});
        in.now = now;
        json manualCard = BuildTriageCandidates(in).front();
        manualCard["manual"] = {{"state", "working"}};
        const json manualOutcome = TriageOutcome(manualCard, in.jobs, json::object());
        Check(running.value("state", std::string()) == "rerunning" &&
              succeeded.value("state", std::string()) == "succeeded" &&
              manualCard["state"] == "pending" && manualOutcome.is_null(),
              "BT state tracking and untouched manual outcome");
    }

    // 15. The 200-card cap evicts the oldest closed cards but never drops open cards.
    {
        json cards = json::array();
        for (int i = 0; i < 201; ++i) {
            const std::string key = "job:retained-" + std::to_string(i) + "#1";
            cards.push_back({{"id", TriageCardIdFromKey(key)}, {"key", key}, {"kind", "job_failure"},
                             {"state", "pending"}, {"lifecycle", "pending"},
                             {"createdAt", "2026-11-14 10:" + std::to_string(i / 60) + ":" + std::to_string(i % 60)}});
        }
        json store = {{"schema", "hancho.triage/1"}, {"cards", cards}, {"autoRetries", json::array()}};
        MergeTriageCards(store, cards, now);
        const bool openCardsRetained = store["cards"].size() == 201;

        json closed = store["cards"];
        closed.push_back({{"id", "closed-oldest"}, {"key", "job:closed#1"}, {"kind", "job_failure"},
                          {"state", "acted"}, {"lifecycle", "done"},
                          {"createdAt", "2026-11-13 00:00:00"}});
        store["cards"] = closed;
        MergeTriageCards(store, closed, now);
        bool keptEveryOpen = store["cards"].size() == 201;
        bool keptClosed = false;
        for (const auto& card : store["cards"])
            keptClosed = keptClosed || card.value("id", std::string()) == "closed-oldest";
        Check(openCardsRetained && keptEveryOpen && !keptClosed,
              "trim evicts oldest closed cards and retains open cards");
    }

    // 16. The shared display catalog localizes keyed values, preserves legacy text, and
    //     keeps evidence and log output in independent rows.
    {
        const json failureClass = {{"k", "failureClass"},
            {"v", {{"class", "timeout"}, {"confidence", "high"}}},
            {"text", "failure class: timeout (high confidence)"}};
        const json evidence = {{"k", "evidence"},
            {"v", {{"file", "C:/logs/job.log"}, {"line", 30}, {"lines", json::array({"before", "matching line", "after"})}}},
            {"text", "legacy evidence sentence"}};
        const auto evidenceRows = TriageFormatInfoRows("ja", evidence);
        Check(TriageFormatInfo("ja", failureClass).find("タイムアウト") != std::string::npos &&
              TriageFormatInfo("ja", failureClass).find("高") != std::string::npos &&
              evidenceRows.size() == 4 && evidenceRows[0].find("C:/logs/job.log:30") != std::string::npos &&
              evidenceRows[1] == "before" && TriageFormatInfo("ja", json("legacy plain text")) == "legacy plain text" &&
              TriageFormatInfo("ja", json{{"k", "futureKey"}, {"v", 1}, {"text", "fallback sentence"}}) == "fallback sentence" &&
              TriageFormatDuration("ja", 90) == "1 分 30 秒" &&
              TriageCandidateLabel("ja", "retry") != "Retry unchanged" &&
              TriageFormatRationale("ja", "timeoutProgressing", json::object()).find("再試行") != std::string::npos,
              "shared triage catalog formatting and fallback behavior");
    }

    return g_failures == 0 ? 0 : 1;
}
