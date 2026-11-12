#include "memory_record.h"
#include "common.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace pc {
namespace memory {

namespace {

using json = nlohmann::json;
namespace fs = std::filesystem;

constexpr size_t kFilesPerList = 50;
constexpr size_t kCommandsMax = 20;
constexpr size_t kErrorsMax = 10;
constexpr size_t kGoalChars = 300;
constexpr size_t kFinalMessageBytes = 2 * 1024;

std::string LowerAscii(const std::string& s) {
    std::string out = s;
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool IEquals(const std::string& a, const std::string& b) {
    return LowerAscii(a) == LowerAscii(b);
}

std::string Trim(const std::string& s) {
    size_t start = 0;
    size_t end = s.size();
    while (start < end && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

std::string CollapseWhitespace(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool space = false;
    for (unsigned char c : s) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v') {
            space = !out.empty();
            continue;
        }
        if (space) {
            out.push_back(' ');
            space = false;
        }
        out.push_back(static_cast<char>(c));
    }
    return out;
}

std::string Utf8PrefixBytes(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t n = maxBytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}

std::string Utf8PrefixChars(const std::string& s, size_t maxChars) {
    size_t chars = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (chars == maxChars) return s.substr(0, i);
            ++chars;
        }
    }
    return s;
}

std::string BaseName(const std::string& path) {
    size_t pos = path.find_last_of("\\/");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}

bool HasPathSeparator(const std::string& s) {
    return s.find('/') != std::string::npos || s.find('\\') != std::string::npos;
}

std::string NormalizeSlashes(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        char ch = (c == '\\') ? '/' : c;
        if (ch == '/' && !out.empty() && out.back() == '/') {
            if (out.size() == 1 && out[0] == '/') {
                out.push_back('/');
            }
            continue;
        }
        out.push_back(ch);
    }
    return out;
}

// Turns an absolute target into a path relative to the job cwd (doc/design-v3.md 3.3).
// Paths outside the cwd are kept, with forward slashes, so they stay traceable.
std::string RelativeToCwd(const std::string& target, const std::string& cwd) {
    std::string t = NormalizeSlashes(Trim(target));
    while (t.rfind("./", 0) == 0) t.erase(0, 2);
    if (cwd.empty()) return t;
    std::string c = NormalizeSlashes(cwd);
    while (c.size() > 1 && c.back() == '/') c.pop_back();
    if (c.empty()) return t;
    std::string tl = LowerAscii(t);
    std::string cl = LowerAscii(c);
    if (tl.size() > cl.size() && tl.compare(0, cl.size(), cl) == 0 &&
        t[cl.size()] == '/') {
        t = t.substr(cl.size() + 1);
        while (t.rfind("./", 0) == 0) t.erase(0, 2);
    }
    return t;
}

int64_t ParseIsoEpoch(const std::string& raw) {
    if (raw.empty()) return 0;
    std::string s = raw;
    for (char& c : s) {
        if (c == 'T') c = ' ';
    }
    if (!s.empty() && s.back() == 'Z') s.pop_back();
    if (s.size() > 10) {
        size_t plus = s.find('+', 11);
        size_t minus = s.find('-', 11);
        if (plus != std::string::npos) s = s.substr(0, plus);
        if (minus != std::string::npos) s = s.substr(0, minus);
    }
    int64_t epoch = ParseTimestampStrToEpoch(s);
    if (epoch != 0) return epoch;
    if (s.size() == 10) return ParseTimestampStrToEpoch(s + " 00:00:00");
    return 0;
}

std::string MonthOf(const std::string& iso) {
    if (iso.size() >= 7 && std::isdigit(static_cast<unsigned char>(iso[0])) &&
        std::isdigit(static_cast<unsigned char>(iso[1])) &&
        std::isdigit(static_cast<unsigned char>(iso[2])) &&
        std::isdigit(static_cast<unsigned char>(iso[3])) && iso[4] == '-' &&
        std::isdigit(static_cast<unsigned char>(iso[5])) &&
        std::isdigit(static_cast<unsigned char>(iso[6]))) {
        return iso.substr(0, 7);
    }
    return std::string();
}

std::string OutcomeFor(const std::string& reason, const std::string& jobState) {
    std::string r = LowerAscii(reason);
    if (r == "ok" || r == "success" || r == "succeeded") return "success";
    if (r == "killed" || r == "cancelled" || r == "canceled" || IEquals(jobState, "cancelled")) {
        return "cancelled";
    }
    if (r == "interrupted") return "interrupted";
    if (r == "deferred") return "deferred";
    return "failed";
}

static bool IsCrashExitCode(int exitCode) {
    uint32_t u = static_cast<uint32_t>(exitCode);
    if (u == 0xFFFFFFFFu) return false;
    if (u >= 0xC0000000u) return true;
    if (u == 0x40000015u) return true;
    return false;
}


std::string JStr(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) return std::string();
    try {
        return it->get<std::string>();
    } catch (...) {
        return std::string();
    }
}

int64_t JInt(const json& j, const char* key, int64_t fallback = 0) {
    auto it = j.find(key);
    if (it == j.end()) return fallback;
    try {
        if (it->is_number_integer()) return it->get<int64_t>();
        if (it->is_number_unsigned()) return static_cast<int64_t>(it->get<uint64_t>());
        if (it->is_number_float()) return static_cast<int64_t>(it->get<double>());
        if (it->is_string()) return std::stoll(it->get<std::string>());
    } catch (...) {
    }
    return fallback;
}

double JDouble(const json& j, const char* key, double fallback = 0.0) {
    auto it = j.find(key);
    if (it == j.end()) return fallback;
    try {
        if (it->is_number()) return it->get<double>();
        if (it->is_string()) return std::stod(it->get<std::string>());
    } catch (...) {
    }
    return fallback;
}

const json* JObj(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_object()) return nullptr;
    return &*it;
}

std::vector<json> ReadEventsFile(const std::string& path) {
    std::vector<json> events;
    if (path.empty()) return events;
    std::string text = ReadFileUtf8Wide(Utf8ToWide(path));
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB &&
        static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        std::string line = end == std::string::npos ? text.substr(start)
                                                    : text.substr(start, end - start);
        start = end == std::string::npos ? text.size() + 1 : end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        json j = json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;
        if (JStr(j, "schema") != "hancho.event/1") continue;
        events.push_back(std::move(j));
    }
    return events;
}

std::string FindEventsFileForProcess(int processId) {
    if (processId <= 0) return std::string();
    std::wstring pattern = L"proc_" + std::to_wstring(processId) + L"_*.events.jsonl";
    auto files = ListDirectoryWide(GetLogsDirectoryWide(), pattern);
    if (files.empty()) {
        pattern = L"proc_" + std::to_wstring(processId) + L"*.events.jsonl";
        files = ListDirectoryWide(GetLogsDirectoryWide(), pattern);
    }
    if (files.empty()) return std::string();
    return WideToUtf8(files.front());
}

std::wstring AttemptLogPathWide(const std::string& jobId, int attempt) {
    std::wstring dir = PathJoinWide(GetLogsDirectoryWide(), L"taskqueue");
    std::wstring fileName = Utf8ToWide(SanitizeFileNameComponent(jobId)) + L"." +
                            std::to_wstring(attempt) + L".log";
    return PathJoinWide(dir, fileName);
}

std::string ResolveLogText(const RunRecordInput& input, const std::string& logPath) {
    // Prefer the file so a live record and a later rebuild see exactly the same bytes.
    if (!logPath.empty()) {
        std::string text = ReadFileUtf8Wide(Utf8ToWide(logPath));
        if (!text.empty()) return text;
    }
    if (!input.logText.empty()) return input.logText;
    if (!input.lastLinesFile.empty()) {
        return ReadFileUtf8Wide(Utf8ToWide(input.lastLinesFile));
    }
    return std::string();
}

// Replaces numbers and path-shaped tokens so similar errors group together
// (doc/design-v3.md 3.3: "数字やパスを伏せた形で重複を数え").
std::string NormalizeErrorTerm(const std::string& token) {
    if (HasPathSeparator(token) && token.size() > 1) return "<path>";
    std::string out;
    out.reserve(token.size());
    for (size_t i = 0; i < token.size();) {
        unsigned char c = static_cast<unsigned char>(token[i]);
        if (c >= '0' && c <= '9') {
            out += "<n>";
            while (i < token.size() && std::isdigit(static_cast<unsigned char>(token[i]))) ++i;
            continue;
        }
        out.push_back(static_cast<char>(c));
        ++i;
    }
    return out;
}

std::string NormalizeErrorLine(const std::string& raw) {
    std::string line = CollapseWhitespace(raw);
    if (line.empty()) return line;
    std::string out;
    std::string token;
    auto flush = [&]() {
        if (token.empty()) return;
        if (!out.empty()) out.push_back(' ');
        out += NormalizeErrorTerm(token);
        token.clear();
    };
    for (char c : line) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            flush();
        } else {
            token.push_back(c);
        }
    }
    flush();
    return out;
}

bool LooksLikeErrorLine(const std::string& line) {
    std::string l = LowerAscii(line);
    static const char* const kWords[] = {
        "error", "fatal", "exception", "failed", "failure",
        "undefined reference", "lnk", "traceback", "cannot open",
    };
    for (const char* word : kWords) {
        if (l.find(word) != std::string::npos) return true;
    }
    return false;
}

std::vector<std::string> SelectCommands(const std::vector<std::string>& all) {
    std::vector<std::string> picked;
    if (all.size() <= kCommandsMax) {
        picked = all;
    } else {
        const size_t half = kCommandsMax / 2;
        for (size_t i = 0; i < half; ++i) picked.push_back(all[i]);
        for (size_t i = all.size() - (kCommandsMax - half); i < all.size(); ++i) {
            picked.push_back(all[i]);
        }
    }
    std::vector<std::string> out;
    std::set<std::string> seen;
    for (const auto& cmd : picked) {
        std::string trimmed = Trim(cmd);
        if (trimmed.empty()) continue;
        if (!seen.insert(trimmed).second) continue;
        out.push_back(trimmed);
        if (out.size() >= kCommandsMax) break;
    }
    return out;
}

std::string DumpJson(const json& j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

fs::path MemoryRoot() {
    return fs::path(GetEffectiveDataDirectoryWide()) / L"memory";
}

fs::path RecordsDirectory() {
    return MemoryRoot() / L"records";
}

bool RecordIdExists(const std::string& id) {
    std::error_code ec;
    fs::path dir = RecordsDirectory();
    if (!fs::is_directory(dir, ec)) return false;
    for (const auto& path : ListDirectoryWide(dir.wstring(), L"*.jsonl")) {
        std::string text = ReadFileUtf8Wide(path);
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            std::string line = end == std::string::npos ? text.substr(start)
                                                        : text.substr(start, end - start);
            start = end == std::string::npos ? text.size() : end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            json j = json::parse(line, nullptr, false);
            if (!j.is_object()) continue;
            if (JStr(j, "id") == id) return true;
        }
    }
    return false;
}

// Existing records are kept when a rebuild runs. They are not derived data the rebuild
// can invent (a test fixture, a record whose events aged out), so only records with the
// same id are replaced by the regenerated one.
std::vector<std::pair<json, std::string>> LoadExistingRecords() {
    std::vector<std::pair<json, std::string>> out;
    for (const auto& path : ListDirectoryWide(RecordsDirectory().wstring(), L"*.jsonl")) {
        std::string text = ReadFileUtf8Wide(path);
        std::string fileName = WideToUtf8(fs::path(path).filename().wstring());
        std::string stem = fileName;
        if (stem.size() > 6 && stem.compare(stem.size() - 6, 6, ".jsonl") == 0) {
            stem.resize(stem.size() - 6);
        }
        std::string fileMonth = MonthOf(stem);
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            std::string line = end == std::string::npos ? text.substr(start)
                                                        : text.substr(start, end - start);
            start = end == std::string::npos ? text.size() : end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            json j = json::parse(line, nullptr, false);
            if (!j.is_object() || JStr(j, "id").empty()) continue;
            out.push_back({std::move(j), fileMonth});
        }
    }
    return out;
}

} // namespace

std::pair<std::string, std::string> ClassifyDeterministicFailure(
    const std::string& reason, int exitCode, const std::string& outcome,
    const std::string& checkDetail, int timeoutSec, int64_t durationSec) {

    std::string r = LowerAscii(reason);
    std::string outLower = LowerAscii(outcome);

    // 1. launch_error: the process never started
    if (r == "launch_error" || r == "launcherror" || r == "launch_failed" || r == "launchfailed") {
        std::string summary = !checkDetail.empty() ? checkDetail : "The process could not be started.";
        return {"launch_error", summary};
    }

    // 2. killed: cancelled / killed
    if (r == "killed" || r == "cancelled" || r == "canceled" || outLower == "cancelled") {
        return {"killed", "The job was cancelled; the attempt was stopped."};
    }

    // 3. timeout: attempt timed out
    if (r == "timed_out" || r == "timedout" || r == "timeout" || (timeoutSec > 0 && durationSec >= timeoutSec)) {
        std::string summary = timeoutSec > 0
            ? ("The attempt did not finish within " + std::to_string(timeoutSec) + " s.")
            : "The attempt timed out.";
        return {"timeout", summary};
    }

    // 4. idle_stall: output and CPU activity stopped
    if (r == "idle_timed_out" || r == "idle_stall") {
        return {"idle_stall", "Output and CPU activity stopped."};
    }

    // 5. crash: NTSTATUS exception exit code
    if (IsCrashExitCode(exitCode)) {
        char hex[16] = {};
        std::snprintf(hex, sizeof(hex), "0x%08X", static_cast<unsigned>(exitCode));
        return {"crash", std::string("The process crashed with exit code ") + hex + "."};
    }

    // 6. exit_nonzero: process exited with a non-zero code
    if (exitCode != 0) {
        return {"exit_nonzero", "Exited with code " + std::to_string(exitCode) + "."};
    }

    // 7. success_check_failed: exit code was 0 but check condition not met
    if (!checkDetail.empty()) {
        return {"success_check_failed", checkDetail};
    }

    return {"", ""};
}

std::string RunRecordId(const std::string& jobId, int attempt) {
    return "rr_" + jobId + "_" + std::to_string(attempt);
}

nlohmann::json BuildRunRecord(const RunRecordInput& input) {
    std::string eventsPath = input.eventsFile;
    if (eventsPath.empty()) eventsPath = FindEventsFileForProcess(input.processId);
    std::vector<json> events = ReadEventsFile(eventsPath);

    std::string logPath = input.logFile;
    if (logPath.empty() && !input.jobId.empty()) {
        logPath = WideToUtf8(AttemptLogPathWide(input.jobId, input.attempt));
    }
    std::string logText = ResolveLogText(input, logPath);

    std::vector<std::string> filesRead;
    std::vector<std::string> filesEdited;
    std::vector<std::string> commands;
    std::vector<std::string> eventErrors;
    std::vector<std::string> subagentTypes;
    std::set<std::string> seenRead;
    std::set<std::string> seenEdited;
    int subagentCount = 0;

    std::string engine;
    std::string model;
    std::string sessionId;
    std::string eventCwd;
    std::string finalMessage;
    bool hasPlan = false;
    int64_t todoDone = 0;
    int64_t todoTotal = 0;
    int64_t totalTokens = 0;
    double costUsd = 0.0;

    for (const auto& ev : events) {
        const json* data = JObj(ev, "data");
        if (!data) continue;
        const json* agent = JObj(ev, "agent");
        if (agent) {
            std::string aType = JStr(*agent, "type");
            if (engine.empty() && !aType.empty() && !IEquals(aType, "generic")) engine = aType;
            std::string aModel = JStr(*agent, "model");
            if (model.empty() && !aModel.empty()) model = aModel;
            std::string aSession = JStr(*agent, "sessionId");
            if (sessionId.empty() && !aSession.empty()) sessionId = aSession;
        }

        std::string kind = JStr(ev, "kind");
        if (kind == "run.started") {
            std::string cwd = JStr(*data, "cwd");
            if (eventCwd.empty() && !cwd.empty()) eventCwd = cwd;
        } else if (kind == "tool.call") {
            std::string activity = JStr(*data, "activity");
            std::string target = Trim(JStr(*data, "target"));
            if (IEquals(activity, "read")) {
                std::string rel = RelativeToCwd(target, input.cwd);
                if (!rel.empty() && seenRead.insert(LowerAscii(rel)).second) filesRead.push_back(rel);
            } else if (IEquals(activity, "edit")) {
                std::string rel = RelativeToCwd(target, input.cwd);
                if (!rel.empty() && seenEdited.insert(LowerAscii(rel)).second) filesEdited.push_back(rel);
            } else if (IEquals(activity, "command") && !target.empty()) {
                commands.push_back(target);
            }
        } else if (kind == "tool.result") {
            std::string err = JStr(*data, "error");
            if (!err.empty()) eventErrors.push_back(err);
        } else if (kind == "error") {
            std::string msg = JStr(*data, "message");
            if (!msg.empty()) eventErrors.push_back(msg);
        } else if (kind == "plan") {
            auto itemsIt = data->find("items");
            if (itemsIt != data->end() && itemsIt->is_array()) {
                hasPlan = true;
                todoDone = 0;
                todoTotal = static_cast<int64_t>(itemsIt->size());
                for (const auto& item : *itemsIt) {
                    if (!item.is_object()) continue;
                    std::string status = JStr(item, "status");
                    if (IEquals(status, "completed") || IEquals(status, "done")) ++todoDone;
                }
            }
        } else if (kind == "message") {
            std::string text = JStr(*data, "text");
            if (!text.empty()) finalMessage = text;
        } else if (kind == "usage") {
            int64_t tokens = JInt(*data, "totalTokens");
            if (tokens <= 0) {
                tokens = JInt(*data, "inputTokens") + JInt(*data, "outputTokens") +
                         JInt(*data, "thinkingTokens") + JInt(*data, "cacheReadTokens") +
                         JInt(*data, "cacheWriteTokens");
            }
            totalTokens += tokens;
            costUsd += JDouble(*data, "costUsd");
        } else if (kind == "subagent.started") {
            ++subagentCount;
            std::string type = JStr(*data, "type");
            if (!type.empty() &&
                std::find(subagentTypes.begin(), subagentTypes.end(), type) == subagentTypes.end()) {
                subagentTypes.push_back(type);
            }
        }
    }

    if (filesRead.size() > kFilesPerList) filesRead.resize(kFilesPerList);
    if (filesEdited.size() > kFilesPerList) filesEdited.resize(kFilesPerList);
    if (commands.empty() && !Trim(input.command).empty()) commands.push_back(Trim(input.command));
    commands = SelectCommands(commands);

    std::vector<std::string> logErrors;
    if (!logText.empty()) {
        std::string tail = GetTailLines(logText, 50);
        size_t start = 0;
        while (start <= tail.size()) {
            size_t end = tail.find('\n', start);
            std::string line = end == std::string::npos ? tail.substr(start)
                                                        : tail.substr(start, end - start);
            start = end == std::string::npos ? tail.size() + 1 : end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            if (!LooksLikeErrorLine(line)) continue;
            logErrors.push_back(Utf8PrefixBytes(line, 500));
        }
    }

    std::map<std::string, int> errorCounts;
    for (const auto& raw : eventErrors) {
        std::string norm = NormalizeErrorLine(Utf8PrefixBytes(raw, 500));
        if (!norm.empty()) ++errorCounts[norm];
    }
    for (const auto& raw : logErrors) {
        std::string norm = NormalizeErrorLine(raw);
        if (!norm.empty()) ++errorCounts[norm];
    }
    std::vector<std::pair<std::string, int>> sortedErrors(errorCounts.begin(), errorCounts.end());
    std::sort(sortedErrors.begin(), sortedErrors.end(),
              [](const auto& a, const auto& b) {
                  if (a.second != b.second) return a.second > b.second;
                  return a.first < b.first;
              });
    std::vector<std::string> errors;
    for (const auto& kv : sortedErrors) {
        if (errors.size() >= kErrorsMax) break;
        std::string text = kv.first;
        if (kv.second > 1) text += " (x" + std::to_string(kv.second) + ")";
        errors.push_back(text);
    }

    std::string start = input.start;
    std::string end = input.end;
    int64_t startEpoch = ParseIsoEpoch(start);
    int64_t endEpoch = ParseIsoEpoch(end);
    int64_t durationSec = (startEpoch > 0 && endEpoch > startEpoch) ? endEpoch - startEpoch : 0;

    std::string createdAt = input.createdAt;
    if (createdAt.empty()) createdAt = !end.empty() ? end : start;

    std::string goal = Trim(input.summary);
    std::string instructions = Trim(input.instructions);
    if (!instructions.empty()) {
        if (!goal.empty()) goal += " ";
        goal += instructions;
    }
    goal = Utf8PrefixChars(goal, kGoalChars);

    json out;
    out["schema"] = "hancho.memory.run/1";
    out["id"] = RunRecordId(input.jobId, input.attempt);
    out["jobId"] = input.jobId;
    out["taskqueue"] = input.taskqueue;
    out["jobName"] = input.jobName;
    if (!input.btId.empty() || !input.btNode.empty() || input.btRun > 0) {
        json bt;
        if (!input.btId.empty()) bt["id"] = input.btId;
        if (!input.btNode.empty()) bt["node"] = input.btNode;
        if (input.btRun > 0) bt["run"] = input.btRun;
        out["bt"] = std::move(bt);
    }
    out["attempt"] = input.attempt;

    json origin;
    if (!input.originClient.empty()) origin["client"] = input.originClient;
    if (!input.originSession.empty()) origin["session"] = input.originSession;
    if (!input.originCwd.empty()) origin["cwd"] = input.originCwd;
    if (!origin.empty()) out["origin"] = std::move(origin);

    if (!engine.empty()) out["engine"] = engine;
    if (!model.empty()) out["model"] = model;
    if (!sessionId.empty()) out["sessionId"] = sessionId;
    out["goal"] = goal;
    std::string cwd = input.cwd.empty() ? eventCwd : input.cwd;
    if (!cwd.empty()) out["cwd"] = cwd;
    out["start"] = start;
    out["end"] = end;
    out["durationSec"] = durationSec;
    if (input.timeoutSec > 0) out["timeoutSec"] = input.timeoutSec;
    out["outcome"] = OutcomeFor(input.reason, input.jobState);
    std::string failureClass = input.failureClass;
    std::string failureSummary = input.failureSummary;
    if (failureClass.empty() && !IEquals(out["outcome"], "success")) {
        auto classified = ClassifyDeterministicFailure(
            input.reason, input.exitCode, out["outcome"], input.checkDetail,
            input.timeoutSec, durationSec);
        if (!classified.first.empty()) {
            failureClass = classified.first;
            if (failureSummary.empty()) failureSummary = classified.second;
        }
    }

    if (!failureClass.empty()) {
        json failure;
        failure["class"] = failureClass;
        if (!failureSummary.empty()) failure["summary"] = failureSummary;
        out["failure"] = std::move(failure);
    } else if (!input.checkDetail.empty() &&
               !IEquals(out["outcome"], "success")) {
        json failure;
        failure["summary"] = input.checkDetail;
        out["failure"] = std::move(failure);
    }
    out["exitCode"] = input.exitCode;
    out["filesRead"] = filesRead;
    out["filesEdited"] = filesEdited;
    out["commands"] = commands;
    out["errors"] = errors;
    if (hasPlan) {
        out["todos"] = {{"done", todoDone}, {"total", todoTotal}};
    }
    if (!finalMessage.empty()) {
        out["finalMessage"] = Utf8PrefixBytes(finalMessage, kFinalMessageBytes);
    }
    if (totalTokens > 0 || costUsd > 0.0) {
        json usage;
        usage["totalTokens"] = totalTokens;
        usage["costUsd"] = costUsd;
        out["usage"] = std::move(usage);
    }
    if (subagentCount > 0) {
        json sub;
        sub["count"] = subagentCount;
        sub["types"] = subagentTypes;
        out["subagents"] = std::move(sub);
    }

    json sources;
    if (!eventsPath.empty()) {
        sources["eventsFile"] = BaseName(eventsPath);
        if (!events.empty()) {
            uint64_t first = 0;
            uint64_t last = 0;
            bool any = false;
            for (const auto& ev : events) {
                auto it = ev.find("seq");
                if (it == ev.end()) continue;
                uint64_t seq = 0;
                if (it->is_number_unsigned()) seq = it->get<uint64_t>();
                else if (it->is_number_integer()) {
                    int64_t v = it->get<int64_t>();
                    if (v < 0) continue;
                    seq = static_cast<uint64_t>(v);
                } else {
                    continue;
                }
                if (!any || seq < first) first = seq;
                if (!any || seq > last) last = seq;
                any = true;
            }
            if (any) sources["seqRange"] = json::array({first, last});
        }
    }
    std::string logSource = !logPath.empty() ? logPath : input.lastLinesFile;
    if (!logSource.empty()) sources["logFile"] = BaseName(logSource);
    if (!sources.empty()) out["sources"] = std::move(sources);

    out["createdAt"] = createdAt;
    return out;
}

std::string RunRecordsDirectoryUtf8() {
    return WideToUtf8(RecordsDirectory().wstring());
}

std::string AppendRunRecordJson(const RunRecordInput& input) {
    try {
        json record = BuildRunRecord(input);
        std::string id = JStr(record, "id");
        std::string month = MonthOf(JStr(record, "start"));
        if (month.empty()) month = MonthOf(JStr(record, "createdAt"));
        if (month.empty()) month = MonthOf(JStr(record, "end"));
        if (month.empty()) month = "1970-01";

        json out;
        out["ok"] = true;
        out["id"] = id;
        if (RecordIdExists(id)) {
            out["created"] = false;
            out["path"] = "memory/records/" + month + ".jsonl";
            return DumpJson(out);
        }

        std::wstring dir = RecordsDirectory().wstring();
        EnsureDirectoryWide(dir);
        std::wstring path = PathJoinWide(dir, Utf8ToWide(month) + L".jsonl");
        AppendToLogFileWide(path, DumpJson(record) + "\r\n");
        out["created"] = true;
        out["path"] = "memory/records/" + month + ".jsonl";

        // Enforce memory capacity limit if configured.
        EnforceMemoryLimits(false);

        return DumpJson(out);
    } catch (const std::exception& e) {
        json out;
        out["ok"] = false;
        out["error"] = std::string("memory record append failed: ") + e.what();
        return DumpJson(out);
    }
}

nlohmann::json BuildPlainRunRecord(const PlainRunRecordInput& input) {
    RunRecordInput in;
    in.jobId = !input.jobId.empty() ? input.jobId : ("j_plain_" + std::to_string(input.processId));
    in.attempt = input.attempt > 0 ? input.attempt : 1;
    in.taskqueue = !input.taskqueue.empty() ? input.taskqueue : "plain";
    in.jobName = !input.jobName.empty() ? input.jobName : (!input.command.empty() ? input.command : "plain");
    in.summary = input.summary;
    in.command = input.command;
    in.cwd = input.cwd;
    in.start = input.start;
    in.end = input.end;
    in.exitCode = input.exitCode;
    in.reason = !input.reason.empty() ? input.reason : (input.exitCode == 0 ? "ok" : "failed");
    in.jobState = (input.exitCode == 0 && (in.reason == "ok" || in.reason == "success")) ? "succeeded" : "failed";
    in.timeoutSec = input.timeoutSec;
    in.processId = input.processId;
    in.logFile = input.logFile;
    in.lastLinesFile = input.lastLinesFile;
    in.checkDetail = input.checkDetail;
    return BuildRunRecord(in);
}

std::string AppendPlainRunRecordJson(const PlainRunRecordInput& input) {
    RunRecordInput in;
    in.jobId = !input.jobId.empty() ? input.jobId : ("j_plain_" + std::to_string(input.processId));
    in.attempt = input.attempt > 0 ? input.attempt : 1;
    in.taskqueue = !input.taskqueue.empty() ? input.taskqueue : "plain";
    in.jobName = !input.jobName.empty() ? input.jobName : (!input.command.empty() ? input.command : "plain");
    in.summary = input.summary;
    in.command = input.command;
    in.cwd = input.cwd;
    in.start = input.start;
    in.end = input.end;
    in.exitCode = input.exitCode;
    in.reason = !input.reason.empty() ? input.reason : (input.exitCode == 0 ? "ok" : "failed");
    in.jobState = (input.exitCode == 0 && (in.reason == "ok" || in.reason == "success")) ? "succeeded" : "failed";
    in.timeoutSec = input.timeoutSec;
    in.processId = input.processId;
    in.logFile = input.logFile;
    in.lastLinesFile = input.lastLinesFile;
    in.checkDetail = input.checkDetail;
    return AppendRunRecordJson(in);
}

std::string RebuildRunRecordsJson() {
    try {
        json out;
        out["ok"] = true;
        std::wstring tqDir = PathJoinWide(GetLogsDirectoryWide(), L"taskqueues");
        std::vector<std::wstring> tqFiles = ListDirectoryWide(tqDir, L"*.json");
        if (tqFiles.empty()) {
            out["skipped"] = true;
            out["records"] = 0;
            out["files"] = 0;
            out["months"] = json::array();
            out["reason"] = "no taskqueue state files";
            return DumpJson(out);
        }

        std::vector<json> records;
        std::set<std::string> seenIds;

        for (const auto& tqPath : tqFiles) {
            std::string text = ReadFileUtf8Wide(tqPath);
            if (text.empty()) continue;
            if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
                static_cast<unsigned char>(text[1]) == 0xBB &&
                static_cast<unsigned char>(text[2]) == 0xBF) {
                text.erase(0, 3);
            }
            json root = json::parse(text, nullptr, false);
            if (!root.is_object()) continue;
            std::string tqName = JStr(root, "name");
            auto jobsIt = root.find("jobs");
            if (jobsIt == root.end() || !jobsIt->is_array()) continue;

            for (const auto& job : *jobsIt) {
                if (!job.is_object()) continue;
                if (JStr(job, "id").empty()) continue;

                std::string jobState = JStr(job, "state");
                const json* instructions = JObj(job, "instructions");
                const json* bt = JObj(job, "bt");
                const json* origin = JObj(job, "origin");
                std::string jobCreatedAt = JStr(job, "created_at");
                std::string jobUpdatedAt = JStr(job, "updated_at");

                auto attemptsIt = job.find("attempts");
                if (attemptsIt == job.end() || !attemptsIt->is_array()) continue;

                for (const auto& attempt : *attemptsIt) {
                    if (!attempt.is_object()) continue;

                    RunRecordInput in;
                    in.jobId = JStr(job, "id");
                    in.taskqueue = !JStr(job, "taskqueue").empty() ? JStr(job, "taskqueue") : tqName;
                    in.jobName = JStr(job, "name");
                    in.summary = JStr(job, "summary");
                    in.instructions = instructions ? JStr(*instructions, "text") : std::string();
                    in.command = JStr(job, "command");
                    in.cwd = JStr(job, "cwd");
                    in.timeoutSec = static_cast<int>(JInt(job, "timeoutSec", JInt(job, "timeout_sec", 0)));
                    if (bt) {
                        in.btId = JStr(*bt, "id");
                        in.btNode = JStr(*bt, "node");
                        in.btRun = static_cast<int>(JInt(*bt, "run"));
                    }
                    in.attempt = static_cast<int>(JInt(attempt, "n", 1));
                    if (in.attempt <= 0) in.attempt = 1;
                    if (origin) {
                        in.originClient = JStr(*origin, "client");
                        in.originSession = JStr(*origin, "session");
                        in.originCwd = JStr(*origin, "cwd");
                    }
                    in.start = JStr(attempt, "start");
                    in.end = JStr(attempt, "end");
                    // A running attempt has no end time yet; it gets its record when it
                    // finishes, exactly like the live path.
                    if (in.end.empty()) continue;
                    in.createdAt = !in.end.empty() ? in.end
                                  : !in.start.empty() ? in.start
                                  : !jobUpdatedAt.empty() ? jobUpdatedAt : jobCreatedAt;
                    in.exitCode = static_cast<int>(JInt(attempt, "exit_code", -1));
                    in.reason = JStr(attempt, "reason");
                    in.jobState = jobState;
                    const json* failure = JObj(attempt, "failure");
                    if (failure) {
                        in.failureClass = JStr(*failure, "class");
                        in.failureSummary = JStr(*failure, "summary");
                    }
                    in.checkDetail = JStr(attempt, "check_detail");
                    in.processId = static_cast<int>(JInt(attempt, "process_id", 0));
                    in.eventsFile = FindEventsFileForProcess(in.processId);
                    std::string attLog = JStr(attempt, "log_file");
                    if (attLog.empty()) attLog = JStr(attempt, "logFile");
                    in.logFile = !attLog.empty() ? attLog : WideToUtf8(AttemptLogPathWide(in.jobId, in.attempt));
                    in.lastLinesFile = JStr(attempt, "last_lines_file");

                    json record = BuildRunRecord(in);
                    std::string id = JStr(record, "id");
                    if (id.empty()) continue;
                    if (!seenIds.insert(id).second) continue;
                    records.push_back(std::move(record));
                }
            }
        }

        std::sort(records.begin(), records.end(), [](const json& a, const json& b) {
            std::string as = JStr(a, "start");
            std::string bs = JStr(b, "start");
            if (as != bs) return as < bs;
            return JStr(a, "id") < JStr(b, "id");
        });

        const size_t generatedCount = records.size();
        std::map<std::string, std::pair<json, std::string>> byId;
        for (auto& existing : LoadExistingRecords()) {
            std::string id = JStr(existing.first, "id");
            byId[id] = std::move(existing);
        }
        for (auto& record : records) {
            std::string id = JStr(record, "id");
            std::string month = MonthOf(JStr(record, "start"));
            if (month.empty()) month = MonthOf(JStr(record, "createdAt"));
            if (month.empty()) month = MonthOf(JStr(record, "end"));
            byId[id] = {std::move(record), month};
        }

        std::map<std::string, std::vector<json>> byMonth;
        for (auto& kv : byId) {
            std::string month = kv.second.second;
            if (month.empty()) month = MonthOf(JStr(kv.second.first, "start"));
            if (month.empty()) month = MonthOf(JStr(kv.second.first, "createdAt"));
            if (month.empty()) month = "1970-01";
            byMonth[month].push_back(std::move(kv.second.first));
        }
        for (auto& kv : byMonth) {
            std::sort(kv.second.begin(), kv.second.end(), [](const json& a, const json& b) {
                std::string as = JStr(a, "start");
                std::string bs = JStr(b, "start");
                if (as != bs) return as < bs;
                return JStr(a, "id") < JStr(b, "id");
            });
        }

        std::wstring dir = RecordsDirectory().wstring();
        EnsureDirectoryWide(dir);
        for (const auto& path : ListDirectoryWide(dir, L"*.jsonl")) {
            std::error_code ec;
            fs::remove(fs::path(path), ec);
        }

        for (const auto& kv : byMonth) {
            std::string text;
            for (const auto& line : kv.second) {
                text += DumpJson(line);
                text += "\r\n";
            }
            std::wstring path = PathJoinWide(dir, Utf8ToWide(kv.first) + L".jsonl");
            WriteFileAtomicWide(path, text);
        }

        json months = json::array();
        for (const auto& kv : byMonth) months.push_back(kv.first);
        out["skipped"] = false;
        out["records"] = generatedCount;
        out["totalRecords"] = byId.size();
        out["files"] = byMonth.size();
        out["months"] = std::move(months);

        // Enforce configured retention and capacity limits after rebuild
        int retDays = GetConfiguredRetentionDays();
        int64_t limitBytes = GetConfiguredMemoryLimitBytes();
        if (retDays > 0 || limitBytes > 0) {
            PruneOptions popt;
            popt.dryRun = false;
            popt.retentionDays = retDays;
            popt.limitBytes = limitBytes;
            PruneRunRecords(popt);
        }

        return DumpJson(out);
    } catch (const std::exception& e) {
        json out;
        out["ok"] = false;
        out["error"] = std::string("memory record rebuild failed: ") + e.what();
        return DumpJson(out);
    }
}

int64_t GetConfiguredMemoryLimitBytes() {
    try {
        std::wstring configPath = PathJoinWide(GetEffectiveDataDirectoryWide(), L"config.json");
        std::string content = ReadFileUtf8Wide(configPath);
        if (content.empty()) return 0;
        json j = json::parse(content, nullptr, false);
        if (!j.is_object()) return 0;
        if (j.contains("memory_max_bytes")) {
            return JInt(j, "memory_max_bytes", 0);
        }
        if (j.contains("memory_limit_bytes")) {
            return JInt(j, "memory_limit_bytes", 0);
        }
        if (j.contains("memory_max_mb")) {
            return JInt(j, "memory_max_mb", 0) * 1024LL * 1024LL;
        }
    } catch (...) {
    }
    return 0;
}

int GetConfiguredRetentionDays() {
    try {
        std::wstring configPath = PathJoinWide(GetEffectiveDataDirectoryWide(), L"config.json");
        std::string content = ReadFileUtf8Wide(configPath);
        if (content.empty()) return 90;
        json j = json::parse(content, nullptr, false);
        if (!j.is_object()) return 90;
        if (j.contains("memory_retention_days")) {
            int days = static_cast<int>(JInt(j, "memory_retention_days", 90));
            return days > 0 ? days : 90;
        }
    } catch (...) {
    }
    return 90;
}

MemoryStorageStats GetMemoryStorageStats() {
    MemoryStorageStats stats;
    stats.limitBytes = GetConfiguredMemoryLimitBytes();
    std::error_code ec;

    fs::path memDir = MemoryRoot();
    if (!fs::exists(memDir, ec)) {
        return stats;
    }

    // L1: records/*.jsonl
    fs::path recDir = RecordsDirectory();
    if (fs::is_directory(recDir, ec)) {
        for (const auto& ent : fs::directory_iterator(recDir, ec)) {
            if (ent.is_regular_file(ec) && ent.path().extension() == L".jsonl") {
                uintmax_t sz = ent.file_size(ec);
                if (!ec) stats.l1Bytes += static_cast<int64_t>(sz);
            }
        }
    }

    // L2: notes.jsonl
    fs::path notesPath = memDir / L"notes.jsonl";
    if (fs::is_regular_file(notesPath, ec)) {
        uintmax_t sz = fs::file_size(notesPath, ec);
        if (!ec) stats.l2Bytes = static_cast<int64_t>(sz);
    }

    // L3: knowledge.jsonl
    fs::path knowledgePath = memDir / L"knowledge.jsonl";
    if (fs::is_regular_file(knowledgePath, ec)) {
        uintmax_t sz = fs::file_size(knowledgePath, ec);
        if (!ec) stats.l3Bytes = static_cast<int64_t>(sz);
    }

    // index/*
    fs::path indexDir = memDir / L"index";
    if (fs::is_directory(indexDir, ec)) {
        for (const auto& ent : fs::directory_iterator(indexDir, ec)) {
            if (ent.is_regular_file(ec)) {
                uintmax_t sz = ent.file_size(ec);
                if (!ec) stats.indexBytes += static_cast<int64_t>(sz);
            }
        }
    }

    stats.totalBytes = stats.l1Bytes + stats.l2Bytes + stats.l3Bytes + stats.indexBytes + stats.otherBytes;

    if (stats.limitBytes > 0) {
        stats.nearLimit = (stats.totalBytes >= static_cast<int64_t>(stats.limitBytes * 0.8));
        stats.exceeded = (stats.totalBytes > stats.limitBytes);
        if (stats.exceeded) {
            stats.warning = "Memory storage limit exceeded (" + FormatBytes(stats.totalBytes) + " / " +
                            FormatBytes(stats.limitBytes) + "). L2 notes and L3 knowledge cannot be deleted automatically.";
        } else if (stats.nearLimit) {
            stats.warning = "Memory storage is near configured limit (" + FormatBytes(stats.totalBytes) + " / " +
                            FormatBytes(stats.limitBytes) + ", >= 80%). L2/L3 are preserved.";
        }
    }

    return stats;
}

PruneResult PruneRunRecords(const PruneOptions& options) {
    PruneResult res;
    res.dryRun = options.dryRun;
    res.retentionDays = options.retentionDays;

    try {
        int64_t nowEpoch = options.nowEpoch > 0 ? options.nowEpoch : static_cast<int64_t>(std::time(nullptr));
        int64_t cutoffEpoch = (options.retentionDays > 0)
                                  ? (nowEpoch - static_cast<int64_t>(options.retentionDays) * 86400LL)
                                  : 0;

        int64_t limitBytes = (options.limitBytes >= 0) ? options.limitBytes : GetConfiguredMemoryLimitBytes();
        res.limitBytes = limitBytes;

        MemoryStorageStats statsBefore = GetMemoryStorageStats();
        res.totalBytesBefore = statsBefore.totalBytes;

        fs::path recDir = RecordsDirectory();
        std::error_code ec;
        if (!fs::is_directory(recDir, ec)) {
            res.totalBytesAfter = res.totalBytesBefore;
            res.nearLimit = statsBefore.nearLimit;
            res.exceeded = statsBefore.exceeded;
            res.warning = statsBefore.warning;
            return res;
        }

        struct MonthFile {
            std::string month;
            fs::path path;
            int64_t size = 0;
            int64_t monthEndEpoch = 0;
            bool pruneRetention = false;
            bool pruneCapacity = false;
        };

        std::vector<MonthFile> monthFiles;
        for (const auto& ent : fs::directory_iterator(recDir, ec)) {
            if (ec) break;
            if (!ent.is_regular_file(ec) || ent.path().extension() != L".jsonl") continue;
            std::string stem = WideToUtf8(ent.path().stem().wstring());
            int64_t sz = static_cast<int64_t>(ent.file_size(ec));
            if (ec) sz = 0;

            MonthFile mf;
            mf.month = stem;
            mf.path = ent.path();
            mf.size = sz;

            int y = 0, m = 0;
            if (sscanf_s(stem.c_str(), "%04d-%02d", &y, &m) == 2 && y > 1900 && m >= 1 && m <= 12) {
                int nextY = (m == 12) ? y + 1 : y;
                int nextM = (m == 12) ? 1 : m + 1;
                char nextBuf[32];
                snprintf(nextBuf, sizeof(nextBuf), "%04d-%02d-01 00:00:00", nextY, nextM);
                mf.monthEndEpoch = ParseIsoEpoch(nextBuf);
            } else {
                auto ftime = fs::last_write_time(ent.path(), ec);
                if (!ec) {
                    auto sctp = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
                        ftime - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
                    mf.monthEndEpoch = std::chrono::system_clock::to_time_t(sctp);
                }
            }

            if (options.retentionDays > 0 && mf.monthEndEpoch > 0 && mf.monthEndEpoch < cutoffEpoch) {
                mf.pruneRetention = true;
            }

            monthFiles.push_back(mf);
        }

        // Sort month files by month / end time ascending (oldest first)
        std::sort(monthFiles.begin(), monthFiles.end(), [](const MonthFile& a, const MonthFile& b) {
            if (a.monthEndEpoch != b.monthEndEpoch) return a.monthEndEpoch < b.monthEndEpoch;
            return a.month < b.month;
        });

        int64_t currentBytes = res.totalBytesBefore;

        // First pass: mark retention prune
        for (auto& mf : monthFiles) {
            if (mf.pruneRetention) {
                res.prunedFiles.push_back("memory/records/" + mf.month + ".jsonl");
                res.prunedMonths.push_back(mf.month);
                res.bytesFreed += mf.size;
                currentBytes -= mf.size;
            }
        }

        // Second pass: capacity limit prune (oldest remaining L1 files first)
        if (limitBytes > 0 && currentBytes > limitBytes) {
            for (auto& mf : monthFiles) {
                if (mf.pruneRetention) continue; // already marked
                mf.pruneCapacity = true;
                res.prunedFiles.push_back("memory/records/" + mf.month + ".jsonl");
                res.prunedMonths.push_back(mf.month);
                res.bytesFreed += mf.size;
                currentBytes -= mf.size;
                if (currentBytes <= limitBytes) break;
            }
        }

        // If not dry-run, actually delete the marked files
        if (!options.dryRun) {
            for (const auto& mf : monthFiles) {
                if (mf.pruneRetention || mf.pruneCapacity) {
                    fs::remove(mf.path, ec);
                }
            }
        }

        res.totalBytesAfter = res.totalBytesBefore - res.bytesFreed;
        if (res.totalBytesAfter < 0) res.totalBytesAfter = 0;

        if (limitBytes > 0) {
            res.nearLimit = (res.totalBytesAfter >= static_cast<int64_t>(limitBytes * 0.8));
            res.exceeded = (res.totalBytesAfter > limitBytes);
            if (res.exceeded) {
                res.warning = "Memory storage exceeds limit (" + FormatBytes(res.totalBytesAfter) + " > " +
                              FormatBytes(limitBytes) + ") even after deleting L1 records. L2/L3 are preserved.";
            } else if (res.nearLimit) {
                res.warning = "Memory storage is near configured limit (" + FormatBytes(res.totalBytesAfter) +
                              " / " + FormatBytes(limitBytes) + ", >= 80%). L2/L3 are preserved.";
            }
        }

        return res;
    } catch (const std::exception& e) {
        res.ok = false;
        res.error = std::string("memory prune failed: ") + e.what();
        return res;
    }
}

std::string PruneRunRecordsJson(const PruneOptions& options) {
    PruneResult res = PruneRunRecords(options);
    json out;
    out["ok"] = res.ok;
    if (!res.ok) {
        out["error"] = res.error;
        return DumpJson(out);
    }
    out["dryRun"] = res.dryRun;
    out["retentionDays"] = res.retentionDays;
    out["prunedFiles"] = res.prunedFiles;
    out["prunedMonths"] = res.prunedMonths;
    out["bytesFreed"] = res.bytesFreed;
    out["totalBytesBefore"] = res.totalBytesBefore;
    out["totalBytesAfter"] = res.totalBytesAfter;
    out["limitBytes"] = res.limitBytes;
    out["nearLimit"] = res.nearLimit;
    out["exceeded"] = res.exceeded;
    if (!res.warning.empty()) out["warning"] = res.warning;
    return DumpJson(out);
}

PruneResult EnforceMemoryLimits(bool dryRun) {
    int64_t limitBytes = GetConfiguredMemoryLimitBytes();
    if (limitBytes <= 0) {
        PruneResult r;
        r.ok = true;
        r.dryRun = dryRun;
        return r;
    }
    PruneOptions opt;
    opt.dryRun = dryRun;
    opt.retentionDays = 0; // only prune to satisfy capacity limit
    opt.limitBytes = limitBytes;
    return PruneRunRecords(opt);
}

} // namespace memory
} // namespace pc
