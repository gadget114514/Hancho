// Headless smoke test for routines (save / list / run twice / show / testReport / validation).
// Build: cmake --build build --config Release --target routine_smoke
// Run:   build\Release\routine_smoke.exe

#include "bt_manager.h"
#include "common.h"
#include "ipc_server.h"
#include "nlohmann/json.hpp"
#include "process_manager.h"
#include "routine.h"
#include "taskqueue.h"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

using namespace pc;
using json = nlohmann::json;

namespace {

int g_failures = 0;
int g_passes = 0;

void Check(bool ok, const std::string& msg) {
    if (ok) {
        ++g_passes;
        std::fprintf(stdout, "PASS: %s\n", msg.c_str());
    } else {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
    }
}

void DumpFilesForDiagnostics(const std::wstring& dir) {
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        const std::wstring full = dir + L"\\" + name;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            DumpFilesForDiagnostics(full);
        } else {
            std::ifstream ifs(full, std::ios::binary);
            if (!ifs) continue;
            std::ostringstream ss;
            ss << ifs.rdbuf();
            std::string content = ss.str();
            if (content.size() > 5000) content = content.substr(0, 5000) + "...(truncated)";
            std::fprintf(stdout, "DIAG FILE %s:\n%s\n", WideToUtf8(full).c_str(), content.c_str());
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

void RemoveDirRecursive(const std::wstring& dir) {
    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            const std::wstring full = dir + L"\\" + name;
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
        const std::string raw = ipc->HandleRequestJson(req.dump());
        json parsed = json::parse(raw, nullptr, false);
        if (parsed.is_discarded()) return json{{"ok", false}, {"error", "discarded JSON: " + raw}};
        return parsed;
    }
};

} // namespace

int main() {
    wchar_t tempBase[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempBase);
    const std::wstring tempDirW = std::wstring(tempBase) + L"pc_routine_smoke_" + std::to_wstring(GetCurrentProcessId());
    RemoveDirRecursive(tempDirW);
    CreateDirectoryW(tempDirW.c_str(), nullptr);
    SetDataDirectoryOverride(tempDirW);
    const std::wstring projW = PathJoinWide(tempDirW, L"project");
    CreateDirectoryW(projW.c_str(), nullptr);
    const std::string projUtf8 = WideToUtf8(projW);
    std::fprintf(stdout, "routine_smoke temp dir: %s\n", WideToUtf8(tempDirW).c_str());

    const std::string reportXml =
        "<?xml version=\"1.0\"?>\n"
        "<testsuites><testsuite name=\"s\" tests=\"2\" failures=\"0\">"
        "<testcase name=\"alpha\"/><testcase name=\"beta\"/>"
        "</testsuite></testsuites>\n";
    WriteFileAtomicWide(PathJoinWide(projW, L"fixed_report.txt"), reportXml);
    WriteFileAtomicWide(PathJoinWide(projW, L"write_report.bat"),
                        "@echo off\r\ntype fixed_report.txt > report.xml\r\nexit /b 0\r\n");

    // 093-5: fake worker engines. Each writes a JSON definition to %PC_BT_RESULT_FILE%.
    const std::wstring goodBat = PathJoinWide(tempDirW, L"fake_good.bat");
    const std::wstring flakyBat = PathJoinWide(tempDirW, L"fake_flaky.bat");
    const std::wstring badBat = PathJoinWide(tempDirW, L"fake_bad.bat");
    WriteFileAtomicWide(goodBat,
        "@echo off\r\ncopy /Y \"%~dp0answer-good.txt\" \"%PC_BT_RESULT_FILE%\" >nul\r\nexit /b 0\r\n");
    WriteFileAtomicWide(flakyBat,
        "@echo off\r\n"
        "set /a n=0\r\n"
        "if exist \"%~dp0flaky-count.txt\" set /p n=<\"%~dp0flaky-count.txt\"\r\n"
        "set /a n=n+1\r\n"
        ">\"%~dp0flaky-count.txt\" echo %n%\r\n"
        "if %n%==1 (copy /Y \"%~dp0answer-bad.txt\" \"%PC_BT_RESULT_FILE%\" >nul) else (copy /Y \"%~dp0answer-good.txt\" \"%PC_BT_RESULT_FILE%\" >nul)\r\n"
        "exit /b 0\r\n");
    WriteFileAtomicWide(badBat,
        "@echo off\r\ncopy /Y \"%~dp0answer-bad.txt\" \"%PC_BT_RESULT_FILE%\" >nul\r\nexit /b 0\r\n");
    const std::string goodCmd = QuoteCommandArg(WideToUtf8(goodBat)) + " --model {model} --prompt {prompt}";
    const std::string flakyCmd = QuoteCommandArg(WideToUtf8(flakyBat)) + " --model {model} --prompt {prompt}";
    const std::string badCmd = QuoteCommandArg(WideToUtf8(badBat)) + " --model {model} --prompt {prompt}";
    const auto workerEngine = [](const std::string& cmd) {
        return json{{"command", cmd}, {"models", json::array({"fake-m1"})}, {"defaultModel", "fake-m1"}};
    };
    json enginesJson = {
        {"default", "fake-good"},
        {"engines", {{"fake-good", workerEngine(goodCmd)},
                     {"fake-flaky", workerEngine(flakyCmd)},
                     {"fake-bad", workerEngine(badCmd)}}}
    };
    WriteFileAtomicWide(PathJoinWide(tempDirW, L"engines.json"), enginesJson.dump(2));
    const auto llmsJson = [](const std::string& engine) {
        return json{{"worker", json::array({json{{"engine", engine}, {"model", "fake-m1"},
                                                  {"rank", 1}, {"enabled", true},
                                                  {"status", "approved"}}})},
                    {"consult", json::array()}, {"supervisor", json::array()},
                    {"history", json::array()}};
    };
    WriteFileAtomicWide(PathJoinWide(tempDirW, L"llms.json"), llmsJson("fake-good").dump(2));

    const auto answerJson = [&](const std::string& name, bool bad,
                                const std::string& summary = "Generated by fake worker") {
        json root = {{"type", "sequence"},
                     {"children", json::array({json{{"type", "job"}, {"id", "report"},
                         {"summary", "Write report"}, {"command", "cmd.exe /c .\\write_report.bat"},
                         {"timeoutSec", 30}}})}};
        if (bad) {
            root = {{"type", "sequence"},
                    {"children", json::array({json{{"type", "worker"}, {"id", "nope"},
                        {"summary", "Not allowed"}, {"prompt", "x"}, {"timeoutSec", 30}}})}};
        }
        return json{{"name", name},
                    {"summary", summary},
                    {"bt", json{{"id", name + "-bt"}, {"summary", "Generated BT"},
                                {"cwd", projUtf8}, {"root", root}}},
                    {"inputs", json::array({"CMakeLists.txt"})},
                    {"notes", "fake worker notes"},
                    {"rationale", "The user asked for it and the repository smoke tests use write_report.bat."}}
               .dump(2);
    };
    WriteFileAtomicWide(PathJoinWide(tempDirW, L"answer-good.txt"), answerJson("llm-smoke", false));
    WriteFileAtomicWide(PathJoinWide(tempDirW, L"answer-bad.txt"), answerJson("llm-smoke", true));

    json bt = {
        {"id", "routine-bt-template"},
        {"summary", "Build and smoke template"},
        {"cwd", projUtf8},
        {"root", {
            {"type", "sequence"},
            {"children", json::array({
                {{"type", "job"},
                 {"id", "report"},
                 {"summary", "Write a JUnit report"},
                 {"command", "cmd.exe /c .\\write_report.bat"},
                 {"timeoutSec", 30},
                 {"success", {{"testReport", "report.xml"}}}}
            })}
        }}
    };

    {
        IpcTestHarness h;

        const std::wstring defPath = PathJoinWide(PathJoinWide(PathJoinWide(projW, L".hancho"), L"routines"),
                                                  L"build-smoke.json");
        const std::wstring runsPath = PathJoinWide(PathJoinWide(tempDirW, L"routines"), L"build-smoke.runs.json");

        json saveReq = {
            {"action", "routine_save"},
            {"name", "build-smoke"},
            {"bt", bt},
            {"inputs", json::array({"CMakeLists.txt"})},
            {"summary", "Build and smoke"},
            {"notes", "smoke"},
            {"scope", "project"},
            {"cwd", projUtf8},
            {"by", "routine_smoke"},
            {"reason", "initial"}
        };
        json saved = h.Call(saveReq);
        Check(saved.value("ok", false) && saved.value("scope", std::string()) == "project",
              "routine save accepted a valid BT into project scope: " + saved.dump());
        Check(PathExistsWide(defPath), "project routine definition file exists");

        const json definition = json::parse(ReadFileUtf8Wide(defPath), nullptr, false);
        Check(!definition.is_discarded() && definition.value("name", std::string()) == "build-smoke" &&
                  definition.contains("bt") && definition.contains("inputsSnapshot") &&
                  definition.contains("history") && definition["history"].size() == 1 &&
                  definition["history"][0].value("reason", std::string()) == "initial",
              "definition has name/bt/inputsSnapshot/history: " + definition.dump());

        json listed = h.Call({{"action", "routine_list"}, {"cwd", projUtf8}});
        bool found = false;
        if (listed.value("ok", false) && listed.contains("routines") && listed["routines"].is_array()) {
            for (const auto& row : listed["routines"]) {
                if (row.value("name", std::string()) == "build-smoke" &&
                    row.value("scope", std::string()) == "project") found = true;
            }
        }
        Check(found, "routine list contains build-smoke with project scope: " + listed.dump());

        json invalidBt = bt;
        invalidBt.erase("root");
        saveReq["name"] = "bad-routine";
        saveReq["bt"] = invalidBt;
        json rejected = h.Call(saveReq);
        Check(!rejected.value("ok", true) && rejected.contains("error"),
              "invalid BT rejected by routine save: " + rejected.dump());

        json run1 = h.Call({{"action", "routine_run"}, {"name", "build-smoke"}, {"cwd", projUtf8},
                            {"wait", true}, {"timeout_ms", 20000}});
        Check(run1.value("ok", false) && run1.value("state", std::string()) == "succeeded" &&
                  !run1.value("runId", std::string()).empty(),
              "first routine run succeeded: " + run1.dump());
        Check(run1.value("runId", std::string()).rfind("build-smoke-", 0) == 0 &&
                  run1.value("taskqueue", std::string()) == "routine-build-smoke",
              "run id and taskqueue naming are correct");

        json run2 = h.Call({{"action", "routine_run"}, {"name", "build-smoke"}, {"cwd", projUtf8},
                            {"wait", true}, {"timeout_ms", 20000}});
        Check(run2.value("ok", false) && run2.value("state", std::string()) == "succeeded" &&
                  run2.value("runId", std::string()) != run1.value("runId", std::string()),
              "second routine run succeeded with a different run id: " + run2.dump());

        const std::string jobId = run2.value("runId", std::string()) + "-report-1";
        json jobList = h.Call({{"action", "taskqueue_list"}, {"no_planned", true}});
        bool jobFound = false;
        if (jobList.contains("jobs") && jobList["jobs"].is_array()) {
            for (const auto& row : jobList["jobs"]) {
                if (row.value("id", std::string()).find(run2.value("runId", std::string())) != std::string::npos) {
                    jobFound = true;
                }
            }
        }
        Check(jobFound, "generated job ids contain the run id: " + jobList.dump().substr(0, 500));

        const json runs = json::parse(ReadFileUtf8Wide(runsPath), nullptr, false);
        Check(!runs.is_discarded() && runs.is_array() && runs.size() == 2 &&
                  runs[0].value("runId", std::string()) == run1.value("runId", std::string()) &&
                  runs[1].value("runId", std::string()) == run2.value("runId", std::string()),
              ".runs.json has both runs: " + runs.dump());

        json shown = h.Call({{"action", "routine_show"}, {"name", "build-smoke"}, {"cwd", projUtf8}});
        Check(shown.value("ok", false) && shown.contains("lastRun") && shown["lastRun"].is_object() &&
                  shown["lastRun"].value("runId", std::string()) == run2.value("runId", std::string()),
              "routine show returns the last run: " + shown.dump().substr(0, 500));
        bool nodeOk = false;
        if (shown.contains("nodes") && shown["nodes"].is_array()) {
            for (const auto& node : shown["nodes"]) {
                if (node.value("node", std::string()) == "report" &&
                    node.value("state", std::string()) == "succeeded" &&
                    node.value("duration_ms", 0) >= 0) nodeOk = true;
            }
        }
        Check(nodeOk, "routine show has a per-node result row for report: " +
                          (shown.contains("nodes") ? shown["nodes"].dump() : "no nodes"));

        json jobGet = h.Call({{"action", "taskqueue_get"}, {"id", jobId}});
        bool reportSummaryOk = false;
        if (jobGet.value("ok", false) && jobGet.contains("job") && jobGet["job"].is_object() &&
            jobGet["job"].contains("attempts") && jobGet["job"]["attempts"].is_array() &&
            !jobGet["job"]["attempts"].empty()) {
            const json& lastAttempt = jobGet["job"]["attempts"].back();
            if (lastAttempt.contains("test_report_summary")) {
                const json& s = lastAttempt["test_report_summary"];
                reportSummaryOk = s.value("total", 0) == 2 && s.value("failed", 0) == 0 &&
                                  s.value("skipped", 0) == 0 &&
                                  lastAttempt.value("check_detail", std::string()) == "ok";
            }
            std::fprintf(stdout, "last attempt: %s\n", lastAttempt.dump().c_str());
        }
        Check(reportSummaryOk, "successful attempt keeps test_report_summary in taskqueue get: " +
                                   jobGet.dump().substr(0, 700));

        json checked = h.Call({{"action", "routine_check"}, {"name", "build-smoke"}, {"cwd", projUtf8}});
        Check(checked.value("ok", false) && checked.value("state", std::string()) == "ok" &&
                  checked.contains("added") && checked.contains("removed") && checked.contains("changed") &&
                  checked["added"].empty() && checked["removed"].empty() && checked["changed"].empty(),
              "routine check has the placeholder shape: " + checked.dump());
    }

    // 093-3: inputs snapshots, stale detection, absorbed foreach changes, and triage.
    {
        IpcTestHarness h;
        const std::wstring inputsDir = PathJoinWide(projW, L"inputs");
        const std::wstring casesDir = PathJoinWide(projW, L"cases");
        CreateDirectoryW(inputsDir.c_str(), nullptr);
        CreateDirectoryW(casesDir.c_str(), nullptr);
        const std::wstring aTxt = PathJoinWide(inputsDir, L"a.txt");
        const std::wstring bTxt = PathJoinWide(inputsDir, L"b.txt");
        const std::wstring case1 = PathJoinWide(casesDir, L"case1.json");
        const std::wstring case2 = PathJoinWide(casesDir, L"case2.json");
        const std::wstring failMarker = PathJoinWide(projW, L"fail.marker");
        WriteFileAtomicWide(aTxt, "alpha\n");
        WriteFileAtomicWide(case1, "{}\n");
        WriteFileAtomicWide(PathJoinWide(projW, L"fail_check.bat"),
                            "@echo off\r\nif exist fail.marker exit /b 1\r\nexit /b 0\r\n");
        RemoveFileWide(failMarker);

        json staleBt = {
            {"id", "routine-bt-stale"},
            {"summary", "Stale detection test"},
            {"cwd", projUtf8},
            {"root", {{"type", "sequence"}, {"children", json::array({
                {{"type", "foreach"}, {"id", "cases"}, {"glob", "cases/*.json"}, {"as", "item"},
                 {"do", {{"type", "job"}, {"id", "case"}, {"summary", "Handle a case"},
                         {"command", "cmd.exe /c .\\write_report.bat"}, {"timeoutSec", 30}}}},
                {{"type", "job"}, {"id", "verify"}, {"summary", "Verify"},
                 {"command", "cmd.exe /c .\\fail_check.bat"}, {"timeoutSec", 30}}
            })}}}
        };
        json save = h.Call({
            {"action", "routine_save"}, {"name", "stale-smoke"}, {"bt", staleBt},
            {"inputs", json::array({"inputs/*.txt", "cases/*.json"})},
            {"summary", "stale smoke"}, {"notes", ""}, {"scope", "project"}, {"cwd", projUtf8},
            {"by", "routine_smoke"}, {"reason", "stale setup"}
        });
        Check(save.value("ok", false), "stale routine saved: " + save.dump());

        json def = json::parse(ReadFileUtf8Wide(PathJoinWide(PathJoinWide(PathJoinWide(projW, L".hancho"),
                                                                          L"routines"), L"stale-smoke.json")),
                               nullptr, false);
        Check(!def.is_discarded() && def["inputsSnapshot"].is_object() &&
                  def["inputsSnapshot"].contains("inputs/a.txt") &&
                  def["inputsSnapshot"].contains("cases/case1.json") &&
                  def.value("foreachSnapshot", json::array()).size() == 1,
              "save recorded input hashes and the foreach snapshot: " +
                  (def.is_discarded() ? std::string("invalid def") : def["inputsSnapshot"].dump()));

        json checked = h.Call({{"action", "routine_check"}, {"name", "stale-smoke"}, {"cwd", projUtf8}});
        Check(checked.value("ok", false) && checked.value("state", std::string()) == "ok",
              "check is ok right after save: " + checked.dump());

        WriteFileAtomicWide(aTxt, "beta\n");
        checked = h.Call({{"action", "routine_check"}, {"name", "stale-smoke"}, {"cwd", projUtf8}});
        Check(checked.value("state", std::string()) == "stale" &&
                  std::find(checked["changed"].begin(), checked["changed"].end(), json("inputs/a.txt")) !=
                      checked["changed"].end(),
              "changing an input file makes the routine stale with a changed entry: " + checked.dump());

        WriteFileAtomicWide(bTxt, "gamma\n");
        checked = h.Call({{"action", "routine_check"}, {"name", "stale-smoke"}, {"cwd", projUtf8}});
        Check(checked.value("state", std::string()) == "stale" &&
                  std::find(checked["added"].begin(), checked["added"].end(), json("inputs/b.txt")) !=
                      checked["added"].end(),
              "adding an input file appears in added: " + checked.dump());

        RemoveFileWide(bTxt);
        WriteFileAtomicWide(aTxt, "alpha\n");
        checked = h.Call({{"action", "routine_check"}, {"name", "stale-smoke"}, {"cwd", projUtf8}});
        Check(checked.value("state", std::string()) == "ok", "restoring inputs returns to ok: " + checked.dump());

        WriteFileAtomicWide(case2, "{}\n");
        checked = h.Call({{"action", "routine_check"}, {"name", "stale-smoke"}, {"cwd", projUtf8}});
        Check(checked.value("state", std::string()) == "ok" &&
                  std::find(checked["absorbed"].begin(), checked["absorbed"].end(), json("cases/case2.json")) !=
                      checked["absorbed"].end(),
              "a new foreach-matched file is absorbed, not stale: " + checked.dump());
        RemoveFileWide(case2);

        WriteFileAtomicWide(aTxt, "delta\n");
        json staleRun = h.Call({{"action", "routine_run"}, {"name", "stale-smoke"}, {"cwd", projUtf8},
                                {"wait", true}, {"timeout_ms", 20000}});
        Check(staleRun.value("ok", false) && staleRun.value("state", std::string()) == "succeeded" &&
                  staleRun.value("inputsState", std::string()) == "stale",
              "a stale routine still runs and reports its stale state: " + staleRun.dump().substr(0, 400));
        checked = h.Call({{"action", "routine_check"}, {"name", "stale-smoke"}, {"cwd", projUtf8}});
        Check(checked.value("state", std::string()) == "ok",
              "a successful stale run refreshes the snapshot: " + checked.dump());
        json defAfter = json::parse(ReadFileUtf8Wide(PathJoinWide(PathJoinWide(PathJoinWide(projW, L".hancho"),
                                                                              L"routines"), L"stale-smoke.json")),
                                    nullptr, false);
        bool refreshedByRun = false;
        if (!defAfter.is_discarded() && defAfter.contains("history") && defAfter["history"].is_array()) {
            for (const auto& entry : defAfter["history"]) {
                if (entry.value("by", std::string()) == "routine-run") refreshedByRun = true;
            }
        }
        Check(refreshedByRun, "snapshot refresh is recorded in history with by=routine-run");

        WriteFileAtomicWide(aTxt, "epsilon\n");
        WriteFileAtomicWide(failMarker, "1\n");
        json failedRun = h.Call({{"action", "routine_run"}, {"name", "stale-smoke"}, {"cwd", projUtf8},
                                 {"wait", true}, {"timeout_ms", 20000}});
        Check(failedRun.value("ok", false) && failedRun.value("state", std::string()) == "failed" &&
                  failedRun.value("inputsState", std::string()) == "stale",
              "a stale routine that fails reports failed: " + failedRun.dump().substr(0, 400));

        json cards = h.Call({{"action", "triage_list"}, {"state", "pending"}});
        bool staleCard = false;
        bool updateRecommended = false;
        if (cards.value("ok", false) && cards.contains("cards") && cards["cards"].is_array()) {
            for (const auto& card : cards["cards"]) {
                if (card.value("kind", std::string()) != "routine_stale_failure") continue;
                if (card.value("target", json::object()).value("routine", std::string()) != "stale-smoke") continue;
                staleCard = true;
                if (card.value("recommendation", json::object()).value("candidateId", std::string()) ==
                    "update_routine") updateRecommended = true;
                bool hasDiff = false, hasFailedNode = false;
                const json info = card.value("info", json::object());
                for (const auto& item : info.value("conditions", json::array())) {
                    const std::string key = item.value("k", std::string());
                    if (key == "inputDiff") hasDiff = true;
                    if (key == "failedNodes") hasFailedNode = true;
                }
                Check(hasDiff, "triage evidence contains the input diff: " + info.dump().substr(0, 500));
                Check(hasFailedNode, "triage evidence contains the failed nodes: " + info.dump().substr(0, 500));
            }
        }
        Check(staleCard, "failed stale run created a routine_stale_failure card: " + cards.dump().substr(0, 600));
        Check(updateRecommended, "the card recommends update_routine: " + cards.dump().substr(0, 600));

        json shown = h.Call({{"action", "routine_show"}, {"name", "stale-smoke"}, {"cwd", projUtf8}});
        bool newlyFailedOk = false;
        if (shown.contains("newlyFailed") && shown["newlyFailed"].is_array()) {
            for (const auto& node : shown["newlyFailed"]) {
                if (node.get<std::string>() == "verify") newlyFailedOk = true;
            }
        }
        Check(newlyFailedOk, "routine show reports verify as newlyFailed vs the previous run: " +
                                 (shown.contains("newlyFailed") ? shown["newlyFailed"].dump() : std::string("no field")));

        RemoveFileWide(failMarker);
        RemoveFileWide(aTxt);
        RemoveFileWide(bTxt);
        RemoveFileWide(case1);
        RemoveFileWide(case2);
    }

    // 093-5: routine ask, proposals, approvals, validation/fix, triage and --from-bt.
    {
        IpcTestHarness h;
        const std::wstring routinesDir = PathJoinWide(PathJoinWide(projW, L".hancho"), L"routines");
        const std::wstring proposalsDir = PathJoinWide(routinesDir, L".proposals");
        const std::wstring llmProposal = PathJoinWide(proposalsDir, L"llm-smoke.json");
        const std::wstring llmApproved = PathJoinWide(routinesDir, L"llm-smoke.json");

        // 1. A fake worker produces a definition; Hancho validates it and saves a proposal.
        json asked = h.Call({{"action", "routine_ask"},
                             {"request", "全スモークを流して"},
                             {"cwd", projUtf8}, {"scope", "project"},
                             {"by", "chat"}, {"timeout_ms", 60000}});
        Check(asked.value("ok", false) && asked.value("status", std::string()) == "proposed" &&
                  asked.value("name", std::string()) == "llm-smoke" &&
                  asked.value("engine", std::string()) == "fake-good",
              "routine ask saved a proposal via the fake worker: " + asked.dump().substr(0, 400));
        Check(PathExistsWide(llmProposal) && !PathExistsWide(llmApproved),
              "the proposal is kept beside (not over) the approved definition");
        Check(asked.value("dryRun", json::object()).value("count", -1) == 1 &&
                  !asked.value("rationale", std::string()).empty(),
              "ask reports a one-job dry run and the worker rationale: " + asked.dump().substr(0, 400));
        json shown = h.Call({{"action", "routine_show"}, {"name", "llm-smoke"}, {"cwd", projUtf8}});
        Check(shown.value("ok", false) && shown.value("approved", true) == false &&
                  shown.value("hasProposal", false) &&
                  shown.value("proposal", json::object()).value("rationale", std::string()) ==
                      "The user asked for it and the repository smoke tests use write_report.bat.",
              "routine show exposes the proposal, diff inputs and rationale: " + shown.dump().substr(0, 500));

        // MCP (LLM) routine_save also produces a proposal.
        json mcpSave = h.Call({{"action", "routine_save"}, {"name", "mcp-proposed"}, {"bt", bt},
                               {"summary", "from MCP"}, {"scope", "project"}, {"cwd", projUtf8},
                               {"by", "mcp"},
                               {"origin", json{{"client", "mcp"}, {"label", ""}}}});
        Check(mcpSave.value("ok", false) && PathExistsWide(PathJoinWide(proposalsDir, L"mcp-proposed.json")),
              "MCP routine_save is stored as a proposal: " + mcpSave.dump());

        // 2. A proposed routine cannot run; approval needs real user words.
        json runProposed = h.Call({{"action", "routine_run"}, {"name", "llm-smoke"}, {"cwd", projUtf8},
                                   {"wait", true}, {"timeout_ms", 20000}});
        Check(!runProposed.value("ok", true) && runProposed.value("error", std::string()).find("approve") != std::string::npos,
              "routine run refuses a proposed-only routine: " + runProposed.dump());
        json emptyApproval = h.Call({{"action", "routine_approve"}, {"name", "llm-smoke"}, {"cwd", projUtf8},
                                     {"userSaid", ""}, {"via", "chat"}});
        Check(!emptyApproval.value("ok", true), "empty --user-said is not an approval: " + emptyApproval.dump());
        json approval = h.Call({{"action", "routine_approve"}, {"name", "llm-smoke"}, {"cwd", projUtf8},
                                {"userSaid", "承認する"}, {"via", "chat"}, {"by", "chat"}});
        Check(approval.value("ok", false) && approval.value("status", std::string()) == "approved" &&
                  approval.value("approvedVia", std::string()) == "chat" && PathExistsWide(llmApproved) &&
                  !PathExistsWide(llmProposal),
              "approval promotes the proposal to the approved version: " + approval.dump());
        json approvedRun = h.Call({{"action", "routine_run"}, {"name", "llm-smoke"}, {"cwd", projUtf8},
                                   {"wait", true}, {"timeout_ms", 20000}});
        Check(approvedRun.value("ok", false) && approvedRun.value("state", std::string()) == "succeeded",
              "an approved routine runs: " + approvedRun.dump().substr(0, 300));

        // 3. A revision proposal leaves the approved version usable until it is approved.
        WriteFileAtomicWide(PathJoinWide(tempDirW, L"answer-good.txt"),
                            answerJson("llm-smoke", false, "Updated by LLM"));
        json updateAsk = h.Call({{"action", "routine_ask"}, {"request", "直す"}, {"name", "llm-smoke"},
                                 {"cwd", projUtf8}, {"scope", "project"}, {"timeout_ms", 60000}});
        Check(updateAsk.value("ok", false) && PathExistsWide(llmProposal),
              "a second ask for an existing routine creates a revision proposal: " + updateAsk.dump().substr(0, 300));
        json beforeApprove = json::parse(ReadFileUtf8Wide(llmApproved), nullptr, false);
        Check(beforeApprove.value("summary", std::string()) == "Generated by fake worker",
              "the approved version is unchanged while a revision is pending: " + beforeApprove.dump().substr(0, 200));
        json runDuringProposal = h.Call({{"action", "routine_run"}, {"name", "llm-smoke"}, {"cwd", projUtf8},
                                         {"wait", true}, {"timeout_ms", 20000}});
        Check(runDuringProposal.value("state", std::string()) == "succeeded",
              "the approved version still runs during a pending revision");
        json approveUpdate = h.Call({{"action", "routine_approve"}, {"name", "llm-smoke"}, {"cwd", projUtf8},
                                     {"userSaid", "OK"}, {"via", "chat"}});
        json afterApprove = json::parse(ReadFileUtf8Wide(llmApproved), nullptr, false);
        Check(approveUpdate.value("ok", false) &&
                  afterApprove.value("summary", std::string()) == "Updated by LLM",
              "approving a revision replaces the approved version: " + afterApprove.dump().substr(0, 200));

        // 4. An invalid definition is fixed once; a persistently invalid one fails.
        WriteFileAtomicWide(PathJoinWide(tempDirW, L"llms.json"), llmsJson("fake-flaky").dump(2));
        RemoveFileWide(PathJoinWide(tempDirW, L"flaky-count.txt"));
        WriteFileAtomicWide(PathJoinWide(tempDirW, L"answer-bad.txt"), answerJson("fix-flaky", true));
        WriteFileAtomicWide(PathJoinWide(tempDirW, L"answer-good.txt"), answerJson("fix-flaky", false));
        json fixed = h.Call({{"action", "routine_ask"}, {"request", "fix"}, {"name", "fix-flaky"},
                             {"cwd", projUtf8}, {"timeout_ms", 60000}});
        Check(fixed.value("ok", false) && fixed.value("attempts", 0) == 2 &&
                  PathExistsWide(PathJoinWide(proposalsDir, L"fix-flaky.json")),
              "an invalid worker definition is corrected once and then saved: " + fixed.dump().substr(0, 400));
        WriteFileAtomicWide(PathJoinWide(tempDirW, L"llms.json"), llmsJson("fake-bad").dump(2));
        json failedAsk = h.Call({{"action", "routine_ask"}, {"request", "fix"}, {"name", "always-bad"},
                                 {"cwd", projUtf8}, {"timeout_ms", 60000}});
        Check(!failedAsk.value("ok", true) &&
                  failedAsk.value("error", std::string()).find("one fix") != std::string::npos &&
                  !PathExistsWide(PathJoinWide(proposalsDir, L"always-bad.json")),
              "a still-invalid definition after one fix fails the job: " + failedAsk.dump().substr(0, 400));
    }

    // 5. The triage update_routine candidate launches a routine ask.
    {
        IpcTestHarness h;
        WriteFileAtomicWide(PathJoinWide(tempDirW, L"llms.json"), llmsJson("fake-good").dump(2));
        WriteFileAtomicWide(PathJoinWide(tempDirW, L"answer-good.txt"), answerJson("stale-smoke", false));
        json cards = h.Call({{"action", "triage_list"}, {"state", "pending"}});
        std::string cardId;
        if (cards.value("ok", false) && cards.contains("cards") && cards["cards"].is_array()) {
            for (const auto& card : cards["cards"]) {
                if (card.value("kind", std::string()) == "routine_stale_failure" &&
                    card.value("target", json::object()).value("routine", std::string()) == "stale-smoke") {
                    cardId = card.value("id", std::string());
                    break;
                }
            }
        }
        Check(!cardId.empty(), "the stale routine card is still available for update_routine");
        if (!cardId.empty()) {
            json decided = h.Call({{"action", "triage_decide"}, {"id", cardId},
                                   {"candidateId", "update_routine"},
                                   {"params", json{{"name", "stale-smoke"}, {"cwd", projUtf8}}},
                                   {"by", "human"}});
            Check(decided.value("ok", false) &&
                      decided.value("card", json::object()).value("decision", json::object())
                          .value("candidateId", std::string()) == "update_routine" &&
                      decided.value("card", json::object()).value("routineAsk", json::object())
                          .value("status", std::string()) == "proposed",
                  "update_routine launches routine ask and records the proposal on the card: " +
                      decided.dump().substr(0, 500));
            Check(PathExistsWide(PathJoinWide(PathJoinWide(PathJoinWide(PathJoinWide(projW, L".hancho"),
                                                                        L"routines"), L".proposals"),
                                              L"stale-smoke.json")),
                  "the triage-launched ask saved a proposal awaiting approval");
        }
    }

    // 6. --from-bt registers a past BT (human = approved, MCP = proposed).
    {
        IpcTestHarness h;
        json savedBt = {{"id", "frombt-1"}, {"summary", "A past successful BT"},
                        {"root", {{"type", "sequence"},
                                  {"children", json::array({json{{"type", "job"}, {"id", "report"},
                                      {"summary", "Write report"},
                                      {"command", "cmd.exe /c .\\write_report.bat"}, {"timeoutSec", 30}}})}}}};
        WriteFileAtomicWide(PathJoinWide(PathJoinWide(tempDirW, L"behaviortrees"), L"frombt-1.json"), savedBt.dump(2));
        json fromBt = h.Call({{"action", "routine_save_from_bt"}, {"name", "from-bt-smoke"},
                              {"btId", "frombt-1"}, {"scope", "project"}, {"cwd", projUtf8},
                              {"by", "hancho-cli"}, {"reason", "reuse a passing BT"}});
        Check(fromBt.value("ok", false) &&
                  PathExistsWide(PathJoinWide(PathJoinWide(projW, L".hancho"), L"routines") + L"\\from-bt-smoke.json"),
              "--from-bt registered an approved routine from a past BT: " + fromBt.dump());
        json fromBtMcp = h.Call({{"action", "routine_save_from_bt"}, {"name", "from-bt-mcp"},
                                 {"btId", "frombt-1"}, {"scope", "project"}, {"cwd", projUtf8},
                                 {"by", "mcp"}, {"proposed", true}, {"reason", "mcp"}});
        Check(fromBtMcp.value("ok", false) &&
                  PathExistsWide(PathJoinWide(PathJoinWide(PathJoinWide(projW, L".hancho"), L"routines"),
                                              L".proposals") + L"\\from-bt-mcp.json"),
              "--from-bt from MCP is stored as a proposal: " + fromBtMcp.dump());
    }

    // 7. A routine whose BT fails mid-run (e.g. token exhaustion / 429 quota on a worker node) reports failed.
    {
        IpcTestHarness h;
        const std::wstring inputsDir = PathJoinWide(projW, L"inputs");
        CreateDirectoryW(inputsDir.c_str(), nullptr);
        const std::wstring sampleTxt = PathJoinWide(inputsDir, L"sample.txt");
        WriteFileAtomicWide(sampleTxt, "sample data\n");

        json tokenFailBt = {
            {"id", "routine-bt-token-fail"},
            {"summary", "BT that fails mid-execution due to token exhaustion"},
            {"inputs", json::array({"inputs/sample.txt"})},
            {"root", {
                {"type", "sequence"},
                {"children", json::array({
                    json{{"type", "job"}, {"id", "step1"}, {"summary", "First step succeeds"},
                         {"command", "cmd.exe /c exit 0"}, {"timeoutSec", 30}},
                    json{{"type", "job"}, {"id", "step2"}, {"summary", "Worker hits token limit / quota 429"},
                         {"command", "cmd.exe /c echo HTTP 429: quota exceeded, resource exhausted >&2 & exit /b 1"}, {"timeoutSec", 30}}
                })}
            }}
        };
        json saveRes = h.Call({{"action", "routine_save"}, {"name", "token-fail-smoke"}, {"bt", tokenFailBt},
                               {"scope", "project"}, {"cwd", projUtf8}, {"by", "user"}});
        Check(saveRes.value("ok", false), "saved routine with token-fail sequence: " + saveRes.dump());

        json runRes = h.Call({{"action", "routine_run"}, {"name", "token-fail-smoke"}, {"cwd", projUtf8},
                              {"wait", true}, {"timeout_ms", 20000}});
        Check(runRes.value("ok", false), "routine_run completed and returned response");
        Check(runRes.value("state", std::string()) == "failed",
              "routine state is failed when mid-BT node fails: " + runRes.dump().substr(0, 300));

        std::string btId = runRes.value("btId", std::string());
        if (!btId.empty()) {
            json btInfo = h.Call({{"action", "bt_get"}, {"id", btId}});
            Check(btInfo.value("ok", false) && btInfo.value("state", std::string()) == "failed",
                  "underlying BT is also marked failed: " + btInfo.dump().substr(0, 300));
        }
    }

    if (g_failures > 0) DumpFilesForDiagnostics(tempDirW);
    RemoveDirRecursive(tempDirW);
    std::fprintf(stdout, "routine_smoke: %d passed, %d failed\n", g_passes, g_failures);
    return g_failures == 0 ? 0 : 1;
}
