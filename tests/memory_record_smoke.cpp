// Headless smoke test for L1 run records (src/memory_record.cpp, doc/design-v3.md 3.3 / 3.7).
#include "../src/memory_record.h"
#include "../src/common.h"
#include "../src/nlohmann/json.hpp"

#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace pc;
using json = nlohmann::json;
namespace fs = std::filesystem;

static int g_failures = 0;

static void Check(bool cond, const std::string& label) {
    std::printf("%s %s\n", cond ? "[PASS]" : "[FAIL]", label.c_str());
    std::fflush(stdout);
    if (!cond) g_failures++;
}

static void WriteText(const fs::path& path, const std::string& text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

static std::string ReadText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

static json Parse(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    return j.is_discarded() ? json::object() : j;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);

    fs::path temp = fs::temp_directory_path() /
                    ("hancho_memory_record_smoke_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(temp, ec);
    fs::create_directories(temp / "taskqueues", ec);
    fs::create_directories(temp / "taskqueue", ec);
    fs::create_directories(temp / "memory" / "records", ec);
    SetDataDirectoryOverride(temp.wstring());

    {
        json events = json::array();
        auto ev = [](uint64_t seq, const char* kind, json data) {
            return json{{"schema", "hancho.event/1"},
                        {"seq", seq},
                        {"kind", kind},
                        {"agent", {{"type", "claude"}, {"model", "claude-opus-5-5"},
                                   {"sessionId", "sess_1"}}},
                        {"data", std::move(data)}};
        };
        events.push_back(ev(1, "run.started", {{"cwd", "D:/wsgit/app"}}));
        events.push_back(ev(2, "tool.call", {{"tool", "Read"}, {"activity", "read"},
                                             {"target", "D:\\\\wsgit\\\\app\\\\CMakeLists.txt"}}));
        events.push_back(ev(3, "tool.call", {{"tool", "Edit"}, {"activity", "edit"},
                                             {"target", "D:/wsgit/app/src/net.cpp"}}));
        events.push_back(ev(4, "tool.call", {{"tool", "Bash"}, {"activity", "command"},
                                             {"target", "cmake --build build --config Release"}}));
        events.push_back(ev(5, "tool.result", {{"ok", false},
                                               {"error", "LNK2019: unresolved external symbol __imp_strerror at C:\\\\ws\\\\app\\\\net.cpp:42"}}));
        json planItems = json::array();
        planItems.push_back(json{{"content", "read"}, {"status", "completed"}});
        planItems.push_back(json{{"content", "fix"}, {"status", "pending"}});
        events.push_back(ev(6, "plan", json{{"items", planItems}}));
        events.push_back(ev(7, "message", {{"text", "net.cpp のリンクエラーを直しました"}}));
        events.push_back(ev(8, "usage", {{"totalTokens", 100}, {"costUsd", 0.5}}));
        events.push_back(ev(9, "subagent.started", {{"type", "general-purpose"}}));

        std::string text;
        for (const auto& e : events) text += e.dump() + "\r\n";
        WriteText(temp / "proc_42_build.events.jsonl", text);

        WriteText(temp / "taskqueue" / "j_7f3a.2.log",
                  "building...\r\nLNK2019: unresolved external symbol __imp_strerror at C:\\ws\\app\\net.cpp:42\r\n"
                  "LNK2019: unresolved external symbol __imp_strerror at C:\\ws\\app\\other.cpp:7\r\n"
                  "link failed\r\n");
    }

    {
        json tq = {
            {"name", "main"},
            {"jobs", json::array({
                json{
                    {"id", "j_7f3a"},
                    {"taskqueue", "main"},
                    {"name", "build"},
                    {"summary", "Release build"},
                    {"instructions", {{"text", "Pass the build"}}},
                    {"command", "cmake --build build"},
                    {"cwd", "D:/wsgit/app"},
                    {"state", "failed"},
                    {"bt", {{"id", "bt_main"}, {"node", "build"}, {"run", 3}}},
                    {"origin", {{"client", "claude-code 2.4.0"}, {"session", "sess_1"},
                                {"cwd", "D:/wsgit/app"}}},
                    {"attempts", json::array({
                        json{{"n", 2},
                             {"process_id", 42},
                             {"start", "2026-11-10 09:02:11"},
                             {"end", "2026-11-10 09:14:40"},
                             {"reason", "failed"},
                             {"exit_code", 0},
                             {"check_detail", "filesNewer missing"},
                             {"failure", {{"class", "success_check_failed"},
                                          {"summary", "app.exe was not updated"}}},
                             {"last_lines_file", ""}}
                    })}
                },
                json{
                    {"id", "j_plain"},
                    {"taskqueue", "main"},
                    {"name", "plain"},
                    {"summary", "Run a plain command"},
                    {"command", "deploy --now"},
                    {"cwd", "D:/wsgit/app"},
                    {"state", "failed"},
                    {"attempts", json::array({
                        json{{"n", 1},
                             {"process_id", 0},
                             {"start", "2026-11-10 10:00:00"},
                             {"end", "2026-11-10 10:00:05"},
                             {"reason", "failed"},
                             {"exit_code", 2},
                             {"check_detail", ""},
                             {"last_lines_file", ""}}
                    })}
                }
            })}
        };
        WriteText(temp / "taskqueues" / "main.json", tq.dump(2) + "\r\n");
    }

    std::string firstBuild;
    {
        json r = Parse(memory::RebuildRunRecordsJson());
        Check(r.value("ok", false), "run record rebuild succeeds");
        Check(r.value("skipped", true) == false, "rebuild uses the taskqueue state");
        Check(r.value("records", 0) == 2, "one record per attempt is created");
        Check(fs::exists(temp / "memory" / "records" / "2026-11.jsonl"),
              "records are written to the month file");

        std::string text = ReadText(temp / "memory" / "records" / "2026-11.jsonl");
        size_t lineEnd = text.find('\n');
        firstBuild = text.substr(0, lineEnd);
        json rec = Parse(firstBuild);
        Check(rec.value("schema", std::string()) == "hancho.memory.run/1", "schema is hancho.memory.run/1");
        Check(rec.value("id", std::string()) == "rr_j_7f3a_2", "id is rr_<jobId>_<attempt>");
        Check(rec.value("jobId", std::string()) == "j_7f3a", "jobId is kept");
        Check(rec.value("jobName", std::string()) == "build", "jobName is kept");
        Check(rec.value("attempt", 0) == 2, "attempt number is kept");
        Check(rec.value("outcome", std::string()) == "failed", "failed outcome is derived");
        Check(rec.value("exitCode", 0) == 0, "exit code is kept");
        Check(rec.value("durationSec", 0) == 749, "duration is derived from start/end");
        Check(rec.value("goal", std::string()).find("Release build") != std::string::npos &&
                  rec.value("goal", std::string()).find("Pass the build") != std::string::npos,
              "goal combines summary and instructions");
        Check(rec.value("engine", std::string()) == "claude", "engine comes from events");
        Check(rec.value("model", std::string()) == "claude-opus-5-5", "model comes from events");
        Check(rec.value("sessionId", std::string()) == "sess_1", "session id comes from events");
        Check(rec.value("failure", json::object()).value("class", std::string()) ==
                  "success_check_failed",
              "failure class is copied from the attempt");
        Check(rec.value("bt", json::object()).value("run", 0) == 3, "BT reference is kept");

        auto read = rec.value("filesRead", json::array());
        auto edited = rec.value("filesEdited", json::array());
        Check(read.size() == 1 && read[0] == "CMakeLists.txt", "filesRead is cwd-relative");
        Check(edited.size() == 1 && edited[0] == "src/net.cpp", "filesEdited is cwd-relative");
        auto commands = rec.value("commands", json::array());
        Check(commands.size() == 1 && commands[0] == "cmake --build build --config Release",
              "command activity becomes commands");
        auto todos = rec.value("todos", json::object());
        Check(todos.value("done", 0) == 1 && todos.value("total", 0) == 2,
              "last plan becomes todos");
        Check(rec.value("finalMessage", std::string()).find("リンクエラー") != std::string::npos,
              "last message becomes finalMessage");
        Check(rec.value("usage", json::object()).value("totalTokens", 0) == 100,
              "usage events are summed");
        Check(rec.value("subagents", json::object()).value("count", 0) == 1,
              "subagent starts are counted");
        auto sources = rec.value("sources", json::object());
        Check(sources.value("eventsFile", std::string()) == "proc_42_build.events.jsonl",
              "sources.eventsFile names the events file");
        auto range = sources.value("seqRange", json::array());
        Check(range.size() == 2 && range[0] == 1 && range[1] == 9,
              "sources.seqRange spans the events");
        Check(sources.value("logFile", std::string()) == "j_7f3a.2.log",
              "sources.logFile names the attempt log");
        auto errors = rec.value("errors", json::array());
        Check(!errors.empty(), "log and event errors are collected");
        bool masked = false;
        for (const auto& e : errors) {
            if (e.get<std::string>().find("<path>") != std::string::npos &&
                e.get<std::string>().find("<n>") != std::string::npos) {
                masked = true;
            }
        }
        Check(masked, "numbers and paths are masked before grouping");
    }

    {
        std::string first = ReadText(temp / "memory" / "records" / "2026-11.jsonl");
        json r = Parse(memory::RebuildRunRecordsJson());
        std::string second = ReadText(temp / "memory" / "records" / "2026-11.jsonl");
        Check(r.value("ok", false), "a second rebuild succeeds");
        Check(first == second, "the same inputs produce the same L1");
    }

    {
        fs::remove(temp / "memory" / "records" / "2026-11.jsonl", ec);

        memory::RunRecordInput in;
        in.jobId = "j_7f3a";
        in.taskqueue = "main";
        in.jobName = "build";
        in.summary = "Release build";
        in.instructions = "Pass the build";
        in.command = "cmake --build build";
        in.cwd = "D:/wsgit/app";
        in.btId = "bt_main";
        in.btNode = "build";
        in.btRun = 3;
        in.attempt = 2;
        in.originClient = "claude-code 2.4.0";
        in.originSession = "sess_1";
        in.originCwd = "D:/wsgit/app";
        in.start = "2026-11-10 09:02:11";
        in.end = "2026-11-10 09:14:40";
        in.exitCode = 0;
        in.reason = "failed";
        in.jobState = "failed";
        in.failureClass = "success_check_failed";
        in.failureSummary = "app.exe was not updated";
        in.checkDetail = "filesNewer missing";
        in.processId = 42;

        json first = Parse(memory::AppendRunRecordJson(in));
        json second = Parse(memory::AppendRunRecordJson(in));
        Check(first.value("ok", false) && first.value("created", false),
              "a finished attempt appends one record");
        Check(second.value("ok", false) && !second.value("created", true),
              "appending the same attempt twice is idempotent");

        std::string text = ReadText(temp / "memory" / "records" / "2026-11.jsonl");
        size_t lineEnd = text.find('\n');
        std::string line = text.substr(0, lineEnd);
        Check(line == firstBuild, "a live record matches the rebuilt record");
    }

    fs::remove_all(temp, ec);
    SetDataDirectoryOverride(L"");

    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
