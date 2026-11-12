// Smoke test for hancho-cli top TUI components (cards 062-1, 062-2, doc/tui-design-ja.md).
// Verifies key sequence parsing, tab navigation, 5 dashboard-equivalent views, and format helpers.

#include "cli_top.h"

#include <cassert>
#include <cstdio>
#include <string>
#include <vector>

using namespace pc::top;

namespace {

int g_passes = 0;
int g_failures = 0;

void Check(bool cond, const std::string& desc) {
    if (cond) {
        ++g_passes;
        std::printf("[PASS] %s\n", desc.c_str());
    } else {
        ++g_failures;
        std::printf("[FAIL] %s\n", desc.c_str());
    }
}

void TestFormatHelpers() {
    Check(FormatBytes(0) == "0 B", "FormatBytes(0)");
    Check(FormatBytes(500) == "500 B", "FormatBytes(500)");
    Check(FormatBytes(1024) == "1.0 KB", "FormatBytes(1024)");
    Check(FormatBytes(1048576) == "1.0 MB", "FormatBytes(1048576)");
    Check(FormatBytes(1073741824) == "1.00 GB", "FormatBytes(1073741824)");

    Check(PadOrTruncate("hello", 10) == "hello     ", "PadOrTruncate pad");
    Check(PadOrTruncate("hello world long string", 8) == "hello...", "PadOrTruncate truncate");

    Check(FormatTimeStr("2026-11-11T19:12:14") == "19:12:14", "FormatTimeStr ISO-T");
    Check(FormatTimeStr("2026-11-11 19:12:14") == "19:12:14", "FormatTimeStr space");
}

void TestKeyParsing() {
    Check(ParseKeySequence("\x1b") == TopKey::Escape, "Parse ESC");
    Check(ParseKeySequence("\x1b[A") == TopKey::Up, "Parse Up Arrow");
    Check(ParseKeySequence("\x1b[B") == TopKey::Down, "Parse Down Arrow");
    Check(ParseKeySequence("\x1b[C") == TopKey::Right, "Parse Right Arrow");
    Check(ParseKeySequence("\x1b[D") == TopKey::Left, "Parse Left Arrow");
    Check(ParseKeySequence("\x1b[Z") == TopKey::PrevTab, "Parse Shift+Tab");

    Check(ParseKeySequence("q") == TopKey::Quit, "Parse 'q' as Quit");
    Check(ParseKeySequence("Q") == TopKey::Quit, "Parse 'Q' as Quit");
    Check(ParseKeySequence("\x03") == TopKey::Quit, "Parse Ctrl+C as Quit");
    Check(ParseKeySequence(" ") == TopKey::Refresh, "Parse ' ' as Refresh");
    Check(ParseKeySequence("u") == TopKey::Refresh, "Parse 'u' as Refresh");
    Check(ParseKeySequence("\x12") == TopKey::Refresh, "Parse Ctrl+R as Refresh");
    Check(ParseKeySequence("s") == TopKey::JobStop, "Parse 's' as JobStop");
    Check(ParseKeySequence("S") == TopKey::JobStop, "Parse 'S' as JobStop");
    Check(ParseKeySequence("r") == TopKey::JobRetry, "Parse 'r' as JobRetry");
    Check(ParseKeySequence("R") == TopKey::JobRetry, "Parse 'R' as JobRetry");
    Check(ParseKeySequence("e") == TopKey::JobRetry, "Parse 'e' as JobRetry");
    Check(ParseKeySequence("c") == TopKey::JobCancel, "Parse 'c' as JobCancel");
    Check(ParseKeySequence("C") == TopKey::JobCancel, "Parse 'C' as JobCancel");
    Check(ParseKeySequence("x") == TopKey::JobCancel, "Parse 'x' as JobCancel");
    Check(ParseKeySequence("\t") == TopKey::NextTab, "Parse Tab as NextTab");
    Check(ParseKeySequence("?") == TopKey::Help, "Parse '?' as Help");

    Check(ParseKeySequence("1") == TopKey::TabProcesses, "Parse '1' as TabProcesses");
    Check(ParseKeySequence("2") == TopKey::TabQueue, "Parse '2' as TabQueue");
    Check(ParseKeySequence("3") == TopKey::TabMonitor, "Parse '3' as TabMonitor");
    Check(ParseKeySequence("4") == TopKey::TabDiagnostics, "Parse '4' as TabDiagnostics");
    Check(ParseKeySequence("5") == TopKey::TabMemory, "Parse '5' as TabMemory");

    Check(ParseKeySequence("k") == TopKey::Up, "Parse 'k' as Up");
    Check(ParseKeySequence("j") == TopKey::Down, "Parse 'j' as Down");
    Check(ParseKeySequence("h") == TopKey::Left, "Parse 'h' as Left");
    Check(ParseKeySequence("l") == TopKey::Right, "Parse 'l' as Right");
}

void TestStateTabNavigation() {
    TopState state;
    Check(state.activeTab == TopTab::Processes, "Default tab is Processes");

    state.NextTab();
    Check(state.activeTab == TopTab::Queue, "NextTab -> Queue");

    state.NextTab();
    Check(state.activeTab == TopTab::Monitor, "NextTab -> Monitor");

    state.NextTab();
    Check(state.activeTab == TopTab::Diagnostics, "NextTab -> Diagnostics");

    state.NextTab();
    Check(state.activeTab == TopTab::Memory, "NextTab -> Memory");

    state.NextTab();
    Check(state.activeTab == TopTab::Processes, "NextTab wraps around to Processes");

    state.PrevTab();
    Check(state.activeTab == TopTab::Memory, "PrevTab wraps backwards to Memory");

    state.SetTab(TopTab::Queue);
    Check(state.activeTab == TopTab::Queue, "SetTab sets Queue directly");
}

void TestRenderingProcessesView() {
    TopState state;
    state.coreConnected = true;
    state.timestamp = "2026-11-11 19:00:00";
    state.activeTab = TopTab::Processes;
    state.selectedRow = 0;

    // Mock process list with details
    nlohmann::json j;
    j["ok"] = true;
    j["processes"] = nlohmann::json::array();
    nlohmann::json p1;
    p1["id"] = 1;
    p1["pid"] = 4321;
    p1["state"] = "running";
    p1["elapsed_str"] = "00:01:23";
    p1["cpu_time_str"] = "1.5s";
    p1["last_line"] = "Compiling target...";
    p1["command"] = "cmake --build build";
    p1["cwd"] = "/home/user/workspace";
    p1["exit_code"] = 0;
    j["processes"].push_back(p1);
    state.processData = j;

    std::string rendered = RenderScreen(state, 80, 24, false);

    Check(rendered.find("Hancho Top") != std::string::npos, "Render contains 'Hancho Top'");
    Check(rendered.find("ONLINE") != std::string::npos, "Render contains ONLINE");
    Check(rendered.find("1: Processes") != std::string::npos, "Render contains '1: Processes'");
    Check(rendered.find("Compiling target...") != std::string::npos, "Render contains process last_line");
    Check(rendered.find(">    1  4321") != std::string::npos, "Render contains selected row pointer '>'");
    Check(rendered.find("Process Detail: #1") != std::string::npos, "Render contains process detail pane");
    Check(rendered.find("cmake --build build") != std::string::npos, "Render contains process command line");
}

void TestRenderingQueueView() {
    TopState state;
    state.coreConnected = true;
    state.activeTab = TopTab::Queue;
    state.selectedRow = 0;

    nlohmann::json counts;
    counts["ok"] = true;
    counts["running"] = 2;
    counts["waiting"] = 3;
    counts["pending"] = 5;
    counts["succeeded"] = 10;
    counts["failed"] = 1;
    counts["total"] = 21;
    state.countsData = counts;

    // Mock jobs list
    nlohmann::json qData;
    qData["ok"] = true;
    qData["jobs"] = nlohmann::json::array();
    nlohmann::json j1;
    j1["id"] = "build-1";
    j1["name"] = "Build Project";
    j1["taskqueue"] = "ci";
    j1["state"] = "running";
    j1["duration_str"] = "45s";
    j1["summary"] = "Compiling release binary";
    j1["commandLine"] = "cmake --build .";
    j1["attemptsCount"] = 1;
    j1["maxAttempts"] = 3;
    j1["timeoutSec"] = 300;
    qData["jobs"].push_back(j1);
    state.queueData = qData;

    std::string rendered = RenderScreen(state, 80, 24, false);
    Check(rendered.find("Task Queue Overview") != std::string::npos, "Render contains 'Task Queue Overview'");
    Check(rendered.find("Running: 2") != std::string::npos, "Render contains Running count 2");
    Check(rendered.find("Succeeded: 10") != std::string::npos, "Render contains Succeeded count 10");
    Check(rendered.find("Failed: 1") != std::string::npos, "Render contains Failed count 1");
    Check(rendered.find("build-1") != std::string::npos, "Render contains job ID 'build-1'");
    Check(rendered.find("Job Detail: build-1") != std::string::npos, "Render contains Job Detail header");
    Check(rendered.find("Compiling release binary") != std::string::npos, "Render contains job summary");
}

void TestRenderingMonitorView() {
    TopState state;
    state.coreConnected = true;
    state.activeTab = TopTab::Monitor;

    // Mock process
    nlohmann::json procsObj;
    procsObj["ok"] = true;
    procsObj["processes"] = nlohmann::json::array();
    nlohmann::json p1;
    p1["id"] = 2;
    p1["pid"] = 9876;
    p1["state"] = "running";
    p1["name"] = "agent-claude";
    procsObj["processes"].push_back(p1);
    state.processData = procsObj;

    // Mock agent state
    nlohmann::json agState;
    agState["ok"] = true;
    agState["agent_type"] = "claude-code";
    agState["status"] = "working";
    agState["model"] = "claude-3-7-sonnet";
    agState["goal"] = "Implement terminal dashboard";
    agState["notes"] = "Editing src/cli_top.h";
    state.monitorState = agState;

    // Mock agent event stream
    nlohmann::json streamObj;
    streamObj["ok"] = true;
    streamObj["events"] = nlohmann::json::array();
    nlohmann::json ev1;
    ev1["timestamp"] = "2026-11-11T19:05:00";
    ev1["type"] = "THOUGHT";
    ev1["payload"] = nlohmann::json::object({{"thought", "Analyzing required views"}});
    streamObj["events"].push_back(ev1);

    nlohmann::json ev2;
    ev2["timestamp"] = "2026-11-11T19:05:05";
    ev2["type"] = "TOOL_CALL";
    ev2["payload"] = nlohmann::json::object({{"tool", "read_file"}, {"args", "src/cli_top.h"}});
    streamObj["events"].push_back(ev2);

    state.monitorStream = streamObj;

    std::string rendered = RenderScreen(state, 80, 24, false);
    Check(rendered.find("Execution Monitor Stream") != std::string::npos, "Render contains 'Execution Monitor Stream'");
    Check(rendered.find("Process: [#2]") != std::string::npos, "Render contains monitored process #2");
    Check(rendered.find("claude-code") != std::string::npos, "Render contains agent_type");
    Check(rendered.find("Implement terminal dashboard") != std::string::npos, "Render contains goal");
    Check(rendered.find("THOUGHT") != std::string::npos, "Render contains THOUGHT event");
    Check(rendered.find("TOOL_CALL") != std::string::npos, "Render contains TOOL_CALL event");
    Check(rendered.find("read_file") != std::string::npos, "Render contains tool name read_file");
}

void TestRenderingDiagnosticsWhyView() {
    TopState state;
    state.coreConnected = true;
    state.activeTab = TopTab::Diagnostics;
    state.selectedRow = 0;

    // Mock diagnosis breakdown
    nlohmann::json diagBreak;
    diagBreak["ok"] = true;
    diagBreak["summary"] = "exit_nonzero x2, timeout x1";
    state.diagBreakdown = diagBreak;

    // Mock failed jobs
    nlohmann::json fData;
    fData["ok"] = true;
    fData["jobs"] = nlohmann::json::array();
    nlohmann::json jFailed;
    jFailed["id"] = "test-fail-1";
    jFailed["name"] = "Run Unit Tests";
    jFailed["taskqueue"] = "verify";
    jFailed["state"] = "failed";

    nlohmann::json failureObj;
    failureObj["class"] = "exit_nonzero";
    failureObj["confidence"] = "high";
    failureObj["summary"] = "Test assertion failed in test_top.cpp line 42";
    failureObj["suggestedAction"] = "Fix return code handling";
    failureObj["retryAt"] = "2026-11-11 19:15:00";

    nlohmann::json lastAct;
    lastAct["activity"] = "Run pytest";
    lastAct["target"] = "tests/test_top.py";
    lastAct["sinceSec"] = 12;
    failureObj["lastActivity"] = lastAct;

    nlohmann::json llmG;
    llmG["class"] = "assertion_failure";
    llmG["summary"] = "Expected 0 but got 1";
    llmG["suggestedAction"] = "Review assertions";
    failureObj["llmGuess"] = llmG;

    nlohmann::json att;
    att["attempt"] = 1;
    att["failure"] = failureObj;
    jFailed["attempts"] = nlohmann::json::array({att});

    fData["jobs"].push_back(jFailed);
    state.failedJobsData = fData;
    state.diagJobDetail = jFailed;

    std::string rendered = RenderScreen(state, 80, 24, false);
    Check(rendered.find("Failure Diagnostics (Why)") != std::string::npos, "Render contains 'Failure Diagnostics (Why)'");
    Check(rendered.find("exit_nonzero x2, timeout x1") != std::string::npos, "Render contains failure categories summary");
    Check(rendered.find("test-fail-1") != std::string::npos, "Render contains failed job ID");
    Check(rendered.find("Why: exit_nonzero (high)") != std::string::npos, "Render contains Why failure class");
    Check(rendered.find("Test assertion failed") != std::string::npos, "Render contains failure summary");
    Check(rendered.find("Next: Fix return code handling") != std::string::npos, "Render contains Next action");
    Check(rendered.find("Last activity: Run pytest tests/test_top.py (12s ago)") != std::string::npos, "Render contains Last activity");
    Check(rendered.find("LLM guess: assertion_failure") != std::string::npos, "Render contains LLM guess");
}

void TestRenderingMemoryView() {
    TopState state;
    state.coreConnected = true;
    state.activeTab = TopTab::Memory;
    state.selectedRow = 0;

    // Mock storage stats
    nlohmann::json mStats;
    mStats["ok"] = true;
    mStats["total_bytes"] = 1048576LL;    // 1.0 MB
    mStats["limit_bytes"] = 10485760LL;   // 10.0 MB
    mStats["l1_bytes"] = 524288LL;        // 512 KB
    mStats["l2_bytes"] = 262144LL;        // 256 KB
    mStats["l3_bytes"] = 131072LL;        // 128 KB
    mStats["warning"] = "";
    state.memoryStats = mStats;

    // Mock memory records
    nlohmann::json mRecs;
    mRecs["ok"] = true;
    mRecs["results"] = nlohmann::json::array();

    nlohmann::json r1;
    r1["id"] = "run-20261111-001";
    r1["kind"] = "l1";
    r1["jobName"] = "compile-core";
    r1["status"] = "succeeded";
    r1["summary"] = "Compiled 42 source files in 15s";
    r1["createdAt"] = "2026-11-11T18:30:00";
    mRecs["results"].push_back(r1);

    nlohmann::json r2;
    r2["id"] = "note-cmake-ninja";
    r2["kind"] = "l2";
    r2["jobName"] = "build-env";
    r2["status"] = "active";
    r2["summary"] = "Ninja generator requires MSVC developer prompt";
    r2["createdAt"] = "2026-11-11T18:40:00";
    mRecs["results"].push_back(r2);

    nlohmann::json r3;
    r3["id"] = "know-posix-signal";
    r3["kind"] = "l3";
    r3["jobName"] = "core";
    r3["status"] = "approved";
    r3["summary"] = "SIGCHLD handler needs SA_NOCLDSTOP";
    r3["createdAt"] = "2026-11-11T18:50:00";
    mRecs["results"].push_back(r3);

    state.memoryRecords = mRecs;

    std::string rendered = RenderScreen(state, 80, 24, false);
    Check(rendered.find("Memory & Knowledge Records") != std::string::npos, "Render contains 'Memory & Knowledge Records'");
    Check(rendered.find("Storage: 1.0 MB / 10.0 MB (10%)") != std::string::npos, "Render contains Storage quota usage");
    Check(rendered.find("Healthy") != std::string::npos, "Render contains storage Healthy status");
    Check(rendered.find("[L1]") != std::string::npos, "Render contains [L1] badge");
    Check(rendered.find("run-20261111-001") != std::string::npos, "Render contains L1 record ID");
    Check(rendered.find("[L2]") != std::string::npos, "Render contains [L2] badge");
    Check(rendered.find("note-cmake-ninja") != std::string::npos, "Render contains L2 record ID");
    Check(rendered.find("[L3]") != std::string::npos, "Render contains [L3] badge");
    Check(rendered.find("know-posix-signal") != std::string::npos, "Render contains L3 record ID");
    Check(rendered.find("Memory Record: run-20261111-001") != std::string::npos, "Render contains selected memory record preview");
    Check(rendered.find("Compiled 42 source files in 15s") != std::string::npos, "Render contains record summary");
}

void TestRemoteHelpers() {
    Check(FormatQueueColumn("main", "buildbox") == "main @ buildbox", "FormatQueueColumn with host");
    Check(FormatQueueColumn("ci", "") == "ci", "FormatQueueColumn local queue");
    Check(FormatQueueColumn("", "worker-1") == "default @ worker-1", "FormatQueueColumn default empty queue");

    nlohmann::json j1;
    j1["host"] = "buildbox";
    Check(GetJobHost(j1) == "buildbox", "GetJobHost job.host");

    nlohmann::json j2;
    j2["remote_host"] = "worker-2";
    Check(GetJobHost(j2) == "worker-2", "GetJobHost job.remote_host");

    nlohmann::json j3;
    j3["attempts"] = nlohmann::json::array();
    nlohmann::json a3;
    a3["remote_host"] = "box-3";
    j3["attempts"].push_back(a3);
    Check(GetJobHost(j3) == "box-3", "GetJobHost attempt remote_host");

    nlohmann::json j4;
    Check(GetJobHost(j4) == "", "GetJobHost empty");

    // Merge remote jobs
    nlohmann::json localJobs = nlohmann::json::array();
    nlohmann::json local1;
    local1["id"] = "local-1";
    localJobs.push_back(local1);

    nlohmann::json remDoc;
    remDoc["ok"] = true;
    remDoc["hosts"] = nlohmann::json::array();
    nlohmann::json h1;
    h1["host"] = "buildbox";
    h1["response"] = nlohmann::json::object();
    h1["response"]["jobs"] = nlohmann::json::array();
    nlohmann::json rJob;
    rJob["id"] = "remote-job-1";
    rJob["taskqueue"] = "main";
    h1["response"]["jobs"].push_back(rJob);
    remDoc["hosts"].push_back(h1);

    MergeRemoteJobs(localJobs, remDoc);
    Check(localJobs.size() == 2, "MergeRemoteJobs added 1 remote job");
    Check(localJobs[1].value("id", "") == "remote-job-1", "Merged job ID correct");
    Check(localJobs[1].value("host", "") == "buildbox", "Merged job host attribution set");

    // Deduplication check
    MergeRemoteJobs(localJobs, remDoc);
    Check(localJobs.size() == 2, "MergeRemoteJobs deduplicates existing job ID");

    // Merge remote processes
    nlohmann::json localProcs = nlohmann::json::array();
    nlohmann::json lp1;
    lp1["id"] = 1;
    lp1["pid"] = 1001;
    localProcs.push_back(lp1);

    nlohmann::json remProcDoc;
    remProcDoc["ok"] = true;
    remProcDoc["hosts"] = nlohmann::json::array();
    nlohmann::json hp1;
    hp1["host"] = "box-a";
    hp1["response"] = nlohmann::json::object();
    hp1["response"]["processes"] = nlohmann::json::array();
    nlohmann::json rp1;
    rp1["id"] = 1; // ID 1 on box-a
    rp1["pid"] = 9999;
    hp1["response"]["processes"].push_back(rp1);
    remProcDoc["hosts"].push_back(hp1);

    MergeRemoteProcesses(localProcs, remProcDoc);
    Check(localProcs.size() == 2, "MergeRemoteProcesses added remote process");
    Check(localProcs[1].value("host", "") == "box-a", "Remote process has host set");
}

void TestJobOperationRequests() {
    TopState state;

    // 1. Process tab Stop
    state.activeTab = TopTab::Processes;
    state.selectedRow = 0;
    nlohmann::json pData;
    pData["processes"] = nlohmann::json::array();
    nlohmann::json p1;
    p1["id"] = 42;
    p1["pid"] = 12345;
    pData["processes"].push_back(p1);
    state.processData = pData;

    std::string req, desc;
    Check(BuildJobStopRequest(state, req, desc), "BuildJobStopRequest for Process");
    Check(req.find("\"action\":\"kill\"") != std::string::npos, "Stop request action is kill");
    Check(req.find("\"id\":42") != std::string::npos, "Stop request id is 42");

    // 2. Queue tab Stop on running job
    state.activeTab = TopTab::Queue;
    state.selectedRow = 0;
    nlohmann::json qData;
    qData["jobs"] = nlohmann::json::array();
    nlohmann::json j1;
    j1["id"] = "job-run-1";
    j1["state"] = "running";
    nlohmann::json a1;
    a1["process_id"] = 5432;
    j1["attempts"] = nlohmann::json::array({a1});
    qData["jobs"].push_back(j1);
    state.queueData = qData;

    Check(BuildJobStopRequest(state, req, desc), "BuildJobStopRequest for running Queue job");
    Check(req.find("\"action\":\"kill\"") != std::string::npos, "Stop running job kills its process");
    Check(req.find("\"id\":5432") != std::string::npos, "Kills PID 5432");

    // 3. Queue tab Retry
    Check(BuildJobRetryRequest(state, req, desc), "BuildJobRetryRequest for Queue job");
    Check(req.find("\"action\":\"taskqueue_retry\"") != std::string::npos, "Retry request action is taskqueue_retry");
    Check(req.find("\"id\":\"job-run-1\"") != std::string::npos, "Retry job id is job-run-1");
    Check(req.find("\"reset_attempts\":true") != std::string::npos, "Retry resets attempts");

    // 4. Queue tab Cancel
    Check(BuildJobCancelRequest(state, req, desc), "BuildJobCancelRequest for Queue job");
    Check(req.find("\"action\":\"taskqueue_cancel\"") != std::string::npos, "Cancel request action is taskqueue_cancel");
    Check(req.find("\"id\":\"job-run-1\"") != std::string::npos, "Cancel job id is job-run-1");

    // 5. Remote job Stop/Cancel
    nlohmann::json jRemote;
    jRemote["id"] = "rem-job-9";
    jRemote["host"] = "buildbox";
    jRemote["state"] = "running";
    qData["jobs"].push_back(jRemote);
    state.selectedRow = 1;
    state.queueData = qData;

    Check(BuildJobStopRequest(state, req, desc), "BuildJobStopRequest for remote running job");
    Check(req.find("\"action\":\"taskqueue_cancel\"") != std::string::npos, "Stopping remote job cancels it");
    Check(desc.find("buildbox") != std::string::npos, "Description includes remote host name");

    Check(BuildJobRetryRequest(state, req, desc), "BuildJobRetryRequest for remote job");
    Check(desc.find("buildbox") != std::string::npos, "Retry description includes remote host name");
}

void TestRenderingRemoteJobsInQueueView() {
    TopState state;
    state.coreConnected = true;
    state.activeTab = TopTab::Queue;
    state.selectedRow = 0;

    nlohmann::json qData;
    qData["ok"] = true;
    qData["jobs"] = nlohmann::json::array();
    nlohmann::json rJob;
    rJob["id"] = "rem-compile-1";
    rJob["name"] = "Remote Compile";
    rJob["taskqueue"] = "main";
    rJob["host"] = "buildbox";
    rJob["state"] = "running";
    rJob["duration_str"] = "1m12s";
    rJob["summary"] = "Compiling on remote buildbox";

    nlohmann::json att;
    att["remote_host"] = "buildbox";
    att["remote_process_id"] = 7788;
    att["n"] = 1;
    rJob["attempts"] = nlohmann::json::array({att});

    qData["jobs"].push_back(rJob);
    state.queueData = qData;

    std::string rendered = RenderScreen(state, 80, 24, false);
    Check(rendered.find("rem-compile-1") != std::string::npos, "Render contains remote job ID");
    Check(rendered.find("main @ buildbox") != std::string::npos, "Render contains 'main @ buildbox' queue column");
    Check(rendered.find("Job Detail: rem-compile-1") != std::string::npos, "Render contains remote Job Detail header");
    Check(rendered.find("Host: buildbox") != std::string::npos, "Render contains Host: buildbox");
    Check(rendered.find("Remote PID: 7788") != std::string::npos, "Render contains Remote PID: 7788");
}

void TestRenderingRemoteJobsInDiagnosticsView() {
    TopState state;
    state.coreConnected = true;
    state.activeTab = TopTab::Diagnostics;
    state.selectedRow = 0;

    nlohmann::json fData;
    fData["ok"] = true;
    fData["jobs"] = nlohmann::json::array();
    nlohmann::json fj;
    fj["id"] = "fail-rem-01";
    fj["taskqueue"] = "verify";
    fj["host"] = "testbox";
    fj["state"] = "failed";

    nlohmann::json failObj;
    failObj["class"] = "timeout";
    failObj["summary"] = "Remote test execution timed out after 300s";
    failObj["suggestedAction"] = "Retry with increased timeout";

    nlohmann::json att;
    att["failure"] = failObj;
    att["remote_host"] = "testbox";
    fj["attempts"] = nlohmann::json::array({att});

    fData["jobs"].push_back(fj);
    state.failedJobsData = fData;
    state.diagJobDetail = fj;

    std::string rendered = RenderScreen(state, 80, 24, false);
    Check(rendered.find("fail-rem-01") != std::string::npos, "Render contains failed remote job ID");
    Check(rendered.find("verify @ testbox") != std::string::npos, "Render contains 'verify @ testbox' in table");
    Check(rendered.find("queue=verify @ testbox") != std::string::npos, "Render detail shows queue=verify @ testbox");
    Check(rendered.find("host=testbox") != std::string::npos, "Render detail shows host=testbox");
}

void TestRenderingRemoteProcessesView() {
    TopState state;
    state.coreConnected = true;
    state.activeTab = TopTab::Processes;
    state.selectedRow = 0;

    nlohmann::json pData;
    pData["ok"] = true;
    pData["processes"] = nlohmann::json::array();
    nlohmann::json rp;
    rp["id"] = 5;
    rp["pid"] = 8899;
    rp["host"] = "buildbox";
    rp["state"] = "running";
    rp["command"] = "clang++ -c src/main.cpp";
    pData["processes"].push_back(rp);
    state.processData = pData;

    std::string rendered = RenderScreen(state, 80, 24, false);
    Check(rendered.find("[buildbox]") != std::string::npos, "Render contains [buildbox] tag for remote process");
    Check(rendered.find("Host: buildbox") != std::string::npos, "Render detail contains Host: buildbox");
}

void TestRenderingHelpOverlay() {
    TopState state;
    state.showHelp = true;

    std::string rendered = RenderScreen(state, 80, 24, false);
    Check(rendered.find("Hancho Top Keyboard Shortcuts") != std::string::npos, "Render contains help shortcuts title");
    Check(rendered.find("Quit hancho-cli top") != std::string::npos, "Render contains quit shortcut help");
    Check(rendered.find("Stop selected process or job") != std::string::npos, "Render contains Stop shortcut help");
    Check(rendered.find("Retry / rerun selected job") != std::string::npos, "Render contains Retry shortcut help");
    Check(rendered.find("Cancel selected job") != std::string::npos, "Render contains Cancel shortcut help");
}

void TestExtendedKeyParsing() {
    Check(ParseKeySequence("\x1b[H") == TopKey::Home, "Parse Home key");
    Check(ParseKeySequence("\x1b[F") == TopKey::End, "Parse End key");
    Check(ParseKeySequence("\x1b[5~") == TopKey::PageUp, "Parse PageUp key");
    Check(ParseKeySequence("\x1b[6~") == TopKey::PageDown, "Parse PageDown key");
    Check(ParseKeySequence("\r") == TopKey::Enter, "Parse CR as Enter");
    Check(ParseKeySequence("\n") == TopKey::Enter, "Parse LF as Enter");
    Check(ParseKeySequence("\x1bOA") == TopKey::Up, "Parse SS3 Up Arrow");
    Check(ParseKeySequence("\x1bOB") == TopKey::Down, "Parse SS3 Down Arrow");
    Check(ParseKeySequence("\x1bOC") == TopKey::Right, "Parse SS3 Right Arrow");
    Check(ParseKeySequence("\x1bOD") == TopKey::Left, "Parse SS3 Left Arrow");
}

void TestRenderingOfflineAndStatusMessage() {
    TopState state;
    state.coreConnected = false;
    state.statusMessage = "Stopped process #42.";
    state.activeTab = TopTab::Processes;

    std::string rendered = RenderScreen(state, 80, 24, false);
    Check(rendered.find("OFFLINE") != std::string::npos, "Render contains OFFLINE status");
    Check(rendered.find("Stopped process #42.") != std::string::npos, "Render contains statusMessage toast");
}

void TestRenderingEmptyLists() {
    TopState state;
    state.coreConnected = true;

    // 1. Empty processes
    state.activeTab = TopTab::Processes;
    state.processData = nlohmann::json::object({{"processes", nlohmann::json::array()}});
    std::string rendProc = RenderScreen(state, 80, 24, false);
    Check(rendProc.find("(No running or recent processes)") != std::string::npos, "Render empty processes placeholder");

    // 2. Empty queue
    state.activeTab = TopTab::Queue;
    state.queueData = nlohmann::json::object({{"jobs", nlohmann::json::array()}});
    std::string rendQ = RenderScreen(state, 80, 24, false);
    Check(rendQ.find("(Queue is empty)") != std::string::npos, "Render empty queue placeholder");

    // 3. Empty monitor
    state.activeTab = TopTab::Monitor;
    state.processData = nlohmann::json::object({{"processes", nlohmann::json::array()}});
    state.monitorStream = nlohmann::json::object({{"events", nlohmann::json::array()}});
    std::string rendMon = RenderScreen(state, 80, 24, false);
    Check(rendMon.find("(No running or recent processes)") != std::string::npos ||
          rendMon.find("(No agent events recorded yet)") != std::string::npos, "Render empty monitor placeholder");

    // 4. Empty diagnostics
    state.activeTab = TopTab::Diagnostics;
    state.failedJobsData = nlohmann::json::object({{"jobs", nlohmann::json::array()}});
    std::string rendDiag = RenderScreen(state, 80, 24, false);
    Check(rendDiag.find("(No failed jobs recorded)") != std::string::npos, "Render empty diagnostics placeholder");

    // 5. Empty memory
    state.activeTab = TopTab::Memory;
    state.memoryRecords = nlohmann::json::object({{"results", nlohmann::json::array()}});
    std::string rendMem = RenderScreen(state, 80, 24, false);
    Check(rendMem.find("(No memory records found)") != std::string::npos, "Render empty memory placeholder");
}

void TestColorAndNoColorRendering() {
    TopState state;
    state.coreConnected = true;
    state.activeTab = TopTab::Processes;

    std::string noColor = RenderScreen(state, 80, 24, false);
    Check(noColor.find("\x1b[") == std::string::npos, "Render without color has no ANSI escapes");

    std::string withColor = RenderScreen(state, 80, 24, true);
    Check(withColor.find("\x1b[") != std::string::npos, "Render with color contains ANSI escapes");
}

void TestNarrowTerminalHandling() {
    TopState state;
    state.coreConnected = true;
    state.activeTab = TopTab::Processes;

    // Small dimensions (width 40, height 12)
    std::string rendered = RenderScreen(state, 40, 12, false);
    Check(!rendered.empty(), "RenderScreen handles narrow width and short height");
    Check(rendered.find("Hancho Top") != std::string::npos, "Narrow render contains title");
}

} // namespace

int main() {
    std::printf("=== Starting cli_top_smoke ===\n");
    TestFormatHelpers();
    TestRemoteHelpers();
    TestKeyParsing();
    TestExtendedKeyParsing();
    TestJobOperationRequests();
    TestStateTabNavigation();
    TestRenderingProcessesView();
    TestRenderingRemoteProcessesView();
    TestRenderingQueueView();
    TestRenderingRemoteJobsInQueueView();
    TestRenderingMonitorView();
    TestRenderingDiagnosticsWhyView();
    TestRenderingRemoteJobsInDiagnosticsView();
    TestRenderingMemoryView();
    TestRenderingHelpOverlay();
    TestRenderingOfflineAndStatusMessage();
    TestRenderingEmptyLists();
    TestColorAndNoColorRendering();
    TestNarrowTerminalHandling();
    std::printf("=== Results: %d passed, %d failed ===\n", g_passes, g_failures);
    return g_failures == 0 ? 0 : 1;
}
