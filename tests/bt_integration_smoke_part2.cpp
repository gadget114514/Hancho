// Headless integration smoke test for BtManager + TaskQueueManager + ProcessManager (Part 2).

#include "bt_integration_smoke_common.h"
#include "triage_manager.h"

namespace bt_int_test {

void RunPart2IntegrationTests(const std::wstring& tempDirW, const std::string& tempDirUtf8) {
    {
        TestHarness h;

        // 6 & 7. External taskqueue_retry / taskqueue_update guard + GUI TaskQueueRetry forwarding
        {
            std::wstring guiFlagW = tempDirW + L"\\gui_retry_ready.flag";
            DeleteFileW(guiFlagW.c_str());

            json btDef = {
                {"id", "bt-tq-guard"},
                {"cwd", tempDirUtf8},
                {"root", {
                    {"type", "job"},
                    {"id", "guarded_job"},
                    {"summary", "Guarded BT job"},
                    {"command", "cmd.exe /c if exist gui_retry_ready.flag (exit 0) else (exit 1)"},
                    {"timeoutSec", 30}
                }}
            };

            h.btm->Start(btDef.dump());
            json waitFail = json::parse(h.btm->Wait("bt-tq-guard", 0, 15000), nullptr, false);
            Check(waitFail.value("state", "") == "failed",
                  "bt-tq-guard failed initially");

            // A failed tree is not over: its group row stays listed (without planned work)
            // so it can still be retried or closed from the task list.
            auto groupState = [&](const std::string& id) {
                for (const auto& g : h.btm->GetRemainingInfo().groups) {
                    if (g.id == id) return g.state;
                }
                return std::string();
            };
            Check(groupState("bt-tq-guard") == "failed",
                  "failed tree keeps its task-group row");

            std::string jobId = "bt-tq-guard-guarded_job-1";
            TaskQueueJob snap;
            Check(h.tqm.GetJobSnapshot(jobId, snap) && snap.bt.id == "bt-tq-guard" &&
                  snap.bt.node == "guarded_job" && snap.bt.run == 1,
                  "TaskQueueJob carries bt reference {id, node, run}");

            // External TaskQueueRetry must be rejected
            std::string extErr;
            json extRetry = json::parse(
                h.tqm.TaskQueueRetry(jobId, true, true, TaskQueueCaller::External, extErr),
                nullptr, false);
            Check(!extRetry.value("ok", true) &&
                  extErr == "this job is managed by a behavior tree; use bt_retry",
                  "External TaskQueueRetry rejected with exact error: " + extErr);

            // External TaskQueueUpdate must be rejected
            std::string updErr;
            json extUpd = json::parse(
                h.tqm.TaskQueueUpdate(jobId, R"({"summary":"hacked"})", TaskQueueCaller::External, updErr),
                nullptr, false);
            Check(!extUpd.value("ok", true) &&
                  updErr == "this job is managed by a behavior tree; use bt_retry",
                  "External TaskQueueUpdate rejected with exact error: " + updErr);

            // External TaskQueueRetryFailed must skip BT-owned jobs
            std::string bulkErr;
            json extBulk = json::parse(
                h.tqm.TaskQueueRetryFailed("bt-bt-tq-guard", 0, true, true, TaskQueueCaller::External, bulkErr),
                nullptr, false);
            Check(extBulk.value("ok", false) && extBulk.value("retried_count", -1) == 0,
                  "External TaskQueueRetryFailed skips BT-owned jobs");

            // GUI TaskQueueRetry must forward to BtManager::Retry and restart the node
            WriteFileAtomicWide(guiFlagW, "ready");
            std::string guiErr;
            json guiRetry = json::parse(
                h.tqm.TaskQueueRetry(jobId, true, true, TaskQueueCaller::Gui, guiErr),
                nullptr, false);
            Check(guiRetry.value("ok", false) && guiRetry.value("bt_retried", false),
                  "GUI TaskQueueRetry forwarded to BtManager::Retry: " + guiErr);

            json waitGuiOk = json::parse(h.btm->Wait("bt-tq-guard", 0, 15000), nullptr, false);
            Check(waitGuiOk.value("state", "") == "succeeded",
                  "bt-tq-guard reached succeeded after GUI TaskQueueRetry forwarding");
            Check(groupState("bt-tq-guard").empty(),
                  "succeeded tree drops its task-group row");

            // Closing a failed tree (bt_cancel) marks it cancelled and drops its row.
            json closeDef = {
                {"id", "bt-close-failed"},
                {"cwd", tempDirUtf8},
                {"root", {{"type", "job"}, {"id", "always_fail"}, {"summary", "Always fails"},
                          {"command", "cmd.exe /c exit 1"}, {"timeoutSec", 30}}}
            };
            h.btm->Start(closeDef.dump());
            json closeWait = json::parse(h.btm->Wait("bt-close-failed", 0, 15000), nullptr, false);
            Check(closeWait.value("state", "") == "failed" && groupState("bt-close-failed") == "failed",
                  "bt-close-failed failed and stays listed");
            json closed = json::parse(h.btm->Cancel("bt-close-failed"), nullptr, false);
            Check(closed.value("state", "") == "cancelled" && groupState("bt-close-failed").empty(),
                  "Cancel(id) closes a failed tree and drops its task-group row");
        }

        // 8. Pause and Cancel
        {
            json pauseBt = {
                {"id", "bt-pause-cancel"},
                {"cwd", tempDirUtf8},
                {"root", {
                    {"type", "sequence"},
                    {"children", json::array({
                        {
                            {"type", "wait"},
                            {"id", "w_hold"},
                            {"sec", 60}
                        }
                    })}
                }}
            };

            h.btm->Start(pauseBt.dump());
            json pTrue = json::parse(h.btm->Pause("bt-pause-cancel", true), nullptr, false);
            Check(pTrue.value("ok", false) && pTrue.value("state", "") == "paused",
                  "Pause(id, true) sets tree state to paused");

            json pFalse = json::parse(h.btm->Pause("bt-pause-cancel", false), nullptr, false);
            Check(pFalse.value("ok", false) && pFalse.value("state", "") == "running",
                  "Pause(id, false) resumes tree state to running");

            // Now test Cancel on a tree with a running job
            json cancelBt = {
                {"id", "bt-cancel-job"},
                {"cwd", tempDirUtf8},
                {"root", {
                    {"type", "job"},
                    {"id", "long_ping"},
                    {"summary", "Long running ping"},
                    {"command", "ping -n 30 127.0.0.1"},
                    {"timeoutSec", 60}
                }}
            };

            h.btm->Start(cancelBt.dump());
            json cancelResp = json::parse(h.btm->Cancel("bt-cancel-job"), nullptr, false);
            Check(cancelResp.value("ok", false) && cancelResp.value("state", "") == "cancelled",
                  "Cancel(id) marks tree state cancelled");

            TaskQueueJob cancelledSnap;
            Check(h.tqm.GetJobSnapshot("bt-cancel-job-long_ping-1", cancelledSnap) &&
                  cancelledSnap.state == "cancelled",
                  "Cancel(id) cancelled the underlying TaskQueueJob");

            // Two parallel leaves in a concurrency-1 queue: the first runs, the second stays
            // queued. Cancelling the tree must cancel that provisionally registered job too.
            json queuedBt = {
                {"id", "bt-cancel-queued"},
                {"cwd", tempDirUtf8},
                {"root", {
                    {"type", "parallel"},
                    {"children", json::array({
                        {
                            {"type", "job"},
                            {"id", "first_ping"},
                            {"summary", "First ping"},
                            {"command", "ping -n 30 127.0.0.1"},
                            {"timeoutSec", 60}
                        },
                        {
                            {"type", "job"},
                            {"id", "queued_ping"},
                            {"summary", "Queued ping"},
                            {"command", "ping -n 30 127.0.0.1"},
                            {"timeoutSec", 60}
                        }
                    })}
                }}
            };
            h.btm->Start(queuedBt.dump());
            TaskQueueJob queuedSnap;
            bool sawQueued = false;
            for (int i = 0; i < 100; ++i) {
                if (h.tqm.GetJobSnapshot("bt-cancel-queued-queued_ping-1", queuedSnap) &&
                    (queuedSnap.state == "pending" || queuedSnap.state == "waiting")) {
                    sawQueued = true;
                    break;
                }
                Sleep(20);
            }
            Check(sawQueued, "the second parallel leaf is registered while the first runs");
            json queuedCancelResp = json::parse(h.btm->Cancel("bt-cancel-queued"), nullptr, false);
            Check(queuedCancelResp.value("ok", false) &&
                      queuedCancelResp.value("state", "") == "cancelled",
                  "Cancel(id) marks a tree with queued leaves cancelled");
            TaskQueueJob queuedAfter;
            Check(h.tqm.GetJobSnapshot("bt-cancel-queued-queued_ping-1", queuedAfter) &&
                      queuedAfter.state == "cancelled",
                  "Cancel(id) also cancels provisionally registered queued jobs");
        }
    }

    // 9. Persistence + startup recovery across a brand-new BtManager instance
    {
        // Prepare a persisted running BT state file whose job "bt-recover-ok-1" already succeeded in TaskQueue,
        // and another persisted running BT state file whose job "bt-recover-missing-1" is missing from TaskQueue.
        {
            ProcessManager pm1;
            TaskQueueManager tqm1;
            tqm1.SetProcessManager(&pm1);
            pm1.SetProcessFinishedCallback([&tqm1](int pid, int ec, const std::string& kind) {
                tqm1.OnProcessFinished(pid, ec, kind);
            });

            std::string addErr;
            TaskQueueBtRef ref{"bt-recover-ok", "bt-recover-ok/step1", 1};
            json jobObj = {
                {"id", "bt-recover-ok-1"},
                {"summary", "Recover step 1"},
                {"command", "cmd.exe /c echo RECOVERED_OK"},
                {"cwd", tempDirUtf8},
                {"timeout_sec", 30}
            };
            tqm1.AddBtJob("bt-bt-recover-ok", jobObj.dump(), ref, addErr);

            // Wait for bt-recover-ok-1 to finish in tqm1 without BtManager attached
            for (int i = 0; i < 100; ++i) {
                TaskQueueJob s;
                if (tqm1.GetJobSnapshot("bt-recover-ok-1", s) && s.state == "succeeded") break;
                Sleep(50);
            }
            pm1.SetProcessFinishedCallback(nullptr);
            tqm1.ShutdownTaskQueueManager();
        }

        // Write a running state file for bt-recover-ok attached to bt-recover-ok-1
        json recoverOkState = {
            {"id", "bt-recover-ok"},
            {"summary", "Recovery test ok"},
            {"cwd", tempDirUtf8},
            {"state", "running"},
            {"nodeRuns", 1},
            {"vars", json::object()},
            {"root", {
                {"type", "job"},
                {"id", "step1"},
                {"summary", "Recover step 1"},
                {"command", "cmd.exe /c echo RECOVERED_OK"},
                {"timeoutSec", 30},
                {"status", {
                    {"state", "running"},
                    {"runs", 1},
                    {"jobs", json::array({"bt-recover-ok-1"})}
                }}
            }}
        };
        WriteFileAtomicWide(BtManager::GetBtStateFileWide("bt-recover-ok"), recoverOkState.dump(2));

        // Write a running state file for bt-recover-missing attached to non-existent job bt-recover-missing-1
        json recoverMissingState = {
            {"id", "bt-recover-missing"},
            {"summary", "Recovery test missing job"},
            {"cwd", tempDirUtf8},
            {"state", "running"},
            {"nodeRuns", 1},
            {"vars", json::object()},
            {"root", {
                {"type", "job"},
                {"id", "step_missing"},
                {"summary", "Missing job step"},
                {"command", "cmd.exe /c echo MISSING"},
                {"timeoutSec", 30},
                {"status", {
                    {"state", "running"},
                    {"runs", 1},
                    {"jobs", json::array({"bt-recover-missing-1"})}
                }}
            }}
        };
        WriteFileAtomicWide(BtManager::GetBtStateFileWide("bt-recover-missing"), recoverMissingState.dump(2));

        // Now start a new TestHarness over the same temp data directory
        TestHarness h2;
        json listResp = json::parse(h2.btm->List(), nullptr, false);
        Check(listResp.value("ok", false) && listResp["trees"].is_array() && listResp["trees"].size() >= 7,
              "new BtManager List() loaded persisted trees from disk");

        json recOk = json::parse(h2.btm->Get("bt-recover-ok", false), nullptr, false);
        Check(recOk.value("ok", false) && recOk.value("state", "") == "succeeded",
              "startup recovery reconciled finished job bt-recover-ok-1 and marked tree succeeded");

        json recCompact = json::parse(h2.btm->Get("bt-recover-ok", true), nullptr, false);
        Check(recCompact.value("ok", false) && recCompact.value("state", "") == "succeeded",
              "Get(id, compact=true) returns valid compact tree JSON");

        json recMissing = json::parse(h2.btm->Get("bt-recover-missing", false), nullptr, false);
        Check(recMissing.value("ok", false) &&
              recMissing.value("state", "") == "failed" &&
              recMissing.value("detail", "") == "jobMissing",
              "startup recovery marked missing job bt-recover-missing-1 as failed with jobMissing");
    }

    // 12. BT conditions read normalized hancho.event/1 fields from a .events.jsonl file.
    {
        TestHarness h;
        std::string eventsContent =
            R"({"schema":"hancho.event/1","seq":1,"kind":"message","data":{"text":"start"}})" "\r\n"
            R"({"schema":"hancho.event/1","seq":2,"kind":"tool.call","data":{"tool":"Edit","activity":"edit","target":"src/a.cpp"},"failure":{"class":"success_check_failed"}})" "\r\n";
        WriteFileAtomicWide(tempDirW + L"\\v1_conditions.events.jsonl", eventsContent);

        json btDef = {
            {"id", "bt-v1-conditions"},
            {"cwd", tempDirUtf8},
            {"root", {
                {"type", "sequence"},
                {"children", json::array({
                    {{"type", "jsonValue"}, {"id", "cond_kind"}, {"file", "v1_conditions.events.jsonl"},
                     {"path", "kind"}, {"eq", "tool.call"}},
                    {{"type", "jsonValue"}, {"id", "cond_activity"}, {"file", "v1_conditions.events.jsonl"},
                     {"path", "activity"}, {"eq", "edit"}},
                    {{"type", "jsonValue"}, {"id", "cond_failure"}, {"file", "v1_conditions.events.jsonl"},
                     {"path", "failure.class"}, {"eq", "success_check_failed"}},
                    {{"type", "jsonValue"}, {"id", "cond_last"}, {"file", "v1_conditions.events.jsonl"},
                     {"path", "last.data.target"}, {"eq", "src/a.cpp"}}
                })}
            }}
        };
        h.btm->Start(btDef.dump());
        json waitResp = json::parse(h.btm->Wait("bt-v1-conditions", 0, 15000), nullptr, false);
        Check(waitResp.value("state", "") == "succeeded",
              "BT conditions read v1 kind/activity/failure from .events.jsonl (got " +
                  waitResp.value("state", "") + ")");

        json btMiss = {
            {"id", "bt-v1-conditions-miss"},
            {"cwd", tempDirUtf8},
            {"root", {
                {"type", "jsonValue"},
                {"id", "cond_kind_miss"},
                {"file", "v1_conditions.events.jsonl"},
                {"path", "kind"},
                {"eq", "message"}
            }}
        };
        h.btm->Start(btMiss.dump());
        json missResp = json::parse(h.btm->Wait("bt-v1-conditions-miss", 0, 15000), nullptr, false);
        Check(missResp.value("state", "") == "failed",
              "BT condition compares the last v1 event, not an earlier one");
    }

    // 13. A failure record stays readable after later queue events are appended.
    {
        TestHarness h;
        std::string eventsContent =
            R"({"schema":"hancho.event/1","seq":1,"kind":"run.started"})" "\r\n"
            R"({"version":7,"seq":7,"type":"jobFailed","job_id":"j1","failure":{"class":"rate_limited","suggestedAction":"retry_after","summary":"five_hour limit"}})" "\r\n"
            R"({"version":8,"seq":8,"type":"jobQueued","job_id":"j2"})" "\r\n";
        WriteFileAtomicWide(tempDirW + L"\\failure_tail.events.jsonl", eventsContent);

        json btDef = {
            {"id", "bt-failure-tail"},
            {"cwd", tempDirUtf8},
            {"root", {
                {"type", "sequence"},
                {"children", json::array({
                    {{"type", "jsonValue"}, {"id", "cond_failure_class"}, {"file", "failure_tail.events.jsonl"},
                     {"path", "failure.class"}, {"eq", "rate_limited"}},
                    {{"type", "jsonValue"}, {"id", "cond_failure_action"}, {"file", "failure_tail.events.jsonl"},
                     {"path", "failure.suggestedAction"}, {"eq", "retry_after"}},
                    {{"type", "jsonValue"}, {"id", "cond_tail_kind"}, {"file", "failure_tail.events.jsonl"},
                     {"path", "last.type"}, {"eq", "jobQueued"}}
                })}
            }}
        };
        h.btm->Start(btDef.dump());
        json waitResp = json::parse(h.btm->Wait("bt-failure-tail", 0, 15000), nullptr, false);
        Check(waitResp.value("state", "") == "succeeded",
              "BT failure.class/suggestedAction use the newest failure even when later events follow (got " +
                  waitResp.value("state", "") + ")");
    }

    // 091-2: bt_get compact text and full tree render catch handlers and the selected handler.
    {
        TestHarness h;
        json btDef = {
            {"id", "bt-catch-display"},
            {"cwd", tempDirUtf8},
            {"root", {
                {"type", "catch"}, {"id", "guard"},
                {"child", {{"type", "fail"}, {"id", "impl"}, {"detail", "boom"}}},
                {"handlers", json::array({
                    {{"class", "*"}, {"do", {{"type", "set"}, {"id", "fix"},
                                             {"var", "fixed"}, {"value", "1"}}}}
                })},
                {"otherwise", "fail"}
            }}
        };
        json startResp = json::parse(h.btm->Start(btDef.dump()), nullptr, false);
        Check(startResp.value("state", "") == "succeeded", "091-2: catch display BT completed");
        json getResp = json::parse(h.btm->Get("bt-catch-display", false), nullptr, false);
        std::string compact = getResp.value("compact", "");
        Check(compact.find("catch guard") != std::string::npos &&
              compact.find("handlers=1") != std::string::npos &&
              compact.find("maxRetries=1") != std::string::npos &&
              compact.find("handler[0]") != std::string::npos,
              "091-2: bt_get compact shows catch handlers/maxRetries/selected handler");
        Check(compact.find("do: set fix") != std::string::npos &&
              compact.find("[failed] fail impl") != std::string::npos,
              "091-2: bt_get compact shows do and child nodes");
        json tree = getResp.value("tree", json::object());
        json rootNode = tree.value("root", json::object());
        Check(rootNode.value("type", "") == "catch" &&
              rootNode.contains("handlers") && rootNode["handlers"].is_array() &&
              rootNode["handlers"].size() == 1 &&
              rootNode["handlers"][0].contains("do") &&
              rootNode["handlers"][0]["do"].value("id", "") == "fix",
              "091-2: bt_get tree keeps catch handlers with their do nodes");
    }

    // 101-3: End-to-end blame attribution through BtManager + TaskQueueManager.
    {
        TestHarness h;
        std::wstring countBatW = tempDirW + L"\\blame_test_cmd.bat";
        std::wstring flag1W = tempDirW + L"\\blame_run1.flag";
        std::wstring flag2W = tempDirW + L"\\blame_run2.flag";
        DeleteFileW(flag1W.c_str());
        DeleteFileW(flag2W.c_str());
        std::string countBat =
            "@echo off\r\n"
            "if not exist blame_run1.flag (\r\n"
            "    echo ok > blame_run1.flag\r\n"
            "    echo [FAIL] integration_case_1\r\n"
            "    exit /b 1\r\n"
            ")\r\n"
            "if not exist blame_run2.flag (\r\n"
            "    echo ok > blame_run2.flag\r\n"
            "    echo [FAIL] integration_case_1\r\n"
            "    exit /b 1\r\n"
            ")\r\n"
            "echo [PASS] integration_case_1\r\n"
            "exit /b 0\r\n";
        WriteFileAtomicWide(countBatW, countBat);

        json btDef = {
            {"id", "bt-blame-int"},
            {"cwd", tempDirUtf8},
            {"root", {
                {"type", "retry"},
                {"max", 4},
                {"child", {
                    {"type", "sequence"},
                    {"children", json::array({
                        {{"type", "worker"}, {"id", "impl"}, {"engine", "test-worker"},
                         {"summary", "Mock worker"}, {"prompt", "Fix it"}, {"timeoutSec", 30}},
                        {{"type", "job"}, {"id", "check"}, {"summary", "Run check"},
                         {"command", QuoteCommandArg(WideToUtf8(countBatW))},
                         {"timeoutSec", 30}, {"blame", "impl"}}
                    })}
                }}
            }}
        };
        h.btm->Start(btDef.dump());
        json waitResp = json::parse(h.btm->Wait("bt-blame-int", 0, 20000), nullptr, false);
        Check(waitResp.value("state", "") == "succeeded",
              "101-3: blame integration tree succeeded after retries (got " + waitResp.value("state", "") + ")");
        json getResp = json::parse(h.btm->Get("bt-blame-int", false), nullptr, false);
        const auto& implSt = getResp["tree"]["root"]["child"]["children"][0]["status"];
        Check(implSt.contains("switchFailCounts") &&
              implSt["switchFailCounts"].value("fix_failed", 0) == 1 &&
              implSt["switchFailCounts"].value("fix_no_progress", 0) == 1 &&
              implSt.contains("recentFailSignatures") &&
              implSt["recentFailSignatures"].size() == 2,
              "101-3: BtManager + TaskQueueManager recorded fix_failed, fix_no_progress, and recentFailSignatures on blamed worker");
    }

    // 101-4: Task-attached BT logs engine_switched to TaskLog on switch.
    {
        std::wstring workerBatW = tempDirW + L"\\mock_worker.bat";
        std::string workerCmd = QuoteCommandArg(WideToUtf8(workerBatW)) + " --model {model} --prompt {prompt}";
        json enginesJson = {
            {"default", "test-worker"},
            {"engines", {
                {"test-worker", {
                    {"command", workerCmd},
                    {"models", json::array({"test-m1", "test-m2"})},
                    {"defaultModel", "test-m1"}
                }}
            }}
        };
        WriteFileAtomicWide(tempDirW + L"\\engines.json", enginesJson.dump(2));

        TestHarness h;
        std::string taskErr;
        json taskCreateResp = json::parse(
            h.tqm.TaskCreate("Switch log task", "Verify engine_switched task log", "", "", taskErr),
            nullptr, false);
        std::string taskId = taskCreateResp.is_object() ? taskCreateResp.value("id", "") : "";
        Check(taskCreateResp.is_object() && taskCreateResp.value("ok", false) && !taskId.empty(),
              "101-4: TaskCreate succeeded: " + taskErr);

        json btDef = {
            {"id", "bt-switch-tasklog"},
            {"taskId", taskId},
            {"cwd", tempDirUtf8},
            {"root", {
                {"type", "job"},
                {"id", "switch_job"},
                {"summary", "Switch job"},
                {"command", "cmd.exe /c if \"{model}\"==\"test-m1\" (exit 1) else (exit 0)"},
                {"timeoutSec", 30},
                {"engines", json::array({
                    {{"engine", "test-worker"}, {"model", "test-m1"}},
                    {{"engine", "test-worker"}, {"model", "test-m2"}}
                })},
                {"switchEngineOn", json::array({"exit_nonzero"})}
            }}
        };
        h.btm->Start(btDef.dump());
        json waitResp = json::parse(h.btm->Wait("bt-switch-tasklog", 0, 15000), nullptr, false);
        Check(waitResp.value("state", "") == "succeeded",
              "101-4: bt-switch-tasklog succeeded after switch (got " + waitResp.value("state", "") + ")");

        std::string showErr;
        json taskShown = json::parse(h.tqm.TaskShow(taskId, 50, showErr), nullptr, false);
        bool hasSwitchedLog = false;
        if (taskShown.contains("log") && taskShown["log"].is_array()) {
            for (const auto& entry : taskShown["log"]) {
                if (entry.value("kind", "") == "engine_switched" &&
                    entry.value("bt", "") == "bt-switch-tasklog" &&
                    entry.value("text", "").find("test-worker/test-m1 -> test-worker/test-m2") != std::string::npos) {
                    hasSwitchedLog = true;
                }
            }
        }
        Check(hasSwitchedLog, "101-4: TaskLog recorded engine_switched entry on task");
    }

    // Task history: a retry inside a task's BT is logged as bt_node_failed (node, run, job), and
    // each job attempt as attempt_finished, even though the retry is not a lineage.
    {
        TestHarness h;
        std::string taskErr;
        json created = json::parse(h.tqm.TaskCreate("Retry history", "", "", "", taskErr), nullptr, false);
        const std::string taskId = created.is_object() ? created.value("id", "") : "";
        Check(!taskId.empty(), "history: TaskCreate succeeded: " + taskErr);

        // Fails on the first run (creates the flag), succeeds on the next.
        const std::string flag = tempDirUtf8 + "\\retry_history.flag";
        DeleteFileW(Utf8ToWide(flag).c_str());
        const std::wstring flakyBatW = tempDirW + L"\\flaky_once.bat";
        WriteFileAtomicWide(flakyBatW,
                            "@echo off\r\n"
                            "if exist \"%~dp0retry_history.flag\" exit /b 0\r\n"
                            "type nul > \"%~dp0retry_history.flag\"\r\n"
                            "exit /b 1\r\n");
        json btDef = {
            {"id", "bt-retry-history"},
            {"taskId", taskId},
            {"cwd", tempDirUtf8},
            {"root", {
                {"type", "retry"},
                {"max", 3},
                {"child", {
                    {"type", "job"},
                    {"id", "flaky"},
                    {"summary", "Fails once"},
                    {"command", QuoteCommandArg(WideToUtf8(flakyBatW))},
                    {"timeoutSec", 30}
                }}
            }}
        };
        h.btm->Start(btDef.dump());
        json waitResp = json::parse(h.btm->Wait("bt-retry-history", 0, 15000), nullptr, false);
        Check(waitResp.value("state", "") == "succeeded",
              "history: retry BT succeeded on the second run (got " + waitResp.dump() + ")");

        std::string showErr;
        json shown = json::parse(h.tqm.TaskShow(taskId, 0, showErr), nullptr, false);
        int nodeFailed = 0, attemptsOk = 0, attemptsFailed = 0;
        for (const auto& entry : shown.value("log", json::array())) {
            const std::string kind = entry.value("kind", "");
            if (kind == "bt_node_failed" && entry.value("bt", "") == "bt-retry-history" &&
                entry.value("node", "").find("flaky") != std::string::npos && entry.value("run", 0) == 1 &&
                !entry.value("job", "").empty()) {
                ++nodeFailed;
            }
            if (kind == "attempt_finished") {
                if (entry.value("result", "") == "succeeded") ++attemptsOk;
                else ++attemptsFailed;
            }
        }
        Check(nodeFailed == 1, "history: bt_node_failed recorded once for the failed run (got " +
                                   std::to_string(nodeFailed) + ")");
        Check(attemptsFailed == 1 && attemptsOk == 1,
              "history: attempt_finished recorded the failed and the succeeded job");
        DeleteFileW(Utf8ToWide(flag).c_str());
        DeleteFileW(flakyBatW.c_str());
    }

    // 10. BtManager::Start with triage:false parameter and in definition creates direction with triage:false immediately
    {
        TestHarness h;
        json btDef1 = {
            {"id", "bt-triage-flag-def"},
            {"cwd", tempDirUtf8},
            {"triage", false},
            {"root", {
                {"type", "job"},
                {"id", "j1"},
                {"summary", "Job 1"},
                {"command", "cmd.exe /c exit 0"},
                {"timeoutSec", 10}
            }}
        };
        std::string resp1 = h.btm->Start(btDef1.dump());
        json r1 = json::parse(resp1, nullptr, false);
        Check(r1.value("ok", false), "bt_start with triage:false in def succeeded: " + resp1);
        std::string dirId1 = r1.value("directionId", "");
        Check(!dirId1.empty(), "got directionId for bt-triage-flag-def");
        json dirRec1;
        Check(h.tqm.DirectionGet(dirId1, dirRec1) && dirRec1.contains("triage") && !dirRec1["triage"].get<bool>(),
              "bt_start with triage:false in def creates direction with triage:false immediately");

        json btDef2 = {
            {"id", "bt-triage-flag-param"},
            {"cwd", tempDirUtf8},
            {"root", {
                {"type", "job"},
                {"id", "j2"},
                {"summary", "Job 2"},
                {"command", "cmd.exe /c exit 0"},
                {"timeoutSec", 10}
            }}
        };
        std::string resp2 = h.btm->Start(btDef2.dump(), false, false);
        json r2 = json::parse(resp2, nullptr, false);
        Check(r2.value("ok", false), "bt_start with triage=false param succeeded: " + resp2);
        std::string dirId2 = r2.value("directionId", "");
        Check(!dirId2.empty(), "got directionId for bt-triage-flag-param");
        json dirRec2;
        Check(h.tqm.DirectionGet(dirId2, dirRec2) && dirRec2.contains("triage") && !dirRec2["triage"].get<bool>(),
              "bt_start with triage=false param creates direction with triage:false immediately");

        json btDef3 = {
            {"id", "bt-triage-flag-def-true"},
            {"cwd", tempDirUtf8},
            {"root", {
                {"type", "job"},
                {"id", "j3"},
                {"summary", "Job 3"},
                {"command", "cmd.exe /c exit 0"},
                {"timeoutSec", 10}
            }}
        };
        std::string resp3 = h.btm->Start(btDef3.dump());
        json r3 = json::parse(resp3, nullptr, false);
        Check(r3.value("ok", false), "bt_start default succeeded: " + resp3);
        std::string dirId3 = r3.value("directionId", "");
        json dirRec3;
        Check(h.tqm.DirectionGet(dirId3, dirRec3) && dirRec3.value("triage", true),
              "bt_start default has triage:true");
    }

    // 11. Triage redo on card direction delegates to CardRedo + CardRun and uses direction terminology
    {
        TestHarness h;
        std::string tErr;
        std::string taskResp = h.tqm.TaskCreate("Triage Redo Task", "", "", "", tErr);
        json taskDoc = json::parse(taskResp, nullptr, false);
        Check(taskDoc.value("ok", false), "created task for triage redo: " + taskResp);
        const std::string taskId = taskDoc.value("id", "");
        Check(!taskId.empty(), "got taskId for triage redo");

        std::string cardErr;
        std::string addCardResp = h.tqm.CardAdd(taskId, "110-1", "Test Card 110-1", "Card body",
                                                {}, "card", "{}", "", cardErr);
        json addCardJson = json::parse(addCardResp, nullptr, false);
        Check(addCardJson.value("ok", false), "CardAdd succeeded: " + addCardResp);

        std::string runResp = h.btm->CardRun(taskId, "110-1", false, 1, false, "");
        json runDoc = json::parse(runResp, nullptr, false);
        Check(runDoc.value("ok", false), "CardRun succeeded: " + runResp);
        const std::string originalDirId = runDoc.value("directionId", "");
        Check(!originalDirId.empty(), "CardRun returned directionId");
        const std::string btId = runDoc.value("btId", "");
        if (!btId.empty()) {
            h.btm->Cancel(btId);
        }

        TriageManager tm(h.pm, h.tqm, h.btm.get());
        json triageCard = {
            {"key", "dir:" + originalDirId},
            {"id", "card-dir-failure-test"},
            {"kind", "direction_failure"},
            {"state", "pending"},
            {"target", {{"directionId", originalDirId}, {"taskId", taskId}}},
            {"candidates", json::array({
                {{"id", "redo"}, {"name", "Redo"}, {"safe", true}},
                {{"id", "retry"}, {"name", "Retry"}, {"safe", true}}
            })}
        };
        const std::string cardId = tm.AddCard(triageCard);
        Check(!cardId.empty(), "AddCard succeeded: " + cardId);

        const std::string decideResp = tm.Decide(cardId, "redo", "rescued card failure", json::object(), "human");
        json decideDoc = json::parse(decideResp, nullptr, false);
        Check(decideDoc.value("ok", false), "triage redo decide succeeded: " + decideResp);
        const std::string resMsg = decideDoc.value("result", "");
        Check(resMsg.find("Created rescue direction ") != std::string::npos &&
              resMsg.find(" (110-1-r1).") != std::string::npos,
              "result uses direction terminology with rescue number: " + resMsg);

        json origRec;
        Check(h.tqm.DirectionGet(originalDirId, origRec), "got original direction");
        const std::string supersededBy = origRec.value("supersededBy", "");
        Check(!supersededBy.empty(), "original direction marked supersededBy: " + supersededBy);

        json rescueRec;
        Check(h.tqm.DirectionGet(supersededBy, rescueRec), "got rescue direction");
        Check(rescueRec.contains("card") && rescueRec["card"].value("number", "") == "110-1-r1",
              "rescue direction has card.number == 110-1-r1");
    }
}

} // namespace bt_int_test
