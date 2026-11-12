#include "../src/memory_search.h"
#include "../src/common.h"
#include "../src/nlohmann/json.hpp"

#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
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
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
}

static std::string ReadText(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static json Parse(const std::string& text) {
    json j = json::parse(text, nullptr, false);
    return j.is_discarded() ? json::object() : j;
}

static std::string FirstId(const json& response) {
    auto results = response.find("results");
    if (results == response.end() || !results->is_array() || results->empty()) return std::string();
    const json& first = results->front();
    auto id = first.find("id");
    if (id == first.end() || !id->is_string()) return std::string();
    return id->get<std::string>();
}

class FavourRanker : public memory::IMemoryRanker {
public:
    const char* Name() const override { return "test-favour"; }
    bool Rank(const std::string&,
              const std::vector<memory::RankCandidate>& candidates,
              std::vector<double>& outScores,
              std::string& outError) const override {
        outError.clear();
        outScores.assign(candidates.size(), 0.0);
        for (size_t i = 0; i < candidates.size(); ++i) {
            outScores[i] = candidates[i].id == "rr_2" ? 10.0 : 0.0;
        }
        return true;
    }
};

int main() {
    SetConsoleOutputCP(CP_UTF8);

    fs::path temp = fs::temp_directory_path() /
                    ("hancho_memory_search_smoke_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    fs::remove_all(temp, ec);
    fs::create_directories(temp / "memory" / "records", ec);
    SetDataDirectoryOverride(temp.wstring());

    {
        json run1 = {
            {"schema", "hancho.memory.run/1"},
            {"id", "rr_1"},
            {"jobName", "build"},
            {"taskqueue", "main"},
            {"engine", "claude"},
            {"model", "claude-opus-5-5"},
            {"goal", "Release build passes"},
            {"outcome", "failed"},
            {"failure", {{"class", "success_check_failed"}, {"summary", "build/app.exe was not updated"}}},
            {"start", "2026-11-10T09:02:11Z"},
            {"end", "2026-11-10T09:14:40Z"},
            {"durationSec", 749},
            {"exitCode", 0},
            {"filesEdited", {"src/net.cpp"}},
            {"errors", {"LNK2019 unresolved external symbol __imp_strerror"}},
            {"usage", {{"totalTokens", 1000}, {"costUsd", 0.5}}},
            {"sources", {{"eventsFile", "proc_12_build.events.jsonl"}, {"seqRange", {1, 220}}}}};
        json run2 = {
            {"schema", "hancho.memory.run/1"},
            {"id", "rr_2"},
            {"jobName", "test"},
            {"taskqueue", "main"},
            {"engine", "codex"},
            {"model", "gpt-5-codex"},
            {"goal", "Run the unit tests"},
            {"outcome", "success"},
            {"start", "2026-11-09T08:00:00Z"},
            {"end", "2026-11-09T08:02:00Z"},
            {"durationSec", 120},
            {"exitCode", 0},
            {"filesRead", {"src/a.cpp"}},
            {"usage", {{"totalTokens", 500}, {"costUsd", 0.25}}}};
        json run3 = {
            {"schema", "hancho.memory.run/1"},
            {"id", "rr_3"},
            {"taskqueue", "main"},
            {"goal", "One-off command outside a queue"},
            {"outcome", "success"},
            {"start", "2026-11-09T09:00:00Z"},
            {"end", "2026-11-09T09:01:00Z"},
            {"durationSec", 60},
            {"exitCode", 0}};
        WriteText(temp / "memory" / "records" / "2026-11.jsonl",
                  run1.dump() + "\n" + run2.dump() + "\n" + run3.dump() + "\n");

        json note1 = {
            {"schema", "hancho.memory.note/1"},
            {"id", "nt_1"},
            {"text", "libfoo is built with /MD; rebuild it with /MT or linking fails"},
            {"tags", {"build", "link"}},
            {"scope", "job:build"},
            {"author", {{"type", "agent"}}},
            {"sources", {"rr_1"}},
            {"status", "active"},
            {"createdAt", "2026-11-10T09:15:00Z"}};
        json note2 = {
            {"schema", "hancho.memory.note/1"},
            {"id", "nt_2"},
            {"text", "temporary note that will be retracted"},
            {"scope", "file:src/net.cpp"},
            {"status", "active"},
            {"createdAt", "2026-11-10T09:16:00Z"}};
        json retract = {{"op", "retract"}, {"id", "nt_2"}};
        WriteText(temp / "memory" / "notes.jsonl",
                  note1.dump() + "\n" + note2.dump() + "\n" + retract.dump() + "\n");

        json kn1 = {
            {"schema", "hancho.memory.knowledge/1"},
            {"id", "kn_1"},
            {"text", "Link every static library with /MT in this project"},
            {"tags", {"build", "link", "msvc"}},
            {"scope", "project"},
            {"approvedBy", "alice"},
            {"approvedAt", "2026-11-08T10:00:00Z"},
            {"sources", {"nt_1"}}};
        WriteText(temp / "memory" / "knowledge.jsonl", kn1.dump() + "\n");
    }

    {
        json r = Parse(memory::RebuildJson());
        Check(r.value("ok", false), "memory rebuild succeeds");
        Check(r.value("docCount", 0) == 6, "rebuild indexes 6 documents");
        Check(fs::exists(temp / "memory" / "index" / "index.json"), "index file is written");
    }

    {
        memory::Query q;
        q.text = "libfoo /MD";
        json r = Parse(memory::SearchJson(q));
        Check(r.value("ok", false), "search by keyword succeeds");
        Check(FirstId(r) == "nt_1", "keyword search finds the libfoo note first");
        auto results = r.find("results");
        Check(results != r.end() && results->is_array() && !results->empty() &&
                  results->front().value("source", json::object()).value("file", std::string()) ==
                      "memory/notes.jsonl",
              "search hit carries its source file");
    }

    {
        memory::Query q;
        q.text = "failed";
        json r = Parse(memory::SearchJson(q));
        Check(FirstId(r) == "rr_1", "failure query ranks the failed run first");
        auto results = r.find("results");
        Check(results != r.end() && !results->empty() &&
                  results->front().value("failureClass", std::string()) == "success_check_failed",
              "failed run exposes failureClass");
    }

    {
        memory::Query q;
        q.file = "src/net.cpp";
        json r = Parse(memory::SearchJson(q));
        Check(r.value("totalMatched", 0) == 1, "file filter matches only the edited file");
        Check(FirstId(r) == "rr_1", "file filter returns the run that edited the file");
    }

    {
        memory::Query q;
        q.text = "/MT";
        q.kinds = {"knowledge"};
        json r = Parse(memory::SearchJson(q));
        Check(FirstId(r) == "kn_1", "kind filter restricts to L3 knowledge");
    }

    {
        memory::Query q;
        q.text = "retracted";
        q.kinds = {"note"};
        json r = Parse(memory::SearchJson(q));
        Check(r.value("totalMatched", 0) == 0, "retracted notes are not searchable");
    }

    {
        json r = Parse(memory::GetJson("rr_1"));
        Check(r.value("ok", false), "memory get finds a run record");
        auto doc = r.find("document");
        Check(doc != r.end() && doc->value("id", std::string()) == "rr_1", "get returns the full run JSON");
        auto source = r.find("source");
        Check(source != r.end() && source->value("file", std::string()) == "memory/records/2026-11.jsonl" &&
                  source->value("line", 0) == 1,
              "get reports source file and line");
        auto sources = r.find("sources");
        Check(sources != r.end() && sources->value("eventsFile", std::string()) == "proc_12_build.events.jsonl",
              "get keeps the run's event source");
    }

    {
        json r = Parse(memory::GetJson("nt_2"));
        Check(r.value("ok", false), "memory get resolves retracted notes");
        Check(r.value("status", std::string()) == "retracted", "retracted note keeps its status");
    }

    {
        json r = Parse(memory::StatsJson(memory::Query()));
        Check(r.value("ok", false), "memory stats succeeds");
        Check(r.value("runs", 0) == 3, "stats counts three runs");
        Check(r.value("successes", 0) == 2, "stats counts two successes");
        Check(r.value("totalTokens", 0) == 1500, "stats sums run tokens");
        auto failures = r.find("failures");
        Check(failures != r.end() && failures->is_array() && failures->size() == 1 &&
                  failures->front().value("class", std::string()) == "success_check_failed",
              "stats lists failure classes");
        auto byJob = r.find("byJobName");
        Check(byJob != r.end() && byJob->is_array() && byJob->size() == 2,
              "stats groups by job name");
        auto storage = r.find("storage");
        Check(storage != r.end() && storage->is_object(), "stats includes storage object");
        Check(storage->value("totalBytes", 0LL) > 0, "storage totalBytes > 0");
        Check(r.contains("totalBytes") && r.contains("limitBytes"), "stats contains top-level totalBytes and limitBytes");
    }

    {
        fs::remove_all(temp / "memory" / "index", ec);
        memory::Query q;
        q.text = "unit tests";
        json r = Parse(memory::SearchJson(q));
        Check(r.value("ok", false) && r.value("indexRebuilt", false), "search rebuilds a missing index");
    }

    {
        fs::remove_all(temp / "memory" / "index", ec);
        json r = Parse(memory::RebuildJson());
        Check(r.value("ok", false) && r.value("docCount", 0) == 6, "rebuild restores the deleted index");
        Check(fs::exists(temp / "memory" / "index" / "index.json"), "restored index file exists");
    }

    {
        memory::Query q;
        q.text = "run";
        q.kinds = {"run"};
        FavourRanker ranker;
        json r = Parse(memory::SearchJson(q, &ranker));
        Check(r.value("ok", false), "external ranker call succeeds");
        Check(FirstId(r) == "rr_2", "IMemoryRanker scores drive the order");
    }

    {
        // 055-1: Test external ranker plugin protocol and config.json integration
#ifdef _WIN32
        fs::path pluginScript = temp / "test_ranker.bat";
        WriteText(pluginScript, "@echo {\"scores\": {\"rr_2\": 99.0}}\r\n");
#else
        fs::path pluginScript = temp / "test_ranker.sh";
        WriteText(pluginScript, "#!/bin/sh\necho '{\"scores\": {\"rr_2\": 99.0}}'\n");
        ::chmod(pluginScript.string().c_str(), 0755);
#endif
        // 1. Configure in config.json with relative path inside data directory
        json cfg = {{"ranker_plugin", pluginScript.filename().string()}};
        WriteText(temp / "config.json", cfg.dump() + "\n");

        memory::Query q;
        q.text = "run";
        q.kinds = {"run"};
        json r = Parse(memory::SearchJson(q));
        Check(r.value("ok", false), "configured plugin ranker call succeeds");
        Check(FirstId(r) == "rr_2", "configured plugin scores drive the order");
        Check(r.value("ranker", std::string()) == "process-plugin", "response indicates process-plugin was used");

        // 2. Direct ProcessRanker instance
        memory::ProcessRanker directRanker(pluginScript.string());
        json rDirect = Parse(memory::SearchJson(q, &directRanker));
        Check(rDirect.value("ok", false), "direct ProcessRanker succeeds");
        Check(FirstId(rDirect) == "rr_2", "direct ProcessRanker drives the order");

        // 3. Fallback when configured plugin fails / does not exist
        json brokenCfg = {{"ranker_plugin", "nonexistent_plugin_xyz.exe"}};
        WriteText(temp / "config.json", brokenCfg.dump() + "\n");
        json rFallback = Parse(memory::SearchJson(q));
        Check(rFallback.value("ok", false), "broken ranker plugin falls back gracefully");
        Check(rFallback.value("ranker", std::string()) == "builtin", "fallback uses builtin ranking");

        // Clean up
        fs::remove(temp / "config.json", ec);
        fs::remove(pluginScript, ec);
    }

    {
        // 055-2: Comprehensive tests for ranker plugin fallback, timeouts, invalid outputs,
        // configuration switching, and sample plugin verification.
        memory::Query q;
        q.text = "run";
        q.kinds = {"run"};

        // 1. Dynamic switching: verify builtin when unconfigured
        {
            fs::remove(temp / "config.json", ec);
            json r = Parse(memory::SearchJson(q));
            Check(r.value("ok", false), "unconfigured search succeeds");
            Check(r.value("ranker", std::string()) == "builtin", "unconfigured search uses builtin ranker");
        }

        // 2. Fallback on non-existent plugin (startup failure)
        {
            json cfg = {{"ranker_plugin", "nonexistent_plugin_xyz.exe"}};
            WriteText(temp / "config.json", cfg.dump() + "\n");
            json r = Parse(memory::SearchJson(q));
            Check(r.value("ok", false), "startup failure plugin falls back gracefully");
            Check(r.value("ranker", std::string()) == "builtin", "startup failure falls back to builtin ranker");
            Check(r.contains("rankerWarning"), "startup failure includes rankerWarning");
        }

        // 3. Fallback on abnormal exit code
        {
#ifdef _WIN32
            fs::path failScript = temp / "fail_exit.bat";
            WriteText(failScript, "@echo internal plugin error>&2\r\n@exit /b 1\r\n");
#else
            fs::path failScript = temp / "fail_exit.sh";
            WriteText(failScript, "#!/bin/sh\nexit 1\n");
            ::chmod(failScript.string().c_str(), 0755);
#endif
            json cfg = {{"ranker_plugin", failScript.filename().string()}};
            WriteText(temp / "config.json", cfg.dump() + "\n");
            json r = Parse(memory::SearchJson(q));
            Check(r.value("ok", false), "abnormal exit code plugin falls back gracefully");
            Check(r.value("ranker", std::string()) == "builtin", "abnormal exit code falls back to builtin ranker");
            Check(r.contains("rankerWarning"), "abnormal exit code includes rankerWarning");
            fs::remove(failScript, ec);
        }

        // 4. Fallback on timeout
        {
#ifdef _WIN32
            fs::path slowScript = temp / "slow_ranker.bat";
            WriteText(slowScript, "@ping 127.0.0.1 -n 3 >nul\r\n@echo {\"scores\": {\"rr_2\": 99.0}}\r\n");
#else
            fs::path slowScript = temp / "slow_ranker.sh";
            WriteText(slowScript, "#!/bin/sh\nsleep 2\necho '{\"scores\": {\"rr_2\": 99.0}}'\n");
            ::chmod(slowScript.string().c_str(), 0755);
#endif
            json cfg = {{"ranker_plugin", {{"path", slowScript.filename().string()}, {"timeoutMs", 200}}}};
            WriteText(temp / "config.json", cfg.dump() + "\n");
            json r = Parse(memory::SearchJson(q));
            Check(r.value("ok", false), "timed out plugin falls back gracefully");
            Check(r.value("ranker", std::string()) == "builtin", "timed out plugin falls back to builtin ranker");
            Check(r.contains("rankerWarning"), "timed out plugin includes rankerWarning");
            std::string warn = r.value("rankerWarning", "");
            Check(warn.find("timed out") != std::string::npos, "rankerWarning mentions timeout");
            fs::remove(slowScript, ec);
        }

        // 5. Fallback on malformed / non-JSON output
        {
#ifdef _WIN32
            fs::path badJsonScript = temp / "bad_json.bat";
            WriteText(badJsonScript, "@echo NOT_VALID_JSON_AT_ALL\r\n");
#else
            fs::path badJsonScript = temp / "bad_json.sh";
            WriteText(badJsonScript, "#!/bin/sh\necho 'NOT_VALID_JSON_AT_ALL'\n");
            ::chmod(badJsonScript.string().c_str(), 0755);
#endif
            json cfg = {{"ranker_plugin", badJsonScript.filename().string()}};
            WriteText(temp / "config.json", cfg.dump() + "\n");
            json r = Parse(memory::SearchJson(q));
            Check(r.value("ok", false), "non-JSON output plugin falls back gracefully");
            Check(r.value("ranker", std::string()) == "builtin", "non-JSON output falls back to builtin ranker");
            Check(r.contains("rankerWarning"), "non-JSON output includes rankerWarning");
            fs::remove(badJsonScript, ec);
        }

        // 6. Fallback on explicit JSON error response
        {
#ifdef _WIN32
            fs::path errScript = temp / "error_response.bat";
            WriteText(errScript, "@echo {\"error\": \"plugin internal database error\"}\r\n");
#else
            fs::path errScript = temp / "error_response.sh";
            WriteText(errScript, "#!/bin/sh\necho '{\"error\": \"plugin internal database error\"}'\n");
            ::chmod(errScript.string().c_str(), 0755);
#endif
            json cfg = {{"ranker_plugin", errScript.filename().string()}};
            WriteText(temp / "config.json", cfg.dump() + "\n");
            json r = Parse(memory::SearchJson(q));
            Check(r.value("ok", false), "explicit error plugin falls back gracefully");
            Check(r.value("ranker", std::string()) == "builtin", "explicit error falls back to builtin ranker");
            Check(r.value("rankerWarning", "") == "plugin internal database error", "rankerWarning contains plugin error string");
            fs::remove(errScript, ec);
        }

        // 7. Fallback on candidate count mismatch
        {
#ifdef _WIN32
            fs::path countMismatchScript = temp / "mismatch.bat";
            WriteText(countMismatchScript, "@echo [1.0]\r\n");
#else
            fs::path countMismatchScript = temp / "mismatch.sh";
            WriteText(countMismatchScript, "#!/bin/sh\necho '[1.0]'\n");
            ::chmod(countMismatchScript.string().c_str(), 0755);
#endif
            json cfg = {{"ranker_plugin", countMismatchScript.filename().string()}};
            WriteText(temp / "config.json", cfg.dump() + "\n");
            json r = Parse(memory::SearchJson(q));
            Check(r.value("ok", false), "score count mismatch falls back gracefully");
            Check(r.value("ranker", std::string()) == "builtin", "score count mismatch falls back to builtin ranker");
            fs::remove(countMismatchScript, ec);
        }

        // 8. Direct ProcessRanker with broken script falls back without stopping search
        {
            memory::ProcessRanker brokenDirect("nonexistent_direct_ranker.exe");
            json r = Parse(memory::SearchJson(q, &brokenDirect));
            Check(r.value("ok", false), "direct broken ProcessRanker falls back gracefully");
            Check(r.value("ranker", std::string()) == "builtin", "direct broken ProcessRanker uses builtin");
            Check(r.contains("rankerWarning"), "direct broken ProcessRanker provides rankerWarning");
        }

        // 9. Sample ranker plugin (tests/data/sample_ranker.bat or .sh)
        {
#ifdef _WIN32
            fs::path sampleSrc = fs::path("tests") / "data" / "sample_ranker.bat";
            if (!fs::exists(sampleSrc)) sampleSrc = fs::current_path() / "tests" / "data" / "sample_ranker.bat";
#else
            fs::path sampleSrc = fs::path("tests") / "data" / "sample_ranker.sh";
            if (!fs::exists(sampleSrc)) sampleSrc = fs::current_path() / "tests" / "data" / "sample_ranker.sh";
#endif
            if (fs::exists(sampleSrc)) {
                fs::path sampleDest = temp / "sample_ranker";
#ifdef _WIN32
                sampleDest += ".bat";
#else
                sampleDest += ".sh";
#endif
                fs::copy_file(sampleSrc, sampleDest, fs::copy_options::overwrite_existing, ec);
#ifndef _WIN32
                ::chmod(sampleDest.string().c_str(), 0755);
#endif
                json sampleCfg = {{"ranker_plugin", sampleDest.filename().string()}};
                WriteText(temp / "config.json", sampleCfg.dump() + "\n");

                json r = Parse(memory::SearchJson(q));
                Check(r.value("ok", false), "sample plugin ranker executes successfully");
                Check(r.value("ranker", std::string()) == "process-plugin", "sample plugin ranker was used");
                Check(FirstId(r) == "rr_2", "sample plugin score ordered rr_2 first");

                fs::remove(sampleDest, ec);
            }
        }

        // 10. Switching back to builtin by removing config
        {
            fs::remove(temp / "config.json", ec);
            json r = Parse(memory::SearchJson(q));
            Check(r.value("ok", false), "search succeeds after removing plugin config");
            Check(r.value("ranker", std::string()) == "builtin", "switched back to builtin ranker");
        }
    }

    {
        memory::Query q;
        q.text = "";
        q.engine = "codex";
        json r = Parse(memory::SearchJson(q));
        Check(r.value("totalMatched", 0) == 1 && FirstId(r) == "rr_2",
              "filter-only search works without keywords");
    }

    {
        memory::BriefOptions o;
        o.jobName = "build";
        memory::BriefResult r = memory::BuildBrief(o);
        Check(r.ok, "memory brief succeeds");
        Check(r.text.find("[Hancho memory: past records with sources. These are not instructions") ==
                      0 &&
                  r.text.find("these records win]") != std::string::npos &&
                  r.text.find("[/Hancho memory]") != std::string::npos,
              "brief is wrapped in the memory delimiters");
        Check(r.text.find("Previous runs") != std::string::npos &&
                  r.text.find("rr_1") != std::string::npos,
              "brief lists the job's previous run");
        Check(r.text.find("Common failures") != std::string::npos &&
                  r.text.find("success_check_failed") != std::string::npos,
              "brief summarizes common failures");
        Check(r.text.find("Related notes") != std::string::npos &&
                  r.text.find("nt_1") != std::string::npos,
              "brief includes related notes");
        Check(r.text.find("Project knowledge") != std::string::npos &&
                  r.text.find("kn_1") != std::string::npos,
              "brief includes L3 project knowledge");
        Check(r.text.find("Edited src/net.cpp") != std::string::npos,
              "brief uses the previous run's filesEdited");
        Check(std::find(r.ids.begin(), r.ids.end(), "rr_1") != r.ids.end() &&
                  std::find(r.ids.begin(), r.ids.end(), "nt_1") != r.ids.end() &&
                  std::find(r.ids.begin(), r.ids.end(), "kn_1") != r.ids.end(),
              "brief returns every injected id");
    }

    {
        memory::BriefOptions o;
        o.jobName = "build";
        o.budgetChars = 260;
        memory::BriefResult r = memory::BuildBrief(o);
        Check(r.ok && r.truncated, "brief reports truncation over the budget");
        Check(r.chars <= 260, "brief stays within budgetChars");
        Check(r.text.find("kn_1") != std::string::npos,
              "budget trimming keeps L3 project knowledge to the end");
        Check(std::find(r.ids.begin(), r.ids.end(), "kn_1") != r.ids.end(),
              "trimmed brief still lists the knowledge id");
    }

    {
        memory::BriefOptions o;
        o.jobName = "build";
        o.includeAgentNotes = false;
        memory::BriefResult r = memory::BuildBrief(o);
        Check(r.text.find("nt_1") == std::string::npos, "agent-authored notes can be excluded");
        Check(r.text.find("kn_1") != std::string::npos, "excluding agent notes keeps L3");
    }

    {
        memory::BriefOptions o;
        o.jobName = "build";
        json r = Parse(memory::BriefJson(o));
        Check(r.value("ok", false) && r.value("text", std::string()).find("kn_1") != std::string::npos,
              "memory brief JSON returns the block");
        auto ids = r.find("ids");
        Check(ids != r.end() && ids->is_array() && !ids->empty(), "memory brief JSON returns ids");
    }

    {
        json r = Parse(memory::ApproveJson("nt_1", "alice"));
        Check(r.value("ok", false), "memory approve promotes an L2 note to L3");
        std::string knId = r.value("id", std::string());
        Check(!knId.empty(), "approve returns the new knowledge id");
        Check(r.value("approvedBy", std::string()) == "alice", "approve records approvedBy");

        std::ifstream in(temp / "memory" / "knowledge.jsonl", std::ios::binary);
        std::string line;
        json kn = json::object();
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            json parsed = json::parse(line, nullptr, false);
            if (!parsed.is_discarded() && parsed.value("id", std::string()) == knId) kn = parsed;
        }
        Check(kn.value("schema", std::string()) == "hancho.memory.knowledge/1" &&
                  kn.value("approvedBy", std::string()) == "alice" &&
                  !kn.value("approvedAt", std::string()).empty(),
              "knowledge.jsonl carries approvedBy and approvedAt");
        Check(kn.value("promotedFrom", std::string()) == "nt_1",
              "knowledge records the note it was promoted from");

        json note = Parse(memory::GetJson("nt_1", "note"));
        Check(note.value("status", std::string()) == "superseded", "promoted note is retired");
        json got = Parse(memory::GetJson(knId, "knowledge"));
        Check(got.value("ok", false), "promoted knowledge can be fetched by id");
    }

    {
        json r = Parse(memory::ApproveJson("nt_2", "alice"));
        Check(!r.value("ok", true), "a retracted note cannot be promoted");
        json run = Parse(memory::ApproveJson("rr_1", "alice"));
        Check(!run.value("ok", true), "a run record cannot be promoted");
        json tool = Parse(memory::HandleToolCall("memory_approve", R"({"id":"nt_1"})"));
        Check(!tool.value("ok", true) &&
                  tool.value("error", std::string()).find("human-only") != std::string::npos,
              "the MCP boundary refuses L3 promotion");
    }

    {
        std::ofstream out(temp / "memory" / "knowledge.jsonl", std::ios::binary | std::ios::app);
        json unapproved = {{"schema", "hancho.memory.knowledge/1"},
                           {"id", "kn_unapproved"},
                           {"text", "unapproved secret sauce"},
                           {"scope", "project"},
                           {"status", "active"}};
        out << unapproved.dump() << "\n";
        out.close();

        memory::Query q;
        q.text = "unapproved secret sauce";
        q.kinds = {"knowledge"};
        json r = Parse(memory::SearchJson(q));
        Check(r.value("totalMatched", 0) == 0, "knowledge without approvedBy is not active L3");
    }

    // L2 notes: append, replay, redaction, dedupe and agent write discipline (3.4, 3.10).
    std::string redactedNoteId;
    {
        memory::NoteInput n;
        n.text = "use sk-abc1234567890 and password=hunter2 for the mirror";
        n.scope = "taskqueue:main";
        n.authorType = "human";
        n.sources = {"rr_1"};
        json r = Parse(memory::NoteJson(n));
        Check(r.value("ok", false), "memory note appends an L2 note");
        Check(r.value("redacted", false), "memory note reports redaction");
        redactedNoteId = r.value("id", std::string());
        Check(!redactedNoteId.empty(), "memory note returns its id");
        std::string raw = ReadFileUtf8Wide((temp / "memory" / "notes.jsonl").wstring());
        Check(raw.find("sk-abc1234567890") == std::string::npos &&
                  raw.find("hunter2") == std::string::npos,
              "secrets are redacted before hitting notes.jsonl");
        json got = Parse(memory::GetJson(redactedNoteId, "note"));
        Check(got.value("status", std::string()) == "active", "new note is active");
    }

    {
        memory::NoteInput n;
        n.text = "use sk-abc1234567890 and password=hunter2 for the mirror";
        n.scope = "taskqueue:main";
        n.authorType = "human";
        n.sources = {"rr_1"};
        json r = Parse(memory::NoteJson(n));
        Check(r.value("ok", false) && r.value("duplicate", false),
              "same scope and normalized text is not stacked");
        Check(r.value("id", std::string()) == redactedNoteId, "duplicate returns the existing note");
    }

    {
        memory::NoteInput n;
        n.text = "bad scope";
        n.scope = "workspace:main";
        n.authorType = "human";
        json r = Parse(memory::NoteJson(n));
        Check(!r.value("ok", true), "unknown note scope is rejected");
    }

    {
        memory::NoteInput n;
        n.text = "agent fact without sources";
        n.scope = "project";
        n.authorType = "agent";
        json r = Parse(memory::NoteJson(n));
        Check(!r.value("ok", true), "agent note without sources is rejected");

        n.sources = {"rr_missing"};
        json missing = Parse(memory::NoteJson(n));
        Check(!missing.value("ok", true) &&
                  missing.value("error", std::string()).find("source id does not exist") !=
                      std::string::npos,
              "agent note with an unknown source is rejected");
    }

    {
        bool allOk = true;
        for (int i = 0; i < 5; ++i) {
            memory::NoteInput n;
            n.text = "attempt fact " + std::to_string(i);
            n.scope = "job:test";
            n.authorType = "agent";
            n.sources = {"rr_2"};
            json r = Parse(memory::NoteJson(n));
            if (!r.value("ok", false)) allOk = false;
        }
        Check(allOk, "agent can write 5 notes in one attempt");
        memory::NoteInput sixth;
        sixth.text = "attempt fact overflow";
        sixth.scope = "job:test";
        sixth.authorType = "agent";
        sixth.sources = {"rr_2"};
        json r = Parse(memory::NoteJson(sixth));
        Check(!r.value("ok", true) &&
                  r.value("error", std::string()).find("5 notes per attempt") != std::string::npos,
              "agent notes are capped at 5 per attempt");
    }

    {
        memory::NoteInput n;
        n.text = "derived scope fact";
        n.authorType = "agent";
        n.sources = {"rr_1"};
        json r = Parse(memory::NoteJson(n));
        Check(r.value("ok", false) && r.value("scope", std::string()) == "job:build",
              "agent note without a scope lands in its attempt's job");
    }

    {
        memory::NoteInput n;
        n.text = "no job to derive from";
        n.authorType = "agent";
        n.sources = {"rr_3"};
        json r = Parse(memory::NoteJson(n));
        Check(!r.value("ok", true) &&
                  r.value("error", std::string()).find("specify scope explicitly") != std::string::npos,
              "agent note whose run has no job name asks for an explicit scope");
    }

    {
        memory::NoteInput n;
        n.text = "human default scope fact";
        n.authorType = "human";
        json r = Parse(memory::NoteJson(n));
        Check(r.value("ok", false) && r.value("scope", std::string()) == "project",
              "human note without a scope still defaults to project");
    }

    std::string supersededId;
    std::string replacementId;
    {
        memory::NoteInput oldNote;
        oldNote.text = "libfoo is built with /MD";
        oldNote.scope = "project";
        oldNote.authorType = "human";
        json created = Parse(memory::NoteJson(oldNote));
        Check(created.value("ok", false), "baseline note is created");
        supersededId = created.value("id", std::string());

        memory::NoteInput newNote;
        newNote.text = "libfoo is rebuilt with /MT";
        newNote.scope = "project";
        newNote.authorType = "human";
        newNote.supersedes = supersededId;
        json replaced = Parse(memory::NoteJson(newNote));
        Check(replaced.value("ok", false), "superseding note is created");
        replacementId = replaced.value("id", std::string());
        json oldBody = Parse(memory::GetJson(supersededId, "note"));
        Check(oldBody.value("status", std::string()) == "superseded",
              "superseded note is retired on replay");
        json newBody = Parse(memory::GetJson(replacementId, "note"));
        Check(newBody.value("status", std::string()) == "active", "replacement note stays active");
    }

    {
        json r = Parse(memory::RetractJson(replacementId, "tester", "no longer true"));
        Check(r.value("ok", false), "memory retract appends a retraction");
        json got = Parse(memory::GetJson(replacementId, "note"));
        Check(got.value("status", std::string()) == "retracted", "retracted note is retired on replay");
        json again = Parse(memory::RetractJson(replacementId, "tester", ""));
        Check(again.value("ok", false) && again.value("alreadyRetracted", false),
              "retracting twice is idempotent");
        json run = Parse(memory::RetractJson("rr_1", "tester", ""));
        Check(!run.value("ok", true), "a run record cannot be retracted as a note");
    }

    std::string expiredId;
    {
        memory::NoteInput n;
        n.text = "this note expired long ago";
        n.scope = "project";
        n.authorType = "human";
        n.expiresAt = "2000-01-01T00:00:00Z";
        json created = Parse(memory::NoteJson(n));
        Check(created.value("ok", false), "note with expiresAt is created");
        expiredId = created.value("id", std::string());
        json got = Parse(memory::GetJson(expiredId, "note"));
        Check(got.value("status", std::string()) == "expired", "expired note is inactive on replay");
        memory::Query q;
        q.text = "expired long ago";
        json search = Parse(memory::SearchJson(q));
        Check(search.dump().find(expiredId) == std::string::npos, "expired note is not searchable");
    }

    {
        Check(Parse(memory::RebuildJson()).value("ok", false), "memory index rebuilds after note writes");
        json oldBody = Parse(memory::GetJson(supersededId, "note"));
        json newBody = Parse(memory::GetJson(replacementId, "note"));
        json expired = Parse(memory::GetJson(expiredId, "note"));
        Check(oldBody.value("status", std::string()) == "superseded" &&
                  newBody.value("status", std::string()) == "retracted" &&
                  expired.value("status", std::string()) == "expired",
              "create/supersede/retract/expire replay from notes.jsonl");
    }

    // --- Task 072-2: Plain run search, stats aggregation, and rebuild ---
    {
        // Add plain run records to memory records
        fs::path recPath = temp / "memory" / "records" / "2026-11.jsonl";
        json plainRunSuccess = {
            {"schema", "hancho.memory.run/1"},
            {"id", "rr_plain_bench_1"},
            {"jobName", "bench"},
            {"taskqueue", "plain"},
            {"outcome", "success"},
            {"start", "2026-11-10T12:00:00Z"},
            {"end", "2026-11-10T12:05:00Z"},
            {"durationSec", 300},
            {"exitCode", 0},
            {"commands", json::array({"bench.exe --fast"})},
            {"goal", "Run benchmark tool"}
        };
        json plainRunFail = {
            {"schema", "hancho.memory.run/1"},
            {"id", "rr_plain_deploy_1"},
            {"jobName", "deploy"},
            {"taskqueue", "plain"},
            {"outcome", "failed"},
            {"failure", {{"class", "exit_nonzero"}, {"summary", "Exited with code 1."}}},
            {"start", "2026-11-10T13:00:00Z"},
            {"end", "2026-11-10T13:02:00Z"},
            {"durationSec", 120},
            {"exitCode", 1},
            {"commands", json::array({"deploy.cmd --prod"})},
            {"goal", "Deploy artifacts to prod"}
        };
        std::string existingRecs = ReadText(recPath);
        WriteText(recPath, existingRecs + plainRunSuccess.dump() + "\r\n" + plainRunFail.dump() + "\r\n");

        // Force rebuild index to pick up plain runs
        Check(Parse(memory::RebuildJson()).value("ok", false), "index rebuilds with plain runs included");

        // 1. memory search --kind run includes plain runs
        memory::Query qRun;
        qRun.kinds = {"run"};
        json resRun = Parse(memory::SearchJson(qRun));
        Check(resRun.value("ok", false), "search --kind run succeeds");
        std::string dumpRun = resRun.dump();
        Check(dumpRun.find("rr_plain_bench_1") != std::string::npos, "search --kind run finds rr_plain_bench_1");
        Check(dumpRun.find("rr_plain_deploy_1") != std::string::npos, "search --kind run finds rr_plain_deploy_1");

        // 2. memory search --taskqueue plain filters strictly to plain runs
        memory::Query qPlain;
        qPlain.taskqueue = "plain";
        json resPlain = Parse(memory::SearchJson(qPlain));
        Check(resPlain.value("ok", false), "search --taskqueue plain succeeds");
        Check(resPlain.value("count", 0) == 2, "search --taskqueue plain returns exactly 2 plain runs");
        std::string dumpPlain = resPlain.dump();
        Check(dumpPlain.find("rr_plain_bench_1") != std::string::npos &&
              dumpPlain.find("rr_plain_deploy_1") != std::string::npos,
              "search --taskqueue plain finds both plain runs");
        Check(dumpPlain.find("rr_1") == std::string::npos, "main queue run excluded when taskqueue=plain");

        // 3. memory search --failure-class exit_nonzero finds the failed plain run
        memory::Query qFail;
        qFail.failureClass = "exit_nonzero";
        json resFail = Parse(memory::SearchJson(qFail));
        Check(resFail.value("ok", false), "search --failure-class exit_nonzero succeeds");
        Check(resFail.dump().find("rr_plain_deploy_1") != std::string::npos,
              "failed plain run found by failure class exit_nonzero");

        // 4. memory stats includes plain runs in overall, byJob, and failures counts
        memory::Query qStats;
        json stats = Parse(memory::StatsJson(qStats));
        Check(stats.value("ok", false), "memory stats succeeds with plain runs");
        Check(stats.value("runs", 0) >= 4, "memory stats counts plain runs in total runs");

        bool foundDeployJob = false;
        bool foundBenchJob = false;
        for (const auto& job : stats.value("byJobName", json::array())) {
            std::string k = job.value("key", "");
            if (k == "deploy") foundDeployJob = true;
            if (k == "bench") foundBenchJob = true;
        }
        Check(foundDeployJob, "stats byJobName includes plain job 'deploy'");
        Check(foundBenchJob, "stats byJobName includes plain job 'bench'");

        bool foundExitNonzero = false;
        for (const auto& f : stats.value("failures", json::array())) {
            if (f.value("class", "") == "exit_nonzero") {
                foundExitNonzero = true;
                Check(f.value("count", 0) >= 1, "failure exit_nonzero has count >= 1");
            }
        }
        Check(foundExitNonzero, "stats failures includes exit_nonzero from failed plain run");
    }

    // 073: L3 worker promotion (doc/design-v3.md §7, 3.5, ww-data/tasks/073-l3-worker-promotion.md)
    {
        // 1. Create a note to test promotion
        json notePromo1 = {
            {"schema", "hancho.memory.note/1"},
            {"id", "nt_promo_1"},
            {"text", "Always run cmake before ctest in this project"},
            {"tags", {"build", "test"}},
            {"scope", "project"},
            {"author", {{"type", "agent"}}},
            {"sources", {"rr_1"}},
            {"status", "active"},
            {"createdAt", "2026-11-10T12:00:00Z"}
        };
        std::string existingNotes = ReadText(temp / "memory" / "notes.jsonl");
        WriteText(temp / "memory" / "notes.jsonl", existingNotes + notePromo1.dump() + "\r\n");
        Check(Parse(memory::RebuildJson()).value("ok", false), "rebuild index includes nt_promo_1");

        // When config.json is empty / no l3Promoters, worker promotion is refused
        json rUnauthWorker = Parse(memory::ApproveJson("nt_promo_1", "worker:codex"));
        Check(!rUnauthWorker.value("ok", true), "worker promotion fails when l3Promoters is not configured");
        Check(rUnauthWorker.value("error", "").find("not authorized") != std::string::npos,
              "unauthorized error mentions not authorized");

        // MCP without authorized worker fails
        json mcpNoWorker = Parse(memory::HandleToolCall("memory_approve", R"({"id":"nt_promo_1"})"));
        Check(!mcpNoWorker.value("ok", true) &&
                  mcpNoWorker.value("error", "").find("human-only") != std::string::npos,
              "MCP memory_approve without worker returns human-only error");

        json mcpUnauthWorker = Parse(memory::HandleToolCall("memory_approve", R"({"id":"nt_promo_1", "worker":"codex"})"));
        Check(!mcpUnauthWorker.value("ok", true) &&
                  mcpUnauthWorker.value("error", "").find("not authorized") != std::string::npos,
              "MCP memory_approve with unconfigured worker is refused");

        // 2. Configure l3Promoters in config.json
        json cfg = {
            {"memory", {
                {"l3Promoters", json::array({"codex", "reviewer"})}
            }}
        };
        WriteText(temp / "config.json", cfg.dump(2) + "\r\n");

        // Worker not in the list is still refused
        json rUnlisted = Parse(memory::ApproveJson("nt_promo_1", "worker:unauthorized_agent"));
        Check(!rUnlisted.value("ok", true), "unlisted worker cannot promote");
        Check(rUnlisted.value("error", "").find("not authorized") != std::string::npos,
              "unlisted worker error mentions not authorized");

        json mcpUnlisted = Parse(memory::HandleToolCall("memory_approve", R"({"id":"nt_promo_1", "worker":"unauthorized_agent"})"));
        Check(!mcpUnlisted.value("ok", true), "MCP unlisted worker cannot promote");

        // Authorized worker succeeds
        json rAuth = Parse(memory::ApproveJson("nt_promo_1", "worker:codex"));
        Check(rAuth.value("ok", false), "authorized worker can promote note to L3");
        std::string knId1 = rAuth.value("id", "");
        Check(!knId1.empty(), "promoted note returns new knowledge id");
        Check(rAuth.value("approvedBy", "") == "worker:codex", "approvedBy records worker:codex");

        // Verify knowledge.jsonl has approvedBy: "worker:codex"
        std::ifstream knFile(temp / "memory" / "knowledge.jsonl", std::ios::binary);
        std::string knLine;
        json knFound = json::object();
        while (std::getline(knFile, knLine)) {
            if (!knLine.empty() && knLine.back() == '\r') knLine.pop_back();
            if (knLine.empty()) continue;
            json parsed = json::parse(knLine, nullptr, false);
            if (!parsed.is_discarded() && parsed.value("id", "") == knId1) knFound = parsed;
        }
        Check(knFound.value("approvedBy", "") == "worker:codex", "knowledge.jsonl contains approvedBy: worker:codex");

        // 3. Create another note and promote via MCP tool call with worker "reviewer"
        json notePromo2 = {
            {"schema", "hancho.memory.note/1"},
            {"id", "nt_promo_2"},
            {"text", "Disable incremental linking on MSVC release builds"},
            {"tags", {"build", "link"}},
            {"scope", "project"},
            {"author", {{"type", "agent"}}},
            {"sources", {"rr_1"}},
            {"status", "active"},
            {"createdAt", "2026-11-10T12:05:00Z"}
        };
        existingNotes = ReadText(temp / "memory" / "notes.jsonl");
        WriteText(temp / "memory" / "notes.jsonl", existingNotes + notePromo2.dump() + "\r\n");
        Check(Parse(memory::RebuildJson()).value("ok", false), "rebuild index includes nt_promo_2");

        json mcpAuth = Parse(memory::HandleToolCall("memory_approve", R"({"id":"nt_promo_2", "worker":"reviewer"})"));
        Check(mcpAuth.value("ok", false), "MCP memory_approve succeeds for authorized worker");
        Check(mcpAuth.value("approvedBy", "") == "worker:reviewer", "MCP memory_approve records approvedBy: worker:reviewer");

        // 4. Search L3 knowledge and verify approvedBy is returned in results
        memory::Query qKn;
        qKn.kinds = {"knowledge"};
        qKn.text = "cmake ctest incremental linking";
        json searchKn = Parse(memory::SearchJson(qKn));
        Check(searchKn.value("ok", false), "search knowledge succeeds");
        bool foundWorkerCodex = false;
        bool foundWorkerReviewer = false;
        for (const auto& hit : searchKn.value("results", json::array())) {
            std::string by = hit.value("approvedBy", "");
            if (by == "worker:codex") foundWorkerCodex = true;
            if (by == "worker:reviewer") foundWorkerReviewer = true;
        }
        Check(foundWorkerCodex, "search result for L3 knowledge contains approvedBy worker:codex");
        Check(foundWorkerReviewer, "search result for L3 knowledge contains approvedBy worker:reviewer");
    }

    fs::remove_all(temp, ec);
    SetDataDirectoryOverride(L"");

    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
