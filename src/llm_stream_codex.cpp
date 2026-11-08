// Codex CLI (codex exec --json). One line per event: thread.started, turn.started, then
// item.started / item.updated / item.completed around each item, and turn.completed or turn.failed.
// "error" lines are also sent while Codex retries ("Reconnecting... 2/5 (...)"), so those are status.
// A command or MCP call becomes a tool_call when started and a tool_result when completed. A file
// change or web search is only complete in item.completed, which gives both events at once.
// Codex reports no cost: a run on the free tier or a plan shows tokens only.

#include "llm_stream_internal.h"

namespace pc {
namespace llm {

namespace {

bool IsLimitMessage(const std::string& message) {
    const std::string m = LowerAscii(message);
    for (const char* word : {"usage limit", "rate limit", "insufficient_quota", "quota", "status 429", "too many requests"}) {
        if (m.find(word) != std::string::npos) return true;
    }
    return false;
}

void AppendCodexError(const LlmStreamEvent& base, const std::string& message, std::vector<LlmStreamEvent>& out) {
    LlmStreamEvent ev = base;
    ev.type = "error";
    ev.status = "error";
    ev.content = message;
    out.push_back(std::move(ev));
    if (IsLimitMessage(message)) {
        LlmStreamEvent limit = base;
        limit.type = "rate_limit";
        limit.status = "error";
        limit.content = message;
        limit.rateLimitType = "usage_limit";
        limit.rateLimitStatus = "rejected";
        out.push_back(std::move(limit));
    }
}

void AppendToolCall(const LlmStreamEvent& base, const std::string& tool, const std::string& input,
                    std::vector<LlmStreamEvent>& out) {
    LlmStreamEvent ev = base;
    ev.type = "tool_call";
    ev.status = "calling_tool";
    ev.toolName = tool;
    ev.toolInput = input;
    FinishLlmEvent(ev);
    out.push_back(std::move(ev));
}

void AppendToolResult(const LlmStreamEvent& base, const std::string& tool, const std::string& content, bool failed,
                      std::vector<LlmStreamEvent>& out) {
    LlmStreamEvent ev = base;
    ev.type = "tool_result";
    ev.status = "tool_result";
    ev.toolName = tool;
    ev.toolError = failed;
    ev.content = content;
    out.push_back(std::move(ev));
}

// Claude's TodoWrite shape, which the activity parser reads as "done/total: current".
std::string TodoInput(const nlohmann::json& item) {
    nlohmann::json todos = nlohmann::json::array();
    bool currentSet = false;
    if (item.contains("items") && item["items"].is_array()) {
        for (const auto& t : item["items"]) {
            if (!t.is_object()) continue;
            const bool done = t.contains("completed") && t["completed"].is_boolean() && t["completed"].get<bool>();
            const char* status = done ? "completed" : (currentSet ? "pending" : "in_progress");
            if (!done) currentSet = true;
            todos.push_back({{"content", JsonStr(t, "text")}, {"status", status}});
        }
    }
    return nlohmann::json{{"todos", todos}}.dump();
}

std::string McpToolName(const nlohmann::json& item) {
    return JsonStr(item, "server") + "." + JsonStr(item, "tool");
}

bool ParseCodexItem(const nlohmann::json& item, const std::string& rawType, const LlmStreamEvent& base,
                    std::vector<LlmStreamEvent>& out) {
    const std::string itemType = JsonStr(item, "type");
    const bool started = rawType == "item.started";
    const bool completed = rawType == "item.completed";
    const std::string st = JsonStr(item, "status");
    const bool failed = st == "failed" || st == "declined";

    if (itemType == "agent_message" || itemType == "reasoning") {
        if (!completed) return true;
        LlmStreamEvent ev = base;
        ev.content = JsonStr(item, "text");
        if (ev.content.empty()) return true;
        ev.type = itemType == "reasoning" ? "thinking" : "message";
        ev.status = itemType == "reasoning" ? "thinking" : "generating";
        out.push_back(std::move(ev));
    } else if (itemType == "command_execution") {
        if (started) {
            AppendToolCall(base, "command_execution", nlohmann::json{{"command", JsonStr(item, "command")}}.dump(), out);
        } else if (completed) {
            const bool nonZero = item.contains("exit_code") && item["exit_code"].is_number() && item["exit_code"].get<int64_t>() != 0;
            AppendToolResult(base, "command_execution", JsonStr(item, "aggregated_output"), failed || nonZero, out);
        }
    } else if (itemType == "mcp_tool_call") {
        const std::string tool = McpToolName(item);
        if (started) {
            AppendToolCall(base, tool, item.contains("arguments") ? item["arguments"].dump() : "", out);
        } else if (completed) {
            std::string content;
            if (item.contains("error") && item["error"].is_object()) content = JsonStr(item["error"], "message");
            else if (item.contains("result") && !item["result"].is_null()) content = item["result"].dump();
            AppendToolResult(base, tool, content, failed, out);
        }
    } else if (itemType == "file_change") {
        if (!completed) return true;
        std::string paths;
        if (item.contains("changes") && item["changes"].is_array()) {
            for (const auto& c : item["changes"]) {
                const std::string path = JsonStr(c, "path");
                if (path.empty()) continue;
                if (!paths.empty()) paths += ", ";
                paths += path;
            }
        }
        AppendToolCall(base, "apply_patch", nlohmann::json{{"file_path", paths}}.dump(), out);
        AppendToolResult(base, "apply_patch", paths, failed, out);
    } else if (itemType == "web_search") {
        if (!completed) return true;
        AppendToolCall(base, "web_search", nlohmann::json{{"query", JsonStr(item, "query")}}.dump(), out);
        AppendToolResult(base, "web_search", "", failed, out);
    } else if (itemType == "todo_list") {
        if (!completed) AppendToolCall(base, "todo_list", TodoInput(item), out);
    } else if (itemType == "error") {
        if (completed) AppendCodexError(base, JsonStr(item, "message"), out);
    } else {
        return false;
    }
    return true;
}

}  // namespace

bool ParseCodexLines(const nlohmann::json& j, const std::string& trimmed, const std::string& agentType,
                     std::vector<LlmStreamEvent>& out) {
    const std::string rawType = JsonStr(j, "type");

    LlmStreamEvent base;
    base.rawJson = trimmed;
    base.agentType = agentType;

    if (rawType == "thread.started") {
        LlmStreamEvent ev = base;
        ev.type = "status";
        ev.status = "running";
        ev.content = "init";
        ev.sessionId = JsonStr(j, "thread_id");
        out.push_back(std::move(ev));
        return true;
    }

    if (rawType == "turn.started") {
        LlmStreamEvent ev = base;
        ev.type = "step";
        ev.status = "running";
        ev.content = rawType;
        out.push_back(std::move(ev));
        return true;
    }

    if (rawType == "turn.completed") {
        LlmStreamEvent ev = base;
        ev.type = "result";
        ev.status = "completed";
        ev.content = rawType;
        if (j.contains("usage") && j["usage"].is_object()) {
            // input_tokens includes the cached part and output_tokens the reasoning part.
            const auto& u = j["usage"];
            const uint64_t input = JsonU64(u, "input_tokens");
            const uint64_t cached = JsonU64(u, "cached_input_tokens");
            const uint64_t output = JsonU64(u, "output_tokens");
            const uint64_t reasoning = JsonU64(u, "reasoning_output_tokens");
            ev.inputTokens = input >= cached ? input - cached : 0;
            ev.cacheReadTokens = cached;
            ev.outputTokens = output >= reasoning ? output - reasoning : 0;
            ev.thinkingTokens = reasoning;
            ev.totalTokens = input + output;
        }
        out.push_back(std::move(ev));
        return true;
    }

    if (rawType == "turn.failed") {
        std::string message = "turn failed";
        if (j.contains("error") && j["error"].is_object() && !JsonStr(j["error"], "message").empty()) {
            message = JsonStr(j["error"], "message");
        }
        AppendCodexError(base, message, out);
        return true;
    }

    if (rawType == "error") {
        const std::string message = JsonStr(j, "message");
        if (message.rfind("Reconnecting...", 0) == 0) {
            LlmStreamEvent ev = base;
            ev.type = "status";
            ev.status = "retrying";
            ev.content = message;
            out.push_back(std::move(ev));
        } else {
            AppendCodexError(base, message, out);
        }
        return true;
    }

    if (rawType == "item.started" || rawType == "item.updated" || rawType == "item.completed") {
        if (!j.contains("item") || !j["item"].is_object()) return false;
        return ParseCodexItem(j["item"], rawType, base, out);
    }
    return false;
}

}  // namespace llm
}  // namespace pc
