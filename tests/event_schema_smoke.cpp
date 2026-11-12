// Headless smoke test for the hancho.event/1 normalizer and the .events.jsonl writer.
// Representative internal LlmStreamEvent values for claude / opencode / codex / agy are normalized
// and checked for their kind and required data fields; a tracker run checks the derived file path,
// the appended lines and their raw.line numbers.

#include "../src/event_adapter.h"
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

static LlmStreamEvent RoundTripV1(const LlmStreamEvent& ev) {
    HanchoEventSource source;
    source.processId = 12;
    nlohmann::json v1 = BuildHanchoEventV1(ev, source, "", "proc_12_build_stream.jsonl", 7);
    return LlmStreamEventFromHanchoV1(v1);
}

static void CheckLegacyV1Compatibility() {
    LlmStreamEvent call = MakeEvent("tool_call", "claude");
    call.toolName = "Edit";
    call.toolUseId = "toolu-c1";
    call.toolInput = R"({"file_path":"src/a.cpp"})";
    LlmStreamEvent callBack = RoundTripV1(call);
    Check(callBack.type == "tool_call" && callBack.toolName == call.toolName &&
              callBack.toolUseId == call.toolUseId && callBack.toolInput == call.toolInput,
          "compat: tool_call keeps tool name, id and input through v1");

    LlmStreamEvent result = MakeEvent("tool_result", "claude");
    result.toolUseId = "toolu-c1";
    result.toolError = true;
    result.content = "permission denied";
    LlmStreamEvent resultBack = RoundTripV1(result);
    Check(resultBack.type == "tool_result" && resultBack.toolError &&
              resultBack.content == result.content && resultBack.toolUseId == result.toolUseId,
          "compat: tool_result keeps ok/error, text and tool id through v1");

    LlmStreamEvent thought = MakeEvent("thinking", "claude");
    thought.content = "considering";
    Check(RoundTripV1(thought).content == thought.content,
          "compat: thinking keeps its text through v1 (thought)");

    LlmStreamEvent reply = MakeEvent("message", "claude");
    reply.content = "done";
    LlmStreamEvent replyBack = RoundTripV1(reply);
    Check(replyBack.type == "message" && replyBack.content == reply.content,
          "compat: message keeps its text through v1");

    LlmStreamEvent usage = MakeEvent("usage", "claude");
    usage.inputTokens = 11;
    usage.outputTokens = 5;
    usage.cacheReadTokens = 2;
    usage.totalTokens = 18;
    usage.costUsd = 0.25;
    LlmStreamEvent usageBack = RoundTripV1(usage);
    Check(usageBack.type == "usage" && usageBack.inputTokens == usage.inputTokens &&
              usageBack.outputTokens == usage.outputTokens &&
              usageBack.cacheReadTokens == usage.cacheReadTokens &&
              usageBack.totalTokens == usage.totalTokens && usageBack.costUsd == usage.costUsd,
          "compat: usage keeps token counts and cost through v1");

    LlmStreamEvent runEnd = MakeEvent("result", "claude");
    runEnd.status = "completed";
    runEnd.stopReason = "end_turn";
    runEnd.numTurns = 3;
    runEnd.apiDurationMs = 1234;
    runEnd.permissionDenials = 2;
    LlmStreamEvent runEndBack = RoundTripV1(runEnd);
    Check(runEndBack.type == "result" && runEndBack.status == runEnd.status &&
              runEndBack.stopReason == runEnd.stopReason &&
              runEndBack.numTurns == runEnd.numTurns &&
              runEndBack.apiDurationMs == runEnd.apiDurationMs &&
              runEndBack.permissionDenials == runEnd.permissionDenials,
          "compat: result keeps status, turns, duration and denials through v1");

    LlmStreamEvent error = MakeEvent("error", "codex");
    error.content = "usage limit reached";
    LlmStreamEvent errorBack = RoundTripV1(error);
    Check(errorBack.type == "error" && errorBack.content == error.content,
          "compat: error keeps its message through v1");

    LlmStreamEvent subStart = MakeEvent("subagent_start", "claude");
    subStart.agentId = "task-1";
    subStart.toolUseId = "toolu-a1";
    subStart.subagentType = "general-purpose";
    subStart.taskDescription = "say ok";
    LlmStreamEvent subStartBack = RoundTripV1(subStart);
    Check(subStartBack.type == "subagent_start" && subStartBack.agentId == subStart.agentId &&
              subStartBack.toolUseId == subStart.toolUseId &&
              subStartBack.subagentType == subStart.subagentType &&
              subStartBack.taskDescription == subStart.taskDescription,
          "compat: subagent_start keeps agent, tool and description through v1");

    LlmStreamEvent subEnd = MakeEvent("subagent_end", "claude");
    subEnd.agentId = "task-1";
    subEnd.taskStatus = "completed";
    subEnd.taskTokens = 310;
    subEnd.taskToolUses = 2;
    subEnd.taskDurationMs = 900;
    LlmStreamEvent subEndBack = RoundTripV1(subEnd);
    Check(subEndBack.type == "subagent_end" && subEndBack.agentId == subEnd.agentId &&
              subEndBack.taskStatus == subEnd.taskStatus &&
              subEndBack.taskTokens == subEnd.taskTokens &&
              subEndBack.taskToolUses == subEnd.taskToolUses &&
              subEndBack.taskDurationMs == subEnd.taskDurationMs,
          "compat: subagent_end keeps status, tokens, tool uses and duration through v1");

    LlmStreamEvent init = MakeEvent("status", "claude");
    init.content = "init";
    init.cwd = "D:/work";
    init.toolsOffered = 14;
    LlmStreamEvent initBack = RoundTripV1(init);
    Check(initBack.type == "status" && initBack.content == "init" &&
              initBack.cwd == init.cwd && initBack.toolsOffered == init.toolsOffered,
          "compat: run.started keeps cwd and toolsOffered through v1");

    LlmStreamEvent limit = MakeEvent("rate_limit", "claude");
    limit.rateLimitType = "five_hour";
    limit.rateLimitStatus = "rejected";
    limit.rateLimitUtilization = 0.91;
    limit.rateLimitResetsAtSec = 1794303600;
    LlmStreamEvent limitBack = RoundTripV1(limit);
    Check(limitBack.type == "rate_limit" && limitBack.rateLimitType == limit.rateLimitType &&
              limitBack.rateLimitStatus == limit.rateLimitStatus &&
              limitBack.rateLimitUtilization == limit.rateLimitUtilization &&
              limitBack.rateLimitResetsAtSec == limit.rateLimitResetsAtSec,
          "compat: rate_limit keeps status, window, utilization and reset time through v1");
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

static bool CheckType(const nlohmann::json& val, const std::string& expected) {
    if (expected == "string") return val.is_string();
    if (expected == "integer") return val.is_number_integer();
    if (expected == "number") return val.is_number();
    if (expected == "boolean") return val.is_boolean();
    if (expected == "object") return val.is_object();
    if (expected == "array") return val.is_array();
    if (expected == "null") return val.is_null();
    return false;
}

static bool CheckPropType(const nlohmann::json& val, const nlohmann::json& propSchema, std::string& err) {
    if (propSchema.contains("type")) {
        const auto& t = propSchema["type"];
        if (t.is_string()) {
            if (!CheckType(val, t.get<std::string>())) {
                err = "expected type " + t.get<std::string>();
                return false;
            }
        } else if (t.is_array()) {
            bool ok = false;
            for (const auto& item : t) {
                if (item.is_string() && CheckType(val, item.get<std::string>())) {
                    ok = true;
                    break;
                }
            }
            if (!ok) {
                err = "value does not match allowed types " + t.dump();
                return false;
            }
        }
    }
    if (propSchema.contains("minimum") && val.is_number()) {
        double minVal = propSchema["minimum"].get<double>();
        if (val.get<double>() < minVal) {
            err = "value is below minimum";
            return false;
        }
    }
    if (propSchema.contains("enum") && propSchema["enum"].is_array()) {
        bool inEnum = false;
        for (const auto& e : propSchema["enum"]) {
            if (val == e) {
                inEnum = true;
                break;
            }
        }
        if (!inEnum) {
            err = "value not in enum " + propSchema["enum"].dump();
            return false;
        }
    }
    return true;
}

static bool ValidateHanchoEventAgainstSchema(const nlohmann::json& ev, const nlohmann::json& schema, std::string& err) {
    err.clear();
    if (!ev.is_object()) {
        err = "event must be a JSON object";
        return false;
    }
    for (const char* req : {"schema", "seq", "ts", "source", "agent", "kind", "data", "raw"}) {
        if (!ev.contains(req)) {
            err = std::string("missing top-level required field: ") + req;
            return false;
        }
    }
    if (ev["schema"] != "hancho.event/1") {
        err = "schema must be 'hancho.event/1'";
        return false;
    }
    if (!ev["seq"].is_number_integer() || ev["seq"].get<int64_t>() < 0) {
        err = "seq must be non-negative integer";
        return false;
    }
    if (!ev["ts"].is_string()) {
        err = "ts must be string";
        return false;
    }
    if (!ev["source"].is_object() || !ev["source"].contains("processId") || !ev["source"]["processId"].is_number_integer()) {
        err = "source must be object with integer processId";
        return false;
    }
    if (!ev["agent"].is_object() || !ev["agent"].contains("type") || !ev["agent"]["type"].is_string()) {
        err = "agent must be object with string type";
        return false;
    }
    if (!ev["raw"].is_object() || !ev["raw"].contains("file") || !ev["raw"]["file"].is_string()) {
        err = "raw must be object with string file";
        return false;
    }
    if (!ev["kind"].is_string()) {
        err = "kind must be string";
        return false;
    }
    std::string kind = ev["kind"].get<std::string>();
    if (!ev["data"].is_object()) {
        err = "data must be an object";
        return false;
    }
    const auto& data = ev["data"];

    const nlohmann::json* matchingBranch = nullptr;
    if (schema.contains("allOf") && schema["allOf"].is_array()) {
        for (const auto& branch : schema["allOf"]) {
            if (branch.contains("if") && branch["if"].contains("properties") &&
                branch["if"]["properties"].contains("kind") &&
                branch["if"]["properties"]["kind"].contains("const")) {
                if (branch["if"]["properties"]["kind"]["const"] == kind) {
                    matchingBranch = &branch;
                    break;
                }
            }
        }
    }
    if (!matchingBranch) {
        err = "unknown kind: " + kind;
        return false;
    }

    if (!matchingBranch->contains("then") || !(*matchingBranch)["then"].contains("properties") ||
        !(*matchingBranch)["then"]["properties"].contains("data")) {
        err = "schema missing then.properties.data for kind: " + kind;
        return false;
    }
    const auto& dataSchema = (*matchingBranch)["then"]["properties"]["data"];
    if (dataSchema.contains("required") && dataSchema["required"].is_array()) {
        for (const auto& req : dataSchema["required"]) {
            if (!data.contains(req.get<std::string>())) {
                err = "kind '" + kind + "' missing required data field '" + req.get<std::string>() + "'";
                return false;
            }
        }
    }

    if (dataSchema.contains("anyOf") && dataSchema["anyOf"].is_array()) {
        bool anyOk = false;
        for (const auto& opt : dataSchema["anyOf"]) {
            if (opt.contains("required") && opt["required"].is_array()) {
                bool allReq = true;
                for (const auto& r : opt["required"]) {
                    if (!data.contains(r.get<std::string>())) { allReq = false; break; }
                }
                if (allReq) { anyOk = true; break; }
            }
        }
        if (!anyOk) {
            err = "kind '" + kind + "' does not satisfy anyOf condition";
            return false;
        }
    }

    const auto& props = dataSchema.contains("properties") ? dataSchema["properties"] : nlohmann::json::object();
    bool noAdditional = dataSchema.value("additionalProperties", true) == false;
    if (noAdditional) {
        for (auto it = data.begin(); it != data.end(); ++it) {
            if (!props.contains(it.key())) {
                err = "kind '" + kind + "' contains unknown field in data: '" + it.key() + "'";
                return false;
            }
        }
    }

    for (auto it = data.begin(); it != data.end(); ++it) {
        if (props.contains(it.key())) {
            const auto& propSchema = props[it.key()];
            std::string propErr;
            if (!CheckPropType(it.value(), propSchema, propErr)) {
                err = "kind '" + kind + "' field 'data." + it.key() + "' invalid: " + propErr;
                return false;
            }
            if (propSchema.contains("items") && it.value().is_array()) {
                const auto& itemSchema = propSchema["items"];
                bool itemNoAdditional = itemSchema.value("additionalProperties", true) == false;
                const auto& itemProps = itemSchema.contains("properties") ? itemSchema["properties"] : nlohmann::json::object();
                for (size_t i = 0; i < it.value().size(); ++i) {
                    const auto& item = it.value()[i];
                    if (!item.is_object()) {
                        err = "kind '" + kind + "' items[" + std::to_string(i) + "] must be object";
                        return false;
                    }
                    if (itemSchema.contains("required") && itemSchema["required"].is_array()) {
                        for (const auto& r : itemSchema["required"]) {
                            if (!item.contains(r.get<std::string>())) {
                                err = "kind '" + kind + "' items[" + std::to_string(i) + "] missing required '" + r.get<std::string>() + "'";
                                return false;
                            }
                        }
                    }
                    if (itemNoAdditional) {
                        for (auto itemIt = item.begin(); itemIt != item.end(); ++itemIt) {
                            if (!itemProps.contains(itemIt.key())) {
                                err = "kind '" + kind + "' items[" + std::to_string(i) + "] contains unknown field '" + itemIt.key() + "'";
                                return false;
                            }
                        }
                    }
                    for (auto itemIt = item.begin(); itemIt != item.end(); ++itemIt) {
                        if (itemProps.contains(itemIt.key())) {
                            std::string subErr;
                            if (!CheckPropType(itemIt.value(), itemProps[itemIt.key()], subErr)) {
                                err = "kind '" + kind + "' items[" + std::to_string(i) + "]." + itemIt.key() + " invalid: " + subErr;
                                return false;
                            }
                        }
                    }
                }
            }
        }
    }
    return true;
}

static nlohmann::json LoadEventSchemaFile(std::string& resolvedPath) {
    std::vector<std::string> candidates = {
        "doc/schema/hancho.event.v1.json",
        "../doc/schema/hancho.event.v1.json",
        "../../doc/schema/hancho.event.v1.json"
    };
#ifdef PC_REPO_DIR
    candidates.insert(candidates.begin(), std::string(PC_REPO_DIR) + "/doc/schema/hancho.event.v1.json");
#endif
    wchar_t modPath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, modPath, MAX_PATH);
    if (len > 0) {
        std::filesystem::path p(modPath);
        candidates.push_back((p.parent_path() / "doc/schema/hancho.event.v1.json").string());
        candidates.push_back((p.parent_path().parent_path() / "doc/schema/hancho.event.v1.json").string());
        candidates.push_back((p.parent_path().parent_path().parent_path() / "doc/schema/hancho.event.v1.json").string());
    }
    for (const auto& c : candidates) {
        if (std::filesystem::exists(c)) {
            std::ifstream in(c, std::ios::binary);
            if (in) {
                auto j = nlohmann::json::parse(in, nullptr, false);
                if (j.is_object()) {
                    resolvedPath = c;
                    return j;
                }
            }
        }
    }
    return nlohmann::json();
}

static nlohmann::json MakeEnvelope(const std::string& kind, nlohmann::json data) {
    HanchoEventSource src;
    src.processId = 12;
    src.jobId = "j_test";
    src.attempt = 1;
    return BuildHanchoEventObject(src, "claude", "claude-test", "sess-1", "", "", 10, 1794300000000,
                                  kind, std::move(data), "test_stream.jsonl", 1);
}

int main() {
    SetConsoleOutputCP(CP_UTF8);

    std::string schemaPath;
    nlohmann::json schemaJson = LoadEventSchemaFile(schemaPath);
    Check(!schemaJson.empty(), "schema file loaded from " + schemaPath);

    Check(HanchoEventsFilePathForStream("logs/proc_12_build_stream.jsonl") ==
              "logs/proc_12_build.events.jsonl",
          "derived path: _stream.jsonl becomes .events.jsonl");
    Check(HanchoEventsFilePathForStream("logs/nothing.jsonl").empty(),
          "derived path: a path without _stream.jsonl yields no event file");

    CheckTopLevel("claude", "claude", "message");
    CheckLegacyV1Compatibility();

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

    LlmStreamEvent codexErr = MakeEvent("error", "codex");
    codexErr.content = "usage limit reached";
    Check(KindOf(codexErr) == "error" && DataOf(codexErr)["message"] == "usage limit reached",
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
        Check(written[0]["data"]["activity"] == "subagent" && written[2]["data"]["activity"] == "command" &&
                  written[2]["data"]["target"] == "echo hi",
              "reader consumers get v1 kind, activity and target without legacy type strings");
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

    // Schema validation of all generated events:
    if (!schemaJson.empty()) {
        std::string err;
        HanchoEventSource s;
        s.processId = 12;
        Check(ValidateHanchoEventAgainstSchema(BuildHanchoEventV1(call, s, "", "stream.jsonl", 1), schemaJson, err),
              "schema: claude tool.call validates: " + err);
        Check(ValidateHanchoEventAgainstSchema(BuildHanchoEventV1(result, s, "", "stream.jsonl", 2), schemaJson, err),
              "schema: claude tool.result validates: " + err);
        Check(ValidateHanchoEventAgainstSchema(BuildHanchoEventV1(thought, s, "", "stream.jsonl", 3), schemaJson, err),
              "schema: claude thought validates: " + err);
        Check(ValidateHanchoEventAgainstSchema(BuildHanchoEventV1(reply, s, "", "stream.jsonl", 4), schemaJson, err),
              "schema: claude message validates: " + err);
        Check(ValidateHanchoEventAgainstSchema(BuildHanchoEventV1(plan, s, "", "stream.jsonl", 5), schemaJson, err),
              "schema: claude plan validates: " + err);
        Check(ValidateHanchoEventAgainstSchema(BuildHanchoEventV1(init, s, "", "stream.jsonl", 6), schemaJson, err),
              "schema: claude run.started validates: " + err);
        Check(ValidateHanchoEventAgainstSchema(BuildHanchoEventV1(limit, s, "", "stream.jsonl", 7), schemaJson, err),
              "schema: claude rate_limit validates: " + err);
        Check(ValidateHanchoEventAgainstSchema(BuildHanchoEventV1(subStart, s, "", "stream.jsonl", 8), schemaJson, err),
              "schema: claude subagent.started validates: " + err);
        Check(ValidateHanchoEventAgainstSchema(BuildHanchoEventV1(subEnd, s, "", "stream.jsonl", 9), schemaJson, err),
              "schema: claude subagent.ended validates: " + err);
        Check(ValidateHanchoEventAgainstSchema(BuildHanchoEventV1(runEnd, s, "", "stream.jsonl", 10), schemaJson, err),
              "schema: claude run.ended validates: " + err);
        Check(ValidateHanchoEventAgainstSchema(BuildHanchoEventV1(step, s, "", "stream.jsonl", 11), schemaJson, err),
              "schema: opencode turn.started validates: " + err);
        Check(ValidateHanchoEventAgainstSchema(BuildHanchoEventV1(usage, s, "", "stream.jsonl", 12), schemaJson, err),
              "schema: opencode usage validates: " + err);
        Check(ValidateHanchoEventAgainstSchema(BuildHanchoEventV1(codexErr, s, "", "stream.jsonl", 13), schemaJson, err),
              "schema: codex error validates: " + err);
        Check(ValidateHanchoEventAgainstSchema(BuildHanchoEventV1(unknown, s, "", "stream.jsonl", 14), schemaJson, err),
              "schema: generic raw validates: " + err);

        // Tracker generated events validation
        for (size_t i = 0; i < allRead.size(); ++i) {
            std::string tErr;
            Check(ValidateHanchoEventAgainstSchema(allRead[i], schemaJson, tErr),
                  "schema: tracker written event " + std::to_string(i) + " validates: " + tErr);
        }

        // Adapter expected events validation
        std::string expectedPath = "tests/adapters/mycli/basic.expected.jsonl";
        if (std::filesystem::exists(expectedPath)) {
            std::ifstream inExp(expectedPath);
            std::string expLine;
            int adapterCount = 0;
            while (std::getline(inExp, expLine)) {
                if (expLine.empty() || expLine == "\r") continue;
                auto parsed = nlohmann::json::parse(expLine, nullptr, false);
                if (parsed.is_object() && parsed.contains("kind") && parsed.contains("data")) {
                    nlohmann::json ev = MakeEnvelope(parsed["kind"].get<std::string>(), parsed["data"]);
                    std::string aErr;
                    Check(ValidateHanchoEventAgainstSchema(ev, schemaJson, aErr),
                          "schema: adapter expected " + parsed["kind"].get<std::string>() + " validates: " + aErr);
                    adapterCount++;
                }
            }
            Check(adapterCount == 6, "schema: all 6 adapter expected events pass validation");
        }

        // Real sample events validation from ww-data
        int realValidated = 0;
        if (std::filesystem::exists("ww-data")) {
            for (const auto& entry : std::filesystem::directory_iterator("ww-data")) {
                if (!entry.is_regular_file()) continue;
                std::string name = entry.path().filename().string();
                if (name.rfind("proc_", 0) == 0 && name.size() > 13 &&
                    name.substr(name.size() - 13) == ".events.jsonl") {
                    std::ifstream inReal(entry.path());
                    std::string realLine;
                    while (std::getline(inReal, realLine)) {
                        if (realLine.empty() || realLine == "\r") continue;
                        auto parsed = nlohmann::json::parse(realLine, nullptr, false);
                        if (parsed.is_object() && parsed.value("schema", "") == "hancho.event/1") {
                            std::string realErr;
                            bool ok = ValidateHanchoEventAgainstSchema(parsed, schemaJson, realErr);
                            if (!ok) {
                                Check(false, "real sample in " + name + " failed: " + realErr);
                                break;
                            }
                            realValidated++;
                        }
                    }
                }
            }
        }
        if (realValidated > 0) {
            Check(true, "real samples pass schema validation (" + std::to_string(realValidated) + " events checked)");
        }

        // Negative tests: verify unknown fields and type errors are rejected
        // 1. Unknown field in tool.call
        nlohmann::json badToolCall = MakeEnvelope("tool.call", {
            {"tool", "Edit"}, {"activity", "edit"}, {"extra_field", 123}
        });
        Check(!ValidateHanchoEventAgainstSchema(badToolCall, schemaJson, err) &&
                  err.find("unknown field in data") != std::string::npos,
              "negative: tool.call rejects unknown field in data");

        // 2. Invalid activity enum in tool.call
        nlohmann::json badActivity = MakeEnvelope("tool.call", {
            {"tool", "Edit"}, {"activity", "invalid_activity"}
        });
        Check(!ValidateHanchoEventAgainstSchema(badActivity, schemaJson, err) &&
                  err.find("activity") != std::string::npos,
              "negative: tool.call rejects invalid activity enum");

        // 3. Non-string tool name in tool.call
        nlohmann::json badToolType = MakeEnvelope("tool.call", {
            {"tool", 42}, {"activity", "edit"}
        });
        Check(!ValidateHanchoEventAgainstSchema(badToolType, schemaJson, err),
              "negative: tool.call rejects non-string tool name");

        // 4. Unknown field in plan data
        nlohmann::json badPlanExtra = MakeEnvelope("plan", {
            {"items", nlohmann::json::array()}, {"bogus", true}
        });
        Check(!ValidateHanchoEventAgainstSchema(badPlanExtra, schemaJson, err) &&
                  err.find("unknown field in data") != std::string::npos,
              "negative: plan rejects unknown field in data");

        // 5. Plan item with unknown status enum
        nlohmann::json badPlanStatus = MakeEnvelope("plan", {
            {"items", nlohmann::json::array({
                {{"content", "build"}, {"status", "not_a_valid_status"}}
            })}
        });
        Check(!ValidateHanchoEventAgainstSchema(badPlanStatus, schemaJson, err) &&
                  err.find("status") != std::string::npos,
              "negative: plan rejects invalid status enum in items");

        // 6. Plan item with unknown property
        nlohmann::json badPlanItemProp = MakeEnvelope("plan", {
            {"items", nlohmann::json::array({
                {{"content", "build"}, {"status", "pending"}, {"priority", "high"}}
            })}
        });
        Check(!ValidateHanchoEventAgainstSchema(badPlanItemProp, schemaJson, err) &&
                  err.find("unknown field") != std::string::npos,
              "negative: plan rejects unknown property inside items[]");

        // 7. Unknown field in thought data
        nlohmann::json badThought = MakeEnvelope("thought", {
            {"text", "thinking"}, {"confidence", 0.9}
        });
        Check(!ValidateHanchoEventAgainstSchema(badThought, schemaJson, err) &&
                  err.find("unknown field in data") != std::string::npos,
              "negative: thought rejects unknown field in data");

        // 8. Non-string text in message
        nlohmann::json badMessage = MakeEnvelope("message", {
            {"text", 12345}
        });
        Check(!ValidateHanchoEventAgainstSchema(badMessage, schemaJson, err),
              "negative: message rejects non-string text");

        // 9. Unknown field in usage data
        nlohmann::json badUsage = MakeEnvelope("usage", {
            {"inputTokens", 10}, {"unknownTokenCount", 5}
        });
        Check(!ValidateHanchoEventAgainstSchema(badUsage, schemaJson, err) &&
                  err.find("unknown field in data") != std::string::npos,
              "negative: usage rejects unknown field in data");

        // 10. Usage with negative token count
        nlohmann::json badUsageNeg = MakeEnvelope("usage", {
            {"inputTokens", -1}
        });
        Check(!ValidateHanchoEventAgainstSchema(badUsageNeg, schemaJson, err),
              "negative: usage rejects negative token count");

        // 11. Usage with no token counts at all
        nlohmann::json badUsageEmpty = MakeEnvelope("usage", {
            {"costUsd", 0.05}
        });
        Check(!ValidateHanchoEventAgainstSchema(badUsageEmpty, schemaJson, err) &&
                  err.find("anyOf") != std::string::npos,
              "negative: usage without token count fails anyOf condition");

        // 12. Run.ended with unknown field
        nlohmann::json badRunEndExtra = MakeEnvelope("run.ended", {
            {"status", "completed"}, {"unexpected", 1}
        });
        Check(!ValidateHanchoEventAgainstSchema(badRunEndExtra, schemaJson, err) &&
                  err.find("unknown field in data") != std::string::npos,
              "negative: run.ended rejects unknown field in data");

        // 13. Run.ended with invalid status
        nlohmann::json badRunEndStatus = MakeEnvelope("run.ended", {
            {"status", "finished"}
        });
        Check(!ValidateHanchoEventAgainstSchema(badRunEndStatus, schemaJson, err) &&
                  err.find("status") != std::string::npos,
              "negative: run.ended rejects invalid status");

        // 14. Subagent.started missing required agentId
        nlohmann::json badSubagentStart = MakeEnvelope("subagent.started", {
            {"type", "general-purpose"}
        });
        Check(!ValidateHanchoEventAgainstSchema(badSubagentStart, schemaJson, err) &&
                  err.find("missing required") != std::string::npos,
              "negative: subagent.started rejects missing required agentId");

        // 15. Subagent.ended with unknown field
        nlohmann::json badSubagentEnd = MakeEnvelope("subagent.ended", {
            {"agentId", "a1"}, {"status", "completed"}, {"extraMetric", 100}
        });
        Check(!ValidateHanchoEventAgainstSchema(badSubagentEnd, schemaJson, err) &&
                  err.find("unknown field in data") != std::string::npos,
              "negative: subagent.ended rejects unknown field in data");

        // 16. Error with unknown field
        nlohmann::json badError = MakeEnvelope("error", {
            {"message", "err"}, {"stackTrace", "line 1"}
        });
        Check(!ValidateHanchoEventAgainstSchema(badError, schemaJson, err) &&
                  err.find("unknown field in data") != std::string::npos,
              "negative: error rejects unknown field in data");

        // 17. Raw with unknown field
        nlohmann::json badRaw = MakeEnvelope("raw", {
            {"text", "log line"}, {"extra", 42}
        });
        Check(!ValidateHanchoEventAgainstSchema(badRaw, schemaJson, err) &&
                  err.find("unknown field in data") != std::string::npos,
              "negative: raw rejects unknown field in data");

        // Free-form exception: tool.call input can be arbitrary object or string
        nlohmann::json freeFormObj = MakeEnvelope("tool.call", {
            {"tool", "custom_tool"}, {"activity", "tool"},
            {"input", {{"param1", "value1"}, {"nested", {{"x", 123}}}}}
        });
        Check(ValidateHanchoEventAgainstSchema(freeFormObj, schemaJson, err),
              "free-form exception: tool.call accepts arbitrary object in data.input");

        nlohmann::json freeFormStr = MakeEnvelope("tool.call", {
            {"tool", "shell"}, {"activity", "command"},
            {"input", "npm test -- --coverage"}
        });
        Check(ValidateHanchoEventAgainstSchema(freeFormStr, schemaJson, err),
              "free-form exception: tool.call accepts string in data.input");

        nlohmann::json badInputType = MakeEnvelope("tool.call", {
            {"tool", "shell"}, {"activity", "command"},
            {"input", 12345}
        });
        Check(!ValidateHanchoEventAgainstSchema(badInputType, schemaJson, err),
              "negative: tool.call rejects number in data.input");
    }

    std::filesystem::remove(streamPath, ec);
    std::filesystem::remove(eventsPath, ec);
    std::filesystem::remove(dir, ec);

    if (g_failures == 0) std::printf("ALL PASS\n");
    else std::printf("%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
