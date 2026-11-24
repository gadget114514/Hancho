#include "demo.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>

namespace pc {
namespace demo {

using nlohmann::json;

namespace {

std::string FormatLocalTime(int64_t epochSec, const char* fmt) {
    time_t t = static_cast<time_t>(epochSec);
    std::tm tm = {};
    if (!LocalTimeFromSec(&t, &tm)) return "";
    char buf[64] = {};
    if (std::strftime(buf, sizeof(buf), fmt, &tm) == 0) return "";
    return std::string(buf);
}

int LocalOffsetMinutes(int64_t epochSec) {
    time_t t = static_cast<time_t>(epochSec);
    std::tm lt = {};
    if (!LocalTimeFromSec(&t, &lt)) return 0;
    std::tm asLocal = lt;
    std::tm asUtc = lt;
    std::time_t localEpoch = std::mktime(&asLocal);
#ifdef _WIN32
    std::time_t utcEpoch = _mkgmtime64(&asUtc);
#else
    std::time_t utcEpoch = timegm(&asUtc);
#endif
    return static_cast<int>((utcEpoch - localEpoch) / 60);
}

std::wstring SpecPathWide() {
    return PathJoinWide(GetLogsDirectoryWide(), L"demo.json");
}

std::wstring DailyDirWide() {
    return PathJoinWide(PathJoinWide(GetLogsDirectoryWide(), L"usage"), L"daily");
}

std::wstring DailyPathWide(const std::string& yearMonth) {
    return PathJoinWide(DailyDirWide(), Utf8ToWide(yearMonth) + L".jsonl");
}

std::string QuoteArg(const std::string& s) {
    if (s.find_first_of(" \t\"") == std::string::npos) return s;
    std::string out = "\"";
    for (char c : s) {
        if (c == '"') out += '\\';
        out += c;
    }
    out += '"';
    return out;
}

std::string JsonLine(const json& j) {
    return j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n";
}

json MakeUsageJson(int64_t epoch, const DemoModel& model, uint64_t input, uint64_t output,
                   uint64_t cacheRead, uint64_t cacheWrite, double cost) {
    json j = json::object();
    j["epochSec"] = epoch;
    j["inputTokens"] = input;
    j["outputTokens"] = output;
    j["thinkingTokens"] = output / 10;
    j["cacheReadTokens"] = cacheRead;
    j["cacheWriteTokens"] = cacheWrite;
    j["totalTokens"] = input + output + output / 10 + cacheRead + cacheWrite;
    j["costUsd"] = cost;
    j["agentType"] = model.engine;
    j["model"] = model.model;
    return j;
}

json MakeJobJson(const std::string& queue, const std::string& id, const std::string& summary,
                 const std::string& command, const std::string& state, int64_t created,
                 int64_t updated, const std::vector<std::string>& deps,
                 const std::vector<std::pair<int64_t, int64_t>>& attempts) {
    json j = json::object();
    j["id"] = id;
    j["taskqueue"] = queue;
    j["name"] = id;
    j["summary"] = summary;
    j["command"] = command;
    j["cwd"] = "";
    j["depends_on"] = json::array();
    for (const auto& dep : deps) {
        j["depends_on"].push_back(json{{"id", dep}, {"on", "success"}});
    }
    j["timeout_sec"] = 300;
    j["estimate_sec"] = 60;
    j["max_attempts"] = 2;
    j["state"] = state;
    j["attempts"] = json::array();
    for (size_t i = 0; i < attempts.size(); ++i) {
        json a = json::object();
        a["n"] = static_cast<int>(i) + 1;
        a["start"] = FormatLocalTime(attempts[i].first, "%Y-%m-%d %H:%M:%S");
        a["end"] = FormatLocalTime(attempts[i].second, "%Y-%m-%d %H:%M:%S");
        a["exit_code"] = 0;
        a["instructions_rev"] = 1;
        j["attempts"].push_back(std::move(a));
    }
    j["created_at"] = FormatLocalTime(created, "%Y-%m-%d %H:%M:%S");
    j["updated_at"] = FormatLocalTime(updated, "%Y-%m-%d %H:%M:%S");
    j["origin"] = json{{"label", "demo"}, {"client", "demo"}};
    return j;
}

json ClaudeLine(const std::string& type) {
    json j = json::object();
    j["type"] = type;
    j["session_id"] = "demo-session";
    return j;
}

json ClaudeUsage(int seed, int multiplier) {
    json u = json::object();
    u["input_tokens"] = 8000 + seed * 137 + multiplier * 400;
    u["output_tokens"] = 1800 + seed * 29 + multiplier * 90;
    u["cache_creation_input_tokens"] = 900 + seed * 17;
    u["cache_read_input_tokens"] = 3200 + seed * 53;
    u["total_tokens"] = u["input_tokens"].get<uint64_t>() + u["output_tokens"].get<uint64_t>() +
                        u["cache_creation_input_tokens"].get<uint64_t>() +
                        u["cache_read_input_tokens"].get<uint64_t>();
    return u;
}

void AppendAssistant(json& out, const std::string& id, const std::string& text, const json& usage,
                     const std::string& agentId = "", const std::string& parentToolUseId = "") {
    json line = ClaudeLine("assistant");
    json message = json::object();
    message["id"] = id;
    message["model"] = "demo-model";
    message["role"] = "assistant";
    message["content"] = json::array({json{{"type", "text"}, {"text", text}}});
    message["usage"] = usage;
    line["message"] = std::move(message);
    if (!agentId.empty()) line["agent_id"] = agentId;
    if (!parentToolUseId.empty()) line["parent_tool_use_id"] = parentToolUseId;
    out.push_back(std::move(line));
}

} // namespace

bool LoadSpec(DemoSpec& out, std::string& err) {
    out = DemoSpec();
    out.models.push_back({"claude", "claude-sonnet-4", 3});
    out.models.push_back({"opencode", "grok-code-fast-1", 2});

    std::wstring path = SpecPathWide();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) return true;

    std::ifstream f(path);
    if (!f) {
        err = "cannot read demo spec: " + WideToUtf8(path);
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    json root = json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        err = "demo.json must be a JSON object";
        return false;
    }
    if (root.contains("now") && root["now"].is_string()) out.now = root["now"].get<std::string>();
    if (root.contains("days") && root["days"].is_number_integer()) out.days = root["days"].get<int>();
    if (root.contains("points_per_hour") && root["points_per_hour"].is_number_integer()) {
        out.pointsPerHour = root["points_per_hour"].get<int>();
    }
    if (root.contains("tokens_per_day") && root["tokens_per_day"].is_number_unsigned()) {
        out.tokensPerDay = root["tokens_per_day"].get<uint64_t>();
    }
    if (root.contains("cost_per_day") && root["cost_per_day"].is_number()) {
        out.costPerDay = root["cost_per_day"].get<double>();
    }
    if (root.contains("queue") && root["queue"].is_string()) out.queue = root["queue"].get<std::string>();
    if (root.contains("models") && root["models"].is_array()) {
        out.models.clear();
        for (const auto& m : root["models"]) {
            if (!m.is_object()) continue;
            DemoModel dm;
            dm.engine = m.value("engine", std::string());
            dm.model = m.value("model", std::string());
            dm.weight = m.value("weight", 1);
            if (!dm.engine.empty() && !dm.model.empty()) out.models.push_back(std::move(dm));
        }
        if (out.models.empty()) {
            err = "demo.json models must contain at least one engine/model pair";
            return false;
        }
    }
    out.days = std::max(1, std::min(out.days, 365));
    out.pointsPerHour = std::max(1, std::min(out.pointsPerHour, 60));
    if (out.queue.empty()) out.queue = "demo";
    return true;
}

bool GenerateData(const DemoSpec& spec, int64_t nowSec, std::string& err) {
    err.clear();
    std::wstring dataDir = GetLogsDirectoryWide();
    std::wstring dailyDir = DailyDirWide();
    std::error_code ec;
    EnsureDirectoryWide(dataDir);
    EnsureDirectoryWide(dailyDir);

    int totalWeight = 0;
    for (const auto& m : spec.models) totalWeight += std::max(1, m.weight);
    if (totalWeight <= 0) totalWeight = 1;

    std::string history;
    int u0Count = 24 * spec.pointsPerHour;
    for (int i = 0; i < u0Count; ++i) {
        int64_t epoch = nowSec - static_cast<int64_t>(u0Count - 1 - i) * 3600 / spec.pointsPerHour;
        const DemoModel& m = spec.models[i % spec.models.size()];
        int step = i % 7;
        uint64_t input = spec.tokensPerDay / 24 / spec.pointsPerHour * (8 + step) / 10;
        uint64_t output = input / 4;
        double cost = spec.costPerDay / 24 / spec.pointsPerHour * (8 + step) / 10.0;
        history += JsonLine(MakeUsageJson(epoch, m, input, output, input / 8, input / 20, cost));
    }
    WriteFileAtomicWide(PathJoinWide(dataDir, L"usage_history.jsonl"), history);

    std::map<std::string, std::string> monthly;
    for (int d = spec.days - 1; d >= 0; --d) {
        int64_t dayEpoch = nowSec - static_cast<int64_t>(d) * 86400;
        std::string date = FormatLocalTime(dayEpoch, "%Y-%m-%d");
        std::string ym = date.substr(0, 7);
        int pattern = (d * 7) % 11;
        for (const auto& m : spec.models) {
            json rec = json::object();
            rec["schema"] = "hancho.usage.day/1";
            rec["date"] = date;
            rec["tzOffsetMin"] = LocalOffsetMinutes(dayEpoch);
            rec["engine"] = m.engine;
            rec["model"] = m.model;
            uint64_t tokens = spec.tokensPerDay * std::max(1, m.weight) / totalWeight *
                              static_cast<uint64_t>(70 + pattern) / 100;
            rec["inputTokens"] = tokens * 4 / 5;
            rec["outputTokens"] = tokens / 5;
            rec["thinkingTokens"] = tokens / 40;
            rec["cacheReadTokens"] = tokens / 10;
            rec["cacheWriteTokens"] = tokens / 25;
            rec["totalTokens"] = tokens + tokens / 40 + tokens / 10 + tokens / 25;
            rec["costUsd"] = spec.costPerDay * std::max(1, m.weight) / totalWeight *
                             (70 + pattern) / 100.0;
            rec["costKnown"] = true;
            rec["runs"] = 1 + (d % 3);
            rec["firstEpochSec"] = dayEpoch + 9 * 3600;
            rec["lastEpochSec"] = dayEpoch + 17 * 3600;
            monthly[ym] += JsonLine(rec);
        }
    }
    for (const auto& kv : monthly) {
        WriteFileAtomicWide(DailyPathWide(kv.first), kv.second);
    }

    json queue = json::object();
    queue["name"] = spec.queue;
    queue["concurrency"] = 2;
    queue["paused"] = false;
    queue["version"] = 1;
    queue["jobs"] = json::array();
    auto& jobs = queue["jobs"];
    jobs.push_back(MakeJobJson(spec.queue, "demo-intake", "Collect demo inputs", "echo demo-intake",
                               "succeeded", nowSec - 72 * 3600, nowSec - 71 * 3600, {},
                               {{nowSec - 72 * 3600, nowSec - 71 * 3600}}));
    jobs.push_back(MakeJobJson(spec.queue, "demo-implement", "Implement demo feature", "echo demo-implement",
                               "succeeded", nowSec - 50 * 3600, nowSec - 48 * 3600, {"demo-intake"},
                               {{nowSec - 50 * 3600, nowSec - 48 * 3600}}));
    jobs.push_back(MakeJobJson(spec.queue, "demo-verify", "Verify demo output", "echo demo-verify",
                               "succeeded", nowSec - 26 * 3600, nowSec - 25 * 3600, {"demo-implement"},
                               {{nowSec - 26 * 3600, nowSec - 26 * 3600 + 600},
                                {nowSec - 26 * 3600 + 900, nowSec - 25 * 3600}}));
    jobs.push_back(MakeJobJson(spec.queue, "demo-usage", "Generate demo token usage",
                               DummyJobCommand("usage", 1), "pending", nowSec - 600, nowSec - 600,
                               {"demo-verify"}, {}));
    jobs.push_back(MakeJobJson(spec.queue, "demo-subtasks", "Generate demo subtasks",
                               DummyJobCommand("subagents", 2), "pending", nowSec - 540, nowSec - 540,
                               {"demo-verify"}, {}));
    jobs.push_back(MakeJobJson(spec.queue, "demo-finalize", "Summarize demo results",
                               DummyJobCommand("mixed", 3), "pending", nowSec - 480, nowSec - 480,
                               {"demo-usage", "demo-subtasks"}, {}));

    std::wstring queueDir = PathJoinWide(dataDir, L"taskqueues");
    EnsureDirectoryWide(queueDir);
    std::wstring queuePath = PathJoinWide(queueDir, Utf8ToWide(SanitizeFileNameComponent(spec.queue)) + L".json");
    WriteFileAtomicWide(queuePath, JsonLine(queue));
    return true;
}

bool Prepare(std::string& err) {
    DemoSpec spec;
    if (!LoadSpec(spec, err)) return false;
    int64_t now = ParseTimestampStrToEpoch(spec.now);
    if (now <= 0) {
        err = "demo.json 'now' must be a local date/time like '2026-11-21 15:00:00'";
        return false;
    }
    SetDemoMode(true, now);
    return GenerateData(spec, now, err);
}

std::string DummyJobCommand(const std::string& scenario, int seed) {
    return QuoteArg(GetDummyLlmExecutableUtf8()) + " --scenario " + scenario +
           " --seed " + std::to_string(seed);
}

std::string BuildDummyLlmOutput(const std::string& scenario, int seed) {
    json lines = json::array();
    json init = ClaudeLine("system");
    init["subtype"] = "init";
    init["cwd"] = ".";
    init["tools"] = json::array();
    lines.push_back(std::move(init));

    if (scenario == "usage" || scenario == "mixed") {
        AppendAssistant(lines, "msg-demo-usage", "demo usage answer", ClaudeUsage(seed, 1));
    }
    if (scenario == "subagents" || scenario == "mixed") {
        for (int i = 1; i <= 2; ++i) {
            std::string agentId = "agent-demo-" + std::to_string(i);
            std::string toolUseId = "toolu-demo-" + std::to_string(i);

            json toolUse = ClaudeLine("assistant");
            json message = json::object();
            message["id"] = "msg-demo-agent-" + std::to_string(i);
            message["model"] = "demo-model";
            message["content"] = json::array({json{
                {"type", "tool_use"}, {"id", toolUseId}, {"name", "Agent"},
                {"input", json{{"description", "Demo subtask " + std::to_string(i)}}}}});
            message["usage"] = ClaudeUsage(seed, 0);
            toolUse["message"] = std::move(message);
            lines.push_back(std::move(toolUse));

            json started = ClaudeLine("system");
            started["subtype"] = "task_started";
            started["task_id"] = agentId;
            started["tool_use_id"] = toolUseId;
            started["subagent_type"] = "general-purpose";
            started["description"] = "Demo subtask " + std::to_string(i);
            lines.push_back(std::move(started));
            AppendAssistant(lines, "msg-demo-sub-" + std::to_string(i),
                            "demo subtask answer " + std::to_string(i), ClaudeUsage(seed, i),
                            agentId, toolUseId);
            json done = ClaudeLine("system");
            done["subtype"] = "task_notification";
            done["task_id"] = agentId;
            done["tool_use_id"] = toolUseId;
            done["status"] = "completed";
            done["summary"] = "Demo subtask " + std::to_string(i) + " completed";
            done["usage"] = json{{"total_tokens", 5000 + seed * 100 + i * 250},
                                 {"tool_uses", 2 + i}, {"duration_ms", 1500 + i * 500}};
            lines.push_back(std::move(done));
        }
    }

    json result = ClaudeLine("result");
    result["subtype"] = "success";
    result["is_error"] = false;
    result["result"] = "demo run complete";
    result["usage"] = ClaudeUsage(seed, 4);
    result["total_cost_usd"] = 0.25 + seed * 0.01;
    result["duration_api_ms"] = 1200;
    result["num_turns"] = 2;
    lines.push_back(std::move(result));

    std::string out;
    for (const auto& line : lines) out += JsonLine(line);
    return out;
}

} // namespace demo
} // namespace pc
