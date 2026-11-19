// Regression tests for the behavior tree's failure boundaries: retry (attempt budget, delay,
// reset on Retry), escalation answers (expect list, takeover), LLM proxy fallback (consult
// candidates, engine health), and state save/restore while a node is waiting on an LLM.

#include "behavior_tree_smoke_common.h"

#include <set>

namespace bt_test {
int g_failures = 0;
int g_passes = 0;
} // namespace bt_test

using namespace bt_test;
using namespace pc;

namespace {

// Engine health double: a set of engines marked down and a fixed consult candidate list.
class FakeHealth : public IBtEngineHealth {
public:
    std::set<std::string> down;  // "engine/model"
    std::vector<std::pair<std::string, std::string>> consult;

    bool IsDown(const std::string& engine, const std::string& model) const override {
        return down.count(engine + "/" + model) > 0;
    }
    void MarkDown(const std::string& engine, const std::string& model, const std::string&,
                  const std::string&, int64_t) override {
        down.insert(engine + "/" + model);
    }
    std::vector<std::pair<std::string, std::string>> Candidates(const std::string& list) const override {
        return list == "consult" ? consult : std::vector<std::pair<std::string, std::string>>{};
    }
};

// prep job -> escalate(ask) with a proxy fallback. Used by the LLM-boundary tests.
std::string EscalateDef(const std::string& id, const std::string& extra = "") {
    return R"({"id":")" + id + R"(","root":{"type":"sequence","children":[)"
        R"({"type":"job","id":"prep","summary":"prep","command":"prep","timeoutSec":30},)"
        R"({"type":"escalate","id":"ask","question":"Ship?","expect":["approve","reject"],)"
        R"("timeoutSec":500,"fallback":{"afterSec":10}}]}})" + extra;
}

} // namespace

int main() {
    SetConsoleOutputCP(CP_UTF8);
    FakeEngineCatalog catalog;

    // R1. Retry-with-reset after exhaustion must not reuse a job id that already finished.
    {
        const std::string def = R"({"id":"rg-reuse","root":{"type":"retry","id":"r","max":2,)"
            R"("child":{"type":"job","id":"flaky","summary":"f","command":"f","timeoutSec":60}}})";
        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(def, &catalog, err), "R1 load: " + err);
        bt.SetJobRunner(&runner);
        bt.Start(1000);
        bt.OnJobFinished("rg-reuse-flaky-1", false, 1, "", "", 1100);
        bt.OnJobFinished("rg-reuse-flaky-2", false, 1, "", "", 1200);
        Check(bt.State() == "failed" && runner.startedJobs.size() == 2, "R1 exhausted after 2 attempts");

        Check(bt.Retry("", true, 5000, err), "R1 retry with reset: " + err);
        Check(runner.startedJobs.size() == 3, "R1 retry launched a fresh attempt");
        const std::string newId = runner.startedJobs.back().proposedJobId;
        Check(newId != "rg-reuse-flaky-1" && newId != "rg-reuse-flaky-2",
              "R1 fresh attempt has a job id distinct from finished attempts: " + newId);
    }

    // R2. A retry delay holds the next attempt until its deadline, and NextWakeMs reports it.
    {
        const std::string def = R"({"id":"rg-delay","root":{"type":"retry","id":"r","max":3,"delaySec":5,)"
            R"("child":{"type":"job","id":"flaky","summary":"f","command":"f","timeoutSec":60}}})";
        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(def, &catalog, err), "R2 load: " + err);
        bt.SetJobRunner(&runner);
        bt.Start(1000);
        bt.OnJobFinished("rg-delay-flaky-1", false, 1, "", "", 1100);
        Check(runner.startedJobs.size() == 1, "R2 attempt 2 is not launched during the delay");
        Check(bt.NextWakeMs() == 6100, "R2 NextWakeMs is the retry deadline (6100)");
        bt.OnTimer(3000);
        Check(runner.startedJobs.size() == 1, "R2 OnTimer before the deadline does not launch");
        bt.OnTimer(6100);
        Check(runner.startedJobs.size() == 2 && bt.NodeRunCount("r") == 2,
              "R2 attempt 2 launches at the deadline");
    }

    // R3. Retry from a failed attempt with resetCounters=false must not add attempts beyond max.
    {
        const std::string def = R"({"id":"rg-budget","root":{"type":"retry","id":"r","max":2,)"
            R"("child":{"type":"job","id":"flaky","summary":"f","command":"f","timeoutSec":60}}})";
        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(def, &catalog, err), "R3 load: " + err);
        bt.SetJobRunner(&runner);
        bt.Start(1000);
        bt.OnJobFinished("rg-budget-flaky-1", false, 1, "", "", 1100);
        bt.OnJobFinished("rg-budget-flaky-2", false, 1, "", "", 1200);
        Check(bt.State() == "failed", "R3 exhausted before retry");
        Check(!bt.Retry("", false, 5000, err) && !err.empty() && bt.State() == "failed",
              "R3 retry without reset is refused under a spent budget: " + err);
        Check(runner.startedJobs.size() == 2,
              "R3 refused retry launches no attempt past max (launched " +
              std::to_string(runner.startedJobs.size()) + ")");
        Check(bt.Retry("", true, 5000, err) && runner.startedJobs.size() == 3,
              "R3 resetCounters=true still starts a fresh attempt: " + err);
    }

    // R4. "takeover" is always a legal answer, even when expect does not list it.
    {
        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(EscalateDef("rg-takeover"), &catalog, err), "R4 load: " + err);
        bt.SetJobRunner(&runner);
        bt.Start(1000);
        bt.OnJobFinished("rg-takeover-prep-1", true, 0, "", "ok", 1100);
        Check(bt.State() == "waitingForLlm", "R4 waits at escalate");
        Check(bt.Resume("ask", "takeover", "operator handles it", {}, 2000, err),
              "R4 takeover accepted despite expect list: " + err);
        Check(bt.GetVar("ask.answer") == "takeover", "R4 takeover recorded as the answer");
        Check(bt.NodeState("ask") != "running", "R4 escalate is no longer waiting after takeover");
    }

    // R5. An answer outside expect is rejected and the escalation keeps waiting.
    {
        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(EscalateDef("rg-badans"), &catalog, err), "R5 load: " + err);
        bt.SetJobRunner(&runner);
        bt.Start(1000);
        bt.OnJobFinished("rg-badans-prep-1", true, 0, "", "ok", 1100);
        Check(!bt.Resume("ask", "maybe", "unsure", {}, 2000, err) && !err.empty(),
              "R5 unlisted answer rejected: " + err);
        Check(bt.State() == "waitingForLlm" && bt.NodeState("ask") == "running" &&
              bt.GetVar("ask.answer").empty(),
              "R5 rejected answer leaves the gate waiting with no answer set");
    }

    // R6. A consult candidate that is marked down is skipped; the first healthy one is used.
    {
        FakeHealth health;
        health.consult = {{"claude", "opus"}, {"codex", "gpt"}};
        health.down.insert("claude/opus");
        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(EscalateDef("rg-skip"), &catalog, err), "R6 load: " + err);
        bt.SetJobRunner(&runner);
        bt.SetEngineHealth(&health);
        bt.Start(1000);
        bt.OnJobFinished("rg-skip-prep-1", true, 0, "", "ok", 1100);
        bt.OnTimer(11100);
        Check(runner.startedWorkers.size() == 1 && runner.startedWorkers[0].engine == "codex",
              "R6 fallback skips the down engine and uses codex");
    }

    // R7. A failed consult candidate advances to the next one; when none remain, the gate fails.
    {
        FakeHealth health;
        health.consult = {{"claude", "opus"}, {"codex", "gpt"}};
        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(EscalateDef("rg-chain"), &catalog, err), "R7 load: " + err);
        bt.SetJobRunner(&runner);
        bt.SetEngineHealth(&health);
        bt.Start(1000);
        bt.OnJobFinished("rg-chain-prep-1", true, 0, "", "ok", 1100);
        bt.OnTimer(11100);
        Check(runner.startedWorkers.size() == 1 && runner.startedWorkers[0].engine == "claude",
              "R7 first candidate launched");
        bt.OnJobFinished(runner.startedWorkers[0].proposedJobId, false, 1, "", "boom", 11200);
        bt.OnTimer(12200);
        Check(runner.startedWorkers.size() == 2 && runner.startedWorkers[1].engine == "codex",
              "R7 failed candidate advances to codex");
        bt.OnJobFinished(runner.startedWorkers[1].proposedJobId, false, 1, "", "boom", 12300);
        Check(bt.State() == "failed" && bt.NodeState("ask") == "failed",
              "R7 last candidate failing fails the escalation");
    }

    // R8. When every consult candidate is down, the authored/default engine answers instead of
    // the gate launching a down proxy.
    {
        FakeHealth health;
        health.consult = {{"claude", "opus"}};
        health.down.insert("claude/opus");
        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(EscalateDef("rg-alldown"), &catalog, err), "R8 load: " + err);
        bt.SetJobRunner(&runner);
        bt.SetEngineHealth(&health);
        bt.Start(1000);
        bt.OnJobFinished("rg-alldown-prep-1", true, 0, "", "ok", 1100);
        bt.OnTimer(11100);
        Check(runner.startedWorkers.size() == 1 && runner.startedWorkers[0].engine == "agy" &&
              bt.State() == "waitingForLlm",
              "R8 all consult candidates down: default engine answers, no down proxy launched");
    }

    // R9. Retrying an exhausted escalation must start its proxy chain from the first candidate.
    {
        FakeHealth health;
        health.consult = {{"claude", "opus"}, {"codex", "gpt"}};
        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(EscalateDef("rg-reesc"), &catalog, err), "R9 load: " + err);
        bt.SetJobRunner(&runner);
        bt.SetEngineHealth(&health);
        bt.Start(1000);
        bt.OnJobFinished("rg-reesc-prep-1", true, 0, "", "ok", 1100);
        bt.OnTimer(11100);
        bt.OnJobFinished(runner.startedWorkers[0].proposedJobId, false, 1, "", "boom", 11200);
        bt.OnTimer(12200);
        bt.OnJobFinished(runner.startedWorkers[1].proposedJobId, false, 1, "", "boom", 12300);
        Check(bt.State() == "failed", "R9 chain exhausted");

        Check(bt.Retry("", true, 20000, err), "R9 retry: " + err);
        bt.OnTimer(30000);
        const size_t before = runner.startedWorkers.size();
        Check(before >= 3, "R9 proxy launched after retry");
        Check(runner.startedWorkers.back().engine == "claude",
              "R9 retried escalation starts from the first candidate (got '" +
              runner.startedWorkers.back().engine + "')");
    }

    // R10. Save and restore while waiting on the LLM: the gate still accepts an answer and
    // cancels its proxy worker.
    {
        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(EscalateDef("rg-persist"), &catalog, err), "R10 load: " + err);
        bt.SetJobRunner(&runner);
        bt.Start(1000);
        bt.OnJobFinished("rg-persist-prep-1", true, 0, "", "ok", 1100);
        const std::string saved = bt.ToStateJson();

        FakeJobRunner runner2;
        BehaviorTree restored;
        Check(restored.LoadFromStateJson(saved, &catalog, 1500, err), "R10 restore: " + err);
        restored.SetJobRunner(&runner2);
        Check(restored.State() == "waitingForLlm" && restored.NodeState("ask") == "running",
              "R10 restored tree is still waiting on the escalate gate");
        Check(restored.Resume("ask", "approve", "checks green", {}, 2000, err),
              "R10 answer accepted after restore: " + err);
        Check(restored.GetVar("ask.answer") == "approve", "R10 answer variable set after restore");
    }

    // R11. Retry is refused while the tree waits for an LLM answer.
    {
        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(EscalateDef("rg-retrywait"), &catalog, err), "R11 load: " + err);
        bt.SetJobRunner(&runner);
        bt.Start(1000);
        bt.OnJobFinished("rg-retrywait-prep-1", true, 0, "", "ok", 1100);
        Check(!bt.Retry("", false, 2000, err) && !err.empty() && bt.State() == "waitingForLlm",
              "R11 retry refused while waitingForLlm: " + err);
    }

    std::printf("%s (%d passed, %d failures)\n", g_failures ? "FAILED" : "ALL PASSED", g_passes, g_failures);
    return g_failures ? 1 : 0;
}
