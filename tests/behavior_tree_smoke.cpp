// Headless smoke test for the Behavior Tree core engine (src/behavior_tree_*.cpp).

#include "behavior_tree_smoke_common.h"

namespace bt_test {
int g_failures = 0;
int g_passes = 0;
} // namespace bt_test

using namespace bt_test;
using namespace pc;

static void RemoveTempDirRecursive(const std::wstring& dir) {
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring full = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveTempDirRecursive(full);
            } else {
                DeleteFileW(full.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

int main() {
    SetConsoleOutputCP(CP_UTF8);

    wchar_t tmpBase[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tmpBase);
    std::wstring tempDirW = std::wstring(tmpBase) + L"pc_bt_smoke_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(tempDirW.c_str(), nullptr);
    std::string tempDirUtf8 = WideToUtf8(tempDirW);

    FakeEngineCatalog catalog;

    // 1. Worker nodes with two different engines, result/output variables, outputSchema failure, unknown engine
    {
        std::string badBt = R"({
            "id": "bad-eng",
            "worker": { "engine": "nonexistent-cli" },
            "root": { "type": "worker", "id": "w1", "summary": "s", "prompt": "p", "timeoutSec": 60 }
        })";
        std::string valErr;
        Check(!BehaviorTree::ValidateJson(badBt, &catalog, valErr) && valErr.find("worker") != std::string::npos,
              "unknown engine rejected at validation: " + valErr);

        std::string workerBt = R"JSON({
            "id": "worker-test",
            "worker": { "engine": "agy", "model": "gemini-4-argon-high" },
            "root": {
                "type": "sequence",
                "children": [
                    {
                        "type": "worker",
                        "id": "w1",
                        "summary": "Analyze with default engine",
                        "prompt": "Inspect repo",
                        "timeoutSec": 300,
                        "outputSchema": {
                            "type": "object",
                            "required": ["file", "issues"],
                            "properties": {
                                "file": { "type": "string" },
                                "issues": { "type": "integer" }
                            }
                        }
                    },
                    {
                        "type": "worker",
                        "id": "w2",
                        "engine": "claude",
                        "model": "claude-opus-5-5",
                        "summary": "Fix ${w1.output.file} (${w1.output.issues})",
                        "prompt": "Prev response: ${w1.result}",
                        "timeoutSec": 600
                    }
                ]
            }
        })JSON";

        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(workerBt, &catalog, err), "load worker BT: " + err);
        bt.SetJobRunner(&runner);
        bt.Start(1000);

        Check(bt.State() == "running", "worker BT is running");
        Check(runner.startedWorkers.size() == 1, "first worker started");
        if (runner.startedWorkers.size() == 1) {
            Check(runner.startedWorkers[0].engine == "agy" &&
                  runner.startedWorkers[0].model == "gemini-4-argon-high" &&
                  runner.startedWorkers[0].proposedJobId == "worker-test-1",
                  "w1 uses BT default engine/model");
        }

        // Complete w1 with valid outputSchema
        bt.OnJobFinished("worker-test-1", true, 0, "w1.log", "done",
                         1500, "Found 2 issues in src/foo.cpp",
                         R"({"file":"src/foo.cpp","issues":2})");

        Check(bt.GetVar("w1.result") == "Found 2 issues in src/foo.cpp", "w1.result captured");
        Check(bt.GetVar("w1.output.file") == "src/foo.cpp", "w1.output.file captured");
        Check(bt.GetVar("w1.output.issues") == "2", "w1.output.issues captured");
        Check(runner.startedWorkers.size() == 2, "second worker started after w1");
        if (runner.startedWorkers.size() == 2) {
            Check(runner.startedWorkers[1].engine == "claude" &&
                  runner.startedWorkers[1].model == "claude-opus-5-5",
                  "w2 overrides engine/model to claude");
            Check(runner.startedWorkers[1].summary == "Fix src/foo.cpp (2)",
                  "w2 summary expanded w1.output.*: " + runner.startedWorkers[1].summary);
            Check(runner.startedWorkers[1].prompt == "Prev response: Found 2 issues in src/foo.cpp",
                  "w2 prompt expanded w1.result");
        }

        bt.OnJobFinished("worker-test-2", true, 0, "w2.log", "fixed", 2000, "Fixed all", "{}");
        Check(bt.State() == "succeeded", "worker BT succeeded");

        // Now test outputSchema failure -> fails with detail "outputInvalid"
        FakeJobRunner runner2;
        BehaviorTree bt2;
        bt2.LoadDefinition(workerBt, &catalog, err);
        bt2.SetJobRunner(&runner2);
        bt2.Start(1000);
        bt2.OnJobFinished("worker-test-1", true, 0, "w1.log", "done",
                          1200, "Bad json output", R"({"file":"only_file_missing_issues"})");
        Check(bt2.State() == "failed" && bt2.NodeDetail("w1").rfind("outputInvalid: ", 0) == 0 &&
              bt2.NodeDetail("w1").find("issues") != std::string::npos,
              "worker outputSchema failure fails node with outputInvalid: " + bt2.NodeDetail("w1"));
    }

    // 2. Composites & Decorators: sequence, fallback/selector, retry (recovery + exhaustion), invert, forceSuccess, forceFailure
    {
        std::string compBt = R"({
            "id": "comp-test",
            "root": {
                "type": "sequence",
                "children": [
                    {
                        "type": "retry",
                        "id": "r1",
                        "max": 3,
                        "child": {
                            "type": "job",
                            "id": "flaky",
                            "summary": "Flaky job",
                            "command": "run_flaky",
                            "timeoutSec": 60
                        }
                    },
                    {
                        "type": "selector",
                        "id": "fb1",
                        "children": [
                            { "type": "fail", "detail": "first branch failed" },
                            { "type": "invert", "child": { "type": "fail" } }
                        ]
                    },
                    { "type": "forceSuccess", "id": "fs", "child": { "type": "fail" } },
                    { "type": "invert", "child": { "type": "forceFailure", "id": "ff", "child": { "type": "succeed" } } }
                ]
            }
        })";

        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(compBt, &catalog, err), "load composite/decorator BT: " + err);
        bt.SetJobRunner(&runner);
        bt.Start(1000);

        Check(runner.startedJobs.size() == 1, "flaky attempt 1 started");
        bt.OnJobFinished("comp-test-1", false, 1, "f1.log", "err1", 1100);
        Check(runner.startedJobs.size() == 2 && bt.NodeRunCount("r1") == 2, "retry started attempt 2");
        bt.OnJobFinished("comp-test-2", true, 0, "f2.log", "ok", 1200);

        Check(bt.State() == "succeeded", "comp-test succeeded after retry recovery + selector + decorators");
        Check(bt.NodeState("fb1") == "succeeded" && bt.NodeState("fs") == "succeeded" && bt.NodeState("ff") == "failed",
              "selector, forceSuccess, forceFailure states verified");

        // Retry exhaustion test
        FakeJobRunner runnerEx;
        BehaviorTree btEx;
        btEx.LoadDefinition(compBt, &catalog, err);
        btEx.SetJobRunner(&runnerEx);
        btEx.Start(1000);
        btEx.OnJobFinished("comp-test-1", false, 1, "", "", 1100);
        btEx.OnJobFinished("comp-test-2", false, 2, "", "", 1200);
        btEx.OnJobFinished("comp-test-3", false, 3, "", "", 1300);
        Check(btEx.State() == "failed" && btEx.NodeRunCount("r1") == 3 && btEx.NodeRunCount("flaky") == 3,
              "retry exhaustion fails after max=3 attempts");
    }

    // 3. Parallel (all and any) with cancellation of remaining jobs
    {
        std::string parAnyBt = R"({
            "id": "par-any",
            "root": {
                "type": "parallel",
                "id": "p_any",
                "success": "any",
                "children": [
                    { "type": "job", "id": "j1", "summary": "J1", "command": "c1", "timeoutSec": 60 },
                    { "type": "job", "id": "j2", "summary": "J2", "command": "c2", "timeoutSec": 60 }
                ]
            }
        })";
        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(parAnyBt, &catalog, err), "load parallel any BT");
        bt.SetJobRunner(&runner);
        bt.Start(1000);
        Check(runner.startedJobs.size() == 2, "parallel started both j1 and j2");
        bt.OnJobFinished("par-any-1", true, 0, "", "", 1100);
        Check(bt.State() == "succeeded", "parallel any succeeded when j1 succeeded");
        Check(runner.WasCancelled("par-any-2"), "parallel any cancelled remaining running job j2");

        // Parallel all: if j1 fails, j2 is cancelled and parallel fails immediately
        std::string parAllBt = R"({
            "id": "par-all",
            "root": {
                "type": "parallel",
                "id": "p_all",
                "success": "all",
                "children": [
                    { "type": "job", "id": "j1", "summary": "J1", "command": "c1", "timeoutSec": 60 },
                    { "type": "job", "id": "j2", "summary": "J2", "command": "c2", "timeoutSec": 60 }
                ]
            }
        })";
        FakeJobRunner runner2;
        BehaviorTree bt2;
        bt2.LoadDefinition(parAllBt, &catalog, err);
        bt2.SetJobRunner(&runner2);
        bt2.Start(1000);
        bt2.OnJobFinished("par-all-1", false, 1, "", "fail", 1100);
        Check(bt2.State() == "failed" && runner2.WasCancelled("par-all-2"),
              "parallel all failed when j1 failed and cancelled j2");
    }

    // 4. if/else, repeat until, timeout cancelling a job, wait via OnTimer
    {
        std::string flowBt = R"({
            "id": "flow-test",
            "vars": { "mode": "second", "cnt": "0" },
            "root": {
                "type": "sequence",
                "children": [
                    {
                        "type": "if",
                        "id": "if1",
                        "cond": { "type": "var", "name": "mode", "eq": "first" },
                        "then": { "type": "set", "var": "branch", "value": "took-then" },
                        "else": { "type": "set", "var": "branch", "value": "took-else" }
                    },
                    {
                        "type": "repeat",
                        "id": "rep1",
                        "max": 5,
                        "child": { "type": "job", "id": "step", "summary": "Step", "command": "step_cmd", "timeoutSec": 30 },
                        "until": { "type": "var", "name": "done", "eq": "yes" }
                    },
                    {
                        "type": "wait",
                        "id": "w_node",
                        "sec": 10
                    },
                    {
                        "type": "invert",
                        "child": {
                            "type": "timeout",
                            "id": "to_node",
                            "sec": 5,
                            "child": { "type": "job", "id": "slow", "summary": "Slow", "command": "slow_cmd", "timeoutSec": 60 }
                        }
                    }
                ]
            }
        })";

        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(flowBt, &catalog, err), "load flow BT: " + err);
        bt.SetJobRunner(&runner);
        bt.Start(1000);

        Check(bt.GetVar("branch") == "took-else", "if/else took else branch");
        Check(runner.startedJobs.size() == 1 && runner.startedJobs[0].nodeId == "step", "repeat started iteration 1");
        bt.OnJobFinished("flow-test-1", true, 0, "", "", 1100);
        Check(runner.startedJobs.size() == 2 && bt.NodeRunCount("rep1") == 2, "repeat started iteration 2 because until was false");

        // Now satisfy until condition via capture or before finishing job 2: let's set var via a job capture or check repeat
        // Wait: let's test repeat until where job 2 log output or exitCode satisfies until!
    }

    // 4b. Dedicated repeat-until + wait + timeout test
    {
        std::string repWaitToBt = R"JSON({
            "id": "rep-wait-to",
            "root": {
                "type": "sequence",
                "children": [
                    {
                        "type": "repeat",
                        "id": "rep",
                        "max": 5,
                        "child": {
                            "type": "job",
                            "id": "step",
                            "summary": "Step",
                            "command": "step",
                            "timeoutSec": 30,
                            "capture": { "status": { "regex": "STATUS=(\\w+)", "group": 1 } }
                        },
                        "until": { "type": "var", "name": "status", "eq": "ready" }
                    },
                    { "type": "wait", "id": "wait5", "sec": 5 },
                    {
                        "type": "timeout",
                        "id": "lim10",
                        "sec": 10,
                        "child": { "type": "job", "id": "hung", "summary": "Hung", "command": "hung", "timeoutSec": 100 }
                    }
                ]
            }
        })JSON";

        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(repWaitToBt, &catalog, err), "load rep-wait-to BT");
        bt.SetJobRunner(&runner);
        bt.Start(1000);

        bt.OnJobFinished("rep-wait-to-1", true, 0, "", "STATUS=warming", 1200);
        Check(bt.NodeRunCount("rep") == 2 && bt.State() == "running", "repeat continues when until is false");

        bt.OnJobFinished("rep-wait-to-2", true, 0, "", "STATUS=ready", 1500);
        Check(bt.NodeState("rep") == "succeeded" && bt.NodeRunCount("rep") == 2, "repeat until succeeded at run 2/5");
        Check(bt.NodeState("wait5") == "running" && bt.NextWakeMs() == 6500, "wait5 is running with NextWakeMs=6500");

        bt.OnTimer(6400);
        Check(bt.NodeState("wait5") == "running", "wait5 still running before deadline");
        bt.OnTimer(6500);
        Check(bt.NodeState("wait5") == "succeeded" && bt.NodeState("hung") == "running",
              "wait5 finished on timer and hung job started");
        Check(bt.NextWakeMs() == 16500, "timeout deadline is 16500");

        bt.OnTimer(16500);
        Check(bt.State() == "failed" && bt.NodeState("lim10") == "failed" && bt.NodeDetail("lim10") == "timeout",
              "timeout node failed at deadline");
        Check(runner.WasCancelled("rep-wait-to-3"), "timeout cancelled running job beneath it");
    }

    // 5. Capture (regex + jsonFile), variable quoting in command, engine variables, undefinedVar
    {
        std::wstring capJsonW = tempDirW + L"\\cap.json";
        WriteFileAtomicWide(capJsonW, R"({"build":{"target":"RelWithDebInfo","code":42}})");

        std::string varBt = R"JSON({
            "id": "var-test",
            "cwd": ")JSON" + JsonEscape(tempDirUtf8) + R"JSON(",
            "vars": { "tricky": "a b & \"quoted\"" },
            "root": {
                "type": "sequence",
                "children": [
                    {
                        "type": "job",
                        "id": "j1",
                        "summary": "Plain ${tricky} on ${bt.id}",
                        "command": "tool.exe --arg ${tricky}",
                        "timeoutSec": 30,
                        "capture": {
                            "errFile": { "regex": "^(\\S+\\.cpp)\\(\\d+\\): error", "group": 1 },
                            "cfg": { "jsonFile": "cap.json", "path": "build.target" }
                        }
                    },
                    {
                        "type": "job",
                        "id": "j2",
                        "summary": "After ${j1.state} exit=${j1.exitCode} job=${j1.jobId} runs=${j1.runs} btRuns=${bt.runs} err=${errFile} cfg=${cfg}",
                        "command": "next.exe ${errFile}",
                        "timeoutSec": 30
                    },
                    {
                        "type": "job",
                        "id": "j_undef",
                        "summary": "Bad ${noSuchVar}",
                        "command": "noop",
                        "timeoutSec": 30
                    }
                ]
            }
        })JSON";

        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(varBt, &catalog, err), "load varBt: " + err);
        bt.SetJobRunner(&runner);
        bt.Start(1000);

        Check(runner.startedJobs.size() == 1, "j1 started");
        if (!runner.startedJobs.empty()) {
            std::string expectedCmd = "tool.exe --arg " + QuoteCommandArg("a b & \"quoted\"");
            Check(runner.startedJobs[0].command == expectedCmd,
                  "command quotes ${tricky} as single argument: " + runner.startedJobs[0].command);
            Check(runner.startedJobs[0].summary == "Plain a b & \"quoted\" on var-test",
                  "summary uses plain replacement: " + runner.startedJobs[0].summary);
        }

        bt.OnJobFinished("var-test-1", true, 0, "logs/j1.log",
                         "Compiling...\r\nsrc/broken.cpp(17): error C2065: undeclared\r\nDone\r\n", 1500);

        Check(bt.GetVar("errFile") == "src/broken.cpp", "regex capture extracted errFile: " + bt.GetVar("errFile"));
        Check(bt.GetVar("cfg") == "RelWithDebInfo", "jsonFile capture extracted cfg: " + bt.GetVar("cfg"));
        Check(!bt.GetVar("j1.start").empty() && bt.GetVar("j1.logFile") == "logs/j1.log", "engine vars j1.start and j1.logFile");
        Check(runner.startedJobs.size() == 2, "j2 started");
        if (runner.startedJobs.size() == 2) {
            Check(runner.startedJobs[1].summary ==
                  "After succeeded exit=0 job=var-test-1 runs=1 btRuns=2 err=src/broken.cpp cfg=RelWithDebInfo",
                  "engine vars expanded in j2 summary: " + runner.startedJobs[1].summary);
        }

        bt.OnJobFinished("var-test-2", true, 0, "logs/j2.log", "ok", 2000);
        Check(bt.State() == "failed" && bt.NodeState("j_undef") == "failed" &&
              bt.NodeDetail("j_undef") == "undefinedVar: noSuchVar",
              "undefined variable fails node with undefinedVar: " + bt.NodeDetail("j_undef"));
    }

    RunPart2Tests(tempDirUtf8, tempDirW);

    RemoveTempDirRecursive(tempDirW);
    std::printf("%s (%d passed, %d failures)\n", g_failures ? "FAILED" : "ALL PASSED", g_passes, g_failures);
    return g_failures ? 1 : 0;
}
