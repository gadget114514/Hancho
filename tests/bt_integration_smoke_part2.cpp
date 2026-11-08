// Headless integration smoke test for BtManager + TaskQueueManager + ProcessManager (Part 2).

#include "bt_integration_smoke_common.h"

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

            std::string jobId = "bt-tq-guard-1";
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
            Check(h.tqm.GetJobSnapshot("bt-cancel-job-1", cancelledSnap) &&
                  cancelledSnap.state == "cancelled",
                  "Cancel(id) cancelled the underlying TaskQueueJob");
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
}

} // namespace bt_int_test
