// OpenCode JSON (opencode run --format json). One line per event: step_start / step_finish bracket
// a model step. "tool_use" carries one tool call in part.state (input, plus output or error once the
// state is completed or error), so it becomes a tool_call and, when finished, a tool_result.
// "text" carries a reply in part.text.

#include "llm_stream_internal.h"

namespace pc {
namespace llm {

bool ParseOpenCodeLines(const nlohmann::json& j, const std::string& trimmed, const std::string& agentType,
                        std::vector<LlmStreamEvent>& out) {
    if (!j.contains("part") || !j["part"].is_object()) return false;
    const std::string rawType = JsonStr(j, "type");
    const auto& part = j["part"];

    LlmStreamEvent base;
    base.rawJson = trimmed;
    base.agentType = agentType;
    base.messageId = JsonStr(part, "messageID");
    base.sessionId = JsonStr(j, "sessionID");

    if (rawType == "step_start" || rawType == "step_finish") {
        LlmStreamEvent ev = base;
        ev.type = "step";
        ev.status = rawType == "step_start" ? "running" : JsonStr(part, "reason");
        ev.content = rawType;
        if (rawType == "step_finish") {
            // Tokens and cost of this one model step (OpenCode totals include cache reads).
            ev.stopReason = JsonStr(part, "reason");
            if (part.contains("tokens") && part["tokens"].is_object()) {
                const auto& t = part["tokens"];
                ev.totalTokens = JsonU64(t, "total");
                ev.inputTokens = JsonU64(t, "input");
                ev.outputTokens = JsonU64(t, "output");
                ev.thinkingTokens = JsonU64(t, "reasoning");
                if (t.contains("cache") && t["cache"].is_object()) {
                    ev.cacheReadTokens = JsonU64(t["cache"], "read");
                    ev.cacheWriteTokens = JsonU64(t["cache"], "write");
                }
            }
            if (part.contains("cost") && part["cost"].is_number()) ev.costUsd = part["cost"].get<double>();
        }
        out.push_back(std::move(ev));
        return true;
    }

    if (rawType == "text") {
        LlmStreamEvent ev = base;
        ev.content = JsonStr(part, "text");
        if (ev.content.empty()) return true;
        ev.type = "message";
        ev.status = "generating";
        out.push_back(std::move(ev));
        return true;
    }

    if (rawType == "tool_use") {
        if (JsonStr(part, "type") != "tool" || !part.contains("state") || !part["state"].is_object()) return false;
        const auto& state = part["state"];
        const std::string st = JsonStr(state, "status");

        LlmStreamEvent call = base;
        call.type = "tool_call";
        call.status = "calling_tool";
        call.toolName = JsonStr(part, "tool");
        if (state.contains("input")) {
            call.toolInput = state["input"].is_string() ? state["input"].get<std::string>() : state["input"].dump();
        }
        FinishLlmEvent(call);
        out.push_back(call);

        if (st == "completed" || st == "error") {
            LlmStreamEvent res = base;
            res.type = "tool_result";
            res.status = "tool_result";
            res.toolName = call.toolName;
            res.toolError = st == "error";
            const char* key = state.contains("output") ? "output" : "error";
            if (state.contains(key)) {
                res.content = state[key].is_string() ? state[key].get<std::string>() : state[key].dump();
            }
            out.push_back(std::move(res));
        }
        return true;
    }
    return false;
}

}  // namespace llm
}  // namespace pc
