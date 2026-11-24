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

        // Write after_j1.txt while j1 is running so its mtime is strictly newer than old.txt and j1.start.
        // Windows file timestamps can share a tick; wait one tick before writing the newer file.
        Sleep(50);
        WriteFileAtomicWide(tempDirW + L"\\after_j1.txt", "newer content");

        bt.OnJobFinished("cond-test-j1-1", true, 0, "", "Line 1\nPASS_TOKEN_42\nDone", 1500);
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
        bt.OnJobFinished("esc-test-prep-1", true, 0, "", "prep log line 1", 1500);

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
        Check(runner.WasCancelled("esc-test-ask-1"), "Resume cancelled running fallback worker");
        Check(bt.GetVar("ask.answer") == "approve", "ask.answer variable set");
        Check(runner.startedJobs.size() == 2 && runner.startedJobs[1].summary == "Deploy prod",
              "deploy job started with extra var from Resume");
        bt.OnJobFinished("esc-test-deploy-1", true, 0, "", "deployed", 23000);
        Check(bt.State() == "succeeded", "escalate BT succeeded after deploy");

        // Escalate timeout test
        FakeJobRunner runnerTo;
        BehaviorTree btTo;
        btTo.LoadDefinition(escBt, &catalog, err);
        btTo.SetJobRunner(&runnerTo);
        btTo.Start(1000);
        btTo.OnJobFinished("esc-test-prep-1", true, 0, "", "ok", 1500);
        btTo.OnTimer(101500); // past timeoutSec=100
        Check(btTo.State() == "failed" && btTo.NodeDetail("ask") == "escalationTimeout",
              "escalate fails with escalationTimeout when timeoutSec expires");

        // Fallback worker completing on its own applies {answer, rationale} like Resume
        FakeJobRunner runnerFb;
        BehaviorTree btFb;
        btFb.LoadDefinition(escBt, &catalog, err);
        btFb.SetJobRunner(&runnerFb);
        btFb.Start(1000);
        btFb.OnJobFinished("esc-test-prep-1", true, 0, "", "ok", 1500);
        btFb.OnTimer(21500); // starts fallback worker esc-test-2
        btFb.OnJobFinished("esc-test-ask-1", true, 0, "", "", 22000, "Fallback decided",
                           R"({"answer":"approve","rationale":"Fallback approved","vars":{"extra":"auto"}})");
        Check(btFb.GetVar("ask.answer") == "approve" && runnerFb.startedJobs.size() == 2 &&
              runnerFb.startedJobs[1].summary == "Deploy auto",
              "fallback worker result file applied like Resume");
    }

    // 7b. Escalate answer "takeover" is accepted even without expect, and is recorded.
    {
        std::string takeoverBt = R"({
            "id": "takeover-test",
            "cwd": ")" + JsonEscape(tempDirUtf8) + R"(",
            "root": {
                "type": "sequence",
                "children": [
                    { "type": "escalate", "id": "ask", "question": "Delegate again?",
                      "expect": ["approve", "reject"] }
                ]
            }
        })";

        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(takeoverBt, &catalog, err), "load takeover BT: " + err);
        bt.SetJobRunner(&runner);
        bt.Start(1000);
        Check(bt.State() == "waitingForLlm", "takeover BT reaches waitingForLlm");

        bool okTakeover = bt.Resume("ask", "takeover", "I will implement this myself", {}, 2000, err);
        Check(okTakeover && err.empty(), "takeover answer accepted without being in expect: " + err);
        Check(bt.NodeState("ask") == "succeeded", "escalate node itself is succeeded after takeover");
        Check(bt.State() == "succeeded", "takeover BT succeeded after answering takeover");
        std::string stateJson = bt.ToStateJson();
        std::string noWs;
        for (char c : stateJson) {
            if (c != ' ' && c != '\n' && c != '\r' && c != '\t') noWs.push_back(c);
        }
        Check(noWs.find("\"takenOver\":true") != std::string::npos &&
              noWs.find("\"takeover\":{") != std::string::npos &&
              stateJson.find("I will implement this myself") != std::string::npos,
              "BT state records takenOver and the takeover rationale");
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

    // 100-2b: the BT's taskId is read from its definition, saved, and still there after a reload.
    {
        std::string taskBt = R"({
            "id": "feature-task",
            "taskId": "task_bt1",
            "root": { "type": "job", "id": "only", "summary": "Only job", "command": "cmd /c exit 0", "timeoutSec": 60 }
        })";
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(taskBt, &catalog, err), "100-2b: load BT with taskId: " + err);
        Check(bt.TaskId() == "task_bt1", "100-2b: taskId is read from the definition");
        std::string saved = bt.ToStateJson();
        // ToStateJson pretty-prints (dump(2)), so accept either "key":"v" or "key": "v".
        Check(saved.find("\"taskId\":\"task_bt1\"") != std::string::npos ||
              saved.find("\"taskId\": \"task_bt1\"") != std::string::npos,
              "100-2b: taskId is saved in the state JSON");
        BehaviorTree restored;
        Check(restored.LoadFromStateJson(saved, &catalog, err) && restored.TaskId() == "task_bt1",
              "100-2b: taskId survives a reload from the state JSON: " + err);
        BehaviorTree plain;
        Check(plain.LoadDefinition(R"({"id":"feature-plain","root":{"type":"job","id":"p","summary":"p","command":"true","timeoutSec":60}})",
                                   &catalog, err) && plain.TaskId().empty(),
              "100-2b: a BT without taskId has an empty TaskId");
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
        bt.OnJobFinished("feature-persist-build-1", false, 1, "", "err", 1200);
        Check(bt.State() == "running" && bt.NodeRunCount("build") == 2, "build attempt 2 is running");

        // Save state while feature-persist-2 is running
        std::string savedJson = bt.ToStateJson();

        // Verify savedJson loads in ParseBtDebugTree (src/bt_debug_view.h)
        BtDebugNode dbg;
        std::string dbgErr;
        Check(ParseBtDebugTree(savedJson, dbg, dbgErr), "ToStateJson parses in ParseBtDebugTree: " + dbgErr);
        Check(dbg.btId == "feature-persist" && dbg.state == "running", "debug tree has btId and running state");
        Check(FindBtNodeKeyByJob({dbg}, "feature-persist-build-2") == "feature-persist/build",
              "FindBtNodeKeyByJob finds running job on reloaded debug tree");

        // Restore into a brand new BehaviorTree instance and continue ticking!
        BehaviorTree restored;
        Check(restored.LoadFromStateJson(savedJson, &catalog, 1300, err), "LoadFromStateJson succeeded: " + err);
        restored.SetJobRunner(&runner);
        Check(restored.State() == "running" && restored.NodeState("build") == "running",
              "restored tree has build running");

        // Finish the attached job feature-persist-2 with failure -> retry(max=2) exhausts -> tree fails!
        restored.OnJobFinished("feature-persist-build-2", false, 2, "", "err2", 1500);
        Check(restored.State() == "failed" && restored.NodeState("build-loop") == "failed",
              "restored tree failed when second build attempt failed");
        Check(restored.NodeState("test") == "idle", "unreached 'test' node remained idle");

        // Verify unreached 'test' node has no status in ToStateJson
        std::string failedStateJson = restored.ToStateJson();
        Check(ParseBtDebugTree(failedStateJson, dbg, dbgErr), "failed ToStateJson parses in ParseBtDebugTree");

        // Now Retry from failed node "build" with resetCounters=true
        Check(restored.Retry("build", true, 2000, err), "Retry from failed node 'build': " + err);
        Check(restored.State() == "running" && runner.startedJobs.size() == 3 &&
              runner.startedJobs.back().proposedJobId == "feature-persist-build-3",
              "Retry launched job feature-persist-3");

        restored.OnJobFinished("feature-persist-build-3", true, 0, "", "built!", 2500);
        Check(restored.NodeState("test") == "running" && runner.startedJobs.size() == 4,
              "after build succeeded, 'test' job feature-persist-4 started");

        restored.OnJobFinished("feature-persist-test-1", true, 0, "", "all tests passed", 3000);
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

    // 091-2: catch node and tree-level defaults.
    const std::string diagHang = R"({"class":"startup_hang","confidence":"high",)"
        R"("summary":"no events","suggestedAction":"retry_with_change","retryAt":null,)"
        R"("likelyCauses":[{"id":"no_model","why":"no --model"}]})";
    const std::string diagRate = R"({"class":"rate_limited","summary":"quota","likelyCauses":[]})";

    // (1) handler match, selected-handler detail, thenRetry and maxRetries -> otherwise node.
    {
        const std::string def = R"({"id":"catch-match","root":{"type":"catch","id":"guard","maxRetries":1,)"
            R"("child":{"type":"job","id":"impl","summary":"impl","command":"c","timeoutSec":60},)"
            R"("handlers":[)"
            R"({"class":["startup_hang","idle_stall"],"cause":"no_model",)"
            R"("do":{"type":"set","id":"fix","var":"fixed","value":"yes"},"thenRetry":true},)"
            R"({"class":["rate_limited"],"do":{"type":"wait","id":"backoff","sec":0}}],)"
            R"("otherwise":{"type":"fail","id":"otherwise","detail":"no handler"}}})";
        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(def, &catalog, err), "091-2: load catch-match BT: " + err);
        bt.SetJobRunner(&runner);
        bt.Start(1000);
        Check(bt.NodeState("impl") == "running" && runner.startedJobs.size() == 1, "091-2: catch starts child");
        bt.OnJobFinished("catch-match-impl-1", false, -1, "", "", 1100, "", "", "hang", diagHang);
        Check(bt.NodeState("fix") == "succeeded" && bt.GetVar("fixed") == "yes", "091-2: matching handler's do ran");
        Check(runner.startedJobs.size() == 2 && bt.NodeState("impl") == "running",
              "091-2: thenRetry re-ran child");
        Check(bt.NodeDetail("guard").find("handler[0]") != std::string::npos &&
              bt.NodeDetail("guard").find("no_model") != std::string::npos,
              "091-2: selected handler recorded in running node state: " + bt.NodeDetail("guard"));

        bt.OnJobFinished("catch-match-impl-2", false, -1, "", "", 1200, "", "", "hang2", diagHang);
        Check(bt.NodeState("otherwise") == "failed" && bt.NodeState("guard") == "failed",
              "091-2: maxRetries exceeded runs otherwise and fails catch");
        bool detailInEvent = false;
        for (const auto& e : bt.Events()) {
            if (e.type == "nodeFinished" && e.nodeKey == "guard" &&
                e.detail.find("handler[0]") != std::string::npos) detailInEvent = true;
        }
        Check(detailInEvent, "091-2: nodeFinished detail carries selected handler");
        Check(bt.NodeDetail("guard").find("no handler") != std::string::npos,
              "091-2: otherwise failure detail kept: " + bt.NodeDetail("guard"));

        BtDebugNode dbg;
        std::string dbgErr;
        Check(ParseBtDebugTree(bt.ToStateJson(), dbg, dbgErr), "091-2: catch state parses in debug view: " + dbgErr);
        bool foundGuard = false, foundDo = false, foundOtherwise = false;
        auto walk = [&](auto&& self, const BtDebugNode& n) -> void {
            if (n.nodeId == "guard") {
                foundGuard = n.label.find("handlers=2") != std::string::npos &&
                             n.label.find("maxRetries=1") != std::string::npos &&
                             n.label.find("handler[0]") != std::string::npos;
            }
            if (n.nodeId == "fix") foundDo = (n.role == "do");
            if (n.nodeId == "otherwise") foundOtherwise = true;
            for (const auto& c : n.children) self(self, c);
        };
        walk(walk, dbg);
        Check(foundGuard, "091-2: debug view label shows catch handlers/maxRetries/selected handler");
        Check(foundDo && foundOtherwise, "091-2: debug view shows do and otherwise child slots");
    }

    // (2) child success; no match with default otherwise; "*" wildcard; do failure.
    {
        auto makeCatch = [](const std::string& id, const std::string& handlers, const std::string& otherwise) {
            return std::string("{\"id\":\"") + id + R"(","root":{"type":"catch","id":"guard",)"
                R"("child":{"type":"job","id":"impl","summary":"impl","command":"c","timeoutSec":60},)"
                R"("handlers":)" + handlers + R"(,"otherwise":)" + otherwise + "}}";
        };
        {
            FakeJobRunner runner;
            BehaviorTree bt;
            std::string err;
            Check(bt.LoadDefinition(makeCatch("catch-ok", R"([{"class":"startup_hang","do":{"type":"succeed"}}])", R"("fail")"), &catalog, err),
                  "091-2: load catch-ok BT: " + err);
            bt.SetJobRunner(&runner);
            bt.Start(1000);
            bt.OnJobFinished("catch-ok-impl-1", true, 0, "", "", 1100);
            Check(bt.State() == "succeeded" && bt.NodeState("guard") == "succeeded" &&
                  bt.NodeDetail("guard").empty(), "091-2: child success makes catch succeed");
        }
        {
            FakeJobRunner runner;
            BehaviorTree bt;
            std::string err;
            Check(bt.LoadDefinition(makeCatch("catch-nomatch",
                  R"([{"class":"startup_hang","do":{"type":"succeed"}}])", R"("fail")"), &catalog, err),
                  "091-2: load catch-nomatch BT: " + err);
            bt.SetJobRunner(&runner);
            bt.Start(1000);
            bt.OnJobFinished("catch-nomatch-impl-1", false, 1, "", "", 1100, "", "", "boom", diagRate);
            Check(bt.NodeState("guard") == "failed" && bt.NodeDetail("guard") == "boom",
                  "091-2: no handler match with default otherwise fails with child detail");
        }
        {
            FakeJobRunner runner;
            BehaviorTree bt;
            std::string err;
            Check(bt.LoadDefinition(makeCatch("catch-star",
                  R"([{"class":"*","cause":"*none","do":{"type":"set","var":"star","value":"x"}},)"
                  R"({"class":"*","do":{"type":"set","var":"star","value":"ok"}}])", R"("fail")"), &catalog, err),
                  "091-2: load catch-star BT: " + err);
            bt.SetJobRunner(&runner);
            bt.Start(1000);
            bt.OnJobFinished("catch-star-impl-1", false, 1, "", "", 1100, "", "", "boom", diagRate);
            Check(bt.NodeState("guard") == "succeeded" && bt.GetVar("star") == "ok",
                  "091-2: '*' matches any failure and falls through to the next handler when cause misses");
        }
        {
            FakeJobRunner runner;
            BehaviorTree bt;
            std::string err;
            Check(bt.LoadDefinition(makeCatch("catch-dofail",
                  R"([{"class":"*","do":{"type":"fail","detail":"do broke"}}])", R"("fail")"), &catalog, err),
                  "091-2: load catch-dofail BT: " + err);
            bt.SetJobRunner(&runner);
            bt.Start(1000);
            bt.OnJobFinished("catch-dofail-impl-1", false, 1, "", "", 1100, "", "", "boom", diagRate);
            Check(bt.NodeState("guard") == "failed" &&
                  bt.NodeDetail("guard").find("do failed: do broke") != std::string::npos,
                  "091-2: failing do fails the catch: " + bt.NodeDetail("guard"));
        }
    }

    // (2b) a composite child reports the last failed job/worker leaf's diagnosis.
    {
        const std::string def = R"({"id":"catch-composite","root":{"type":"catch","id":"guard",)"
            R"("child":{"type":"fallback","children":[)"
            R"({"type":"job","id":"first","summary":"first","command":"a","timeoutSec":60},)"
            R"({"type":"job","id":"last","summary":"last","command":"b","timeoutSec":60}]},)"
            R"("handlers":[{"class":"startup_hang","cause":"no_model",)"
            R"("do":{"type":"set","var":"matched","value":"last"}}],"otherwise":"fail"}})";
        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(def, &catalog, err), "091-2: load catch-composite BT: " + err);
        bt.SetJobRunner(&runner);
        bt.Start(1000);
        bt.OnJobFinished("catch-composite-first-1", false, 1, "", "", 1100, "", "", "boom", diagRate);
        Check(bt.NodeState("last") == "running", "091-2: fallback moved to the second job");
        bt.OnJobFinished("catch-composite-last-1", false, 1, "", "", 1200, "", "", "hang", diagHang);
        Check(bt.GetVar("matched") == "last" && bt.NodeState("guard") == "succeeded",
              "091-2: catch matches the last failed leaf's diagnosis under a composite child");
    }

    // (3) validation errors.
    {
        auto invalid = [&](const std::string& label, const std::string& def, const std::string& needle) {
            std::vector<std::string> errs;
            bool ok = BehaviorTree::ValidateJson(def, &catalog, errs);
            bool found = false;
            for (const auto& e : errs) if (e.find(needle) != std::string::npos) found = true;
            Check(!ok && found, label);
        };
        std::string base = R"({"id":"catch-bad","root":{"type":"catch",)"
            R"("child":{"type":"succeed"},"handlers":%s,"otherwise":%s}})" ;
        auto fmt = [&](const std::string& handlers, const std::string& otherwise) {
            std::string s = base;
            s.replace(s.find("%s"), 2, handlers);
            s.replace(s.find("%s"), 2, otherwise);
            return s;
        };
        invalid("091-2: empty handlers rejected", fmt(R"([])", R"("fail")"), "handlers");
        invalid("091-2: handler without do rejected", fmt(R"([{"class":"*"}] )", R"("fail")"), "'do'");
        invalid("091-2: maxRetries above 10 rejected",
                R"({"id":"catch-bad","root":{"type":"catch","child":{"type":"succeed"},)"
                R"("handlers":[{"class":"*","do":{"type":"succeed"}}],"maxRetries":11}})",
                "maxRetries");
        invalid("091-2: unknown otherwise string rejected",
                fmt(R"([{"class":"*","do":{"type":"succeed"}}])", R"("escalate")"), "otherwise");
    }

    // (4) state save/resume mid-handler and bt_retry --from a node inside the catch.
    {
        const std::string def = R"({"id":"catch-state","root":{"type":"catch","id":"guard",)"
            R"("child":{"type":"job","id":"impl","summary":"impl","command":"c","timeoutSec":60},)"
            R"("handlers":[{"class":"startup_hang",)"
            R"("do":{"type":"job","id":"fix","summary":"fix","command":"fix","timeoutSec":60},)"
            R"("thenRetry":true}],"otherwise":"fail"}})";
        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(def, &catalog, err), "091-2: load catch-state BT: " + err);
        bt.SetJobRunner(&runner);
        bt.Start(1000);
        bt.OnJobFinished("catch-state-impl-1", false, -1, "", "", 1100, "", "", "hang", diagHang);
        Check(bt.NodeState("fix") == "running" && runner.startedJobs.size() == 2,
              "091-2: handler do job is running");
        std::string saved = bt.ToStateJson();
        BehaviorTree restored;
        Check(restored.LoadFromStateJson(saved, &catalog, 1200, err), "091-2: catch state reloads: " + err);
        restored.SetJobRunner(&runner);
        Check(restored.NodeState("guard") == "running" && restored.NodeState("fix") == "running",
              "091-2: catch phase and do job restored");
        restored.OnJobFinished("catch-state-fix-1", false, 1, "", "", 1300, "", "", "do failed", R"({"class":"auth_error"})");
        Check(restored.NodeState("guard") == "failed", "091-2: do failure after reload fails catch");
        std::string retryErr;
        Check(restored.Retry("fix", false, 1400, retryErr), "091-2: bt_retry from handler node: " + retryErr);
        Check(restored.NodeState("guard") == "running" && restored.NodeState("fix") == "running" &&
              runner.startedJobs.back().proposedJobId == "catch-state-fix-2",
              "091-2: retry from do node re-runs the handler");
        restored.OnJobFinished("catch-state-fix-2", true, 0, "", "", 1500, "", "", "");
        Check(restored.NodeState("impl") == "running" &&
              runner.startedJobs.back().proposedJobId == "catch-state-impl-2",
              "091-2: successful do after retry re-runs the child");
        restored.OnJobFinished("catch-state-impl-2", true, 0, "", "", 1600);
        Check(restored.State() == "succeeded" &&
              restored.NodeDetail("guard").find("handler[0]") != std::string::npos,
              "091-2: catch succeeds after resumed retry with handler detail");
    }

    // (5) pendingEstimateSec / PlannedTasks follow the catch's current phase.
    {
        const std::string def = R"({"id":"catch-est","root":{"type":"catch",)"
            R"("child":{"type":"job","id":"impl","summary":"impl","command":"c","timeoutSec":100},)"
            R"("handlers":[{"class":"*","thenRetry":true,)"
            R"("do":{"type":"job","id":"fix","summary":"fix","command":"fix","timeoutSec":50}}],)"
            R"("otherwise":"fail","maxRetries":2}})";
        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(def, &catalog, err), "091-2: load catch-est BT: " + err);
        bt.SetJobRunner(&runner);
        bt.Start(1000);
        Check(bt.PendingEstimateSec() == 300 && bt.PlannedTasks().empty(),
              "091-2: catch with running child estimates future retries only: " +
                  std::to_string(bt.PendingEstimateSec()));
        bt.OnJobFinished("catch-est-impl-1", false, 1, "", "", 1100, "", "", "boom", diagRate);
        Check(bt.NodeState("fix") == "running" && bt.PendingEstimateSec() == 300,
              "091-2: catch estimate counts running do plus future child retries: " +
                  std::to_string(bt.PendingEstimateSec()));
    }

    // (6) tree-level defaults: explicit values win, including explicit 0.
    {
        const std::string def = R"({"id":"defaults-test",)"
            R"("worker":{"engine":"claude","model":"sonnet"},)"
            R"("defaults":{"worker":{"idle":{"firstEventSec":180,"outputSec":600},"maxAttempts":2,"probe":true},)"
            R"("job":{"idle":{"outputSec":900}}},)"
            R"("root":{"type":"sequence","children":[)"
            R"({"type":"worker","id":"w1","summary":"w1","prompt":"p","timeoutSec":60},)"
            R"({"type":"worker","id":"w2","summary":"w2","prompt":"p","timeoutSec":60,"idle":0,"maxAttempts":1},)"
            R"({"type":"job","id":"j1","summary":"j1","command":"build.exe","timeoutSec":60},)"
            R"({"type":"job","id":"j2","summary":"j2","command":"ww-run --task x","timeoutSec":60},)"
            R"({"type":"job","id":"j3","summary":"j3","command":"claude -p hi","timeoutSec":60},)"
            R"({"type":"job","id":"j4","summary":"j4","command":"build.exe","timeoutSec":60,)"
            R"("idle":{"outputSec":0,"cpuSec":0,"firstEventSec":0},"maxAttempts":4}]}})";
        FakeJobRunner runner;
        BehaviorTree bt;
        std::string err;
        Check(bt.LoadDefinition(def, &catalog, err), "091-2: load defaults BT: " + err);
        Check(bt.Defaults().hasWorkerIdle() && bt.Defaults().hasJobIdle() &&
              bt.Defaults().workerMaxAttempts == 2 && bt.Defaults().workerProbe &&
              bt.Defaults().hasWorkerProbe, "091-2: defaults parsed and stored");
        bt.SetJobRunner(&runner);
        bt.Start(1000);
        auto finish = [&](const std::string& id, int64_t t) {
            std::string jid = "defaults-test-" + id + "-1";
            if (id[0] == 'w') bt.OnJobFinished(jid, true, 0, "", "", t, "ok", "{}");
            else bt.OnJobFinished(jid, true, 0, "", "", t);
        };
        finish("w1", 1100);
        Check(runner.startedWorkers[0].idle.firstEventSec == 180 &&
              runner.startedWorkers[0].idle.outputSec == 600 &&
              runner.startedWorkers[0].idle.configured &&
              runner.startedWorkers[0].maxAttempts == 2,
              "091-2: worker node gets worker idle and maxAttempts defaults");
        finish("w2", 1200);
        Check(runner.startedWorkers.size() == 2 &&
              runner.startedWorkers[1].idle.outputSec == 0 &&
              runner.startedWorkers[1].idle.firstEventSec == 0 &&
              runner.startedWorkers[1].idle.configured &&
              runner.startedWorkers[1].maxAttempts == 1,
              "091-2: explicit idle:0 and maxAttempts on a worker are respected");
        finish("j1", 1300);
        Check(runner.startedJobs[0].idle.outputSec == 900 && runner.startedJobs[0].maxAttempts == 1,
              "091-2: plain job gets defaults.job.idle, not worker defaults");
        finish("j2", 1400);
        Check(runner.startedJobs[1].idle.firstEventSec == 180 &&
              runner.startedJobs[1].idle.outputSec == 600 && runner.startedJobs[1].maxAttempts == 2,
              "091-2: ww-run job gets worker defaults");
        finish("j3", 1500);
        Check(runner.startedJobs[2].idle.firstEventSec == 180 && runner.startedJobs[2].maxAttempts == 2,
              "091-2: known LLM CLI job gets worker defaults");
        finish("j4", 1600);
        Check(runner.startedJobs[3].idle.outputSec == 0 && runner.startedJobs[3].maxAttempts == 4,
              "091-2: explicit job idle/maxAttempts win over defaults");
        Check(bt.State() == "succeeded", "091-2: defaults BT completed");

        std::string saved = bt.ToStateJson();
        Check(saved.find("\"defaults\"") != std::string::npos &&
              saved.find("\"probe\": true") != std::string::npos,
              "091-2: defaults persisted in state JSON");
        BehaviorTree restored;
        Check(restored.LoadFromStateJson(saved, &catalog, 1700, err) &&
              restored.Defaults().workerProbe && restored.Defaults().workerMaxAttempts == 2,
              "091-2: defaults survive state reload");
    }

    // (7) defaults validation errors.
    {
        auto invalidDefaults = [&](const std::string& label, const std::string& def, const std::string& needle) {
            std::vector<std::string> errs;
            bool ok = BehaviorTree::ValidateJson(def, &catalog, errs);
            bool found = false;
            for (const auto& e : errs) if (e.find(needle) != std::string::npos) found = true;
            Check(!ok && found, label);
        };
        std::string head = R"({"id":"defaults-bad","defaults":)";
        std::string tail = R"(,"root":{"type":"succeed"}})";
        invalidDefaults("091-2: bad defaults.worker.idle rejected", head + R"({"worker":{"idle":42}})" + tail, "idle");
        invalidDefaults("091-2: bad defaults.worker.maxAttempts rejected",
                        head + R"({"worker":{"maxAttempts":0}})" + tail, "maxAttempts");
        invalidDefaults("091-2: bad defaults.worker.probe rejected",
                        head + R"({"worker":{"probe":"yes"}})" + tail, "probe");
    }

    // 092: bt retry --from must discard later siblings, including a stale escalate answer.
    {
        const std::string def = R"({"id":"bt-retry-siblings","cwd":")" + JsonEscape(tempDirUtf8) + R"(",)"
            R"("root":{"type":"fallback","children":[)"
            R"({"type":"sequence","id":"do","children":[)"
            R"({"type":"job","id":"a","summary":"a","command":"a","timeoutSec":60},)"
            R"({"type":"job","id":"b","summary":"b","command":"b","timeoutSec":60},)"
            R"({"type":"job","id":"c","summary":"c","command":"c","timeoutSec":60}]},)"
            R"({"type":"sequence","id":"alt","children":[)"
            R"({"type":"escalate","id":"gate","question":"Continue?","expect":["continue","stop"]},)"
            R"({"type":"if","id":"check",)"
            R"("cond":{"type":"var","name":"gate.answer","eq":"continue"},)"
            R"("then":{"type":"job","id":"after_gate","summary":"after","command":"after","timeoutSec":60},)"
            R"("else":{"type":"fail","detail":"stopped at 091-7"}}]}]}})";

        auto runToFailed = [&](FakeJobRunner& runner, BehaviorTree& bt) {
            std::string e;
            Check(bt.LoadDefinition(def, &catalog, e), "092: load retry-siblings BT: " + e);
            bt.SetJobRunner(&runner);
            bt.Start(1000);
            bt.OnJobFinished("bt-retry-siblings-a-1", true, 0, "", "a ok", 1100);
            bt.OnJobFinished("bt-retry-siblings-b-1", true, 0, "", "b ok", 1200);
            bt.OnJobFinished("bt-retry-siblings-c-1", false, 1, "", "c failed", 1300);
            Check(bt.State() == "waitingForLlm" && bt.NodeState("gate") == "running",
                  "092: c failure escalates the gate");
            Check(bt.Resume("gate", "stop", "Stopped at 091-7", {}, 1400, e), "092: resume stop: " + e);
            Check(bt.State() == "failed" && bt.GetVar("gate.answer") == "stop",
                  "092: stop answer fails the tree and sets gate.answer");
        };

        // --from a: a, b and c all re-run; succeeding c keeps the gate untouched.
        {
            FakeJobRunner runner;
            BehaviorTree bt;
            runToFailed(runner, bt);
            Check(runner.startedJobs.size() == 3, "092: first run launched a, b, c");

            std::string err;
            Check(bt.Retry("a", true, 5000, err), "092: retry --from a: " + err);
            Check(bt.NodeState("a") == "running" && runner.startedJobs.size() == 4 &&
                  runner.startedJobs.back().proposedJobId == "bt-retry-siblings-a-2",
                  "092: a re-launched after retry");
            Check(bt.NodeState("b") == "idle" && bt.NodeState("c") == "idle" &&
                  bt.NodeState("gate") == "idle" && bt.GetVar("gate.answer").empty(),
                  "092: later siblings and stale gate answer discarded");
            bt.OnJobFinished("bt-retry-siblings-a-2", true, 0, "", "a ok", 5100);
            Check(runner.startedJobs.size() == 5 &&
                  runner.startedJobs.back().proposedJobId == "bt-retry-siblings-b-2",
                  "092: b re-runs after a succeeds");
            bt.OnJobFinished("bt-retry-siblings-b-2", true, 0, "", "b ok", 5200);
            Check(runner.startedJobs.size() == 6 &&
                  runner.startedJobs.back().proposedJobId == "bt-retry-siblings-c-2",
                  "092: c re-runs after b succeeds");
            bt.OnJobFinished("bt-retry-siblings-c-2", true, 0, "", "c ok", 5300);
            Check(bt.State() == "succeeded" && bt.NodeState("gate") == "idle" &&
                  bt.NodeState("after_gate") == "idle",
                  "092: c success makes the tree succeed without re-running the gate");
            Check(runner.startedWorkers.empty(), "092: no escalation after successful retry");
        }

        // --from b: a is kept, b and c re-run, and the stale gate branch is discarded.
        {
            FakeJobRunner runner;
            BehaviorTree bt;
            runToFailed(runner, bt);

            std::string err;
            Check(bt.Retry("b", true, 6000, err), "092: retry --from b: " + err);
            Check(bt.NodeState("a") == "succeeded" && runner.startedJobs.size() == 4 &&
                  runner.startedJobs.back().proposedJobId == "bt-retry-siblings-b-2",
                  "092: a is kept and b re-runs");
            Check(bt.NodeState("c") == "idle" && bt.NodeState("gate") == "idle" &&
                  bt.GetVar("gate.answer").empty(),
                  "092: c and the escalate branch are reset by --from b");
            bt.OnJobFinished("bt-retry-siblings-b-2", true, 0, "", "b ok", 6100);
            bt.OnJobFinished("bt-retry-siblings-c-2", true, 0, "", "c ok", 6200);
            Check(bt.State() == "succeeded", "092: tree succeeds after --from b");
        }

        // The gate is re-evaluated with a clean answer when the earlier branch fails again.
        {
            FakeJobRunner runner;
            BehaviorTree bt;
            runToFailed(runner, bt);

            std::string err;
            Check(bt.Retry("a", true, 7000, err), "092: retry --from a again: " + err);
            Check(bt.GetVar("gate.answer").empty(), "092: gate.answer cleared before re-escalation");
            bt.OnJobFinished("bt-retry-siblings-a-2", true, 0, "", "a ok", 7100);
            bt.OnJobFinished("bt-retry-siblings-b-2", true, 0, "", "b ok", 7200);
            bt.OnJobFinished("bt-retry-siblings-c-2", false, 1, "", "c failed again", 7300);
            Check(bt.State() == "waitingForLlm" && bt.NodeState("gate") == "running",
                  "092: gate re-escalates when c fails again");
            Check(bt.GetVar("gate.answer").empty(), "092: re-escalated gate has no stale answer");
            Check(bt.Resume("gate", "continue", "Try again", {}, 7400, err), "092: resume continue: " + err);
            bt.OnJobFinished("bt-retry-siblings-after_gate-1", true, 0, "", "after ok", 7500);
            Check(bt.State() == "succeeded", "092: continue branch completes after re-escalation");
        }
    }

    // 093-2: foreach node and ${foreach:...} expansion.
    {
        auto makeSuiteDir = [&](const std::string& name, const std::vector<std::string>& smokes) -> std::wstring {
            std::wstring d = tempDirW + L"\\fe_" + Utf8ToWide(name);
            CreateDirectoryW(d.c_str(), nullptr);
            CreateDirectoryW((d + L"\\tests").c_str(), nullptr);
            for (const auto& s : smokes) {
                WriteFileAtomicWide(d + L"\\tests\\" + Utf8ToWide(s), "// " + s);
            }
            return d;
        };
        auto makeForeachDef = [&](const std::string& id, const std::wstring& d,
                                  const std::string& mode, const std::string& extra) {
            return std::string(R"JSON({"id":")JSON") + id + R"JSON(","cwd":")JSON" +
                JsonEscape(WideToUtf8(d)) +
                R"JSON(","root":{"type":"foreach","id":"smokes","glob":"tests/*_smoke.cpp","as":"t","mode":")JSON" +
                mode + "\"," + extra +
                R"JSON("do":{"type":"job","id":"run_${t.stem}","summary":"Run ${t.stem} from ${t.name} #${t.index}","command":"build\\Release\\${t.stem}.exe","timeoutSec":900}}})JSON";
        };

        // (1) two files -> two children ticked in sorted order; state/debug view show them.
        {
            std::wstring d = makeSuiteDir("basic", {"a_smoke.cpp", "b_smoke.cpp"});
            std::string def = makeForeachDef("fe-basic", d, "sequence", "");
            std::vector<std::string> errs;
            Check(BehaviorTree::ValidateJson(def, &catalog, errs), "093-2: foreach BT validates");

            FakeJobRunner runner;
            BehaviorTree bt;
            std::string err;
            Check(bt.LoadDefinition(def, &catalog, err), "093-2: load foreach BT: " + err);
            bt.SetJobRunner(&runner);
            bt.Start(1000);
            Check(runner.startedJobs.size() == 1 && runner.startedJobs[0].nodeKey == "run_a_smoke" &&
                  runner.startedJobs[0].command == "build\\Release\\a_smoke.exe" &&
                  runner.startedJobs[0].summary == "Run a_smoke from a_smoke.cpp #0",
                  "093-2: first expanded child uses stem/name/index (command=" +
                  (runner.startedJobs.empty() ? "" : runner.startedJobs[0].command) + ")");
            Check(bt.NodeState("run_b_smoke") == "idle", "093-2: second child still idle");
            bt.OnJobFinished("fe-basic-run_a_smoke-1", true, 0, "", "a ok", 1100);
            Check(runner.startedJobs.size() == 2 && runner.startedJobs[1].nodeKey == "run_b_smoke" &&
                  runner.startedJobs[1].summary == "Run b_smoke from b_smoke.cpp #1",
                  "093-2: second child starts after first succeeds");
            bt.OnJobFinished("fe-basic-run_b_smoke-1", true, 0, "", "b ok", 1200);
            Check(bt.State() == "succeeded" && bt.NodeState("smokes") == "succeeded",
                  "093-2: sequence foreach succeeds after all children");

            std::string state = bt.ToStateJson();
            Check(state.find("\"foreachExpanded\": true") != std::string::npos &&
                  state.find("\"id\": \"run_a_smoke\"") != std::string::npos &&
                  state.find("\"id\": \"run_b_smoke\"") != std::string::npos,
                  "093-2: expanded children and marker persisted in state JSON");
            BtDebugNode dbg;
            std::string de;
            Check(ParseBtDebugTree(state, dbg, de), "093-2: foreach state parses in debug view: " + de);
            Check(FindBtNodeKeyByJob({dbg}, "fe-basic-run_a_smoke-1") == "fe-basic/run_a_smoke",
                  "093-2: expanded child visible as a normal debug node");

            // bt retry --from <expanded child> re-runs it (092 consistency).
            BehaviorTree restored;
            Check(restored.LoadFromStateJson(state, &catalog, 2000, err),
                  "093-2: foreach state reloads: " + err);
            Check(restored.NodeState("run_a_smoke") == "succeeded", "093-2: child state restored");
            FakeJobRunner r2;
            restored.SetJobRunner(&r2);
            Check(restored.Retry("run_a_smoke", true, 3000, err), "093-2: retry --from expanded child: " + err);
            Check(r2.startedJobs.size() == 1 && r2.startedJobs[0].nodeKey == "run_a_smoke",
                  "093-2: retried child relaunched");
            restored.OnJobFinished("fe-basic-run_a_smoke-2", true, 0, "", "a ok", 3100);
            Check(r2.startedJobs.size() == 2 && r2.startedJobs[1].nodeKey == "run_b_smoke",
                  "093-2: retry re-runs the later sibling (092 semantics)");
            restored.OnJobFinished("fe-basic-run_b_smoke-2", true, 0, "", "b ok", 3200);
            Check(restored.State() == "succeeded", "093-2: tree succeeds after child retry");
        }

        // (2) adding a file and restarting grows the expanded set to three.
        {
            std::wstring d = makeSuiteDir("regrow", {"a_smoke.cpp", "b_smoke.cpp"});
            std::string def = makeForeachDef("fe-regrow", d, "sequence", "");
            WriteFileAtomicWide(d + L"\\tests\\c_smoke.cpp", "// c");
            FakeJobRunner runner;
            BehaviorTree bt;
            std::string err;
            Check(bt.LoadDefinition(def, &catalog, err), "093-2: load regrow BT: " + err);
            bt.SetJobRunner(&runner);
            bt.Start(1000);
            bt.OnJobFinished("fe-regrow-run_a_smoke-1", true, 0, "", "ok", 1100);
            bt.OnJobFinished("fe-regrow-run_b_smoke-1", true, 0, "", "ok", 1200);
            Check(runner.startedJobs.size() == 3 && runner.startedJobs[2].nodeKey == "run_c_smoke",
                  "093-2: restart after adding a file expands to three children");
        }

        // (3) exclude, minMatches, zero matches, and mode:all with a failure.
        {
            std::wstring d = makeSuiteDir("exclude", {"a_smoke.cpp", "b_smoke.cpp"});
            std::string def = makeForeachDef("fe-exclude", d, "all", R"JSON("exclude":["tests/a_smoke.cpp"],)JSON");
            FakeJobRunner runner;
            BehaviorTree bt;
            std::string err;
            Check(bt.LoadDefinition(def, &catalog, err), "093-2: load exclude BT: " + err);
            bt.SetJobRunner(&runner);
            bt.Start(1000);
            Check(runner.startedJobs.size() == 1 && runner.startedJobs[0].nodeKey == "run_b_smoke",
                  "093-2: exclude removes the matching file");
            bt.OnJobFinished("fe-exclude-run_b_smoke-1", true, 0, "", "ok", 1100);
            Check(bt.State() == "succeeded", "093-2: excluded foreach succeeds");
        }
        {
            std::wstring d = makeSuiteDir("min", {"a_smoke.cpp"});
            std::string def = makeForeachDef("fe-min", d, "sequence", R"JSON("minMatches":2,)JSON");
            FakeJobRunner runner;
            BehaviorTree bt;
            std::string err;
            Check(bt.LoadDefinition(def, &catalog, err), "093-2: load minMatches BT: " + err);
            bt.SetJobRunner(&runner);
            bt.Start(1000);
            Check(bt.State() == "failed" && bt.NodeDetail("smokes").find("minMatches") != std::string::npos &&
                  runner.startedJobs.empty(), "093-2: minMatches failure stops before launching children");
        }
        {
            std::wstring d = makeSuiteDir("empty", {});
            std::string def = makeForeachDef("fe-empty", d, "sequence", "");
            FakeJobRunner runner;
            BehaviorTree bt;
            std::string err;
            Check(bt.LoadDefinition(def, &catalog, err), "093-2: load empty foreach BT: " + err);
            bt.SetJobRunner(&runner);
            bt.Start(1000);
            Check(bt.State() == "succeeded" && runner.startedJobs.empty() &&
                  bt.ToStateJson().find("0 matches") != std::string::npos,
                  "093-2: zero matches succeeds with a warning detail");
        }
        {
            std::wstring d = makeSuiteDir("all", {"a_smoke.cpp", "b_smoke.cpp", "c_smoke.cpp"});
            std::string def = makeForeachDef("fe-all", d, "all", "");
            FakeJobRunner runner;
            BehaviorTree bt;
            std::string err;
            Check(bt.LoadDefinition(def, &catalog, err), "093-2: load mode:all BT: " + err);
            bt.SetJobRunner(&runner);
            bt.Start(1000);
            bt.OnJobFinished("fe-all-run_a_smoke-1", true, 0, "", "ok", 1100);
            bt.OnJobFinished("fe-all-run_b_smoke-1", false, 1, "", "b broke", 1200);
            Check(runner.startedJobs.size() == 3 && runner.startedJobs[2].nodeKey == "run_c_smoke",
                  "093-2: mode:all keeps launching after a failure");
            bt.OnJobFinished("fe-all-run_c_smoke-1", true, 0, "", "ok", 1300);
            Check(bt.State() == "failed" && bt.NodeState("run_c_smoke") == "succeeded",
                  "093-2: mode:all fails at the end when any child failed");
        }

        // parallel mode launches every child and a '**' glob matches nested files.
        {
            std::wstring d = makeSuiteDir("parallel", {});
            CreateDirectoryW((d + L"\\tests\\unit").c_str(), nullptr);
            WriteFileAtomicWide(d + L"\\tests\\unit\\a_smoke.cpp", "// a");
            WriteFileAtomicWide(d + L"\\tests\\unit\\b_smoke.cpp", "// b");
            std::string def = std::string(R"JSON({"id":"fe-parallel","cwd":")JSON") +
                JsonEscape(WideToUtf8(d)) +
                R"JSON(","root":{"type":"foreach","id":"smokes","glob":"tests/**/*_smoke.cpp","as":"t","mode":"parallel","do":{"type":"job","id":"run_${t.stem}","summary":"s ${t.path}","command":"c","timeoutSec":30}}})JSON";
            FakeJobRunner runner;
            BehaviorTree bt;
            std::string err;
            Check(bt.LoadDefinition(def, &catalog, err), "093-2: load parallel/** BT: " + err);
            bt.SetJobRunner(&runner);
            bt.Start(1000);
            Check(runner.startedJobs.size() == 2 &&
                  runner.startedJobs[0].nodeKey == "run_a_smoke" &&
                  runner.startedJobs[1].nodeKey == "run_b_smoke" &&
                  runner.startedJobs[0].summary == "s tests/unit/a_smoke.cpp",
                  "093-2: parallel foreach launches all children from a '**' glob");
            bt.OnJobFinished("fe-parallel-run_a_smoke-1", true, 0, "", "ok", 1100);
            Check(bt.State() == "running", "093-2: parallel foreach waits for remaining children");
            bt.OnJobFinished("fe-parallel-run_b_smoke-1", true, 0, "", "ok", 1200);
            Check(bt.State() == "succeeded", "093-2: parallel foreach succeeds when all children succeed");
        }

        // (4) ${foreach:...} joins stems/names in command, summary and success strings.
        {
            std::wstring d = makeSuiteDir("list", {"a_smoke.cpp", "b_smoke.cpp"});
            std::string def = std::string(R"JSON({"id":"fe-list","cwd":")JSON") +
                JsonEscape(WideToUtf8(d)) +
                R"JSON(","root":{"type":"job","id":"build","summary":"Targets ${foreach:tests/*_smoke.cpp:stem:|}","command":"cmake --build build --config Release --target Hancho ${foreach:tests/*_smoke.cpp:stem: }","timeoutSec":60,"success":{"testReport":"build\\Release\\${foreach:tests/*_smoke.cpp:stem:_}.txt","outputRegex":"^${foreach:tests/*_smoke.cpp:name:,}$"}}})JSON";
            FakeJobRunner runner;
            BehaviorTree bt;
            std::string err;
            Check(bt.LoadDefinition(def, &catalog, err), "093-2: load ${foreach:...} BT: " + err);
            bt.SetJobRunner(&runner);
            bt.Start(1000);
            Check(runner.startedJobs.size() == 1, "093-2: ${foreach:...} job started");
            if (!runner.startedJobs.empty()) {
                const auto& j = runner.startedJobs[0];
                Check(j.command == "cmake --build build --config Release --target Hancho a_smoke b_smoke",
                      "093-2: ${foreach:stem: } joined into command: " + j.command);
                Check(j.summary == "Targets a_smoke|b_smoke", "093-2: ${foreach:stem:|} joined into summary: " + j.summary);
                Check(j.success.testReport == "build\\Release\\a_smoke_b_smoke.txt" &&
                      j.success.outputRegex == "^a_smoke.cpp,b_smoke.cpp$",
                      "093-2: ${foreach:...} expanded in success strings");
            }
            Check(bt.State() == "running", "093-2: ${foreach:...} job waits for completion");
        }

        // (5) duplicate expanded child ids are rejected.
        {
            std::wstring d = makeSuiteDir("collision", {"a_smoke.cpp", "b_smoke.cpp"});
            std::string def = std::string(R"JSON({"id":"fe-collision","cwd":")JSON") +
                JsonEscape(WideToUtf8(d)) +
                R"JSON(","root":{"type":"foreach","id":"smokes","glob":"tests/*_smoke.cpp","as":"t","do":{"type":"job","id":"fixed","summary":"s","command":"c","timeoutSec":30}}})JSON";
            FakeJobRunner runner;
            BehaviorTree bt;
            std::string err;
            Check(bt.LoadDefinition(def, &catalog, err), "093-2: load collision BT: " + err);
            bt.SetJobRunner(&runner);
            bt.Start(1000);
            Check(bt.State() == "failed" &&
                  bt.NodeDetail("smokes").find("duplicate expanded child id") != std::string::npos &&
                  runner.startedJobs.empty(), "093-2: expanded id collision is an error");
        }

        // validate checks the unexpanded `do` and the as/${...} field correspondence.
        {
            std::wstring d = makeSuiteDir("validate", {"a_smoke.cpp"});
            std::string bad = makeForeachDef("fe-bad", d, "sequence", "");
            size_t p = 0;
            while ((p = bad.find("${t.stem}", p)) != std::string::npos) {
                bad.replace(p, 9, "${t.extension}");
                p += 14;
            }
            std::vector<std::string> errs;
            bool ok = BehaviorTree::ValidateJson(bad, &catalog, errs);
            bool found = false;
            for (const auto& e : errs) if (e.find("unknown foreach field") != std::string::npos) found = true;
            Check(!ok && found, "093-2: validate rejects unknown ${t.<field>} references");

            std::string noCmd = std::string(R"JSON({"id":"fe-nocmd","cwd":")JSON") +
                JsonEscape(WideToUtf8(d)) +
                R"JSON(","root":{"type":"foreach","id":"smokes","glob":"tests/*_smoke.cpp","as":"t","do":{"type":"job","id":"x","summary":"s","timeoutSec":30}}})JSON";
            errs.clear();
            Check(!BehaviorTree::ValidateJson(noCmd, &catalog, errs),
                  "093-2: validate checks the unexpanded do template");
        }
    }
}

} // namespace bt_test
