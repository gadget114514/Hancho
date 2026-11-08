// Claude Code stream-json (claude -p --output-format stream-json --verbose).
// "assistant" and "user" lines keep their payload in message.content[], one block per entry:
// thinking, tool_use and text for assistant; tool_result and text for user. Each block becomes
// its own event. system/init, system task_started / task_notification, rate_limit_event and result
// lines are one event each. stream_event lines (--include-partial-messages) and other system lines
// are transient, see ParseClaudeStreamEvent. A sub-agent's lines carry parent_tool_use_id, agent_id
// and subagent_type.

#include "llm_stream_internal.h"

#include <cstdio>

namespace pc {
namespace llm {

namespace {

// Text of a tool_result / user content: a string, or an array of {"type":"text","text":...}.
std::string ClaudeContentText(const nlohmann::json& content) {
    if (content.is_string()) return content.get<std::string>();
    std::string out;
    if (!content.is_array()) return out;
    for (const auto& part : content) {
        std::string text = JsonStr(part, "text");
        if (text.empty()) continue;
        if (!out.empty()) out += '\n';
        out += text;
    }
    return out;
}

bool ParseClaudeMessageLine(const nlohmann::json& j, const std::string& trimmed,
                            const std::string& defaultAgentType, std::vector<LlmStreamEvent>& out) {
    if (!j.contains("message") || !j["message"].is_object() || !j["message"].contains("content")) return false;
    const auto& msg = j["message"];
    const auto& content = msg["content"];
    const bool isAssistant = JsonStr(j, "type") == "assistant";

    LlmStreamEvent base;
    base.rawJson = trimmed;
    base.agentType = defaultAgentType.empty() ? "generic" : defaultAgentType;
    base.model = JsonStr(msg, "model");
    base.messageId = JsonStr(msg, "id");
    base.sessionId = JsonStr(j, "session_id");
    base.stopReason = JsonStr(msg, "stop_reason");
    // Set on the lines of a sub-agent (Agent tool); null on the main agent's lines.
    base.parentToolUseId = JsonStr(j, "parent_tool_use_id");
    base.agentId = JsonStr(j, "agent_id");
    base.subagentType = JsonStr(j, "subagent_type");
    base.taskDescription = JsonStr(j, "task_description");
    // Every content block of one message repeats its usage; the tracker counts it once per message id.
    if (isAssistant && msg.contains("usage")) ApplyLlmUsageFields(msg["usage"], base);

    // Blocks are referenced, not copied: a tool_result block can be megabytes.
    nlohmann::json textBlock;
    std::vector<const nlohmann::json*> blocks;
    if (content.is_array()) {
        for (const auto& b : content) blocks.push_back(&b);
    } else if (content.is_string()) {
        textBlock = {{"type", "text"}, {"text", content}};
        blocks.push_back(&textBlock);
    } else {
        return false;
    }

    for (const nlohmann::json* blockPtr : blocks) {
        const nlohmann::json& block = *blockPtr;
        if (!block.is_object()) continue;
        const std::string btype = JsonStr(block, "type");
        LlmStreamEvent ev = base;

        if (isAssistant && btype == "tool_use") {
            ev.type = "tool_call";
            ev.status = "calling_tool";
            ev.toolUseId = JsonStr(block, "id");
            ev.toolName = JsonStr(block, "name");
            if (block.contains("input")) {
                ev.toolInput = block["input"].is_string() ? block["input"].get<std::string>() : block["input"].dump();
            }
        } else if (btype == "text") {
            ev.content = JsonStr(block, "text");
            if (ev.content.empty()) continue;  // no text, nothing to show
            ev.type = isAssistant ? "message" : "user_input";
            ev.status = "generating";
        } else if (isAssistant) {
            // thinking / redacted_thinking. Claude may stream the block with an empty thinking text.
            ev.type = "thinking";
            ev.status = "thinking";
            ev.content = JsonStr(block, "thinking");
        } else if (btype == "tool_result") {
            ev.type = "tool_result";
            ev.status = "tool_result";
            ev.toolUseId = JsonStr(block, "tool_use_id");
            ev.toolError = block.contains("is_error") && block["is_error"].is_boolean() && block["is_error"].get<bool>();
            if (block.contains("content")) ev.content = ClaudeContentText(block["content"]);
        } else {
            continue;
        }

        FinishLlmEvent(ev);
        out.push_back(std::move(ev));
    }
    return true;
}

// Partial-message chunks (--include-partial-messages) are transient: the tracker uses them for the
// live state and the usage, but they are not stored or saved. The complete assistant line that
// follows still carries the full text, tool input and usage.
void ParseClaudeStreamEvent(const nlohmann::json& j, const std::string& trimmed, const std::string& agentType,
                            std::vector<LlmStreamEvent>& out) {
    static const nlohmann::json kEmpty = nlohmann::json::object();
    const nlohmann::json& e = (j.contains("event") && j["event"].is_object()) ? j["event"] : kEmpty;
    const std::string et = JsonStr(e, "type");

    LlmStreamEvent ev;
    ev.rawJson = trimmed;
    ev.agentType = agentType.empty() ? "generic" : agentType;
    ev.sessionId = JsonStr(j, "session_id");
    ev.messageId = JsonStr(j, "api_message_id");
    ev.parentToolUseId = JsonStr(j, "parent_tool_use_id");
    ev.agentId = JsonStr(j, "agent_id");
    ev.transient = true;
    ev.type = "partial";

    if (et == "message_start") {
        if (e.contains("message") && e["message"].is_object()) ev.model = JsonStr(e["message"], "model");
    } else if (et == "content_block_start") {
        // A new block: the streamed text of the previous one is over.
        const nlohmann::json& block = e.contains("content_block") ? e["content_block"] : kEmpty;
        const std::string bt = JsonStr(block, "type");
        ev.status = "block_start";
        if (bt == "thinking") {
            ev.type = "thinking";
        } else if (bt == "text") {
            ev.type = "message";
        } else if (bt == "tool_use") {
            ev.type = "tool_call";
            ev.status = "calling_tool";
            ev.toolName = JsonStr(block, "name");
        }
    } else if (et == "content_block_delta") {
        const nlohmann::json& delta = e.contains("delta") ? e["delta"] : kEmpty;
        const std::string dt = JsonStr(delta, "type");
        if (dt == "thinking_delta") {
            ev.type = "thinking";
            ev.content = JsonStr(delta, "thinking");
        } else if (dt == "text_delta") {
            ev.type = "message";
            ev.content = JsonStr(delta, "text");
        }
    } else if (et == "message_delta") {
        // The final usage of the message, which the assistant line may not have yet.
        ev.type = "usage";
        ev.status = "idle";
        if (e.contains("delta") && e["delta"].is_object()) ev.stopReason = JsonStr(e["delta"], "stop_reason");
        if (e.contains("usage")) ApplyLlmUsageFields(e["usage"], ev);
    }
    out.push_back(std::move(ev));
}

}  // namespace

bool ParseClaudeLines(const nlohmann::json& j, const std::string& trimmed, const std::string& agentType,
                      std::vector<LlmStreamEvent>& out) {
    const std::string type = JsonStr(j, "type");
    if (type == "stream_event") {
        ParseClaudeStreamEvent(j, trimmed, agentType, out);
        return true;
    }
    if (type == "assistant" || type == "user") return ParseClaudeMessageLine(j, trimmed, agentType, out);

    LlmStreamEvent ev;
    ev.rawJson = trimmed;
    ev.agentType = agentType.empty() ? "generic" : agentType;
    ev.sessionId = JsonStr(j, "session_id");
    ev.model = JsonStr(j, "model");
    ev.stopReason = JsonStr(j, "stop_reason");

    if (type == "system" && JsonStr(j, "subtype") == "init") {
        // Working directory and the tools offered to the model.
        ev.type = "status";
        ev.status = "running";
        ev.content = "init";
        ev.cwd = JsonStr(j, "cwd");
        if (j.contains("tools") && j["tools"].is_array()) ev.toolsOffered = j["tools"].size();
    } else if (type == "system" && JsonStr(j, "subtype") == "task_started") {
        // A sub-agent starts: task_id is its agent id, tool_use_id the Agent call that spawned it.
        ev.type = "subagent_start";
        ev.status = "running";
        ev.agentId = JsonStr(j, "task_id");
        ev.toolUseId = JsonStr(j, "tool_use_id");
        ev.subagentType = JsonStr(j, "subagent_type");
        ev.taskDescription = JsonStr(j, "description");
        ev.taskStatus = "running";
        ev.content = ev.taskDescription;
    } else if (type == "system" && JsonStr(j, "subtype") == "task_notification") {
        // A sub-agent ends: its status, its final text (summary) and its usage.
        ev.type = "subagent_end";
        ev.agentId = JsonStr(j, "task_id");
        ev.toolUseId = JsonStr(j, "tool_use_id");
        ev.taskStatus = JsonStr(j, "status");
        ev.status = ev.taskStatus;
        ev.content = JsonStr(j, "summary");
        if (j.contains("usage") && j["usage"].is_object()) {
            const auto& u = j["usage"];
            ev.taskTokens = JsonU64(u, "total_tokens");
            ev.taskToolUses = JsonU64(u, "tool_uses");
            ev.taskDurationMs = JsonU64(u, "duration_ms");
        }
    } else if (type == "rate_limit_event" && j.contains("rate_limit_info") && j["rate_limit_info"].is_object()) {
        // How much of the plan window (five_hour, seven_day, ...) is used.
        const auto& info = j["rate_limit_info"];
        ev.type = "rate_limit";
        ev.status = "idle";
        ev.rateLimitType = JsonStr(info, "rateLimitType");
        ev.rateLimitStatus = JsonStr(info, "status");
        ev.rateLimitResetsAtSec = static_cast<int64_t>(JsonU64(info, "resetsAt"));
        if (info.contains("unifiedWindows") && info["unifiedWindows"].is_object() &&
            info["unifiedWindows"].contains(ev.rateLimitType) && info["unifiedWindows"][ev.rateLimitType].is_object()) {
            const auto& w = info["unifiedWindows"][ev.rateLimitType];
            if (w.contains("utilization") && w["utilization"].is_number()) {
                ev.rateLimitUtilization = w["utilization"].get<double>();
            }
        }
        char pct[16] = {};
        if (ev.rateLimitUtilization >= 0.0) snprintf(pct, sizeof(pct), " %.0f%%", ev.rateLimitUtilization * 100.0);
        ev.content = ev.rateLimitType + " " + ev.rateLimitStatus + pct;
    } else if (type == "result") {
        // Final line of a run: outcome, total cost, usage of the run, API time and turn count.
        bool isError = j.contains("is_error") && j["is_error"].is_boolean() && j["is_error"].get<bool>();
        ev.type = isError ? "error" : "result";
        ev.status = isError ? "error" : "completed";
        ev.content = JsonStr(j, "summary");
        if (ev.content.empty()) ev.content = JsonStr(j, "result");
        if (j.contains("total_cost_usd") && j["total_cost_usd"].is_number()) ev.costUsd = j["total_cost_usd"].get<double>();
        if (j.contains("usage")) ApplyLlmUsageFields(j["usage"], ev);
        ev.apiDurationMs = JsonU64(j, "duration_api_ms");
        ev.numTurns = JsonU64(j, "num_turns");
        ev.runTotal = true;  // its usage is the run total, not one message's
        if (j.contains("permission_denials") && j["permission_denials"].is_array()) {
            ev.permissionDenials = j["permission_denials"].size();
        }
    } else if (type == "system") {
        // Progress lines (status, thinking_tokens, task_summary, ...) sent with --include-partial-messages.
        ev.type = "system";
        ev.transient = true;
    } else {
        return false;
    }
    out.push_back(std::move(ev));
    return true;
}

}  // namespace llm
}  // namespace pc
