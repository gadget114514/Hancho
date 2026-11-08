#define _CRT_SECURE_NO_WARNINGS
#include "llm_stream.h"
#include "llm_stream_internal.h"
#include "common.h"
#include "nlohmann/json.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstddef>
#include <new>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <regex>
#include <chrono>
#include <ctime>
#include <iterator>
#include <sstream>

namespace pc {

namespace {

static std::string ToLowerStr(const std::string& s) {
    std::string out = s;
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

static bool WordMatches(const std::string& text, const std::string& word) {
    size_t p = text.find(word);
    while (p != std::string::npos) {
        bool leftOk = (p == 0) || !std::isalnum(static_cast<unsigned char>(text[p - 1]));
        bool rightOk = (p + word.size() >= text.size()) || !std::isalnum(static_cast<unsigned char>(text[p + word.size()]));
        if (leftOk && rightOk) return true;
        p = text.find(word, p + 1);
    }
    return false;
}

static std::string TrimStr(const std::string& s) {
    size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

// Byte-limited prefix that never splits a UTF-8 multi-byte sequence
// (a split sequence makes nlohmann::json::dump() throw).
static std::string Utf8Prefix(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t n = maxBytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
    return s.substr(0, n);
}

constexpr int64_t kUsageRetentionSec = 24 * 60 * 60;
constexpr size_t kMaxUsagePoints = 200000;
static std::mutex g_usageHistoryMutex;
static std::deque<LlmUsagePoint> g_usageHistory;
static bool g_usageHistoryLoaded = false;
static bool g_usageHistoryPersist = false;
static std::mutex g_usageFileMutex;
static std::atomic<uint64_t> g_recordCalls{0};
static std::string g_lastSkipReason;

static std::wstring UsageHistoryPathWide() {
    return GetLogsDirectoryWide() + L"\\usage_history.jsonl";
}

static void LoadUsageHistoryLocked() {
    if (g_usageHistoryLoaded) return;
    g_usageHistoryLoaded = true;
    if (!g_usageHistoryPersist) return;
    std::ifstream f(UsageHistoryPathWide());
    if (!f) return;
    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        auto j = nlohmann::json::parse(line, nullptr, false);
        if (!j.is_object()) continue;
        LlmUsagePoint p;
        p.epochSec = j.value("epochSec", (int64_t)0);
        p.inputTokens = j.value("inputTokens", (uint64_t)0);
        p.outputTokens = j.value("outputTokens", (uint64_t)0);
        p.thinkingTokens = j.value("thinkingTokens", (uint64_t)0);
        p.cacheReadTokens = j.value("cacheReadTokens", (uint64_t)0);
        p.cacheWriteTokens = j.value("cacheWriteTokens", (uint64_t)0);
        p.totalTokens = j.value("totalTokens", (uint64_t)0);
        p.costUsd = j.value("costUsd", 0.0);
        p.agentType = j.value("agentType", std::string());
        p.model = j.value("model", std::string());
        if (p.epochSec <= 0) continue;
        g_usageHistory.push_back(std::move(p));
    }
    int64_t now = static_cast<int64_t>(time(nullptr));
    int64_t cutoff = now - kUsageRetentionSec;
    while (!g_usageHistory.empty() && g_usageHistory.front().epochSec < cutoff) g_usageHistory.pop_front();
    while (g_usageHistory.size() > kMaxUsagePoints) g_usageHistory.pop_front();
}

} // namespace

void RecordLlmUsagePoint(int64_t epochSec, const LlmUsagePoint& point) {
    g_recordCalls++;
    if (point.totalTokens == 0 && point.inputTokens == 0 && point.outputTokens == 0 &&
        point.thinkingTokens == 0 && point.cacheReadTokens == 0 && point.cacheWriteTokens == 0 &&
        point.costUsd <= 0.0) {
        return;
    }
    LlmUsagePoint p = point;
    p.epochSec = epochSec;
    bool persist = false;
    {
        std::lock_guard<std::mutex> lock(g_usageHistoryMutex);
        LoadUsageHistoryLocked();
        g_usageHistory.push_back(p);
        int64_t cutoff = epochSec - kUsageRetentionSec;
        while (!g_usageHistory.empty() && g_usageHistory.front().epochSec < cutoff) {
            g_usageHistory.pop_front();
        }
        while (g_usageHistory.size() > kMaxUsagePoints) {
            g_usageHistory.pop_front();
        }
        persist = g_usageHistoryPersist;
    }
    if (persist) {
        nlohmann::json j;
        j["epochSec"] = p.epochSec;
        j["inputTokens"] = p.inputTokens;
        j["outputTokens"] = p.outputTokens;
        j["thinkingTokens"] = p.thinkingTokens;
        j["cacheReadTokens"] = p.cacheReadTokens;
        j["cacheWriteTokens"] = p.cacheWriteTokens;
        j["totalTokens"] = p.totalTokens;
        j["costUsd"] = p.costUsd;
        j["agentType"] = p.agentType;
        j["model"] = p.model;
        std::string text = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
        // Separate lock so dashboard reads of the in-memory history never wait on disk I/O.
        std::lock_guard<std::mutex> fileLock(g_usageFileMutex);
        std::ofstream out(UsageHistoryPathWide(), std::ios::app);
        if (out) out << text;
    }
}

void SetUsageHistoryPersistence(bool enabled) {
    std::lock_guard<std::mutex> lock(g_usageHistoryMutex);
    g_usageHistoryPersist = enabled;
    if (enabled) {
        g_usageHistoryLoaded = false;
        LoadUsageHistoryLocked();
    }
}

bool IsUsageHistoryPersistence() {
    std::lock_guard<std::mutex> lock(g_usageHistoryMutex);
    return g_usageHistoryPersist;
}

uint64_t GetLlmUsageRecordCalls() { return g_recordCalls; }

// Event retention for every stream tracker (see llm_stream.h).
static std::atomic<int64_t> g_llmMaxEvents{kDefaultLlmMaxEvents};

int64_t ClampLlmMaxEvents(int64_t requested) {
    if (requested == -1) return -1;
    if (requested < kMinLlmMaxEvents) return kMinLlmMaxEvents;
    if (requested > kMaxLlmMaxEvents) return kMaxLlmMaxEvents;
    return requested;
}

void SetLlmMaxEvents(int64_t requested) { g_llmMaxEvents.store(ClampLlmMaxEvents(requested)); }
int64_t GetLlmMaxEvents() { return g_llmMaxEvents.load(); }
std::vector<LlmUsagePoint> GetLlmUsageHistory(int64_t sinceEpochSec) {
    std::lock_guard<std::mutex> lock(g_usageHistoryMutex);
    LoadUsageHistoryLocked();
    std::vector<LlmUsagePoint> out;
    out.reserve(g_usageHistory.size());
    for (const auto& p : g_usageHistory) {
        if (p.epochSec >= sinceEpochSec) out.push_back(p);
    }
    return out;
}

static std::string ReadAgyModelFromTranscript(const std::string& conversationId) {
    if (conversationId.empty()) return "";
    const char* home = std::getenv("USERPROFILE");
    if (!home) home = std::getenv("HOME");
    if (!home) return "";
    std::string path = std::string(home) + "/.gemini/antigravity-cli/brain/" + conversationId +
                      "/.system_generated/logs/chunks/transcript/00000000.jsonl";
    std::ifstream f(path);
    if (!f) return "";
    std::string line;
    while (std::getline(f, line)) {
        size_t pos = line.find("Model Selection");
        if (pos == std::string::npos) continue;
        size_t fromPos = line.find("from ", pos);
        if (fromPos == std::string::npos) continue;
        size_t toPos = line.find(" to ", fromPos);
        if (toPos == std::string::npos) continue;
        size_t start = toPos + 4;
        // find ". " after start
        size_t end = line.find(". ", start);
        if (end == std::string::npos) end = line.size();
        std::string model = line.substr(start, end - start);
        // trim
        while (!model.empty() && model.back() == ' ') model.pop_back();
        if (!model.empty()) {
            return model;
        }
    }
    return "";
}

static std::string MatchAgentKeyword(const std::string& lowerText) {
    if (WordMatches(lowerText, "claude")) return "claude";
    if (WordMatches(lowerText, "agy") || WordMatches(lowerText, "antigravity")) return "agy";
    if (WordMatches(lowerText, "opencode")) return "opencode";
    if (WordMatches(lowerText, "codex")) return "codex";
    if (WordMatches(lowerText, "aider")) return "aider";
    if (WordMatches(lowerText, "ollama")) return "ollama";
    return "";
}

// Splits a command line on whitespace, honoring double quotes.
static std::vector<std::string> SplitCommandLine(const std::string& cmd) {
    std::vector<std::string> out;
    std::string cur;
    bool inQuote = false;
    bool hasToken = false;
    for (char c : cmd) {
        if (c == '"') {
            inQuote = !inQuote;
            hasToken = true;
        } else if (!inQuote && (c == ' ' || c == '\t')) {
            if (hasToken) out.push_back(cur);
            cur.clear();
            hasToken = false;
        } else {
            cur += c;
            hasToken = true;
        }
    }
    if (hasToken) out.push_back(cur);
    return out;
}

// Launchers/shells that precede the real agent executable or script.
static bool IsLauncherToken(const std::string& lowerToken) {
    std::string base = lowerToken;
    size_t slash = base.find_last_of("\\/");
    if (slash != std::string::npos) base = base.substr(slash + 1);
    size_t dot = base.rfind('.');
    if (dot != std::string::npos) {
        std::string ext = base.substr(dot);
        if (ext == ".exe" || ext == ".cmd" || ext == ".bat") base = base.substr(0, dot);
    }
    static const char* kLaunchers[] = {
        "node", "nodejs", "npx", "npm", "pnpm", "pnpx", "yarn", "bun", "bunx", "deno",
        "cmd", "powershell", "pwsh", "bash", "sh", "wsl", "start", "call",
        "python", "python3", "py", "uv", "uvx", "pipx",
        "exec", "run", "dlx", "x", "tool",
    };
    for (const char* l : kLaunchers) {
        if (base == l) return true;
    }
    return false;
}

std::string DetectAgentType(const std::string& commandLine, const std::string& name) {
    std::string lcmd = ToLowerStr(commandLine);

    // Prefer the executable (or the script run by a launcher) so that model names in
    // later arguments, e.g. "opencode run -m anthropic/claude-sonnet-4", do not win.
    for (const std::string& tok : SplitCommandLine(lcmd)) {
        if (tok.empty() || tok[0] == '-') continue;
        // Shell switches such as "cmd /c" (but not absolute paths like "/usr/bin/claude").
        if (tok[0] == '/' && tok.find('/', 1) == std::string::npos) continue;
        if (IsLauncherToken(tok)) continue;
        std::string t = MatchAgentKeyword(tok);
        if (!t.empty()) return t;
        break;
    }

    std::string t = MatchAgentKeyword(ToLowerStr(name));
    if (!t.empty()) return t;
    return MatchAgentKeyword(lcmd);
}

namespace llm {

void ApplyLlmUsageFields(const nlohmann::json& u, LlmStreamEvent& outEvent) {
    if (!u.is_object()) return;
    uint64_t in = JsonU64(u, "input_tokens");
    if (!in) in = JsonU64(u, "prompt_tokens");
    if (!in) in = JsonU64(u, "input");
    uint64_t out = JsonU64(u, "output_tokens");
    if (!out) out = JsonU64(u, "completion_tokens");
    if (!out) out = JsonU64(u, "output");
    uint64_t total = JsonU64(u, "total_tokens");
    if (!total) total = JsonU64(u, "total");
    uint64_t cacheRead = JsonU64(u, "cache_read_input_tokens");
    if (!cacheRead) cacheRead = JsonU64(u, "cache_read_tokens");
    if (!cacheRead) cacheRead = JsonU64(u, "cache_read");
    uint64_t cacheWrite = JsonU64(u, "cache_creation_input_tokens");
    if (u.contains("cache") && u["cache"].is_object()) {
        if (!cacheRead) cacheRead = JsonU64(u["cache"], "read");
        if (!cacheWrite) cacheWrite = JsonU64(u["cache"], "write");
    }
    uint64_t thinking = JsonU64(u, "reasoning_tokens");
    if (!thinking) thinking = JsonU64(u, "thinking_tokens");
    if (!thinking) thinking = JsonU64(u, "reasoning");
    if (u.contains("completion_tokens_details") && u["completion_tokens_details"].is_object()) {
        if (!thinking) thinking = JsonU64(u["completion_tokens_details"], "reasoning_tokens");
    }
    if (u.contains("output_tokens_details") && u["output_tokens_details"].is_object()) {
        if (!thinking) thinking = JsonU64(u["output_tokens_details"], "reasoning_tokens");
        if (!thinking) thinking = JsonU64(u["output_tokens_details"], "thinking_tokens");
    }
    if (u.contains("details") && u["details"].is_object()) {
        if (!thinking) thinking = JsonU64(u["details"], "reasoning_tokens");
    }
    if (in) outEvent.inputTokens = in;
    if (out) outEvent.outputTokens = out;
    if (thinking) outEvent.thinkingTokens = thinking;
    if (cacheRead) outEvent.cacheReadTokens = cacheRead;
    if (cacheWrite) outEvent.cacheWriteTokens = cacheWrite;
    if (total) outEvent.totalTokens = total;
    else if (in || out) outEvent.totalTokens = in + out + cacheRead + cacheWrite;
    if (u.contains("cost_usd") && u["cost_usd"].is_number()) outEvent.costUsd = u["cost_usd"].get<double>();
    else if (u.contains("cost") && u["cost"].is_number()) outEvent.costUsd = u["cost"].get<double>();
    else if (u.contains("total_cost") && u["total_cost"].is_number()) outEvent.costUsd = u["total_cost"].get<double>();
}

}  // namespace llm

// Generic parser: any agent's line whose top-level fields look recognisable.
static bool ParseGenericEvent(const nlohmann::json& j, const std::string& trimmed, const std::string& defaultAgentType,
                              LlmStreamEvent& outEvent) {
    outEvent = LlmStreamEvent{};
    outEvent.rawJson = trimmed;
    outEvent.agentType = defaultAgentType.empty() ? "generic" : defaultAgentType;

    auto getStr = [&](const nlohmann::json& obj, const std::string& key) -> std::string {
        if (obj.contains(key)) {
            if (obj[key].is_string()) return obj[key].get<std::string>();
            if (obj[key].is_number() || obj[key].is_boolean()) return obj[key].dump();
        }
        return "";
    };

    // Some agents wrap the interesting payload in "part" / "properties.part".
    const nlohmann::json* partObj = nullptr;
    if (j.contains("part") && j["part"].is_object()) {
        partObj = &j["part"];
    } else if (j.contains("properties") && j["properties"].is_object() &&
               j["properties"].contains("part") && j["properties"]["part"].is_object()) {
        partObj = &j["properties"]["part"];
    } else if (j.contains("content_block") && j["content_block"].is_object()) {
        partObj = &j["content_block"];
    }
    const nlohmann::json* partState = nullptr;
    if (partObj && partObj->contains("state") && (*partObj)["state"].is_object()) {
        partState = &(*partObj)["state"];
    }

    auto getU64 = [](const nlohmann::json& obj, const char* key) -> uint64_t {
        if (obj.contains(key) && obj[key].is_number()) return obj[key].get<uint64_t>();
        return 0;
    };
    auto applyUsage = [&](const nlohmann::json& u) { llm::ApplyLlmUsageFields(u, outEvent); };

    // Usage can be top-level, nested in message/part/tokens, or under properties.
    if (j.contains("usage")) applyUsage(j["usage"]);
    if (j.contains("tokens") && j["tokens"].is_object()) applyUsage(j["tokens"]);
    if (j.contains("message") && j["message"].is_object() && j["message"].contains("usage")) {
        applyUsage(j["message"]["usage"]);
    }
    if (j.contains("properties") && j["properties"].is_object()) {
        const auto& props = j["properties"];
        if (props.contains("usage")) applyUsage(props["usage"]);
        if (props.contains("part") && props["part"].is_object()) {
            if (props["part"].contains("tokens")) applyUsage(props["part"]["tokens"]);
            if (props["part"].contains("cost") && props["part"]["cost"].is_number()) {
                outEvent.costUsd = props["part"]["cost"].get<double>();
            }
        }
    }
    if (partObj) {
        if (partObj->contains("tokens")) applyUsage((*partObj)["tokens"]);
        if (partObj->contains("cost") && (*partObj)["cost"].is_number()) {
            outEvent.costUsd = (*partObj)["cost"].get<double>();
        }
    }

    // Shared post-processing so Tool / Action / Content are populated for every event.
    auto finish = [&]() -> bool {
        if (outEvent.type == "tool_call" || outEvent.type == "tool_result") {
            if (outEvent.toolName.empty()) {
                std::string n = getStr(j, "tool");
                if (n.empty()) n = getStr(j, "name");
                if (n.empty()) n = getStr(j, "tool_name");
                if (n.empty()) n = getStr(j, "toolName");
                if (n.empty() && j.contains("step_update") && j["step_update"].is_object()) {
                    n = getStr(j["step_update"], "tool_name");
                    if (n.empty() && j["step_update"].contains("tool_info") && j["step_update"]["tool_info"].is_object()) {
                        n = getStr(j["step_update"]["tool_info"], "name");
                    }
                }
                if (n.empty() && partObj) {
                    n = getStr(*partObj, "tool");
                    if (n.empty()) n = getStr(*partObj, "name");
                    if (n.empty()) n = getStr(*partObj, "toolName");
                }
                if (!n.empty()) outEvent.toolName = n;
            }
            if (outEvent.toolInput.empty() && partState) {
                if (partState->contains("input")) {
                    outEvent.toolInput = (*partState)["input"].is_string()
                        ? (*partState)["input"].get<std::string>() : (*partState)["input"].dump();
                } else if (partState->contains("output")) {
                    outEvent.toolInput = (*partState)["output"].is_string()
                        ? (*partState)["output"].get<std::string>() : (*partState)["output"].dump();
                }
            }
        }
        if (outEvent.toolAction.empty()) {
            if (outEvent.type == "tool_call" && !outEvent.toolName.empty()) {
                outEvent.toolAction = "Calling " + outEvent.toolName;
            } else if (outEvent.type == "tool_result") {
                outEvent.toolAction = outEvent.toolName.empty() ? "Result" : ("Result: " + outEvent.toolName);
            } else if (outEvent.type == "thinking") {
                outEvent.toolAction = "Thinking";
            } else if (outEvent.type == "message") {
                outEvent.toolAction = "Message";
            }
        }
        if (outEvent.content.empty()) {
            if (!outEvent.toolInput.empty()) outEvent.content = outEvent.toolInput;
            else if (!outEvent.toolAction.empty()) outEvent.content = outEvent.toolAction;
        }
        if (outEvent.type == "usage" && outEvent.content.empty()) {
            outEvent.content = LlmEventUsageText(outEvent);
        }
        return true;
    };

    if (j.contains("agent") && j["agent"].is_string()) {
        outEvent.agentType = j["agent"].get<std::string>();
    }
    if (j.contains("model") && j["model"].is_string()) {
        outEvent.model = j["model"].get<std::string>();
    } else if (j.contains("modelId") && j["modelId"].is_string()) {
        outEvent.model = j["modelId"].get<std::string>();
    } else if (j.contains("model_name") && j["model_name"].is_string()) {
        outEvent.model = j["model_name"].get<std::string>();
    } else if (j.contains("llm") && j["llm"].is_object()) {
        if (j["llm"].contains("model") && j["llm"]["model"].is_string()) outEvent.model = j["llm"]["model"].get<std::string>();
        else if (j["llm"].contains("name") && j["llm"]["name"].is_string()) outEvent.model = j["llm"]["name"].get<std::string>();
    } else if (j.contains("metadata") && j["metadata"].is_object()) {
        if (j["metadata"].contains("model") && j["metadata"]["model"].is_string()) outEvent.model = j["metadata"]["model"].get<std::string>();
    }

    // Claude repeats the same message.usage on every content-block event of one message.
    if (j.contains("message") && j["message"].is_object()) {
        outEvent.messageId = getStr(j["message"], "id");
    }

    outEvent.sessionId = getStr(j, "session_id");
    if (outEvent.sessionId.empty()) outEvent.sessionId = getStr(j, "sessionID");
    outEvent.stopReason = getStr(j, "stop_reason");

    // Step index
    if (j.contains("step_index") && j["step_index"].is_number()) {
        outEvent.stepIndex = j["step_index"].get<int>();
    } else if (j.contains("step") && j["step"].is_number()) {
        outEvent.stepIndex = j["step"].get<int>();
    }

    // Timestamp
    outEvent.timestamp = getStr(j, "timestamp");
    if (outEvent.timestamp.empty()) outEvent.timestamp = getStr(j, "created_at");
    if (outEvent.timestamp.empty()) outEvent.timestamp = getStr(j, "time");
    if (outEvent.timestamp.empty()) outEvent.timestamp = GetCurrentTimestampStr();

    std::string rawType = getStr(j, "type");
    if (rawType.empty()) rawType = getStr(j, "event");
    if (rawType.empty()) rawType = getStr(j, "role");
    std::string lType = ToLowerStr(rawType);

    if (llm::ParseAgyLine(j, outEvent)) return finish();

    // 1. Tool Call / Tool Use (agy, OpenCode, Codex, others)
    std::string partType = partObj ? ToLowerStr(getStr(*partObj, "type")) : "";
    std::string partStatus = partState ? ToLowerStr(getStr(*partState, "status")) : "";
    bool partIsTool = (partType == "tool_use" || partType == "tool" || partType == "function_call");
    bool partToolFinished = (partStatus == "completed" || partStatus == "error" || partStatus == "failed");
    bool partIsToolUse = partIsTool && !partToolFinished;
    if (rawType == "PLANNER_RESPONSE" || j.contains("tool_calls") || partIsToolUse) {
        outEvent.type = "tool_call";
        outEvent.status = "calling_tool";
        if (j.contains("thinking") && j["thinking"].is_string()) {
            outEvent.content = j["thinking"].get<std::string>();
        }
        if (j.contains("tool_calls") && j["tool_calls"].is_array() && !j["tool_calls"].empty()) {
            const auto& tc = j["tool_calls"][0];
            if (tc.is_object()) {
                outEvent.toolName = getStr(tc, "toolName");
                if (outEvent.toolName.empty()) outEvent.toolName = getStr(tc, "name");
                if (outEvent.toolName.empty() && tc.contains("function")) {
                    outEvent.toolName = getStr(tc["function"], "name");
                }
                outEvent.toolAction = getStr(tc, "toolAction");
                if (outEvent.toolAction.empty()) outEvent.toolAction = getStr(tc, "toolSummary");
                if (outEvent.toolAction.empty()) outEvent.toolAction = getStr(tc, "action");
                if (tc.contains("parameters")) outEvent.toolInput = tc["parameters"].dump();
                else if (tc.contains("arguments")) outEvent.toolInput = tc["arguments"].dump();
                else if (tc.contains("input")) outEvent.toolInput = tc["input"].dump();
            }
        }
        if (outEvent.toolAction.empty()) outEvent.toolAction = outEvent.toolName;
        return finish();
    }

    if (lType == "tool_use" || lType == "tool_call" || lType == "call" ||
        lType == "function_call" || j.contains("tool_call")) {
        outEvent.type = "tool_call";
        outEvent.status = "calling_tool";

        const nlohmann::json* targetObj = &j;
        if (j.contains("tool_call") && j["tool_call"].is_object()) {
            targetObj = &j["tool_call"];
        }

        outEvent.toolName = getStr(*targetObj, "name");
        if (outEvent.toolName.empty()) outEvent.toolName = getStr(*targetObj, "tool");
        if (outEvent.toolName.empty()) outEvent.toolName = getStr(*targetObj, "tool_name");
        if (outEvent.toolName.empty() && targetObj->contains("function") && (*targetObj)["function"].is_object()) {
            outEvent.toolName = getStr((*targetObj)["function"], "name");
        }

        outEvent.toolAction = getStr(*targetObj, "toolAction");
        if (outEvent.toolAction.empty()) outEvent.toolAction = getStr(*targetObj, "action");
        if (outEvent.toolAction.empty()) outEvent.toolAction = getStr(*targetObj, "summary");

        if (targetObj->contains("input")) {
            outEvent.toolInput = (*targetObj)["input"].is_string() ? (*targetObj)["input"].get<std::string>() : (*targetObj)["input"].dump();
        } else if (targetObj->contains("arguments")) {
            outEvent.toolInput = (*targetObj)["arguments"].is_string() ? (*targetObj)["arguments"].get<std::string>() : (*targetObj)["arguments"].dump();
        } else if (targetObj->contains("parameters")) {
            outEvent.toolInput = (*targetObj)["parameters"].is_string() ? (*targetObj)["parameters"].get<std::string>() : (*targetObj)["parameters"].dump();
        }

        if (outEvent.toolAction.empty() && !outEvent.toolName.empty()) {
            outEvent.toolAction = "Calling " + outEvent.toolName;
        }
        return finish();
    }

    // 2. Tool Result
    if (lType == "tool_result" || lType == "function_result" || lType == "tool_response" ||
        partType == "tool_result" || (partIsTool && partToolFinished)) {
        outEvent.type = "tool_result";
        outEvent.status = "tool_result";
        outEvent.toolName = getStr(j, "name");
        if (outEvent.toolName.empty()) outEvent.toolName = getStr(j, "tool");
        if (j.contains("content")) {
            outEvent.content = j["content"].is_string() ? j["content"].get<std::string>() : j["content"].dump();
        } else if (j.contains("output")) {
            outEvent.content = j["output"].is_string() ? j["output"].get<std::string>() : j["output"].dump();
        } else if (j.contains("result")) {
            outEvent.content = j["result"].is_string() ? j["result"].get<std::string>() : j["result"].dump();
        } else if (partState && partState->contains("output")) {
            outEvent.content = (*partState)["output"].is_string()
                ? (*partState)["output"].get<std::string>() : (*partState)["output"].dump();
        }
        return finish();
    }

    // 3. Thinking / Chain-of-Thought (Claude, agy, OpenAI o1/o3/codex reasoning)
    if (lType == "thinking" || lType == "thought" || lType == "reasoning" || j.contains("thinking") || j.contains("thought")) {
        outEvent.type = "thinking";
        outEvent.status = "thinking";
        outEvent.content = getStr(j, "thinking");
        if (outEvent.content.empty()) outEvent.content = getStr(j, "thought");
        if (outEvent.content.empty()) outEvent.content = getStr(j, "reasoning");
        if (outEvent.content.empty()) outEvent.content = getStr(j, "message");
        if (outEvent.content.empty()) outEvent.content = getStr(j, "content");
        return finish();
    }

    // 4. Message / Assistant response / Delta
    if (lType == "message" || lType == "assistant" || lType == "content_block_delta" ||
        lType == "delta" || lType == "user" || lType == "user_input" || j.contains("message")) {
        outEvent.type = (lType == "user" || lType == "user_input") ? "user_input" : "message";
        outEvent.status = "generating";
        if (j.contains("delta") && j["delta"].is_object()) {
            outEvent.content = getStr(j["delta"], "content");
            if (outEvent.content.empty()) outEvent.content = getStr(j["delta"], "text");
        } else {
            outEvent.content = getStr(j, "content");
            if (outEvent.content.empty()) outEvent.content = getStr(j, "message");
            if (outEvent.content.empty()) outEvent.content = getStr(j, "text");
        }
        return finish();
    }

    // Result / Done. Checked before Status and Usage because a final result line can carry both
    // "usage" and a total cost. Claude's result line is handled in llm_stream_claude.cpp.
    if (lType == "result" || lType == "done" || lType == "finish" || lType == "completed") {
        bool isError = j.contains("is_error") && j["is_error"].is_boolean() && j["is_error"].get<bool>();
        outEvent.type = isError ? "error" : "result";
        outEvent.status = isError ? "error" : "completed";
        outEvent.content = getStr(j, "summary");
        if (outEvent.content.empty()) outEvent.content = getStr(j, "result");
        if (j.contains("total_cost_usd") && j["total_cost_usd"].is_number()) {
            outEvent.costUsd = j["total_cost_usd"].get<double>();
        }
        return finish();
    }

    // 5. Status / Progress
    if (lType == "status" || lType == "progress" || lType == "step" || lType == "step_start" || lType == "step_finish" || j.contains("status")) {
        outEvent.type = (lType.find("step") != std::string::npos) ? "step" : "status";
        std::string st = getStr(j, "status");
        if (st.empty()) st = getStr(j, "state");
        outEvent.status = st.empty() ? "running" : st;
        std::string summary = getStr(j, "summary");
        if (summary.empty()) summary = getStr(j, "action");
        if (summary.empty()) summary = getStr(j, "message");
        if (summary.empty()) summary = getStr(j, "description");
        outEvent.content = summary;
        return finish();
    }

    // 6. Cost / Usage
    if (lType == "cost" || lType == "usage" || j.contains("usage") || j.contains("total_tokens")) {
        outEvent.type = "usage";
        outEvent.status = "idle";
        const nlohmann::json* u = j.contains("usage") ? &j["usage"] : &j;
        if (u->contains("total_tokens") && (*u)["total_tokens"].is_number()) {
            outEvent.totalTokens = (*u)["total_tokens"].get<uint64_t>();
        } else if (u->contains("tokens") && (*u)["tokens"].is_number()) {
            outEvent.totalTokens = (*u)["tokens"].get<uint64_t>();
        }
        if (u->contains("cost_usd") && (*u)["cost_usd"].is_number()) {
            outEvent.costUsd = (*u)["cost_usd"].get<double>();
        }
        return finish();
    }

    // 7. Error
    if (lType == "error" || j.contains("error")) {
        outEvent.type = "error";
        outEvent.status = "error";
        if (j["error"].is_string()) {
            outEvent.content = j["error"].get<std::string>();
        } else if (j["error"].is_object()) {
            outEvent.content = getStr(j["error"], "message");
            if (outEvent.content.empty()) outEvent.content = j["error"].dump();
        }
        return finish();
    }

    // Generic fallback for any valid JSON object
    outEvent.type = rawType.empty() ? "event" : rawType;
    outEvent.content = getStr(j, "content");
    if (outEvent.content.empty()) outEvent.content = getStr(j, "summary");
    if (outEvent.content.empty()) outEvent.content = getStr(j, "text");
    outEvent.status = "running";
    return finish();
}


bool ParseLlmStreamLine(const std::string& line, const std::string& defaultAgentType, LlmStreamEvent& outEvent) {
    std::string trimmed = TrimStr(line);
    if (trimmed.size() < 2 || trimmed.front() != '{' || trimmed.back() != '}') return false;
    nlohmann::json j = nlohmann::json::parse(trimmed, nullptr, false);
    if (!j.is_object()) return false;
    return ParseGenericEvent(j, trimmed, defaultAgentType, outEvent);
}

bool ParseLlmStreamLines(const std::string& line, const std::string& defaultAgentType, std::vector<LlmStreamEvent>& outEvents) {
    std::string trimmed = TrimStr(line);
    if (trimmed.size() < 2 || trimmed.front() != '{' || trimmed.back() != '}') return false;
    nlohmann::json j = nlohmann::json::parse(trimmed, nullptr, false);
    if (!j.is_object()) return false;

    // Per-agent splitters first. Each returns false for lines that are not its format.
    if (defaultAgentType == "claude" && llm::ParseClaudeLines(j, trimmed, defaultAgentType, outEvents)) return true;
    if (defaultAgentType == "opencode" && llm::ParseOpenCodeLines(j, trimmed, defaultAgentType, outEvents)) return true;
    if (defaultAgentType == "codex" && llm::ParseCodexLines(j, trimmed, defaultAgentType, outEvents)) return true;

    LlmStreamEvent ev;
    if (!ParseGenericEvent(j, trimmed, defaultAgentType, ev)) return false;
    outEvents.push_back(std::move(ev));
    return true;
}

std::string LlmEventUsageText(const LlmStreamEvent& ev) {
    std::ostringstream o;
    auto sep = [&]() { if (!o.str().empty()) o << " / "; };
    auto addCount = [&](const char* label, uint64_t v) {
        if (v == 0) return;
        sep();
        o << label << " " << v;
    };
    addCount("in", ev.inputTokens);
    addCount("out", ev.outputTokens);
    addCount("think", ev.thinkingTokens);
    if (ev.inputTokens == 0 && ev.outputTokens == 0 && ev.thinkingTokens == 0) addCount("tok", ev.totalTokens);
    addCount("cacheR", ev.cacheReadTokens);
    addCount("cacheW", ev.cacheWriteTokens);
    if (ev.costUsd > 0.0) {
        sep();
        char buf[32];
        snprintf(buf, sizeof(buf), "$%.4f", ev.costUsd);
        o << buf;
    }
    return o.str();
}

static std::string ActivityKindForTool(const std::string& toolName) {
    std::string n = ToLowerStr(toolName);
    auto has = [&](const char* w) { return n.find(w) != std::string::npos; };
    if (has("web") || has("fetch") || has("browser") || has("url")) return "web";
    if (n == "task" || has("agent")) return "subagent";
    if (has("todo") || has("plan")) return "plan";
    if (has("edit") || has("write") || has("patch") || has("replace") || has("create_file") || has("insert")) return "edit";
    if (has("grep") || has("glob") || has("search") || has("find") || n == "ls" || has("list_dir") || has("list_files")) return "search";
    if (has("read") || has("view") || has("open_file") || n == "cat") return "read";
    if (has("bash") || has("shell") || has("powershell") || has("command") || has("exec") || has("terminal") || n == "run") return "command";
    return "tool";
}

static std::string ActivityTargetFromInput(const std::string& kind, const std::string& input) {
    if (input.empty()) return "";
    auto j = nlohmann::json::parse(input, nullptr, false);
    if (!j.is_object()) return input;
    static const char* const kFileKeys[] = {"file_path", "filePath", "path", "AbsolutePath", "TargetFile", "file", "filename", "notebook_path"};
    static const char* const kCommandKeys[] = {"command", "CommandLine", "cmd", "script"};
    static const char* const kSearchKeys[] = {"pattern", "query", "Query", "regex", "glob", "path", "SearchPath"};
    static const char* const kWebKeys[] = {"url", "Url", "query", "Query"};
    static const char* const kAgentKeys[] = {"description", "prompt", "subagent_type"};
    auto first = [&](const char* const* keys, size_t count) -> std::string {
        for (size_t i = 0; i < count; ++i) {
            auto it = j.find(keys[i]);
            if (it != j.end() && it->is_string() && !it->get<std::string>().empty()) return it->get<std::string>();
        }
        return "";
    };
    std::string t;
    if (kind == "plan") {
        // Claude TodoWrite: {"todos":[{"content":..,"status":"pending|in_progress|completed"}]}
        auto todos = j.find("todos");
        if (todos != j.end() && todos->is_array()) {
            int done = 0;
            std::string current;
            auto str = [](const nlohmann::json& o, const char* key) -> std::string {
                auto it = o.find(key);
                return (it != o.end() && it->is_string()) ? it->get<std::string>() : std::string();
            };
            for (const auto& td : *todos) {
                if (!td.is_object()) continue;
                std::string st = str(td, "status");
                if (st == "completed") ++done;
                else if (st == "in_progress" && current.empty()) {
                    current = str(td, "activeForm");
                    if (current.empty()) current = str(td, "content");
                }
            }
            t = std::to_string(done) + "/" + std::to_string(todos->size()) + " done";
            if (!current.empty()) t += ": " + current;
            return t;
        }
    }
    if (kind == "command") t = first(kCommandKeys, std::size(kCommandKeys));
    else if (kind == "search") t = first(kSearchKeys, std::size(kSearchKeys));
    else if (kind == "web") t = first(kWebKeys, std::size(kWebKeys));
    else if (kind == "subagent") t = first(kAgentKeys, std::size(kAgentKeys));
    else t = first(kFileKeys, std::size(kFileKeys));
    if (t.empty()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            if (it->is_string() && !it->get<std::string>().empty()) { t = it->get<std::string>(); break; }
        }
    }
    return t;
}

std::string DescribeLlmNote(const LlmStreamEvent& ev) {
    if (ev.type != "tool_call") return "";
    // A subagent's target already is its description.
    if (ActivityKindForTool(ev.toolName) == "subagent") return "";
    auto j = nlohmann::json::parse(ev.toolInput, nullptr, false);
    if (!j.is_object()) return "";
    auto it = j.find("description");
    if (it == j.end() || !it->is_string()) return "";
    return it->get<std::string>();
}

bool DescribeLlmActivity(const LlmStreamEvent& ev, std::string& kind, std::string& target) {
    kind.clear();
    target.clear();
    if (ev.type == "tool_call") {
        kind = ActivityKindForTool(ev.toolName);
        target = ActivityTargetFromInput(kind, ev.toolInput);
        if (kind == "tool") target = ev.toolName + (target.empty() ? "" : " " + target);
    } else if (ev.type == "tool_result") {
        kind = "waiting_model";  // the tool is done; the model works on its result
    } else if (ev.type == "thinking") {
        kind = "thinking";
        target = ev.content;
    } else if (ev.type == "message") {
        if (ev.content.empty()) return false;  // a reply with no text does not change the current activity
        kind = "replying";
        target = ev.content;
    } else if (ev.type == "error") {
        kind = "error";
        target = ev.content;
    } else if (ev.type == "result") {
        kind = "done";
    } else {
        return false;
    }
    for (char& c : target) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    }
    target = Utf8Prefix(target, 300);
    return true;
}

std::string LlmSubagentsToJson(const std::vector<LlmSubagentState>& subagents) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& s : subagents) {
        nlohmann::json item;
        item["id"] = s.id;
        item["tool_use_id"] = s.toolUseId;
        item["parent_agent_id"] = s.parentAgentId;
        item["subagent_type"] = s.subagentType;
        item["description"] = s.description;
        item["status"] = s.status;
        item["started_epoch_ms"] = s.startedEpochMs;
        item["ended_epoch_ms"] = s.endedEpochMs;
        item["total_tokens"] = s.totalTokens;
        item["tool_uses"] = s.toolUses;
        item["activity"] = s.activity;
        item["activity_target"] = s.activityTarget;
        arr.push_back(item);
    }
    return arr.dump();
}

std::string LlmEventsToJson(const std::vector<LlmStreamEvent>& events, const LlmAgentState& state) {
    nlohmann::json root;
    root["ok"] = true;
    root["agent_type"] = state.agentType;
    root["model"] = state.model;
    root["status"] = state.status;
    root["current_tool"] = state.currentTool;
    root["current_tool_action"] = state.currentToolAction;
    root["activity"] = state.activity;
    root["activity_target"] = state.activityTarget;
    root["activity_since_epoch_ms"] = state.activitySinceEpochMs;
    root["last_thought"] = state.lastThought;
    root["last_message"] = state.lastMessage;
    root["step_count"] = state.stepCount;
    root["event_count"] = state.eventCount;
    root["total_tokens"] = state.totalTokens;
    root["cost_usd"] = state.costUsd;
    root["stream_file"] = state.streamFilePath;
    root["supervisor_notes"] = state.supervisorNotes;
    root["supervisor_goal"] = state.supervisorGoal;
    root["updated_at"] = state.updatedAt;
    root["session_id"] = state.sessionId;
    root["stop_reason"] = state.stopReason;
    root["cwd"] = state.cwd;
    root["tool_calls"] = state.toolCalls;
    root["tool_errors"] = state.toolErrors;
    root["tools_offered"] = state.toolsOffered;
    root["num_turns"] = state.numTurns;
    root["api_duration_ms"] = state.apiDurationMs;
    root["permission_denials"] = state.permissionDenials;
    if (!state.rateLimitType.empty()) {
        root["rate_limit"] = {{"type", state.rateLimitType}, {"status", state.rateLimitStatus},
                              {"utilization", state.rateLimitUtilization},
                              {"resets_at_epoch_sec", state.rateLimitResetsAtSec}};
    }
    if (!state.subagents.empty()) root["subagents"] = nlohmann::json::parse(LlmSubagentsToJson(state.subagents));

    nlohmann::json evArr = nlohmann::json::array();
    for (const auto& ev : events) {
        nlohmann::json item;
        item["seq"] = ev.seq;
        item["timestamp"] = ev.timestamp;
        item["type"] = ev.type;
        item["agent_type"] = ev.agentType;
        item["model"] = ev.model;
        item["tool_name"] = ev.toolName;
        item["tool_action"] = ev.toolAction;
        item["tool_input"] = ev.toolInput;
        item["content"] = ev.content;
        item["status"] = ev.status;
        item["step"] = ev.stepIndex;
        if (!ev.toolUseId.empty()) item["tool_use_id"] = ev.toolUseId;
        if (!ev.parentToolUseId.empty()) item["parent_tool_use_id"] = ev.parentToolUseId;
        if (!ev.agentId.empty()) item["agent_id"] = ev.agentId;
        if (!ev.subagentType.empty()) item["subagent_type"] = ev.subagentType;
        if (!ev.taskDescription.empty()) item["task_description"] = ev.taskDescription;
        if (!ev.taskStatus.empty()) item["task_status"] = ev.taskStatus;
        if (ev.totalTokens > 0) item["total_tokens"] = ev.totalTokens;
        if (ev.inputTokens > 0) item["input_tokens"] = ev.inputTokens;
        if (ev.outputTokens > 0) item["output_tokens"] = ev.outputTokens;
        if (ev.thinkingTokens > 0) item["thinking_tokens"] = ev.thinkingTokens;
        if (ev.cacheReadTokens > 0) item["cache_read_tokens"] = ev.cacheReadTokens;
        if (ev.cacheWriteTokens > 0) item["cache_write_tokens"] = ev.cacheWriteTokens;
        if (ev.costUsd > 0.0) item["cost_usd"] = ev.costUsd;
        std::string usageText = LlmEventUsageText(ev);
        if (!usageText.empty()) item["usage_text"] = usageText;
        evArr.push_back(item);
    }
    root["events"] = evArr;
    return root.dump();
}

std::string LlmEventsToSummary(const std::vector<LlmStreamEvent>& events, const LlmAgentState& state) {
    std::ostringstream oss;
    oss << "[Agent: " << (state.agentType.empty() ? "LLM" : state.agentType)
        << " | Status: " << state.status;
    if (!state.currentTool.empty()) {
        oss << " | Tool: " << state.currentTool;
    }
    if (!state.supervisorGoal.empty()) {
        oss << " | Goal: " << state.supervisorGoal;
    }
    oss << "]\n";

    for (const auto& ev : events) {
        std::string timeStr = ev.timestamp;
        if (timeStr.size() >= 19) timeStr = timeStr.substr(11, 8); // Extract HH:MM:SS
        oss << "[" << (timeStr.empty() ? "--:--:--" : timeStr) << "] ";

        if (ev.type == "tool_call") {
            oss << "TOOL CALL: " << ev.toolName;
            if (!ev.toolAction.empty() && ev.toolAction != ev.toolName) {
                oss << " (" << ev.toolAction << ")";
            }
        } else if (ev.type == "tool_result") {
            oss << "TOOL RESULT: " << ev.toolName;
            if (!ev.content.empty()) {
                std::string preview = Utf8Prefix(ev.content, 80);
                std::replace(preview.begin(), preview.end(), '\n', ' ');
                oss << " -> " << preview;
            }
        } else if (ev.type == "thinking") {
            std::string preview = Utf8Prefix(ev.content, 100);
            std::replace(preview.begin(), preview.end(), '\n', ' ');
            oss << "THINKING: " << preview;
        } else if (ev.type == "message") {
            std::string preview = Utf8Prefix(ev.content, 120);
            std::replace(preview.begin(), preview.end(), '\n', ' ');
            oss << "MESSAGE: " << preview;
        } else if (ev.type == "error") {
            oss << "ERROR: " << ev.content;
        } else {
            oss << ev.type << ": " << ev.content;
        }
        oss << "\n";
    }
    return oss.str();
}

void LlmStreamTracker::Init(int processId, const std::string& agentType, const std::string& streamFilePath,
                            const std::string& supervisorNotes, const std::string& supervisorGoal,
                            const std::string& defaultModel) {
    std::lock_guard<std::mutex> lock(mutex_);
    processId_ = processId;
    agentType_ = agentType;
    streamFilePath_ = streamFilePath;
    streamFilePathWide_ = Utf8ToWide(streamFilePath);

    state_.agentType = agentType;
    state_.model = defaultModel;
    state_.streamFilePath = streamFilePath;
    state_.supervisorNotes = supervisorNotes;
    state_.supervisorGoal = supervisorGoal;
    state_.updatedAt = GetCurrentTimestampStr();
}

void LlmStreamTracker::UpdateSupervisorInfo(const std::string& notes, const std::string& goal) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!notes.empty()) state_.supervisorNotes = notes;
    if (!goal.empty()) state_.supervisorGoal = goal;
    state_.updatedAt = GetCurrentTimestampStr();
}

void LlmStreamTracker::IngestChunk(const char* data, size_t len) {
    if (!data || len == 0) return;
    std::vector<LlmUsagePoint> usage;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        lineBuffer_.append(data, len);

        size_t start = 0;
        size_t pos = 0;
        while ((pos = lineBuffer_.find('\n', start)) != std::string::npos) {
            if (discardingLine_) {
                // Tail of an oversized line that was already dropped.
                discardingLine_ = false;
            } else {
                ProcessLine(lineBuffer_.substr(start, pos - start));
            }
            start = pos + 1;
        }
        lineBuffer_.erase(0, start);

        // Safety guard against a runaway line without newlines. Splitting it would only
        // yield unparsable fragments, so drop it entirely up to the next newline.
        if (lineBuffer_.size() > kMaxLineBytes) {
            lineBuffer_.clear();
            discardingLine_ = true;
        }
        usage.swap(pendingUsage_);
    }

    // Recorded outside mutex_: it takes the global history lock and may do file I/O,
    // which must not block GetState()/GetEvents() callers.
    int64_t nowEpoch = static_cast<int64_t>(time(nullptr));
    for (const auto& pt : usage) RecordLlmUsagePoint(nowEpoch, pt);
}

void LlmStreamTracker::ProcessLine(const std::string& line) {
    // OpenCode does not emit the model in its JSON stream; recover it from
    // the opencode log lines that are merged into the process output.
    if (agentType_ == "opencode" && state_.model.empty()) {
        auto extractAfter = [&](const std::string& key) -> std::string {
            size_t pos = line.find(key);
            if (pos == std::string::npos) return "";
            size_t after = pos + key.size();
            while (after < line.size() && (line[after] == ' ' || line[after] == '=' || line[after] == '"')) after++;
            size_t end = after;
            while (end < line.size() && line[end] != ' ' && line[end] != '"' && line[end] != '\r' && line[end] != '\n') end++;
            std::string v = line.substr(after, end - after);
            while (!v.empty() && (v.back() == ' ' || v.back() == '"')) v.pop_back();
            return v;
        };
        std::string m = extractAfter("llm.model=");
        if (m.empty()) m = extractAfter("modelID=");
        if (!m.empty()) state_.model = m;
    }
    std::vector<LlmStreamEvent> parsed;
    if (!ParseLlmStreamLines(line, agentType_, parsed)) {
        return;
    }

    // Save line to persistent stream file (.jsonl) once, even when it yields several events.
    // Partial-message chunks are not saved: the complete message follows on its own line.
    bool persist = parsed.empty();
    for (const auto& ev : parsed) persist = persist || !ev.transient;
    if (persist && !streamFilePathWide_.empty()) {
        std::string writeLine = line;
        if (writeLine.empty() || writeLine.back() != '\n') writeLine += "\r\n";
        AppendToLogFileWide(streamFilePathWide_, writeLine);
    }

    for (auto& ev : parsed) ApplyEvent(ev, line);
}

void LlmStreamTracker::ApplyActivity(const LlmStreamEvent& ev) {
    std::string actKind, actTarget;
    if (DescribeLlmActivity(ev, actKind, actTarget)) {
        // Streamed thinking / reply chunks continue one activity; anything else starts a new one.
        bool continues = actKind == state_.activity && (actKind == "thinking" || actKind == "replying");
        if (!continues) state_.activitySinceEpochMs = ev.recvEpochMs;
        state_.activity = actKind;
        state_.activityTarget = actTarget;
    }
}

void LlmStreamTracker::ApplyPartialEvent(LlmStreamEvent& ev) {
    ev.recvEpochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    state_.updatedAt = GetCurrentTimestampStr();
    if (!ev.model.empty()) state_.model = ev.model;

    if (ev.type == "tool_call") {
        state_.status = "calling_tool";
        state_.currentTool = ev.toolName;
        state_.currentToolAction = ev.toolAction;
    } else if (ev.type == "thinking" || ev.type == "message") {
        // Chunks of one block accumulate; a new block starts from empty text.
        if (ev.type != partialTextType_ || ev.status == "block_start") {
            partialTextType_ = ev.type;
            partialText_.clear();
        }
        if (partialText_.size() < kMaxPartialTextBytes) partialText_ += ev.content;
        ev.content = partialText_;
        if (ev.type == "thinking") {
            state_.status = "thinking";
            if (!ev.content.empty()) state_.lastThought = Utf8Prefix(ev.content, 160);
        } else {
            state_.status = "generating";
            if (!ev.content.empty()) state_.lastMessage = Utf8Prefix(ev.content, 160);
        }
    } else {
        return;
    }
    ApplyActivity(ev);
}

LlmStreamTracker::UsageBaseline& LlmStreamTracker::MessageBaselineLocked(const std::string& messageId) {
    auto it = messageBaselines_.find(messageId);
    if (it != messageBaselines_.end()) return it->second;
    // Bounded by kMaxTrackedMessages: forget the oldest message ids first.
    while (messageBaselineOrder_.size() >= kMaxTrackedMessages) {
        messageBaselines_.erase(messageBaselineOrder_.front());
        messageBaselineOrder_.pop_front();
    }
    messageBaselineOrder_.push_back(messageId);
    return messageBaselines_[messageId];
}

LlmSubagentState& LlmStreamTracker::SubagentEntryLocked(const std::string& agentId, const std::string& toolUseId,
                                                        int64_t nowMs) {
    for (auto& sub : state_.subagents) {
        if ((!agentId.empty() && sub.id == agentId) || (!toolUseId.empty() && sub.toolUseId == toolUseId)) return sub;
    }
    // Bounded by kMaxSubagents (large): past it, the oldest finished sub-agent goes first.
    if (state_.subagents.size() >= kMaxSubagents) {
        auto finished = std::find_if(state_.subagents.begin(), state_.subagents.end(),
                                     [](const LlmSubagentState& s) { return s.status != "running"; });
        state_.subagents.erase(finished != state_.subagents.end() ? finished : state_.subagents.begin());
    }
    LlmSubagentState sub;
    sub.id = agentId.empty() ? toolUseId : agentId;
    sub.toolUseId = toolUseId;
    sub.parentAgentId = ParentAgentIdLocked(toolUseId);
    sub.startedEpochMs = nowMs;
    state_.subagents.push_back(sub);
    return state_.subagents.back();
}

bool LlmStreamTracker::AnySubagentRunningLocked() const {
    for (const auto& sub : state_.subagents) {
        if (sub.status == "running") return true;
    }
    return false;
}

void LlmStreamTracker::RecordToolUseOwnerLocked(const std::string& toolUseId, const std::string& agentId) {
    if (toolUseId.empty() || toolUseOwners_.count(toolUseId)) return;
    // Bounded by kMaxToolUseOwners: forget the oldest tool_use ids first.
    while (toolUseOwnerOrder_.size() >= kMaxToolUseOwners) {
        toolUseOwners_.erase(toolUseOwnerOrder_.front());
        toolUseOwnerOrder_.pop_front();
    }
    toolUseOwnerOrder_.push_back(toolUseId);
    toolUseOwners_[toolUseId] = agentId;
}

std::string LlmStreamTracker::ParentAgentIdLocked(const std::string& toolUseId) const {
    auto it = toolUseOwners_.find(toolUseId);
    return it == toolUseOwners_.end() ? std::string() : it->second;
}

void LlmStreamTracker::ApplyEvent(LlmStreamEvent& ev, const std::string& line) {
    if (ev.transient) {
        // A sub-agent's chunks do not touch the main agent's live state.
        if (ev.parentToolUseId.empty()) ApplyPartialEvent(ev);
        // Usage chunks still count below; other chunks stop here.
        if (ev.type != "usage") return;
    } else if (ev.type != "usage" && ev.parentToolUseId.empty()) {
        // A complete block replaces the chunks that led up to it.
        partialTextType_.clear();
        partialText_.clear();
    }

    ev.seq = nextSeq_++;
    ev.timestamp = GetCurrentTimestampStr();
    ev.recvEpochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (ev.agentType.empty() || ev.agentType == "generic") {
        if (!agentType_.empty()) ev.agentType = agentType_;
    }

    // A sub-agent's line (Claude: parent_tool_use_id set) updates only that sub-agent's entry and
    // the token totals. Its status, activity, tool and message fields belong to the main agent.
    const bool childLine = !ev.parentToolUseId.empty();
    // The agent that issued a tool_use owns it: a sub-agent started by that tool_use is its child.
    if (ev.type == "tool_call") RecordToolUseOwnerLocked(ev.toolUseId, ev.agentId);

    // Update agent state
    state_.eventCount = static_cast<int>(events_.size()) + 1;
    state_.updatedAt = ev.timestamp;

    if (!ev.agentType.empty() && ev.agentType != "generic") {
        state_.agentType = ev.agentType;
    }
    if (ev.stepIndex > 0) {
        state_.stepCount = ev.stepIndex;
    }
    if (!childLine && !ev.model.empty()) {
        state_.model = ev.model;
    }
    // agy does not report model in stream-json; recover it from the agy transcript file.
    // The transcript is re-read at most every kAgyModelProbeIntervalMs, not on every event.
    if (ev.agentType == "agy" && state_.model.empty()) {
        if (conversationId_.empty()) {
            nlohmann::json j2 = nlohmann::json::parse(line, nullptr, false);
            if (!j2.is_discarded()) {
                if (j2.contains("conversation_id") && j2["conversation_id"].is_string()) {
                    conversationId_ = j2["conversation_id"].get<std::string>();
                } else if (j2.contains("step_update") && j2["step_update"].is_object() &&
                           j2["step_update"].contains("conversation_id") && j2["step_update"]["conversation_id"].is_string()) {
                    conversationId_ = j2["step_update"]["conversation_id"].get<std::string>();
                }
            }
        }
        uint64_t nowMs = GetNowTickMs();
        if (!conversationId_.empty() &&
            (lastAgyModelProbeMs_ == 0 || nowMs - lastAgyModelProbeMs_ >= kAgyModelProbeIntervalMs)) {
            lastAgyModelProbeMs_ = nowMs;
            std::string m = ReadAgyModelFromTranscript(conversationId_);
            if (!m.empty()) state_.model = m;
        }
    }

    uint64_t eventTokens = ev.totalTokens;
    if (eventTokens == 0) eventTokens = ev.inputTokens + ev.outputTokens + ev.thinkingTokens + ev.cacheReadTokens + ev.cacheWriteTokens;
    // agy reports usage per step (each DONE agent_response carries only that step's
    // tokens), so every event is already a delta and totals are their sum.
    // OpenCode step_finish tokens are per model step too (input excludes cache reads, as in Claude).
    const bool perStepUsage = (ev.agentType == "agy" || agentType_ == "agy" ||
                               ev.agentType == "opencode" || agentType_ == "opencode");
    if (perStepUsage && (eventTokens > 0 || ev.costUsd > 0.0)) {
        state_.totalTokens += eventTokens;
        state_.costUsd += ev.costUsd;
        LlmUsagePoint pt;
        pt.inputTokens = ev.inputTokens;
        pt.outputTokens = ev.outputTokens;
        pt.thinkingTokens = ev.thinkingTokens;
        pt.cacheReadTokens = ev.cacheReadTokens;
        pt.cacheWriteTokens = ev.cacheWriteTokens;
        pt.totalTokens = eventTokens;
        pt.costUsd = ev.costUsd;
        pt.agentType = ev.agentType.empty() ? agentType_ : ev.agentType;
        pt.model = ev.model.empty() ? state_.model : ev.model;
        pendingUsage_.push_back(std::move(pt));
    }
    // Claude reports usage per message: its assistant lines repeat the usage from the start of
    // the message, a message_delta chunk (--include-partial-messages) has the final usage, and the
    // "result" event has the run total. Count per message: each message.id keeps its own baselines
    // and only growth within a message counts. The result event then adds whatever the
    // messages did not report. Cost only comes from the result event.
    const bool perMessageUsage = !perStepUsage && (ev.agentType == "claude" || agentType_ == "claude");
    const bool countTokens = !perMessageUsage || !ev.messageId.empty();
    // Other agents' usage values are usually cumulative; keep the high-water mark so
    // totals do not double-count per-response deltas.
    if (!perStepUsage && !perMessageUsage && ev.totalTokens > state_.totalTokens) state_.totalTokens = ev.totalTokens;
    if (!perStepUsage && ev.costUsd > state_.costUsd) state_.costUsd = ev.costUsd;
    if (!perStepUsage && (((countTokens || ev.runTotal) && eventTokens > 0) || ev.costUsd > 0.0)) {
        uint64_t deltaInput = 0, deltaOutput = 0, deltaThinking = 0, deltaCacheRead = 0, deltaCacheWrite = 0, deltaTotal = 0;
        if (perMessageUsage && ev.runTotal) {
            // The run total covers the messages too; count only what they did not report.
            auto missing = [](uint64_t total, uint64_t counted) -> uint64_t { return total > counted ? total - counted : 0; };
            deltaInput = missing(ev.inputTokens, countedInput_);
            deltaOutput = missing(ev.outputTokens, countedOutput_);
            deltaThinking = missing(ev.thinkingTokens, countedThinking_);
            deltaCacheRead = missing(ev.cacheReadTokens, countedCacheRead_);
            deltaCacheWrite = missing(ev.cacheWriteTokens, countedCacheWrite_);
            // Claude's output_tokens already include its thinking tokens.
            deltaTotal = deltaInput + deltaOutput + deltaCacheRead + deltaCacheWrite;
        } else if (countTokens) {
            // Claude: the baseline of this line's message id. Others: one shared baseline.
            UsageBaseline& base = perMessageUsage ? MessageBaselineLocked(ev.messageId) : lastUsage_;
            // Within a message the usage only grows; a lower value repeats an earlier line.
            auto growth = [&](uint64_t value, uint64_t& last) -> uint64_t {
                if (value < last) {
                    if (perMessageUsage) return 0;
                    last = value;
                    return value;
                }
                uint64_t d = value - last;
                last = value;
                return d;
            };
            deltaInput = growth(ev.inputTokens, base.input);
            deltaOutput = growth(ev.outputTokens, base.output);
            deltaThinking = growth(ev.thinkingTokens, base.thinking);
            deltaCacheRead = growth(ev.cacheReadTokens, base.cacheRead);
            deltaCacheWrite = growth(ev.cacheWriteTokens, base.cacheWrite);
            // Claude's output_tokens already include its thinking tokens.
            deltaTotal = deltaInput + deltaOutput + deltaCacheRead + deltaCacheWrite + (perMessageUsage ? 0 : deltaThinking);
            if (!perMessageUsage && deltaTotal == 0 && eventTokens >= base.total) deltaTotal = eventTokens - base.total;
            base.total = eventTokens;
        }
        if (perMessageUsage) {
            state_.totalTokens += deltaTotal;
            countedInput_ += deltaInput;
            countedOutput_ += deltaOutput;
            countedThinking_ += deltaThinking;
            countedCacheRead_ += deltaCacheRead;
            countedCacheWrite_ += deltaCacheWrite;
            if (childLine && !ev.runTotal) {
                LlmSubagentState& sub = SubagentEntryLocked(ev.agentId, ev.parentToolUseId, ev.recvEpochMs);
                sub.totalTokens += deltaTotal;
            }
        }
        double deltaCost = 0.0;
        if (ev.costUsd > 0.0) {
            deltaCost = (ev.costUsd >= lastUsageCost_) ? (ev.costUsd - lastUsageCost_) : ev.costUsd;
            lastUsageCost_ = ev.costUsd;
        }
        if (deltaTotal > 0 || deltaCost > 0.0) {
            LlmUsagePoint pt;
            pt.inputTokens = deltaInput;
            pt.outputTokens = deltaOutput;
            pt.thinkingTokens = deltaThinking;
            pt.cacheReadTokens = deltaCacheRead;
            pt.cacheWriteTokens = deltaCacheWrite;
            pt.totalTokens = deltaTotal;
            pt.costUsd = deltaCost;
            pt.agentType = ev.agentType.empty() ? agentType_ : ev.agentType;
            pt.model = ev.model.empty() ? state_.model : ev.model;
            pendingUsage_.push_back(std::move(pt));
        }
    }

    if (!ev.sessionId.empty()) state_.sessionId = ev.sessionId;
    if (!ev.stopReason.empty() && !childLine) state_.stopReason = ev.stopReason;
    if (!ev.cwd.empty()) state_.cwd = ev.cwd;
    if (ev.toolsOffered > 0) state_.toolsOffered = ev.toolsOffered;
    if (ev.numTurns > 0) state_.numTurns = ev.numTurns;
    if (ev.apiDurationMs > 0) state_.apiDurationMs = ev.apiDurationMs;
    if (ev.permissionDenials > 0) state_.permissionDenials = ev.permissionDenials;
    if (ev.type == "rate_limit") {
        state_.rateLimitType = ev.rateLimitType;
        state_.rateLimitStatus = ev.rateLimitStatus;
        state_.rateLimitUtilization = ev.rateLimitUtilization;
        state_.rateLimitResetsAtSec = ev.rateLimitResetsAtSec;
    }

    // Sub-agent lifecycle: the entry in the list. A sub-agent's own lines update its entry below.
    if (ev.type == "subagent_start") {
        LlmSubagentState& sub = SubagentEntryLocked(ev.agentId, ev.toolUseId, ev.recvEpochMs);
        sub.subagentType = ev.subagentType;
        if (!ev.taskDescription.empty()) sub.description = ev.taskDescription;
        sub.status = "running";
        sub.startedEpochMs = ev.recvEpochMs;
    } else if (ev.type == "subagent_end") {
        LlmSubagentState& sub = SubagentEntryLocked(ev.agentId, ev.toolUseId, ev.recvEpochMs);
        sub.status = ev.taskStatus.empty() ? "completed" : ev.taskStatus;
        sub.endedEpochMs = ev.recvEpochMs;
        if (ev.taskTokens > 0) sub.totalTokens = ev.taskTokens;
        if (ev.taskToolUses > 0) sub.toolUses = static_cast<int>(ev.taskToolUses);
        sub.activity.clear();
        sub.activityTarget.clear();
        // The run's result came first and was waiting for this sub-agent: the run is done now.
        if (resultPending_ && !AnySubagentRunningLocked()) {
            resultPending_ = false;
            state_.status = "completed";
            state_.activity = "done";
            state_.activityTarget.clear();
            state_.activitySinceEpochMs = ev.recvEpochMs;
        }
    }

    if (childLine) {
        // Only the sub-agent's own counts and what it is doing now.
        LlmSubagentState& sub = SubagentEntryLocked(ev.agentId, ev.parentToolUseId, ev.recvEpochMs);
        if (sub.subagentType.empty()) sub.subagentType = ev.subagentType;
        if (sub.description.empty()) sub.description = ev.taskDescription;
        if (ev.type == "tool_call") sub.toolUses++;
        std::string subKind, subTarget;
        if (DescribeLlmActivity(ev, subKind, subTarget)) {
            sub.activity = subKind;
            sub.activityTarget = subTarget;
        }
    } else {
        if (ev.type == "tool_call") {
            state_.status = "calling_tool";
            state_.currentTool = ev.toolName;
            state_.currentToolAction = ev.toolAction;
            state_.toolCalls++;
        } else if (ev.type == "tool_result") {
            state_.status = "tool_finished";
            if (ev.toolError) state_.toolErrors++;
        } else if (ev.type == "thinking") {
            state_.status = "thinking";
            if (!ev.content.empty()) {
                state_.lastThought = Utf8Prefix(ev.content, 160);
            }
        } else if (ev.type == "message") {
            state_.status = "generating";
            if (!ev.content.empty()) {
                state_.lastMessage = Utf8Prefix(ev.content, 160);
            }
        } else if (ev.type == "status") {
            if (!ev.status.empty()) state_.status = ev.status;
        } else if (ev.type == "error") {
            state_.status = "error";
            state_.lastError = ev.content;
        } else if (ev.type == "result") {
            state_.status = "completed";
        }

        ApplyActivity(ev);

        // A result can arrive while a background sub-agent still runs. The run is not done
        // until that sub-agent ends (its subagent_end then completes the run).
        if (ev.type == "result") {
            resultPending_ = AnySubagentRunningLocked();
            if (resultPending_) {
                state_.status = "running";
                state_.activity = "subagent";
                state_.activityTarget.clear();
                for (const auto& sub : state_.subagents) {
                    if (sub.status != "running") continue;
                    state_.activityTarget = sub.description;
                    state_.activitySinceEpochMs = sub.startedEpochMs;
                    break;
                }
            }
        } else if (ev.type != "subagent_start" && ev.type != "subagent_end" && !ev.transient) {
            resultPending_ = false;
        }
    }

    if (ev.transient) return;  // a usage chunk: counted above, not stored
    try {
        events_.push_back(ev);
    } catch (const std::bad_alloc&) {
        // Out of memory while keeping every event (-1 setting). Free the older half first, then log
        // once and store the event again. Nothing is allocated before the old events are gone.
        events_.erase(events_.begin(), events_.begin() + static_cast<std::ptrdiff_t>(events_.size() / 2));
        if (!loggedAllocFailure_) {
            loggedAllocFailure_ = true;
            WriteAuditLog("llm_stream", "out of memory keeping events (retention -1): dropped the older half of "
                                        "one process's events; lower the event retention setting");
        }
        try {
            events_.push_back(ev);
        } catch (const std::bad_alloc&) {
            // Still no memory: this event is not kept, the run goes on.
        }
    }
    const int64_t maxEvents = GetLlmMaxEvents();
    if (maxEvents > 0) {
        while (events_.size() > static_cast<size_t>(maxEvents)) {
            events_.pop_front();
        }
    }
}

LlmAgentState LlmStreamTracker::GetState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

std::vector<LlmStreamEvent> LlmStreamTracker::GetEvents(int tail, const std::string& typeFilter, uint64_t sinceSeq) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<LlmStreamEvent> out;
    out.reserve(events_.size());

    for (const auto& ev : events_) {
        if (ev.seq <= sinceSeq) continue;
        if (!typeFilter.empty() && ev.type != typeFilter) continue;
        out.push_back(ev);
    }

    if (tail > 0 && out.size() > static_cast<size_t>(tail)) {
        out.erase(out.begin(), out.begin() + (out.size() - tail));
    }
    return out;
}

std::string LlmStreamTracker::GetEventsJson(int tail, const std::string& typeFilter, uint64_t sinceSeq) const {
    auto evs = GetEvents(tail, typeFilter, sinceSeq);
    auto st = GetState();
    return LlmEventsToJson(evs, st);
}

std::string LlmStreamTracker::GetEventsSummary(int tail, const std::string& typeFilter) const {
    auto evs = GetEvents(tail, typeFilter, 0);
    auto st = GetState();
    return LlmEventsToSummary(evs, st);
}

std::string LlmStreamTracker::GetEventsRawJsonl(int tail) const {
    auto evs = GetEvents(tail, "", 0);
    std::string out;
    for (const auto& ev : evs) {
        out += ev.rawJson + "\n";
    }
    return out;
}

bool LlmStreamTracker::HasEvents() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !events_.empty();
}

uint64_t LlmStreamTracker::EventCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<uint64_t>(events_.size());
}

} // namespace pc