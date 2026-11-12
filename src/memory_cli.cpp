#include "memory_cli.h"
#include "memory_record.h"
#include "memory_search.h"
#include "common.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace pc {

namespace {

using json = nlohmann::json;

std::string Str(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return std::string();
    try {
        return it->get<std::string>();
    } catch (...) {
        return std::string();
    }
}

int64_t Int(const json& j, const char* key, int64_t fallback = 0) {
    auto it = j.find(key);
    if (it == j.end()) return fallback;
    try {
        if (it->is_number_integer()) return it->get<int64_t>();
        if (it->is_number_unsigned()) return static_cast<int64_t>(it->get<uint64_t>());
        if (it->is_number_float()) return static_cast<int64_t>(it->get<double>());
    } catch (...) {
    }
    return fallback;
}

double Num(const json& j, const char* key, double fallback = 0.0) {
    auto it = j.find(key);
    if (it == j.end()) return fallback;
    try {
        if (it->is_number()) return it->get<double>();
    } catch (...) {
    }
    return fallback;
}

void AddKinds(memory::Query& q, const std::string& value) {
    std::stringstream ss(value);
    std::string part;
    while (std::getline(ss, part, ',')) {
        if (!part.empty()) q.kinds.push_back(part);
    }
}

void AddList(std::vector<std::string>& out, const std::string& value) {
    std::stringstream ss(value);
    std::string part;
    while (std::getline(ss, part, ',')) {
        if (!part.empty()) out.push_back(part);
    }
}

bool IsHelp(const std::string& arg) {
    return arg == "--help" || arg == "-h";
}

bool UnknownOption(const std::string& arg) {
    return arg.size() > 1 && arg[0] == '-' && arg != "-" && !IsHelp(arg);
}

void PrintMemoryUsage(std::ostream& out) {
    out << "MEMORY:\n"
        << "  memory search [query] [filters] [--ranker <path>] [--limit <n>]\n"
        << "      Search L1 run records, L2 notes and L3 approved knowledge.\n"
        << "      Filters: --kind run,note,knowledge --scope <scope> --file <path>\n"
        << "               --job-name <name> --taskqueue <name> --engine <name>\n"
        << "               --model <name> --outcome <outcome> --failure-class <class>\n"
        << "               --since <30m|6h|2d|YYYY-MM-DD>\n"
        << "               --ranker <path> (external ranker plugin override)\n"
        << "  memory get <id> [--kind <run|note|knowledge>]\n"
        << "      Show one memory record in full, with its source file and line.\n"
        << "  memory brief [--job-name <name>] [--taskqueue <name>] [--file <path>]...\n"
        << "               [--scope <job,file,taskqueue,project>]... [--budget-chars <n>]\n"
        << "               [--no-agent-notes]\n"
        << "      Summarize the records relevant to a job in a fixed block:\n"
        << "      previous runs, common failures, related notes and project knowledge.\n"
        << "      Sections are trimmed from the bottom to fit --budget-chars (default 1500).\n"
        << "  memory stats [--since <period>] [--job-name <name>] [--taskqueue <name>]\n"
        << "               [--engine <name>] [--model <name>] [--outcome <outcome>]\n"
        << "      Success rate, duration median/p90, failures and token usage from L1.\n"
        << "  memory approve <note-id> [--by <name>] [--worker <name>]\n"
        << "      Promote an L2 note to L3 knowledge. Allowed for humans and\n"
        << "      authorized workers listed in memory.l3Promoters in config.json.\n"
        << "      Writes memory/knowledge.jsonl with approvedBy / approvedAt.\n"
        << "  memory note --text <text> [--scope <scope>] [--tag <tag>]...\n"
        << "              [--source <id>]... [--supersedes <note-id>] [--expires-at <time>]\n"
        << "              [--author human|hancho]\n"
        << "      Append an L2 note. scope is project, taskqueue:<name>, job:<name>,\n"
        << "      file:<path> or engine:<name>. Known secrets (sk-, ghp_, AKIA, Bearer,\n"
        << "      password=) are redacted before writing; job env is never recorded.\n"
        << "  memory retract <note-id> [--reason <text>] [--by <name>]\n"
        << "      Append a retraction for an L2 note. The note and the retraction both\n"
        << "      stay in notes.jsonl; replay marks the note as retracted.\n"
        << "  memory rebuild\n"
        << "      Rebuild memory/records/*.jsonl (L1) from logs/taskqueues/*.json and the\n"
        << "      normalized .events.jsonl, then rebuild memory/index/index.json from the\n"
        << "      records, notes.jsonl and knowledge.jsonl. Derived data only.\n"
        << "  memory prune [--dry-run|-n] [--days <n>]\n"
        << "      Delete memory/records/*.jsonl (L1) for months older than 90 days (or --days).\n"
        << "      Also enforces the configured memory/ capacity limit from config.json by\n"
        << "      pruning oldest L1 files. L2 notes and L3 knowledge are never deleted.\n"
        << "      --dry-run lists candidates without deleting.\n"
        << "      All commands accept --json / -j.\n";
}

bool ParseCommonFilter(const std::string& a, const std::string& v, memory::Query& q, bool& handled) {
    handled = true;
    if (a == "--kind" || a == "--kinds") {
        AddKinds(q, v);
    } else if (a == "--scope") {
        q.scope = v;
    } else if (a == "--file") {
        q.file = v;
    } else if (a == "--job-name" || a == "--jobName" || a == "--job") {
        q.jobName = v;
    } else if (a == "--taskqueue" || a == "--queue") {
        q.taskqueue = v;
    } else if (a == "--engine") {
        q.engine = v;
    } else if (a == "--model") {
        q.model = v;
    } else if (a == "--outcome") {
        q.outcome = v;
    } else if (a == "--failure-class" || a == "--failureClass") {
        q.failureClass = v;
    } else if (a == "--since") {
        q.since = v;
    } else {
        handled = false;
    }
    return handled;
}

int PrintSearch(const std::string& response) {
    json j = json::parse(response, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        std::cerr << "Error: invalid response from memory search\n";
        return 1;
    }
    if (!j.value("ok", false)) {
        std::cerr << "Error: " << Str(j, "error") << "\n";
        return 1;
    }
    if (j.contains("rankerWarning") && j["rankerWarning"].is_string()) {
        std::cerr << "Warning: ranker failed (" << j["rankerWarning"].get<std::string>()
                  << "), fell back to builtin ranking\n";
    }
    auto results = j.find("results");
    size_t count = 0;
    if (results != j.end() && results->is_array()) {
        for (const auto& hit : *results) {
            ++count;
            std::cout << std::fixed << std::setprecision(2) << Num(hit, "score") << "  "
                      << Str(hit, "id") << "  [" << Str(hit, "kind") << "]  "
                      << Str(hit, "summary") << "\n";
            auto src = hit.find("source");
            if (src != hit.end() && src->is_object()) {
                std::cout << "    source: " << Str(*src, "file") << ":"
                          << Int(*src, "line") << "\n";
            }
        }
    }
    std::cout << count << " result(s)";
    int64_t total = Int(j, "totalMatched", static_cast<int64_t>(count));
    if (total > static_cast<int64_t>(count)) std::cout << " (" << total << " matched)";
    std::cout << "\n";
    return 0;
}

int PrintGet(const std::string& response) {
    json j = json::parse(response, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        std::cerr << "Error: invalid response from memory get\n";
        return 1;
    }
    if (!j.value("ok", false)) {
        std::cerr << "Error: " << Str(j, "error") << "\n";
        return 1;
    }
    std::cout << "id: " << Str(j, "id") << "\n"
              << "kind: " << Str(j, "kind") << "\n"
              << "status: " << Str(j, "status") << "\n";
    auto src = j.find("source");
    if (src != j.end() && src->is_object()) {
        std::cout << "source: " << Str(*src, "file") << ":" << Int(*src, "line") << "\n";
    }
    auto doc = j.find("document");
    if (doc != j.end()) std::cout << doc->dump(2) << "\n";
    return 0;
}

void PrintGroups(const char* title, const json& groups) {
    if (!groups.is_array() || groups.empty()) return;
    std::cout << title << ":\n";
    for (const auto& g : groups) {
        auto dur = g.find("durationSec");
        double median = 0.0;
        double p90 = 0.0;
        if (dur != g.end() && dur->is_object()) {
            median = Num(*dur, "median");
            p90 = Num(*dur, "p90");
        }
        std::cout << "  " << Str(g, "key") << "  " << Int(g, "runs") << " run(s)  "
                  << std::fixed << std::setprecision(1) << Num(g, "successRate") * 100.0 << "%  "
                  << "median " << std::setprecision(0) << median << "s  p90 " << p90 << "s\n";
    }
}

int PrintStats(const std::string& response) {
    json j = json::parse(response, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        std::cerr << "Error: invalid response from memory stats\n";
        return 1;
    }
    if (!j.value("ok", false)) {
        std::cerr << "Error: " << Str(j, "error") << "\n";
        return 1;
    }
    auto dur = j.find("durationSec");
    double median = 0.0;
    double p90 = 0.0;
    if (dur != j.end() && dur->is_object()) {
        median = Num(*dur, "median");
        p90 = Num(*dur, "p90");
    }
    std::cout << "Runs: " << Int(j, "runs") << "  "
              << "Success: " << std::fixed << std::setprecision(1) << Num(j, "successRate") * 100.0 << "%  "
              << "Median: " << std::setprecision(0) << median << "s  p90: " << p90 << "s\n";
    std::cout << "Tokens: " << Int(j, "totalTokens")
              << "  Cost: $" << std::setprecision(2) << Num(j, "costUsd")
              << "  Avg tokens/run: " << std::setprecision(0) << Num(j, "avgTokensPerRun") << "\n";
    auto counts = j.find("counts");
    if (counts != j.end() && counts->is_object()) {
        std::cout << "Stored: " << Int(*counts, "run") << " run(s), "
                  << Int(*counts, "note") << " note(s), "
                  << Int(*counts, "knowledge") << " knowledge\n";
    }
    auto storage = j.find("storage");
    if (storage != j.end() && storage->is_object()) {
        int64_t total = Int(*storage, "totalBytes");
        int64_t limit = Int(*storage, "limitBytes");
        std::cout << "Storage: " << FormatBytes(total);
        if (limit > 0) {
            std::cout << " / " << FormatBytes(limit) << " ("
                      << std::fixed << std::setprecision(1) << Num(*storage, "usedPercent") << "%)";
        } else {
            std::cout << " (no limit)";
        }
        std::cout << " [L1: " << FormatBytes(Int(*storage, "l1Bytes"))
                  << ", L2: " << FormatBytes(Int(*storage, "l2Bytes"))
                  << ", L3: " << FormatBytes(Int(*storage, "l3Bytes")) << "]\n";
        if (storage->value("exceeded", false)) {
            std::cout << "Warning: memory storage limit exceeded!\n";
        } else if (storage->value("nearLimit", false)) {
            std::cout << "Warning: memory storage is near configured limit (>= 80%)\n";
        }
    }
    auto byJob = j.find("byJobName");
    if (byJob != j.end()) PrintGroups("By job", *byJob);
    auto byEngine = j.find("byEngine");
    if (byEngine != j.end()) PrintGroups("By engine", *byEngine);
    auto byModel = j.find("byModel");
    if (byModel != j.end()) PrintGroups("By model", *byModel);
    auto failures = j.find("failures");
    if (failures != j.end() && failures->is_array() && !failures->empty()) {
        std::cout << "Failures:\n";
        for (const auto& f : *failures) {
            std::cout << "  " << Str(f, "class") << "  " << Int(f, "count") << "\n";
        }
    }
    return 0;
}

int PrintBrief(const std::string& response) {
    json j = json::parse(response, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        std::cerr << "Error: invalid response from memory brief\n";
        return 1;
    }
    if (!j.value("ok", false)) {
        std::cerr << "Error: " << Str(j, "error") << "\n";
        return 1;
    }
    std::cout << Str(j, "text") << "\n";
    return 0;
}

int PrintApprove(const std::string& response) {
    json j = json::parse(response, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        std::cerr << "Error: invalid response from memory approve\n";
        return 1;
    }
    if (!j.value("ok", false)) {
        std::cerr << "Error: " << Str(j, "error") << "\n";
        return 1;
    }
    std::cout << "Approved " << Str(j, "noteId") << " as " << Str(j, "id") << "\n"
              << "By: " << Str(j, "approvedBy") << "  At: " << Str(j, "approvedAt") << "\n"
              << "Written to " << Str(j, "path") << "\n";
    return 0;
}

int PrintNote(const std::string& response) {
    json j = json::parse(response, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        std::cerr << "Error: invalid response from memory note\n";
        return 1;
    }
    if (!j.value("ok", false)) {
        std::cerr << "Error: " << Str(j, "error") << "\n";
        return 1;
    }
    std::cout << (j.value("duplicate", false) ? "Existing note " : "Noted ")
              << Str(j, "id") << " [" << Str(j, "status") << "] scope: " << Str(j, "scope") << "\n";
    if (j.value("redacted", false)) std::cout << "Secrets were redacted before writing.\n";
    if (!Str(j, "supersedes").empty() && j.find("supersedes") != j.end() &&
        !j["supersedes"].is_null()) {
        std::cout << "Supersedes: " << Str(j, "supersedes") << "\n";
    }
    std::cout << "Written to " << Str(j, "path") << "\n";
    return 0;
}

int PrintRetract(const std::string& response) {
    json j = json::parse(response, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        std::cerr << "Error: invalid response from memory retract\n";
        return 1;
    }
    if (!j.value("ok", false)) {
        std::cerr << "Error: " << Str(j, "error") << "\n";
        return 1;
    }
    std::cout << (j.value("alreadyRetracted", false) ? "Already retracted " : "Retracted ")
              << Str(j, "id") << " [" << Str(j, "status") << "]\n"
              << "Written to " << Str(j, "path") << "\n";
    return 0;
}

int PrintRebuild(const std::string& response) {
    json j = json::parse(response, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        std::cerr << "Error: invalid response from memory rebuild\n";
        return 1;
    }
    if (!j.value("ok", false)) {
        std::cerr << "Error: " << Str(j, "error") << "\n";
        return 1;
    }
    std::cout << "Index rebuilt: " << Int(j, "docCount") << " document(s)";
    auto counts = j.find("counts");
    if (counts != j.end() && counts->is_object()) {
        std::cout << " (run=" << Int(*counts, "run")
                  << ", note=" << Int(*counts, "note")
                  << ", knowledge=" << Int(*counts, "knowledge") << ")";
    }
    std::cout << "\n";
    auto records = j.find("records");
    if (records != j.end() && records->is_object()) {
        if (records->value("skipped", false)) {
            std::cout << "L1 records: kept (no taskqueue state files)\n";
        } else {
            std::cout << "L1 records: " << Int(*records, "records") << " in "
                      << Int(*records, "files") << " month file(s)\n";
        }
    }
    std::cout << "Path: " << Str(j, "indexPath") << "\n";
    return 0;
}

int PrintPrune(const std::string& response) {
    json j = json::parse(response, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        std::cerr << "Error: invalid response from memory prune\n";
        return 1;
    }
    if (!j.value("ok", false)) {
        std::cerr << "Error: " << Str(j, "error") << "\n";
        return 1;
    }
    bool dryRun = j.value("dryRun", false);
    auto files = j.find("prunedFiles");
    std::vector<std::string> fileList;
    if (files != j.end() && files->is_array()) {
        for (const auto& f : *files) {
            if (f.is_string()) fileList.push_back(f.get<std::string>());
        }
    }
    int64_t freed = Int(j, "bytesFreed");
    int days = static_cast<int>(Int(j, "retentionDays", 90));

    if (dryRun) {
        if (fileList.empty()) {
            std::cout << "Dry run: no memory record files would be pruned (retention: " << days << " days).\n";
        } else {
            std::cout << "Dry run: " << fileList.size() << " file(s) would be pruned ("
                      << FormatBytes(freed) << " would be freed):\n";
            for (const auto& f : fileList) {
                std::cout << "  " << f << "\n";
            }
        }
    } else {
        if (fileList.empty()) {
            std::cout << "No memory record files older than " << days << " days to prune.\n";
        } else {
            std::cout << "Pruned " << fileList.size() << " file(s) ("
                      << FormatBytes(freed) << " freed):\n";
            for (const auto& f : fileList) {
                std::cout << "  " << f << "\n";
            }
        }
    }
    int64_t remaining = Int(j, "totalBytesAfter");
    int64_t limit = Int(j, "limitBytes");
    std::cout << "Current memory storage: " << FormatBytes(remaining);
    if (limit > 0) {
        std::cout << " / " << FormatBytes(limit);
    }
    std::cout << "\n";

    if (j.value("exceeded", false)) {
        std::cout << "Warning: memory storage limit exceeded! L2 notes and L3 knowledge are preserved.\n";
    } else if (j.value("nearLimit", false)) {
        std::cout << "Warning: memory storage is near configured limit (>= 80%). L2/L3 are preserved.\n";
    }
    return 0;
}

} // namespace

int RunMemoryCli(const std::vector<std::string>& args, bool jsonOutput) {
    std::string op = args.size() > 2 ? args[2] : "";
    if (op.empty() || op == "help" || op == "--help" || op == "-h") {
        PrintMemoryUsage(op.empty() ? std::cerr : std::cout);
        return op.empty() ? 1 : 0;
    }

    if (op == "search") {
        memory::Query q;
        bool help = false;
        std::string rankerPlugin;
        std::vector<std::string> words;
        for (size_t i = 3; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (IsHelp(a)) {
                help = true;
            } else if ((a == "--ranker" || a == "--ranker-plugin") && i + 1 < args.size()) {
                rankerPlugin = args[++i];
            } else if (a == "--limit" && i + 1 < args.size()) {
                try {
                    q.limit = std::stoi(args[++i]);
                } catch (...) {
                    q.limit = 20;
                }
            } else if ((a == "--kind" || a == "--kinds" || a == "--scope" || a == "--file" ||
                        a == "--job-name" || a == "--jobName" || a == "--job" ||
                        a == "--taskqueue" || a == "--queue" || a == "--engine" ||
                        a == "--model" || a == "--outcome" || a == "--failure-class" ||
                        a == "--failureClass" || a == "--since") && i + 1 < args.size()) {
                bool handled = false;
                ParseCommonFilter(a, args[++i], q, handled);
            } else if (UnknownOption(a)) {
                std::cerr << "Error: unknown option for memory search: " << a << "\n";
                return 1;
            } else {
                words.push_back(a);
            }
        }
        if (help) {
            PrintMemoryUsage(std::cout);
            return 0;
        }
        for (size_t i = 0; i < words.size(); ++i) {
            if (i) q.text += " ";
            q.text += words[i];
        }
        if (q.limit <= 0) q.limit = 20;
        if (q.limit > 200) q.limit = 200;
        std::unique_ptr<memory::IMemoryRanker> customRanker;
        if (!rankerPlugin.empty()) {
            customRanker = std::make_unique<memory::ProcessRanker>(rankerPlugin);
        }
        std::string response = memory::SearchJson(q, customRanker.get());
        if (jsonOutput) {
            std::cout << response << "\n";
            return 0;
        }
        return PrintSearch(response);
    }

    if (op == "get") {
        std::string id;
        std::string kind;
        bool help = false;
        for (size_t i = 3; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (IsHelp(a)) {
                help = true;
            } else if (a == "--kind" && i + 1 < args.size()) {
                kind = args[++i];
            } else if (UnknownOption(a)) {
                std::cerr << "Error: unknown option for memory get: " << a << "\n";
                return 1;
            } else if (id.empty()) {
                id = a;
            }
        }
        if (help) {
            PrintMemoryUsage(std::cout);
            return 0;
        }
        if (id.empty()) {
            std::cerr << "Error: memory get requires an id\n";
            return 1;
        }
        std::string response = memory::GetJson(id, kind);
        if (jsonOutput) {
            std::cout << response << "\n";
            return 0;
        }
        return PrintGet(response);
    }

    if (op == "brief") {
        memory::BriefOptions o;
        bool help = false;
        for (size_t i = 3; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (IsHelp(a)) {
                help = true;
            } else if ((a == "--job-name" || a == "--jobName" || a == "--job") && i + 1 < args.size()) {
                o.jobName = args[++i];
            } else if ((a == "--taskqueue" || a == "--queue") && i + 1 < args.size()) {
                o.taskqueue = args[++i];
            } else if (a == "--engine" && i + 1 < args.size()) {
                o.engine = args[++i];
            } else if (a == "--file" && i + 1 < args.size()) {
                o.files.push_back(args[++i]);
            } else if (a == "--scope" && i + 1 < args.size()) {
                std::stringstream ss(args[++i]);
                std::string part;
                while (std::getline(ss, part, ',')) {
                    if (!part.empty()) o.scopes.push_back(part);
                }
            } else if ((a == "--budget-chars" || a == "--budgetChars") && i + 1 < args.size()) {
                try {
                    o.budgetChars = std::stoi(args[++i]);
                } catch (...) {
                }
            } else if ((a == "--max-previous-runs" || a == "--maxPreviousRuns") && i + 1 < args.size()) {
                try {
                    o.maxPreviousRuns = std::stoi(args[++i]);
                } catch (...) {
                }
            } else if ((a == "--stats-window" || a == "--statsWindow") && i + 1 < args.size()) {
                try {
                    o.statsWindow = std::stoi(args[++i]);
                } catch (...) {
                }
            } else if (a == "--no-agent-notes" || a == "--noAgentNotes") {
                o.includeAgentNotes = false;
            } else if (a == "--include-agent-notes" || a == "--includeAgentNotes") {
                o.includeAgentNotes = true;
            } else if (UnknownOption(a)) {
                std::cerr << "Error: unknown option for memory brief: " << a << "\n";
                return 1;
            }
        }
        if (help) {
            PrintMemoryUsage(std::cout);
            return 0;
        }
        std::string response = memory::BriefJson(o);
        if (jsonOutput) {
            std::cout << response << "\n";
            return 0;
        }
        return PrintBrief(response);
    }

    if (op == "stats") {
        memory::Query q;
        bool help = false;
        for (size_t i = 3; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (IsHelp(a)) {
                help = true;
            } else if ((a == "--kind" || a == "--kinds" || a == "--scope" || a == "--file" ||
                        a == "--job-name" || a == "--jobName" || a == "--job" ||
                        a == "--taskqueue" || a == "--queue" || a == "--engine" ||
                        a == "--model" || a == "--outcome" || a == "--failure-class" ||
                        a == "--failureClass" || a == "--since") && i + 1 < args.size()) {
                bool handled = false;
                ParseCommonFilter(a, args[++i], q, handled);
            } else if (UnknownOption(a)) {
                std::cerr << "Error: unknown option for memory stats: " << a << "\n";
                return 1;
            }
        }
        if (help) {
            PrintMemoryUsage(std::cout);
            return 0;
        }
        std::string response = memory::StatsJson(q);
        if (jsonOutput) {
            std::cout << response << "\n";
            return 0;
        }
        return PrintStats(response);
    }

    if (op == "approve") {
        std::string id;
        std::string approvedBy;
        bool help = false;
        for (size_t i = 3; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (IsHelp(a)) {
                help = true;
            } else if ((a == "--by" || a == "--approved-by" || a == "--approvedBy") &&
                       i + 1 < args.size()) {
                approvedBy = args[++i];
            } else if ((a == "--worker" || a == "-w") && i + 1 < args.size()) {
                std::string w = args[++i];
                approvedBy = (w.rfind("worker:", 0) == 0) ? w : ("worker:" + w);
            } else if (UnknownOption(a)) {
                std::cerr << "Error: unknown option for memory approve: " << a << "\n";
                return 1;
            } else if (id.empty()) {
                id = a;
            }
        }
        if (help) {
            PrintMemoryUsage(std::cout);
            return 0;
        }
        if (id.empty()) {
            std::cerr << "Error: memory approve requires a note id\n";
            return 1;
        }
        std::string response = memory::ApproveJson(id, approvedBy);
        if (jsonOutput) {
            std::cout << response << "\n";
            return 0;
        }
        return PrintApprove(response);
    }

    if (op == "note") {
        memory::NoteInput in;
        in.authorType = "human";
        bool help = false;
        std::vector<std::string> words;
        for (size_t i = 3; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (IsHelp(a)) {
                help = true;
            } else if ((a == "--text" || a == "-t") && i + 1 < args.size()) {
                in.text = args[++i];
            } else if (a == "--scope" && i + 1 < args.size()) {
                in.scope = args[++i];
            } else if ((a == "--tag" || a == "--tags") && i + 1 < args.size()) {
                AddList(in.tags, args[++i]);
            } else if ((a == "--source" || a == "--sources") && i + 1 < args.size()) {
                AddList(in.sources, args[++i]);
            } else if (a == "--supersedes" && i + 1 < args.size()) {
                in.supersedes = args[++i];
            } else if ((a == "--expires-at" || a == "--expiresAt") && i + 1 < args.size()) {
                in.expiresAt = args[++i];
            } else if (a == "--author" && i + 1 < args.size()) {
                in.authorType = args[++i];
            } else if (UnknownOption(a)) {
                std::cerr << "Error: unknown option for memory note: " << a << "\n";
                return 1;
            } else {
                words.push_back(a);
            }
        }
        if (help) {
            PrintMemoryUsage(std::cout);
            return 0;
        }
        if (in.text.empty()) {
            for (size_t i = 0; i < words.size(); ++i) {
                if (i) in.text += " ";
                in.text += words[i];
            }
        }
        if (in.text.empty()) {
            std::cerr << "Error: memory note requires --text <text>\n";
            return 1;
        }
        std::string response = memory::NoteJson(in);
        if (jsonOutput) {
            std::cout << response << "\n";
            return 0;
        }
        return PrintNote(response);
    }

    if (op == "retract") {
        std::string id;
        std::string reason;
        std::string by;
        bool help = false;
        for (size_t i = 3; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (IsHelp(a)) {
                help = true;
            } else if ((a == "--reason" || a == "--why") && i + 1 < args.size()) {
                reason = args[++i];
            } else if ((a == "--by" || a == "--retracted-by" || a == "--retractedBy") &&
                       i + 1 < args.size()) {
                by = args[++i];
            } else if (UnknownOption(a)) {
                std::cerr << "Error: unknown option for memory retract: " << a << "\n";
                return 1;
            } else if (id.empty()) {
                id = a;
            }
        }
        if (help) {
            PrintMemoryUsage(std::cout);
            return 0;
        }
        if (id.empty()) {
            std::cerr << "Error: memory retract requires a note id\n";
            return 1;
        }
        std::string response = memory::RetractJson(id, by.empty() ? "cli:human" : by, reason);
        if (jsonOutput) {
            std::cout << response << "\n";
            return 0;
        }
        return PrintRetract(response);
    }

    if (op == "rebuild") {
        if (args.size() > 3 && IsHelp(args[3])) {
            PrintMemoryUsage(std::cout);
            return 0;
        }
        std::string response = memory::RebuildJson();
        if (jsonOutput) {
            std::cout << response << "\n";
            return 0;
        }
        return PrintRebuild(response);
    }

    if (op == "prune") {
        memory::PruneOptions o;
        bool help = false;
        for (size_t i = 3; i < args.size(); ++i) {
            const std::string& a = args[i];
            if (IsHelp(a)) {
                help = true;
            } else if (a == "--dry-run" || a == "-n") {
                o.dryRun = true;
            } else if ((a == "--days" || a == "--retention-days" || a == "--retentionDays") && i + 1 < args.size()) {
                try {
                    o.retentionDays = std::stoi(args[++i]);
                } catch (...) {
                }
            } else if ((a == "--limit-bytes" || a == "--limitBytes") && i + 1 < args.size()) {
                try {
                    o.limitBytes = std::stoll(args[++i]);
                } catch (...) {
                }
            } else if (UnknownOption(a)) {
                std::cerr << "Error: unknown option for memory prune: " << a << "\n";
                return 1;
            }
        }
        if (help) {
            PrintMemoryUsage(std::cout);
            return 0;
        }
        std::string response = memory::PruneRunRecordsJson(o);
        if (jsonOutput) {
            std::cout << response << "\n";
            return 0;
        }
        return PrintPrune(response);
    }

    std::cerr << "Error: unknown memory command: " << op << "\n";
    PrintMemoryUsage(std::cerr);
    return 1;
}

} // namespace pc
