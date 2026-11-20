#include "../framework/test_registry.h"
#include "../behavior_tree_smoke_common.h"

#include <set>
#include <string>

using namespace bt_test;
using namespace pc;

namespace {

class BoundaryHealth : public IBtEngineHealth {
public:
    std::set<std::string> down;
    int64_t nextProbeAt = 0;

    bool IsDown(const std::string& engine, const std::string& model) const override {
        return down.count(engine + "/" + model) != 0;
    }
    void MarkDown(const std::string& engine, const std::string& model, const std::string&,
                  const std::string&, int64_t) override {
        down.insert(engine + "/" + model);
    }
    void MarkHealthy(const std::string& engine, const std::string& model,
                     const std::string&, int64_t) override {
        down.erase(engine + "/" + model);
    }
    int64_t NextProbeAtMs(const std::string&, const std::string&) const override {
        return nextProbeAt;
    }
    bool IsDownWorthy(const std::string& cls) const override {
        return cls == "startup_hang" || cls == "probe_failed";
    }
};

const char* const kDiagnostics = R"JSON({"class":"startup_hang","summary":"No events from worker.",)JSON"
    R"JSON("suggestedAction":"retry_with_change",)JSON"
    R"JSON("likelyCauses":[{"id":"no_model"}],)JSON"
    R"JSON("nextSteps":[{"text":"do not take over"}],)JSON"
    R"JSON("supervisorNote":"ask the operator"})JSON";

} // namespace

HANCHO_TEST("bt.worker.max_attempts", [](htest::TestContext& ctx) {
    FakeEngineCatalog catalog;
    const char* def = R"JSON({"id":"rgw-attempts","root":{"type":"worker","id":"impl",)JSON"
        R"JSON("prompt":"do work","summary":"s","timeoutSec":60,)JSON"
        R"JSON("engine":"agy","model":"m","maxAttempts":3}})JSON";
    FakeJobRunner runner;
    BehaviorTree bt;
    std::string err;
    const bool loaded = bt.LoadDefinition(def, &catalog, err);
    HT_CHECK(ctx, loaded, std::string("load: ") + err);
    bt.SetJobRunner(&runner);
    bt.Start(1000);
    HT_CHECK(ctx, runner.startedWorkers.size() == 1, "worker launched");
    if (!runner.startedWorkers.empty()) {
        HT_CHECK_D(ctx, runner.startedWorkers[0].maxAttempts == 3,
                   "BT maxAttempts reaches the LLM worker job spec",
                   "got " + std::to_string(runner.startedWorkers[0].maxAttempts));
    }
});

HANCHO_TEST("bt.retry.spent_budget", [](htest::TestContext& ctx) {
    FakeEngineCatalog catalog;
    const char* def = R"JSON({"id":"rgw-budget","root":{"type":"retry","id":"r","max":2,)JSON"
        R"JSON("child":{"type":"job","id":"flaky","summary":"f","command":"f",)JSON"
        R"JSON("timeoutSec":60}}})JSON";
    FakeJobRunner runner;
    BehaviorTree bt;
    std::string err;
    const bool loaded = bt.LoadDefinition(def, &catalog, err);
    HT_CHECK(ctx, loaded, std::string("load: ") + err);
    bt.SetJobRunner(&runner);
    bt.Start(1000);
    bt.OnJobFinished("rgw-budget-flaky-1", false, 1, "", "", 1100);
    bt.OnJobFinished("rgw-budget-flaky-2", false, 1, "", "", 1200);
    HT_CHECK(ctx, bt.State() == "failed" && runner.startedJobs.size() == 2,
             "retry exhausted after max=2 attempts");
    HT_CHECK(ctx, !bt.Retry("", false, 5000, err) && !err.empty(),
             std::string("spent retry without reset refused: ") + err);
    HT_CHECK(ctx, runner.startedJobs.size() == 2,
             "refused retry launches no attempt past max");
    HT_CHECK(ctx, bt.Retry("", true, 5000, err) && runner.startedJobs.size() == 3,
             std::string("retry with resetCounters starts a fresh attempt: ") + err);
});

HANCHO_TEST("bt.escalate.diagnostics_in_packet", [](htest::TestContext& ctx) {
    FakeEngineCatalog catalog;
    const char* def = R"JSON({"id":"rgw-esc-diag","root":{"type":"fallback","children":[)JSON"
        R"JSON({"type":"job","id":"impl","summary":"s","command":"c","timeoutSec":60},)JSON"
        R"JSON({"type":"escalate","id":"gate","question":"What now?",)JSON"
        R"JSON("include":{"logs":["impl"]}}]}})JSON";
    FakeJobRunner runner;
    BehaviorTree bt;
    std::string err;
    const bool loaded = bt.LoadDefinition(def, &catalog, err);
    HT_CHECK(ctx, loaded, std::string("load: ") + err);
    bt.SetJobRunner(&runner);
    bt.Start(1000);
    bt.OnJobFinished("rgw-esc-diag-impl-1", false, -1, "", "", 1100, "", "", "timedOut",
                     kDiagnostics);
    const std::string pkt = bt.GetEscalationPacketJson("gate");
    HT_CHECK(ctx, pkt.find("\"diagnostics\"") != std::string::npos,
             "escalation packet has include.diagnostics");
    HT_CHECK(ctx, pkt.find("startup_hang") != std::string::npos &&
                  pkt.find("no_model") != std::string::npos &&
                  pkt.find("retry_with_change") != std::string::npos,
             "packet carries failure class, cause and suggested action for the LLM");
    HT_CHECK(ctx, bt.GetEscalationPacketMarkdown("gate").find("Included Failure Diagnostics") !=
                      std::string::npos,
             "markdown packet has the failure diagnostics section");
});

HANCHO_TEST("bt.escalate.takeover_persisted", [](htest::TestContext& ctx) {
    FakeEngineCatalog catalog;
    const char* def = R"JSON({"id":"rgw-takeover","root":{"type":"sequence","children":[)JSON"
        R"JSON({"type":"job","id":"prep","summary":"prep","command":"p","timeoutSec":30},)JSON"
        R"JSON({"type":"escalate","id":"ask","question":"Ship?","expect":["approve","reject"],)JSON"
        R"JSON("timeoutSec":500}]}})JSON";
    FakeJobRunner runner;
    BehaviorTree bt;
    std::string err;
    const bool loaded = bt.LoadDefinition(def, &catalog, err);
    HT_CHECK(ctx, loaded, std::string("load: ") + err);
    bt.SetJobRunner(&runner);
    bt.Start(1000);
    bt.OnJobFinished("rgw-takeover-prep-1", true, 0, "", "ok", 1100);
    HT_CHECK(ctx, bt.State() == "waitingForLlm", "tree waits at the escalation gate");
    HT_CHECK(ctx, bt.Resume("ask", "takeover", "operator handles it", {}, 2000, err),
             std::string("takeover accepted outside expect: ") + err);
    const std::string state = bt.ToStateJson();
    HT_CHECK(ctx, bt.GetVar("ask.answer") == "takeover", "answer variable is takeover");
    HT_CHECK(ctx, state.find("\"takenOver\"") != std::string::npos &&
                  state.find("mcp") != std::string::npos,
             "takeover is persisted in the saved state");
});

HANCHO_TEST("bt.engine.down_hold_recovery", [](htest::TestContext& ctx) {
    FakeEngineCatalog catalog;
    const char* def = R"JSON({"id":"rgw-hold","root":{"type":"worker","id":"impl",)JSON"
        R"JSON("prompt":"do work","summary":"held","timeoutSec":60,)JSON"
        R"JSON("engine":"opencode","model":"m1"}})JSON";
    BoundaryHealth health;
    health.down.insert("opencode/m1");
    health.nextProbeAt = 2500;
    FakeJobRunner runner;
    BehaviorTree bt;
    std::string err;
    const bool loaded = bt.LoadDefinition(def, &catalog, err);
    HT_CHECK(ctx, loaded, std::string("load: ") + err);
    bt.SetJobRunner(&runner);
    bt.SetEngineHealth(&health);
    bt.Start(1000);
    HT_CHECK(ctx, runner.startedWorkers.empty() && bt.State() == "running",
             "down LLM engine holds the worker instead of failing it");
    bool sawHold = false;
    for (const auto& ev : bt.Events()) {
        if (ev.type == "nodeHeldForEngine") sawHold = true;
    }
    HT_CHECK(ctx, sawHold, "nodeHeldForEngine event emitted");
    bt.OnTimer(2400);
    HT_CHECK(ctx, runner.startedWorkers.empty(), "worker stays held before nextProbeAt");
    health.down.clear();
    bt.OnTimer(2600);
    bool sawRecovered = false;
    for (const auto& ev : bt.Events()) {
        if (ev.type == "engineRecovered") sawRecovered = true;
    }
    HT_CHECK(ctx, sawRecovered, "engineRecovered event emitted after recovery");
    bt.Tick(2700);
    HT_CHECK(ctx, runner.startedWorkers.size() == 1 &&
                  runner.startedWorkers[0].engine == "opencode",
             "held LLM worker starts after the engine recovers");
});
