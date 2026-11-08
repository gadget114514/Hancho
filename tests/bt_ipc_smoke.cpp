// Headless smoke test for BT IPC actions (src/ipc_server.cpp + BtManager + TaskQueueManager + ProcessManager).
// Build: cmake --build build --config Release --target bt_ipc_smoke
// Run:   build\Release\bt_ipc_smoke.exe

#include "bt_manager.h"
#include "common.h"
#include "ipc_server.h"
#include "nlohmann/json.hpp"
#include "process_manager.h"
#include "taskqueue.h"

#include <windows.h>
#include <cstdio>
#include <string>

using namespace pc;
using json = nlohmann::json;

namespace {

int g_failures = 0;
int g_passes = 0;

void Check(bool ok, const std::string& msg) {
    if (ok) {
        ++g_passes;
    } else {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
    }
}

void RemoveDirRecursive(const std::wstring& dir) {
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring full = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                RemoveDirRecursive(full);
            } else {
                SetFileAttributesW(full.c_str(), FILE_ATTRIBUTE_NORMAL);
                DeleteFileW(full.c_str());
            }
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

struct IpcTestHarness {
    ProcessManager pm;
    TaskQueueManager tqm;
    std::unique_ptr<BtManager> btm;
    std::unique_ptr<IpcServer> ipc;

    IpcTestHarness() {
        tqm.SetProcessManager(&pm);
        pm.SetProcessFinishedCallback([this](int pid, int exitCode, const std::string& kind) {
            tqm.OnProcessFinished(pid, exitCode, kind);
        });
        btm = std::make_unique<BtManager>(pm, tqm);
        ipc = std::make_unique<IpcServer>(pm, tqm, btm.get());
    }

    ~IpcTestHarness() {
        ipc.reset();
        pm.SetProcessFinishedCallback(nullptr);
        if (btm) {
            btm->Shutdown();
            btm.reset();
        }
        tqm.ShutdownTaskQueueManager();
    }

    json Call(const json& req) {
        std::string raw = ipc->HandleRequestJson(req.dump());
        json parsed = json::parse(raw, nullptr, false);
        if (parsed.is_discarded()) {
            return json{{"ok", false}, {"error", "discarded JSON: " + raw}};
        }
        return parsed;
    }
};

} // namespace

int main() {
    wchar_t tempBase[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempBase);
    std::wstring tempDirW = std::wstring(tempBase) + L"pc_bt_ipc_smoke_" + std::to_wstring(GetCurrentProcessId());
    RemoveDirRecursive(tempDirW);
    CreateDirectoryW(tempDirW.c_str(), nullptr);
    SetDataDirectoryOverride(tempDirW);
    std::string tempDirUtf8 = WideToUtf8(tempDirW);

    {
        IpcTestHarness h;

        // 0. Null BtManager guard on a separate IpcServer without BtManager
        {
            IpcServer noBtIpc(h.pm, h.tqm);
            json r = json::parse(noBtIpc.HandleRequestJson(R"({"action":"bt_list"})"), nullptr, false);
            Check(!r.value("ok", true) && r.value("error", "") == "behavior trees are not available",
                  "IpcServer with null BtManager returns 'behavior trees are not available'");
        }

        // 1. bt_validate (good + bad)
        {
            json goodBt = {
                {"id", "ipc-val-good"},
                {"cwd", tempDirUtf8},
                {"root", {
                    {"type", "job"},
                    {"id", "j1"},
                    {"summary", "Good job"},
                    {"command", "cmd /c exit 0"},
                    {"timeoutSec", 10}
                }}
            };
            json rGood = h.Call({{"action", "bt_validate"}, {"bt", goodBt}});
            Check(rGood.value("ok", false) && rGood.value("valid", false),
                  "bt_validate accepts valid BT definition");

            json badBt = {
                {"id", "ipc-val-bad"},
                {"root", {
                    {"type", "retry"},
                    {"max", 999},
                    {"child", {{"type", "succeed"}}}
                }}
            };
            json rBad = h.Call({{"action", "bt_validate"}, {"bt", badBt}});
            Check(!rBad.value("ok", true) && !rBad.value("valid", true) && !rBad.value("error", "").empty(),
                  "bt_validate rejects invalid retry.max=999");
        }

        // 2. bt_start of a two-job BT (cmd /c exit 0), bt_wait until finished, bt_get compact, bt_list
        {
            json twoJobBt = {
                {"id", "ipc-two-jobs"},
                {"summary", "Two job IPC test"},
                {"cwd", tempDirUtf8},
                {"root", {
                    {"type", "sequence"},
                    {"children", json::array({
                        {
                            {"type", "job"},
                            {"id", "job_a"},
                            {"summary", "First job"},
                            {"command", "cmd /c exit 0"},
                            {"timeoutSec", 30}
                        },
                        {
                            {"type", "job"},
                            {"id", "job_b"},
                            {"summary", "Second job"},
                            {"command", "cmd /c exit 0"},
                            {"timeoutSec", 30}
                        }
                    })}
                }}
            };

            json rStart = h.Call({{"action", "bt_start"}, {"bt", twoJobBt}});
            Check(rStart.value("ok", false) && rStart.value("id", "") == "ipc-two-jobs" &&
                      !rStart.contains("tree") && rStart.contains("compact") && rStart.contains("version"),
                  "bt_start registers and starts ipc-two-jobs");

            json rWait = h.Call({{"action", "bt_wait"}, {"id", "ipc-two-jobs"}, {"sinceVersion", 0}, {"timeoutMs", 15000}});
            Check(rWait.value("ok", false) && rWait.value("state", "") == "succeeded",
                  "bt_wait finishes ipc-two-jobs with state=succeeded (got " + rWait.value("state", "") + ")");

            json rWaitCompact = h.Call({{"action", "bt_wait"}, {"id", "ipc-two-jobs"}, {"sinceVersion", 0}, {"timeoutMs", 1000}, {"compact", true}});
            Check(rWaitCompact.value("ok", false) && !rWaitCompact.contains("tree") && rWaitCompact.contains("compact"),
                  "bt_wait with compact:true omits tree and includes compact summary");

            json rGetCompact = h.Call({{"action", "bt_get"}, {"id", "ipc-two-jobs"}, {"compact", true}});
            std::string compactStr = rGetCompact.value("compact", "");
            Check(rGetCompact.value("ok", false) && !rGetCompact.contains("tree") &&
                  compactStr.find("[succeeded] ipc-two-jobs") != std::string::npos &&
                  compactStr.find("[succeeded] job job_a") != std::string::npos &&
                  compactStr.find("[succeeded] job job_b") != std::string::npos,
                  "bt_get compact includes one-line-per-node compact representation");

            json rList = h.Call({{"action", "bt_list"}});
            bool foundInList = false;
            if (rList.value("ok", false) && rList["trees"].is_array()) {
                for (const auto& item : rList["trees"]) {
                    if (item.value("id", "") == "ipc-two-jobs" && item.value("state", "") == "succeeded") {
                        foundInList = true;
                    }
                }
            }
            Check(foundInList, "bt_list includes ipc-two-jobs with state=succeeded");
        }

        // 3. escalate answered with bt_resume (and missing rationale rejected)
        {
            json escBt = {
                {"id", "ipc-escalate"},
                {"intent", "Verify escalation over IPC"},
                {"cwd", tempDirUtf8},
                {"root", {
                    {"type", "sequence"},
                    {"children", json::array({
                        {
                            {"type", "escalate"},
                            {"id", "ask_llm"},
                            {"question", "Continue or stop?"},
                            {"expect", json::array({"continue", "stop"})},
                            {"timeoutSec", 300}
                        },
                        {
                            {"type", "if"},
                            {"cond", {{"type", "var"}, {"name", "ask_llm.answer"}, {"eq", "continue"}}},
                            {"then", {
                                {"type", "job"},
                                {"id", "post_esc"},
                                {"summary", "Post escalation job"},
                                {"command", "cmd /c exit 0"},
                                {"timeoutSec", 30}
                            }},
                            {"else", {{"type", "fail"}, {"detail", "stopped"}}}
                        }
                    })}
                }}
            };

            h.Call({{"action", "bt_start"}, {"bt", escBt}});
            json rWaitEsc = h.Call({{"action", "bt_wait"}, {"id", "ipc-escalate"}, {"sinceVersion", 0}, {"timeoutMs", 5000}});
            Check(rWaitEsc.value("ok", false) && rWaitEsc.value("state", "") == "waitingForLlm" &&
                  rWaitEsc.contains("escalation"),
                  "bt_wait returns waitingForLlm and escalation packet");

            // Missing rationale must be rejected
            json rNoRat = h.Call({
                {"action", "bt_resume"},
                {"id", "ipc-escalate"},
                {"node", "ask_llm"},
                {"answer", "continue"},
                {"rationale", ""}
            });
            Check(!rNoRat.value("ok", true),
                  "bt_resume with empty rationale is rejected");

            // Valid resume with rationale
            json rResume = h.Call({
                {"action", "bt_resume"},
                {"id", "ipc-escalate"},
                {"node", "ask_llm"},
                {"answer", "continue"},
                {"rationale", "Checks passed, continuing"},
                {"vars", {{"reviewer", "smoke"}}}
            });
            Check(rResume.value("ok", false),
                  "bt_resume with valid answer and rationale succeeds");

            json rWaitDone = h.Call({{"action", "bt_wait"}, {"id", "ipc-escalate"}, {"sinceVersion", 0}, {"timeoutMs", 15000}});
            Check(rWaitDone.value("ok", false) && rWaitDone.value("state", "") == "succeeded",
                  "ipc-escalate completes with state=succeeded after bt_resume");
        }

        // 4, 7, & 8. Failure -> taskqueue_retry rejected -> bt_update of non-running node -> bt_retry succeeds
        {
            json failUpdateBt = {
                {"id", "ipc-fail-update-retry"},
                {"cwd", tempDirUtf8},
                {"root", {
                    {"type", "sequence"},
                    {"children", json::array({
                        {
                            {"type", "job"},
                            {"id", "step_ok"},
                            {"summary", "Initial ok step"},
                            {"command", "cmd /c exit 0"},
                            {"timeoutSec", 30}
                        },
                        {
                            {"type", "job"},
                            {"id", "step_broken"},
                            {"summary", "Initially broken step"},
                            {"command", "cmd /c exit 1"},
                            {"timeoutSec", 30}
                        }
                    })}
                }}
            };

            h.Call({{"action", "bt_start"}, {"bt", failUpdateBt}});
            json rFail = h.Call({{"action", "bt_wait"}, {"id", "ipc-fail-update-retry"}, {"sinceVersion", 0}, {"timeoutMs", 15000}});
            Check(rFail.value("ok", false) && rFail.value("state", "") == "failed",
                  "ipc-fail-update-retry fails on step_broken");

            // External taskqueue_retry on BT job ipc-fail-update-retry-2 must be rejected
            json rTqRetry = h.Call({
                {"action", "taskqueue_retry"},
                {"id", "ipc-fail-update-retry-2"}
            });
            Check(!rTqRetry.value("ok", true) &&
                  rTqRetry.value("error", "") == "this job is managed by a behavior tree; use bt_retry",
                  "taskqueue_retry of a BT job is rejected via IPC");

            // Shape-changing bt_update (changing type) must be rejected
            json rBadShape = h.Call({
                {"action", "bt_update"},
                {"id", "ipc-fail-update-retry"},
                {"node", "step_broken"},
                {"bt", {{"id", "step_broken"}, {"type", "succeed"}}}
            });
            Check(!rBadShape.value("ok", true),
                  "bt_update changing node type is rejected");

            // Valid bt_update of non-running node step_broken to exit 0
            json rUpdate = h.Call({
                {"action", "bt_update"},
                {"id", "ipc-fail-update-retry"},
                {"node", "step_broken"},
                {"bt", {
                    {"id", "step_broken"},
                    {"type", "job"},
                    {"summary", "Fixed step"},
                    {"command", "cmd /c exit 0"},
                    {"timeoutSec", 30}
                }}
            });
            json gUpdated = h.Call({{"action", "bt_get"}, {"id", "ipc-fail-update-retry"}});
            Check(rUpdate.value("ok", false) && !rUpdate.contains("tree") &&
                  gUpdated["tree"]["root"]["children"][1].value("command", "") == "cmd /c exit 0",
                  "bt_update replaces command of non-running node step_broken");

            // Now bt_retry from step_broken
            json rRetry = h.Call({
                {"action", "bt_retry"},
                {"id", "ipc-fail-update-retry"},
                {"from", "step_broken"},
                {"resetCounters", false}
            });
            Check(rRetry.value("ok", false),
                  "bt_retry from step_broken succeeds");

            json rRetryDone = h.Call({{"action", "bt_wait"}, {"id", "ipc-fail-update-retry"}, {"sinceVersion", 0}, {"timeoutMs", 15000}});
            Check(rRetryDone.value("ok", false) && rRetryDone.value("state", "") == "succeeded",
                  "ipc-fail-update-retry succeeds after bt_update + bt_retry");
        }

        // 5 & 6. bt_pause and bt_cancel (plus rejecting bt_update on a running node)
        {
            json longBt = {
                {"id", "ipc-pause-cancel"},
                {"cwd", tempDirUtf8},
                {"root", {
                    {"type", "wait"},
                    {"id", "hold_wait"},
                    {"sec", 60}
                }}
            };

            h.Call({{"action", "bt_start"}, {"bt", longBt}});

            // Updating a running node must be rejected
            json rUpdRunning = h.Call({
                {"action", "bt_update"},
                {"id", "ipc-pause-cancel"},
                {"node", "hold_wait"},
                {"bt", {{"id", "hold_wait"}, {"type", "wait"}, {"sec", 5}}}
            });
            Check(!rUpdRunning.value("ok", true),
                  "bt_update on a running node is rejected");

            json rPause = h.Call({{"action", "bt_pause"}, {"id", "ipc-pause-cancel"}, {"paused", true}});
            Check(rPause.value("ok", false) && rPause.value("state", "") == "paused",
                  "bt_pause(paused=true) sets state to paused");

            json rUnpause = h.Call({{"action", "bt_pause"}, {"id", "ipc-pause-cancel"}, {"paused", false}});
            Check(rUnpause.value("ok", false) && rUnpause.value("state", "") == "running",
                  "bt_pause(paused=false) resumes state to running");

            json rCancel = h.Call({{"action", "bt_cancel"}, {"id", "ipc-pause-cancel"}});
            Check(rCancel.value("ok", false) && rCancel.value("state", "") == "cancelled",
                  "bt_cancel sets state to cancelled");
        }

        // 7. A finished BT started again under the same id gets fresh job ids (the old jobs are
        //    still in the queue), and a plain job does not set ${<id>.result}.
        {
            json reuseBt = {
                {"id", "ipc-reuse"},
                {"cwd", tempDirUtf8},
                {"root", {
                    {"type", "job"},
                    {"id", "only"},
                    {"summary", "Reused job"},
                    {"command", "cmd /c echo hello"},
                    {"timeoutSec", 30}
                }}
            };
            auto runToEnd = [&](const char* label) {
                json r = h.Call({{"action", "bt_start"}, {"bt", reuseBt}});
                Check(r.value("ok", false), std::string(label) + ": bt_start ok");
                std::string state;
                int64_t since = 0;
                DWORD deadline = GetTickCount() + 15000;
                while (GetTickCount() < deadline) {
                    json w = h.Call({{"action", "bt_wait"}, {"id", "ipc-reuse"}, {"sinceVersion", since}, {"timeoutMs", 3000}});
                    state = w.value("state", "");
                    since = w.value("version", since);
                    if (state == "succeeded" || state == "failed" || state == "cancelled") break;
                }
                return state;
            };

            Check(runToEnd("first run") == "succeeded", "first run of ipc-reuse succeeds");
            json g1 = h.Call({{"action", "bt_get"}, {"id", "ipc-reuse"}});
            json vars1 = g1.contains("tree") ? g1["tree"].value("vars", json::object()) : json::object();
            Check(!vars1.contains("only.result"), "a plain job does not set ${only.result}: " + vars1.dump());

            Check(runToEnd("second run") == "succeeded",
                  "second run of ipc-reuse succeeds even though job ipc-reuse-1 is still queued");
            TaskQueueJob reused;
            Check(h.tqm.GetJobSnapshot("ipc-reuse-1-r2", reused) && reused.bt.id == "ipc-reuse" &&
                      reused.state == "succeeded",
                  "second run's job got the id ipc-reuse-1-r2");
        }
    }

    RemoveDirRecursive(tempDirW);
    std::printf("%s (%d passed, %d failures)\n", g_failures ? "FAILED" : "ALL PASSED", g_passes, g_failures);
    return g_failures ? 1 : 0;
}
