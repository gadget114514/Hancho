#include "common.h"
#include "demo.h"
#include "llm_stream.h"
#include "nlohmann/json.hpp"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

using nlohmann::json;
using namespace pc;

static int Fail(const std::string& msg) {
    std::cerr << "demo_smoke FAIL: " << msg << "\n";
    return 1;
}

int main() {
    std::string dir = GetTempDirectoryUtf8() + "hancho-demo-smoke-" +
                      std::to_string(GetCurrentProcessIdPortable());
    std::wstring wdir = Utf8ToWide(dir);
    std::error_code ec;
    std::filesystem::create_directories(wdir, ec);
    SetDataDirectoryOverride(wdir);

    const std::string nowStr = "2026-11-21 15:00:00";
    json spec = json::object();
    spec["now"] = nowStr;
    spec["days"] = 30;
    spec["points_per_hour"] = 4;
    spec["tokens_per_day"] = 180000;
    spec["cost_per_day"] = 5.0;
    spec["queue"] = "demo";
    WriteFileAtomicWide(PathJoinWide(wdir, L"demo.json"), spec.dump() + "\n");

    std::string err;
    if (!pc::demo::Prepare(err)) return Fail("Prepare: " + err);
    int64_t now = ParseTimestampStrToEpoch(nowStr);
    if (GetNowEpochSec() != now) return Fail("demo clock is not pinned to spec.now");

    std::ifstream history(PathJoinWide(wdir, L"usage_history.jsonl"));
    if (!history) return Fail("usage_history.jsonl missing");
    int historyLines = 0;
    std::string line;
    while (std::getline(history, line)) {
        if (line.empty()) continue;
        ++historyLines;
        json j = json::parse(line, nullptr, false);
        int64_t epoch = j.value("epochSec", (int64_t)0);
        if (epoch < now - 86400 || epoch > now) return Fail("usage point outside 24h window");
    }
    if (historyLines != 96) return Fail("expected 96 usage points, got " + std::to_string(historyLines));

    pc::LlmUsageStatsOptions opts;
    opts.since = "30d";
    auto stats30 = pc::QueryUsageStats(opts);
    if (stats30.totalTokens == 0) return Fail("30d usage stats empty");
    if (stats30.records.size() < 60) return Fail("30d daily records missing");

    opts.since = "24h";
    auto stats24 = pc::QueryUsageStats(opts);
    if (stats24.totalTokens == 0) return Fail("24h usage stats empty");

    opts.since = "1h";
    auto stats1 = pc::QueryUsageStats(opts);
    if (stats1.totalTokens == 0) return Fail("1h usage stats empty");

    std::ifstream queueFile(PathJoinWide(PathJoinWide(wdir, L"taskqueues"), L"demo.json"));
    if (!queueFile) return Fail("taskqueues/demo.json missing");
    std::string queueText((std::istreambuf_iterator<char>(queueFile)), std::istreambuf_iterator<char>());
    json queue = json::parse(queueText, nullptr, false);
    if (queue.is_discarded() || !queue.contains("jobs")) return Fail("invalid demo queue JSON");
    if (queue["jobs"].size() < 6) return Fail("demo queue has too few jobs");
    bool hasDependency = false;
    for (const auto& job : queue["jobs"]) {
        if (job.contains("depends_on") && !job["depends_on"].empty()) hasDependency = true;
    }
    if (!hasDependency) return Fail("demo queue has no subtask dependencies");

    std::string out1 = pc::demo::BuildDummyLlmOutput("mixed", 1);
    std::string out2 = pc::demo::BuildDummyLlmOutput("mixed", 1);
    std::string out3 = pc::demo::BuildDummyLlmOutput("mixed", 2);
    if (out1.empty() || out1 != out2) return Fail("dummy LLM output is not deterministic");
    if (out1 == out3) return Fail("dummy LLM ignores seed");

    pc::SetUsageHistoryPersistence(false);
    pc::SetDailyUsagePersistence(false);
    pc::LlmStreamTracker tracker;
    tracker.Init(4242, "claude", "");
    tracker.IngestChunk(out1.data(), out1.size());
    pc::LlmAgentState state = tracker.GetState();
    if (state.totalTokens == 0) return Fail("dummy LLM produced no token usage");
    if (state.subagents.size() < 2) return Fail("dummy LLM produced no subtasks");

    std::cout << "demo_smoke PASS\n";
    return 0;
}
