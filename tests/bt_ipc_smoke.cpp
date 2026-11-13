// Headless smoke test for BT IPC actions (src/ipc_server.cpp + BtManager + TaskQueueManager + ProcessManager).
// Build: cmake --build build --config Release --target bt_ipc_smoke
// Run:   build\Release\bt_ipc_smoke.exe

#include "bt_manager.h"
#include "common.h"
#include "event_schema.h"
#include "ipc_server.h"
#include "llm_stream.h"
#include "nlohmann/json.hpp"
#include "process_manager.h"
#include "remote_client.h"
#include "remote_ws_test.h"
#include "taskqueue.h"

#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <future>
#include <string>
#include <thread>
#include <vector>

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

// Stream messages carry the normalized event under "data"; direct callbacks may carry it at top level.
std::string StreamEventKind(const json& doc) {
    if (doc.contains("data") && doc["data"].is_object()) {
        return doc["data"].value("kind", "");
    }
    return doc.value("kind", "");
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
                            {"command", "cmd /c ping -n 3 127.0.0.1 >nul"},
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

            // While job_a runs, job_b is an idle BT leaf: it must appear as a provisional row
            // and its estimate must count toward the remaining time.
            json rPlanned = h.Call({{"action", "taskqueue_list"}, {"include_planned", true}});
            bool foundPlanned = false, plannedIsVirtual = false;
            if (rPlanned["jobs"].is_array()) {
                for (const auto& job : rPlanned["jobs"]) {
                    if (job.value("id", "") == "btplan:ipc-two-jobs:job_b") {
                        foundPlanned = true;
                        plannedIsVirtual = job.value("virtual", false) && job.value("state", "") == "planned";
                    }
                }
            }
            Check(foundPlanned, "taskqueue_list include_planned shows the idle BT job");
            Check(plannedIsVirtual, "planned BT row is marked virtual/planned");

            json rPlanCounts = h.Call({{"action", "taskqueue_counts"}});
            Check(rPlanCounts.value("bt_planned", 0) >= 1, "taskqueue_counts includes planned BT rows");
            Check(rPlanCounts.value("sum_remaining_sec", -1) >= 30,
                  "taskqueue_counts adds BT estimates to the remaining time");

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

            json rDoneCounts = h.Call({{"action", "taskqueue_counts"}});
            Check(rDoneCounts.value("bt_planned", -1) == 0,
                  "planned BT rows disappear when the tree finishes");
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

        // 12. get_agent_stream returns hancho.event/1 for schema=v1 (the MCP default) and the legacy
        // shape when asked explicitly, so both stay available during the migration.
        {
            std::wstring batW = tempDirW + L"\\v1_stream.bat";
            WriteFileAtomicWide(batW,
                "@echo off\r\n"
                "echo {\"type\":\"assistant\",\"message\":{\"id\":\"m1\",\"model\":\"claude-test\","
                "\"content\":[{\"type\":\"text\",\"text\":\"hello v1\"}]}}\r\n");
            json start = h.Call({{"action", "start"},
                                 {"cmd", QuoteCommandArg(WideToUtf8(batW))},
                                 {"name", "v1_stream"},
                                 {"agent_type", "claude"},
                                 {"json_stream", true},
                                 {"wait", true},
                                 {"wait_ms", 15000}});
            Check(start.value("ok", false), "start a json-stream process for the v1 IPC test");
            int id = start.value("id", 0);
            if (id > 0) {
                json v1 = h.Call({{"action", "get_agent_stream"}, {"id", id}, {"schema", "v1"}, {"tail", 0}});
                Check(v1.value("ok", false) && v1.value("schema", "") == "hancho.event/1" &&
                          v1.contains("events") && v1["events"].is_array() && !v1["events"].empty() &&
                          v1["events"][0].value("kind", "") == "message",
                      "IPC get_agent_stream schema=v1 returns hancho.event/1 events");

                json legacy = h.Call({{"action", "get_agent_stream"}, {"id", id}, {"schema", "legacy"}, {"tail", 0}});
                Check(legacy.value("ok", false) && legacy.value("schema", "") != "hancho.event/1",
                      "IPC get_agent_stream schema=legacy still returns the old shape");
            }
        }

        // 13. get_usage_daily (U1) and get_usage_history (U0)
        {
            SetDailyUsagePersistence(true);
            SetUsageHistoryPersistence(true);

            std::tm tmNov10 = {};
            tmNov10.tm_year = 2026 - 1900;
            tmNov10.tm_mon = 10; // November (0-based)
            tmNov10.tm_mday = 10;
            tmNov10.tm_hour = 12;
            int64_t epochNov10 = static_cast<int64_t>(std::mktime(&tmNov10));

            // Record Point 1: Nov 10, claude, opus
            LlmUsagePoint ptClaude1;
            ptClaude1.agentType = "claude";
            ptClaude1.model = "claude-opus-5-5";
            ptClaude1.inputTokens = 1000;
            ptClaude1.outputTokens = 200;
            ptClaude1.thinkingTokens = 150;
            ptClaude1.cacheReadTokens = 500;
            ptClaude1.totalTokens = 1850;
            ptClaude1.costUsd = 0.04;
            RecordLlmUsagePoint(epochNov10, ptClaude1);

            // Record Point 2: Nov 10 (+1h), claude, opus (same day, same engine, same model -> sums deltas)
            LlmUsagePoint ptClaude2;
            ptClaude2.agentType = "claude";
            ptClaude2.model = "claude-opus-5-5";
            ptClaude2.inputTokens = 500;
            ptClaude2.totalTokens = 500;
            ptClaude2.costUsd = 0.01;
            RecordLlmUsagePoint(epochNov10 + 3600, ptClaude2);

            // Record Point 3: Nov 10 (+2h), agy, gemini (same day, different engine)
            LlmUsagePoint ptAgy;
            ptAgy.agentType = "agy";
            ptAgy.model = "gemini-1.5-pro";
            ptAgy.inputTokens = 2000;
            ptAgy.outputTokens = 1000;
            ptAgy.totalTokens = 3000;
            ptAgy.costUsd = 0.0;
            RecordLlmUsagePoint(epochNov10 + 7200, ptAgy);

            // Record Point 4: Nov 11, claude, sonnet (next day)
            int64_t epochNov11 = epochNov10 + 86400;
            LlmUsagePoint ptNov11;
            ptNov11.agentType = "claude";
            ptNov11.model = "claude-3-7-sonnet";
            ptNov11.inputTokens = 800;
            ptNov11.outputTokens = 200;
            ptNov11.totalTokens = 1000;
            ptNov11.costUsd = 0.02;
            RecordLlmUsagePoint(epochNov11, ptNov11);

            // 13a: IPC get_usage_history (U0 raw points) still works alongside U1
            json histResp = h.Call({{"action", "get_usage_history"}, {"since_sec", 7 * 86400}});
            Check(histResp.value("ok", false), "IPC get_usage_history returns ok == true");
            Check(histResp.contains("points") && histResp["points"].is_array(), "IPC get_usage_history returns points array");

            // 13b: IPC get_usage_daily (default / all records)
            json dailyAll = h.Call({{"action", "get_usage_daily"}});
            Check(dailyAll.value("ok", false), "IPC get_usage_daily default returns ok == true");
            Check(dailyAll.contains("records") && dailyAll["records"].is_array(), "IPC get_usage_daily returns records array");
            Check(dailyAll["records"].size() == 3, "IPC get_usage_daily records size is 3 (2 on Nov 10, 1 on Nov 11)");
            Check(dailyAll.contains("totals"), "IPC get_usage_daily returns totals");
            Check(dailyAll["totals"].value("totalTokens", (uint64_t)0) == 6350, "IPC get_usage_daily totals.totalTokens is 6350 (2350 + 3000 + 1000)");
            Check(dailyAll["totals"].value("runs", (uint64_t)0) == 4, "IPC get_usage_daily totals.runs is 4");

            // 13c: IPC get_usage_daily - 当日の内訳 (breakdown for a single date)
            json dailyNov10 = h.Call({{"action", "get_usage_daily"}, {"date", "2026-11-10"}});
            Check(dailyNov10.value("ok", false), "IPC get_usage_daily for date 2026-11-10 returns ok == true");
            Check(dailyNov10["records"].size() == 2, "IPC get_usage_daily for 2026-11-10 has exactly 2 breakdown records (claude and agy)");
            Check(dailyNov10["totals"].value("totalTokens", (uint64_t)0) == 5350, "IPC get_usage_daily Nov 10 total tokens is 5350 (2350 + 3000)");
            Check(dailyNov10["totals"].value("runs", (uint64_t)0) == 3, "IPC get_usage_daily Nov 10 runs is 3");

            // 13d: IPC get_usage_daily - 月内の合計 (month total via groupBy: "month" or date range)
            json dailyMonth = h.Call({{"action", "get_usage_daily"}, {"since", "2026-11"}, {"until", "2026-11"}, {"groupBy", "month"}});
            Check(dailyMonth.value("ok", false), "IPC get_usage_daily groupBy month returns ok == true");
            Check(dailyMonth.contains("groups") && dailyMonth["groups"].size() == 1, "IPC get_usage_daily groupBy month returns 1 group for 2026-11");
            if (dailyMonth["groups"].size() == 1) {
                Check(dailyMonth["groups"][0].value("key", "") == "2026-11", "month group key is 2026-11");
                Check(dailyMonth["groups"][0].value("totalTokens", (uint64_t)0) == 6350, "month group totalTokens is 6350");
                Check(dailyMonth["groups"][0].value("runs", (uint64_t)0) == 4, "month group runs is 4");
            }
            Check(dailyMonth["totals"].value("totalTokens", (uint64_t)0) == 6350, "IPC get_usage_daily month query totals match");

            // 13e: IPC get_usage_daily groupBy engine
            json dailyEngine = h.Call({{"action", "get_usage_daily"}, {"groupBy", "engine"}});
            Check(dailyEngine.value("ok", false), "IPC get_usage_daily groupBy engine returns ok == true");
            Check(dailyEngine["groups"].size() == 2, "IPC get_usage_daily groupBy engine has 2 groups (agy and claude)");

            // 13f: IPC get_usage_daily invalid groupBy
            json dailyBad = h.Call({{"action", "get_usage_daily"}, {"groupBy", "invalid_dimension"}});
            Check(!dailyBad.value("ok", true), "IPC get_usage_daily invalid groupBy returns ok == false");
            Check(!dailyBad.value("error", "").empty(), "IPC get_usage_daily invalid groupBy returns error string");

            SetDailyUsagePersistence(false);
            SetUsageHistoryPersistence(false);
        }

        // 14. WebSocket 1 本での複数要求と要求 id 照合 (058-1-ws-multiplex)
        {
            remote::RemoteListener listener;
            listener.SetHandler([&h](const std::string& reqJson, const std::string& /*peer*/) {
                return h.ipc->HandleRequestJson(reqJson);
            });

            std::string startErr;
            bool started = listener.Start("127.0.0.1", 0, "", startErr);
            Check(started, "RemoteListener dynamic port start: " + startErr);
            Check(listener.Running(), "RemoteListener is running");
            std::string endpoint = listener.Endpoint();
            Check(!endpoint.empty(), "RemoteListener endpoint is non-empty (" + endpoint + ")");

            if (started) {
                // 14a. 1 接続で順次複数の要求を送り、要求 id が正しく照合されることを確認
                remote::WebSocketClient client;
                std::string connectErr;
                bool connected = client.Connect(endpoint, "", 5000, &connectErr);
                Check(connected, "WebSocketClient connected: " + connectErr);
                Check(client.IsConnected(), "WebSocketClient reports connected");

                // Request 1: string req_id
                std::string resp1, err1;
                bool ok1 = client.SendRequest("{\"action\":\"ping\",\"req_id\":\"seq-alpha\"}", resp1, err1, 5000);
                Check(ok1, "SendRequest 1 succeeded: " + err1);
                json doc1 = json::parse(resp1, nullptr, false);
                Check(doc1.is_object() && doc1.value("ok", false), "Response 1 ok is true");
                Check(doc1.value("req_id", "") == "seq-alpha", "Response 1 req_id matches 'seq-alpha'");

                // Request 2: integer req_id
                std::string resp2, err2;
                bool ok2 = client.SendRequest("{\"action\":\"gui_status\",\"req_id\":8888}", resp2, err2, 5000);
                Check(ok2, "SendRequest 2 succeeded: " + err2);
                json doc2 = json::parse(resp2, nullptr, false);
                Check(doc2.is_object() && doc2.value("ok", false), "Response 2 ok is true");
                Check(doc2.value("req_id", 0) == 8888, "Response 2 req_id matches 8888");

                // Request 3: requestId (camelCase)
                std::string resp3, err3;
                bool ok3 = client.SendRequest("{\"action\":\"ping\",\"requestId\":\"camel-req-3\"}", resp3, err3, 5000);
                Check(ok3, "SendRequest 3 succeeded: " + err3);
                json doc3 = json::parse(resp3, nullptr, false);
                Check(doc3.is_object() && doc3.value("requestId", "") == "camel-req-3", "Response 3 requestId matches");

                // Request 4: request_id (snake_case)
                std::string resp4, err4;
                bool ok4 = client.SendRequest("{\"action\":\"ping\",\"request_id\":\"snake_req_4\"}", resp4, err4, 5000);
                Check(ok4, "SendRequest 4 succeeded: " + err4);
                json doc4 = json::parse(resp4, nullptr, false);
                Check(doc4.is_object() && doc4.value("request_id", "") == "snake_req_4", "Response 4 request_id matches");

                // Request 5: id (string field)
                std::string resp5, err5;
                bool ok5 = client.SendRequest("{\"action\":\"ping\",\"id\":\"str-id-5\"}", resp5, err5, 5000);
                Check(ok5, "SendRequest 5 succeeded: " + err5);
                json doc5 = json::parse(resp5, nullptr, false);
                Check(doc5.is_object() && doc5.value("id", "") == "str-id-5", "Response 5 id matches");

                // Request 6: auto-assigned req_id
                std::string resp6, err6;
                bool ok6 = client.SendRequest("{\"action\":\"ping\"}", resp6, err6, 5000);
                Check(ok6, "SendRequest 6 succeeded (auto-assigned req_id): " + err6);
                json doc6 = json::parse(resp6, nullptr, false);
                Check(doc6.is_object() && doc6.value("ok", false), "Response 6 ok is true");
                Check(doc6.contains("req_id") && !doc6.value("req_id", "").empty(), "Response 6 has auto-assigned req_id");

                // All sequential requests were sent over the same connection
                Check(client.IsConnected(), "WebSocket connection remained open across all requests");

                // 14b. 1 接続で同時に複数の要求を出し、応答を要求 id で突き合わせられる（並行要求）
                constexpr int kConcurrentThreads = 8;
                std::vector<std::thread> threads;
                std::vector<std::string> responses(kConcurrentThreads);
                std::vector<std::string> errors(kConcurrentThreads);
                std::vector<bool> successes(kConcurrentThreads, false);

                for (int i = 0; i < kConcurrentThreads; ++i) {
                    threads.emplace_back([&client, &responses, &errors, &successes, i]() {
                        std::string reqId = "concurrent-" + std::to_string(i);
                        json req = {{"action", "ping"}, {"req_id", reqId}};
                        successes[i] = client.SendRequest(req.dump(), responses[i], errors[i], 10000);
                    });
                }

                for (auto& t : threads) {
                    if (t.joinable()) t.join();
                }

                bool allConcurrentOk = true;
                for (int i = 0; i < kConcurrentThreads; ++i) {
                    if (!successes[i]) {
                        allConcurrentOk = false;
                        Check(false, "Concurrent thread " + std::to_string(i) + " failed: " + errors[i]);
                    } else {
                        json respDoc = json::parse(responses[i], nullptr, false);
                        std::string expectedId = "concurrent-" + std::to_string(i);
                        if (!respDoc.is_object() || !respDoc.value("ok", false) || respDoc.value("req_id", "") != expectedId) {
                            allConcurrentOk = false;
                            Check(false, "Concurrent response mismatch for thread " + std::to_string(i));
                        }
                    }
                }
                Check(allConcurrentOk, "All " + std::to_string(kConcurrentThreads) + " simultaneous requests matched by req_id");

                // 14c. Pipelined / pooled multiplexed requests through WebSocketRequest / CallHost
                std::string poolResp1, poolErr1;
                bool pOk1 = remote::WebSocketRequest(endpoint, "", "{\"action\":\"ping\",\"req_id\":\"pool-1\"}", poolResp1, poolErr1, 5000);
                Check(pOk1, "Pooled WebSocketRequest 1 succeeded: " + poolErr1);
                json pDoc1 = json::parse(poolResp1, nullptr, false);
                Check(pDoc1.is_object() && pDoc1.value("req_id", "") == "pool-1", "Pooled request 1 matched req_id");

                std::string poolResp2, poolErr2;
                bool pOk2 = remote::WebSocketRequest(endpoint, "", "{\"action\":\"gui_status\",\"req_id\":\"pool-2\"}", poolResp2, poolErr2, 5000);
                Check(pOk2, "Pooled WebSocketRequest 2 succeeded: " + poolErr2);
                json pDoc2 = json::parse(poolResp2, nullptr, false);
                Check(pDoc2.is_object() && pDoc2.value("req_id", "") == "pool-2", "Pooled request 2 matched req_id");

                // Cleanup client
                client.Close();
                Check(!client.IsConnected(), "WebSocketClient is closed");
            }

            listener.Stop();
            Check(!listener.Running(), "RemoteListener stopped cleanly");
            remote::ResetWebSocketPool();
        }

        // 15. WebSocket 購読アクションと seq による再開 (058-2-ws-subscribe)
        {
            remote::RemoteListener listener;
            listener.SetHandler([&h](const std::string& reqJson, const std::string& /*peer*/) {
                return h.ipc->HandleRequestJson(reqJson);
            });

            std::string startErr;
            bool started = listener.Start("127.0.0.1", 0, "", startErr);
            Check(started, "RemoteListener start for subscribe: " + startErr);
            std::string endpoint = listener.Endpoint();

            if (started) {
                std::wstring testDirW = PathJoinWide(tempDirW, L"ws_sub_test");
                EnsureDirectoryWide(testDirW);

                // 15a. 購読のアクション (.events.jsonl) と順序通りの受信
                std::wstring evPathW = PathJoinWide(testDirW, L"job.1.events.jsonl");
                std::string evPath = WideToUtf8(evPathW);

                // Write 3 initial events
                AppendToLogFileWide(evPathW,
                    "{\"schema\":\"hancho.event/1\",\"seq\":1,\"kind\":\"tool.call\",\"data\":{\"tool\":\"read_file\"}}\n"
                    "{\"schema\":\"hancho.event/1\",\"seq\":2,\"kind\":\"tool.result\",\"data\":{\"ok\":true}}\n"
                    "{\"schema\":\"hancho.event/1\",\"seq\":3,\"kind\":\"message\",\"data\":{\"text\":\"init done\"}}\n");

                remote::WebSocketClient client1;
                std::string connErr;
                bool cOk = client1.Connect(endpoint, "", 5000, &connErr);
                Check(cOk, "client1 connected for subscribe test: " + connErr);

                struct ReceivedEvent {
                    std::string subId;
                    uint64_t seq = 0;
                    std::string line;
                    json doc;
                };
                std::mutex recvMutex;
                std::vector<ReceivedEvent> received1;

                client1.SetStreamCallback([&recvMutex, &received1](const std::string& subId, uint64_t seq,
                                                                   const std::string& line, const json& doc) {
                    std::lock_guard<std::mutex> lock(recvMutex);
                    received1.push_back({subId, seq, line, doc});
                });

                std::string subId1, subErr1;
                bool subOk1 = client1.SubscribeFile(evPath, 0, subId1, subErr1, 5000);
                Check(subOk1, "SubscribeFile .events.jsonl succeeded: " + subErr1);
                Check(!subId1.empty(), "Received non-empty sub_id: " + subId1);

                // Wait for initial events 1, 2, 3
                for (int i = 0; i < 50; ++i) {
                    {
                        std::lock_guard<std::mutex> lock(recvMutex);
                        if (received1.size() >= 3) break;
                    }
                    SleepMs(20);
                }

                {
                    std::lock_guard<std::mutex> lock(recvMutex);
                    Check(received1.size() >= 3, "Received initial 3 events via subscription (got " + std::to_string(received1.size()) + ")");
                    if (received1.size() >= 3) {
                        Check(received1[0].seq == 1 && received1[1].seq == 2 && received1[2].seq == 3,
                              "Events received strictly in seq order 1, 2, 3");
                        Check(received1[0].doc.value("stream", "") == "file" || received1[0].doc.value("stream", "") == "events",
                              "Stream field present in event message");
                    }
                }

                // Append event 4 and check live streaming arrival
                AppendToLogFileWide(evPathW, "{\"schema\":\"hancho.event/1\",\"seq\":4,\"kind\":\"tool.call\",\"data\":{\"tool\":\"write_file\"}}\n");
                for (int i = 0; i < 50; ++i) {
                    {
                        std::lock_guard<std::mutex> lock(recvMutex);
                        if (received1.size() >= 4) break;
                    }
                    SleepMs(20);
                }

                {
                    std::lock_guard<std::mutex> lock(recvMutex);
                    Check(received1.size() >= 4 && received1[3].seq == 4,
                          "Live appended event 4 received without loss");
                }

                // 15b. 購読の切断と再開 (seq による再開、欠落・重複なし)
                client1.Close();
                Check(!client1.IsConnected(), "client1 closed cleanly");

                // In the disconnected state, events 5 and 6 are appended
                AppendToLogFileWide(evPathW,
                    "{\"schema\":\"hancho.event/1\",\"seq\":5,\"kind\":\"tool.result\",\"data\":{\"ok\":true}}\n"
                    "{\"schema\":\"hancho.event/1\",\"seq\":6,\"kind\":\"message\",\"data\":{\"text\":\"all done\"}}\n");

                // Reconnect as client2 and subscribe from seq 4
                remote::WebSocketClient client2;
                bool cOk2 = client2.Connect(endpoint, "", 5000, &connErr);
                Check(cOk2, "client2 connected for resubscribe test: " + connErr);

                std::vector<ReceivedEvent> received2;
                std::mutex recvMutex2;
                client2.SetStreamCallback([&recvMutex2, &received2](const std::string& subId, uint64_t seq,
                                                                   const std::string& line, const json& doc) {
                    std::lock_guard<std::mutex> lock(recvMutex2);
                    received2.push_back({subId, seq, line, doc});
                });

                std::string subId2, subErr2;
                bool subOk2 = client2.SubscribeFile(evPath, 4, subId2, subErr2, 5000);
                Check(subOk2, "client2 resubscribe from seq 4 succeeded: " + subErr2);

                for (int i = 0; i < 50; ++i) {
                    {
                        std::lock_guard<std::mutex> lock(recvMutex2);
                        if (received2.size() >= 2) break;
                    }
                    SleepMs(20);
                }

                {
                    std::lock_guard<std::mutex> lock(recvMutex2);
                    Check(received2.size() == 2,
                          "Resubscribed client received exactly 2 events (no duplicates of 1..4, got " + std::to_string(received2.size()) + ")");
                    if (received2.size() >= 2) {
                        Check(received2[0].seq == 5 && received2[1].seq == 6,
                              "Resumed events are strictly seq 5 and 6 without loss or duplication");
                    }
                }

                // Append event 7 while connected
                AppendToLogFileWide(evPathW, "{\"schema\":\"hancho.event/1\",\"seq\":7,\"kind\":\"run.ended\",\"data\":{\"status\":\"success\"}}\n");
                for (int i = 0; i < 50; ++i) {
                    {
                        std::lock_guard<std::mutex> lock(recvMutex2);
                        if (received2.size() >= 3) break;
                    }
                    SleepMs(20);
                }

                {
                    std::lock_guard<std::mutex> lock(recvMutex2);
                    Check(received2.size() == 3 && received2[2].seq == 7,
                          "Appended event 7 received in order");
                }

                // 15c. 購読解除 (unsubscribe)
                std::string unsubErr;
                bool unsubOk = client2.Unsubscribe(subId2, unsubErr, 5000);
                Check(unsubOk, "Unsubscribe succeeded: " + unsubErr);

                AppendToLogFileWide(evPathW, "{\"schema\":\"hancho.event/1\",\"seq\":8,\"kind\":\"message\",\"data\":{\"text\":\"after unsub\"}}\n");
                SleepMs(100);
                {
                    std::lock_guard<std::mutex> lock(recvMutex2);
                    Check(received2.size() == 3,
                          "No events delivered after unsubscribe (count remained 3)");
                }

                client2.Close();

                // 15d. .tail.txt と .stream.jsonl のストリーミングおよび seq 再開
                std::wstring tailPathW = PathJoinWide(testDirW, L"job.1.tail.txt");
                std::string tailPath = WideToUtf8(tailPathW);
                AppendToLogFileWide(tailPathW, "line 1 of tail\nline 2 of tail\nline 3 of tail\n");

                remote::WebSocketClient client3;
                client3.Connect(endpoint, "", 5000, nullptr);

                std::vector<ReceivedEvent> receivedTail;
                std::mutex recvTailMutex;
                client3.SetStreamCallback([&recvTailMutex, &receivedTail](const std::string& subId, uint64_t seq,
                                                                         const std::string& line, const json& doc) {
                    std::lock_guard<std::mutex> lock(recvTailMutex);
                    receivedTail.push_back({subId, seq, line, doc});
                });

                std::string subId3;
                bool subOk3 = client3.SubscribeFile(tailPath, 0, subId3, subErr1, 5000);
                Check(subOk3, "SubscribeFile .tail.txt succeeded: " + subErr1);

                for (int i = 0; i < 50; ++i) {
                    {
                        std::lock_guard<std::mutex> lock(recvTailMutex);
                        if (receivedTail.size() >= 3) break;
                    }
                    SleepMs(20);
                }

                {
                    std::lock_guard<std::mutex> lock(recvTailMutex);
                    Check(receivedTail.size() == 3,
                          "Received 3 lines of tail.txt (got " + std::to_string(receivedTail.size()) + ")");
                    if (receivedTail.size() >= 3) {
                        Check(receivedTail[0].seq == 1 && receivedTail[2].seq == 3,
                              "Tail seq ordered 1 to 3");
                        Check(receivedTail[0].line == "line 1 of tail", "Tail line content preserved");
                    }
                }

                // Append line 4 to tail
                AppendToLogFileWide(tailPathW, "line 4 of tail\n");
                for (int i = 0; i < 50; ++i) {
                    {
                        std::lock_guard<std::mutex> lock(recvTailMutex);
                        if (receivedTail.size() >= 4) break;
                    }
                    SleepMs(20);
                }

                {
                    std::lock_guard<std::mutex> lock(recvTailMutex);
                    Check(receivedTail.size() == 4 && receivedTail[3].seq == 4,
                          "Appended tail line 4 streamed live");
                }

                // Disconnect and resubscribe tail from seq 3
                client3.Close();
                AppendToLogFileWide(tailPathW, "line 5 of tail\n");

                remote::WebSocketClient client4;
                client4.Connect(endpoint, "", 5000, nullptr);
                std::vector<ReceivedEvent> receivedTail2;
                std::mutex recvTailMutex2;
                client4.SetStreamCallback([&recvTailMutex2, &receivedTail2](const std::string& subId, uint64_t seq,
                                                                           const std::string& line, const json& doc) {
                    std::lock_guard<std::mutex> lock(recvTailMutex2);
                    receivedTail2.push_back({subId, seq, line, doc});
                });

                std::string subId4;
                bool subOk4 = client4.SubscribeFile(tailPath, 3, subId4, subErr1, 5000);
                Check(subOk4, "client4 resubscribe tail from seq 3 succeeded");

                for (int i = 0; i < 50; ++i) {
                    {
                        std::lock_guard<std::mutex> lock(recvTailMutex2);
                        if (receivedTail2.size() >= 2) break;
                    }
                    SleepMs(20);
                }

                {
                    std::lock_guard<std::mutex> lock(recvTailMutex2);
                    Check(receivedTail2.size() == 2,
                          "Tail resubscribe got exactly 2 lines (seq 4 and 5, got " + std::to_string(receivedTail2.size()) + ")");
                    if (receivedTail2.size() >= 2) {
                        Check(receivedTail2[0].seq == 4 && receivedTail2[1].seq == 5,
                              "Tail seq resumed from 4 to 5 without duplication");
                    }
                }
                client4.Close();
            }

            listener.Stop();
        }

        // 16. リモートジョブの監視ペインへのストリーミング反映テスト (058c)
        {
            remote::RemoteListener listener;
            std::string listenErr;
            Check(listener.Start("127.0.0.1", 0, "", listenErr),
                  "Remote listener for monitor pane test started: " + listenErr);
            const std::string endpoint = listener.Endpoint();

            std::wstring hostDirW = PathJoinWide(tempDirW, L"remote_host_pane");
            EnsureDirectoryWide(hostDirW);
            std::wstring tqDirW = PathJoinWide(hostDirW, L"taskqueue");
            EnsureDirectoryWide(tqDirW);

            // A remote job with events
            std::wstring jobEventsW = PathJoinWide(tqDirW, L"build-linux.1.events.jsonl");
            std::string initialEvent = "{\"schema\":\"hancho.event/1\",\"seq\":1,\"kind\":\"plan\",\"data\":{\"items\":[{\"content\":\"build binary\",\"status\":\"in_progress\"},{\"content\":\"run tests\",\"status\":\"pending\"}]}}\n";
            AppendToLogFileWide(jobEventsW, initialEvent);

            listener.SetHandler([jobEventsW](const std::string& requestJson, const std::string& peer) -> std::string {
                json req = json::parse(requestJson, nullptr, false);
                std::string action = req.value("action", "");
                if (action == "subscribe") {
                    std::string stream = req.value("stream", "events");
                    std::string jobId = req.value("job_id", "");
                    int attempt = req.value("attempt", 1);
                    json resp;
                    resp["ok"] = true;
                    resp["action"] = "subscribe";
                    resp["stream"] = stream;
                    resp["job_id"] = jobId;
                    resp["attempt"] = attempt;
                    resp["file"] = WideToUtf8(jobEventsW);
                    resp["seq"] = req.value("seq", uint64_t{0});
                    return resp.dump();
                }
                return "{\"ok\":false,\"error\":\"unhandled\"}";
            });

            remote::WebSocketClient wsClient;
            Check(wsClient.Connect(endpoint, "", 5000, nullptr), "Connect to remote host succeeded");

            std::vector<json> streamedDocs;
            std::mutex streamMtx;
            std::wstring localJobEventsW = PathJoinWide(tempDirW, L"local.build-linux.1.events.jsonl");

            wsClient.SetStreamCallback([&streamMtx, &streamedDocs, localJobEventsW](
                const std::string& subId, uint64_t seq, const std::string& line, const json& doc) {
                std::lock_guard<std::mutex> lock(streamMtx);
                streamedDocs.push_back(doc);
                AppendToLogFileWide(localJobEventsW, line + "\n");
            });

            std::string subId;
            std::string subErr;
            bool subOk = wsClient.SubscribeFile(WideToUtf8(jobEventsW), 0, subId, subErr, 5000);
            Check(subOk, "Subscribe to remote job events succeeded: " + subErr);

            // Wait for initial event
            for (int i = 0; i < 50; ++i) {
                {
                    std::lock_guard<std::mutex> lock(streamMtx);
                    if (!streamedDocs.empty()) break;
                }
                SleepMs(20);
            }

            {
                std::lock_guard<std::mutex> lock(streamMtx);
                Check(streamedDocs.size() == 1, "Initial plan event received over stream");
                if (!streamedDocs.empty()) {
                    Check(StreamEventKind(streamedDocs[0]) == "plan", "Streamed event 1 kind is plan");
                }
            }

            // Simulate remote job emitting tool call, thought, and usage events
            std::string ev2 = "{\"schema\":\"hancho.event/1\",\"seq\":2,\"kind\":\"thought\",\"data\":{\"text\":\"compiling sources...\"}}\n";
            std::string ev3 = "{\"schema\":\"hancho.event/1\",\"seq\":3,\"kind\":\"tool.call\",\"agent\":{\"type\":\"claude\",\"model\":\"claude-3-7-sonnet\"},\"data\":{\"tool\":\"Bash\",\"activity\":\"Compiling\",\"target\":\"src/main.cpp\",\"inputTokens\":500,\"outputTokens\":100,\"totalTokens\":600}}\n";
            std::string ev4 = "{\"schema\":\"hancho.event/1\",\"seq\":4,\"kind\":\"run.ended\",\"data\":{\"status\":\"success\"}}\n";
            AppendToLogFileWide(jobEventsW, ev2 + ev3 + ev4);

            for (int i = 0; i < 50; ++i) {
                {
                    std::lock_guard<std::mutex> lock(streamMtx);
                    if (streamedDocs.size() >= 4) break;
                }
                SleepMs(20);
            }

            {
                std::lock_guard<std::mutex> lock(streamMtx);
                Check(streamedDocs.size() == 4, "All 4 streamed events received live without polling (got " + std::to_string(streamedDocs.size()) + ")");
                if (streamedDocs.size() >= 4) {
                    Check(StreamEventKind(streamedDocs[1]) == "thought", "Event 2 is thought");
                    Check(StreamEventKind(streamedDocs[2]) == "tool.call", "Event 3 is tool.call");
                    Check(StreamEventKind(streamedDocs[3]) == "run.ended", "Event 4 is run.ended");
                }
            }

            // Verify local mirrored events file
            auto localEvents = ReadHanchoEventsFile(WideToUtf8(localJobEventsW), 0, "", 0);
            Check(localEvents.size() == 4, "Local mirrored events file has 4 events");

            wsClient.Close();
            listener.Stop();
        }

        // -------------------------------------------------------------------
        // GUI IPC Requests Smoke Test (card 060a)
        // Verify all new IPC requests needed by GUI client:
        // process remove & reset_ids, taskqueue_counts, taskqueue_resource_locks,
        // taskqueue_diagnose / failure_diagnose, memory_* actions, config_get & config_set.
        // Use an isolated data directory so the harness does not pick up jobs
        // created by the harness that is still alive above.
        // -------------------------------------------------------------------
        {
            std::wstring guiIpcDirW = PathJoinWide(tempDirW, L"gui_ipc");
            RemoveDirRecursive(guiIpcDirW);
            EnsureDirectoryWide(guiIpcDirW);
            SetDataDirectoryOverride(guiIpcDirW);
            {
            IpcTestHarness h;

            // 1. taskqueue_counts
            json countsResp = h.Call({{"action", "taskqueue_counts"}});
            Check(countsResp.value("ok", false), "taskqueue_counts ok");
            Check(countsResp.contains("counts") && countsResp["counts"].is_object(), "taskqueue_counts contains counts");
            Check(countsResp.value("total", -1) == 0, "taskqueue_counts total is 0 initially");

            // 2. taskqueue_resource_locks
            json rlGet = h.Call({{"action", "taskqueue_resource_locks"}});
            Check(rlGet.value("ok", false), "taskqueue_resource_locks get ok");
            json rlSet = h.Call({{"action", "taskqueue_resource_locks"}, {"enabled", false}});
            Check(rlSet.value("ok", false) && !rlSet.value("enabled", true), "taskqueue_resource_locks set false");
            json rlReset = h.Call({{"action", "taskqueue_resource_locks"}, {"enabled", true}});
            Check(rlReset.value("ok", false) && rlReset.value("enabled", false), "taskqueue_resource_locks restore true");

            // 3. process remove and reset_ids
            json resetResp = h.Call({{"action", "reset_ids"}});
            Check(resetResp.value("ok", false), "reset_ids ok");
            json remErr = h.Call({{"action", "remove"}, {"id", 0}});
            Check(!remErr.value("ok", true), "remove with id 0 returns error");
            json remOk = h.Call({{"action", "remove"}, {"id", 9999}});
            Check(remOk.value("ok", false) && remOk.value("removed_id", 0) == 9999, "remove with valid id accepted");

            // 4. taskqueue_diagnose / failure_diagnose
            json diagEmpty = h.Call({{"action", "taskqueue_diagnose"}});
            Check(!diagEmpty.value("ok", true), "taskqueue_diagnose without params returns error");
            json diagProc = h.Call({{"action", "taskqueue_diagnose"}, {"process_id", 9999}});
            Check(!diagProc.value("ok", true), "taskqueue_diagnose non-existent proc returns error");
            json diagBreakdown = h.Call({{"action", "failure_diagnose"}, {"taskqueue", "test-q"}, {"breakdown", true}});
            Check(diagBreakdown.value("ok", false) && diagBreakdown.contains("classes"), "failure_diagnose breakdown ok");

            // 5. memory actions
            json memStorage = h.Call({{"action", "memory_storage_stats"}});
            Check(memStorage.value("ok", false) && memStorage.contains("total_bytes"), "memory_storage_stats returns storage stats");
            json memSearch = h.Call({{"action", "memory_search"}, {"query", "test"}});
            Check(memSearch.is_object(), "memory_search returns valid object");
            json memStats = h.Call({{"action", "memory_stats"}, {"job_name", "test"}});
            Check(memStats.is_object(), "memory_stats returns valid object");
            json memGetErr = h.Call({{"action", "memory_get"}});
            Check(!memGetErr.value("ok", true), "memory_get requires id");
            json memCompare = h.Call({{"action", "memory_compare"}, {"job", "jobA"}});
            Check(memCompare.is_object(), "memory_compare returns valid object");

            // 6. config_get and config_set
            json cfgGet = h.Call({{"action", "config_get"}});
            Check(cfgGet.value("ok", false) && cfgGet.contains("config"), "config_get ok");
            Check(cfgGet["config"].contains("webui") &&
                  cfgGet["config"]["webui"].value("scope", "") == "loopback",
                  "config_get includes webui exposure defaults");
            json cfgSet = h.Call({{"action", "config_set"}, {"patch", {{"default_shell", "powershell"}, {"test_setting", 42}}}});
            Check(cfgSet.value("ok", false) && cfgSet["config"].value("test_setting", 0) == 42, "config_set patch ok");
            json cfgGetAfter = h.Call({{"action", "config_get"}});
            Check(cfgGetAfter.value("ok", false) && cfgGetAfter["config"].value("test_setting", 0) == 42, "config_get reflects updated setting");

            // -------------------------------------------------------------------
            // GUI IPC Replacement Smoke Test (card 060b)
            // Verify that GUI's 5 replaced functional areas operate over IPC:
            // 1. Process list / details
            // 2. Task queue counts / list / get
            // 3. Monitor pane agent state & agent stream
            // 4. Failure diagnosis (by process_id, by job_id, breakdown by taskqueue)
            // 5. Memory list search / get / storage stats / compare
            // -------------------------------------------------------------------
            // 1. Process list & details
            json listResp = h.Call({{"action", "list"}});
            Check(listResp.value("ok", false) && listResp.contains("processes") && listResp["processes"].is_array(),
                  "GUI IPC: list returns processes array");
            json listFilter = h.Call({{"action", "list"}, {"id", 1}});
            Check(listFilter.value("ok", false) && listFilter["processes"].is_array(),
                  "GUI IPC: list with id returns filtered array");

            // 2. Task queue
            json tqCounts = h.Call({{"action", "taskqueue_counts"}});
            Check(tqCounts.value("ok", false) && tqCounts.contains("counts"),
                  "GUI IPC: taskqueue_counts returns counts object");
            json tqList = h.Call({{"action", "taskqueue_list"}, {"taskqueue", ""}, {"state", ""}});
            Check(tqList.value("ok", false) && tqList.contains("jobs"),
                  "GUI IPC: taskqueue_list returns jobs array");

            // 3. Monitor pane / agent stream
            json agentState = h.Call({{"action", "get_agent_state"}, {"id", 1}});
            Check(agentState.is_object(), "GUI IPC: get_agent_state returns object");
            json agentStream = h.Call({{"action", "get_agent_stream"}, {"id", 1}, {"schema", "v1"}});
            Check(agentStream.is_object(), "GUI IPC: get_agent_stream returns object");

            // 4. Failure diagnosis
            json diagProcGui = h.Call({{"action", "taskqueue_diagnose"}, {"process_id", 1}});
            Check(diagProcGui.is_object(), "GUI IPC: taskqueue_diagnose by process_id returns object");
            json diagBreakdownGui = h.Call({{"action", "taskqueue_diagnose"}, {"taskqueue", "default"}, {"breakdown", true}});
            Check(diagBreakdownGui.value("ok", false) && diagBreakdownGui.contains("classes"),
                  "GUI IPC: taskqueue_diagnose breakdown returns classes");

            // 5. Memory list
            json memStats2 = h.Call({{"action", "memory_storage_stats"}});
            Check(memStats2.value("ok", false) && memStats2.contains("total_bytes"),
                  "GUI IPC: memory_storage_stats returns storage stats");
            json memSearch2 = h.Call({{"action", "memory_search"}, {"text", ""}, {"limit", 10}});
            Check(memSearch2.is_object(), "GUI IPC: memory_search returns object");
            }
            SetDataDirectoryOverride(tempDirW);
            RemoveDirRecursive(guiIpcDirW);
        }
    }

    RemoveDirRecursive(tempDirW);
    std::printf("%s (%d passed, %d failures)\n", g_failures ? "FAILED" : "ALL PASSED", g_passes, g_failures);
    return g_failures ? 1 : 0;
}
