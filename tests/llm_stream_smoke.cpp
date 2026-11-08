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

    if (g_failures == 0) std::printf("ALL PASS\n");
    else std::printf("%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
