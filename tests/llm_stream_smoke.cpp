// Headless smoke test for the LLM stream tracker: Claude sub-agent events.
// Feeds tools/ww/samples/claude-subagent-events.sample.jsonl line by line and checks that
//  - sub-agent lines (parent_tool_use_id set) never overwrite the parent's status or activity,
//  - a sub-agent's start and end (task_started / task_notification) make one list entry,
//  - token usage is counted once per message id, even when other lines sit between its blocks,
//  - a result line does not mark the run completed while a sub-agent is still running.

#include "../src/llm_stream.h"
#include "../src/common.h"
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <filesystem>
#include <string>
#include <vector>

using namespace pc;

#ifndef PC_REPO_DIR
#define PC_REPO_DIR "."
#endif

static int g_failures = 0;

static void Check(bool cond, const std::string& label) {
    std::printf("%s %s\n", cond ? "[PASS]" : "[FAIL]", label.c_str());
    std::fflush(stdout);  // a crash must not lose the checks that already ran
    if (!cond) g_failures++;
}

static void Feed(LlmStreamTracker& tracker, const std::string& line) {
    std::string withNewline = line + "\n";
    tracker.IngestChunk(withNewline.data(), withNewline.size());
}

static const LlmSubagentState* FindSubagent(const LlmAgentState& st, const std::string& id) {
    for (const auto& sub : st.subagents) {
        if (sub.id == id) return &sub;
    }
    return nullptr;
}

static std::string EventTypesJoined(const std::vector<LlmStreamEvent>& events) {
    std::string out;
    for (const auto& ev : events) {
        if (!out.empty()) out += ",";
        out += ev.type;
    }
    return out;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetUsageHistoryPersistence(false);

    const std::string samplePath = std::string(PC_REPO_DIR) + "/tools/ww/samples/claude-subagent-events.sample.jsonl";
    std::ifstream in(samplePath);
    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    Check(lines.size() == 16, "sample file has 16 lines (" + samplePath + ")");
    if (lines.size() != 16) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }

    LlmStreamTracker tracker;
    tracker.Init(1, "claude", "", "", "", "");

    // Part A: the first run, up to and including its completed result (lines 1-12).
    for (size_t i = 0; i < 4; ++i) Feed(tracker, lines[i]);
    LlmAgentState st = tracker.GetState();
    Check(st.currentTool == "Agent", "after task_started: parent current tool is Agent");
    Check(st.toolCalls == 1, "after task_started: parent has 1 tool call");

    Feed(tracker, lines[4]);  // sub-agent tool_use (Bash)
    st = tracker.GetState();
    Check(st.currentTool == "Agent", "sub-agent tool_use does not replace parent current tool");
    Check(st.toolCalls == 1, "sub-agent tool_use does not count as a parent tool call");
    const LlmSubagentState* sub = FindSubagent(st, "task-1");
    Check(sub != nullptr && sub->toolUses == 1, "sub-agent tool_use counts in its own tool uses");

    Feed(tracker, lines[5]);  // sub-agent tool_result
    Feed(tracker, lines[6]);  // parent tool_result of the Agent call
    Feed(tracker, lines[7]);  // parent text "Launched. Waiting for the subagent."
    Feed(tracker, lines[8]);  // sub-agent text "OK"
    st = tracker.GetState();
    Check(st.lastMessage == "Launched. Waiting for the subagent.",
          "sub-agent text does not replace parent last message");
    Check(st.status == "generating", "sub-agent text does not change parent status");
    Check(st.activity == "replying" && st.activityTarget == "Launched. Waiting for the subagent.",
          "sub-agent text does not change parent activity");

    Feed(tracker, lines[9]);  // task_notification (completed)
    st = tracker.GetState();
    sub = FindSubagent(st, "task-1");
    Check(sub != nullptr && sub->status == "completed", "task_notification marks the sub-agent completed");
    Check(sub != nullptr && sub->totalTokens == 310, "sub-agent tokens come from its notification (310)");
    Check(st.status == "generating", "task_notification does not change parent status");

    Feed(tracker, lines[10]);  // parent DONE message (same message id as line 7, usage repeated)
    Feed(tracker, lines[11]);  // result, nothing running
    st = tracker.GetState();
    sub = FindSubagent(st, "task-1");  // points into st: look it up again after each refresh
    Check(st.totalTokens == 944, "tokens counted once per message id after run A (expected 944, got " +
                                  std::to_string(st.totalTokens) + ")");
    Check(st.status == "completed" && st.activity == "done", "result with no running sub-agent completes the run");
    Check(st.subagents.size() == 1, "one sub-agent in the list after run A");
    Check(sub != nullptr && sub->description == "say ok" && sub->subagentType == "general-purpose",
          "sub-agent keeps its description and type");
    Check(sub != nullptr && sub->toolUseId == "toolu-a1" && sub->endedEpochMs > 0,
          "sub-agent links its tool_use id and records an end time");
    Check(tracker.GetEvents(0, "subagent_start").size() == 1, "one subagent_start event");
    Check(tracker.GetEvents(0, "subagent_end").size() == 1, "one subagent_end event");

    // Part B: the second run; its sub-agent never reports a notification (lines 12-15).
    for (size_t i = 12; i < 16; ++i) Feed(tracker, lines[i]);
    st = tracker.GetState();
    Check(st.totalTokens == 1176, "tokens after run B (expected 1176, got " + std::to_string(st.totalTokens) + ")");
    Check(st.subagents.size() == 2, "two sub-agents in the list after run B");
    sub = FindSubagent(st, "task-2");
    Check(sub != nullptr && sub->status == "running", "unfinished sub-agent is still running");
    Check(st.status == "running", "result while a sub-agent runs does not mark the run completed");
    Check(st.activity == "subagent" && st.activityTarget == "long task",
          "parent activity shows the running sub-agent");
    Check(st.toolCalls == 2, "parent counts only its own tool calls (2)");

    std::string json = tracker.GetEventsJson(0, "", 0);
    Check(json.find("\"subagents\"") != std::string::npos, "events JSON has a subagents array");
    Check(json.find("\"parent_tool_use_id\":\"toolu-a1\"") != std::string::npos,
          "events JSON carries parent_tool_use_id on sub-agent events");
    Check(json.find("\"task_status\":\"completed\"") != std::string::npos,
          "events JSON carries task_status on the sub-agent end event");

    std::vector<LlmStreamEvent> all = tracker.GetEvents(0, "", 0);
    Check(!all.empty() && all.back().type == "result", "last stored event is the result");
    std::printf("event types: %s\n", EventTypesJoined(all).c_str());

    // Part C: a nested sub-agent. SYNTHETIC lines, not captured from Claude Code: the real samples
    // hold no nested run. The root starts "outer" (task-n1), which starts "inner" (task-n2).
    const std::vector<std::string> nested = {
        R"({"type":"assistant","message":{"model":"claude-haiku-5-5","id":"msg-n-1","role":"assistant","content":[{"type":"tool_use","id":"toolu-n1","name":"Agent","input":{"description":"outer","subagent_type":"general-purpose"}}],"usage":{"input_tokens":1,"cache_creation_input_tokens":0,"cache_read_input_tokens":0,"output_tokens":5}},"parent_tool_use_id":null})",
        R"({"type":"system","subtype":"task_started","task_id":"task-n1","tool_use_id":"toolu-n1","description":"outer","subagent_type":"general-purpose"})",
        R"({"type":"assistant","message":{"model":"claude-haiku-5-5","id":"msg-n-2","role":"assistant","content":[{"type":"tool_use","id":"toolu-n2","name":"Agent","input":{"description":"inner","subagent_type":"general-purpose"}}],"usage":{"input_tokens":1,"cache_creation_input_tokens":0,"cache_read_input_tokens":0,"output_tokens":7}},"parent_tool_use_id":"toolu-n1","agent_id":"task-n1","subagent_type":"general-purpose"})",
        R"({"type":"system","subtype":"task_started","task_id":"task-n2","tool_use_id":"toolu-n2","description":"inner","subagent_type":"general-purpose"})",
        R"({"type":"assistant","message":{"model":"claude-haiku-5-5","id":"msg-n-3","role":"assistant","content":[{"type":"tool_use","id":"toolu-n3","name":"Bash","input":{"command":"echo nested"}}],"usage":{"input_tokens":1,"cache_creation_input_tokens":0,"cache_read_input_tokens":0,"output_tokens":3}},"parent_tool_use_id":"toolu-n2","agent_id":"task-n2","subagent_type":"general-purpose"})",
        R"({"type":"system","subtype":"task_notification","task_id":"task-n2","tool_use_id":"toolu-n2","status":"completed","summary":"inner done","usage":{"total_tokens":4,"tool_uses":1,"duration_ms":10}})",
        R"({"type":"system","subtype":"task_notification","task_id":"task-n1","tool_use_id":"toolu-n1","status":"completed","summary":"outer done","usage":{"total_tokens":12,"tool_uses":1,"duration_ms":30}})",
    };
    LlmStreamTracker nestedTracker;
    nestedTracker.Init(2, "claude", "", "", "", "");
    for (size_t i = 0; i < 6; ++i) Feed(nestedTracker, nested[i]);
    st = nestedTracker.GetState();
    Check(st.subagents.size() == 2, "nested: two sub-agents listed");
    sub = FindSubagent(st, "task-n1");
    Check(sub != nullptr && sub->parentAgentId.empty(), "nested: outer sub-agent's parent is the root (empty id)");
    sub = FindSubagent(st, "task-n2");
    Check(sub != nullptr && sub->parentAgentId == "task-n1", "nested: inner sub-agent's parent is task-n1");
    Check(sub != nullptr && sub->toolUses == 1 && sub->toolUseId == "toolu-n2",
          "nested: inner tool use counts for the inner sub-agent only");
    sub = FindSubagent(st, "task-n1");
    Check(sub != nullptr && sub->toolUses == 1, "nested: outer counts its own Agent call, not the inner Bash");
    Check(st.toolCalls == 1 && st.currentTool == "Agent", "nested: root counts only its own Agent call");
    Check(st.totalTokens == 18, "nested: tokens counted once per message (expected 18, got " +
                                std::to_string(st.totalTokens) + ")");
    Feed(nestedTracker, nested[6]);  // outer sub-agent's task_notification
    st = nestedTracker.GetState();
    sub = FindSubagent(st, "task-n1");
    Check(sub != nullptr && sub->status == "completed", "nested: outer sub-agent completes");
    Check(st.status == "calling_tool", "nested: sub-agent ends do not change the root status (no result line here)");
    std::string nestedJson = nestedTracker.GetEventsJson(0, "", 0);
    Check(nestedJson.find("\"parent_agent_id\":\"task-n1\"") != std::string::npos,
          "nested: subagents JSON carries parent_agent_id");

    // Part D: event retention. -1 is unlimited, other values are kept within 100..100000 (and
    // anything else becomes 100). The bad_alloc path is not exercised here (no fault injection).
    Check(GetLlmMaxEvents() == 2000, "retention: default is 2000");
    Check(ClampLlmMaxEvents(-1) == -1, "retention: -1 means unlimited");
    Check(ClampLlmMaxEvents(0) == 100 && ClampLlmMaxEvents(-2) == 100 && ClampLlmMaxEvents(-5000) == 100,
          "retention: 0 and other negatives clamp to 100");
    Check(ClampLlmMaxEvents(50) == 100 && ClampLlmMaxEvents(100) == 100, "retention: below 100 clamps to 100");
    Check(ClampLlmMaxEvents(2000) == 2000 && ClampLlmMaxEvents(100000) == 100000,
          "retention: values in range are kept");
    Check(ClampLlmMaxEvents(100001) == 100000 && ClampLlmMaxEvents(INT64_MAX) == 100000,
          "retention: above 100000 clamps to 100000");

    auto textLine = [](int n) {
        return std::string(R"({"type":"assistant","message":{"model":"claude-haiku-5-5","id":"msg-r","role":"assistant","content":[{"type":"text","text":"line )") +
               std::to_string(n) + R"("}]}})";
    };

    SetLlmMaxEvents(100);
    LlmStreamTracker limited;
    limited.Init(3, "claude", "", "", "", "");
    for (int i = 0; i < 150; ++i) Feed(limited, textLine(i));
    Check(limited.EventCount() == 100, "retention 100: 150 messages keep 100 events");

    SetLlmMaxEvents(-1);
    LlmStreamTracker unlimited;
    unlimited.Init(4, "claude", "", "", "", "");
    for (int i = 0; i < 600; ++i) Feed(unlimited, textLine(i));
    Check(unlimited.EventCount() == 600, "retention -1: 600 messages are all kept (no trim at the old 500)");

    SetLlmMaxEvents(2000);
    LlmStreamTracker shrinking;
    shrinking.Init(5, "claude", "", "", "", "");
    for (int i = 0; i < 300; ++i) Feed(shrinking, textLine(i));
    Check(shrinking.EventCount() == 300, "retention 2000: 300 messages are kept");
    SetLlmMaxEvents(100);
    Feed(shrinking, textLine(999));
    Check(shrinking.EventCount() == 100, "retention lowered while running: the next event trims to 100");
    SetLlmMaxEvents(2000);

    // Part E: U1 daily usage writing and U0 7-day retention/pruning.
    std::printf("--- Part E: daily usage writing and retention ---\n");
    Check(IsDailyUsagePersistence(), "retention: daily usage persistence is enabled by default");
    SetDailyUsagePersistence(false);
    Check(!IsDailyUsagePersistence(), "retention: SetDailyUsagePersistence(false) disables daily usage");
    SetDailyUsagePersistence(true);
    Check(IsDailyUsagePersistence(), "retention: SetDailyUsagePersistence(true) enables daily usage");

    wchar_t tempPath[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempPath);
    std::wstring origDataDir = GetDataDirectoryOverride();
    std::wstring testDataDir = std::wstring(tempPath) + L"hancho_smoke_usage_test_" + std::to_wstring(GetCurrentProcessId());
    std::error_code ec;
    std::filesystem::remove_all(std::filesystem::path(testDataDir), ec);
    EnsureDirectoryWide(testDataDir);
    SetDataDirectoryOverride(testDataDir);

    // 1. Record points for two different days/months and verify daily file append
    LlmUsagePoint pt1;
    pt1.agentType = "claude";
    pt1.model = "claude-opus-5-5";
    pt1.inputTokens = 1000;
    pt1.outputTokens = 200;
    pt1.thinkingTokens = 50;
    pt1.cacheReadTokens = 500;
    pt1.cacheWriteTokens = 100;
    pt1.totalTokens = 1850;
    pt1.costUsd = 0.05;

    // Fixed epoch: 2026-11-10 12:00:00 UTC = 1794312000
    int64_t epochNov10 = 1794312000;
    RecordLlmUsagePoint(epochNov10, pt1);

    // Check that usage/daily directory exists
    std::wstring dailyDir = GetDailyUsageDirWide();
    Check(std::filesystem::exists(dailyDir), "daily usage directory was created");

    // Read the 2026-11 daily file
    std::wstring ymPath = GetDailyUsagePathWide("2026-11");
    Check(std::filesystem::exists(ymPath), "2026-11.jsonl file was created");

    std::ifstream dailyIn(ymPath);
    std::string dline;
    std::vector<std::string> dlines;
    while (std::getline(dailyIn, dline)) {
        if (!dline.empty()) dlines.push_back(dline);
    }
    dailyIn.close();

    Check(dlines.size() == 1, "exactly one record in 2026-11.jsonl");
    LlmDailyUsageRecord rec1;
    bool parsed1 = !dlines.empty() && ParseDailyUsageRecordJson(nlohmann::json::parse(dlines[0], nullptr, false), rec1);
    Check(parsed1, "parsed daily usage record successfully");
    Check(rec1.schema == "hancho.usage.day/1", "schema is hancho.usage.day/1");
    Check(rec1.engine == "claude", "engine is claude");
    Check(rec1.model == "claude-opus-5-5", "model is claude-opus-5-5");
    Check(rec1.inputTokens == 1000 && rec1.outputTokens == 200, "token counts match delta");
    Check(rec1.totalTokens == 1850, "total tokens match delta");
    Check(rec1.runs == 1, "runs is 1");
    Check(rec1.firstEpochSec == epochNov10 && rec1.lastEpochSec == epochNov10, "epochs match recorded epoch");

    // 2. Append another point to the same month and check append behavior
    LlmUsagePoint pt2;
    pt2.agentType = "claude";
    pt2.model = "claude-opus-5-5";
    pt2.inputTokens = 500;
    pt2.totalTokens = 500;
    RecordLlmUsagePoint(epochNov10 + 3600, pt2);

    dlines.clear();
    dailyIn.open(ymPath);
    while (std::getline(dailyIn, dline)) {
        if (!dline.empty()) dlines.push_back(dline);
    }
    dailyIn.close();
    Check(dlines.size() == 2, "second point appended to 2026-11.jsonl (now 2 records)");

    // 3. Replay resilience: corrupt/incomplete lines are safely skipped
    {
        AppendToLogFileWide(ymPath, "{\"corrupt\":true, broken json\n");
        AppendToLogFileWide(ymPath, "not even json\n");
    }
    dlines.clear();
    dailyIn.open(ymPath);
    int validRecords = 0;
    uint64_t replayedTokens = 0;
    while (std::getline(dailyIn, dline)) {
        if (dline.empty()) continue;
        auto j = nlohmann::json::parse(dline, nullptr, false);
        LlmDailyUsageRecord r;
        if (ParseDailyUsageRecordJson(j, r)) {
            validRecords++;
            replayedTokens += r.totalTokens;
        }
    }
    dailyIn.close();
    Check(validRecords == 2, "corrupt lines skipped; exactly 2 valid records recovered");
    Check(replayedTokens == 2350, "replayed total tokens match valid sum (1850 + 500 = 2350)");

    // 4. Test U0 retention and 7-day pruning of logs/usage_history.jsonl
    SetDailyUsagePersistence(false);
    SetUsageHistoryPersistence(true);
    int64_t nowSec = static_cast<int64_t>(time(nullptr));
    LlmUsagePoint ptOld;
    ptOld.agentType = "claude";
    ptOld.totalTokens = 100;
    int64_t tenDaysAgo = nowSec - 10 * 86400;
    RecordLlmUsagePoint(tenDaysAgo, ptOld);

    LlmUsagePoint ptRecent;
    ptRecent.agentType = "claude";
    ptRecent.totalTokens = 200;
    int64_t oneDayAgo = nowSec - 1 * 86400;
    RecordLlmUsagePoint(oneDayAgo, ptRecent);

    // Call prune with 7-day retention relative to nowSec
    PruneUsageHistory(7 * 86400, nowSec);
    SetDailyUsagePersistence(true);

    std::wstring histPath = PathJoinWide(GetLogsDirectoryWide(), L"usage_history.jsonl");
    Check(std::filesystem::exists(histPath), "usage_history.jsonl exists");

    std::ifstream histIn(histPath);
    std::vector<int64_t> keptEpochs;
    std::string hline;
    while (std::getline(histIn, hline)) {
        if (hline.empty()) continue;
        auto j = nlohmann::json::parse(hline, nullptr, false);
        if (j.is_object()) {
            keptEpochs.push_back(j.value("epochSec", (int64_t)0));
        }
    }
    histIn.close();

    bool hasOld = false;
    bool hasRecent = false;
    for (int64_t ep : keptEpochs) {
        if (ep == tenDaysAgo) hasOld = true;
        if (ep == oneDayAgo) hasRecent = true;
    }
    Check(!hasOld, "point older than 7 days was pruned from usage_history.jsonl");
    Check(hasRecent, "point within 7 days was kept in usage_history.jsonl");

    // Part F: U1 daily usage reading, aggregation, grouping, and CLI/MCP queries (card 051-2)
    std::printf("--- Part F: daily usage reading, aggregation, grouping (051-2) ---\n");
    // 1. ReadDailyUsageSummary collapses the 2 existing points for (2026-11-10, claude, claude-opus-5-5)
    auto summaryNov10 = ReadDailyUsageSummary();
    Check(summaryNov10.size() == 1, "read daily usage: exactly 1 aggregated record for Nov 10 claude");
    if (!summaryNov10.empty()) {
        const auto& r = summaryNov10[0];
        Check(r.schema == "hancho.usage.day/1", "read daily usage: schema is hancho.usage.day/1");
        Check(r.date == "2026-11-10", "read daily usage: date is 2026-11-10");
        Check(r.engine == "claude", "read daily usage: engine is claude");
        Check(r.model == "claude-opus-5-5", "read daily usage: model is claude-opus-5-5");
        Check(r.runs == 2, "read daily usage: runs summed to 2");
        Check(r.totalTokens == 2350, "read daily usage: totalTokens summed to 2350 (1850 + 500)");
        Check(r.inputTokens == 1500, "read daily usage: inputTokens summed to 1500 (1000 + 500)");
        Check(r.outputTokens == 200, "read daily usage: outputTokens is 200");
        Check(r.costUsd > 0.049 && r.costUsd < 0.051, "read daily usage: costUsd is 0.05");
        Check(r.firstEpochSec == epochNov10, "read daily usage: firstEpochSec preserved");
        Check(r.lastEpochSec == epochNov10 + 3600, "read daily usage: lastEpochSec updated to second point");
    }

    // 2. Add second engine on the same day (Nov 10)
    LlmUsagePoint ptAgy;
    ptAgy.agentType = "agy";
    ptAgy.model = "gemini-1.5-pro";
    ptAgy.inputTokens = 2500;
    ptAgy.outputTokens = 500;
    ptAgy.totalTokens = 3000;
    ptAgy.costUsd = 0.0;
    RecordLlmUsagePoint(epochNov10 + 7200, ptAgy);

    auto summary2Engines = ReadDailyUsageSummary();
    Check(summary2Engines.size() == 2, "read daily usage: 2 records for distinct (engine, model) keys on same day");

    // 3. Add point on next day: Nov 11 (epochNov10 + 86400)
    int64_t epochNov11 = epochNov10 + 86400;
    LlmUsagePoint ptNov11;
    ptNov11.agentType = "claude";
    ptNov11.model = "claude-opus-5-5";
    ptNov11.inputTokens = 800;
    ptNov11.outputTokens = 200;
    ptNov11.totalTokens = 1000;
    ptNov11.costUsd = 0.02;
    RecordLlmUsagePoint(epochNov11, ptNov11);

    auto summaryAll = ReadDailyUsageSummary();
    Check(summaryAll.size() == 3, "read daily usage: 3 records across 2 days and 2 engines");

    // 4. Test date range filtering
    auto filterNov10Only = ReadDailyUsageSummary("2026-11-10", "2026-11-10");
    Check(filterNov10Only.size() == 2, "date filter: exactly 2 records on 2026-11-10");

    auto filterNov11Only = ReadDailyUsageSummary("2026-11-11", "2026-11-11");
    Check(filterNov11Only.size() == 1, "date filter: exactly 1 record on 2026-11-11");
    if (!filterNov11Only.empty()) {
        Check(filterNov11Only[0].date == "2026-11-11", "date filter: Nov 11 record matches date");
        Check(filterNov11Only[0].totalTokens == 1000, "date filter: Nov 11 token count matches");
    }

    auto filterMonth = ReadDailyUsageSummary("2026-11", "2026-11");
    Check(filterMonth.size() == 3, "date filter: month 2026-11 returns all 3 records");

    // 5. Test QueryUsageStats default (no groupBy)
    LlmUsageStatsOptions optsDefault;
    auto qDefault = QueryUsageStats(optsDefault);
    Check(qDefault.ok, "QueryUsageStats: default query ok");
    Check(qDefault.records.size() == 3, "QueryUsageStats: returns 3 records");
    Check(qDefault.totalTokens == 6350, "QueryUsageStats: grand total tokens is 6350 (2350 + 3000 + 1000)");
    Check(qDefault.totalRuns == 4, "QueryUsageStats: total runs is 4 (2 + 1 + 1)");
    Check(qDefault.totalCostUsd > 0.069 && qDefault.totalCostUsd < 0.071, "QueryUsageStats: total cost is 0.07");

    // 6. Test QueryUsageStats group-by day
    LlmUsageStatsOptions optsDay;
    optsDay.groupBy = "day";
    auto qDay = QueryUsageStats(optsDay);
    Check(qDay.ok, "QueryUsageStats: group-by day ok");
    Check(qDay.groups.size() == 2, "QueryUsageStats: exactly 2 day groups (Nov 10 and Nov 11)");
    if (qDay.groups.size() == 2) {
        Check(qDay.groups[0].key == "2026-11-10", "QueryUsageStats: first day group is 2026-11-10");
        Check(qDay.groups[0].totalTokens == 5350, "QueryUsageStats: Nov 10 tokens = 5350 (2350 + 3000)");
        Check(qDay.groups[0].runs == 3, "QueryUsageStats: Nov 10 runs = 3 (2 + 1)");
        Check(qDay.groups[1].key == "2026-11-11", "QueryUsageStats: second day group is 2026-11-11");
        Check(qDay.groups[1].totalTokens == 1000, "QueryUsageStats: Nov 11 tokens = 1000");
    }

    // 7. Test QueryUsageStats group-by engine
    LlmUsageStatsOptions optsEngine;
    optsEngine.groupBy = "engine";
    auto qEngine = QueryUsageStats(optsEngine);
    Check(qEngine.ok, "QueryUsageStats: group-by engine ok");
    Check(qEngine.groups.size() == 2, "QueryUsageStats: exactly 2 engine groups (agy and claude)");
    if (qEngine.groups.size() == 2) {
        Check(qEngine.groups[0].key == "agy", "QueryUsageStats: engine group 0 is agy");
        Check(qEngine.groups[0].totalTokens == 3000, "QueryUsageStats: agy totalTokens = 3000");
        Check(qEngine.groups[1].key == "claude", "QueryUsageStats: engine group 1 is claude");
        Check(qEngine.groups[1].totalTokens == 3350, "QueryUsageStats: claude totalTokens = 3350 (2350 + 1000)");
    }

    // 8. Test QueryUsageStats group-by model
    LlmUsageStatsOptions optsModel;
    optsModel.groupBy = "model";
    auto qModel = QueryUsageStats(optsModel);
    Check(qModel.ok, "QueryUsageStats: group-by model ok");
    Check(qModel.groups.size() == 2, "QueryUsageStats: 2 model groups");

    // 9. Test QueryUsageStats group-by month
    LlmUsageStatsOptions optsMonth;
    optsMonth.groupBy = "month";
    auto qMonth = QueryUsageStats(optsMonth);
    Check(qMonth.ok, "QueryUsageStats: group-by month ok");
    Check(qMonth.groups.size() == 1, "QueryUsageStats: 1 month group (2026-11)");
    if (qMonth.groups.size() == 1) {
        Check(qMonth.groups[0].key == "2026-11", "QueryUsageStats: month key is 2026-11");
        Check(qMonth.groups[0].totalTokens == 6350, "QueryUsageStats: month totalTokens = 6350");
    }

    // 10. Test invalid group-by
    LlmUsageStatsOptions optsInvalid;
    optsInvalid.groupBy = "unsupported_dimension";
    auto qInvalid = QueryUsageStats(optsInvalid);
    Check(!qInvalid.ok, "QueryUsageStats: invalid groupBy returns ok == false");
    Check(!qInvalid.error.empty(), "QueryUsageStats: invalid groupBy returns error message");

    // 11. Test UsageStatsResultToJson
    nlohmann::json jsonRes = UsageStatsResultToJson(qDay);
    Check(jsonRes["ok"] == true, "UsageStatsResultToJson: ok is true");
    Check(jsonRes.contains("totals"), "UsageStatsResultToJson: contains totals");
    Check(jsonRes["totals"]["totalTokens"] == 6350, "UsageStatsResultToJson: totals.totalTokens is 6350");
    Check(jsonRes.contains("groups"), "UsageStatsResultToJson: contains groups");
    Check(jsonRes["groups"].is_array() && jsonRes["groups"].size() == 2, "UsageStatsResultToJson: groups array has 2 items");

    // 12. Test FormatUsageStatsTable
    std::string tableOutput = FormatUsageStatsTable(qDay);
    Check(tableOutput.find("LLM Usage Statistics") != std::string::npos, "FormatUsageStatsTable: contains header");
    Check(tableOutput.find("2026-11-10") != std::string::npos, "FormatUsageStatsTable: contains date 2026-11-10");
    Check(tableOutput.find("5,350") != std::string::npos, "FormatUsageStatsTable: contains formatted token count 5,350");
    Check(tableOutput.find("Total") != std::string::npos, "FormatUsageStatsTable: contains Total row");

    // Part G: End-to-end smoke verification covering 051a〜051c pipeline (card 051-3)
    std::printf("--- Part G: end-to-end 051a~051c smoke verification (051-3) ---\n");
    // 1. Verify 当日の内訳 (breakdown for a single date: 2026-11-10 has 2 engines, totals match)
    LlmUsageStatsOptions optsNov10;
    optsNov10.since = "2026-11-10";
    optsNov10.until = "2026-11-10";
    auto qNov10 = QueryUsageStats(optsNov10);
    Check(qNov10.ok, "e2e smoke: query date 2026-11-10 ok");
    Check(qNov10.records.size() == 2, "e2e smoke: Nov 10 breakdown has 2 distinct (engine, model) records");
    Check(qNov10.totalTokens == 5350, "e2e smoke: Nov 10 total tokens is 5350 (2350 + 3000)");
    Check(qNov10.totalRuns == 3, "e2e smoke: Nov 10 runs is 3 (2 claude + 1 agy)");
    Check(qNov10.totalCostUsd > 0.049 && qNov10.totalCostUsd < 0.051, "e2e smoke: Nov 10 cost is 0.05");

    // 2. Verify 月内の合計 (month total: Nov 2026 has all 3 aggregated records, totals match)
    LlmUsageStatsOptions optsNovMonth;
    optsNovMonth.since = "2026-11";
    optsNovMonth.until = "2026-11";
    auto qNovMonth = QueryUsageStats(optsNovMonth);
    Check(qNovMonth.ok, "e2e smoke: query month 2026-11 ok");
    Check(qNovMonth.records.size() == 3, "e2e smoke: Nov 2026 month has 3 daily records");
    Check(qNovMonth.totalTokens == 6350, "e2e smoke: Nov 2026 total tokens is 6350 (5350 + 1000)");
    Check(qNovMonth.totalRuns == 4, "e2e smoke: Nov 2026 total runs is 4");
    Check(qNovMonth.totalCostUsd > 0.069 && qNovMonth.totalCostUsd < 0.071, "e2e smoke: Nov 2026 total cost is 0.07");

    // 3. Verify JSON serialization matches U1 specification (doc/design-v3.md 3.6.1)
    nlohmann::json jNov10 = UsageStatsResultToJson(qNov10);
    Check(jNov10.value("ok", false), "e2e smoke: JSON ok is true");
    Check(jNov10.contains("records") && jNov10["records"].size() == 2, "e2e smoke: JSON records array has 2 items");
    Check(jNov10["records"][0].value("schema", "") == "hancho.usage.day/1", "e2e smoke: schema is hancho.usage.day/1");
    Check(jNov10.contains("totals") && jNov10["totals"].value("totalTokens", 0) == 5350, "e2e smoke: JSON totals match");

    // Part H: Card 052 - usage export, compact, costKnown: false, and 12m aggregation
    std::printf("--- Part H: card 052 export, compact, costKnown, and 12m smoke ---\n");

    // 1. Verify costKnown: false on agy engine (does not report cost)
    Check(qNov10.records.size() >= 2, "Part H: qNov10 has at least 2 records");
    bool foundAgy = false;
    for (const auto& rec : qNov10.records) {
        if (rec.engine == "agy") {
            foundAgy = true;
            Check(!rec.costKnown, "Part H: agy record has costKnown: false");
        }
    }
    Check(foundAgy, "Part H: found agy record in Nov 10 breakdown");
    Check(!qNov10.costKnown, "Part H: total result has costKnown: false because agy was included");

    // 2. Test FormatUsageStatsCsv (raw records format)
    std::string csvOutput = FormatUsageStatsCsv(qNov10);
    Check(csvOutput.find("date,tzOffsetMin,engine,model,inputTokens,outputTokens") != std::string::npos,
          "FormatUsageStatsCsv: contains expected CSV header for raw records");
    Check(csvOutput.find("2026-11-10") != std::string::npos, "FormatUsageStatsCsv: contains date 2026-11-10");
    Check(csvOutput.find("claude") != std::string::npos, "FormatUsageStatsCsv: contains claude");
    Check(csvOutput.find("agy") != std::string::npos, "FormatUsageStatsCsv: contains agy");
    Check(csvOutput.find(",false,") != std::string::npos, "FormatUsageStatsCsv: contains costKnown false for agy");

    // 3. Test FormatUsageStatsCsv (groupBy day / month)
    std::string csvGrouped = FormatUsageStatsCsv(qDay);
    Check(csvGrouped.find("date,inputTokens,outputTokens") != std::string::npos,
          "FormatUsageStatsCsv: contains grouped date header");
    Check(csvGrouped.find("2026-11-10,") != std::string::npos, "FormatUsageStatsCsv: contains grouped date row");

    std::string csvMonth = FormatUsageStatsCsv(qMonth);
    Check(csvMonth.find("month,inputTokens,outputTokens") != std::string::npos,
          "FormatUsageStatsCsv: contains grouped month header");
    Check(csvMonth.find("2026-11,") != std::string::npos, "FormatUsageStatsCsv: contains grouped month row 2026-11");

    // 4. Test CompactDailyUsage
    // Before compact: Nov 2026 has multiple deltas (we recorded 4 points: 2 for claude on Nov 10, 1 for agy on Nov 10, 1 on Nov 11)
    // So there were 4 delta lines.
    LlmUsageCompactResult compactNov = CompactDailyUsage("2026-11");
    Check(compactNov.ok, "CompactDailyUsage: ok == true for 2026-11");
    Check(compactNov.filesProcessed == 1, "CompactDailyUsage: 1 file processed");
    Check(compactNov.filesCompacted == 1, "CompactDailyUsage: 1 file compacted");
    Check(compactNov.linesBefore > compactNov.linesAfter, "CompactDailyUsage: linesBefore > linesAfter");
    Check(compactNov.linesAfter == 3, "CompactDailyUsage: linesAfter is 3 (2 on Nov 10, 1 on Nov 11)");

    // Re-query after compact: totals and records must match identically!
    auto qNovMonthAfter = QueryUsageStats(optsNovMonth);
    Check(qNovMonthAfter.ok, "QueryUsageStats after compact: ok == true");
    Check(qNovMonthAfter.records.size() == 3, "QueryUsageStats after compact: records size is 3");
    Check(qNovMonthAfter.totalTokens == 6350, "QueryUsageStats after compact: totalTokens unchanged (6350)");
    Check(qNovMonthAfter.totalRuns == 4, "QueryUsageStats after compact: totalRuns preserved (4)");
    Check(qNovMonthAfter.totalCostUsd > 0.069 && qNovMonthAfter.totalCostUsd < 0.071,
          "QueryUsageStats after compact: totalCostUsd preserved");

    // Second compact on already compacted file: no-op, 0 files modified
    LlmUsageCompactResult compactNov2 = CompactDailyUsage("2026-11");
    Check(compactNov2.ok, "CompactDailyUsage re-run: ok == true");
    Check(compactNov2.filesCompacted == 0, "CompactDailyUsage re-run: 0 files compacted");

    // 5. Test 12-month query aggregation (groupBy: month over multi-month range)
    LlmUsageStatsOptions opts12m;
    opts12m.since = "12m";
    opts12m.groupBy = "month";
    auto q12m = QueryUsageStats(opts12m);
    Check(q12m.ok, "QueryUsageStats: 12m query ok == true");
    Check(q12m.groups.size() >= 1, "QueryUsageStats: 12m query returned at least 1 month group");
    Check(q12m.totalTokens == 6350, "QueryUsageStats: 12m query totalTokens matches");

    // Clean up temporary test data directory
    SetUsageHistoryPersistence(false);
    SetDataDirectoryOverride(origDataDir);
    std::filesystem::remove_all(std::filesystem::path(testDataDir), ec);

    if (g_failures == 0) std::printf("ALL PASS\n");
    else std::printf("%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
