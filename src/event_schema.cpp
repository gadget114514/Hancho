#include "event_schema.h"
#include "llm_stream.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>

namespace pc {

namespace {

constexpr size_t kMaxTextBytes = 16 * 1024;
constexpr size_t kMaxSummaryBytes = 512;

std::string FileNameOf(const std::string& path) {
    size_t pos = path.find_last_of("\\/");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

// Byte-limited prefix that never splits a UTF-8 multi-byte sequence.
std::string Utf8Prefix(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t n = maxBytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
    return s.substr(0, n);
}

std::string IsoUtcFromEpochMs(int64_t epochMs) {
    if (epochMs <= 0) {
        epochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }
    time_t sec = static_cast<time_t>(epochMs / 1000);
    int millis = static_cast<int>(epochMs % 1000);
    if (millis < 0) millis += 1000;
    std::tm tmv{};
    gmtime_s(&tmv, &sec);
    char buf[40] = {};
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec, millis);
    return buf;
}

void PutText(std::string& out, const std::string& value, bool& truncated) {
    out = Utf8Prefix(value, kMaxTextBytes);
    truncated = out.size() < value.size();
}

nlohmann::json ToolInputJson(const std::string& input) {
    if (input.empty()) return nlohmann::json();
    auto parsed = nlohmann::json::parse(input, nullptr, false);
    if (!parsed.is_discarded()) return parsed;
    return nlohmann::json(input);
}

nlohmann::json PlanItems(const std::string& input) {
    nlohmann::json items = nlohmann::json::array();
    auto parsed = nlohmann::json::parse(input, nullptr, false);
    if (!parsed.is_object()) return items;
    const char* key = parsed.contains("todos") ? "todos" : (parsed.contains("items") ? "items" : nullptr);
    if (!key || !parsed[key].is_array()) return items;
    for (const auto& todo : parsed[key]) {
        if (!todo.is_object()) continue;
        nlohmann::json item;
        if (todo.contains("content") && todo["content"].is_string()) {
            item["content"] = todo["content"].get<std::string>();
        } else if (todo.contains("text") && todo["text"].is_string()) {
            item["content"] = todo["text"].get<std::string>();
        } else {
            item["content"] = "";
        }
        if (todo.contains("status") && todo["status"].is_string()) {
            item["status"] = todo["status"].get<std::string>();
        } else if (todo.contains("completed") && todo["completed"].is_boolean()) {
            item["status"] = todo["completed"].get<bool>() ? "completed" : "pending";
        } else {
            item["status"] = "pending";
        }
        items.push_back(std::move(item));
    }
    return items;
}

bool HasUsage(const LlmStreamEvent& ev) {
    return ev.inputTokens || ev.outputTokens || ev.thinkingTokens || ev.cacheReadTokens ||
           ev.cacheWriteTokens || ev.totalTokens;
}

} // namespace

const char* HanchoEventKindFor(const LlmStreamEvent& ev) {    if (ev.type == "tool_call") {
        std::string activity, target;
        if (DescribeLlmActivity(ev, activity, target) && activity == "plan") return "plan";
        return "tool.call";
    }
    if (ev.type == "tool_result") return "tool.result";
    if (ev.type == "thinking") return "thought";
    if (ev.type == "message") return "message";
    if (ev.type == "usage") return "usage";
    if (ev.type == "rate_limit") return "rate_limit";
    if (ev.type == "subagent_start") return "subagent.started";
    if (ev.type == "subagent_end") return "subagent.ended";
    if (ev.type == "result") return "run.ended";
    if (ev.type == "error") return "error";
    if (ev.type == "status") return ev.content == "init" ? "run.started" : "turn.started";
    if (ev.type == "step") return HasUsage(ev) || ev.costUsd > 0.0 ? "usage" : "turn.started";
    return "raw";
}

std::string HanchoEventsFilePathForStream(const std::string& streamFilePath) {
    const std::string from = "_stream.jsonl";
    const std::string to = ".events.jsonl";
    size_t pos = streamFilePath.rfind(from);
    if (pos == std::string::npos) return "";
    std::string out = streamFilePath;
    out.replace(pos, from.size(), to);
    return out;
}

std::vector<nlohmann::json> ReadHanchoEventsFile(const std::string& path, size_t tail,
                                                 const std::string& kindFilter, uint64_t sinceSeq) {
    std::vector<nlohmann::json> result;
    if (path.empty()) return result;
    std::ifstream in(Utf8ToWide(path), std::ios::binary);
    if (!in) return result;

    std::deque<nlohmann::json> kept;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line == "\r") continue;
        auto parsed = nlohmann::json::parse(line, nullptr, false);
        if (!parsed.is_object()) continue;
        if (parsed.value("schema", std::string()) != "hancho.event/1") continue;
        if (!kindFilter.empty() && parsed.value("kind", std::string()) != kindFilter) continue;
        if (parsed.value("seq", uint64_t{0}) <= sinceSeq) continue;
        kept.push_back(std::move(parsed));
        if (tail > 0 && kept.size() > tail) kept.pop_front();
    }

    result.reserve(kept.size());
    for (auto& ev : kept) result.push_back(std::move(ev));
    return result;
}

nlohmann::json BuildHanchoEventV1(const LlmStreamEvent& ev, const HanchoEventSource& source,
                                  const std::string& parentAgentId, const std::string& rawFile,
                                  uint64_t rawLine) {
    const std::string kind = HanchoEventKindFor(ev);

    nlohmann::json data = nlohmann::json::object();
    if (kind == "run.started") {
        if (!ev.cwd.empty()) data["cwd"] = ev.cwd;
        if (ev.toolsOffered > 0) data["toolsOffered"] = ev.toolsOffered;
    } else if (kind == "turn.started") {
        if (!ev.messageId.empty()) data["messageId"] = ev.messageId;
    } else if (kind == "tool.call") {
        std::string activity, target;
        DescribeLlmActivity(ev, activity, target);
        data["tool"] = ev.toolName;
        data["activity"] = activity.empty() ? "tool" : activity;
        if (!ev.toolUseId.empty()) data["toolUseId"] = ev.toolUseId;
        if (!target.empty()) data["target"] = target;
        if (!ev.toolInput.empty()) data["input"] = ToolInputJson(ev.toolInput);
        std::string note = DescribeLlmNote(ev);
        if (!note.empty()) data["note"] = note;
    } else if (kind == "tool.result") {
        data["ok"] = !ev.toolError;
        if (!ev.toolUseId.empty()) data["toolUseId"] = ev.toolUseId;
        if (ev.toolError) {
            data["error"] = Utf8Prefix(ev.content, kMaxSummaryBytes);
        } else if (!ev.content.empty()) {
            data["summary"] = Utf8Prefix(ev.content, kMaxSummaryBytes);
        }
    } else if (kind == "thought" || kind == "message") {
        std::string text;
        bool truncated = false;
        PutText(text, ev.content, truncated);
        data["text"] = text;
        if (truncated) data["truncated"] = true;
    } else if (kind == "plan") {
        data["items"] = PlanItems(ev.toolInput);
    } else if (kind == "usage") {
        if (ev.inputTokens) data["inputTokens"] = ev.inputTokens;
        if (ev.outputTokens) data["outputTokens"] = ev.outputTokens;
        if (ev.thinkingTokens) data["thinkingTokens"] = ev.thinkingTokens;
        if (ev.cacheReadTokens) data["cacheReadTokens"] = ev.cacheReadTokens;
        if (ev.cacheWriteTokens) data["cacheWriteTokens"] = ev.cacheWriteTokens;
        if (ev.totalTokens) data["totalTokens"] = ev.totalTokens;
        if (ev.costUsd > 0.0) data["costUsd"] = ev.costUsd;
    } else if (kind == "rate_limit") {
        data["status"] = ev.rateLimitStatus.empty() ? ev.status : ev.rateLimitStatus;
        if (!ev.rateLimitType.empty()) data["window"] = ev.rateLimitType;
        if (ev.rateLimitUtilization >= 0.0) data["utilization"] = ev.rateLimitUtilization;
        if (ev.rateLimitResetsAtSec > 0) data["resetsAt"] = ev.rateLimitResetsAtSec;
    } else if (kind == "subagent.started") {
        data["agentId"] = ev.agentId.empty() ? ev.toolUseId : ev.agentId;
        if (!ev.subagentType.empty()) data["type"] = ev.subagentType;
        if (!ev.taskDescription.empty()) data["description"] = ev.taskDescription;
        if (!ev.toolUseId.empty()) data["toolUseId"] = ev.toolUseId;
    } else if (kind == "subagent.ended") {
        data["agentId"] = ev.agentId.empty() ? ev.toolUseId : ev.agentId;
        data["status"] = ev.taskStatus.empty() ? "completed" : ev.taskStatus;
        if (ev.taskTokens > 0) data["tokens"] = ev.taskTokens;
        if (ev.taskToolUses > 0) data["toolUses"] = ev.taskToolUses;
        if (ev.taskDurationMs > 0) data["durationMs"] = ev.taskDurationMs;
    } else if (kind == "run.ended") {
        data["status"] = ev.status == "error" ? "error" : "completed";
        if (!ev.stopReason.empty()) data["stopReason"] = ev.stopReason;
        if (ev.numTurns > 0) data["numTurns"] = ev.numTurns;
        if (ev.apiDurationMs > 0) data["durationMs"] = ev.apiDurationMs;
        if (ev.permissionDenials > 0) data["permissionDenials"] = ev.permissionDenials;
    } else if (kind == "error") {
        data["message"] = ev.content;
    } else {
        std::string text = ev.content;
        if (text.empty()) text = ev.toolAction;
        if (text.empty()) text = ev.type;
        data["text"] = text;
    }

    std::string finalKind = kind;
    // A usage event needs at least one token count; a cost-only step has nothing to report.
    if (finalKind == "usage" && !HasUsage(ev)) {
        finalKind = "raw";
        data = nlohmann::json{{"text", LlmEventUsageText(ev)}};
    }

    nlohmann::json sourceJson;
    sourceJson["processId"] = source.processId;
    if (!source.jobId.empty()) sourceJson["jobId"] = source.jobId;
    if (source.attempt > 0) sourceJson["attempt"] = source.attempt;
    if (!source.btId.empty()) sourceJson["btId"] = source.btId;
    if (!source.node.empty()) sourceJson["node"] = source.node;

    nlohmann::json agentJson;
    agentJson["type"] = ev.agentType.empty() ? "generic" : ev.agentType;
    agentJson["model"] = ev.model;
    agentJson["sessionId"] = ev.sessionId;
    agentJson["agentId"] = ev.agentId;
    agentJson["parentAgentId"] = parentAgentId;

    nlohmann::json raw;
    raw["file"] = FileNameOf(rawFile);
    if (rawLine > 0) raw["line"] = rawLine;

    nlohmann::json out;
    out["schema"] = "hancho.event/1";
    out["seq"] = ev.seq;
    out["ts"] = IsoUtcFromEpochMs(ev.recvEpochMs);
    out["source"] = std::move(sourceJson);
    out["agent"] = std::move(agentJson);
    out["kind"] = finalKind;
    out["data"] = std::move(data);
    out["raw"] = std::move(raw);
    return out;
}

nlohmann::json BuildHanchoEventObject(const HanchoEventSource& source,
                                      const std::string& agentType, const std::string& model,
                                      const std::string& sessionId, const std::string& agentId,
                                      const std::string& parentAgentId, uint64_t seq, int64_t epochMs,
                                      const std::string& kind, nlohmann::json data,
                                      const std::string& rawFile, uint64_t rawLine) {
    nlohmann::json sourceJson;
    sourceJson["processId"] = source.processId;
    if (!source.jobId.empty()) sourceJson["jobId"] = source.jobId;
    if (source.attempt > 0) sourceJson["attempt"] = source.attempt;
    if (!source.btId.empty()) sourceJson["btId"] = source.btId;
    if (!source.node.empty()) sourceJson["node"] = source.node;

    nlohmann::json agentJson;
    agentJson["type"] = agentType.empty() ? "generic" : agentType;
    agentJson["model"] = model;
    agentJson["sessionId"] = sessionId;
    agentJson["agentId"] = agentId;
    agentJson["parentAgentId"] = parentAgentId;

    nlohmann::json raw;
    raw["file"] = FileNameOf(rawFile);
    if (rawLine > 0) raw["line"] = rawLine;

    nlohmann::json out;
    out["schema"] = "hancho.event/1";
    out["seq"] = seq;
    out["ts"] = IsoUtcFromEpochMs(epochMs);
    out["source"] = std::move(sourceJson);
    out["agent"] = std::move(agentJson);
    out["kind"] = kind;
    out["data"] = std::move(data);
    out["raw"] = std::move(raw);
    return out;
}

} // namespace pc
