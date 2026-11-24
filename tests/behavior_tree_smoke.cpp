// Headless smoke test for the Behavior Tree core engine (src/behavior_tree_*.cpp).

#include "behavior_tree_smoke_common.h"
#include "../src/bt_templates.h"

#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <utility>

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
                  runner.startedWorkers[0].proposedJobId == "worker-test-w1-1",
                  "w1 uses BT default engine/model");
        }

        // Complete w1 with valid outputSchema
        bt.OnJobFinished("worker-test-w1-1", true, 0, "w1.log", "done",
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

        bt.OnJobFinished("worker-test-w2-1", true, 0, "w2.log", "fixed", 2000, "Fixed all", "{}");
        Check(bt.State() == "succeeded", "worker BT succeeded");

        // Now test outputSchema failure -> fails with detail "outputInvalid"
        FakeJobRunner runner2;
        BehaviorTree bt2;
        bt2.LoadDefinition(workerBt, &catalog, err);
        bt2.SetJobRunner(&runner2);
        bt2.Start(1000);
        bt2.OnJobFinished("worker-test-w1-1", true, 0, "w1.log", "done",
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
        bt.OnJobFinished("comp-test-flaky-1", false, 1, "f1.log", "err1", 1100);
        Check(runner.startedJobs.size() == 2 && bt.NodeRunCount("r1") == 2, "retry started attempt 2");
        bt.OnJobFinished("comp-test-flaky-2", true, 0, "f2.log", "ok", 1200);

        Check(bt.State() == "succeeded", "comp-test succeeded after retry recovery + selector + decorators");
        Check(bt.NodeState("fb1") == "succeeded" && bt.NodeState("fs") == "succeeded" && bt.NodeState("ff") == "failed",
              "selector, forceSuccess, forceFailure states verified");

        // Retry exhaustion test
        FakeJobRunner runnerEx;
        BehaviorTree btEx;
        btEx.LoadDefinition(compBt, &catalog, err);
        btEx.SetJobRunner(&runnerEx);
        btEx.Start(1000);
        btEx.OnJobFinished("comp-test-flaky-1", false, 1, "", "", 1100);
        btEx.OnJobFinished("comp-test-flaky-2", false, 2, "", "", 1200);
        btEx.OnJobFinished("comp-test-flaky-3", false, 3, "", "", 1300);
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
        Check(runner.startedJobs.size() == 2 &&
                  !runner.startedJobs[0].queueName.empty() &&
                  runner.startedJobs[0].queueName != runner.startedJobs[1].queueName,
              "parallel children get distinct queues");
        Check(bt.TaskQueueName() != runner.startedJobs[0].queueName &&
                  bt.TaskQueueName() != runner.startedJobs[1].queueName,
              "parallel child queues differ from the tree queue");
        bt.OnJobFinished("par-any-j1-1", true, 0, "", "", 1100);
        Check(bt.State() == "succeeded", "parallel any succeeded when j1 succeeded");
        Check(runner.WasCancelled("par-any-j2-1"), "parallel any cancelled remaining running job j2");

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
        bt2.OnJobFinished("par-all-j1-1", false, 1, "", "fail", 1100);
        Check(bt2.State() == "failed" && runner2.WasCancelled("par-all-j2-1"),
              "parallel all failed when j1 failed and cancelled j2");

        // Serial nodes never run together, so they stay on the tree queue (no extra queues).
        std::string serialBt = R"({
            "id": "serial-q",
            "root": {
                "type": "sequence",
                "children": [
                    { "type": "job", "id": "s1", "summary": "S1", "command": "c1", "timeoutSec": 60 },
                    { "type": "job", "id": "s2", "summary": "S2", "command": "c2", "timeoutSec": 60 }
                ]
            }
        })";
        FakeJobRunner runner3;
        BehaviorTree bt3;
        bt3.LoadDefinition(serialBt, &catalog, err);
        bt3.SetJobRunner(&runner3);
        bt3.Start(1000);
        Check(runner3.startedJobs.size() == 1 && runner3.startedJobs[0].queueName.empty(),
              "sequence job stays on the tree queue");
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
        bt.OnJobFinished("flow-test-step-1", true, 0, "", "", 1100);
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

        bt.OnJobFinished("rep-wait-to-step-1", true, 0, "", "STATUS=warming", 1200);
        Check(bt.NodeRunCount("rep") == 2 && bt.State() == "running", "repeat continues when until is false");

        bt.OnJobFinished("rep-wait-to-step-2", true, 0, "", "STATUS=ready", 1500);
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
        Check(runner.WasCancelled("rep-wait-to-hung-1"), "timeout cancelled running job beneath it");
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

        bt.OnJobFinished("var-test-j1-1", true, 0, "logs/j1.log",
                         "Compiling...\r\nsrc/broken.cpp(17): error C2065: undeclared\r\nDone\r\n", 1500);

        Check(bt.GetVar("errFile") == "src/broken.cpp", "regex capture extracted errFile: " + bt.GetVar("errFile"));
        Check(bt.GetVar("cfg") == "RelWithDebInfo", "jsonFile capture extracted cfg: " + bt.GetVar("cfg"));
        Check(!bt.GetVar("j1.start").empty() && bt.GetVar("j1.logFile") == "logs/j1.log", "engine vars j1.start and j1.logFile");
        Check(runner.startedJobs.size() == 2, "j2 started");
        if (runner.startedJobs.size() == 2) {
            Check(runner.startedJobs[1].summary ==
                  "After succeeded exit=0 job=var-test-j1-1 runs=1 btRuns=2 err=src/broken.cpp cfg=RelWithDebInfo",
                  "engine vars expanded in j2 summary: " + runner.startedJobs[1].summary);
        }

        bt.OnJobFinished("var-test-j2-1", true, 0, "logs/j2.log", "ok", 2000);
        Check(bt.State() == "failed" && bt.NodeState("j_undef") == "failed" &&
              bt.NodeDetail("j_undef") == "undefinedVar: noSuchVar",
              "undefined variable fails node with undefinedVar: " + bt.NodeDetail("j_undef"));
    }

    // Start paused: register the tree without creating any jobs; resume starts it.
    {
        std::string pausedBt = R"({
            "id": "paused-test",
            "root": { "type": "job", "id": "p1", "summary": "P", "command": "c", "timeoutSec": 60 }
        })";
        FakeJobRunner runnerP;
        BehaviorTree btP;
        std::string err;
        Check(btP.LoadDefinition(pausedBt, &catalog, err), "load paused-start BT");
        btP.SetJobRunner(&runnerP);
        btP.Start(1000, true);
        Check(btP.State() == "paused" && runnerP.startedJobs.empty(),
              "Start(paused=true) creates no jobs");
        Check(btP.SetPaused(false, 1100, err) && btP.State() == "running" &&
                  runnerP.startedJobs.size() == 1,
              "resuming a paused start creates the first job");
    }

    // estimateSec (optional ETA hint) is carried from the job node into the job spec.
    {
        std::string estBt = R"({
            "id": "est-test",
            "root": { "type": "job", "id": "e1", "summary": "E", "command": "c",
                      "timeoutSec": 600, "estimateSec": 300 }
        })";
        FakeJobRunner runnerE;
        BehaviorTree btE;
        std::string err;
        Check(btE.LoadDefinition(estBt, &catalog, err), "load estimateSec BT");
        btE.SetJobRunner(&runnerE);
        btE.Start(1000);
        Check(runnerE.startedJobs.size() == 1 && runnerE.startedJobs[0].estimateSec == 300 &&
                  runnerE.startedJobs[0].timeoutSec == 600,
              "estimateSec reaches the job spec");
    }

    // A sequence's not-yet-started jobs are planned rows; the running job is already in the queue.
    {
        std::string planBt = R"JSON({
            "id": "plan-test",
            "taskqueue": "main",
            "root": {
                "type": "sequence",
                "children": [
                    { "type": "job", "id": "a", "summary": "A", "command": "a", "timeoutSec": 600, "estimateSec": 100 },
                    { "type": "job", "id": "b", "summary": "B", "command": "b", "timeoutSec": 600, "estimateSec": 200 },
                    { "type": "job", "id": "c", "summary": "C", "command": "c", "timeoutSec": 600, "estimateSec": 300 }
                ]
            }
        })JSON";
        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(planBt, &catalog, err), "load planning BT: " + err);
        bt.SetJobRunner(&runner);
        bt.Start(1000);

        std::vector<BtPlannedTask> planned = bt.PlannedTasks();
        Check(planned.size() == 2 && planned[0].nodeKey == "b" && planned[1].nodeKey == "c",
              "sequence reports future jobs as planned");
        Check(!planned.empty() && planned[0].queueName == "main" && planned[0].estimateSec == 200,
              "planned row carries queue and estimate");
        Check(bt.PendingEstimateSec() == 500, "pending estimate sums unstarted sequence jobs");

        bt.OnJobFinished("plan-test-a-1", true, 0, "", "", 1100);
        planned = bt.PlannedTasks();
        Check(planned.size() == 1 && planned[0].nodeKey == "c", "finished job leaves the planned list");
        Check(bt.PendingEstimateSec() == 300, "pending estimate drops the finished job");
    }

    // A branch change drops provisional rows that the tree will no longer run.
    {
        std::string branchBt = R"JSON({
            "id": "branch-test",
            "root": {
                "type": "fallback",
                "children": [
                    {
                        "type": "sequence",
                        "children": [
                            { "type": "job", "id": "f1", "summary": "F1", "command": "f1", "timeoutSec": 60, "estimateSec": 10 },
                            { "type": "job", "id": "f2", "summary": "F2", "command": "f2", "timeoutSec": 60, "estimateSec": 20 }
                        ]
                    },
                    { "type": "job", "id": "alt", "summary": "Alt", "command": "alt", "timeoutSec": 60, "estimateSec": 30 }
                ]
            }
        })JSON";
        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(branchBt, &catalog, err), "load branch-change BT: " + err);
        bt.SetJobRunner(&runner);
        bt.Start(1000);

        std::vector<BtPlannedTask> planned = bt.PlannedTasks();
        Check(planned.size() == 1 && planned[0].nodeKey == "f2",
              "fallback shows only the active branch's planned job");
        Check(bt.PendingEstimateSec() == 20, "fallback estimate ignores inactive branches");

        bt.OnJobFinished("branch-test-f1-1", false, 1, "", "", 1100);
        planned = bt.PlannedTasks();
        Check(planned.empty(), "failed branch's provisional job is gone after the tree moves on");
        Check(bt.PendingEstimateSec() == 0, "pending estimate follows the new branch");
    }

    // 091-1: failure diagnostic variables, the `failure` condition and escalate packet.
    {
        const std::string diagJson = R"({"class":"startup_hang","confidence":"high",)"
            R"("summary":"The worker started but emitted no events.",)"
            R"("suggestedAction":"retry_with_change","retryAt":null,)"
            R"("likelyCauses":[{"id":"no_model","why":"no --model"},{"id":"waiting_for_input","why":"idle"}],)"
            R"("nextSteps":[{"action":"retry"},{"action":"retry","change":{"model":"m"}}],)"
            R"("supervisorNote":"do not take over"})";

        auto treeWithCond = [](const std::string& id, const std::string& cond) {
            return std::string("{\"id\":\"") + id + R"(","root":{"type":"fallback","children":[)"
                R"({"type":"job","id":"impl","summary":"s","command":"c","timeoutSec":60},)"
                + cond + "]}}";
        };

        // (2) class match (array) and the diagnostic variables.
        {
            FakeJobRunner runner;
            BehaviorTree bt;
            std::string err;
            std::string def = treeWithCond("fc-match",
                R"({"type":"failure","id":"cond","node":"impl","class":["startup_hang","idle_stall"]})");
            Check(bt.LoadDefinition(def, &catalog, err), "091-1: load failure-match BT: " + err);
            bt.SetJobRunner(&runner);
            bt.Start(1000);
            bt.OnJobFinished("fc-match-impl-1", false, -1, "", "", 1100, "", "", "timedOut", diagJson);
            Check(bt.NodeState("cond") == "succeeded", "091-1: failure class array match succeeds");
            Check(bt.GetVar("impl.failureClass") == "startup_hang", "091-1: ${impl.failureClass}");
            Check(bt.GetVar("impl.failureSummary") == "The worker started but emitted no events.",
                  "091-1: ${impl.failureSummary}");
            Check(bt.GetVar("impl.likelyCauses") == "no_model,waiting_for_input",
                  "091-1: ${impl.likelyCauses} is comma-separated ids");
            Check(bt.GetVar("impl.suggestedAction") == "retry_with_change", "091-1: ${impl.suggestedAction}");
            Check(bt.GetVar("impl.retryAt").empty(), "091-1: null retryAt becomes empty string");
            Check(bt.GetVar("impl.engine").empty() && bt.GetVar("impl.model").empty(),
                  "091-1: job leaves engine/model empty");

            BtDebugNode dbg;
            std::string dbgErr;
            Check(ParseBtDebugTree(bt.ToStateJson(), dbg, dbgErr), "091-1: state parses in bt_debug_view: " + dbgErr);
            bool foundLabel = false;
            auto findNode = [&](auto&& self, const BtDebugNode& n) -> void {
                if (n.nodeId == "cond") {
                    foundLabel = n.label.find("startup_hang") != std::string::npos &&
                                 n.label.find("idle_stall") != std::string::npos;
                }
                for (const auto& c : n.children) self(self, c);
            };
            findNode(findNode, dbg);
            Check(foundLabel, "091-1: bt_debug_view label shows failure class values");
        }

        // (2) class mismatch and cause match.
        {
            FakeJobRunner runner;
            BehaviorTree bt;
            std::string err;
            Check(bt.LoadDefinition(treeWithCond("fc-miss",
                R"({"type":"failure","id":"cond","node":"impl","class":"auth_error"})"), &catalog, err),
                "091-1: load failure-miss BT: " + err);
            bt.SetJobRunner(&runner);
            bt.Start(1000);
            bt.OnJobFinished("fc-miss-impl-1", false, -1, "", "", 1100, "", "", "timedOut", diagJson);
            Check(bt.NodeState("cond") == "failed", "091-1: failure class mismatch fails");
        }
        {
            FakeJobRunner runner;
            BehaviorTree bt;
            std::string err;
            Check(bt.LoadDefinition(treeWithCond("fc-cause",
                R"({"type":"failure","id":"cond","node":"impl","cause":"no_model"})"), &catalog, err),
                "091-1: load failure-cause BT: " + err);
            bt.SetJobRunner(&runner);
            bt.Start(1000);
            bt.OnJobFinished("fc-cause-impl-1", false, -1, "", "", 1100, "", "", "timedOut", diagJson);
            Check(bt.NodeState("cond") == "succeeded", "091-1: likely cause match succeeds");
        }

        // (2) unexecuted target node -> condition fails.
        {
            std::string def = R"({"id":"fc-unexec","root":{"type":"if",)"
                R"("cond":{"type":"succeed"},)"
                R"("then":{"type":"failure","id":"cond","node":"never","class":"startup_hang"},)"
                R"("else":{"type":"job","id":"never","summary":"n","command":"n","timeoutSec":60}}})";
            FakeJobRunner runner;
            BehaviorTree bt;
            std::string err;
            Check(bt.LoadDefinition(def, &catalog, err), "091-1: load unexecuted-target BT: " + err);
            bt.SetJobRunner(&runner);
            bt.Start(1000);
            Check(bt.NodeState("never") == "idle", "091-1: target node not executed");
            Check(bt.NodeState("cond") == "failed", "091-1: failure condition on unexecuted node fails");
        }

        // (2) unknown node id is a validation error.
        {
            std::vector<std::string> errs;
            std::string def = R"({"id":"fc-bad","root":{"type":"failure","node":"nope","class":"startup_hang"}})";
            Check(!BehaviorTree::ValidateJson(def, &catalog, errs), "091-1: unknown node id rejected");
            bool mentionsNode = false;
            for (const auto& e : errs) if (e.find("nope") != std::string::npos) mentionsNode = true;
            Check(mentionsNode, "091-1: validation error names the unknown node");
        }

        // (3) success clears the failure variables so a stale failure never leaks.
        {
            FakeJobRunner runner;
            BehaviorTree bt;
            std::string err;
            Check(bt.LoadDefinition(treeWithCond("fc-clear",
                R"({"type":"failure","id":"cond","node":"impl","class":"startup_hang"})"), &catalog, err),
                "091-1: load clear-vars BT: " + err);
            bt.SetJobRunner(&runner);
            bt.Start(1000);
            bt.OnJobFinished("fc-clear-impl-1", false, -1, "", "", 1100, "", "", "timedOut", diagJson);
            Check(bt.GetVar("impl.failureClass") == "startup_hang" && bt.NodeState("cond") == "succeeded",
                  "091-1: failure values present before recovery");
            std::string rerr;
            Check(bt.Retry("impl", false, 2000, rerr), "091-1: retry impl: " + rerr);
            bt.OnJobFinished("fc-clear-impl-2", true, 0, "", "", 2100);
            Check(bt.GetVar("impl.failureClass").empty(), "091-1: success clears ${impl.failureClass}");
            Check(bt.GetVar("impl.likelyCauses").empty(), "091-1: success clears ${impl.likelyCauses}");
            Check(bt.GetVar("impl.failureSummary").empty(), "091-1: success clears ${impl.failureSummary}");
        }

        // (4) escalate packet carries the diagnosis of failed include.logs nodes.
        {
            std::string def = R"({"id":"fc-esc","root":{"type":"fallback","children":[)"
                R"({"type":"job","id":"impl","summary":"s","command":"c","timeoutSec":60},)"
                R"({"type":"escalate","id":"gate","question":"What now?",)"
                R"("include":{"logs":["impl"]}}]}})";
            FakeJobRunner runner;
            BehaviorTree bt;
            std::string err;
            Check(bt.LoadDefinition(def, &catalog, err), "091-1: load escalate-diagnostic BT: " + err);
            bt.SetJobRunner(&runner);
            bt.Start(1000);
            bt.OnJobFinished("fc-esc-impl-1", false, -1, "", "", 1100, "", "", "timedOut", diagJson);
            std::string pkt = bt.GetEscalationPacketJson("gate");
            Check(pkt.find("\"diagnostics\"") != std::string::npos,
                  "091-1: packet has include.diagnostics");
            Check(pkt.find("startup_hang") != std::string::npos &&
                  pkt.find("no_model") != std::string::npos &&
                  pkt.find("retry_with_change") != std::string::npos &&
                  pkt.find("nextSteps") != std::string::npos &&
                  pkt.find("do not take over") != std::string::npos,
                  "091-1: packet includes class/causes/action/nextSteps/supervisorNote");
            std::string md = bt.GetEscalationPacketMarkdown("gate");
            Check(md.find("Included Failure Diagnostics") != std::string::npos,
                  "091-1: markdown has a failure diagnostics section");
        }
    }

    // 091-3: engine candidates, switching, approvals and probes.
    {
        class ProbeCatalog : public bt_test::FakeEngineCatalog {
        public:
            mutable std::set<std::string> approved;
            bool IsApprovedForList(const std::string&, const std::string& engine,
                                   const std::string& model) const override {
                return approved.count(engine + "/" + model) != 0;
            }
        };
        class FakeProber : public IBtProber {
        public:
            std::vector<std::pair<std::string, bool>> script;
            mutable int next = 0;
            BtProbeResult Probe(const std::string& engine, const std::string&, int) override {
                BtProbeResult r;
                bool ok = true;
                if (next < static_cast<int>(script.size())) ok = script[static_cast<size_t>(next++)].second;
                r.ok = ok;
                r.failureClass = ok ? "" : "startup_hang";
                r.detail = ok ? "ok" : "no events";
                r.elapsedMs = 5;
                (void)engine;
                return r;
            }
        };

        const char* switchBt = R"JSON({"id":"eng-switch","root":{
            "type":"worker","id":"impl","prompt":"do work","summary":"switch test","timeoutSec":60,
            "engines":[{"engine":"opencode","model":"m1"},{"engine":"agy","model":"m2"}],
            "switchEngineOn":["startup_hang","auth_error","probe_failed"]}})JSON";

        ProbeCatalog catalogAll;
        catalogAll.approved.insert("opencode/m1");
        catalogAll.approved.insert("agy/m2");
        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(switchBt, &catalogAll, err), "091-3: load engines BT: " + err);
        bt.SetJobRunner(&runner);
        bt.Start(1000);
        Check(runner.startedWorkers.size() == 1 && runner.startedWorkers[0].engine == "opencode",
              "091-3: first approved candidate is used");
        bt.OnJobFinished("eng-switch-impl-1", false, -1, "", "", 1100, "", "",
                         "timedOut", R"({"class":"startup_hang","summary":"no events"})");
        Check(runner.startedWorkers.size() == 2 && runner.startedWorkers[1].engine == "agy" &&
              runner.startedWorkers[1].model == "m2", "091-3: startup_hang switches to next candidate");
        bool sawSwitch = false;
        for (const auto& ev : bt.Events()) if (ev.type == "engineSwitched") sawSwitch = true;
        Check(sawSwitch, "091-3: engineSwitched event emitted");
        Check(bt.GetVar("impl.engine") == "agy" && bt.GetVar("impl.model") == "m2",
              "091-3: ${node.engine}/${node.model} track the last candidate");
        Check(bt.ToStateJson().find("engineAttempts") != std::string::npos,
              "091-3: attempt history persisted in node state");
        bt.OnJobFinished("eng-switch-impl-2", true, 0, "", "ok", 1200, "done", "{}");
        Check(bt.State() == "succeeded", "091-3: tree succeeds after switch");

        // Non-switchable failure must not advance the candidate.
        FakeJobRunner runner2;
        BehaviorTree bt2;
        bt2.LoadDefinition(switchBt, &catalogAll, err);
        bt2.SetJobRunner(&runner2);
        bt2.Start(1000);
        bt2.OnJobFinished("eng-switch-impl-1", false, 1, "", "", 1100, "", "",
                          "failed", R"({"class":"test_failed","summary":"assertion"})");
        Check(runner2.startedWorkers.size() == 1 && bt2.State() == "failed",
              "091-3: non-switchable failure does not switch");

        // Candidate missing from the worker list is rejected; approval makes it pass.
        ProbeCatalog catalogNone;
        std::vector<std::string> errs;
        Check(!BehaviorTree::ValidateJson(switchBt, &catalogNone, errs) &&
              errs[0].find("llm add --list worker") != std::string::npos,
              "091-3: bt validate rejects unapproved candidate with guidance");
        errs.clear();
        Check(BehaviorTree::ValidateJson(switchBt, &catalogAll, errs) && errs.empty(),
              "091-3: bt validate accepts approved candidates");

        // probe leaf and worker preflight.
        const char* probeBt = R"JSON({"id":"probe-test","root":{"type":"sequence","children":[
            {"type":"probe","id":"p","engine":"opencode","model":"m1","timeoutSec":5},
            {"type":"succeed"}]}})JSON";
        FakeProber proberOk;
        proberOk.script.push_back({"opencode", true});
        FakeJobRunner runner3;
        BehaviorTree bt3;
        bt3.LoadDefinition(probeBt, &catalogAll, err);
        bt3.SetJobRunner(&runner3);
        bt3.SetProber(&proberOk);
        bt3.Start(1000);
        Check(bt3.State() == "succeeded" && bt3.NodeState("p") == "succeeded",
              "091-3: probe leaf succeeds when engine answers");

        FakeProber proberBad;
        proberBad.script.push_back({"opencode", false});
        FakeJobRunner runner4;
        BehaviorTree bt4;
        bt4.LoadDefinition(probeBt, &catalogAll, err);
        bt4.SetJobRunner(&runner4);
        bt4.SetProber(&proberBad);
        bt4.Start(1000);
        Check(bt4.State() == "failed" && bt4.GetVar("p.failureClass") == "probe_failed",
              "091-3: failed probe is classified probe_failed");

        const char* preflightBt = R"JSON({"id":"preflight","root":{
            "type":"worker","id":"impl","prompt":"do work","summary":"preflight","timeoutSec":60,
            "probe":true,
            "engines":[{"engine":"opencode","model":"m1"},{"engine":"agy","model":"m2"}],
            "switchEngineOn":["probe_failed"]}})JSON";
        FakeProber proberSwitch;
        proberSwitch.script.push_back({"opencode", false});
        proberSwitch.script.push_back({"agy", true});
        FakeJobRunner runner5;
        BehaviorTree bt5;
        bt5.LoadDefinition(preflightBt, &catalogAll, err);
        bt5.SetJobRunner(&runner5);
        bt5.SetProber(&proberSwitch);
        bt5.Start(1000);
        Check(runner5.startedWorkers.size() == 1 && runner5.startedWorkers[0].engine == "agy",
              "091-3: failed preflight switches before launching production");

        FakeProber proberFailAll;
        proberFailAll.script.push_back({"opencode", false});
        proberFailAll.script.push_back({"agy", false});
        FakeJobRunner runner6;
        BehaviorTree bt6;
        bt6.LoadDefinition(preflightBt, &catalogAll, err);
        bt6.SetJobRunner(&runner6);
        bt6.SetProber(&proberFailAll);
        bt6.Start(1000);
        Check(runner6.startedWorkers.empty() && bt6.State() == "failed" &&
              bt6.GetVar("impl.failureClass") == "probe_failed",
              "091-3: all probes fail -> production never starts");

        // 101-1: switchEngineOn with {class, after} counts per candidate and persists across save/load.
        ProbeCatalog catalog3;
        catalog3.approved.insert("opencode/m1");
        catalog3.approved.insert("agy/m2");
        catalog3.approved.insert("claude/m3");
        const char* afterBt = R"JSON({"id":"eng-after","root":{
            "type":"retry","max":5,"child":{
                "type":"worker","id":"impl","prompt":"do work","summary":"after test","timeoutSec":60,
                "engines":[
                    {"engine":"opencode","model":"m1"},
                    {"engine":"agy","model":"m2"},
                    {"engine":"claude","model":"m3"}],
                "switchEngineOn":["startup_hang",{"class":"test_failed","after":2}]}}})JSON";
        FakeJobRunner runnerAfter;
        BehaviorTree btAfter;
        Check(btAfter.LoadDefinition(afterBt, &catalog3, err), "101-1: load afterBt: " + err);
        btAfter.SetJobRunner(&runnerAfter);
        btAfter.Start(1000);
        Check(runnerAfter.startedWorkers.size() == 1 && runnerAfter.startedWorkers[0].engine == "opencode",
              "101-1: starts with first candidate");
        // 1st test_failed does not switch; outer retry re-runs with the same candidate.
        btAfter.OnJobFinished("eng-after-impl-1", false, 1, "", "", 1100, "", "",
                              "failed", R"({"class":"test_failed","summary":"fail 1"})");
        Check(runnerAfter.startedWorkers.size() == 2 && runnerAfter.startedWorkers[1].engine == "opencode",
              "101-1: after=2 stays on same candidate after 1st failure");

        // Save and reload after 1st failure: count must persist so 2nd failure switches immediately.
        const std::string savedAfter = btAfter.ToStateJson();
        FakeJobRunner runnerReload;
        BehaviorTree btReload;
        Check(btReload.LoadFromStateJson(savedAfter, &catalog3, err), "101-1: reload state: " + err);
        btReload.SetJobRunner(&runnerReload);
        btReload.OnJobFinished("eng-after-impl-2", false, 1, "", "", 1200, "", "",
                               "failed", R"({"class":"test_failed","summary":"fail 2"})");
        Check(runnerReload.startedWorkers.size() == 1 && runnerReload.startedWorkers[0].engine == "agy",
              "101-1: 2nd failure after reload switches to 2nd candidate");

        // Count resets to 0 on the new candidate: 1st failure on agy does not switch, 2nd does.
        btReload.OnJobFinished("eng-after-impl-3", false, 1, "", "", 1300, "", "",
                               "failed", R"({"class":"test_failed","summary":"fail 3"})");
        Check(runnerReload.startedWorkers.size() == 2 && runnerReload.startedWorkers[1].engine == "agy",
              "101-1: count resets to 0 after switch (1st failure on new candidate stays)");
        btReload.OnJobFinished("eng-after-impl-4", false, 1, "", "", 1400, "", "",
                               "failed", R"({"class":"test_failed","summary":"fail 4"})");
        Check(runnerReload.startedWorkers.size() == 3 && runnerReload.startedWorkers[2].engine == "claude",
              "101-1: 2nd failure on new candidate switches to 3rd candidate");
        btReload.OnJobFinished("eng-after-impl-5", true, 0, "", "ok", 1500, "done", "{}");
        Check(btReload.State() == "succeeded", "101-1: tree succeeds after second switch");

        // Validation errors for invalid switchEngineOn entries.
        auto expectInvalid = [&](const std::string& swJson, const std::string& label) {
            std::string doc = R"JSON({"id":"inv","root":{"type":"worker","id":"w","prompt":"p","summary":"s","timeoutSec":60,"engines":[{"engine":"opencode","model":"m1"},{"engine":"agy","model":"m2"}],"switchEngineOn":)JSON" + swJson + R"JSON(}})JSON";
            std::vector<std::string> vErrs;
            Check(!BehaviorTree::ValidateJson(doc, &catalog3, vErrs) && !vErrs.empty(),
                  "101-1: invalid switchEngineOn rejected: " + label);
        };
        expectInvalid(R"([{"class":"test_failed","after":0}])", "after=0");
        expectInvalid(R"([{"class":"test_failed","after":11}])", "after=11");
        expectInvalid(R"([{"after":2}])", "missing class");

        // defaults.worker.switchEngineOn with {class, after} is valid.
        const char* defaultsSwBt = R"JSON({"id":"def-sw","defaults":{"worker":{"switchEngineOn":[{"class":"test_failed","after":2}]}},"root":{"type":"worker","id":"w","prompt":"p","summary":"s","timeoutSec":60,"engines":[{"engine":"opencode","model":"m1"},{"engine":"agy","model":"m2"}]}})JSON";
        std::vector<std::string> defErrs;
        Check(BehaviorTree::ValidateJson(defaultsSwBt, &catalog3, defErrs) && defErrs.empty(),
              "101-1: defaults.worker.switchEngineOn with {class, after} is valid");

        // 101-2: worker handoff from PC_BT_RESULT_FILE persists on BtEngineAttempt and lastHandoff,
        // is read on both failure and success, and each value is truncated to 2000 bytes.
        const char* handoffBt = R"JSON({"id":"eng-handoff","root":{
            "type":"retry","max":3,"child":{
                "type":"worker","id":"impl","prompt":"do work","summary":"handoff test","timeoutSec":60,
                "engines":[{"engine":"opencode","model":"m1"},{"engine":"agy","model":"m2"}],
                "switchEngineOn":[{"class":"root_cause_not_found","after":2}]}}})JSON";
        FakeJobRunner runnerHo;
        BehaviorTree btHo;
        Check(btHo.LoadDefinition(handoffBt, &catalog3, err), "101-2: load handoffBt: " + err);
        btHo.SetJobRunner(&runnerHo);
        btHo.Start(1000);
        std::string longSuspected(2500, 's');
        nlohmann::json res1 = {
            {"handoff", {
                {"suspected", longSuspected},
                {"tried", "checked parser"},
                {"ruledOut", "lexer"}
            }}
        };
        // 1st failure: does not switch (after=2), but records attempt + lastHandoff with 2000-byte cap.
        btHo.OnJobFinished("eng-handoff-impl-1", false, 1, "", "", 1100, "", res1.dump(),
                           "failed", R"({"class":"root_cause_not_found","summary":"not found"})");
        nlohmann::json stHo1 = nlohmann::json::parse(btHo.ToStateJson());
        const auto& implSt1 = stHo1["root"]["child"]["status"];
        Check(implSt1["engineAttempts"].size() == 1 &&
                  implSt1["engineAttempts"][0]["handoff"]["tried"] == "checked parser" &&
                  implSt1["engineAttempts"][0]["handoff"]["suspected"].get<std::string>().size() == 2000,
              "101-2: failed non-switch attempt keeps handoff truncated to 2000 bytes");
        Check(implSt1["lastHandoff"]["ruledOut"] == "lexer" &&
                  implSt1["lastHandoff"]["suspected"].get<std::string>().size() == 2000,
              "101-2: lastHandoff saved when not switching");

        // Reload and verify handoff & lastHandoff survive save/load.
        BehaviorTree btHoReload;
        Check(btHoReload.LoadFromStateJson(btHo.ToStateJson(), &catalog3, err), "101-2: reload handoff state");
        FakeJobRunner runnerHo2;
        btHoReload.SetJobRunner(&runnerHo2);
        nlohmann::json res2 = {
            {"handoff", {
                {"suspected", "race in state"},
                {"tried", "added lock"},
                {"ruledOut", "parser"}
            }}
        };
        // 2nd failure: switches to agy/m2 and records handoff on the switched attempt.
        btHoReload.OnJobFinished("eng-handoff-impl-2", false, 1, "", "", 1200, "", res2.dump(),
                                 "failed", R"({"class":"root_cause_not_found","summary":"not found 2"})");
        Check(runnerHo2.startedWorkers.size() == 1 && runnerHo2.startedWorkers[0].engine == "agy",
              "101-2: 2nd root_cause_not_found switches to agy");
        // Succeed on agy with handoff too (read on success as well).
        nlohmann::json res3 = {{"handoff", {{"suspected", "fixed"}, {"tried", "patch"}, {"ruledOut", "all"}}}};
        btHoReload.OnJobFinished("eng-handoff-impl-3", true, 0, "", "ok", 1300, "done", res3.dump());
        nlohmann::json stHoFinal = nlohmann::json::parse(btHoReload.ToStateJson());
        const auto& attemptsFinal = stHoFinal["root"]["child"]["status"]["engineAttempts"];
        Check(attemptsFinal.size() == 3 &&
                  attemptsFinal[0]["handoff"]["tried"] == "checked parser" &&
                  attemptsFinal[1]["handoff"]["tried"] == "added lock" &&
                  attemptsFinal[2]["result"] == "succeeded" &&
                  attemptsFinal[2]["handoff"]["tried"] == "patch",
              "101-2: handoff preserved across reload and read on both failure and success");

        // 101-3: blame & stall detection (fix_failed, fix_no_progress, fix_oscillating, env skip, save/load, validation).
        {
            // (a) Validation errors & blame_before_worker warning.
            std::vector<std::string> bErrs;
            const char* missingBlameBt = R"JSON({"id":"b-miss","root":{"type":"job","id":"t","summary":"t","command":"c","timeoutSec":30,"blame":"no_such_worker"}})JSON";
            Check(!BehaviorTree::ValidateJson(missingBlameBt, &catalog3, bErrs) && !bErrs.empty(),
                  "101-3: blame referencing missing node is rejected");

            bErrs.clear();
            const char* nonWorkerBlameBt = R"JSON({"id":"b-nonw","root":{"type":"sequence","children":[
                {"type":"job","id":"j1","summary":"j1","command":"c","timeoutSec":30},
                {"type":"job","id":"t","summary":"t","command":"c","timeoutSec":30,"blame":"j1"}]}})JSON";
            Check(!BehaviorTree::ValidateJson(nonWorkerBlameBt, &catalog3, bErrs) && !bErrs.empty(),
                  "101-3: blame referencing non-worker node is rejected");

            bErrs.clear();
            std::vector<BtWarning> bWarns;
            const char* beforeWorkerBt = R"JSON({"id":"b-before","root":{"type":"sequence","children":[
                {"type":"job","id":"t","summary":"t","command":"c","timeoutSec":30,"blame":"impl"},
                {"type":"worker","id":"impl","prompt":"p","summary":"w","timeoutSec":30,"idle":{"firstEventSec":60}}]}})JSON";
            Check(BehaviorTree::ValidateJson(beforeWorkerBt, &catalog3, bErrs, bWarns),
                  "101-3: blame_before_worker is a warning, not an error");
            bool hasBeforeWarn = false;
            for (const auto& w : bWarns) {
                if (w.code == "blame_before_worker") hasBeforeWarn = true;
            }
            Check(hasBeforeWarn, "101-3: blame_before_worker warning emitted when job precedes worker");

            // (b) fix_failed -> reload -> fix_no_progress -> switch candidate on next worker run.
            const std::wstring blameDirW = tempDirW + L"\\blame_test";
            CreateDirectoryW(blameDirW.c_str(), nullptr);
            const std::string blameCwd = WideToUtf8(blameDirW);
            const std::string blameBt = std::string(R"JSON({"id":"bt-blame-np","cwd":")JSON") +
                JsonEscape(blameCwd) + R"JSON(","root":{
                "type":"retry","max":5,"child":{
                    "type":"sequence","children":[
                        {"type":"worker","id":"impl","prompt":"fix it","summary":"fix","timeoutSec":60,
                         "engines":[{"engine":"opencode","model":"m1"},{"engine":"agy","model":"m2"}],
                         "switchEngineOn":[{"class":"fix_no_progress","after":1}]},
                        {"type":"job","id":"test","summary":"run tests","command":"ctest","timeoutSec":60,"blame":"impl"}
                    ]}}})JSON";

            FakeJobRunner rBlame;
            BehaviorTree btBlame;
            Check(btBlame.LoadDefinition(blameBt, &catalog3, err), "101-3: load blameBt: " + err);
            btBlame.SetJobRunner(&rBlame);
            btBlame.Start(1000);
            Check(rBlame.startedWorkers.size() == 1 && rBlame.startedWorkers[0].engine == "opencode",
                  "101-3: worker starts on 1st candidate");
            btBlame.OnJobFinished("bt-blame-np-impl-1", true, 0, "", "ok", 1100, "done", "{}");
            Check(rBlame.startedJobs.size() == 1 && rBlame.startedJobs[0].nodeKey == "test",
                  "101-3: test job starts after worker succeeds");
            // 1st test failure: [FAIL] testA, [FAIL] testB -> fix_failed (no switch yet).
            btBlame.OnJobFinished("bt-blame-np-test-1", false, 1, "",
                                  "[FAIL] testA\n[PASS] ok\n[FAIL] testB\n", 1200, "", "", "test failed");
            Check(rBlame.startedWorkers.size() == 2 && rBlame.startedWorkers[1].engine == "opencode",
                  "101-3: first fix_failed does not switch; outer retry re-runs opencode");

            // Save & reload state: recentFailSignatures and switchFailCounts must survive.
            const std::string savedBlame = btBlame.ToStateJson();
            nlohmann::json savedBlameJson = nlohmann::json::parse(savedBlame);
            const auto& implSt = savedBlameJson["root"]["child"]["children"][0]["status"];
            Check(implSt["switchFailCounts"]["fix_failed"] == 1 &&
                  implSt["recentFailSignatures"].size() == 1 &&
                  implSt["recentFailSignatures"][0].size() == 2 &&
                  implSt["engineAttempts"].size() == 1 &&
                  implSt["engineAttempts"][0]["failSignature"].size() == 2,
                  "101-3: fix_failed count, recentFailSignatures, and attempt failSignature saved");

            FakeJobRunner rBlame2;
            BehaviorTree btBlame2;
            Check(btBlame2.LoadFromStateJson(savedBlame, &catalog3, err), "101-3: reload blame state: " + err);
            btBlame2.SetJobRunner(&rBlame2);
            btBlame2.OnJobFinished("bt-blame-np-impl-2", true, 0, "", "ok", 1300, "done", "{}");
            // 2nd test failure with same failing tests in reversed order -> sorted set matches -> fix_no_progress -> switch!
            btBlame2.OnJobFinished("bt-blame-np-test-2", false, 1, "",
                                   "[FAIL] testB\n[FAIL] testA\n[FAIL] testA\n", 1400, "", "", "test failed again");
            Check(rBlame2.startedWorkers.size() == 1 && rBlame2.startedWorkers[0].engine == "agy",
                  "101-3: fix_no_progress switches worker to 2nd candidate for next run");
            btBlame2.OnJobFinished("bt-blame-np-impl-3", true, 0, "", "ok", 1500, "done", "{}");
            btBlame2.OnJobFinished("bt-blame-np-test-3", true, 0, "", "all pass", 1600);
            Check(btBlame2.State() == "succeeded", "101-3: tree succeeds after blamed switch");

            // (c) A -> B -> A triggers fix_oscillating using success.testReport, and env failures are ignored.
            const std::string reportPath = WideToUtf8(blameDirW + L"\\report.xml");
            const std::string oscBt = std::string(R"JSON({"id":"bt-blame-osc","cwd":")JSON") +
                JsonEscape(blameCwd) + R"JSON(","root":{
                "type":"retry","max":6,"child":{
                    "type":"sequence","children":[
                        {"type":"worker","id":"impl","prompt":"fix it","summary":"fix","timeoutSec":60,
                         "engines":[{"engine":"opencode","model":"m1"},{"engine":"agy","model":"m2"}],
                         "switchEngineOn":[{"class":"fix_oscillating","after":1}]},
                        {"type":"job","id":"test","summary":"run tests","command":"ctest","timeoutSec":60,
                         "blame":"impl","success":{"testReport":"report.xml"}}
                    ]}}})JSON";

            FakeJobRunner rOsc;
            BehaviorTree btOsc;
            Check(btOsc.LoadDefinition(oscBt, &catalog3, err), "101-3: load oscBt: " + err);
            btOsc.SetJobRunner(&rOsc);
            btOsc.Start(2000);

            auto writeReport = [&](const std::string& failedTestName) {
                std::string xml = "<testsuite tests=\"2\" failures=\"1\">"
                    "<testcase classname=\"Suite\" name=\"" + failedTestName + "\"><failure message=\"boom\"/></testcase>"
                    "<testcase classname=\"Suite\" name=\"ok\"/>"
                    "</testsuite>";
                WriteFileAtomicWide(Utf8ToWide(reportPath), xml);
            };

            // Env failure on test job must NOT be counted on impl.
            btOsc.OnJobFinished("bt-blame-osc-impl-1", true, 0, "", "ok", 2100, "done", "{}");
            writeReport("tA");
            btOsc.OnJobFinished("bt-blame-osc-test-1", false, 1, "", "", 2200, "", "", "perm",
                                R"({"class":"permission_denied","summary":"denied"})");
            nlohmann::json oscEnvSt = nlohmann::json::parse(btOsc.ToStateJson());
            Check(!oscEnvSt["root"]["child"]["children"][0]["status"].contains("switchFailCounts") &&
                  !oscEnvSt["root"]["child"]["children"][0]["status"].contains("recentFailSignatures"),
                  "101-3: environmental failure class is not counted as fix failure");

            // A -> B -> A oscillation:
            btOsc.OnJobFinished("bt-blame-osc-impl-2", true, 0, "", "ok", 2300, "done", "{}");
            writeReport("tA");
            btOsc.OnJobFinished("bt-blame-osc-test-2", false, 1, "", "", 2400, "", "", "fail A");
            Check(rOsc.startedWorkers.size() == 3 && rOsc.startedWorkers[2].engine == "opencode",
                  "101-3: A -> fix_failed, stays on opencode");

            btOsc.OnJobFinished("bt-blame-osc-impl-3", true, 0, "", "ok", 2500, "done", "{}");
            writeReport("tB");
            btOsc.OnJobFinished("bt-blame-osc-test-3", false, 1, "", "", 2600, "", "", "fail B");
            Check(rOsc.startedWorkers.size() == 4 && rOsc.startedWorkers[3].engine == "opencode",
                  "101-3: A -> B -> fix_failed, stays on opencode");

            btOsc.OnJobFinished("bt-blame-osc-impl-4", true, 0, "", "ok", 2700, "done", "{}");
            writeReport("tA");
            btOsc.OnJobFinished("bt-blame-osc-test-4", false, 1, "", "", 2800, "", "", "fail A again");
            Check(rOsc.startedWorkers.size() == 5 && rOsc.startedWorkers[4].engine == "agy",
                  "101-3: A -> B -> A triggers fix_oscillating and switches to agy");
        }

        // 101-4: Handoff prompt on same-candidate retry vs after switch, 6000-byte trim, and onSwitch.
        {
            std::string reportPath = tempDirUtf8 + "\\handoff_junit.xml";
            auto writeManyTestsReport = [&](int count) {
                std::string xml = "<testsuite tests=\"" + std::to_string(count) + "\">";
                for (int i = 1; i <= count; ++i) {
                    char buf[32];
                    std::snprintf(buf, sizeof(buf), "t%02d", i);
                    xml += std::string("<testcase classname=\"S\" name=\"") + buf + "\"><failure/></testcase>";
                }
                xml += "</testsuite>";
                WriteFileAtomicWide(Utf8ToWide(reportPath), xml);
            };

            std::string hoBt = std::string(R"JSON({
                "id": "bt-101-4-ho",
                "cwd": ")JSON") + JsonEscape(tempDirUtf8) + R"JSON(",
                "root": {
                    "type": "retry",
                    "max": 5,
                    "child": {
                        "type": "sequence",
                        "children": [
                            {
                                "type": "worker",
                                "id": "w",
                                "summary": "Fix code",
                                "prompt": "Base prompt.",
                                "timeoutSec": 60,
                                "onSwitch": "keep",
                                "engines": [
                                    {"engine": "opencode", "model": "m1"},
                                    {"engine": "agy", "model": "m2"},
                                    {"engine": "claude", "model": "m3"}
                                ],
                                "switchEngineOn": [
                                    {"class": "root_cause_not_found", "after": 2},
                                    {"class": "fix_no_progress", "after": 1}
                                ]
                            },
                            {
                                "type": "job",
                                "id": "t",
                                "summary": "Test",
                                "command": "test",
                                "timeoutSec": 60,
                                "blame": "w",
                                "success": {"testReport": ")JSON" + JsonEscape(reportPath) + R"JSON("}
                            }
                        ]
                    }
                }
            })JSON";

            FakeJobRunner rHo;
            BehaviorTree btHo;
            std::string err;
            Check(btHo.LoadDefinition(hoBt, &catalog3, err), "101-4: load hoBt: " + err);
            btHo.SetJobRunner(&rHo);
            btHo.Start(1000);

            // Before any failure/switch: prompt must NOT contain handoff sections.
            Check(rHo.startedWorkers.size() == 1 &&
                  rHo.startedWorkers[0].prompt.find("Previous attempts by other models") == std::string::npos &&
                  rHo.startedWorkers[0].prompt.find("Your previous attempt noted:") == std::string::npos,
                  "101-4: initial worker prompt has no handoff section");

            // Same-candidate failure with handoff (after=2 so no switch yet).
            std::string res1 = R"({"handoff":{"suspected":"null ptr","tried":"added guard","ruledOut":"bad input"}})";
            btHo.OnJobFinished("bt-101-4-ho-w-1", false, 1, "", "HANCHO-RESULT: root_cause_not_found\n",
                               1100, "", res1, "not found",
                               R"({"class":"root_cause_not_found","summary":"not found"})");
            Check(rHo.startedWorkers.size() == 2 && rHo.startedWorkers[1].engine == "opencode" &&
                  rHo.startedWorkers[1].prompt.find("Your previous attempt noted:") != std::string::npos &&
                  rHo.startedWorkers[1].prompt.find("suspected: null ptr") != std::string::npos &&
                  rHo.startedWorkers[1].prompt.find("Previous attempts by other models") == std::string::npos,
                  "101-4: same-candidate retry includes Your previous attempt noted");

            // Now w succeeds twice while blamed test fails twice with 12 identical tests -> fix_no_progress switches to agy/m2.
            btHo.OnJobFinished("bt-101-4-ho-w-2", true, 0, "", "ok", 1200, "done", "{}");
            writeManyTestsReport(12);
            btHo.OnJobFinished("bt-101-4-ho-t-1", false, 1, "", "", 1300, "", "", "tests failed");
            btHo.OnJobFinished("bt-101-4-ho-w-3", true, 0, "", "ok", 1400, "done", "{}");
            writeManyTestsReport(12);
            btHo.OnJobFinished("bt-101-4-ho-t-2", false, 1, "", "", 1500, "", "", "tests failed again");

            Check(rHo.startedWorkers.size() == 4 && rHo.startedWorkers[3].engine == "agy",
                  "101-4: worker switched to agy/m2");
            if (rHo.startedWorkers.size() >= 4) {
                const std::string& p = rHo.startedWorkers[3].prompt;
                Check(p.find("Previous attempts by other models (do not repeat what did not work):") != std::string::npos &&
                      p.find("opencode/m1") != std::string::npos &&
                      p.find("fix_no_progress") != std::string::npos &&
                      p.find("t01") != std::string::npos && p.find("t10") != std::string::npos &&
                      p.find("t11") == std::string::npos && p.find("t12") == std::string::npos &&
                      p.find("Your previous attempt noted:") == std::string::npos,
                      "101-4: switched prompt includes Previous attempts (top 10 tests) and omits Your previous attempt");
            }

            // 6000-byte cap: drop oldest failed attempts first.
            std::string trimBt = R"({
                "id": "bt-101-4-trim",
                "root": {
                    "type": "worker",
                    "id": "w",
                    "summary": "Trim test",
                    "prompt": "Base.",
                    "timeoutSec": 60,
                    "engines": [
                        {"engine": "opencode", "model": "m1"},
                        {"engine": "agy", "model": "m2"},
                        {"engine": "claude", "model": "m3"}
                    ],
                    "switchEngineOn": ["root_cause_not_found"]
                }
            })";
            FakeJobRunner rTrim;
            BehaviorTree btTrim;
            Check(btTrim.LoadDefinition(trimBt, &catalog3, err), "101-4: load trimBt: " + err);
            btTrim.SetJobRunner(&rTrim);
            btTrim.Start(2000);

            std::string bigOld(1800, 'A');
            std::string bigNew(1800, 'B');
            std::string resOld = nlohmann::json({{"handoff", {{"suspected", "OLD_ATTEMPT_MARKER " + bigOld},
                                                    {"tried", bigOld}}}}).dump();
            std::string resNew = nlohmann::json({{"handoff", {{"suspected", "NEW_ATTEMPT_MARKER " + bigNew},
                                                    {"tried", bigNew}}}}).dump();
            btTrim.OnJobFinished("bt-101-4-trim-w-1", false, 1, "", "", 2100, "", resOld, "r1",
                                 R"({"class":"root_cause_not_found","summary":"nf"})");
            btTrim.OnJobFinished("bt-101-4-trim-w-2", false, 1, "", "", 2200, "", resNew, "r2",
                                 R"({"class":"root_cause_not_found","summary":"nf"})");
            Check(rTrim.startedWorkers.size() == 3 && rTrim.startedWorkers[2].engine == "claude",
                  "101-4: trimBt switched twice to claude/m3");
            if (rTrim.startedWorkers.size() >= 3) {
                const std::string& p3 = rTrim.startedWorkers[2].prompt;
                size_t hdrPos = p3.find("Previous attempts by other models (do not repeat what did not work):");
                Check(hdrPos != std::string::npos &&
                      (p3.size() - hdrPos) <= 6000 &&
                      p3.find("NEW_ATTEMPT_MARKER") != std::string::npos &&
                      p3.find("OLD_ATTEMPT_MARKER") == std::string::npos,
                      "101-4: handoff section capped at 6000 bytes by dropping oldest attempt");
            }

            // onSwitch validation and warning.
            auto makeOnSwitchBt = [](const std::string& val) {
                return std::string(R"({"id":"os-test","root":{"type":"catch","child":{"type":"worker","id":"w","summary":"s","prompt":"p","timeoutSec":30,"onSwitch":")") +
                       val + R"(","engines":[{"engine":"opencode","model":"m1"},{"engine":"agy","model":"m2"}],"switchEngineOn":["startup_hang"]},"handlers":[{"on":"*","do":{"type":"fail","detail":"x"}}]}})";
            };
            std::vector<std::string> vErrs;
            std::vector<BtWarning> vWarns;
            Check(BehaviorTree::ValidateJson(makeOnSwitchBt("keep"), &catalog3, vErrs, vWarns),
                  "101-4: onSwitch='keep' is valid");
            bool hasRevWarn = false;
            for (const auto& w : vWarns) if (w.code == "on_switch_revert_unsupported") hasRevWarn = true;
            Check(!hasRevWarn, "101-4: onSwitch='keep' emits no on_switch_revert_unsupported warning");

            vErrs.clear();
            vWarns.clear();
            Check(BehaviorTree::ValidateJson(makeOnSwitchBt("revert"), &catalog3, vErrs, vWarns),
                  "101-4: onSwitch='revert' is valid (not an error)");
            for (const auto& w : vWarns) if (w.code == "on_switch_revert_unsupported") hasRevWarn = true;
            Check(hasRevWarn, "101-4: onSwitch='revert' emits on_switch_revert_unsupported warning");

            vErrs.clear();
            Check(!BehaviorTree::ValidateJson(makeOnSwitchBt("bogus"), &catalog3, vErrs),
                  "101-4: invalid onSwitch value is rejected");
        }

        // 101-5: switchLimits (maxCandidates, maxSec, stopOnSameSymptom) and escalate packet engineAttempts.
        {
            // Validation of switchLimits on worker and defaults.worker.
            std::vector<std::string> vErrs;
            std::string validLim = R"({
                "id": "lim-val",
                "defaults": {"worker": {"switchLimits": {"maxCandidates": 2, "maxSec": 0, "stopOnSameSymptom": false}}},
                "root": {
                    "type": "worker", "id": "w", "summary": "s", "prompt": "p", "timeoutSec": 30,
                    "engines": [{"engine": "opencode", "model": "m1"}, {"engine": "agy", "model": "m2"}],
                    "switchEngineOn": ["startup_hang"],
                    "switchLimits": {"maxCandidates": 2, "maxSec": 60, "stopOnSameSymptom": true}
                }
            })";
            Check(BehaviorTree::ValidateJson(validLim, &catalog3, vErrs),
                  "101-5: valid switchLimits on defaults.worker and worker accepted");

            auto checkInvalidLim = [&](const std::string& limJson, const std::string& label) {
                std::string bad = std::string(R"({"id":"lim-bad","root":{"type":"worker","id":"w","summary":"s","prompt":"p","timeoutSec":30,"engines":[{"engine":"opencode","model":"m1"},{"engine":"agy","model":"m2"}],"switchEngineOn":["startup_hang"],"switchLimits":)") +
                                  limJson + R"(}})";
                std::vector<std::string> errs;
                Check(!BehaviorTree::ValidateJson(bad, &catalog3, errs),
                      "101-5: invalid switchLimits rejected: " + label);
            };
            checkInvalidLim("123", "non-object");
            checkInvalidLim(R"({"maxCandidates": 0})", "maxCandidates=0");
            checkInvalidLim(R"({"maxSec": -1})", "maxSec=-1");
            checkInvalidLim(R"({"stopOnSameSymptom": "yes"})", "stopOnSameSymptom=string");

            // 1) maxCandidates stops switching after 2 candidates and falls into catch "*" escalate.
            std::string maxCandBt = R"({
                "id": "bt-101-5-maxcand",
                "root": {
                    "type": "catch",
                    "child": {
                        "type": "worker", "id": "w", "summary": "s", "prompt": "p", "timeoutSec": 60,
                        "engines": [
                            {"engine": "opencode", "model": "m1"},
                            {"engine": "agy", "model": "m2"},
                            {"engine": "claude", "model": "m3"}
                        ],
                        "switchEngineOn": ["root_cause_not_found"],
                        "switchLimits": {"maxCandidates": 2}
                    },
                    "handlers": [{"on": "*", "do": {"type": "escalate", "id": "esc", "question": "Why?"}}]
                }
            })";
            FakeJobRunner rMc;
            BehaviorTree btMc;
            Check(btMc.LoadDefinition(maxCandBt, &catalog3, err), "101-5: load maxCandBt: " + err);
            btMc.SetJobRunner(&rMc);
            btMc.Start(1000);
            btMc.OnJobFinished("bt-101-5-maxcand-w-1", false, 1, "", "", 1100, "", "{}", "r1",
                               R"({"class":"root_cause_not_found","summary":"nf1"})");
            Check(rMc.startedWorkers.size() == 2 && rMc.startedWorkers[1].engine == "agy",
                  "101-5: maxCandidates=2 allows switch to 2nd candidate");
            btMc.OnJobFinished("bt-101-5-maxcand-w-2", false, 1, "", "", 1200, "", "{}", "r2",
                               R"({"class":"root_cause_not_found","summary":"nf2"})");
            Check(rMc.startedWorkers.size() == 2,
                  "101-5: maxCandidates=2 stops before 3rd candidate");
            bool hasMcEvent = false;
            for (const auto& ev : btMc.Events()) {
                if (ev.type == "engineSwitchStopped" && ev.detail == "max_candidates") hasMcEvent = true;
            }
            Check(hasMcEvent, "101-5: engineSwitchStopped event emitted with reason max_candidates");
            Check(btMc.State() == "waitingForLlm",
                  "101-5: maxCandidates stop falls through to catch '*' escalate");
            nlohmann::json mcStatus = nlohmann::json::parse(btMc.ToStateJson());
            Check(mcStatus["root"]["child"]["status"]["detail"].get<std::string>().find("max_candidates") != std::string::npos,
                  "101-5: worker failure detail records max_candidates");

            // 2) maxSec stops switching when elapsed time exceeds maxSec.
            std::string maxSecBt = R"({
                "id": "bt-101-5-maxsec",
                "root": {
                    "type": "catch",
                    "child": {
                        "type": "worker", "id": "w", "summary": "s", "prompt": "p", "timeoutSec": 60,
                        "engines": [
                            {"engine": "opencode", "model": "m1"},
                            {"engine": "agy", "model": "m2"},
                            {"engine": "claude", "model": "m3"}
                        ],
                        "switchEngineOn": ["root_cause_not_found"],
                        "switchLimits": {"maxCandidates": 3, "maxSec": 10}
                    },
                    "handlers": [{"on": "*", "do": {"type": "escalate", "id": "esc", "question": "Why?"}}]
                }
            })";
            FakeJobRunner rMs;
            BehaviorTree btMs;
            Check(btMs.LoadDefinition(maxSecBt, &catalog3, err), "101-5: load maxSecBt: " + err);
            btMs.SetJobRunner(&rMs);
            btMs.Start(1000);
            btMs.OnJobFinished("bt-101-5-maxsec-w-1", false, 1, "", "", 2000, "", "{}", "r1",
                               R"({"class":"root_cause_not_found","summary":"nf1"})");
            Check(rMs.startedWorkers.size() == 2 && rMs.startedWorkers[1].engine == "agy",
                  "101-5: within maxSec allows switch to 2nd candidate");
            btMs.OnJobFinished("bt-101-5-maxsec-w-2", false, 1, "", "", 12000, "", "{}", "r2",
                               R"({"class":"root_cause_not_found","summary":"nf2"})");
            Check(rMs.startedWorkers.size() == 2,
                  "101-5: exceeding maxSec stops before 3rd candidate");
            bool hasMsEvent = false;
            for (const auto& ev : btMs.Events()) {
                if (ev.type == "engineSwitchStopped" && ev.detail == "max_sec") hasMsEvent = true;
            }
            Check(hasMsEvent, "101-5: engineSwitchStopped event emitted with reason max_sec");
            Check(btMs.State() == "waitingForLlm",
                  "101-5: maxSec stop falls through to catch '*' escalate");

            // 3) stopOnSameSymptom stops on matching failSignature and populates escalate packet.
            std::string symBt = R"({
                "id": "bt-101-5-sym",
                "root": {
                    "type": "catch",
                    "child": {
                        "type": "retry", "max": 2,
                        "child": {
                            "type": "sequence",
                            "children": [
                                {
                                    "type": "worker", "id": "w", "summary": "s", "prompt": "p", "timeoutSec": 60,
                                    "engines": [
                                        {"engine": "opencode", "model": "m1"},
                                        {"engine": "agy", "model": "m2"},
                                        {"engine": "claude", "model": "m3"}
                                    ],
                                    "switchEngineOn": [
                                        {"class": "fix_failed", "after": 1},
                                        {"class": "fix_no_progress", "after": 1}
                                    ],
                                    "switchLimits": {"maxCandidates": 3, "stopOnSameSymptom": true}
                                },
                                {
                                    "type": "job", "id": "t", "summary": "test", "command": "ctest",
                                    "timeoutSec": 30, "blame": "w"
                                }
                            ]
                        }
                    },
                    "handlers": [{"on": "*", "do": {"type": "escalate", "id": "esc", "question": "Stuck?"}}]
                }
            })";
            FakeJobRunner rSym;
            BehaviorTree btSym;
            Check(btSym.LoadDefinition(symBt, &catalog3, err), "101-5: load symBt: " + err);
            btSym.SetJobRunner(&rSym);
            btSym.Start(3000);
            std::string ho1 = nlohmann::json({{"handoff", {{"suspected", "S1"}, {"tried", "T1"}, {"ruledOut", "R1"}}}}).dump();
            std::string ho2 = nlohmann::json({{"handoff", {{"suspected", "S2"}, {"tried", "T2"}, {"ruledOut", "R2"}}}}).dump();
            btSym.OnJobFinished("bt-101-5-sym-w-1", true, 0, "", "ok", 3100, "done", ho1);
            btSym.OnJobFinished("bt-101-5-sym-t-1", false, 1, "", "[FAIL] TestA\n[FAIL] TestB\n", 3200, "", "", "fail 1");
            Check(rSym.startedWorkers.size() == 2 && rSym.startedWorkers[1].engine == "agy",
                  "101-5: first blamed failure switches to 2nd candidate");
            btSym.OnJobFinished("bt-101-5-sym-w-2", true, 0, "", "ok", 3300, "done", ho2);
            btSym.OnJobFinished("bt-101-5-sym-t-2", false, 1, "", "[FAIL] TestA\n[FAIL] TestB\n", 3400, "", "", "fail 2");
            Check(rSym.startedWorkers.size() == 2,
                  "101-5: same symptom on 2nd candidate stops before 3rd candidate");
            bool hasSymEvent = false;
            for (const auto& ev : btSym.Events()) {
                if (ev.type == "engineSwitchStopped" && ev.detail == "same_symptom") hasSymEvent = true;
            }
            Check(hasSymEvent, "101-5: engineSwitchStopped event emitted with reason same_symptom");
            Check(btSym.State() == "waitingForLlm",
                  "101-5: same_symptom stop falls through to catch '*' escalate");

            BtEscalationPacket pkt = btSym.GetCurrentEscalationPacket();
            nlohmann::json pktJ = nlohmann::json::parse(pkt.json, nullptr, false);
            Check(pktJ.is_object() && pktJ.contains("engineAttempts") && pktJ["engineAttempts"].is_object(),
                  "101-5: escalate packet includes engineAttempts");
            if (pktJ.contains("engineAttempts") && pktJ["engineAttempts"].is_object()) {
                const auto& ea = pktJ["engineAttempts"];
                Check(ea.value("likelyNotModel", false) == true,
                      "101-5: escalate packet sets likelyNotModel=true on same_symptom");
                Check(ea.value("stoppedReason", "") == "same_symptom",
                      "101-5: escalate packet records stoppedReason");
                Check(ea.contains("commonFailedTests") &&
                      ea["commonFailedTests"] == nlohmann::json::array({"[FAIL] TestA", "[FAIL] TestB"}),
                      "101-5: escalate packet includes commonFailedTests intersection");
                bool sawHo1 = false, sawHo2 = false;
                if (ea.contains("attempts") && ea["attempts"].is_array()) {
                    for (const auto& a : ea["attempts"]) {
                        if (a.contains("handoff") && a["handoff"].value("suspected", "") == "S1") sawHo1 = true;
                        if (a.contains("handoff") && a["handoff"].value("suspected", "") == "S2") sawHo2 = true;
                    }
                }
                Check(sawHo1 && sawHo2, "101-5: escalate packet includes handoffs from candidates");
            }
        }
    }

    // 091-4: validate warnings, strict mode and embedded templates.
    {
        class LintCatalog : public bt_test::FakeEngineCatalog {
        public:
            std::set<std::string> usable;
            bool IsApprovedForList(const std::string& list, const std::string& engine,
                                   const std::string& model) const override {
                return usable.count(list + "/" + engine + "/" + model) != 0;
            }
        };
        LintCatalog lintCatalog;
        lintCatalog.usable.insert("worker/agy/m2");
        lintCatalog.usable.insert("worker/agy/m3");
        lintCatalog.usable.insert("worker/opencode/m1");
        FakeEngineCatalog allApproved;

        auto codes = [](const std::vector<BtWarning>& ws) {
            std::set<std::string> out;
            for (const auto& w : ws) out.insert(w.code);
            return out;
        };
        auto hasCode = [&](const std::vector<BtWarning>& ws, const std::string& code) {
            return codes(ws).count(code) != 0;
        };
        auto lint = [&](const std::string& def, std::vector<BtWarning>& ws) {
            std::vector<std::string> errs;
            bool ok = BehaviorTree::ValidateJson(def, &lintCatalog, errs, ws);
            Check(ok, "091-4: lint input is valid: " + (errs.empty() ? std::string("") : errs[0]));
            return ok;
        };
        auto pathFor = [](const std::vector<BtWarning>& ws, const std::string& code) {
            for (const auto& w : ws) if (w.code == code) return w.path;
            return std::string();
        };
        auto suggestionFor = [](const std::vector<BtWarning>& ws, const std::string& code) {
            for (const auto& w : ws) if (w.code == code) return w.suggestion;
            return std::string();
        };

        const char* bareWorker = R"JSON({"id":"lint-bare","root":{
            "type":"worker","id":"impl","summary":"s","prompt":"p","timeoutSec":60}})JSON";
        std::vector<BtWarning> ws;
        lint(bareWorker, ws);
        Check(hasCode(ws, "no_hang_watch"), "091-4: no_hang_watch on worker without idle/defaults");
        Check(hasCode(ws, "unguarded_worker"), "091-4: unguarded_worker on root worker");
        Check(!suggestionFor(ws, "no_hang_watch").empty(), "091-4: warning carries a suggestion");

        const char* guardedDefaults = R"JSON({"id":"lint-guarded","defaults":{
            "worker":{"idle":{"firstEventSec":180,"outputSec":600}}},
            "root":{"type":"catch","id":"guard","child":{"type":"worker","id":"impl",
            "summary":"s","prompt":"p","timeoutSec":60},"handlers":[{"do":{"type":"succeed"}}]}})JSON";
        ws.clear();
        lint(guardedDefaults, ws);
        Check(ws.empty(), "091-4: guarded worker with defaults has no warnings");

        const char* unguarded = R"JSON({"id":"lint-unguarded","defaults":{
            "worker":{"idle":{"firstEventSec":180}}},"root":{"type":"worker","id":"impl",
            "summary":"s","prompt":"p","timeoutSec":60}})JSON";
        ws.clear();
        lint(unguarded, ws);
        Check(hasCode(ws, "unguarded_worker") && !hasCode(ws, "no_hang_watch"),
              "091-4: unguarded_worker alone when idle defaults are present");

        const char* opencodeNoModel = R"JSON({"id":"lint-opencode","defaults":{
            "worker":{"idle":{"firstEventSec":180}}},"root":{"type":"catch","id":"guard",
            "child":{"type":"worker","id":"impl","summary":"s","prompt":"p","timeoutSec":60,
            "engine":"opencode"},"handlers":[{"do":{"type":"succeed"}}]}})JSON";
        ws.clear();
        lint(opencodeNoModel, ws);
        Check(hasCode(ws, "opencode_without_model"), "091-4: opencode_without_model when model is missing");

        const char* enginesNoSwitch = R"JSON({"id":"lint-engines","defaults":{
            "worker":{"idle":{"firstEventSec":180}}},"root":{"type":"catch","id":"guard",
            "child":{"type":"worker","id":"impl","summary":"s","prompt":"p","timeoutSec":60,
            "engines":[{"engine":"opencode","model":"m1"},{"engine":"agy","model":"m2"}]},
            "handlers":[{"do":{"type":"succeed"}}]}})JSON";
        ws.clear();
        lint(enginesNoSwitch, ws);
        Check(hasCode(ws, "engines_without_switch"), "091-4: engines_without_switch on two candidates");

        const char* switchNoEngines = R"JSON({"id":"lint-switch","defaults":{
            "worker":{"idle":{"firstEventSec":180}}},"root":{"type":"catch","id":"guard",
            "child":{"type":"worker","id":"impl","summary":"s","prompt":"p","timeoutSec":60,
            "engines":[{"engine":"opencode","model":"m1"}],
            "switchEngineOn":["startup_hang"]},"handlers":[{"do":{"type":"succeed"}}]}})JSON";
        ws.clear();
        lint(switchNoEngines, ws);
        Check(hasCode(ws, "switch_without_engines"), "091-4: switch_without_engines on one candidate");

        const char* notPermitted = R"JSON({"id":"lint-permit","defaults":{
            "worker":{"idle":{"firstEventSec":180}}},"root":{"type":"catch","id":"guard","child":{
            "type":"job","id":"llm","summary":"s","command":"echo m2","timeoutSec":60,
            "engine":"agy","model":"m9"},"handlers":[{"do":{"type":"succeed"}}]}})JSON";
        ws.clear();
        lint(notPermitted, ws);
        Check(hasCode(ws, "engine_not_permitted") && pathFor(ws, "engine_not_permitted") == "root.child.engine",
              "091-4: engine_not_permitted for a job engine outside the worker list");
        Check(!suggestionFor(ws, "engine_not_permitted").empty(),
              "091-4: engine_not_permitted suggests llm add/approve");

        const char* consultNotPermitted = R"JSON({"id":"lint-consult","root":{"type":"escalate",
            "id":"ask","question":"q","expect":["continue","stop","takeover"],
            "fallback":{"afterSec":60,"engine":"agy","model":"m9"}}})JSON";
        ws.clear();
        lint(consultNotPermitted, ws);
        Check(hasCode(ws, "engine_not_permitted") && pathFor(ws, "engine_not_permitted") == "root.fallback",
              "091-4: escalate fallback is checked against the consult list");

        const char* unswitchable = R"JSON({"id":"lint-cmd","defaults":{
            "worker":{"idle":{"firstEventSec":180}}},"root":{"type":"catch","id":"guard","child":{
            "type":"job","id":"card","summary":"s",
            "command":"ww-run.cmd 091 -Main -Engine agy -Model m2","timeoutSec":60},
            "handlers":[{"do":{"type":"succeed"}}]}})JSON";
        ws.clear();
        lint(unswitchable, ws);
        Check(hasCode(ws, "unswitchable_command"), "091-4: unswitchable_command without {engine}/{model}");

        const char* noTakeover = R"JSON({"id":"lint-takeover","root":{"type":"escalate",
            "id":"ask","question":"q","expect":["continue","stop"]}})JSON";
        ws.clear();
        lint(noTakeover, ws);
        Check(hasCode(ws, "escalate_without_takeover"), "091-4: escalate_without_takeover without takeover");
        ws.clear();
        lint(consultNotPermitted, ws);
        Check(!hasCode(ws, "escalate_without_takeover"),
              "091-4: no escalate warning when takeover is offered");

        std::string looseResp = BehaviorTree::ValidateResponse(unguarded, &lintCatalog, false);
        nlohmann::json looseJson = nlohmann::json::parse(looseResp, nullptr, false);
        Check(looseJson.value("ok", false) && looseJson.value("valid", false) &&
              looseJson["warnings"].is_array() && !looseJson["warnings"].empty(),
              "091-4: non-strict validate succeeds and returns warnings[]");
        std::string strictResp = BehaviorTree::ValidateResponse(unguarded, &lintCatalog, true);
        nlohmann::json strictJson = nlohmann::json::parse(strictResp, nullptr, false);
        Check(!strictJson.value("ok", true) && !strictJson.value("valid", true) &&
              strictJson.value("error", std::string()).find("unguarded_worker") != std::string::npos,
              "091-4: strict mode turns warnings into errors");
        std::string bareResp = BehaviorTree::ValidateResponse(guardedDefaults, &lintCatalog, true);
        Check(nlohmann::json::parse(bareResp, nullptr, false).value("ok", false),
              "091-4: strict mode passes a clean tree");

        // 091-7 (4): an unapproved `needs` is a warning; --strict makes it an error with the
        // permit_not_approved code and a permit request suggestion.
        const char* needsTree = R"JSON({"id":"lint-needs","defaults":{
            "worker":{"idle":{"firstEventSec":180,"outputSec":600}}},
            "root":{"type":"catch","id":"guard","child":{"type":"worker","id":"impl",
            "summary":"s","prompt":"p","timeoutSec":60,
            "needs":[{"kind":"read_path","target":"C:/outside/0917-never-approved/**"}]},
            "handlers":[{"do":{"type":"succeed"}}]}})JSON";
        ws.clear();
        lint(needsTree, ws);
        Check(hasCode(ws, "permit_not_approved"),
              "091-7: unapproved needs warns with permit_not_approved");
        Check(!suggestionFor(ws, "permit_not_approved").empty() &&
                  suggestionFor(ws, "permit_not_approved").find("permit request") != std::string::npos,
              "091-7: warning suggests `permit request`");
        nlohmann::json needsLoose = nlohmann::json::parse(
            BehaviorTree::ValidateResponse(needsTree, &lintCatalog, false), nullptr, false);
        Check(needsLoose.value("ok", false), "091-7: non-strict validate still succeeds");
        nlohmann::json needsStrict = nlohmann::json::parse(
            BehaviorTree::ValidateResponse(needsTree, &lintCatalog, true), nullptr, false);
        Check(!needsStrict.value("ok", true) &&
                  needsStrict.value("errorCodes", nlohmann::json::array()).size() > 0 &&
                  needsStrict["errorCodes"][0] == "permit_not_approved",
              "091-7: --strict reports error code permit_not_approved");

        auto fillPlaceholders = [](std::string text) {
            std::map<std::string, std::string> values;
            size_t pos = 0;
            while ((pos = text.find('<', pos)) != std::string::npos) {
                size_t end = text.find('>', pos + 1);
                if (end == std::string::npos) break;
                std::string key = text.substr(pos + 1, end - pos - 1);
                if (!values.count(key)) values[key] = "v" + std::to_string(values.size() + 1);
                text.replace(pos, end - pos + 1, values[key]);
                pos += values[key].size();
            }
            return text;
        };
        nlohmann::json list = BtTemplateListJson();
        Check(list.value("ok", false) && list["templates"].is_array() &&
              list["templates"].size() == 3 &&
              list.value("note", std::string()).find("<...>") != std::string::npos,
              "091-4/093-2: template list has three entries and a placeholder note");
        Check(BtTemplateDefinition("does-not-exist").is_null() &&
              !BtTemplateResult("does-not-exist").value("ok", true),
              "091-4: unknown template is rejected");
        for (const auto& entry : BtTemplateEntries()) {
            nlohmann::json def = BtTemplateDefinition(entry.name);
            Check(def.is_object() && def.value("id", std::string()) == entry.name,
                  "091-4: template " + entry.name + " returns a full BT object");
            nlohmann::json tool = BtTemplateResult(entry.name);
            Check(tool.value("ok", false) && tool["bt"].is_object() &&
                  tool.value("note", std::string()).find("bt validate") != std::string::npos,
                  "091-4: MCP template result includes note and bt for " + entry.name);
            std::string text = fillPlaceholders(def.dump());
            Check(text.find('<') == std::string::npos, "091-4: all placeholders replaced in " + entry.name);
            std::string resp = BehaviorTree::ValidateResponse(text, &allApproved, true);
            nlohmann::json j = nlohmann::json::parse(resp, nullptr, false);
            Check(j.value("ok", false) && j.value("valid", false) &&
                  j["warnings"].is_array() && j["warnings"].empty(),
                  "091-4: " + entry.name + " passes bt validate --strict with no warnings: " +
                  j.value("error", std::string()));
        }

        // 101-6: fix_class_without_blame warning & --strict error.
        {
            const char* fixWithoutBlameBt = R"JSON({
                "id": "fix-no-blame",
                "defaults": {"worker": {"idle": {"firstEventSec": 60}}},
                "root": {
                    "type": "catch",
                    "child": {
                        "type": "worker",
                        "id": "impl",
                        "summary": "fix",
                        "prompt": "fix it",
                        "timeoutSec": 60,
                        "engines": [
                            {"engine": "opencode", "model": "m1"},
                            {"engine": "agy", "model": "m2"}
                        ],
                        "switchEngineOn": [{"class": "fix_no_progress", "after": 1}]
                    },
                    "handlers": [{"class": "*", "do": {"type": "fail", "detail": "done"}}]
                }
            })JSON";
            std::vector<std::string> fwErrs;
            std::vector<BtWarning> fwWarns;
            Check(BehaviorTree::ValidateJson(fixWithoutBlameBt, &allApproved, fwErrs, fwWarns),
                  "101-6: fix_class_without_blame is a warning, not a parse error");
            bool hasFixWarn = false;
            for (const auto& w : fwWarns) {
                if (w.code == "fix_class_without_blame") hasFixWarn = true;
            }
            Check(hasFixWarn, "101-6: worker with fix_* in switchEngineOn without blame emits fix_class_without_blame");

            nlohmann::json strictResp = nlohmann::json::parse(
                BehaviorTree::ValidateResponse(fixWithoutBlameBt, &allApproved, true), nullptr, false);
            Check(!strictResp.value("ok", true) && !strictResp.value("valid", true) &&
                  strictResp.value("error", std::string()).find("fix_class_without_blame") != std::string::npos,
                  "101-6: bt validate --strict promotes fix_class_without_blame to error");

            const char* fixWithBlameBt = R"JSON({
                "id": "fix-with-blame",
                "defaults": {"worker": {"idle": {"firstEventSec": 60}}},
                "root": {
                    "type": "catch",
                    "child": {
                        "type": "sequence",
                        "children": [
                            {
                                "type": "worker",
                                "id": "impl",
                                "summary": "fix",
                                "prompt": "fix it",
                                "timeoutSec": 60,
                                "engines": [
                                    {"engine": "opencode", "model": "m1"},
                                    {"engine": "agy", "model": "m2"}
                                ],
                                "switchEngineOn": [{"class": "fix_no_progress", "after": 1}]
                            },
                            {
                                "type": "job",
                                "id": "test",
                                "summary": "run tests",
                                "command": "ctest",
                                "timeoutSec": 60,
                                "blame": "impl"
                            }
                        ]
                    },
                    "handlers": [{"class": "*", "do": {"type": "fail", "detail": "done"}}]
                }
            })JSON";
            std::vector<std::string> okErrs;
            std::vector<BtWarning> okWarns;
            Check(BehaviorTree::ValidateJson(fixWithBlameBt, &allApproved, okErrs, okWarns),
                  "101-6: worker with fix_* and blaming job validates");
            bool hasUnexpectedWarn = false;
            for (const auto& w : okWarns) {
                if (w.code == "fix_class_without_blame") hasUnexpectedWarn = true;
            }
            Check(!hasUnexpectedWarn, "101-6: no fix_class_without_blame when a job blames the worker");
        }
    }

    // 091-4: existing checked-in plans must stay valid (warnings are allowed).
    {
#ifdef PC_REPO_DIR
        std::wstring dir = Utf8ToWide(PC_REPO_DIR) + L"\\ww-data\\bt";
        WIN32_FIND_DATAW fd = {};
        HANDLE h = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
        int checked = 0;
        if (h != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring name = fd.cFileName;
                if (name == L"." || name == L"..") continue;
                std::wstring pathW = dir + L"\\" + name;
                std::ifstream ifs(pathW.c_str(), std::ios::binary);
                if (!ifs) continue;
                std::ostringstream ss;
                ss << ifs.rdbuf();
                std::vector<std::string> errs;
                std::vector<BtWarning> warnings;
                bool ok = BehaviorTree::ValidateJson(ss.str(), &catalog, errs, warnings);
                Check(ok, "091-4: existing BT " + WideToUtf8(name) + " still validates without strict: " +
                          (errs.empty() ? std::string("") : errs[0]));
                checked++;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        Check(checked > 0, "091-4: found existing BT JSON files to re-validate");
#endif
    }

    // 091-6: shared engine health (down candidates are skipped, held nodes recover).
    {
        class FakeHealth : public IBtEngineHealth {
        public:
            std::set<std::string> down;
            int64_t nextProbeAt = 0;
            bool IsDown(const std::string& engine, const std::string& model) const override {
                return down.count(engine + "/" + model) != 0;
            }
            void MarkDown(const std::string& engine, const std::string& model,
                          const std::string&, const std::string&, int64_t) override {
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

        class HealthCatalog : public bt_test::FakeEngineCatalog {
        public:
            mutable std::set<std::string> approved;
            bool IsApprovedForList(const std::string&, const std::string& engine,
                                   const std::string& model) const override {
                return approved.count(engine + "/" + model) != 0;
            }
        };
        HealthCatalog catalogAll;
        catalogAll.approved.insert("opencode/m1");
        catalogAll.approved.insert("agy/m2");
        const char* twoCandidates = R"JSON({"id":"eh-two","root":{
            "type":"worker","id":"impl","prompt":"do work","summary":"health test","timeoutSec":60,
            "engines":[{"engine":"opencode","model":"m1"},{"engine":"agy","model":"m2"}],
            "switchEngineOn":["startup_hang"]}})JSON";

        FakeHealth skipHealth;
        skipHealth.down.insert("opencode/m1");
        FakeJobRunner skipRunner;
        BehaviorTree skipTree;
        std::string err;
        Check(skipTree.LoadDefinition(twoCandidates, &catalogAll, err), "0916: load two-candidate BT: " + err);
        skipTree.SetJobRunner(&skipRunner);
        skipTree.SetEngineHealth(&skipHealth);
        skipTree.Start(1000);
        Check(skipRunner.startedWorkers.size() == 1 && skipRunner.startedWorkers[0].engine == "agy" &&
              skipRunner.startedWorkers[0].model == "m2", "0916: down candidate skipped, next used");

        const char* singleEngine = R"JSON({"id":"eh-single","root":{
            "type":"worker","id":"impl","prompt":"do work","summary":"held test","timeoutSec":60,
            "engine":"opencode","model":"m1"}})JSON";
        FakeHealth holdHealth;
        holdHealth.down.insert("opencode/m1");
        holdHealth.nextProbeAt = 2500;
        FakeJobRunner holdRunner;
        BehaviorTree holdTree;
        Check(holdTree.LoadDefinition(singleEngine, &catalogAll, err), "0916: load single-engine BT: " + err);
        holdTree.SetJobRunner(&holdRunner);
        holdTree.SetEngineHealth(&holdHealth);
        holdTree.Start(1000);
        Check(holdRunner.startedWorkers.empty() && holdTree.State() == "running",
              "0916: single down engine holds the worker instead of failing it");
        bool sawHold = false;
        for (const auto& ev : holdTree.Events()) if (ev.type == "nodeHeldForEngine") sawHold = true;
        Check(sawHold, "0916: nodeHeldForEngine event emitted");
        Check(holdTree.ToStateJson().find("waitingForEngine") != std::string::npos,
              "0916: held state is persisted");
        holdTree.OnTimer(2400);
        Check(holdRunner.startedWorkers.empty(), "0916: still held before nextProbeAt");
        holdHealth.down.clear();
        holdTree.OnTimer(2600);
        bool sawRecovered = false;
        for (const auto& ev : holdTree.Events()) if (ev.type == "engineRecovered") sawRecovered = true;
        Check(sawRecovered, "0916: engineRecovered event emitted after recovery");
        holdTree.Tick(2700);
        Check(holdRunner.startedWorkers.size() == 1 &&
              holdRunner.startedWorkers[0].engine == "opencode", "0916: held worker starts after recovery");

        FakeHealth markHealth;
        FakeJobRunner markRunner;
        BehaviorTree markTree;
        Check(markTree.LoadDefinition(singleEngine, &catalogAll, err), "0916: load mark BT: " + err);
        markTree.SetJobRunner(&markRunner);
        markTree.SetEngineHealth(&markHealth);
        markTree.Start(1000);
        markTree.OnJobFinished("eh-single-impl-1", false, -1, "", "", 1100, "", "",
                               "timedOut", R"({"class":"startup_hang","summary":"no events"})");
        Check(markHealth.IsDown("opencode", "m1"),
              "0916: a startup_hang failure marks the engine down for other jobs");
    }

    RunPart2Tests(tempDirUtf8, tempDirW);

    RemoveTempDirRecursive(tempDirW);
    std::printf("%s (%d passed, %d failures)\n", g_failures ? "FAILED" : "ALL PASSED", g_passes, g_failures);
    return g_failures ? 1 : 0;
}
