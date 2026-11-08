// Part 2 of headless smoke tests for the Behavior Tree core engine.

#include "behavior_tree_smoke_common.h"

namespace bt_test {

void RunPart2Tests(const std::string& tempDirUtf8, const std::wstring& tempDirW) {
    FakeEngineCatalog catalog;

    // 6. All condition types: fileExists, fileNewer, outputMatches, exitCode, jsonValue, var, nodeState
    {
        WriteFileAtomicWide(tempDirW + L"\\old.txt", "old");
        WriteFileAtomicWide(tempDirW + L"\\res.json", R"({"status":"ok","score":95,"items":["a","b"]})");

        std::string condBt = R"({
            "id": "cond-test",
            "cwd": ")" + JsonEscape(tempDirUtf8) + R"(",
            "vars": { "num": "10", "tag": "alpha" },
            "root": {
                "type": "sequence",
                "children": [
                    { "type": "job", "id": "j1", "summary": "Run j1", "command": "cmd1", "timeoutSec": 30 },
                    { "type": "fileExists", "id": "c_exists", "path": "*.json" },
                    { "type": "fileNewer", "id": "c_newer_start", "path": "after_j1.txt", "than": "${j1.start}" },
                    { "type": "fileNewer", "id": "c_newer_file", "path": "after_j1.txt", "than": "old.txt" },
                    { "type": "outputMatches", "id": "c_out", "node": "j1", "regex": "PASS_TOKEN_\\d+" },
                    { "type": "exitCode", "id": "c_exit_eq", "node": "j1", "eq": 0 },
                    { "type": "exitCode", "id": "c_exit_in", "node": "j1", "in": [0, 2] },
                    { "type": "jsonValue", "id": "c_json_eq", "file": "res.json", "path": "status", "eq": "ok" },
                    { "type": "jsonValue", "id": "c_json_gt", "file": "res.json", "path": "score", "gt": 90, "lt": 100 },
                    { "type": "jsonValue", "id": "c_json_in", "file": "res.json", "path": "items[1]", "in": ["b", "c"] },
                    { "type": "jsonValue", "id": "c_json_exists", "file": "res.json", "path": "missing_key", "exists": false },
                    { "type": "var", "id": "c_var_gt", "name": "num", "gt": 5, "ne": "99" },
                    { "type": "var", "id": "c_var_in", "name": "tag", "in": ["alpha", "beta"] },
                    { "type": "nodeState", "id": "c_nstate", "node": "j1", "eq": "succeeded" }
                ]
            }
        })";

        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(condBt, &catalog, err), "load condition BT: " + err);
        bt.SetJobRunner(&runner);
        bt.Start(1000);

        // Write after_j1.txt while j1 is running so its mtime is strictly newer than old.txt and j1.start
        WriteFileAtomicWide(tempDirW + L"\\after_j1.txt", "newer content");

        bt.OnJobFinished("cond-test-1", true, 0, "", "Line 1\nPASS_TOKEN_42\nDone", 1500);
        Check(bt.State() == "succeeded", "all 7 condition types succeeded: " + bt.Detail());
        Check(bt.NodeState("c_exists") == "succeeded" &&
              bt.NodeState("c_newer_start") == "succeeded" &&
              bt.NodeState("c_newer_file") == "succeeded" &&
              bt.NodeState("c_out") == "succeeded" &&
              bt.NodeState("c_exit_eq") == "succeeded" &&
              bt.NodeState("c_exit_in") == "succeeded" &&
              bt.NodeState("c_json_eq") == "succeeded" &&
              bt.NodeState("c_json_gt") == "succeeded" &&
              bt.NodeState("c_json_in") == "succeeded" &&
              bt.NodeState("c_json_exists") == "succeeded" &&
              bt.NodeState("c_var_gt") == "succeeded" &&
              bt.NodeState("c_var_in") == "succeeded" &&
              bt.NodeState("c_nstate") == "succeeded",
              "individual condition node states are succeeded");
    }

    // 7. Escalate: packet, bad answer, missing rationale, fallback launch + cancel on Resume, timeout, fallback completion
    {
        WriteFileAtomicWide(tempDirW + L"\\summary.txt", "Build summary text");

        std::string escBt = R"({
            "id": "esc-test",
            "intent": "Ship feature safely",
            "cwd": ")" + JsonEscape(tempDirUtf8) + R"(",
            "vars": { "env": "staging" },
            "root": {
                "type": "sequence",
                "children": [
                    { "type": "job", "id": "prep", "summary": "Prep", "command": "prep", "timeoutSec": 30 },
                    {
                        "type": "escalate",
                        "id": "ask",
                        "question": "Deploy to ${env}?",
                        "guidance": "Check summary.txt first",
                        "expect": ["approve", "reject"],
                        "include": { "logs": ["prep"], "vars": ["env"], "files": ["summary.txt"] },
                        "timeoutSec": 100,
                        "fallback": { "afterSec": 20, "engine": "claude", "model": "claude-opus-5-5" }
                    },
                    {
                        "type": "if",
                        "cond": { "type": "var", "name": "ask.answer", "eq": "approve" },
                        "then": { "type": "job", "id": "deploy", "summary": "Deploy ${extra}", "command": "deploy", "timeoutSec": 30 },
                        "else": { "type": "fail", "detail": "rejected" }
                    }
                ]
            }
        })";

        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(escBt, &catalog, err), "load escalate BT: " + err);
        bt.SetJobRunner(&runner);
        bt.Start(1000);
        bt.OnJobFinished("esc-test-1", true, 0, "", "prep log line 1", 1500);

        Check(bt.State() == "waitingForLlm", "BT enters waitingForLlm at escalate");
        std::string pktJson = bt.GetEscalationPacketJson("ask");
        std::string pktMd = bt.GetEscalationPacketMarkdown("ask");
        Check(pktJson.find("Deploy to staging?") != std::string::npos &&
              pktJson.find("prep log line 1") != std::string::npos &&
              pktJson.find("Build summary text") != std::string::npos,
              "escalation packet JSON includes question, log tail, and file snapshot");
        Check(pktMd.find("Ship feature safely") != std::string::npos &&
              pktMd.find("bt_resume") != std::string::npos,
              "escalation packet Markdown includes intent and how-to-answer");

        // Reject answer not in expect
        bool okAns = bt.Resume("ask", "maybe", "unsure", {}, 2000, err);
        Check(!okAns && !err.empty(), "Resume rejects answer not in expect: " + err);

        // Reject empty rationale
        bool okRat = bt.Resume("ask", "approve", "   ", {}, 2000, err);
        Check(!okRat && !err.empty(), "Resume rejects empty rationale: " + err);

        // Trigger fallback at +20s (t = 1500 + 20000 = 21500)
        Check(bt.NextWakeMs() == 21500, "NextWakeMs is fallback deadline 21500");
        bt.OnTimer(21500);
        Check(runner.startedWorkers.size() == 1 && runner.startedWorkers[0].engine == "claude",
              "fallback worker started at afterSec via StartWorker");

        // Resume arrives before fallback finishes -> cancels fallback worker and continues
        bool okRes = bt.Resume("ask", "approve", "All checks green", {{"extra", "prod"}}, 22000, err);
        Check(okRes, "Resume succeeds with valid answer and rationale: " + err);
        Check(runner.WasCancelled("esc-test-2"), "Resume cancelled running fallback worker");
        Check(bt.GetVar("ask.answer") == "approve", "ask.answer variable set");
        Check(runner.startedJobs.size() == 2 && runner.startedJobs[1].summary == "Deploy prod",
              "deploy job started with extra var from Resume");
        bt.OnJobFinished("esc-test-3", true, 0, "", "deployed", 23000);
        Check(bt.State() == "succeeded", "escalate BT succeeded after deploy");

        // Escalate timeout test
        FakeJobRunner runnerTo;
        BehaviorTree btTo;
        btTo.LoadDefinition(escBt, &catalog, err);
        btTo.SetJobRunner(&runnerTo);
        btTo.Start(1000);
        btTo.OnJobFinished("esc-test-1", true, 0, "", "ok", 1500);
        btTo.OnTimer(101500); // past timeoutSec=100
        Check(btTo.State() == "failed" && btTo.NodeDetail("ask") == "escalationTimeout",
              "escalate fails with escalationTimeout when timeoutSec expires");

        // Fallback worker completing on its own applies {answer, rationale} like Resume
        FakeJobRunner runnerFb;
        BehaviorTree btFb;
        btFb.LoadDefinition(escBt, &catalog, err);
        btFb.SetJobRunner(&runnerFb);
        btFb.Start(1000);
        btFb.OnJobFinished("esc-test-1", true, 0, "", "ok", 1500);
        btFb.OnTimer(21500); // starts fallback worker esc-test-2
        btFb.OnJobFinished("esc-test-2", true, 0, "", "", 22000, "Fallback decided",
                           R"({"answer":"approve","rationale":"Fallback approved","vars":{"extra":"auto"}})");
        Check(btFb.GetVar("ask.answer") == "approve" && runnerFb.startedJobs.size() == 2 &&
              runnerFb.startedJobs[1].summary == "Deploy auto",
              "fallback worker result file applied like Resume");
    }

    // 8. Limits (maxNodeRuns) and validation errors (missing max, duplicate id, depth, subtree)
    {
        std::string limBt = R"({
            "id": "lim-test",
            "limits": { "maxNodeRuns": 2 },
            "root": {
                "type": "repeat",
                "max": 5,
                "child": { "type": "succeed" }
            }
        })";
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(limBt, &catalog, err), "load maxNodeRuns BT");
        bt.Start(1000);
        Check(bt.State() == "failed" && bt.Detail() == "limitExceeded" && bt.NodeRuns() == 2,
              "maxNodeRuns exceeded fails BT with limitExceeded");

        // Validation errors with JSON path
        std::string noMax = R"({"id":"x","root":{"type":"retry","child":{"type":"succeed"}}})";
        bool vNoMax = BehaviorTree::ValidateJson(noMax, &catalog, err);
        Check(!vNoMax && err.find("root") != std::string::npos,
              "validation catches missing max on retry: " + err);

        std::string dupId = R"({
            "id": "x",
            "root": {
                "type": "sequence",
                "children": [
                    { "type": "succeed", "id": "same" },
                    { "type": "succeed", "id": "same" }
                ]
            }
        })";
        bool vDup = BehaviorTree::ValidateJson(dupId, &catalog, err);
        Check(!vDup && err.find("root.children[1]") != std::string::npos &&
              err.find("duplicate") != std::string::npos,
              "validation catches duplicate id with path: " + err);

        std::string subBt = R"({"id":"x","root":{"type":"subtree","file":"other.json"}})";
        bool vSub = BehaviorTree::ValidateJson(subBt, &catalog, err);
        Check(!vSub && err.find("not supported yet") != std::string::npos,
              "validation rejects subtree with clear message: " + err);

        std::string fbCmdBt = R"({"id":"x","root":{"type":"escalate","question":"q?","fallback":{"afterSec":10,"command":"foo.exe"}}})";
        bool vFbCmd = BehaviorTree::ValidateJson(fbCmdBt, &catalog, err);
        Check(!vFbCmd && err.find("fallback.command is not supported; use engine/model") != std::string::npos,
              "validation rejects fallback.command: " + err);

        // Build a 34-deep tree -> exceeds depth 32
        std::string deep = R"({"type":"succeed"})";
        for (int i = 0; i < 33; ++i) {
            deep = R"({"type":"invert","child":)" + deep + "}";
        }
        std::string deepBt = R"({"id":"deep","root":)" + deep + "}";
        bool vDeep = BehaviorTree::ValidateJson(deepBt, &catalog, err);
        Check(!vDeep && err.find("depth") != std::string::npos,
              "validation catches depth > 32: " + err);
    }

    // 9. Retry from failed node + Save (ToStateJson) -> Load (LoadFromStateJson) -> Continue + ParseBtDebugTree
    {
        std::string persistBt = R"({
            "id": "feature-persist",
            "summary": "Persist and retry test",
            "intent": "Verify save/load and retry",
            "vars": { "target": "Release" },
            "root": {
                "type": "sequence",
                "children": [
                    {
                        "type": "retry",
                        "id": "build-loop",
                        "max": 2,
                        "child": {
                            "type": "job",
                            "id": "build",
                            "summary": "Build ${target}",
                            "command": "cmake --build build",
                            "timeoutSec": 60
                        }
                    },
                    {
                        "type": "job",
                        "id": "test",
                        "summary": "Run tests",
                        "command": "ctest",
                        "timeoutSec": 60
                    }
                ]
            }
        })";

        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(persistBt, &catalog, err), "load persistBt");
        bt.SetJobRunner(&runner);
        bt.Start(1000);

        // Fail attempt 1, attempt 2 starts and is now running
        bt.OnJobFinished("feature-persist-1", false, 1, "", "err", 1200);
        Check(bt.State() == "running" && bt.NodeRunCount("build") == 2, "build attempt 2 is running");

        // Save state while feature-persist-2 is running
        std::string savedJson = bt.ToStateJson();

        // Verify savedJson loads in ParseBtDebugTree (src/bt_debug_view.h)
        BtDebugNode dbg;
        std::string dbgErr;
        Check(ParseBtDebugTree(savedJson, dbg, dbgErr), "ToStateJson parses in ParseBtDebugTree: " + dbgErr);
        Check(dbg.btId == "feature-persist" && dbg.state == "running", "debug tree has btId and running state");
        Check(FindBtNodeKeyByJob({dbg}, "feature-persist-2") == "feature-persist/build",
              "FindBtNodeKeyByJob finds running job on reloaded debug tree");

        // Restore into a brand new BehaviorTree instance and continue ticking!
        BehaviorTree restored;
        Check(restored.LoadFromStateJson(savedJson, &catalog, 1300, err), "LoadFromStateJson succeeded: " + err);
        restored.SetJobRunner(&runner);
        Check(restored.State() == "running" && restored.NodeState("build") == "running",
              "restored tree has build running");

        // Finish the attached job feature-persist-2 with failure -> retry(max=2) exhausts -> tree fails!
        restored.OnJobFinished("feature-persist-2", false, 2, "", "err2", 1500);
        Check(restored.State() == "failed" && restored.NodeState("build-loop") == "failed",
              "restored tree failed when second build attempt failed");
        Check(restored.NodeState("test") == "idle", "unreached 'test' node remained idle");

        // Verify unreached 'test' node has no status in ToStateJson
        std::string failedStateJson = restored.ToStateJson();
        Check(ParseBtDebugTree(failedStateJson, dbg, dbgErr), "failed ToStateJson parses in ParseBtDebugTree");

        // Now Retry from failed node "build" with resetCounters=true
        Check(restored.Retry("build", true, 2000, err), "Retry from failed node 'build': " + err);
        Check(restored.State() == "running" && runner.startedJobs.size() == 3 &&
              runner.startedJobs.back().proposedJobId == "feature-persist-3",
              "Retry launched job feature-persist-3");

        restored.OnJobFinished("feature-persist-3", true, 0, "", "built!", 2500);
        Check(restored.NodeState("test") == "running" && runner.startedJobs.size() == 4,
              "after build succeeded, 'test' job feature-persist-4 started");

        restored.OnJobFinished("feature-persist-4", true, 0, "", "all tests passed", 3000);
        Check(restored.State() == "succeeded", "tree succeeded after Retry");
        Check(!restored.Events().empty() && restored.Version() > 0, "events and version counter populated");

        // Also test loading sample tests/data/bt/feature-x.json and resuming it
        std::wstring samplePath = Utf8ToWide(PC_REPO_DIR) + L"\\tests\\data\\bt\\feature-x.json";
        std::string sampleJson = ReadFileUtf8Wide(samplePath);
        BehaviorTree sampleBt;
        bool loadedSample = sampleBt.LoadFromStateJson(sampleJson, &catalog, 5000, err);
        Check(loadedSample && sampleBt.State() == "waitingForLlm", "loaded tests/data/bt/feature-x.json in waitingForLlm");
        bool resumedSample = sampleBt.Resume("review", "approve", "Approved in smoke test", {}, 6000, err);
        Check(resumedSample && sampleBt.State() == "succeeded", "resuming sample feature-x.json completes the tree");
        BtDebugNode sampleDbg;
        Check(ParseBtDebugTree(sampleBt.ToStateJson(), sampleDbg, dbgErr) && sampleDbg.state == "succeeded",
              "resumed feature-x.json ToStateJson loads in ParseBtDebugTree");
    }
}

} // namespace bt_test
