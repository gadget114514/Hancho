// Headless smoke test for the hancho.event/1 normalizer and the .events.jsonl writer.
// Representative internal LlmStreamEvent values for claude / opencode / codex / agy are normalized
// and checked for their kind and required data fields; a tracker run checks the derived file path,
// the appended lines and their raw.line numbers.

#include "../src/event_schema.h"
#include "../src/llm_stream.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

using namespace pc;

static int g_failures = 0;

static void Check(bool cond, const std::string& label) {
    std::printf("%s %s\n", cond ? "[PASS]" : "[FAIL]", label.c_str());
    std::fflush(stdout);
    if (!cond) g_failures++;
}

static LlmStreamEvent MakeEvent(const std::string& type, const std::string& agent) {
    LlmStreamEvent ev;
    ev.type = type;
    ev.agentType = agent;
    ev.seq = 1;
    ev.recvEpochMs = 1794300000123;
    return ev;
}

static std::string KindOf(const LlmStreamEvent& ev) {
    HanchoEventSource source;
    source.processId = 12;
    nlohmann::json j = BuildHanchoEventV1(ev, source, "", "proc_12_build_stream.jsonl", 7);
    return j["kind"].get<std::string>();
}

static nlohmann::json DataOf(const LlmStreamEvent& ev, const std::string& parentAgentId = "") {
    HanchoEventSource source;
    source.processId = 12;
    return BuildHanchoEventV1(ev, source, parentAgentId, "proc_12_build_stream.jsonl", 7)["data"];
}

static bool HasAnyToken(const nlohmann::json& data) {
    for (const char* key : {"inputTokens", "outputTokens", "thinkingTokens", "cacheReadTokens",
                            "cacheWriteTokens", "totalTokens"}) {
        if (data.contains(key)) return true;
    }
    return false;
}

static void CheckTopLevel(const std::string& label, const std::string& agent, const std::string& kind) {
    LlmStreamEvent ev = MakeEvent("message", agent);
    ev.model = "model-x";
    ev.sessionId = "sess-1";
    ev.agentId = "agent-1";
    ev.content = "hello";
    HanchoEventSource source;
    source.processId = 42;
    source.jobId = "j_7f3a";
    source.attempt = 2;
    source.btId = "bt_main";
    source.node = "build";
    nlohmann::json j = BuildHanchoEventV1(ev, source, "parent-1", "C:/logs/proc_42_build_stream.jsonl", 931);
    Check(j["schema"] == "hancho.event/1", label + ": schema is hancho.event/1");
    Check(j["seq"] == 1, label + ": seq is carried");
    std::string ts = j["ts"].get<std::string>();
    Check(ts.size() > 20 && ts.back() == 'Z' && ts[10] == 'T', label + ": ts is ISO 8601 UTC");
    Check(j["source"]["processId"] == 42 && j["source"]["jobId"] == "j_7f3a" &&
              j["source"]["attempt"] == 2 && j["source"]["btId"] == "bt_main" &&
              j["source"]["node"] == "build",
          label + ": source carries process/job/attempt/bt/node");
    Check(j["agent"]["type"] == agent && j["agent"]["model"] == "model-x" &&
              j["agent"]["sessionId"] == "sess-1" && j["agent"]["agentId"] == "agent-1" &&
              j["agent"]["parentAgentId"] == "parent-1",
          label + ": agent carries type/model/session/agent/parent");
    Check(j["kind"] == kind, label + ": kind is " + kind);
    Check(j["raw"]["file"] == "proc_42_build_stream.jsonl" && j["raw"]["line"] == 931,
          label + ": raw carries the stream file name and line");
}

int main() {
    SetConsoleOutputCP(CP_UTF8);

    Check(HanchoEventsFilePathForStream("logs/proc_12_build_stream.jsonl") ==
              "logs/proc_12_build.events.jsonl",
          "derived path: _stream.jsonl becomes .events.jsonl");
    Check(HanchoEventsFilePathForStream("logs/nothing.jsonl").empty(),
          "derived path: a path without _stream.jsonl yields no event file");

    CheckTopLevel("claude", "claude", "message");

    // Claude: tool call, tool result, thinking, message, plan, init, rate limit, sub-agents, result.
    LlmStreamEvent call = MakeEvent("tool_call", "claude");
    call.toolName = "Edit";
    call.toolUseId = "toolu_01";
    call.toolInput = R"({"file_path":"src/a.cpp","old_string":"a","new_string":"b"})";
    nlohmann::json callData = DataOf(call);
    Check(KindOf(call) == "tool.call", "claude tool_call -> tool.call");
    Check(callData["tool"] == "Edit" && callData["activity"] == "edit" &&
              callData["target"] == "src/a.cpp" && callData["toolUseId"] == "toolu_01" &&
              callData["input"]["file_path"] == "src/a.cpp",
          "claude tool_call: tool, activity, target, toolUseId and input");
    Check(callData.contains("tool") && callData.contains("activity"),
          "claude tool_call: required data fields are present");

    LlmStreamEvent result = MakeEvent("tool_result", "claude");
    result.toolUseId = "toolu_01";
    result.toolError = true;
    result.content = "permission denied";
    nlohmann::json resData = DataOf(result);
    Check(KindOf(result) == "tool.result", "claude tool_result -> tool.result");
    Check(resData["ok"] == false && resData["error"] == "permission denied" &&
              resData["toolUseId"] == "toolu_01",
          "claude tool_result: ok=false, error and toolUseId");
    Check(resData.contains("ok"), "claude tool_result: required data.ok is present");

    LlmStreamEvent thought = MakeEvent("thinking", "claude");
    thought.content = "thinking about it";
    Check(KindOf(thought) == "thought" && DataOf(thought)["text"] == "thinking about it",
          "claude thinking -> thought with text");

    LlmStreamEvent reply = MakeEvent("message", "claude");
    reply.content = "done";
    Check(KindOf(reply) == "message" && DataOf(reply)["text"] == "done",
          "claude message -> message with text");

    LlmStreamEvent plan = MakeEvent("tool_call", "claude");
    plan.toolName = "TodoWrite";
    plan.toolInput = R"({"todos":[{"content":"build","status":"completed"},{"content":"test","status":"in_progress"}]})";
    nlohmann::json planData = DataOf(plan);
    Check(KindOf(plan) == "plan", "claude TodoWrite -> plan");
    Check(planData["items"].size() == 2 && planData["items"][0]["content"] == "build" &&
              planData["items"][0]["status"] == "completed" &&
              planData["items"][1]["status"] == "in_progress",
          "claude plan: items carry content and status");

    LlmStreamEvent init = MakeEvent("status", "claude");
    init.content = "init";
    init.cwd = "D:/work";
    init.toolsOffered = 14;
    nlohmann::json initData = DataOf(init);
    Check(KindOf(init) == "run.started" && initData["cwd"] == "D:/work" &&
              initData["toolsOffered"] == 14,
          "claude init status -> run.started with cwd and toolsOffered");

    LlmStreamEvent limit = MakeEvent("rate_limit", "claude");
    limit.rateLimitType = "five_hour";
    limit.rateLimitStatus = "rejected";
    limit.rateLimitUtilization = 0.91;
    limit.rateLimitResetsAtSec = 1794303600;
    nlohmann::json limitData = DataOf(limit);
    Check(KindOf(limit) == "rate_limit" && limitData["status"] == "rejected" &&
              limitData["window"] == "five_hour" && limitData["utilization"] == 0.91 &&
              limitData["resetsAt"] == 1794303600,
          "claude rate_limit -> rate_limit with status, window, utilization, resetsAt");

    LlmStreamEvent subStart = MakeEvent("subagent_start", "claude");
    subStart.agentId = "task-1";
    subStart.toolUseId = "toolu-a1";
    subStart.subagentType = "general-purpose";
    subStart.taskDescription = "say ok";
    nlohmann::json subStartData = DataOf(subStart, "");
    Check(KindOf(subStart) == "subagent.started" && subStartData["agentId"] == "task-1" &&
              subStartData["type"] == "general-purpose" && subStartData["toolUseId"] == "toolu-a1",
          "claude task_started -> subagent.started with agentId/type/toolUseId");

    LlmStreamEvent subEnd = MakeEvent("subagent_end", "claude");
    subEnd.agentId = "task-1";
    subEnd.taskStatus = "completed";
    subEnd.taskTokens = 310;
    subEnd.taskToolUses = 2;
    subEnd.taskDurationMs = 900;
    nlohmann::json subEndData = DataOf(subEnd, "");
    Check(KindOf(subEnd) == "subagent.ended" && subEndData["agentId"] == "task-1" &&
              subEndData["status"] == "completed" && subEndData["tokens"] == 310 &&
              subEndData["toolUses"] == 2 && subEndData["durationMs"] == 900,
          "claude task_notification -> subagent.ended with status/tokens/toolUses/durationMs");

    LlmStreamEvent runEnd = MakeEvent("result", "claude");
    runEnd.status = "completed";
    runEnd.stopReason = "end_turn";
    runEnd.numTurns = 3;
    runEnd.apiDurationMs = 1234;
    runEnd.permissionDenials = 2;
    nlohmann::json runEndData = DataOf(runEnd);
    Check(KindOf(runEnd) == "run.ended" && runEndData["status"] == "completed" &&
              runEndData["stopReason"] == "end_turn" && runEndData["numTurns"] == 3 &&
              runEndData["durationMs"] == 1234 && runEndData["permissionDenials"] == 2,
          "claude result -> run.ended with status/stopReason/numTurns/duration/permissions");

    // OpenCode: step start, step finish with usage, tool call and tool result.
    LlmStreamEvent step = MakeEvent("step", "opencode");
    step.content = "step_start";
    Check(KindOf(step) == "turn.started" && DataOf(step).is_object(),
          "opencode step_start -> turn.started");

    LlmStreamEvent usage = MakeEvent("step", "opencode");
    usage.content = "step_finish";
    usage.inputTokens = 100;
    usage.outputTokens = 20;
    usage.cacheReadTokens = 30;
    usage.totalTokens = 150;
    usage.costUsd = 0.0123;
    nlohmann::json usageData = DataOf(usage);
    Check(KindOf(usage) == "usage", "opencode step_finish with tokens -> usage");
    Check(HasAnyToken(usageData) && usageData["inputTokens"] == 100 &&
              usageData["outputTokens"] == 20 && usageData["cacheReadTokens"] == 30 &&
              usageData["totalTokens"] == 150 && usageData["costUsd"] == 0.0123,
          "opencode usage: token counts and costUsd");

    LlmStreamEvent costOnly = MakeEvent("usage", "opencode");
    costOnly.costUsd = 0.5;
    Check(KindOf(costOnly) == "raw" && DataOf(costOnly).contains("text"),
          "usage without a token count falls back to raw");

    LlmStreamEvent ocCall = MakeEvent("tool_call", "opencode");
    ocCall.toolName = "bash";
    ocCall.toolInput = R"({"command":"npm test"})";
    Check(KindOf(ocCall) == "tool.call" && DataOf(ocCall)["activity"] == "command" &&
              DataOf(ocCall)["target"] == "npm test",
          "opencode tool_call -> tool.call with command activity and target");

    LlmStreamEvent ocResult = MakeEvent("tool_result", "opencode");
    ocResult.toolError = false;
    ocResult.content = "all pass";
    Check(KindOf(ocResult) == "tool.result" && DataOf(ocResult)["ok"] == true &&
              DataOf(ocResult)["summary"] == "all pass",
          "opencode tool_result -> tool.result with ok and summary");

    // Codex: thread start, turn start, error, turn completed.
    LlmStreamEvent thread = MakeEvent("status", "codex");
    thread.content = "init";
    thread.sessionId = "thread-9";
    Check(KindOf(thread) == "run.started", "codex thread.started -> run.started");

    LlmStreamEvent turn = MakeEvent("step", "codex");
    turn.content = "turn.started";
    Check(KindOf(turn) == "turn.started", "codex turn.started -> turn.started");

    LlmStreamEvent err = MakeEvent("error", "codex");
    err.content = "usage limit reached";
    Check(KindOf(err) == "error" && DataOf(err)["message"] == "usage limit reached",
          "codex error -> error with message");

    LlmStreamEvent turnEnd = MakeEvent("result", "codex");
    turnEnd.status = "completed";
    Check(KindOf(turnEnd) == "run.ended" && DataOf(turnEnd)["status"] == "completed",
          "codex turn.completed -> run.ended");

    // agy: init and a finished tool step (recognised by the generic parser).
    LlmStreamEvent agyInit = MakeEvent("status", "agy");
    agyInit.content = "init";
    Check(KindOf(agyInit) == "run.started", "agy init -> run.started");

    LlmStreamEvent agyTool = MakeEvent("tool_result", "agy");
    agyTool.toolName = "run_command";
    agyTool.content = "ok";
    Check(KindOf(agyTool) == "tool.result" && DataOf(agyTool)["ok"] == true,
          "agy finished tool -> tool.result");

    // agy response text and user_input text.
    LlmStreamEvent agyMessage = MakeEvent("message", "agy");
    agyMessage.content = "answer";
    Check(KindOf(agyMessage) == "message", "agy agent_response -> message");

    LlmStreamEvent userInput = MakeEvent("user_input", "claude");
    userInput.content = "continue";
    Check(KindOf(userInput) == "raw" && DataOf(userInput)["text"] == "continue",
          "user_input has no v1 kind and falls back to raw with text");

    LlmStreamEvent unknown = MakeEvent("mystery_event", "generic");
    unknown.content = "payload";
    Check(KindOf(unknown) == "raw" && DataOf(unknown)["text"] == "payload",
          "unknown type -> raw with text");

    // Tracker integration: the normalized events are appended to the derived .events.jsonl file.
    std::filesystem::path dir = std::filesystem::temp_directory_path() /
                                ("hancho_event_smoke_" + std::to_string(GetCurrentProcessId()));
    std::filesystem::create_directories(dir);
    std::string streamPath = (dir / "proc_7_test_stream.jsonl").string();
    std::string eventsPath = (dir / "proc_7_test.events.jsonl").string();
    std::error_code ec;
    std::filesystem::remove(streamPath, ec);
    std::filesystem::remove(eventsPath, ec);

    LlmStreamTracker tracker;
    tracker.Init(7, "claude", streamPath, "", "", "", "j_42", 2);
    const std::vector<std::string> lines = {
        R"({"type":"assistant","message":{"id":"msg-1","model":"claude-test","role":"assistant","content":[{"type":"tool_use","id":"toolu-a1","name":"Agent","input":{"description":"sub","subagent_type":"general-purpose"}}]}})",
        R"({"type":"system","subtype":"task_started","task_id":"task-1","tool_use_id":"toolu-a1","description":"sub","subagent_type":"general-purpose"})",
        R"({"type":"assistant","message":{"id":"msg-2","model":"claude-test","role":"assistant","content":[{"type":"tool_use","id":"toolu-b1","name":"Bash","input":{"command":"echo hi"}}]},"parent_tool_use_id":"toolu-a1","agent_id":"task-1"})",
    };
    for (const auto& line : lines) {
        std::string withNewline = line + "\n";
        tracker.IngestChunk(withNewline.data(), withNewline.size());
    }

    std::vector<nlohmann::json> written;
    std::ifstream in(eventsPath);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line == "\r") continue;
        auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (parsed.is_object()) written.push_back(parsed);
    }
    Check(std::filesystem::exists(eventsPath), "tracker writes the derived .events.jsonl file");
    Check(written.size() == 3, "tracker appends one normalized event per non-transient event");
    if (written.size() == 3) {
        Check(written[0]["kind"] == "tool.call" && written[0]["raw"]["line"] == 1 &&
                  written[0]["agent"]["agentId"] == "" &&
                  written[0]["source"]["processId"] == 7 &&
                  written[0]["source"]["jobId"] == "j_42" && written[0]["source"]["attempt"] == 2,
              "tracker: first event is the root tool.call on raw line 1 with process/job/attempt");
        Check(written[1]["kind"] == "subagent.started" && written[1]["raw"]["line"] == 2 &&
                  written[1]["data"]["agentId"] == "task-1" &&
                  written[1]["agent"]["parentAgentId"] == "",
              "tracker: subagent.started carries agentId and the root as parent");
        Check(written[2]["kind"] == "tool.call" && written[2]["raw"]["line"] == 3 &&
                  written[2]["agent"]["agentId"] == "task-1" &&
                  written[2]["agent"]["parentAgentId"] == "",
              "tracker: sub-agent tool.call carries its agent and parent agent ids");
    }
    Check(std::filesystem::exists(streamPath), "tracker still writes the .stream.jsonl file");

    auto allRead = ReadHanchoEventsFile(eventsPath, 0, "", 0);
    Check(allRead.size() == 3, "reader returns every normalized event");
    auto lastOne = ReadHanchoEventsFile(eventsPath, 1, "", 0);
    Check(lastOne.size() == 1 && lastOne[0]["kind"] == "tool.call" &&
              lastOne[0]["agent"]["agentId"] == "task-1",
          "reader keeps the last N events");
    auto onlySub = ReadHanchoEventsFile(eventsPath, 0, "subagent.started", 0);
    Check(onlySub.size() == 1 && onlySub[0]["data"]["agentId"] == "task-1",
          "reader filters by kind");
    auto sinceTwo = ReadHanchoEventsFile(eventsPath, 0, "", 2);
    Check(sinceTwo.size() == 1 && sinceTwo[0]["raw"]["line"] == 3,
          "reader filters by since_seq");
    Check(ReadHanchoEventsFile("", 0, "", 0).empty(),
          "reader returns nothing for an empty path");

    std::filesystem::remove(streamPath, ec);
    std::filesystem::remove(eventsPath, ec);
    std::filesystem::remove(dir, ec);

    if (g_failures == 0) std::printf("ALL PASS\n");
    else std::printf("%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
