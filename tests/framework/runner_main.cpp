#include "test_registry.h"
#include "process_runner.h"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace {

using json = nlohmann::json;

#ifdef PC_REPO_DIR
const std::string kDefaultDefs = std::string(PC_REPO_DIR) + "/tests/definitions";
#else
const std::string kDefaultDefs = "tests/definitions";
#endif

struct Options {
    std::vector<std::string> defPaths;
    std::string filter;
    bool list = false;
    bool verbose = false;
};

struct TestCase {
    std::string id;
    std::string kind;
    std::string summary;
    std::string command;
    int timeoutSec = 30;
    json expect = json::object();
    std::string source;
};

struct Outcome {
    bool ok = true;
    std::vector<std::string> failures;
    std::vector<std::string> notes;
};

void PrintUsage() {
    std::printf(
        "usage: hancho_test [options]\n"
        "  --defs <file|dir>   add a definition file or directory (repeatable)\n"
        "                     default: %s\n"
        "  --filter <text>     run only cases whose id/summary contains <text>\n"
        "  --list              list discovered cases without running them\n"
        "  -v, --verbose       print every check and command output\n"
        "  -h, --help          show this help\n",
        kDefaultDefs.c_str());
}

bool Matches(const TestCase& c, const std::string& filter) {
    if (filter.empty()) return true;
    return c.id.find(filter) != std::string::npos ||
           c.summary.find(filter) != std::string::npos ||
           c.kind.find(filter) != std::string::npos;
}

std::string ReadFile(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::string();
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::vector<std::string> StringArray(const json& j, const char* key) {
    std::vector<std::string> out;
    if (!j.is_object()) return out;
    auto it = j.find(key);
    if (it == j.end() || !it->is_array()) return out;
    for (const auto& item : *it) {
        if (item.is_string()) out.push_back(item.get<std::string>());
    }
    return out;
}

bool ContainsAll(const std::string& text, const std::vector<std::string>& needles,
                 bool expectPresent, std::vector<std::string>& failures) {
    bool ok = true;
    for (const auto& needle : needles) {
        const bool found = text.find(needle) != std::string::npos;
        if (found != expectPresent) {
            failures.push_back(std::string(expectPresent ? "missing text: " : "unexpected text: ") +
                               needle);
            ok = false;
        }
    }
    return ok;
}

void CheckRegex(const std::string& text, const std::string& pattern, const char* label,
                bool expectMatch, Outcome& out) {
    std::regex re;
    try {
        re.assign(pattern);
    } catch (const std::regex_error& e) {
        out.ok = false;
        out.failures.push_back(std::string("invalid regex for ") + label + ": " + e.what());
        return;
    }
    const bool matched = std::regex_search(text, re);
    if (matched != expectMatch) {
        out.ok = false;
        out.failures.push_back(std::string(expectMatch ? "regex did not match: " :
                                                         "regex unexpectedly matched: ") + label +
                               " (" + pattern + ")");
    }
}

Outcome RunApiCase(const TestCase& c, bool verbose) {
    Outcome out;
    htest::TestEntry* entry = nullptr;
    for (auto& e : htest::Registry()) {
        if (e.id == c.id) {
            entry = &e;
            break;
        }
    }
    if (!entry) {
        out.ok = false;
        out.failures.push_back("no registered API test with id '" + c.id + "'");
        return out;
    }

    htest::TestContext ctx(c.id);
    try {
        entry->fn(ctx);
    } catch (const std::exception& e) {
        out.ok = false;
        out.failures.push_back(std::string("API test threw: ") + e.what());
    } catch (...) {
        out.ok = false;
        out.failures.push_back("API test threw an unknown exception");
    }
    for (const auto& r : ctx.Records()) {
        if (verbose) {
            std::printf("  %s %s\n", r.ok ? "[PASS]" : "[FAIL]", r.label.c_str());
        }
        if (!r.ok) {
            out.ok = false;
            out.failures.push_back(r.label + (r.detail.empty() ? "" : (": " + r.detail)));
        }
    }
    for (const auto& n : ctx.Notes()) out.notes.push_back(n);
    return out;
}

Outcome RunCliCase(const TestCase& c, bool verbose) {
    Outcome out;
    const std::string command = htest::ExpandExeDir(c.command);
    if (verbose) std::printf("  command: %s\n", command.c_str());

    const htest::ProcessResult r = htest::RunProcess(command, htest::ExecutableDir(), c.timeoutSec);
    if (verbose) {
        if (!r.stdoutText.empty()) std::printf("  stdout: %s\n", r.stdoutText.c_str());
        if (!r.stderrText.empty()) std::printf("  stderr: %s\n", r.stderrText.c_str());
    }
    if (!r.error.empty()) {
        out.ok = false;
        out.failures.push_back(r.error);
        return out;
    }
    if (r.timedOut) {
        out.ok = false;
        out.failures.push_back("command timed out after " + std::to_string(c.timeoutSec) + "s");
    }

    const json& e = c.expect;
    if (e.contains("exitCode") && e["exitCode"].is_number_integer()) {
        const int expected = e["exitCode"].get<int>();
        if (r.exitCode != expected) {
            out.ok = false;
            out.failures.push_back("exit code " + std::to_string(r.exitCode) +
                                   " (expected " + std::to_string(expected) + ")");
        }
    }
    ContainsAll(r.stdoutText, StringArray(e, "stdoutContains"), true, out.failures);
    ContainsAll(r.stdoutText, StringArray(e, "stdoutNotContains"), false, out.failures);
    ContainsAll(r.stderrText, StringArray(e, "stderrContains"), true, out.failures);
    ContainsAll(r.stderrText, StringArray(e, "stderrNotContains"), false, out.failures);

    if (e.contains("stdoutRegex") && e["stdoutRegex"].is_string()) {
        CheckRegex(r.stdoutText, e["stdoutRegex"].get<std::string>(), "stdoutRegex", true, out);
    }
    if (e.contains("stderrRegex") && e["stderrRegex"].is_string()) {
        CheckRegex(r.stderrText, e["stderrRegex"].get<std::string>(), "stderrRegex", true, out);
    }

    auto it = e.find("stdoutJson");
    if (it != e.end() && it->is_object()) {
        const json parsed = json::parse(r.stdoutText, nullptr, false);
        if (parsed.is_discarded()) {
            out.ok = false;
            out.failures.push_back("stdout is not valid JSON");
        } else {
            for (auto kv = it->begin(); kv != it->end(); ++kv) {
                if (!parsed.contains(kv.key()) || parsed[kv.key()] != kv.value()) {
                    out.ok = false;
                    out.failures.push_back("stdout JSON field '" + kv.key() + "' mismatch");
                }
            }
        }
    }
    if (!out.failures.empty()) out.ok = false;
    return out;
}

bool LoadDefinitionFile(const std::filesystem::path& path, std::vector<TestCase>& cases,
                        std::string& err) {
    const std::string text = ReadFile(path);
    if (text.empty()) {
        err = "cannot read " + path.string();
        return false;
    }
    const json doc = json::parse(text, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) {
        err = "invalid JSON in " + path.string();
        return false;
    }
    const std::string setId = doc.value("id", path.stem().string());
    auto testsIt = doc.find("tests");
    if (testsIt == doc.end() || !testsIt->is_array()) {
        err = "no tests array in " + path.string();
        return false;
    }
    for (const auto& t : *testsIt) {
        if (!t.is_object()) continue;
        TestCase c;
        c.id = t.value("id", "");
        if (c.id.empty()) continue;
        c.kind = t.value("kind", "api");
        c.summary = t.value("summary", "");
        c.command = t.value("command", "");
        c.timeoutSec = t.value("timeoutSec", 30);
        if (t.contains("expect") && t["expect"].is_object()) c.expect = t["expect"];
        c.source = setId;
        cases.push_back(std::move(c));
    }
    return true;
}

bool CollectDefinitions(const Options& opts, std::vector<TestCase>& cases, std::string& err) {
    std::vector<std::filesystem::path> files;
    for (const auto& p : opts.defPaths) {
        std::error_code ec;
        const std::filesystem::path path(p);
        if (std::filesystem::is_directory(path, ec)) {
            for (const auto& entry : std::filesystem::directory_iterator(path, ec)) {
                if (entry.is_regular_file() && entry.path().extension() == ".json") {
                    files.push_back(entry.path());
                }
            }
        } else {
            files.push_back(path);
        }
    }
    std::sort(files.begin(), files.end());
    for (const auto& f : files) {
        if (!LoadDefinitionFile(f, cases, err)) return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    SetConsoleOutputCP(CP_UTF8);

    Options opts;
    opts.defPaths.push_back(kDefaultDefs);
    bool useDefaultDefs = true;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--defs" && i + 1 < argc) {
            if (useDefaultDefs) {
                opts.defPaths.clear();
                useDefaultDefs = false;
            }
            opts.defPaths.push_back(argv[++i]);
        } else if (arg == "--filter" && i + 1 < argc) {
            opts.filter = argv[++i];
        } else if (arg == "--list") {
            opts.list = true;
        } else if (arg == "-v" || arg == "--verbose") {
            opts.verbose = true;
        } else if (arg == "-h" || arg == "--help") {
            PrintUsage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            PrintUsage();
            return 2;
        }
    }

    std::vector<TestCase> cases;
    std::string err;
    if (!CollectDefinitions(opts, cases, err)) {
        std::fprintf(stderr, "hancho_test: %s\n", err.c_str());
        return 2;
    }

    if (opts.list) {
        for (const auto& c : cases) {
            if (!Matches(c, opts.filter)) continue;
            std::printf("%-12s %-36s %s\n", c.kind.c_str(), c.id.c_str(), c.summary.c_str());
        }
        return 0;
    }

    int passed = 0;
    int failed = 0;
    for (const auto& c : cases) {
        if (!Matches(c, opts.filter)) continue;
        std::printf("[RUN ] %s\n", c.id.c_str());
        Outcome out;
        if (c.kind == "api") {
            out = RunApiCase(c, opts.verbose);
        } else if (c.kind == "cli") {
            out = RunCliCase(c, opts.verbose);
        } else {
            out.ok = false;
            out.failures.push_back("unknown kind: " + c.kind);
        }
        if (out.ok) {
            ++passed;
            std::printf("[PASS] %s\n", c.id.c_str());
        } else {
            ++failed;
            std::printf("[FAIL] %s\n", c.id.c_str());
            for (const auto& f : out.failures) std::printf("       - %s\n", f.c_str());
        }
        for (const auto& n : out.notes) std::printf("       note: %s\n", n.c_str());
    }

    std::printf("\n%d passed, %d failed, %d total\n", passed, failed, passed + failed);
    return failed ? 1 : 0;
}
