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

    {
        memory::RunRecordInput tin;
        tin.jobId = "j_timeout";
        tin.jobName = "build";
        tin.timeoutSec = 1800;
        tin.start = "2026-11-10 11:00:00";
        tin.end = "2026-11-10 11:10:00";
        json rec = memory::BuildRunRecord(tin);
        Check(rec.value("timeoutSec", 0) == 1800, "timeoutSec is included in run record JSON");
    }

    {
        // 072-1: Plain run state rebuild from logs/taskqueues/plain.json
        json plainTq = {
            {"name", "plain"},
            {"jobs", json::array({
                json{
                    {"id", "j_plain_test"},
                    {"taskqueue", "plain"},
                    {"name", "benchmark"},
                    {"summary", "Run benchmark tool"},
                    {"command", "benchmark.exe --all"},
                    {"cwd", "D:/wsgit/app"},
                    {"state", "succeeded"},
                    {"attempts", json::array({
                        json{{"n", 1},
                             {"process_id", 777},
                             {"start", "2026-11-10 16:00:00"},
                             {"end", "2026-11-10 16:05:00"},
                             {"reason", "ok"},
                             {"exit_code", 0},
                             {"check_detail", ""},
                             {"last_lines_file", ""}}
                    })}
                }
            })}
        };
        WriteText(temp / "taskqueues" / "plain.json", plainTq.dump(2) + "\r\n");

        json r = Parse(memory::RebuildRunRecordsJson());
        Check(r.value("ok", false), "rebuild succeeds with plain.json present");
        std::string recsText = ReadText(temp / "memory" / "records" / "2026-11.jsonl");
        Check(recsText.find("rr_j_plain_test_1") != std::string::npos, "plain run record rr_j_plain_test_1 created by rebuild");
        Check(recsText.find("\"taskqueue\":\"plain\"") != std::string::npos, "plain run record preserves taskqueue 'plain'");
        Check(recsText.find("rr_j_7f3a_2") != std::string::npos, "existing main queue run record still preserved");
    }

    // --- Task 072-2: Deterministic failure classification and plain run records ---
    {
        // 1. Deterministic failure classification rules for unclassified failures
        auto cLaunch = memory::ClassifyDeterministicFailure("launch_error", -1, "failed", "exe missing");
        Check(cLaunch.first == "launch_error" && cLaunch.second == "exe missing", "deterministic rule: launch_error");

        auto cKilled = memory::ClassifyDeterministicFailure("killed", 1, "cancelled");
        Check(cKilled.first == "killed", "deterministic rule: killed");

        auto cTimeout = memory::ClassifyDeterministicFailure("timed_out", 1, "failed", "", 120, 125);
        Check(cTimeout.first == "timeout", "deterministic rule: timeout");

        auto cIdle = memory::ClassifyDeterministicFailure("idle_stall", 1, "failed");
        Check(cIdle.first == "idle_stall", "deterministic rule: idle_stall");

        auto cCrash = memory::ClassifyDeterministicFailure("failed", static_cast<int>(0xC0000005), "failed");
        Check(cCrash.first == "crash", "deterministic rule: crash for NTSTATUS exception");

        auto cExit = memory::ClassifyDeterministicFailure("failed", 42, "failed");
        Check(cExit.first == "exit_nonzero" && cExit.second.find("42") != std::string::npos, "deterministic rule: exit_nonzero");

        auto cCheck = memory::ClassifyDeterministicFailure("failed", 0, "failed", "testReport had 3 failures");
        Check(cCheck.first == "success_check_failed", "deterministic rule: success_check_failed");

        auto cSuccess = memory::ClassifyDeterministicFailure("ok", 0, "success");
        Check(cSuccess.first.empty(), "deterministic rule: no failure class for success");

        // 2. Direct plain run record construction and unobtainable fields
        memory::PlainRunRecordInput pin;
        pin.jobId = "j_plain_manual_101";
        pin.taskqueue = "plain";
        pin.jobName = "deploy";
        pin.summary = "Deploy binary to staging";
        pin.command = "deploy.cmd --staging";
        pin.cwd = "D:/wsgit/app";
        pin.start = "2026-11-10 17:00:00";
        pin.end = "2026-11-10 17:01:00";
        pin.exitCode = 1;
        pin.reason = "failed";

        json plainRec = memory::BuildPlainRunRecord(pin);
        Check(plainRec.value("schema", std::string()) == "hancho.memory.run/1", "plain record schema is hancho.memory.run/1");
        Check(plainRec.value("id", std::string()) == "rr_j_plain_manual_101_1", "plain record id is rr_<jobId>_<attempt>");
        Check(plainRec.value("taskqueue", std::string()) == "plain", "plain record taskqueue is plain");
        Check(plainRec.value("outcome", std::string()) == "failed", "plain record outcome is failed");
        Check(plainRec.value("exitCode", 0) == 1, "plain record exitCode is 1");
        Check(plainRec.value("failure", json::object()).value("class", std::string()) == "exit_nonzero",
              "plain record failure class is exit_nonzero by deterministic rule");
        Check(plainRec.value("filesRead", json::array()).empty(), "filesRead is empty array (not guessed)");
        Check(plainRec.value("filesEdited", json::array()).empty(), "filesEdited is empty array (not guessed)");
        Check(!plainRec.contains("usage"), "usage is omitted when tokens cannot be obtained (not guessed)");

        auto cmds = plainRec.value("commands", json::array());
        Check(cmds.size() == 1 && cmds[0] == "deploy.cmd --staging", "plain command is preserved in commands");

        // 3. Append plain run record and verify idempotency
        json appFirst = Parse(memory::AppendPlainRunRecordJson(pin));
        json appSecond = Parse(memory::AppendPlainRunRecordJson(pin));
        Check(appFirst.value("ok", false) && appFirst.value("created", false), "AppendPlainRunRecordJson appends record");
        Check(appSecond.value("ok", false) && !appSecond.value("created", true), "AppendPlainRunRecordJson is idempotent");

        // 4. Rebuild reproduction with multiple plain processes in plain.json
        json multiPlain = {
            {"name", "plain"},
            {"jobs", json::array({
                json{
                    {"id", "j_plain_ok"},
                    {"taskqueue", "plain"},
                    {"name", "verify"},
                    {"summary", "Run lint verification"},
                    {"command", "lint.exe"},
                    {"cwd", "D:/wsgit/app"},
                    {"state", "succeeded"},
                    {"attempts", json::array({
                        json{{"n", 1},
                             {"process_id", 801},
                             {"start", "2026-11-10 18:00:00"},
                             {"end", "2026-11-10 18:02:00"},
                             {"reason", "ok"},
                             {"exit_code", 0},
                             {"check_detail", ""},
                             {"last_lines_file", ""}}
                    })}
                },
                json{
                    {"id", "j_plain_fail"},
                    {"taskqueue", "plain"},
                    {"name", "test"},
                    {"summary", "Run integration tests"},
                    {"command", "test.exe"},
                    {"cwd", "D:/wsgit/app"},
                    {"state", "failed"},
                    {"attempts", json::array({
                        json{{"n", 1},
                             {"process_id", 802},
                             {"start", "2026-11-10 18:05:00"},
                             {"end", "2026-11-10 18:06:00"},
                             {"reason", "failed"},
                             {"exit_code", 2},
                             {"check_detail", ""},
                             {"last_lines_file", ""}}
                    })}
                },
                json{
                    {"id", "j_plain_timeout"},
                    {"taskqueue", "plain"},
                    {"name", "sync"},
                    {"summary", "Sync remote repository"},
                    {"command", "sync.exe"},
                    {"cwd", "D:/wsgit/app"},
                    {"state", "failed"},
                    {"attempts", json::array({
                        json{{"n", 1},
                             {"process_id", 803},
                             {"start", "2026-11-10 18:10:00"},
                             {"end", "2026-11-10 18:15:00"},
                             {"reason", "timed_out"},
                             {"exit_code", 1},
                             {"check_detail", ""},
                             {"last_lines_file", ""}}
                    })}
                }
            })}
        };
        WriteText(temp / "taskqueues" / "plain.json", multiPlain.dump(2) + "\r\n");

        json rebRes = Parse(memory::RebuildRunRecordsJson());
        Check(rebRes.value("ok", false), "rebuild with multiple plain runs succeeds");

        std::string rebuiltText = ReadText(temp / "memory" / "records" / "2026-11.jsonl");
        Check(rebuiltText.find("rr_j_plain_ok_1") != std::string::npos, "rr_j_plain_ok_1 created on rebuild");
        Check(rebuiltText.find("rr_j_plain_fail_1") != std::string::npos, "rr_j_plain_fail_1 created on rebuild");
        Check(rebuiltText.find("rr_j_plain_timeout_1") != std::string::npos, "rr_j_plain_timeout_1 created on rebuild");
        Check(rebuiltText.find("\"class\":\"exit_nonzero\"") != std::string::npos, "plain fail classified as exit_nonzero");
        Check(rebuiltText.find("\"class\":\"timeout\"") != std::string::npos, "plain timeout classified as timeout");

        // Idempotent rebuild check
        json rebRes2 = Parse(memory::RebuildRunRecordsJson());
        Check(rebRes2.value("ok", false), "second rebuild succeeds");
        std::string rebuiltText2 = ReadText(temp / "memory" / "records" / "2026-11.jsonl");
        Check(rebuiltText == rebuiltText2, "rebuild of plain runs is deterministic and idempotent");

        // Verify existing main queue job unaffected
        Check(rebuiltText.find("rr_j_7f3a_2") != std::string::npos, "existing queue job record rr_j_7f3a_2 unaffected");
    }

    // --- Task 054: memory prune, capacity limit, and L2/L3 preservation ---
    {
        fs::path memDir = temp / "memory";
        fs::path recDir = memDir / "records";
        fs::create_directories(recDir, ec);

        // Create month files: 2026-05, 2026-06, 2026-07 (older than 90 days from 2026-11-15)
        // and 2026-11 (current month).
        WriteText(recDir / "2026-05.jsonl", "{\"id\":\"rr_old_1\",\"start\":\"2026-05-10T10:00:00Z\"}\r\n");
        WriteText(recDir / "2026-06.jsonl", "{\"id\":\"rr_old_2\",\"start\":\"2026-06-10T10:00:00Z\"}\r\n");
        WriteText(recDir / "2026-07.jsonl", "{\"id\":\"rr_old_3\",\"start\":\"2026-07-10T10:00:00Z\"}\r\n");
        WriteText(recDir / "2026-11.jsonl", "{\"id\":\"rr_j_7f3a_2\",\"start\":\"2026-11-10T09:02:11Z\"}\r\n");

        // Create L2 notes and L3 knowledge
        WriteText(memDir / "notes.jsonl",
                  "{\"id\":\"nt_1\",\"schema\":\"hancho.memory.note/1\",\"text\":\"note 1\",\"status\":\"active\"}\r\n");
        WriteText(memDir / "knowledge.jsonl",
                  "{\"id\":\"kn_1\",\"schema\":\"hancho.memory.note/1\",\"text\":\"knowledge 1\",\"status\":\"active\"}\r\n");

        int64_t fakeNow = 1794700800LL; // approx 2026-11-15 00:00:00 UTC

        // 1. Dry run prune (90 days retention)
        memory::PruneOptions dryOpt;
        dryOpt.dryRun = true;
        dryOpt.retentionDays = 90;
        dryOpt.nowEpoch = fakeNow;

        memory::PruneResult dryRes = memory::PruneRunRecords(dryOpt);
        Check(dryRes.ok, "dry-run prune succeeds");
        Check(dryRes.dryRun, "result indicates dryRun == true");
        Check(dryRes.prunedFiles.size() == 3, "dry-run identifies 3 old month files (2026-05, 06, 07)");
        Check(fs::exists(recDir / "2026-05.jsonl"), "dry-run does not delete 2026-05.jsonl");
        Check(fs::exists(recDir / "2026-06.jsonl"), "dry-run does not delete 2026-06.jsonl");
        Check(fs::exists(recDir / "2026-07.jsonl"), "dry-run does not delete 2026-07.jsonl");
        Check(fs::exists(recDir / "2026-11.jsonl"), "dry-run keeps 2026-11.jsonl");

        // 2. Live prune (90 days retention)
        memory::PruneOptions liveOpt;
        liveOpt.dryRun = false;
        liveOpt.retentionDays = 90;
        liveOpt.nowEpoch = fakeNow;

        memory::PruneResult liveRes = memory::PruneRunRecords(liveOpt);
        Check(liveRes.ok, "live prune succeeds");
        Check(!liveRes.dryRun, "result indicates live prune");
        Check(liveRes.prunedFiles.size() == 3, "live prune pruned 3 month files");
        Check(!fs::exists(recDir / "2026-05.jsonl"), "2026-05.jsonl is deleted");
        Check(!fs::exists(recDir / "2026-06.jsonl"), "2026-06.jsonl is deleted");
        Check(!fs::exists(recDir / "2026-07.jsonl"), "2026-07.jsonl is deleted");
        Check(fs::exists(recDir / "2026-11.jsonl"), "2026-11.jsonl remains on disk");
        Check(fs::exists(memDir / "notes.jsonl"), "L2 notes.jsonl is NEVER deleted by prune");
        Check(fs::exists(memDir / "knowledge.jsonl"), "L3 knowledge.jsonl is NEVER deleted by prune");

        // 3. Rebuild after prune preserves remaining records without error
        json reb = Parse(memory::RebuildRunRecordsJson());
        Check(reb.value("ok", false), "rebuild succeeds after prune");
        Check(fs::exists(recDir / "2026-11.jsonl"), "2026-11.jsonl remains intact after rebuild");
        Check(ReadText(recDir / "2026-11.jsonl").find("rr_j_7f3a_2") != std::string::npos,
              "remaining records are preserved and not corrupted");

        // 4. Capacity limit: prune oldest L1 when limit is exceeded
        WriteText(recDir / "2026-09.jsonl", "{\"id\":\"rr_sep\",\"start\":\"2026-09-10T10:00:00Z\"}\r\n");
        WriteText(recDir / "2026-10.jsonl", "{\"id\":\"rr_oct\",\"start\":\"2026-10-10T10:00:00Z\"}\r\n");

        auto statsBeforeCap = memory::GetMemoryStorageStats();
        // Set capacity limit to only fit 2026-11 + L2 + L3, so 2026-09 must be pruned
        int64_t capLimit = statsBeforeCap.totalBytes - static_cast<int64_t>(fs::file_size(recDir / "2026-09.jsonl", ec)) - 5;

        memory::PruneOptions capOpt;
        capOpt.dryRun = false;
        capOpt.retentionDays = 0; // disable age prune, only test capacity
        capOpt.limitBytes = capLimit;

        memory::PruneResult capRes = memory::PruneRunRecords(capOpt);
        Check(capRes.ok, "capacity limit prune succeeds");
        Check(!fs::exists(recDir / "2026-09.jsonl"), "oldest L1 (2026-09) deleted on capacity overflow");
        Check(fs::exists(recDir / "2026-11.jsonl"), "newer L1 (2026-11) preserved within capacity");
        Check(fs::exists(memDir / "notes.jsonl"), "L2 notes.jsonl preserved during capacity prune");
        Check(fs::exists(memDir / "knowledge.jsonl"), "L3 knowledge.jsonl preserved during capacity prune");

        // 5. Overflow beyond L1: L2 and L3 must NOT be deleted even when limit is exceeded
        memory::PruneOptions tinyOpt;
        tinyOpt.dryRun = false;
        tinyOpt.retentionDays = 0;
        tinyOpt.limitBytes = 1; // Tiny limit: 1 byte

        memory::PruneResult tinyRes = memory::PruneRunRecords(tinyOpt);
        Check(tinyRes.ok, "tiny limit prune completes");
        Check(tinyRes.exceeded, "exceeded flag is set when storage exceeds limit");
        Check(!tinyRes.warning.empty(), "warning message is set when storage exceeds limit");
        Check(fs::exists(memDir / "notes.jsonl"), "L2 notes.jsonl is NEVER deleted on capacity overflow");
        Check(fs::exists(memDir / "knowledge.jsonl"), "L3 knowledge.jsonl is NEVER deleted on capacity overflow");

        // 6. Storage stats verification
        auto finalStats = memory::GetMemoryStorageStats();
        Check(finalStats.l2Bytes > 0, "storage stats reports L2 bytes");
        Check(finalStats.l3Bytes > 0, "storage stats reports L3 bytes");
        Check(finalStats.totalBytes >= finalStats.l2Bytes + finalStats.l3Bytes, "total bytes includes all layers");
    }

    fs::remove_all(temp, ec);
    SetDataDirectoryOverride(L"");

    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
