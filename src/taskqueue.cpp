#include "taskqueue.h"
#include "common.h"
#include "chain_guard.h"
#include "event_schema.h"
#include "memory_search.h"
#include "memory_record.h"
#include "platform/platform.h"
#include <sstream>
#include <algorithm>
#include <functional>
#include <cctype>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <chrono>
#include <set>
#include <regex>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <stdexcept>
#include <iomanip>

namespace pc {

// Defined with the process/log helpers further down; the restart path in the constructor
// classifies the interrupted attempt before those definitions.
static std::wstring GetTaskQueueLogDirWide();
static std::wstring GetAttemptLogPathWide(const std::string& jobId, int n);
static std::wstring GetAttemptDonePathWide(const std::string& jobId, int n);
static void AppendRunRecordForAttempt(const TaskQueue& tq, const TaskQueueJob& job,
                                      const TaskQueueAttempt& attempt,
                                      const TaskQueueFailureInput& fin,
                                      const std::string& logText);

std::string TaskQueueStateToString(TaskQueueState state) {
    switch (state) {
        case TaskQueueState::Pending:   return "pending";
        case TaskQueueState::Running:   return "running";
        case TaskQueueState::Succeeded: return "succeeded";
        case TaskQueueState::Failed:    return "failed";
        case TaskQueueState::Blocked:   return "blocked";
        case TaskQueueState::Cancelled: return "cancelled";
    }
    return "pending";
}

TaskQueueState TaskQueueStateFromString(const std::string& s) {
    if (s == "running") return TaskQueueState::Running;
    if (s == "succeeded") return TaskQueueState::Succeeded;
    if (s == "failed") return TaskQueueState::Failed;
    if (s == "blocked") return TaskQueueState::Blocked;
    if (s == "cancelled") return TaskQueueState::Cancelled;
    return TaskQueueState::Pending;
}

// ---------------------------------------------------------------------------
// File system locations
// ---------------------------------------------------------------------------

// SanitizeFileNameComponent truncates long names to 32 chars, which would make two queues
// whose names share a prefix (e.g. per-parallel queues of one tree) collide on disk.
// Append a hash of the full name when it was truncated so the file stays unique.
static std::wstring QueueNameComponentWide(const std::string& name) {
    std::string s = SanitizeFileNameComponent(name);
    if (name.size() > 32) {
        uint32_t hash = 2166136261u;
        for (unsigned char c : name) {
            hash ^= c;
            hash *= 16777619u;
        }
        char buf[16] = {};
        std::snprintf(buf, sizeof(buf), "-%08x", hash);
        s += buf;
    }
    return Utf8ToWide(s);
}

std::wstring TaskQueueManager::GetTaskQueuesDirWide() {
    std::wstring dir = PathJoinWide(GetLogsDirectoryWide(), L"taskqueues");
    EnsureDirectoryWide(dir);
    return dir;
}

std::wstring TaskQueueManager::GetTaskQueueFileWide(const std::string& name) {
    return PathJoinWide(GetTaskQueuesDirWide(), QueueNameComponentWide(name)) + L".json";
}

std::wstring TaskQueueManager::GetInstructionsDirWide(const std::string& name) {
    std::wstring base = PathJoinWide(GetTaskQueuesDirWide(), QueueNameComponentWide(name));
    EnsureDirectoryWide(base);
    std::wstring dir = PathJoinWide(base, L"instructions");
    EnsureDirectoryWide(dir);
    return dir;
}

std::wstring TaskQueueManager::GetInstructionsFileWide(const std::string& name, const std::string& jobId, int rev) {
    std::wstring fileName = Utf8ToWide(SanitizeFileNameComponent(jobId)) + L".r" + std::to_wstring(rev) + L".md";
    return PathJoinWide(GetInstructionsDirWide(name), fileName);
}

std::wstring TaskQueueManager::GetTaskEventsFileWide(const std::string& name) {
    return PathJoinWide(GetLogsDirectoryWide(), L"taskqueue-" + QueueNameComponentWide(name) + L".events.jsonl");
}

// ---------------------------------------------------------------------------
// Minimal JSON field accessors (snake_case keys used by the taskqueue layer)
// ---------------------------------------------------------------------------

static size_t FindValPos(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = 0;
    while (true) {
        size_t found = json.find(pattern, pos);
        if (found == std::string::npos) return std::string::npos;
        size_t afterKey = found + pattern.size();
        while (afterKey < json.size() && std::isspace(static_cast<unsigned char>(json[afterKey]))) afterKey++;
        if (afterKey < json.size() && json[afterKey] == ':') {
            afterKey++;
            while (afterKey < json.size() && std::isspace(static_cast<unsigned char>(json[afterKey]))) afterKey++;
            return afterKey;
        }
        pos = found + pattern.size();
    }
}

static bool HasJsonKey(const std::string& json, const std::string& key) {
    return FindValPos(json, key) != std::string::npos;
}

static std::string JsonFieldStr(const std::string& obj, const std::string& key, const std::string& defVal = "") {
    size_t p = FindValPos(obj, key);
    if (p == std::string::npos) return defVal;
    if (p < obj.size() && obj[p] == '"') {
        std::string out;
        size_t i = p + 1;
        while (i < obj.size()) {
            char c = obj[i++];
            if (c == '"') break;
            if (c == '\\' && i < obj.size()) {
                char esc = obj[i++];
                switch (esc) {
                    case '"': out.push_back('"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    default: out.push_back(esc); break;
                }
            } else {
                out.push_back(c);
            }
        }
        return out;
    }
    size_t end = p;
    while (end < obj.size() && obj[end] != ',' && obj[end] != '}' && obj[end] != ']' &&
           !std::isspace(static_cast<unsigned char>(obj[end]))) {
        end++;
    }
    std::string raw = obj.substr(p, end - p);
    if (raw == "null" || raw.empty()) return defVal;
    return raw;
}

static int64_t JsonFieldInt(const std::string& obj, const std::string& key, int64_t defVal = 0) {
    std::string s = JsonFieldStr(obj, key, "");
    if (s.empty()) return defVal;
    try {
        return std::stoll(s);
    } catch (...) {
        return defVal;
    }
}

static bool JsonFieldBool(const std::string& obj, const std::string& key, bool defVal = false) {
    std::string s = JsonFieldStr(obj, key, "");
    if (s == "true" || s == "1") return true;
    if (s == "false" || s == "0") return false;
    return defVal;
}

static std::string JsonFieldObj(const std::string& obj, const std::string& key, const std::string& defVal = "{}") {
    size_t p = FindValPos(obj, key);
    if (p == std::string::npos || p >= obj.size() || obj[p] != '{') return defVal;
    int depth = 0;
    bool inStr = false;
    bool escaped = false;
    for (size_t i = p; i < obj.size(); ++i) {
        char c = obj[i];
        if (inStr) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; continue; }
        if (c == '{') depth++;
        else if (c == '}') { depth--; if (depth == 0) return obj.substr(p, i - p + 1); }
    }
    return defVal;
}

static bool FindArrayBounds(const std::string& json, const std::string& key, size_t& outStart, size_t& outEnd) {
    size_t p = FindValPos(json, key);
    if (p == std::string::npos || p >= json.size() || json[p] != '[') return false;
    int depth = 0;
    bool inStr = false;
    bool escaped = false;
    for (size_t i = p; i < json.size(); ++i) {
        char c = json[i];
        if (inStr) {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; continue; }
        if (c == '[') depth++;
        else if (c == ']') { depth--; if (depth == 0) { outStart = p; outEnd = i; return true; } }
    }
    return false;
}

static std::string TrimJsonStr(const std::string& s) {
    size_t b = 0;
    size_t e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) b++;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) e--;
    return s.substr(b, e - b);
}

static std::vector<std::string> SplitTopLevelArray(const std::string& json, size_t openPos, size_t closePos) {
    std::vector<std::string> items;
    std::string current;
    int depth = 0;
    bool inStr = false;
    bool escaped = false;
    for (size_t i = openPos + 1; i < closePos && i < json.size(); ++i) {
        char c = json[i];
        if (inStr) {
            current.push_back(c);
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; current.push_back(c); continue; }
        if (c == '[' || c == '{') depth++;
        if (c == ']' || c == '}') depth--;
        if (c == ',' && depth == 0) {
            std::string item = TrimJsonStr(current);
            if (!item.empty()) items.push_back(item);
            current.clear();
            continue;
        }
        current.push_back(c);
    }
    std::string tail = TrimJsonStr(current);
    if (!tail.empty()) items.push_back(tail);
    return items;
}

static std::string StripQuotes(const std::string& s) {
    std::string t = TrimJsonStr(s);
    if (t.size() >= 2 && t.front() == '"' && t.back() == '"') return t.substr(1, t.size() - 2);
    return t;
}

static std::vector<std::string> JsonFieldStrArray(const std::string& obj, const std::string& key) {
    size_t start = 0, end = 0;
    if (!FindArrayBounds(obj, key, start, end)) return {};
    std::vector<std::string> out;
    for (const auto& it : SplitTopLevelArray(obj, start, end)) {
        std::string t = StripQuotes(it);
        if (!t.empty()) out.push_back(t);
    }
    return out;
}

static std::vector<std::string> JsonFieldObjArray(const std::string& obj, const std::string& key) {
    size_t start = 0, end = 0;
    if (!FindArrayBounds(obj, key, start, end)) return {};
    std::vector<std::string> out;
    for (const auto& it : SplitTopLevelArray(obj, start, end)) {
        std::string t = StripQuotes(it);
        if (t.size() >= 2 && t.front() == '{' && t.back() == '}') out.push_back(t);
    }
    return out;
}

static std::string BuildErrorJson(const std::string& msg) {
    return "{\"ok\":false,\"error\":\"" + JsonEscape(msg) + "\"}";
}

// Accept both camelCase (design doc / MCP arguments) and snake_case keys.
static std::string NormalizeJobJsonKeys(const std::string& in) {
    static const char* kPairs[][2] = {
        {"timeoutSec", "timeout_sec"},
        {"estimateSec", "estimate_sec"},
        {"maxAttempts", "max_attempts"},
        {"retryDelaySec", "retry_delay_sec"},
        {"exitCodes", "exit_codes"},
        {"delaySec", "delay_sec"},
        {"dependsOn", "depends_on"},
        {"killExclude", "kill_exclude"},
        {"instructionsFile", "instructions_file"},
        {"instructionsText", "instructions_text"},
        {"outputSec", "output_sec"},
        {"cpuSec", "cpu_sec"},
        {"exitCode", "exit_code"},
        {"filesNewer", "files_newer"},
        {"outputRegex", "output_regex"},
        {"testReport", "test_report"},
        {"budgetChars", "budget_chars"},
        {"includeAgentNotes", "include_agent_notes"},
        {"waitForRateLimit", "wait_for_rate_limit"}
    };
    std::string out = in;
    for (const auto& pair : kPairs) {
        std::string pattern = std::string("\"") + pair[0] + "\"";
        std::string repl = std::string("\"") + pair[1] + "\"";
        size_t pos = 0;
        while ((pos = out.find(pattern, pos)) != std::string::npos) {
            out.replace(pos, pattern.size(), repl);
            pos += repl.size();
        }
    }
    return out;
}

} // namespace pc

namespace pc {

// ---------------------------------------------------------------------------
// JSON serialization (taskqueue.json format, snake_case keys)
// ---------------------------------------------------------------------------

static std::string StrArrayToJson(const std::vector<std::string>& v);

// ---------------------------------------------------------------------------
// Failure classifier (doc/design-v3.md 4.2-4.4)
//
// The rules below are evaluated from the top and the first match wins. Each rule
// records the original event seq or log line it acted on, so a diagnostic can be
// traced back to the data that produced it.
// ---------------------------------------------------------------------------

static std::string LowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

static bool MatchesNoCase(const std::string& text, const char* pattern) {
    if (text.empty()) return false;
    try {
        std::regex re(pattern, std::regex::icase);
        return std::regex_search(text, re);
    } catch (const std::regex_error&) {
        return false;
    }
}

// External rules may opt into case-insensitive matching with a leading (?i), the form
// used in doc/design-v3.md. std::regex's ECMAScript grammar does not parse it.
static bool PrepareRegex(std::string pattern, bool defaultIcase, std::regex& out) {
    bool icase = defaultIcase;
    if (pattern.rfind("(?i)", 0) == 0) {
        icase = true;
        pattern.erase(0, 4);
    } else if (pattern.rfind("(?-i)", 0) == 0) {
        icase = false;
        pattern.erase(0, 5);
    }
    std::regex::flag_type flags = std::regex::ECMAScript;
    if (icase) flags |= std::regex::icase;
    try {
        out = std::regex(pattern, flags);
        return true;
    } catch (const std::regex_error&) {
        return false;
    }
}

static std::vector<std::string> RuleStringList(const nlohmann::json& v) {
    std::vector<std::string> out;
    if (v.is_string()) {
        out.push_back(v.get<std::string>());
    } else if (v.is_array()) {
        for (const auto& item : v) {
            if (item.is_string()) out.push_back(item.get<std::string>());
        }
    }
    return out;
}

static std::vector<int> RuleIntList(const nlohmann::json& v) {
    std::vector<int> out;
    auto add = [&out](const nlohmann::json& item) {
        if (item.is_number_integer()) out.push_back(static_cast<int>(item.get<int64_t>()));
        else if (item.is_number_unsigned()) out.push_back(static_cast<int>(item.get<uint64_t>()));
    };
    if (v.is_array()) {
        for (const auto& item : v) add(item);
    } else {
        add(v);
    }
    return out;
}

std::string TaskQueueFailureRulesPath() {
    return WideToUtf8(GetEffectiveDataDirectoryWide()) + "/failure-rules.json";
}

std::vector<TaskQueueFailureRule> LoadTaskQueueFailureRules(const std::string& path) {
    std::vector<TaskQueueFailureRule> rules;
    if (path.empty()) return rules;
    std::string text = ReadFileUtf8Wide(Utf8ToWide(path));
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
        text.erase(0, 3);
    }
    nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded()) return rules;
    const nlohmann::json* list = nullptr;
    if (root.is_array()) {
        list = &root;
    } else if (root.is_object()) {
        auto it = root.find("rules");
        if (it != root.end() && it->is_array()) list = &*it;
    }
    if (!list) return rules;

    for (const auto& entry : *list) {
        if (!entry.is_object()) continue;
        auto str = [&entry](const char* key) {
            auto it = entry.find(key);
            return it != entry.end() && it->is_string() ? it->get<std::string>() : std::string();
        };
        TaskQueueFailureRule r;
        r.failureClass = str("class");
        if (r.failureClass.empty()) r.failureClass = str("failureClass");
        if (r.failureClass.empty()) continue;

        auto when = entry.find("when");
        if (when == entry.end() || !when->is_object()) continue;
        auto takeStrings = [&when](const char* key, std::vector<std::string>& out) {
            auto it = when->find(key);
            if (it != when->end()) out = RuleStringList(*it);
        };
        auto takeInts = [&when](const char* key, std::vector<int>& out) {
            auto it = when->find(key);
            if (it != when->end()) out = RuleIntList(*it);
        };
        takeStrings("jobName", r.jobNames);
        takeStrings("taskqueue", r.taskqueues);
        takeStrings("endKind", r.endKinds);
        takeInts("exitCode", r.exitCodes);
        if (when->contains("logRegex") && (*when)["logRegex"].is_string()) {
            r.logRegex = (*when)["logRegex"].get<std::string>();
        }
        if (when->contains("eventRegex") && (*when)["eventRegex"].is_string()) {
            r.eventRegex = (*when)["eventRegex"].get<std::string>();
        }
        if (when->contains("eventKind") && (*when)["eventKind"].is_string()) {
            r.eventKind = (*when)["eventKind"].get<std::string>();
        }
        auto takeBool = [&when](const char* key, bool& isSet, bool& value) {
            auto it = when->find(key);
            if (it != when->end() && it->is_boolean()) {
                isSet = true;
                value = it->get<bool>();
            }
        };
        takeBool("launchFailed", r.launchFailedSet, r.launchFailed);
        takeBool("cancelled", r.cancelledSet, r.cancelled);
        takeBool("interrupted", r.interruptedSet, r.interrupted);
        if (!r.HasConditions()) continue;

        std::string confidence = LowerAscii(str("confidence"));
        if (confidence == "high" || confidence == "medium" || confidence == "low") {
            r.confidence = confidence;
        }
        r.summary = str("summary");
        r.suggestedAction = str("action");
        if (r.suggestedAction.empty()) r.suggestedAction = str("suggestedAction");
        auto detail = entry.find("detail");
        if (detail != entry.end() && detail->is_object()) r.detail = *detail;
        rules.push_back(std::move(r));
    }
    return rules;
}

static std::string JsonScalarToString(const nlohmann::json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number_unsigned()) return std::to_string(v.get<uint64_t>());
    if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
    if (v.is_number_float()) return std::to_string(v.get<double>());
    return "";
}

static uint64_t JsonUInt64(const nlohmann::json& o, const std::string& key, uint64_t defVal = 0) {
    auto it = o.find(key);
    if (it == o.end()) return defVal;
    if (it->is_number_unsigned()) return it->get<uint64_t>();
    if (it->is_number_integer()) {
        int64_t v = it->get<int64_t>();
        return v < 0 ? defVal : static_cast<uint64_t>(v);
    }
    return defVal;
}

static int64_t JsonInt64(const nlohmann::json& o, const std::string& key, int64_t defVal = 0) {
    auto it = o.find(key);
    if (it == o.end()) return defVal;
    if (it->is_number_unsigned()) return static_cast<int64_t>(it->get<uint64_t>());
    if (it->is_number_integer()) return it->get<int64_t>();
    return defVal;
}

static const nlohmann::json* LastEventOfKind(const std::vector<nlohmann::json>& events,
                                             const std::string& kind) {
    const nlohmann::json* found = nullptr;
    for (const auto& ev : events) {
        if (ev.is_object() && ev.value("kind", std::string()) == kind) found = &ev;
    }
    return found;
}

template <typename Pred>
static const nlohmann::json* FindLastEvent(const std::vector<nlohmann::json>& events, Pred pred) {
    const nlohmann::json* found = nullptr;
    for (const auto& ev : events) {
        if (ev.is_object() && pred(ev)) found = &ev;
    }
    return found;
}

static std::string EventDataString(const nlohmann::json& ev, const std::string& key) {
    auto data = ev.find("data");
    if (data == ev.end() || !data->is_object()) return "";
    auto v = data->find(key);
    if (v == data->end()) return "";
    return JsonScalarToString(*v);
}

static int64_t EventDataInt(const nlohmann::json& ev, const std::string& key, int64_t defVal = 0) {
    auto data = ev.find("data");
    if (data == ev.end() || !data->is_object()) return defVal;
    auto v = data->find(key);
    if (v == data->end()) return defVal;
    if (v->is_number_unsigned()) return static_cast<int64_t>(v->get<uint64_t>());
    if (v->is_number_integer()) return v->get<int64_t>();
    if (v->is_string()) {
        try { return std::stoll(v->get<std::string>()); } catch (...) { return defVal; }
    }
    return defVal;
}

static bool EventDataBool(const nlohmann::json& ev, const std::string& key, bool defVal) {
    auto data = ev.find("data");
    if (data == ev.end() || !data->is_object()) return defVal;
    auto v = data->find(key);
    if (v == data->end()) return defVal;
    if (v->is_boolean()) return v->get<bool>();
    if (v->is_string()) return LowerAscii(v->get<std::string>()) == "true";
    return defVal;
}

static uint64_t EventSeqOf(const nlohmann::json& ev) {
    auto it = ev.find("seq");
    if (it == ev.end()) return 0;
    if (it->is_number_unsigned()) return it->get<uint64_t>();
    if (it->is_number_integer()) return static_cast<uint64_t>(it->get<int64_t>());
    return 0;
}

// "2026-11-10T09:12:33.120Z" (or without the fraction/zone) to UTC epoch seconds.
static int64_t ParseIsoUtcSec(const std::string& s) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    char sep = 0;
    if (std::sscanf(s.c_str(), "%d-%d-%d%c%d:%d:%d", &y, &mo, &d, &sep, &h, &mi, &se) < 7) return 0;
    if (sep != 'T' && sep != ' ') return 0;
    std::tm tmv = {};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = se;
    return static_cast<int64_t>(_mkgmtime64(&tmv));
}

static std::string EpochToIso8601Local(int64_t epochSec) {
    time_t t = static_cast<time_t>(epochSec);
    std::tm local = {};
    if (localtime_s(&local, &t) != 0) return "";
    char base[32] = {};
    if (std::strftime(base, sizeof(base), "%Y-%m-%dT%H:%M:%S", &local) == 0) return "";
    std::tm asLocal = local;
    std::tm asUtc = local;
    std::time_t localEpoch = std::mktime(&asLocal);
    std::time_t utcEpoch = _mkgmtime64(&asUtc);
    long long offsetSec = static_cast<long long>(utcEpoch) - static_cast<long long>(localEpoch);
    if (offsetSec == 0) return std::string(base) + "Z";
    char sign = offsetSec < 0 ? '-' : '+';
    long long absSec = offsetSec < 0 ? -offsetSec : offsetSec;
    char off[8] = {};
    std::snprintf(off, sizeof(off), "%c%02lld:%02lld", sign, absSec / 3600, (absSec % 3600) / 60);
    return std::string(base) + off;
}

static int CountTextLines(const std::string& text) {
    if (text.empty()) return 0;
    int n = 0;
    for (char c : text) {
        if (c == '\n') n++;
    }
    if (text.back() != '\n') n++;
    return n;
}

static void FillFailureInputBase(TaskQueueFailureInput& in, const TaskQueueJob& job,
                                 const TaskQueueAttempt& attempt) {
    in.jobId = job.id;
    in.jobName = job.name;
    in.taskqueue = job.taskqueue;
    in.attempt = attempt.n;
    in.maxAttempts = job.maxAttempts;
    in.expectedExitCode = job.success.exitCode;
    in.timeoutSec = job.timeoutSec;
    in.idleOutputSec = job.idle.outputSec;
    in.idleCpuSec = job.idle.cpuSec;
    in.filesNewerConfigured = !job.success.filesNewer.empty();
    in.outputRegexConfigured = !job.success.outputRegex.empty();
    in.nowEpochSec = static_cast<int64_t>(std::time(nullptr));
    in.externalRules = LoadTaskQueueFailureRules(TaskQueueFailureRulesPath());
}

// Keeps the log tail for matching, but remembers where it started in the file so
// evidence can name a real line number.
static void FillFailureLog(TaskQueueFailureInput& in, const std::string& fullLog) {
    if (fullLog.empty()) return;
    const int kTailLines = 200;
    int total = CountTextLines(fullLog);
    in.logText = GetTailLines(fullLog, kTailLines);
    in.logLastLine = total;
    int tailLines = CountTextLines(in.logText);
    in.logFirstLine = total - tailLines + 1;
    if (in.logFirstLine < 1) in.logFirstLine = 1;
}

static void AddEventEvidence(TaskQueueFailureDiagnostic& d, const std::string& file, uint64_t seq) {
    if (file.empty() || seq == 0) return;
    for (const auto& e : d.evidence) {
        if (e.events == file && e.seq == seq) return;
    }
    TaskQueueFailureEvidence e;
    e.events = file;
    e.seq = seq;
    d.evidence.push_back(e);
}

static void AddLogEvidence(TaskQueueFailureDiagnostic& d, const std::string& file, int line) {
    if (file.empty() || line <= 0) return;
    for (const auto& e : d.evidence) {
        if (e.log == file && e.line == line) return;
    }
    TaskQueueFailureEvidence e;
    e.log = file;
    e.line = line;
    d.evidence.push_back(e);
}

// At least one pointer back to the source, even when the rule had no direct match.
static void EnsureEvidence(TaskQueueFailureDiagnostic& d, const TaskQueueFailureInput& in) {
    if (!d.evidence.empty()) return;
    if (in.logLastLine > 0 && !in.logFile.empty()) {
        AddLogEvidence(d, in.logFile, in.logLastLine);
        return;
    }
    AddEventEvidence(d, in.fallbackEventsFile, in.fallbackEventSeq);
}

// Last line in the log tail matching a pattern, as a file line number (0 when none).
static int FindLogLineNoCase(const TaskQueueFailureInput& in, const char* pattern) {
    if (in.logText.empty()) return 0;
    try {
        std::regex re(pattern, std::regex::icase);
        int lineNo = in.logFirstLine;
        int found = 0;
        size_t pos = 0;
        while (pos <= in.logText.size()) {
            size_t nl = in.logText.find('\n', pos);
            std::string line = nl == std::string::npos ? in.logText.substr(pos)
                                                       : in.logText.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && std::regex_search(line, re)) found = lineNo;
            if (nl == std::string::npos) break;
            pos = nl + 1;
            ++lineNo;
        }
        return found;
    } catch (const std::regex_error&) {
        return 0;
    }
}

// Last line in the log tail matching an external pattern, as a file line number (0 when none).
static int FindLogLineRegex(const TaskQueueFailureInput& in, const std::string& pattern) {
    if (in.logText.empty() || pattern.empty()) return 0;
    std::regex re;
    if (!PrepareRegex(pattern, false, re)) return 0;
    int lineNo = in.logFirstLine;
    int found = 0;
    size_t pos = 0;
    while (pos <= in.logText.size()) {
        size_t nl = in.logText.find('\n', pos);
        std::string line = nl == std::string::npos ? in.logText.substr(pos)
                                                   : in.logText.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && std::regex_search(line, re)) found = lineNo;
        if (nl == std::string::npos) break;
        pos = nl + 1;
        ++lineNo;
    }
    return found;
}

static std::string EventMatchText(const nlohmann::json& ev) {
    std::string message = EventDataString(ev, "message");
    if (!message.empty()) return message;
    auto data = ev.find("data");
    if (data != ev.end() && !data->is_null()) return data->dump();
    return "";
}

static const nlohmann::json* FindExternalRuleEvent(const TaskQueueFailureRule& rule,
                                                   const TaskQueueFailureInput& in) {
    if (rule.eventKind.empty() && rule.eventRegex.empty()) return nullptr;
    std::regex re;
    bool useRegex = !rule.eventRegex.empty();
    if (useRegex && !PrepareRegex(rule.eventRegex, false, re)) return nullptr;
    const nlohmann::json* found = nullptr;
    for (const auto& ev : in.events) {
        if (!ev.is_object()) continue;
        if (!rule.eventKind.empty() && ev.value("kind", std::string()) != rule.eventKind) continue;
        if (useRegex && !std::regex_search(EventMatchText(ev), re)) continue;
        found = &ev;
    }
    return found;
}

static bool MatchExternalRule(const TaskQueueFailureRule& rule, const TaskQueueFailureInput& in,
                              int& logLine, const nlohmann::json*& event) {
    logLine = 0;
    event = nullptr;
    if (!rule.HasConditions()) return false;
    if (!rule.exitCodes.empty()) {
        if (std::find(rule.exitCodes.begin(), rule.exitCodes.end(), in.exitCode) == rule.exitCodes.end()) {
            return false;
        }
    }
    auto matchesOneOf = [](const std::vector<std::string>& values, const std::string& actual) {
        if (values.empty()) return true;
        for (const auto& v : values) {
            if (LowerAscii(v) == LowerAscii(actual)) return true;
        }
        return false;
    };
    if (!matchesOneOf(rule.jobNames, in.jobName)) return false;
    if (!matchesOneOf(rule.taskqueues, in.taskqueue)) return false;
    if (!matchesOneOf(rule.endKinds, in.endKind)) return false;
    if (rule.launchFailedSet && in.launchFailed != rule.launchFailed) return false;
    if (rule.cancelledSet && in.cancelled != rule.cancelled) return false;
    if (rule.interruptedSet && in.interrupted != rule.interrupted) return false;
    if (!rule.logRegex.empty()) {
        logLine = FindLogLineRegex(in, rule.logRegex);
        if (logLine <= 0) return false;
    }
    if (!rule.eventKind.empty() || !rule.eventRegex.empty()) {
        event = FindExternalRuleEvent(rule, in);
        if (!event) return false;
    }
    return true;
}

static void FillFromExternalRule(TaskQueueFailureDiagnostic& d, const TaskQueueFailureRule& rule,
                                 int logLine, const nlohmann::json* event,
                                 const TaskQueueFailureInput& in) {
    d.failureClass = rule.failureClass;
    d.confidence = rule.confidence.empty() ? "high" : rule.confidence;
    d.summary = rule.summary.empty()
        ? "An external failure rule classified this as " + rule.failureClass + "."
        : rule.summary;
    d.detail = rule.detail.is_object() ? rule.detail : nlohmann::json::object();
    d.detail["rule"] = "external." + rule.failureClass;
    d.detail["external"] = true;
    d.suggestedAction = rule.suggestedAction.empty() ? "retry" : rule.suggestedAction;
    if (event) AddEventEvidence(d, in.eventsFile, EventSeqOf(*event));
    if (logLine > 0) AddLogEvidence(d, in.logFile, logLine);
    EnsureEvidence(d, in);
    if (!in.llmGuessJson.empty() && TaskQueueFailureNeedsLlmGuess(d)) {
        ApplyTaskQueueFailureLlmGuess(d, in.llmGuessJson);
    }
}

static bool IsCrashExitCode(int exitCode) {
    uint32_t u = static_cast<uint32_t>(exitCode);
    if (u == 0xFFFFFFFFu) return false; // a plain `exit(-1)`, not an NTSTATUS exception
    if (u >= 0xC0000000u) return true; // NTSTATUS exception (access violation, stack overflow, ...)
    if (u == 0x40000015u) return true; // fatal application exit
    return false;
}

static std::string SuccessConditionOf(const std::string& detail) {
    if (detail.rfind("missing file:", 0) == 0 ||
        detail.rfind("file not newer", 0) == 0 ||
        detail.rfind("cannot read mtime", 0) == 0) {
        return "filesNewer";
    }
    if (detail.find("output_regex") != std::string::npos) return "outputRegex";
    if (detail.rfind("test report", 0) == 0) return "testReport";
    if (detail.rfind("exit code ", 0) == 0) return "exitCode";
    return "";
}

static int CountUnfinishedPlanItems(const nlohmann::json& ev) {
    auto data = ev.find("data");
    if (data == ev.end() || !data->is_object()) return 0;
    auto items = data->find("items");
    if (items == data->end() || !items->is_array()) return 0;
    int remaining = 0;
    for (const auto& item : *items) {
        if (!item.is_object()) continue;
        auto st = item.find("status");
        std::string status = st == item.end() ? std::string() : LowerAscii(JsonScalarToString(*st));
        if (status.empty()) continue;
        if (status != "completed" && status != "done" && status != "cancelled") remaining++;
    }
    return remaining;
}

struct ToolErrorLoop {
    bool found = false;
    std::string tool;
    int errors = 0;
    uint64_t lastSeq = 0;
};

static ToolErrorLoop FindToolErrorLoop(const std::vector<nlohmann::json>& events) {
    std::map<std::string, std::string> toolByUseId;
    for (const auto& ev : events) {
        if (!ev.is_object() || ev.value("kind", std::string()) != "tool.call") continue;
        std::string useId = EventDataString(ev, "toolUseId");
        std::string tool = EventDataString(ev, "tool");
        if (!useId.empty() && !tool.empty()) toolByUseId[useId] = tool;
    }
    std::map<std::string, int> counts;
    std::map<std::string, uint64_t> lastSeq;
    for (const auto& ev : events) {
        if (!ev.is_object() || ev.value("kind", std::string()) != "tool.result") continue;
        if (EventDataBool(ev, "ok", true)) continue;
        std::string useId = EventDataString(ev, "toolUseId");
        std::string tool = toolByUseId.count(useId) ? toolByUseId[useId] : std::string("tool");
        counts[tool]++;
        lastSeq[tool] = EventSeqOf(ev);
    }
    ToolErrorLoop out;
    for (const auto& [tool, n] : counts) {
        if (n > out.errors) {
            out.errors = n;
            out.tool = tool;
            out.lastSeq = lastSeq[tool];
        }
    }
    out.found = out.errors >= 3;
    return out;
}

static void FillLastActivity(TaskQueueFailureDiagnostic& d, const TaskQueueFailureInput& in,
                             int64_t nowEpochSec) {
    for (auto it = in.events.rbegin(); it != in.events.rend(); ++it) {
        if (!it->is_object() || it->value("kind", std::string()) != "tool.call") continue;
        std::string activity = EventDataString(*it, "activity");
        if (activity.empty()) continue;
        d.lastActivity.activity = activity;
        d.lastActivity.target = EventDataString(*it, "target");
        int64_t ts = ParseIsoUtcSec(it->value("ts", std::string()));
        if (nowEpochSec > 0 && ts > 0 && nowEpochSec >= ts) d.lastActivity.sinceSec = nowEpochSec - ts;
        return;
    }
    if (in.lastActivity.empty() && in.lastActivityTarget.empty()) return;
    d.lastActivity.activity = in.lastActivity;
    d.lastActivity.target = in.lastActivityTarget;
    d.lastActivity.sinceSec = in.lastActivitySinceSec;
}

TaskQueueFailureDiagnostic ClassifyTaskQueueFailure(const TaskQueueFailureInput& in) {
    // Patterns are the built-in "fixed wording" the design refers to. They are tried in
    // this order only where a rule is reached; the rules themselves run top to bottom.
    static const char* kRatePattern =
        "(rate.?limit|too many requests|usage limit|quota exceeded|insufficient quota|resource exhausted|\\b429\\b)";
    static const char* kAuthPattern =
        "(invalid api key|invalid_api_key|invalid x-api-key|api key not valid|missing api key|"
        "401 unauthorized|authentication failed|not authenticated|unauthorized request)";
    static const char* kPermissionPattern =
        "(permission denied|permission_denied|access is denied|not permitted|eacces|operation not permitted)";
    static const char* kTurnPattern =
        "(max_turns|maximum number of turns|turn limit|context length|context window|maximum context|"
        "prompt is too long|too many tokens|token limit)";

    TaskQueueFailureDiagnostic d;
    d.detail = nlohmann::json::object();
    const int64_t now = in.nowEpochSec > 0 ? in.nowEpochSec : static_cast<int64_t>(std::time(nullptr));
    FillLastActivity(d, in, now);

    // 0. External rules from <data-dir>/failure-rules.json run first, so they can add a
    // class or take precedence over a built-in rule with the same class. The order in the
    // file is preserved and the first match wins, just like the built-in chain.
    for (const auto& rule : in.externalRules) {
        int logLine = 0;
        const nlohmann::json* event = nullptr;
        if (!MatchExternalRule(rule, in, logLine, event)) continue;
        FillFromExternalRule(d, rule, logLine, event, in);
        return d;
    }

    // Pre-read the normalized events once so the rules stay cheap and deterministic.
    const nlohmann::json* rateEvent = nullptr;
    for (const auto& ev : in.events) {
        if (!ev.is_object() || ev.value("kind", std::string()) != "rate_limit") continue;
        if (LowerAscii(EventDataString(ev, "status")) == "rejected") rateEvent = &ev;
    }
    const nlohmann::json* rateError = FindLastEvent(in.events, [&](const nlohmann::json& ev) {
        return ev.value("kind", std::string()) == "error" &&
               MatchesNoCase(EventDataString(ev, "message"), kRatePattern);
    });
    const nlohmann::json* authEvent = FindLastEvent(in.events, [&](const nlohmann::json& ev) {
        return ev.value("kind", std::string()) == "error" &&
               MatchesNoCase(EventDataString(ev, "message"), kAuthPattern);
    });
    const nlohmann::json* permissionEvent = FindLastEvent(in.events, [&](const nlohmann::json& ev) {
        return ev.value("kind", std::string()) == "error" &&
               MatchesNoCase(EventDataString(ev, "message"), kPermissionPattern);
    });
    const nlohmann::json* runEnded = LastEventOfKind(in.events, "run.ended");
    const nlohmann::json* turnEvent = FindLastEvent(in.events, [&](const nlohmann::json& ev) {
        return ev.value("kind", std::string()) == "error" &&
               MatchesNoCase(EventDataString(ev, "message"), kTurnPattern);
    });
    const nlohmann::json* planEvent = LastEventOfKind(in.events, "plan");
    const ToolErrorLoop toolLoop = FindToolErrorLoop(in.events);

    const bool completed = in.endKind == "completed";

    // 1. launch_error: the process never started.
    if (in.launchFailed) {
        d.failureClass = "launch_error";
        d.confidence = "high";
        d.summary = "The process could not be started.";
        d.detail["rule"] = "builtin.launch_error";
        if (!in.checkDetail.empty()) d.detail["error"] = in.checkDetail;
        d.suggestedAction = "fix_spec";
        EnsureEvidence(d, in);
        return d;
    }

    // 2. killed: a caller stopped the job, or Hancho restarted under a running attempt.
    if (in.interrupted || in.cancelled || in.endKind == "killed") {
        d.failureClass = "killed";
        d.confidence = "high";
        d.detail["rule"] = "builtin.killed";
        if (in.interrupted) {
            d.summary = "Hancho restarted while the attempt was running.";
            d.detail["reason"] = "interrupted";
            d.suggestedAction = "retry"; // interrupted attempts are re-run by design
        } else {
            d.summary = "The job was cancelled; the attempt was stopped.";
            d.detail["reason"] = "cancelled";
            d.suggestedAction = "human";
        }
        AddLogEvidence(d, in.logFile, in.logLastLine);
        EnsureEvidence(d, in);
        return d;
    }

    // 3. timeout: the attempt exceeded timeoutSec and was terminated.
    if (in.endKind == "timed_out") {
        d.failureClass = "timeout";
        d.confidence = "high";
        d.summary = "The attempt did not finish within " + std::to_string(in.timeoutSec) + " s.";
        d.detail["rule"] = "builtin.timeout";
        d.detail["timeoutSec"] = in.timeoutSec;
        if (in.elapsedSec > 0) d.detail["elapsedSec"] = in.elapsedSec;
        d.suggestedAction = "raise_timeout";
        AddLogEvidence(d, in.logFile, in.logLastLine);
        EnsureEvidence(d, in);
        return d;
    }

    // 4. idle_stall: output and CPU activity stopped.
    if (in.endKind == "idle_timed_out") {
        d.failureClass = "idle_stall";
        d.confidence = "high";
        d.summary = "Output and CPU activity stopped for " + std::to_string(in.idleOutputSec) + " s.";
        d.detail["rule"] = "builtin.idle_stall";
        d.detail["idleOutputSec"] = in.idleOutputSec;
        d.detail["idleCpuSec"] = in.idleCpuSec;
        d.suggestedAction = "retry";
        AddLogEvidence(d, in.logFile, in.logLastLine);
        EnsureEvidence(d, in);
        return d;
    }

    // 5. rate_limited: a rejected rate_limit event, or an equivalent error/log line.
    if (rateEvent || rateError) {
        const nlohmann::json& ev = rateEvent ? *rateEvent : *rateError;
        d.failureClass = "rate_limited";
        d.confidence = rateEvent ? "high" : "medium";
        d.detail["rule"] = "builtin.rate_limited";
        std::string window = EventDataString(ev, "window");
        if (!window.empty()) d.detail["window"] = window;
        std::string status = EventDataString(ev, "status");
        if (!status.empty()) d.detail["status"] = status;
        int64_t resets = EventDataInt(ev, "resetsAt", 0);
        if (resets > 0) {
            std::string iso = EpochToIso8601Local(resets);
            if (!iso.empty()) d.detail["resetsAt"] = iso;
            d.detail["resetsAtEpochSec"] = resets;
            std::string retryIso = EpochToIso8601Local(resets + 30);
            if (!retryIso.empty()) d.retryAt = retryIso;
        }
        auto util = ev.find("data");
        if (util != ev.end() && util->is_object()) {
            auto u = util->find("utilization");
            if (u != util->end() && u->is_number()) d.detail["utilization"] = *u;
        }
        d.summary = window.empty() ? "The agent was rate limited; wait before retrying."
                                   : "Rejected by the " + window + " rate limit; wait before retrying.";
        d.suggestedAction = "retry_after";
        AddEventEvidence(d, in.eventsFile, EventSeqOf(ev));
        EnsureEvidence(d, in);
        return d;
    }
    {
        int line = FindLogLineNoCase(in, kRatePattern);
        if (line > 0) {
            d.failureClass = "rate_limited";
            d.confidence = "medium";
            d.summary = "The log reports a rate limit; wait before retrying.";
            d.detail["rule"] = "builtin.rate_limited";
            d.detail["source"] = "log";
            d.suggestedAction = "retry_after";
            AddLogEvidence(d, in.logFile, line);
            return d;
        }
    }

    // 6. auth_error: an authentication error event, or the same wording in the log.
    if (authEvent) {
        d.failureClass = "auth_error";
        d.confidence = "high";
        std::string msg = EventDataString(*authEvent, "message");
        d.summary = msg.empty() ? "Authentication failed." : "Authentication failed: " + msg;
        d.detail["rule"] = "builtin.auth_error";
        if (!msg.empty()) d.detail["message"] = msg;
        d.suggestedAction = "human";
        AddEventEvidence(d, in.eventsFile, EventSeqOf(*authEvent));
        EnsureEvidence(d, in);
        return d;
    }
    {
        int line = FindLogLineNoCase(in, kAuthPattern);
        if (line > 0) {
            d.failureClass = "auth_error";
            d.confidence = "high";
            d.summary = "The log reports an authentication failure.";
            d.detail["rule"] = "builtin.auth_error";
            d.detail["source"] = "log";
            d.suggestedAction = "human";
            AddLogEvidence(d, in.logFile, line);
            return d;
        }
    }

    // 7. permission_denied: explicit denials, or a non-zero permission_denials count.
    {
        int64_t denials = runEnded ? EventDataInt(*runEnded, "permissionDenials", 0) : 0;
        if (permissionEvent || denials > 0) {
            d.failureClass = "permission_denied";
            d.confidence = permissionEvent ? "high" : "medium";
            d.detail["rule"] = "builtin.permission_denied";
            if (permissionEvent) {
                std::string msg = EventDataString(*permissionEvent, "message");
                if (!msg.empty()) d.detail["message"] = msg;
                d.summary = msg.empty() ? "A tool call was denied by permissions."
                                        : "Permission denied: " + msg;
            } else {
                d.detail["permissionDenials"] = denials;
                d.summary = "The agent was denied permission " + std::to_string(denials) + " time(s).";
            }
            d.suggestedAction = "fix_spec";
            if (permissionEvent) AddEventEvidence(d, in.eventsFile, EventSeqOf(*permissionEvent));
            EnsureEvidence(d, in);
            return d;
        }
        int line = FindLogLineNoCase(in, kPermissionPattern);
        if (line > 0) {
            d.failureClass = "permission_denied";
            d.confidence = "high";
            d.summary = "The log reports a permission denial.";
            d.detail["rule"] = "builtin.permission_denied";
            d.detail["source"] = "log";
            d.suggestedAction = "fix_spec";
            AddLogEvidence(d, in.logFile, line);
            return d;
        }
    }

    // 8. turn_or_context_limit: the agent stopped at a turn or context limit.
    {
        std::string stopReason = runEnded ? EventDataString(*runEnded, "stopReason") : "";
        if (MatchesNoCase(stopReason, kTurnPattern) || turnEvent) {
            const nlohmann::json* ev = turnEvent ? turnEvent : runEnded;
            d.failureClass = "turn_or_context_limit";
            d.confidence = "high";
            d.detail["rule"] = "builtin.turn_or_context_limit";
            if (!stopReason.empty()) d.detail["stopReason"] = stopReason;
            if (turnEvent) {
                std::string msg = EventDataString(*turnEvent, "message");
                if (!msg.empty()) d.detail["message"] = msg;
            }
            d.summary = stopReason.empty() ? "The agent stopped at a turn or context limit."
                                           : "The agent stopped at the " + stopReason + " limit.";
            d.suggestedAction = "escalate";
            if (ev) AddEventEvidence(d, in.eventsFile, EventSeqOf(*ev));
            EnsureEvidence(d, in);
            return d;
        }
    }

    // 9. crash: an NTSTATUS exception exit code.
    if (IsCrashExitCode(in.exitCode)) {
        d.failureClass = "crash";
        d.confidence = "high";
        char hex[16] = {};
        std::snprintf(hex, sizeof(hex), "0x%08X", static_cast<unsigned>(in.exitCode));
        d.summary = std::string("The process crashed with exit code ") + hex + ".";
        d.detail["rule"] = "builtin.crash";
        d.detail["exitCode"] = in.exitCode;
        d.detail["exitCodeHex"] = hex;
        d.suggestedAction = in.attempt >= in.maxAttempts ? "human" : "retry";
        AddLogEvidence(d, in.logFile, in.logLastLine);
        EnsureEvidence(d, in);
        return d;
    }

    // 10. tool_error_loop: the same tool failed repeatedly in this attempt.
    if (toolLoop.found) {
        d.failureClass = "tool_error_loop";
        d.confidence = "medium";
        d.summary = "The " + toolLoop.tool + " tool failed " + std::to_string(toolLoop.errors) + " times.";
        d.detail["rule"] = "builtin.tool_error_loop";
        d.detail["tool"] = toolLoop.tool;
        d.detail["errors"] = toolLoop.errors;
        d.suggestedAction = "escalate";
        AddEventEvidence(d, in.eventsFile, toolLoop.lastSeq);
        EnsureEvidence(d, in);
        return d;
    }

    // 11. dependency_blocked: a dependency failed, so this job never ran.
    if (in.blockKind == "dependency") {
        d.failureClass = "dependency_blocked";
        d.confidence = "high";
        d.summary = in.blockedBy.empty() ? "Blocked by a failed dependency."
                                         : "Blocked by the failed dependency " + in.blockedBy + ".";
        d.detail["rule"] = "builtin.dependency_blocked";
        if (!in.blockedBy.empty()) d.detail["blockedBy"] = in.blockedBy;
        d.suggestedAction = "human";
        EnsureEvidence(d, in);
        return d;
    }

    // 12. resource_conflict: the job is waiting for a resource lock.
    if (in.blockKind == "resource") {
        d.failureClass = "resource_conflict";
        d.confidence = "high";
        d.summary = in.blockedResource.empty() ? "Waiting for a resource lock."
                                               : "Waiting for " + in.blockedResource + " held by " +
                                                     (in.blockedBy.empty() ? "another job" : in.blockedBy) + ".";
        d.detail["rule"] = "builtin.resource_conflict";
        if (!in.blockedBy.empty()) d.detail["blockedBy"] = in.blockedBy;
        if (!in.blockedResource.empty()) d.detail["resource"] = in.blockedResource;
        d.suggestedAction = "human";
        EnsureEvidence(d, in);
        return d;
    }

    // 13. incomplete: the agent ended normally but left TODOs and did not satisfy the checks.
    if (completed && !in.success && planEvent) {
        int remaining = CountUnfinishedPlanItems(*planEvent);
        if (remaining > 0) {
            d.failureClass = "incomplete";
            d.confidence = "medium";
            d.summary = "The agent stopped with " + std::to_string(remaining) + " unfinished TODO item(s).";
            d.detail["rule"] = "builtin.incomplete";
            d.detail["remaining"] = remaining;
            d.suggestedAction = "retry";
            AddEventEvidence(d, in.eventsFile, EventSeqOf(*planEvent));
            EnsureEvidence(d, in);
            return d;
        }
    }

    // 14. success_check_failed: exit was fine but filesNewer/outputRegex/exitCode failed.
    if (completed && !in.success) {
        std::string condition = SuccessConditionOf(in.checkDetail);
        d.failureClass = "success_check_failed";
        d.confidence = "high";
        d.summary = in.checkDetail.empty() ? "A success condition was not met."
                                           : "Success condition not met: " + in.checkDetail;
        d.detail["rule"] = "builtin.success_check_failed";
        if (!condition.empty()) d.detail["condition"] = condition;
        if (!in.checkDetail.empty()) d.detail["detail"] = in.checkDetail;
        d.suggestedAction = "retry";
        AddLogEvidence(d, in.logFile, in.logLastLine);
        EnsureEvidence(d, in);
        return d;
    }

    // 15. exit_nonzero: the process exited with a non-zero code.
    if (in.endKind == "failed" || in.exitCode != in.expectedExitCode) {
        d.failureClass = "exit_nonzero";
        d.confidence = "high";
        d.summary = "Exited with code " + std::to_string(in.exitCode) +
                    " (expected " + std::to_string(in.expectedExitCode) + ").";
        d.detail["rule"] = "builtin.exit_nonzero";
        d.detail["exitCode"] = in.exitCode;
        d.detail["expectedExitCode"] = in.expectedExitCode;
        d.suggestedAction = "retry";
        AddLogEvidence(d, in.logFile, in.logLastLine);
        EnsureEvidence(d, in);
        return d;
    }

    // 16. unknown: nothing matched. Retry, and suggest escalating.
    d.failureClass = "unknown";
    d.confidence = "low";
    d.summary = "The failure did not match any known classification.";
    d.detail["rule"] = "builtin.unknown";
    d.detail["escalateSuggested"] = true;
    d.suggestedAction = "retry";
    EnsureEvidence(d, in);
    if (!in.llmGuessJson.empty()) ApplyTaskQueueFailureLlmGuess(d, in.llmGuessJson);
    return d;
}

std::string TaskQueueFailureDiagnosticToJson(const TaskQueueFailureDiagnostic& d) {
    if (d.Empty()) return "";
    nlohmann::json o;
    o["class"] = d.failureClass;
    o["confidence"] = d.confidence.empty() ? "low" : d.confidence;
    o["summary"] = d.summary;
    o["detail"] = d.detail.is_object() ? d.detail : nlohmann::json::object();
    nlohmann::json evidence = nlohmann::json::array();
    for (const auto& e : d.evidence) {
        nlohmann::json item = nlohmann::json::object();
        if (e.IsEvent()) {
            item["events"] = e.events;
            item["seq"] = e.seq;
        } else if (e.IsLog()) {
            item["log"] = e.log;
            item["line"] = e.line;
        } else {
            continue;
        }
        evidence.push_back(std::move(item));
    }
    o["evidence"] = std::move(evidence);
    if (d.lastActivity.Present()) {
        nlohmann::json activity;
        if (!d.lastActivity.activity.empty()) activity["activity"] = d.lastActivity.activity;
        if (!d.lastActivity.target.empty()) activity["target"] = d.lastActivity.target;
        if (d.lastActivity.sinceSec >= 0) activity["sinceSec"] = d.lastActivity.sinceSec;
        o["lastActivity"] = std::move(activity);
    } else {
        o["lastActivity"] = nullptr;
    }
    o["suggestedAction"] = d.suggestedAction.empty() ? "retry" : d.suggestedAction;
    o["retryAt"] = d.retryAt.empty() ? nlohmann::json(nullptr) : nlohmann::json(d.retryAt);
    if (d.llmGuess.Present()) {
        nlohmann::json guess = nlohmann::json::object();
        guess["class"] = d.llmGuess.failureClass;
        guess["confidence"] = "low";
        guess["advisory"] = true;
        if (!d.llmGuess.summary.empty()) guess["summary"] = d.llmGuess.summary;
        if (!d.llmGuess.suggestedAction.empty()) guess["suggestedAction"] = d.llmGuess.suggestedAction;
        if (!d.llmGuess.rationale.empty()) guess["rationale"] = d.llmGuess.rationale;
        if (!d.llmGuess.source.empty()) guess["source"] = d.llmGuess.source;
        o["llmGuess"] = std::move(guess);
    }
    // Log text and process error messages may contain invalid UTF-8; keep the record valid.
    return o.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

bool TaskQueueFailureNeedsLlmGuess(const TaskQueueFailureDiagnostic& diagnostic) {
    if (diagnostic.Empty()) return false;
    return LowerAscii(diagnostic.failureClass) == "unknown" ||
           LowerAscii(diagnostic.confidence) == "low";
}

std::string BuildTaskQueueFailureEscalationPacket(const TaskQueueFailureInput& input,
                                                  const TaskQueueFailureDiagnostic& diagnostic) {
    if (!TaskQueueFailureNeedsLlmGuess(diagnostic)) return "";

    nlohmann::json packet = nlohmann::json::object();
    packet["schema"] = "hancho.failure-guess/1";
    packet["question"] =
        "Which failure class fits this attempt? Answer from the evidence only; the "
        "deterministic classifier could not decide.";
    packet["guidance"] =
        "Reply with JSON {\"class\", \"summary\", \"action\", \"rationale\"}. The guess "
        "is advisory (confidence low) and will not trigger an automatic retry.";

    nlohmann::json attempt = nlohmann::json::object();
    attempt["jobId"] = input.jobId;
    attempt["jobName"] = input.jobName;
    attempt["taskqueue"] = input.taskqueue;
    attempt["attempt"] = input.attempt;
    attempt["maxAttempts"] = input.maxAttempts;
    attempt["endKind"] = input.endKind;
    attempt["exitCode"] = input.exitCode;
    attempt["expectedExitCode"] = input.expectedExitCode;
    attempt["timeoutSec"] = input.timeoutSec;
    attempt["elapsedSec"] = input.elapsedSec;
    attempt["success"] = input.success;
    if (!input.checkDetail.empty()) attempt["checkDetail"] = input.checkDetail;
    packet["attempt"] = std::move(attempt);

    nlohmann::json classifier = nlohmann::json::object();
    classifier["class"] = diagnostic.failureClass;
    classifier["confidence"] = diagnostic.confidence.empty() ? "low" : diagnostic.confidence;
    classifier["summary"] = diagnostic.summary;
    classifier["detail"] = diagnostic.detail.is_object() ? diagnostic.detail : nlohmann::json::object();
    classifier["suggestedAction"] = diagnostic.suggestedAction;
    packet["classifier"] = std::move(classifier);

    nlohmann::json evidence = nlohmann::json::array();
    for (const auto& e : diagnostic.evidence) {
        if (e.IsEvent()) {
            evidence.push_back({{"events", e.events}, {"seq", e.seq}});
        } else if (e.IsLog()) {
            evidence.push_back({{"log", e.log}, {"line", e.line}});
        }
    }
    packet["evidence"] = std::move(evidence);

    // Only the tail of the normalized events and of the attempt log is handed over.
    nlohmann::json events = nlohmann::json::array();
    const size_t kMaxEvents = 24;
    size_t start = input.events.size() > kMaxEvents ? input.events.size() - kMaxEvents : 0;
    for (size_t i = start; i < input.events.size(); ++i) {
        if (input.events[i].is_object()) events.push_back(input.events[i]);
    }
    packet["events"] = std::move(events);

    nlohmann::json log = nlohmann::json::object();
    log["file"] = input.logFile;
    log["firstLine"] = input.logFirstLine;
    log["lastLine"] = input.logLastLine;
    log["text"] = input.logText;
    packet["log"] = std::move(log);

    return packet.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

bool ApplyTaskQueueFailureLlmGuess(TaskQueueFailureDiagnostic& diagnostic,
                                   const std::string& answerJson, std::string* outError) {
    if (!TaskQueueFailureNeedsLlmGuess(diagnostic)) {
        if (outError) *outError = "the deterministic classifier already decided";
        return false;
    }
    nlohmann::json answer = nlohmann::json::parse(TrimJsonStr(answerJson), nullptr, false);
    if (answer.is_string()) {
        answer = nlohmann::json::parse(TrimJsonStr(answer.get<std::string>()), nullptr, false);
    }
    if (answer.is_object()) {
        auto nested = answer.find("answer");
        if (nested != answer.end()) {
            if (nested->is_object()) {
                answer = *nested;
            } else if (nested->is_string()) {
                nlohmann::json parsed = nlohmann::json::parse(TrimJsonStr(nested->get<std::string>()),
                                                             nullptr, false);
                if (parsed.is_object()) answer = std::move(parsed);
            }
        }
    }
    if (!answer.is_object()) {
        if (outError) *outError = "the answer is not a JSON object";
        return false;
    }

    auto readString = [&answer](const char* key) {
        auto it = answer.find(key);
        return it != answer.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    std::string failureClass = readString("class");
    if (failureClass.empty()) failureClass = readString("failureClass");
    if (failureClass.empty()) {
        if (outError) *outError = "the answer has no class";
        return false;
    }
    std::string summary = readString("summary");
    if (summary.empty()) summary = readString("reason");
    std::string action = readString("action");
    if (action.empty()) action = readString("suggestedAction");
    std::string rationale = readString("rationale");
    if (rationale.empty()) rationale = readString("reason");

    TaskQueueFailureLlmGuess guess;
    guess.failureClass = failureClass;
    guess.confidence = "low";
    guess.summary = summary;
    guess.suggestedAction = action;
    guess.rationale = rationale;
    guess.source = "escalate";
    diagnostic.llmGuess = std::move(guess);
    if (outError) outError->clear();
    return true;
}

static TaskQueueFailureDiagnostic ParseFailureDiagnostic(const std::string& obj) {
    TaskQueueFailureDiagnostic d;
    if (TrimJsonStr(obj).empty()) return d;
    nlohmann::json o = nlohmann::json::parse(obj, nullptr, false);
    if (o.is_discarded() || !o.is_object()) return d;
    auto readString = [&o](const char* key) {
        auto it = o.find(key);
        return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
    };
    d.failureClass = readString("class");
    d.confidence = readString("confidence");
    d.summary = readString("summary");
    auto detail = o.find("detail");
    if (detail != o.end() && detail->is_object()) d.detail = *detail;
    auto evidence = o.find("evidence");
    if (evidence != o.end() && evidence->is_array()) {
        for (const auto& item : *evidence) {
            if (!item.is_object()) continue;
            TaskQueueFailureEvidence e;
            e.events = item.value("events", std::string());
            e.seq = JsonUInt64(item, "seq");
            e.log = item.value("log", std::string());
            e.line = static_cast<int>(JsonInt64(item, "line"));
            d.evidence.push_back(e);
        }
    }
    auto activity = o.find("lastActivity");
    if (activity != o.end() && activity->is_object()) {
        d.lastActivity.activity = activity->value("activity", std::string());
        d.lastActivity.target = activity->value("target", std::string());
        d.lastActivity.sinceSec = JsonInt64(*activity, "sinceSec", -1);
    }
    d.suggestedAction = readString("suggestedAction");
    d.retryAt = readString("retryAt");
    auto guess = o.find("llmGuess");
    if (guess != o.end() && guess->is_object()) {
        auto readGuess = [&guess](const char* key) {
            auto it = guess->find(key);
            return it != guess->end() && it->is_string() ? it->get<std::string>() : std::string();
        };
        d.llmGuess.failureClass = readGuess("class");
        d.llmGuess.confidence = "low";
        d.llmGuess.summary = readGuess("summary");
        d.llmGuess.suggestedAction = readGuess("suggestedAction");
        d.llmGuess.rationale = readGuess("rationale");
        d.llmGuess.source = readGuess("source");
    }
    return d;
}

static std::string AttemptToJson(const TaskQueueAttempt& a) {
    std::ostringstream oss;
    oss << "{\"n\":" << a.n
        << ",\"process_id\":" << a.processId;
    if (!a.remoteHost.empty()) {
        oss << ",\"remote_host\":\"" << JsonEscape(a.remoteHost) << "\""
            << ",\"remote_process_id\":" << a.remoteProcessId;
        if (!a.remoteJobId.empty()) {
            oss << ",\"remote_job_id\":\"" << JsonEscape(a.remoteJobId) << "\"";
        }
        if (a.remoteAttempt > 0) {
            oss << ",\"remote_attempt\":" << a.remoteAttempt;
        }
    }
    oss << ",\"start\":\"" << JsonEscape(a.start) << "\""
        << ",\"end\":\"" << JsonEscape(a.end) << "\""
        << ",\"reason\":\"" << JsonEscape(a.reason) << "\""
        << ",\"exit_code\":" << a.exitCode
        << ",\"instructions_rev\":" << a.instructionsRev;
    if (!a.logFile.empty()) {
        oss << ",\"log_file\":\"" << JsonEscape(a.logFile) << "\"";
    }
    oss << ",\"last_lines_file\":\"" << JsonEscape(a.lastLinesFile) << "\""
        << ",\"check_detail\":\"" << JsonEscape(a.checkDetail) << "\"";
    if (!a.memoryRefs.empty()) oss << ",\"memory_refs\":" << StrArrayToJson(a.memoryRefs);
    if (!a.failure.Empty()) oss << ",\"failure\":" << TaskQueueFailureDiagnosticToJson(a.failure);
    oss << "}";
    return oss.str();
}

static std::string EnvToJson(const std::map<std::string, std::string>& env) {
    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (const auto& kv : env) {
        if (!first) oss << ",";
        first = false;
        oss << "\"" << JsonEscape(kv.first) << "\":\"" << JsonEscape(kv.second) << "\"";
    }
    oss << "}";
    return oss.str();
}

static std::string DepsToJson(const std::vector<TaskQueueDependency>& deps) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < deps.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "{\"id\":\"" << JsonEscape(deps[i].id) << "\",\"on\":\"" << JsonEscape(deps[i].on) << "\"}";
    }
    oss << "]";
    return oss.str();
}

static std::string StrArrayToJson(const std::vector<std::string>& v) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << JsonEscape(v[i]) << "\"";
    }
    oss << "]";
    return oss.str();
}

static std::string AttemptsToJson(const std::vector<TaskQueueAttempt>& attempts) {
    std::ostringstream oss;
    oss << "[";
    for (size_t i = 0; i < attempts.size(); ++i) {
        if (i > 0) oss << ",";
        oss << AttemptToJson(attempts[i]);
    }
    oss << "]";
    return oss.str();
}

static std::string InstructionsToJson(const TaskQueueInstructions& in) {
    std::ostringstream oss;
    oss << "{\"source\":\"" << JsonEscape(in.source) << "\""
        << ",\"rev\":" << in.rev
        << ",\"sha256\":\"" << JsonEscape(in.sha256) << "\""
        << ",\"text\":\"" << JsonEscape(in.text) << "\""
        << ",\"captured_at\":\"" << JsonEscape(in.capturedAt) << "\"}";
    return oss.str();
}

static std::string SuccessToJson(const TaskQueueSuccess& s) {
    std::ostringstream oss;
    oss << "{\"exit_code\":" << s.exitCode
        << ",\"files_newer\":" << StrArrayToJson(s.filesNewer)
        << ",\"output_regex\":";
    if (s.outputRegex.empty()) oss << "null";
    else oss << "\"" << JsonEscape(s.outputRegex) << "\"";
    if (!s.testReport.empty()) oss << ",\"test_report\":\"" << JsonEscape(s.testReport) << "\"";
    oss << "}";
    return oss.str();
}

static std::string ResourcesToJson(const TaskQueueResources& r) {
    return "{\"write\":" + StrArrayToJson(r.write) + ",\"read\":" + StrArrayToJson(r.read) + "}";
}

static std::string MemoryToJson(const TaskQueueMemory& m) {
    std::ostringstream oss;
    oss << "{\"inject\":" << (m.inject ? "true" : "false")
        << ",\"budget_chars\":" << m.budgetChars
        << ",\"scopes\":" << StrArrayToJson(m.scopes)
        << ",\"include_agent_notes\":" << (m.includeAgentNotes ? "true" : "false") << "}";
    return oss.str();
}

static std::string DeferToJson(const TaskQueueDefer& d) {
    std::ostringstream oss;
    oss << "{\"exit_codes\":[";
    for (size_t i = 0; i < d.exitCodes.size(); ++i) {
        if (i > 0) oss << ",";
        oss << d.exitCodes[i];
    }
    oss << "],\"delay_sec\":" << d.delaySec << ",\"limit\":" << d.limit << "}";
    return oss.str();
}

static std::string RetryToJson(const TaskQueueRetry& r) {
    return "{\"on\":" + StrArrayToJson(r.on) + ",\"never\":" + StrArrayToJson(r.never) +
           ",\"wait_for_rate_limit\":" + (r.waitForRateLimit ? "true" : "false") + "}";
}

// "defer" as given by a caller (add/update) or as persisted; missing fields keep the defaults.
// Returns false with a message on a malformed value.
static bool ParseDeferChecked(const std::string& json, int successExitCode, TaskQueueDefer& out, std::string& err) {
    out = TaskQueueDefer();
    if (!HasJsonKey(json, "defer")) return true;
    std::string obj = JsonFieldObj(json, "defer", "");
    if (obj.empty()) {
        err = "defer must be an object like {\"exitCodes\":[75],\"delaySec\":60,\"limit\":60}";
        return false;
    }
    if (HasJsonKey(obj, "exit_codes")) {
        out.exitCodes.clear();
        for (const auto& s : JsonFieldStrArray(obj, "exit_codes")) {
            int code = 0;
            try {
                size_t used = 0;
                code = std::stoi(s, &used);
                if (used != s.size()) throw std::invalid_argument(s);
            } catch (const std::exception&) {
                err = "defer.exitCodes must be integers";
                return false;
            }
            if (code == successExitCode) {
                err = "defer.exitCodes must not contain the success exit code (" + std::to_string(code) + ")";
                return false;
            }
            out.exitCodes.push_back(code);
        }
    }
    out.delaySec = static_cast<int>(JsonFieldInt(obj, "delay_sec", out.delaySec));
    out.limit = static_cast<int>(JsonFieldInt(obj, "limit", out.limit));
    if (out.delaySec < 0 || out.limit < 0) {
        err = "defer.delaySec and defer.limit must not be negative";
        return false;
    }
    return true;
}

static bool RetryClassKnown(const std::string& c) {
    static const char* kClasses[] = {
        "launch_error", "timeout", "idle_stall", "exit_nonzero", "crash",
        "success_check_failed", "rate_limited", "auth_error", "permission_denied",
        "turn_or_context_limit", "tool_error_loop", "incomplete", "killed",
        "dependency_blocked", "resource_conflict", "unknown",
    };
    for (const char* known : kClasses) {
        if (LowerAscii(c) == LowerAscii(known)) return true;
    }
    // Classes added by <data-dir>/failure-rules.json are valid retry targets too.
    for (const auto& rule : LoadTaskQueueFailureRules(TaskQueueFailureRulesPath())) {
        if (LowerAscii(rule.failureClass) == LowerAscii(c)) return true;
    }
    return false;
}

// "retry" as given by a caller (add/update) or as persisted. Returns false on a malformed
// value. A missing "retry" keeps configured false, i.e. the legacy retry-everything policy.
static bool ParseRetryChecked(const std::string& json, TaskQueueRetry& out, std::string& err) {
    out = TaskQueueRetry();
    if (!HasJsonKey(json, "retry")) return true;
    std::string obj = JsonFieldObj(json, "retry", "");
    if (obj.empty()) {
        err = "retry must be an object like {\"on\":[...],\"never\":[...],\"waitForRateLimit\":true}";
        return false;
    }
    out.configured = true;
    out.on = JsonFieldStrArray(obj, "on");
    out.never = JsonFieldStrArray(obj, "never");
    out.waitForRateLimit = JsonFieldBool(obj, "wait_for_rate_limit", true);
    for (const auto* list : { &out.on, &out.never }) {
        for (const auto& c : *list) {
            if (!RetryClassKnown(c)) {
                err = "retry.on/never contains an unknown failure class: " + c;
                return false;
            }
        }
    }
    return true;
}

static TaskQueueResources ParseResources(const std::string& obj) {
    TaskQueueResources r;
    r.write = JsonFieldStrArray(obj, "write");
    r.read = JsonFieldStrArray(obj, "read");
    return r;
}

static TaskQueueMemory ParseMemory(const std::string& obj) {
    TaskQueueMemory m;
    if (TrimJsonStr(obj).empty()) return m;
    m.inject = JsonFieldBool(obj, "inject", false);
    m.budgetChars = static_cast<int>(JsonFieldInt(obj, "budget_chars", 1500));
    if (m.budgetChars <= 0) m.budgetChars = 1500;
    m.scopes = JsonFieldStrArray(obj, "scopes");
    m.includeAgentNotes = JsonFieldBool(obj, "include_agent_notes", true);
    return m;
}

// "resources" as given by a caller (add/update): an object with optional string arrays
// "write" and "read". Returns false with a message on a malformed value.
static bool ParseResourcesChecked(const std::string& json, TaskQueueResources& out, std::string& err) {
    out = TaskQueueResources();
    if (!HasJsonKey(json, "resources")) return true;
    std::string obj = JsonFieldObj(json, "resources", "");
    if (obj.empty()) {
        err = "resources must be an object like {\"write\":[...],\"read\":[...]}";
        return false;
    }
    out = ParseResources(obj);
    for (const auto* list : { &out.write, &out.read }) {
        for (const auto& r : *list) {
            if (r == "@") {
                err = "resources: '@' needs a name (e.g. @build)";
                return false;
            }
        }
    }
    return true;
}

static TaskQueueAttempt ParseAttempt(const std::string& j) {
    TaskQueueAttempt a;
    a.n = static_cast<int>(JsonFieldInt(j, "n", 1));
    a.processId = static_cast<int>(JsonFieldInt(j, "process_id", 0));
    a.remoteHost = JsonFieldStr(j, "remote_host", "");
    a.remoteProcessId = static_cast<int>(JsonFieldInt(j, "remote_process_id", 0));
    a.remoteJobId = JsonFieldStr(j, "remote_job_id", "");
    a.remoteAttempt = static_cast<int>(JsonFieldInt(j, "remote_attempt", 0));
    if (!a.remoteHost.empty()) {
        if (a.remoteJobId.empty()) a.remoteJobId = JsonFieldStr(j, "job_id", "");
        if (a.remoteAttempt <= 0) a.remoteAttempt = static_cast<int>(JsonFieldInt(j, "attempt", 0));
    }
    a.start = JsonFieldStr(j, "start", "");
    a.end = JsonFieldStr(j, "end", "");
    a.reason = JsonFieldStr(j, "reason", "");
    a.exitCode = static_cast<int>(JsonFieldInt(j, "exit_code", -1));
    a.instructionsRev = static_cast<int>(JsonFieldInt(j, "instructions_rev", 1));
    a.logFile = JsonFieldStr(j, "log_file", "");
    a.lastLinesFile = JsonFieldStr(j, "last_lines_file", "");
    a.checkDetail = JsonFieldStr(j, "check_detail", "");
    a.memoryRefs = JsonFieldStrArray(j, "memory_refs");
    std::string failureObj = JsonFieldObj(j, "failure", "");
    if (!failureObj.empty()) a.failure = ParseFailureDiagnostic(failureObj);
    return a;
}

static TaskQueueInstructions ParseInstructions(const std::string& j) {
    TaskQueueInstructions in;
    in.source = JsonFieldStr(j, "source", "");
    in.rev = static_cast<int>(JsonFieldInt(j, "rev", 1));
    in.sha256 = JsonFieldStr(j, "sha256", "");
    in.text = JsonFieldStr(j, "text", "");
    in.capturedAt = JsonFieldStr(j, "captured_at", "");
    return in;
}

static std::map<std::string, std::string> ParseJsonStringMap(const std::string& objJson) {
    std::map<std::string, std::string> out;
    size_t p = objJson.find('{');
    if (p == std::string::npos) return out;
    size_t i = p + 1;
    while (i < objJson.size()) {
        while (i < objJson.size() && (std::isspace(static_cast<unsigned char>(objJson[i])) || objJson[i] == ',')) i++;
        if (i >= objJson.size() || objJson[i] == '}') break;
        if (objJson[i] != '"') { i++; continue; }
        std::string key;
        i++;
        while (i < objJson.size() && objJson[i] != '"') {
            if (objJson[i] == '\\' && i + 1 < objJson.size()) { i++; }
            key.push_back(objJson[i]);
            i++;
        }
        i++;
        while (i < objJson.size() && objJson[i] != ':') i++;
        if (i < objJson.size()) i++;
        while (i < objJson.size() && std::isspace(static_cast<unsigned char>(objJson[i]))) i++;
        if (i >= objJson.size()) break;
        std::string val;
        if (objJson[i] == '"') {
            i++;
            while (i < objJson.size() && objJson[i] != '"') {
                if (objJson[i] == '\\' && i + 1 < objJson.size()) {
                    char e = objJson[i + 1];
                    switch (e) {
                        case 'n': val.push_back('\n'); break;
                        case 'r': val.push_back('\r'); break;
                        case 't': val.push_back('\t'); break;
                        case '"': val.push_back('"'); break;
                        case '\\': val.push_back('\\'); break;
                        default: val.push_back(e); break;
                    }
                    i += 2;
                } else {
                    val.push_back(objJson[i]);
                    i++;
                }
            }
            i++;
        } else {
            size_t e = i;
            while (e < objJson.size() && objJson[e] != ',' && objJson[e] != '}') e++;
            val = TrimJsonStr(objJson.substr(i, e - i));
            i = e;
        }
        out[key] = val;
    }
    return out;
}

static TaskQueueJob ParseJob(const std::string& j) {
    TaskQueueJob job;
    job.id = JsonFieldStr(j, "id", "");
    job.taskqueue = JsonFieldStr(j, "taskqueue", "");
    job.name = JsonFieldStr(j, "name", "");
    job.summary = JsonFieldStr(j, "summary", "");
    job.instructions = ParseInstructions(JsonFieldObj(j, "instructions"));
    job.command = JsonFieldStr(j, "command", "");
    job.cwd = JsonFieldStr(j, "cwd", "");
    job.host = JsonFieldStr(j, "host", "");
    job.env = ParseJsonStringMap(JsonFieldObj(j, "env"));
    for (const auto& depJson : JsonFieldObjArray(j, "depends_on")) {
        TaskQueueDependency dep;
        dep.id = JsonFieldStr(depJson, "id", "");
        dep.on = JsonFieldStr(depJson, "on", "success");
        if (!dep.id.empty()) job.dependsOn.push_back(dep);
    }
    job.timeoutSec = static_cast<int>(JsonFieldInt(j, "timeout_sec", 0));
    job.estimateSec = static_cast<int>(JsonFieldInt(j, "estimate_sec", 0));
    std::string idleObj = JsonFieldObj(j, "idle");
    job.idle.outputSec = static_cast<int>(JsonFieldInt(idleObj, "output_sec", 0));
    job.idle.cpuSec = static_cast<int>(JsonFieldInt(idleObj, "cpu_sec", 0));
    job.maxAttempts = static_cast<int>(JsonFieldInt(j, "max_attempts", 2));
    job.retryDelaySec = static_cast<int>(JsonFieldInt(j, "retry_delay_sec", 0));
    std::string retryErr;
    if (!ParseRetryChecked(j, job.retry, retryErr)) job.retry = TaskQueueRetry();
    std::string successObj = JsonFieldObj(j, "success");
    job.success.exitCode = static_cast<int>(JsonFieldInt(successObj, "exit_code", 0));
    job.success.filesNewer = JsonFieldStrArray(successObj, "files_newer");
    job.success.outputRegex = JsonFieldStr(successObj, "output_regex", "");
    job.success.testReport = JsonFieldStr(successObj, "test_report", "");
    std::string deferErr;
    if (!ParseDeferChecked(j, job.success.exitCode, job.defer, deferErr)) job.defer = TaskQueueDefer();
    job.killExclude = JsonFieldStrArray(j, "kill_exclude");
    job.resources = ParseResources(JsonFieldObj(j, "resources"));
    job.memory = ParseMemory(JsonFieldObj(j, "memory", ""));
    job.priority = static_cast<int>(JsonFieldInt(j, "priority", 0));
    job.state = JsonFieldStr(j, "state", "pending");
    for (const auto& attJson : JsonFieldObjArray(j, "attempts")) {
        job.attempts.push_back(ParseAttempt(attJson));
    }
    job.blockedBy = JsonFieldStr(j, "blocked_by", "");
    job.createdAt = JsonFieldStr(j, "created_at", "");
    job.updatedAt = JsonFieldStr(j, "updated_at", "");
    job.notes = JsonFieldStr(j, "notes", "");
    job.retryAtMs = JsonFieldInt(j, "retry_at_ms", 0);
    if (HasJsonKey(j, "bt")) {
        std::string btObj = JsonFieldObj(j, "bt", "");
        if (!btObj.empty()) {
            job.bt.id = JsonFieldStr(btObj, "id", "");
            job.bt.node = JsonFieldStr(btObj, "node", "");
            job.bt.run = static_cast<int>(JsonFieldInt(btObj, "run", 0));
        }
    }
    if (HasJsonKey(j, "origin")) {
        job.origin = TaskQueueManager::ParseOriginJson(JsonFieldObj(j, "origin", ""));
    }
    return job;
}

static TaskQueue ParseQueue(const std::string& content) {
    TaskQueue tq;
    tq.name = JsonFieldStr(content, "name", "");
    tq.concurrency = static_cast<int>(JsonFieldInt(content, "concurrency", 1));
    tq.paused = JsonFieldBool(content, "paused", false);
    tq.version = static_cast<int>(JsonFieldInt(content, "version", 0));
    tq.notifyFile = JsonFieldStr(content, "notify_file", "");
    tq.maxTimeoutSec = static_cast<int>(JsonFieldInt(content, "max_timeout_sec", 7200));
    tq.defaultMaxAttempts = static_cast<int>(JsonFieldInt(content, "default_max_attempts", 2));
    for (const auto& jobJson : JsonFieldObjArray(content, "jobs")) {
        TaskQueueJob job = ParseJob(jobJson);
        if (!job.id.empty()) {
            if (job.taskqueue.empty()) job.taskqueue = tq.name;
            if (job.name.empty()) job.name = job.id;
            for (auto& a : job.attempts) {
                if (!a.remoteHost.empty()) {
                    if (a.remoteJobId.empty()) a.remoteJobId = job.id;
                    if (a.remoteAttempt <= 0) a.remoteAttempt = a.n;
                }
            }
            tq.jobs.push_back(job);
        }
    }
    return tq;
}

} // namespace pc

namespace pc {

static bool IsRemoteHostInHostsJson(const std::string& hostName) {
    if (hostName.empty()) return false;
    std::wstring path = (std::filesystem::path(GetEffectiveDataDirectoryWide()) / L"hosts.json").wstring();
    std::string text = ReadFileUtf8Wide(path);
    if (Trim(text).empty()) return false;
    nlohmann::json doc = nlohmann::json::parse(text, nullptr, false);
    if (!doc.is_object() && !doc.is_array()) return false;
    const nlohmann::json* list = nullptr;
    if (doc.is_array()) list = &doc;
    else if (doc.contains("hosts") && doc["hosts"].is_array()) list = &doc["hosts"];
    if (!list) return false;
    for (const auto& item : *list) {
        if (!item.is_object()) continue;
        if (item.value("name", "") == hostName) {
            return item.value("enabled", true);
        }
    }
    return false;
}

// ---------------------------------------------------------------------------
// TaskQueueManager: construction / persistence / notifications
// ---------------------------------------------------------------------------

std::string TaskQueueManager::BuildError(const std::string& msg) {
    return BuildErrorJson(msg);
}

TaskQueueManager::TaskQueueManager() {
    LoadQueues();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [name, tq] : queues_) {
            bool changed = false;
            for (auto& job : tq.jobs) {
                if (job.state != "running") continue;
                if (!job.attempts.empty()) {
                    TaskQueueAttempt& a = job.attempts.back();
                    bool isRemote = !a.remoteHost.empty() && a.remoteProcessId > 0;
                    if (isRemote && IsRemoteHostInHostsJson(a.remoteHost)) {
                        a.needsRemoteReconnect = true;
                        if (a.remoteJobId.empty()) a.remoteJobId = job.id;
                        if (a.remoteAttempt <= 0) a.remoteAttempt = a.n;
                        continue;
                    }
                    TaskQueueFailureInput fin;
                    FillFailureInputBase(fin, job, a);
                    fin.interrupted = true;
                    fin.endKind = "killed";
                    fin.exitCode = -1;
                    fin.checkDetail = isRemote ? ("Remote host '" + a.remoteHost + "' not found in hosts.json")
                                               : "Hancho restarted";
                    std::wstring logPath = GetAttemptLogPathWide(job.id, a.n);
                    fin.logFile = WideToUtf8(logPath);
                    FillFailureLog(fin, ReadFileUtf8Wide(logPath));
                    fin.fallbackEventsFile = WideToUtf8(GetTaskEventsFileWide(tq.name));
                    fin.fallbackEventSeq = static_cast<uint64_t>(tq.version + 1);
                    ApplyAttemptOutcome(tq, job, false, "interrupted", -1, fin.checkDetail, &fin);
                    AppendRunRecordForAttempt(tq, job, a, fin,
                                              ReadFileUtf8Wide(logPath));
                } else {
                    job.state = "pending";
                    job.updatedAt = GetCurrentTimestampStr();
                    tq.version++;
                }
                changed = true;
            }
            if (changed) {
                ReevaluateAll();
                SaveQueue(tq);
            }
        }
    }
    schedulerRunning_.store(true);
    schedulerThread_ = std::thread(&TaskQueueManager::SchedulerLoop, this);
}

TaskQueueManager::~TaskQueueManager() {
    ShutdownTaskQueueManager();
}

void TaskQueueManager::ShutdownTaskQueueManager() {
    if (!schedulerRunning_.exchange(false)) return;
    cv_.notify_all();
    if (schedulerThread_.joinable()) {
        schedulerThread_.join();
    }
}

void TaskQueueManager::SetProcessManager(ProcessManager* pm) {
    std::lock_guard<std::mutex> lock(mutex_);
    pm_ = pm;
}

void TaskQueueManager::SetProcessFinishedCallback(ProcessFinishedCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    finishedCb_ = std::move(cb);
}

void TaskQueueManager::SetJobFinishedCallback(JobFinishedCallback cb) {
    std::unique_lock<std::mutex> lock(mutex_);
    const bool clearing = !cb;
    jobFinishedCb_ = std::move(cb);
    // FlushFinishedCallbacks calls a copy outside mutex_, and the owner (BtManager) clears
    // the callback right before it is destroyed: wait for calls already running. The caller
    // must not hold a lock that the callback takes, and must not be inside the callback.
    if (clearing) {
        jobFinishedCbIdleCv_.wait(lock, [this] { return jobFinishedCbInFlight_ == 0; });
    }
}

void TaskQueueManager::SetBtRetryCallback(BtRetryCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    btRetryCb_ = std::move(cb);
}

void TaskQueueManager::SetRemoteCallbacks(RemoteStartCallback start, RemotePollCallback poll,
                                          RemoteKillCallback kill) {
    RemotePollCallback pollCopy;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        remoteStartCb_ = std::move(start);
        remotePollCb_ = std::move(poll);
        remoteKillCb_ = std::move(kill);
        pollCopy = remotePollCb_;
    }
    if (pollCopy) {
        ReconnectRemoteJobs();
    }
}

void TaskQueueManager::FlushFinishedCallbacks() {
    std::vector<std::pair<TaskQueueJob, std::string>> items;
    JobFinishedCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (pendingFinished_.empty() || !jobFinishedCb_) {
            pendingFinished_.clear();
            return;
        }
        items.swap(pendingFinished_);
        cb = jobFinishedCb_;
        jobFinishedCbInFlight_++;
    }
    struct InFlightGuard {
        TaskQueueManager& self;
        ~InFlightGuard() {
            std::lock_guard<std::mutex> lock(self.mutex_);
            if (--self.jobFinishedCbInFlight_ == 0) self.jobFinishedCbIdleCv_.notify_all();
        }
    } guard{*this};
    for (const auto& [snap, state] : items) {
        cb(snap, state);
    }
}

void TaskQueueManager::RequestSchedulerTick() {
    cv_.notify_all();
}

int64_t TaskQueueManager::GetTaskQueueVersion(const std::string& taskqueue) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
    auto it = queues_.find(taskqueue);
    return (it == queues_.end()) ? -1 : it->second.version;
}

TaskQueueCounts TaskQueueManager::GetJobCounts() const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
    TaskQueueCounts counts;
    counts.minRemainingSec = -1;
    counts.sumRemainingSec = 0;
    bool anyRemain = false;

    // True when a job can never run: it is blocked, or a dependency has finished in a way that
    // will never satisfy it (e.g. the upstream job failed). Pending upstream jobs are checked recursively.
    std::function<bool(const TaskQueueJob&, int)> isNeverRunnable =
        [this, &isNeverRunnable](const TaskQueueJob& j, int depth) -> bool {
        if (j.state == "blocked") return true;
        if (j.state != "pending" || depth > 64) return false;
        for (const auto& d : j.dependsOn) {
            const TaskQueueJob* dep = FindJobConst(d.id);
            if (!dep) return true;
            if (IsDepSatisfied(*dep, d.on)) continue;
            if (dep->state == "running" || dep->state == "pending") {
                if (isNeverRunnable(*dep, depth + 1)) return true;
                continue;
            }
            return true;  // finished or blocked upstream that does not satisfy this dependency
        }
        return false;
    };

    for (const auto& [name, tq] : queues_) {
        (void)name;
        for (const auto& job : tq.jobs) {
            counts.total++;
            if (job.state == "running") {
                counts.running++;
                const int64_t budget = job.estimateSec > 0 ? job.estimateSec : job.timeoutSec;
                if (budget > 0 && !job.attempts.empty()) {
                    const auto& lastAtt = job.attempts.back();
                    if (!lastAtt.start.empty()) {
                        int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
                        if (sscanf_s(lastAtt.start.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) == 6) {
                            struct tm t = {};
                            t.tm_year = y - 1900;
                            t.tm_mon = mo - 1;
                            t.tm_mday = d;
                            t.tm_hour = h;
                            t.tm_min = mi;
                            t.tm_sec = sec;
                            t.tm_isdst = -1;
                            time_t startEpoch = mktime(&t);
                            if (startEpoch != static_cast<time_t>(-1)) {
                                time_t nowEpoch = time(nullptr);
                                int64_t elapsed = static_cast<int64_t>(nowEpoch - startEpoch);
                                int64_t rem = budget - elapsed;
                                if (rem < 0) rem = 0;
                                if (counts.minRemainingSec < 0 || rem < counts.minRemainingSec) {
                                    counts.minRemainingSec = rem;
                                }
                                counts.sumRemainingSec += rem;
                                anyRemain = true;
                            }
                        }
                    }
                } else if (budget > 0) {
                    int64_t rem = budget;
                    if (counts.minRemainingSec < 0 || rem < counts.minRemainingSec) {
                        counts.minRemainingSec = rem;
                    }
                    counts.sumRemainingSec += rem;
                    anyRemain = true;
                }
            } else if (job.state == "pending") {
                bool allSat = true;
                for (const auto& d : job.dependsOn) {
                    const TaskQueueJob* dep = FindJobConst(d.id);
                    if (!dep) {
                        for (const auto& tj : tq.jobs) {
                            if (tj.id == d.id) { dep = &tj; break; }
                        }
                    }
                    if (!dep || !IsDepSatisfied(*dep, d.on)) {
                        allSat = false;
                        break;
                    }
                }
                if (allSat) {
                    counts.waiting++;
                } else {
                    counts.pending++;
                }
                // Only jobs that can still run count toward the remaining time.
                const int64_t budget = job.estimateSec > 0 ? job.estimateSec : job.timeoutSec;
                if (budget > 0 && !isNeverRunnable(job, 0)) {
                    counts.sumRemainingSec += budget;
                    anyRemain = true;
                }
            } else if (job.state == "blocked") {
                // Blocked by a failed upstream job: it will not run, so it adds no remaining time.
                counts.blocked++;
            } else if (job.state == "succeeded") {
                counts.succeeded++;
            } else if (job.state == "failed") {
                counts.failed++;
            } else if (job.state == "cancelled") {
                counts.cancelled++;
            }
        }
    }
    if (!anyRemain) counts.sumRemainingSec = -1;
    return counts;
}

bool TaskQueueManager::FindUnfinishedJobByCommand(const std::string& command, std::string& outJobId) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
    for (const auto& [name, tq] : queues_) {
        if (name == "plain") continue;
        for (const auto& job : tq.jobs) {
            if (job.state == "succeeded" || job.state == "cancelled") continue;
            if (job.command == command) {
                outJobId = job.id;
                return true;
            }
        }
    }
    return false;
}

void TaskQueueManager::EmitTaskQueueEvent(const std::string& taskqueue, const std::string& type, const std::string& jobId, int64_t attempt) {
    std::lock_guard<std::mutex> lock(mutex_);
    TaskQueue& tq = queues_[taskqueue];
    EmitEventLocked(tq, type, jobId, attempt);
    SaveQueue(tq);
}

void TaskQueueManager::EmitEventLocked(TaskQueue& tq, const std::string& type, const std::string& jobId,
                                       int64_t attempt, const TaskQueueFailureDiagnostic* failure) {
    tq.version++;
    std::ostringstream oss;
    oss << "{\"version\":" << tq.version
        << ",\"seq\":" << tq.version
        << ",\"type\":\"" << JsonEscape(type) << "\""
        << ",\"taskqueue\":\"" << JsonEscape(tq.name) << "\""
        << ",\"job_id\":\"" << JsonEscape(jobId) << "\""
        << ",\"attempt\":" << attempt
        << ",\"timestamp\":\"" << GetCurrentTimestampStr() << "\"";
    if (failure && !failure->Empty()) {
        oss << ",\"failure\":" << TaskQueueFailureDiagnosticToJson(*failure);
    }
    oss << "}";
    std::string line = oss.str() + "\r\n";
    AppendToLogFileWide(GetTaskEventsFileWide(tq.name), line);
    if (!tq.notifyFile.empty() &&
        (type == "taskqueueDrained" || type == "taskqueueStalled" || type == "jobFailed")) {
        WriteFileAtomicWide(Utf8ToWide(tq.notifyFile), oss.str());
    }
}

void TaskQueueManager::LoadQueues() {
    std::wstring dir = GetTaskQueuesDirWide();
    for (const std::wstring& path : ListDirectoryWide(dir, L"*.json")) {
        std::string content = ReadFileUtf8Wide(path);
        if (content.empty()) continue;
        TaskQueue tq = ParseQueue(content);
        if (tq.name.empty()) {
            std::wstring fn = path.substr(path.find_last_of(L"/\\") + 1);
            if (fn.size() > 5) tq.name = WideToUtf8(fn.substr(0, fn.size() - 5));
        }
        if (tq.name.empty()) continue;
        for (auto& job : tq.jobs) {
            if (job.num <= 0) job.num = nextJobNum_++;
        }
        queues_[tq.name] = tq;
    }
}

void TaskQueueManager::SaveQueue(const TaskQueue& tq) {
    if (tq.name.empty()) return;
    std::ostringstream oss;
    oss << "{\r\n"
        << "  \"name\": \"" << JsonEscape(tq.name) << "\",\r\n"
        << "  \"concurrency\": " << tq.concurrency << ",\r\n"
        << "  \"paused\": " << (tq.paused ? "true" : "false") << ",\r\n"
        << "  \"version\": " << tq.version << ",\r\n"
        << "  \"notify_file\": \"" << JsonEscape(tq.notifyFile) << "\",\r\n"
        << "  \"max_timeout_sec\": " << tq.maxTimeoutSec << ",\r\n"
        << "  \"default_max_attempts\": " << tq.defaultMaxAttempts << ",\r\n"
        << "  \"jobs\": [";
    for (size_t i = 0; i < tq.jobs.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\r\n    " << JobToJson(tq.jobs[i], true);
    }
    oss << "\r\n  ]\r\n}\r\n";
    WriteFileAtomicWide(GetTaskQueueFileWide(tq.name), oss.str());
}

// Timing-divergence thresholds come from the data directory's config.json.
// JobToJson is called often, so the parsed values are cached and the file is re-read
// at most every kTimingConfigRefreshSec seconds. A missing or invalid file yields defaults.
struct TimingThresholds {
    double over = 2.0;
    double under = 0.33;
};

static TimingThresholds LoadTimingThresholds() {
    static constexpr double kDefaultOver = 2.0;
    static constexpr double kDefaultUnder = 0.33;
    static constexpr auto kTimingConfigRefreshInterval = std::chrono::seconds(5);
    static std::mutex cacheMutex;
    static TimingThresholds cached;
    static bool loadedOnce = false;
    static std::chrono::steady_clock::time_point lastLoad;

    std::lock_guard<std::mutex> lock(cacheMutex);
    auto now = std::chrono::steady_clock::now();
    if (loadedOnce && now - lastLoad < kTimingConfigRefreshInterval) return cached;
    loadedOnce = true;
    lastLoad = now;

    TimingThresholds t;
    std::string content = ReadFileUtf8Wide(PathJoinWide(GetEffectiveDataDirectoryWide(), L"config.json"));
    if (!content.empty()) {
        nlohmann::json cfg = nlohmann::json::parse(content, nullptr, false);
        if (cfg.is_object()) {
            if (cfg.contains("audit_timing_divergence_threshold") &&
                cfg["audit_timing_divergence_threshold"].is_number()) {
                double v = cfg["audit_timing_divergence_threshold"].get<double>();
                if (v > 0.0) t.over = v;
            }
            if (cfg.contains("audit_timing_under_threshold") &&
                cfg["audit_timing_under_threshold"].is_number()) {
                double v = cfg["audit_timing_under_threshold"].get<double>();
                if (v > 0.0) t.under = v;
            }
        }
    }
    if (t.under >= t.over) {
        t.over = kDefaultOver;
        t.under = kDefaultUnder;
    }
    cached = t;
    return cached;
}

static std::string TimingFeedbackToJson(const TaskQueueJob& job) {
    if (job.estimateSec <= 0 || job.attempts.empty()) return "";
    // Only a finished job's successful attempt says anything about the estimate.
    // Failed, killed, deferred or interrupted attempts are measurement noise here.
    if (job.state != "succeeded") return "";
    const TaskQueueAttempt* finished = nullptr;
    for (auto it = job.attempts.rbegin(); it != job.attempts.rend(); ++it) {
        if (it->reason != "ok") continue;
        if (it->start.empty() || it->end.empty()) continue;
        finished = &(*it);
        break;
    }
    if (!finished) return "";

    const TimingThresholds thresholds = LoadTimingThresholds();

    int64_t startSec = ParseTimestampStrToEpoch(finished->start);
    int64_t endSec = ParseTimestampStrToEpoch(finished->end);
    int64_t durationSec = (endSec >= startSec) ? (endSec - startSec) : 0;
    double ratio = (job.estimateSec > 0) ? ((double)durationSec / (double)job.estimateSec) : 1.0;

    std::string accuracy = "accurate";
    std::string advice;
    if (ratio >= thresholds.over) {
        accuracy = "diverged_over";
        std::ostringstream adv;
        adv << "Actual duration (" << durationSec << "s) exceeded estimate ("
            << job.estimateSec << "s) by " << std::fixed << std::setprecision(1) << ratio << "x. "
            << "Supervisor should calibrate future estimates upward or partition long tasks.";
        advice = adv.str();
    } else if (ratio <= thresholds.under && durationSec >= 5) {
        // Under 5s is too short to be a meaningful divergence signal.
        accuracy = "diverged_under";
        std::ostringstream adv;
        adv << "Actual duration (" << durationSec << "s) was far below estimate ("
            << job.estimateSec << "s) (" << std::fixed << std::setprecision(1) << (ratio * 100.0) << "% of estimate). "
            << "Supervisor should tighten future estimates to optimize queue throughput.";
        advice = adv.str();
    } else {
        std::ostringstream adv;
        adv << "Actual duration matched estimate within normal tolerance ("
            << std::fixed << std::setprecision(2) << thresholds.under << "x - " << thresholds.over << "x).";
        advice = adv.str();
    }

    std::ostringstream oss;
    oss << "{\"duration_sec\":" << durationSec
        << ",\"estimate_sec\":" << job.estimateSec
        << ",\"ratio\":" << std::fixed << std::setprecision(2) << ratio
        << ",\"accuracy\":\"" << accuracy << "\""
        << ",\"advice\":\"" << JsonEscape(advice) << "\"}";
    return oss.str();
}

std::string TaskQueueManager::JobToJson(const TaskQueueJob& job, bool includeInstructions) const {
    std::ostringstream oss;
    oss << "{\"id\":\"" << JsonEscape(job.id) << "\""
        << ",\"taskqueue\":\"" << JsonEscape(job.taskqueue) << "\""
        << ",\"name\":\"" << JsonEscape(job.name) << "\""
        << ",\"summary\":\"" << JsonEscape(job.summary) << "\"";
    if (includeInstructions) {
        oss << ",\"instructions\":" << InstructionsToJson(job.instructions);
    } else {
        oss << ",\"instructions\":{\"source\":\"" << JsonEscape(job.instructions.source)
            << "\",\"rev\":" << job.instructions.rev << "}";
    }
    oss << ",\"command\":\"" << JsonEscape(job.command) << "\""
        << ",\"cwd\":\"" << JsonEscape(job.cwd) << "\""
        << ",\"host\":\"" << JsonEscape(job.host) << "\""
        << ",\"env\":" << EnvToJson(job.env)
        << ",\"depends_on\":" << DepsToJson(job.dependsOn)
        << ",\"timeout_sec\":" << job.timeoutSec
        << ",\"estimate_sec\":" << job.estimateSec
        << ",\"idle\":{\"output_sec\":" << job.idle.outputSec << ",\"cpu_sec\":" << job.idle.cpuSec << "}"
        << ",\"max_attempts\":" << job.maxAttempts
        << ",\"retry_delay_sec\":" << job.retryDelaySec;
    if (job.retry.configured) oss << ",\"retry\":" << RetryToJson(job.retry);
    oss << ",\"defer\":" << DeferToJson(job.defer)
        << ",\"success\":" << SuccessToJson(job.success)
        << ",\"kill_exclude\":" << StrArrayToJson(job.killExclude)
        << ",\"resources\":" << ResourcesToJson(job.resources)
        << ",\"memory\":" << MemoryToJson(job.memory)
        << ",\"priority\":" << job.priority
        << ",\"state\":\"" << JsonEscape(job.state) << "\""
        << ",\"attempts\":" << AttemptsToJson(job.attempts)
        << ",\"blocked_by\":";
    if (job.blockedBy.empty()) oss << "null";
    else oss << "\"" << JsonEscape(job.blockedBy) << "\"";
    oss << ",\"created_at\":\"" << JsonEscape(job.createdAt) << "\""
        << ",\"updated_at\":\"" << JsonEscape(job.updatedAt) << "\""
        << ",\"notes\":\"" << JsonEscape(job.notes) << "\"";
    if (!job.bt.id.empty()) {
        oss << ",\"bt\":{\"id\":\"" << JsonEscape(job.bt.id)
            << "\",\"node\":\"" << JsonEscape(job.bt.node)
            << "\",\"run\":" << job.bt.run << "}";
    }
    if (!job.origin.empty()) oss << ",\"origin\":" << OriginToJson(job.origin);
    std::string tf = TimingFeedbackToJson(job);
    if (!tf.empty()) oss << ",\"timing_feedback\":" << tf;
    oss << "}";
    return oss.str();
}

TaskQueueOrigin TaskQueueManager::ParseOriginJson(const std::string& json) {
    TaskQueueOrigin o;
    std::string j = TrimJsonStr(json);
    if (j.empty() || j.front() != '{') return o;
    o.label = JsonFieldStr(j, "label", "");
    o.client = JsonFieldStr(j, "client", "");
    o.session = JsonFieldStr(j, "session", "");
    o.hostSession = JsonFieldStr(j, "host_session", "");
    if (o.hostSession.empty()) o.hostSession = JsonFieldStr(j, "hostSession", "");
    o.cwd = JsonFieldStr(j, "cwd", "");
    o.pid = JsonFieldInt(j, "pid", 0);
    return o;
}

std::string TaskQueueManager::OriginToJson(const TaskQueueOrigin& o) {
    std::ostringstream oss;
    oss << "{\"label\":\"" << JsonEscape(o.label) << "\""
        << ",\"client\":\"" << JsonEscape(o.client) << "\""
        << ",\"session\":\"" << JsonEscape(o.session) << "\""
        << ",\"host_session\":\"" << JsonEscape(o.hostSession) << "\""
        << ",\"cwd\":\"" << JsonEscape(o.cwd) << "\""
        << ",\"pid\":" << o.pid << "}";
    return oss.str();
}

bool TaskQueueManager::OriginMatches(const TaskQueueOrigin& o, const std::string& filter) {
    if (filter.empty()) return true;
    if (filter.rfind("pid:", 0) == 0) {
        try {
            return o.pid != 0 && o.pid == std::stoll(filter.substr(4));
        } catch (...) {
            return false;
        }
    }
    auto prefixOf = [&](const std::string& value) {
        return !value.empty() && value.size() >= filter.size() && value.compare(0, filter.size(), filter) == 0;
    };
    if (prefixOf(o.session) || prefixOf(o.hostSession)) return true;
    if (o.label.size() != filter.size()) return false;
    for (size_t i = 0; i < filter.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(o.label[i])) != std::tolower(static_cast<unsigned char>(filter[i]))) return false;
    }
    return true;
}

void TaskQueueManager::SetBtOrigin(const std::string& btId, const std::string& originJson) {
    if (btId.empty()) return;
    TaskQueueOrigin o = ParseOriginJson(originJson);
    std::lock_guard<std::mutex> lock(mutex_);
    if (o.empty()) btOrigins_.erase(btId);
    else btOrigins_[btId] = o;
}

TaskQueueJob* TaskQueueManager::FindJob(const std::string& id, TaskQueue** outQueue) {
    for (auto& [name, tq] : queues_) {
        (void)name;
        for (auto& job : tq.jobs) {
            if (job.id == id) {
                if (outQueue) *outQueue = &tq;
                return &job;
            }
        }
    }
    return nullptr;
}

const TaskQueueJob* TaskQueueManager::FindJobConst(const std::string& id) const {
    for (const auto& [name, tq] : queues_) {
        (void)name;
        for (const auto& job : tq.jobs) {
            if (job.id == id) return &job;
        }
    }
    return nullptr;
}


bool TaskQueueManager::IsDepSatisfied(const TaskQueueJob& dep, const std::string& on) {
    if (on == "done") {
        return dep.state == "succeeded" || dep.state == "failed" || dep.state == "cancelled";
    }
    return dep.state == "succeeded";
}

// ---------------------------------------------------------------------------
// Resource locks (doc/resource-locks.md)
// ---------------------------------------------------------------------------

static void ReplaceAllInPlace(std::string& s, const std::string& from, const std::string& to) {
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

// ---------------------------------------------------------------------------
// Test reports (success.testReport). A small tag scanner, not a full XML parser: it reads
// JUnit <testcase> (failed when it holds <failure> or <error>, skipped with <skipped>) and
// NUnit 2/3 <test-case result="..."> elements.

static std::string XmlAttr(const std::string& tag, const std::string& name) {
    size_t pos = 0;
    while ((pos = tag.find(name, pos)) != std::string::npos) {
        const bool startOk = pos > 0 && std::isspace(static_cast<unsigned char>(tag[pos - 1]));
        size_t eq = pos + name.size();
        while (eq < tag.size() && std::isspace(static_cast<unsigned char>(tag[eq]))) ++eq;
        if (!startOk || eq >= tag.size() || tag[eq] != '=') { pos += name.size(); continue; }
        size_t q = eq + 1;
        while (q < tag.size() && std::isspace(static_cast<unsigned char>(tag[q]))) ++q;
        if (q >= tag.size() || (tag[q] != '"' && tag[q] != '\'')) return "";
        size_t end = tag.find(tag[q], q + 1);
        if (end == std::string::npos) return "";
        std::string v = tag.substr(q + 1, end - q - 1);
        static const std::pair<const char*, const char*> kEnt[] = {
            {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
        for (const auto& e : kEnt) ReplaceAllInPlace(v, e.first, e.second);
        return v;
    }
    return "";
}

// Next element start tag at or after pos whose name is one of `names`. Returns the tag text
// (between '<' and '>') and sets pos past it; "" when none is left.
static std::string NextXmlTag(const std::string& xml, size_t& pos, std::string& outName) {
    while ((pos = xml.find('<', pos)) != std::string::npos) {
        size_t end = xml.find('>', pos);
        if (end == std::string::npos) { pos = std::string::npos; return ""; }
        std::string tag = xml.substr(pos + 1, end - pos - 1);
        pos = end + 1;
        if (tag.empty() || tag[0] == '?' || tag[0] == '!') continue;
        size_t n = 0;
        while (n < tag.size() && !std::isspace(static_cast<unsigned char>(tag[n])) && tag[n] != '/') ++n;
        outName = tag.substr(0, n);
        return tag;
    }
    return "";
}

bool ParseTestReport(const std::string& xml, TestReportSummary& out) {
    out = TestReportSummary();
    const size_t kMaxNames = 5;
    auto addFailed = [&out, kMaxNames](const std::string& name) {
        ++out.failed;
        if (out.failedNames.size() < kMaxNames) out.failedNames.push_back(name.empty() ? "(unnamed)" : name);
    };
    size_t pos = 0;
    std::string name;
    // JUnit case currently open: its display name and whether it already failed or skipped.
    bool inCase = false;
    bool caseFailed = false;
    bool caseSkipped = false;
    std::string caseName;
    auto closeCase = [&]() {
        if (!inCase) return;
        ++out.total;
        if (caseFailed) addFailed(caseName);
        else if (caseSkipped) ++out.skipped;
        inCase = false;
    };
    while (true) {
        std::string tag = NextXmlTag(xml, pos, name);
        if (pos == std::string::npos || (tag.empty() && name.empty())) break;
        const bool selfClosing = !tag.empty() && tag.back() == '/';
        if (name == "testcase") {
            closeCase();
            std::string cls = XmlAttr(tag, "classname");
            std::string nm = XmlAttr(tag, "name");
            caseName = cls.empty() ? nm : cls + "." + nm;
            inCase = true;
            caseFailed = false;
            caseSkipped = false;
            if (selfClosing) closeCase();
        } else if (name == "/testcase") {
            closeCase();
        } else if (inCase && (name == "failure" || name == "error")) {
            caseFailed = true;
        } else if (inCase && name == "skipped") {
            caseSkipped = true;
        } else if (name == "test-case") {
            // NUnit 3: result="Passed|Failed|Skipped|Inconclusive|Warning".
            // NUnit 2: result="Success|Failure|Error|Ignored|..." plus executed="True|False".
            std::string full = XmlAttr(tag, "fullname");
            if (full.empty()) full = XmlAttr(tag, "name");
            std::string result = LowerAscii(XmlAttr(tag, "result"));
            ++out.total;
            if (result == "failed" || result == "failure" || result == "error") addFailed(full);
            else if (result != "passed" && result != "success") ++out.skipped;
        }
    }
    closeCase();
    return out.total > 0;
}

static bool IsAbsoluteResourcePath(const std::string& s) {
    return (s.size() >= 2 && s[1] == ':') || (!s.empty() && s[0] == '/');
}

// '\' -> '/', ASCII lower case, '.' / '..' resolved, no trailing '/'.
static std::string NormalizeResourcePath(std::string s) {
    for (auto& c : s) {
        if (c == '\\') c = '/';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    std::string prefix;
    if (s.compare(0, 2, "//") == 0) prefix = "//";
    else if (!s.empty() && s[0] == '/') prefix = "/";
    std::vector<std::string> segs;
    size_t i = prefix.size();
    while (i <= s.size()) {
        size_t e = s.find('/', i);
        if (e == std::string::npos) e = s.size();
        std::string seg = s.substr(i, e - i);
        if (seg == "..") {
            if (!segs.empty() && segs.back() != ".." && segs.back().find(':') == std::string::npos) segs.pop_back();
            else if (segs.empty()) segs.push_back(seg);
        } else if (!seg.empty() && seg != ".") {
            segs.push_back(seg);
        }
        i = e + 1;
    }
    std::string out = prefix;
    for (size_t k = 0; k < segs.size(); ++k) {
        if (k > 0) out += "/";
        out += segs[k];
    }
    return out;
}

// Git top level above cwd: the nearest ancestor holding a .git directory or file (a linked
// worktree has a .git file). Cached, since the scheduler normalizes resources every tick.
static std::string FindGitRootCached(const std::string& cwd) {
    static std::mutex mu;
    static std::unordered_map<std::string, std::string> cache;
    {
        std::lock_guard<std::mutex> lock(mu);
        auto it = cache.find(cwd);
        if (it != cache.end()) return it->second;
    }
    std::string root = cwd;
    try {
        std::error_code ec;
        std::filesystem::path p{std::u8string(cwd.begin(), cwd.end())};
        for (int depth = 0; depth < 64 && !p.empty(); ++depth) {
            if (std::filesystem::exists(p / ".git", ec)) {
                const std::u8string u = p.u8string();
                root.assign(u.begin(), u.end());
                break;
            }
            std::filesystem::path parent = p.parent_path();
            if (parent == p) break;
            p = parent;
        }
    } catch (const std::exception&) {
        root = cwd;
    }
    std::lock_guard<std::mutex> lock(mu);
    cache[cwd] = root;
    return root;
}

std::string NormalizeResource(const std::string& resource, const std::string& cwd) {
    return NormalizeResource(resource, cwd, "");
}

std::string NormalizeResource(const std::string& resource, const std::string& cwd,
                              const std::string& host) {
    std::string s = TrimJsonStr(resource);
    for (auto& c : s) {
        if (c == '\\') c = '/';
        else c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    if (!s.empty() && s[0] == '@') {
        if (s.find('{') != std::string::npos) {
            const std::string trimmedCwd = TrimJsonStr(cwd);
            const std::string cwdNorm = NormalizeResourcePath(trimmedCwd);
            ReplaceAllInPlace(s, "{cwd}", cwdNorm);
            if (s.find("{root}") != std::string::npos) {
                // A remote cwd is not a local path, so it cannot be walked here.
                std::string root = (host.empty() && !trimmedCwd.empty())
                                       ? NormalizeResourcePath(FindGitRootCached(trimmedCwd))
                                       : cwdNorm;
                ReplaceAllInPlace(s, "{root}", root);
            }
        }
        return s;
    }
    // A bare file name (no '/') can live anywhere below cwd, as tools/ww reads it: Foo.cs -> **/foo.cs.
    if (!s.empty() && s.find('/') == std::string::npos && s != "**" && s != "." && s != ".." &&
        !IsAbsoluteResourcePath(s)) {
        s = "**/" + s;
    }
    if (!IsAbsoluteResourcePath(s) && !cwd.empty()) {
        s = NormalizeResourcePath(TrimJsonStr(cwd)) + "/" + s;
    }
    return NormalizeResourcePath(s);
}

static bool HasWildcard(const std::string& s) {
    return s.find_first_of("*?") != std::string::npos;
}

// One glob segment against a literal segment: '*' is any run of characters, '?' one character.
static bool SegmentGlobMatch(const std::string& p, size_t pi, const std::string& t, size_t ti) {
    while (pi < p.size()) {
        if (p[pi] == '*') {
            for (size_t k = ti; k <= t.size(); ++k) {
                if (SegmentGlobMatch(p, pi + 1, t, k)) return true;
            }
            return false;
        }
        if (ti >= t.size() || (p[pi] != '?' && p[pi] != t[ti])) return false;
        ++pi;
        ++ti;
    }
    return ti == t.size();
}

// Two wildcard segments are assumed to overlap (conservative).
static bool SegmentOverlap(const std::string& a, const std::string& b) {
    bool globA = HasWildcard(a);
    bool globB = HasWildcard(b);
    if (!globA && !globB) return a == b;
    if (globA && globB) return true;
    return globA ? SegmentGlobMatch(a, 0, b, 0) : SegmentGlobMatch(b, 0, a, 0);
}

// "b.cpp" names a file, "src" may be a directory.
static bool SegmentLooksLikeFile(const std::string& s) {
    size_t dot = s.rfind('.');
    if (dot == std::string::npos || dot + 1 >= s.size()) return false;
    for (size_t i = dot + 1; i < s.size(); ++i) {
        if (!std::isalnum(static_cast<unsigned char>(s[i]))) return false;
    }
    return true;
}

// The same rule as Test-SegmentsOverlap in tools/ww/ww-run.lib.ps1, so ww-run and the task
// queue agree. A directory covers everything below it and a "**" segment matches any number of
// segments. When one side is used up it is taken as an ancestor directory of the other, except
// when a "**" has run over part of it and it ends in a file name: **/foo.cs does not overlap
// src/b.cpp (it still overlaps src and src/foo.cs).
static bool SegmentsOverlap(const std::vector<std::string>& a, size_t i,
                            const std::vector<std::string>& b, size_t j, bool swallowed) {
    if (i >= a.size() && j >= b.size()) return true;
    if (i >= a.size() || j >= b.size()) {
        const auto& restSide = i >= a.size() ? b : a;
        size_t restFrom = i >= a.size() ? j : i;
        bool onlyGlobstars = true;
        for (size_t k = restFrom; k < restSide.size(); ++k) {
            if (restSide[k] != "**") { onlyGlobstars = false; break; }
        }
        if (onlyGlobstars) return true;
        const auto& done = i >= a.size() ? a : b;
        if (swallowed && !done.empty() && SegmentLooksLikeFile(done.back())) return false;
        return true;
    }
    if (a[i] == "**") {
        if (i + 1 >= a.size()) return true;
        for (size_t k = j; k <= b.size(); ++k) {
            if (SegmentsOverlap(a, i + 1, b, k, swallowed || k > j)) return true;
        }
        return false;
    }
    if (b[j] == "**") return SegmentsOverlap(b, j, a, i, swallowed);
    if (!SegmentOverlap(a[i], b[j])) return false;
    return SegmentsOverlap(a, i + 1, b, j + 1, swallowed);
}

static std::vector<std::string> SplitResourcePath(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (true) {
        size_t e = s.find('/', i);
        out.push_back(s.substr(i, e == std::string::npos ? std::string::npos : e - i));
        if (e == std::string::npos) break;
        i = e + 1;
    }
    return out;
}

bool ResourcesOverlap(const std::string& a, const std::string& b) {
    if (a.empty() || b.empty()) return false;
    bool namedA = a[0] == '@';
    bool namedB = b[0] == '@';
    if (namedA || namedB) return namedA && namedB && a == b;
    return SegmentsOverlap(SplitResourcePath(a), 0, SplitResourcePath(b), 0, false);
}

bool FindResourceConflict(const TaskQueueResources& a, const std::string& cwdA,
                          const TaskQueueResources& b, const std::string& cwdB,
                          std::string& outResource) {
    return FindResourceConflict(a, cwdA, "", b, cwdB, "", outResource);
}

bool FindResourceConflict(const TaskQueueResources& a, const std::string& cwdA,
                          const std::string& hostA,
                          const TaskQueueResources& b, const std::string& cwdB,
                          const std::string& hostB,
                          std::string& outResource) {
    // The same path means different things on different machines.
    if (hostA != hostB) return false;
    if (a.empty() || b.empty()) return false;
    auto norm = [](const std::vector<std::string>& v, const std::string& cwd, const std::string& host) {
        std::vector<std::string> out;
        for (const auto& r : v) out.push_back(NormalizeResource(r, cwd, host));
        return out;
    };
    std::vector<std::string> aw = norm(a.write, cwdA, hostA), ar = norm(a.read, cwdA, hostA);
    std::vector<std::string> bw = norm(b.write, cwdB, hostB), br = norm(b.read, cwdB, hostB);
    auto check = [&](const std::vector<std::string>& x, const std::vector<std::string>& xRaw,
                     const std::vector<std::string>& y) {
        for (size_t i = 0; i < x.size(); ++i) {
            for (const auto& r : y) {
                if (ResourcesOverlap(x[i], r)) { outResource = xRaw[i]; return true; }
            }
        }
        return false;
    };
    return check(aw, a.write, bw) || check(aw, a.write, br) || check(ar, a.read, bw);
}

void TaskQueueManager::SetResourceLocksEnabled(bool enabled) {
    resourceLocksEnabled_.store(enabled);
    RequestSchedulerTick();
}

bool TaskQueueManager::IsReadyLocked(const TaskQueueJob& job, int64_t nowMs) const {
    if (job.state != "pending" || job.retryAtMs > nowMs) return false;
    for (const auto& d : job.dependsOn) {
        const TaskQueueJob* dep = FindJobConst(d.id);
        if (!dep || !IsDepSatisfied(*dep, d.on)) return false;
    }
    return true;
}

// Global start order: higher priority first, then earlier registration, then queue and id.
bool TaskQueueManager::RunsBefore(const TaskQueueJob& a, const TaskQueueJob& b) {
    if (a.priority != b.priority) return a.priority > b.priority;
    if (a.createdAt != b.createdAt) return a.createdAt < b.createdAt;
    if (a.taskqueue != b.taskqueue) return a.taskqueue < b.taskqueue;
    return a.num < b.num;
}

bool TaskQueueManager::FindQueuedResourceConflictLocked(const TaskQueueJob& job, int64_t nowMs,
                                                        std::string& outHolder, std::string& outResource) const {
    if (!resourceLocksEnabled_.load() || job.resources.empty()) return false;
    for (const auto& [name, tq] : queues_) {
        (void)name;
        if (tq.paused) continue;
        for (const auto& other : tq.jobs) {
            if (other.id == job.id || !RunsBefore(other, job) || !IsReadyLocked(other, nowMs)) continue;
            if (FindResourceConflict(job.resources, job.cwd, job.host,
                                     other.resources, other.cwd, other.host, outResource)) {
                outHolder = other.id;
                return true;
            }
        }
    }
    return false;
}

std::string TaskQueueManager::ResourceWaitLocked(const TaskQueueJob& job, bool allSat, int64_t nowMs) const {
    if (job.state != "pending") return "";
    std::string holder, resource;
    if (FindRunningResourceConflictLocked(job, holder, resource)) return holder + ": " + resource;
    if (allSat && FindQueuedResourceConflictLocked(job, nowMs, holder, resource)) {
        return holder + " (queued ahead): " + resource;
    }
    return "";
}

bool TaskQueueManager::FindRunningResourceConflictLocked(const TaskQueueJob& job, std::string& outHolder,
                                                         std::string& outResource) const {
    if (!resourceLocksEnabled_.load() || job.resources.empty()) return false;
    for (const auto& [name, tq] : queues_) {
        (void)name;
        for (const auto& other : tq.jobs) {
            if (other.state != "running" || other.id == job.id) continue;
            if (FindResourceConflict(job.resources, job.cwd, job.host,
                                     other.resources, other.cwd, other.host, outResource)) {
                outHolder = other.id;
                return true;
            }
        }
    }
    return false;
}

bool TaskQueueManager::HasDependencyCycle(const TaskQueue& tq, const std::string& jobId,
                                          const std::vector<TaskQueueDependency>& deps,
                                          const std::string& ignoreDependency) {
    std::vector<std::string> stack;
    std::vector<std::string> visited;
    for (const auto& d : deps) {
        if (d.id == ignoreDependency) continue;
        stack.push_back(d.id);
    }
    while (!stack.empty()) {
        std::string cur = stack.back();
        stack.pop_back();
        if (cur == jobId) return true;
        if (std::find(visited.begin(), visited.end(), cur) != visited.end()) continue;
        visited.push_back(cur);
        const TaskQueueJob* j = FindJobConst(cur);
        if (!j) {
            for (const auto& tj : tq.jobs) {
                if (tj.id == cur) { j = &tj; break; }
            }
        }
        if (j) {
            for (const auto& d : j->dependsOn) stack.push_back(d.id);
        }
    }
    return false;
}

} // namespace pc

namespace pc {

// ---------------------------------------------------------------------------
// Small self-contained SHA-256 (used for instructions versioning)
// ---------------------------------------------------------------------------

namespace {

struct Sha256Ctx {
    uint32_t h[8];
    uint64_t len = 0;
    uint8_t buf[64];
    size_t bufLen = 0;
};

static uint32_t Rotr32(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

static void Sha256Init(Sha256Ctx& c) {
    static const uint32_t kInit[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    for (int i = 0; i < 8; ++i) c.h[i] = kInit[i];
}

static void Sha256Block(Sha256Ctx& c, const uint8_t* p) {
    static const uint32_t k[64] = {
        0x428a2f98u,0x71374491u,0xb5c0fbcfu,0xe9b5dba5u,0x3956c25bu,0x59f111f1u,0x923f82a4u,0xab1c5ed5u,
        0xd807aa98u,0x12835b01u,0x243185beu,0x550c7dc3u,0x72be5d74u,0x80deb1feu,0x9bdc06a7u,0xc19bf174u,
        0xe49b69c1u,0xefbe4786u,0x0fc19dc6u,0x240ca1ccu,0x2de92c6fu,0x4a7484aau,0x5cb0a9dcu,0x76f988dau,
        0x983e5152u,0xa831c66du,0xb00327c8u,0xbf597fc7u,0xc6e00bf3u,0xd5a79147u,0x06ca6351u,0x14292967u,
        0x27b70a85u,0x2e1b2138u,0x4d2c6dfcu,0x53380d13u,0x650a7354u,0x766a0abbu,0x81c2c92eu,0x92722c85u,
        0xa2bfe8a1u,0xa81a664bu,0xc24b8b70u,0xc76c51a3u,0xd192e819u,0xd6990624u,0xf40e3585u,0x106aa070u,
        0x19a4c116u,0x1e376c08u,0x2748774cu,0x34b0bcb5u,0x391c0cb3u,0x4ed8aa4au,0x5b9cca4fu,0x682e6ff3u,
        0x748f82eeu,0x78a5636fu,0x84c87814u,0x8cc70208u,0x90befffau,0xa4506cebu,0xbef9a3f7u,0xc67178f2u};
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) | (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
    }
    for (int i = 16; i < 64; ++i) {
        uint32_t s0 = Rotr32(w[i - 15], 7) ^ Rotr32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = Rotr32(w[i - 2], 17) ^ Rotr32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = c.h[0], b = c.h[1], cc = c.h[2], d = c.h[3], e = c.h[4], f = c.h[5], g = c.h[6], h = c.h[7];
    for (int i = 0; i < 64; ++i) {
        uint32_t s1 = Rotr32(e, 6) ^ Rotr32(e, 11) ^ Rotr32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + s1 + ch + k[i] + w[i];
        uint32_t s0 = Rotr32(a, 2) ^ Rotr32(a, 13) ^ Rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        uint32_t t2 = s0 + maj;
        h = g; g = f; f = e; e = d + t1; d = cc; cc = b; b = a; a = t1 + t2;
    }
    c.h[0] += a; c.h[1] += b; c.h[2] += cc; c.h[3] += d;
    c.h[4] += e; c.h[5] += f; c.h[6] += g; c.h[7] += h;
}

static void Sha256Update(Sha256Ctx& c, const uint8_t* data, size_t len) {
    c.len += len;
    while (len > 0) {
        size_t take = 64 - c.bufLen;
        if (take > len) take = len;
        memcpy(c.buf + c.bufLen, data, take);
        c.bufLen += take;
        data += take;
        len -= take;
        if (c.bufLen == 64) {
            Sha256Block(c, c.buf);
            c.bufLen = 0;
        }
    }
}

static std::string Sha256Hex(const std::string& input) {
    Sha256Ctx c;
    Sha256Init(c);
    Sha256Update(c, reinterpret_cast<const uint8_t*>(input.data()), input.size());
    uint64_t bitLen = c.len * 8;
    uint8_t pad = 0x80;
    Sha256Update(c, &pad, 1);
    uint8_t zero = 0;
    while (c.bufLen != 56) Sha256Update(c, &zero, 1);
    uint8_t lenBuf[8];
    for (int i = 0; i < 8; ++i) lenBuf[i] = uint8_t((bitLen >> (56 - i * 8)) & 0xFF);
    Sha256Update(c, lenBuf, 8);
    char out[65];
    for (int i = 0; i < 8; ++i) snprintf(out + i * 8, 9, "%08x", c.h[i]);
    return std::string(out, 64);
}

static std::vector<std::string> ParseObjectArrayDirect(const std::string& arrJson) {
    size_t start = arrJson.find('[');
    if (start == std::string::npos) return {};
    int depth = 0;
    bool inStr = false;
    bool escaped = false;
    size_t end = std::string::npos;
    for (size_t i = start; i < arrJson.size(); ++i) {
        char ch = arrJson[i];
        if (inStr) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') inStr = false;
            continue;
        }
        if (ch == '"') { inStr = true; continue; }
        if (ch == '[') depth++;
        else if (ch == ']') { depth--; if (depth == 0) { end = i; break; } }
    }
    if (end == std::string::npos) return {};
    std::vector<std::string> out;
    for (const auto& it : SplitTopLevelArray(arrJson, start, end)) {
        std::string t = StripQuotes(it);
        if (t.size() >= 2 && t.front() == '{' && t.back() == '}') out.push_back(t);
    }
    return out;
}

} // namespace

} // namespace pc

namespace pc {

// ---------------------------------------------------------------------------
// Job definition parsing + validation
// ---------------------------------------------------------------------------

static bool ParseNewJob(const std::string& rawJson, const std::string& taskqueue,
                        int defaultMaxAttempts, int maxTimeoutSec,
                        TaskQueueJob& job, std::string& err) {
    const std::string j = NormalizeJobJsonKeys(rawJson);
    job = TaskQueueJob();
    job.id = JsonFieldStr(j, "id", "");
    if (job.id.empty()) {
        err = "job id is required";
        return false;
    }
    job.taskqueue = taskqueue;
    job.name = JsonFieldStr(j, "name", job.id);
    job.summary = JsonFieldStr(j, "summary", "");
    if (job.summary.empty()) {
        err = "job '" + job.id + "': summary is required";
        return false;
    }
    if (job.summary.find('\n') != std::string::npos || job.summary.find('\r') != std::string::npos) {
        err = "job '" + job.id + "': summary must be a single line";
        return false;
    }
    if (job.summary.size() > 200) {
        err = "job '" + job.id + "': summary must be 200 characters or less";
        return false;
    }
    job.command = JsonFieldStr(j, "command", "");
    if (job.command.empty()) {
        err = "job '" + job.id + "': command is required";
        return false;
    }
    std::string chainErr;
    if (!CheckChainGuard(job.command, chainErr)) {
        err = "job '" + job.id + "': " + chainErr;
        return false;
    }
    job.cwd = JsonFieldStr(j, "cwd", "");
    job.host = JsonFieldStr(j, "host", "");
    if (!job.host.empty()) {
        for (char c : job.host) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '-') {
                err = "job '" + job.id + "': host may only contain letters, digits, '.', '_' and '-'";
                return false;
            }
        }
    }
    job.timeoutSec = static_cast<int>(JsonFieldInt(j, "timeout_sec", 0));
    if (job.timeoutSec <= 0) {
        err = "job '" + job.id + "': timeout_sec is required and must be greater than 0";
        return false;
    }
    if (maxTimeoutSec > 0 && job.timeoutSec > maxTimeoutSec) {
        err = "job '" + job.id + "': timeout_sec (" + std::to_string(job.timeoutSec) +
              ") exceeds max_timeout_sec (" + std::to_string(maxTimeoutSec) + ")";
        return false;
    }
    job.estimateSec = static_cast<int>(JsonFieldInt(j, "estimate_sec", 0));
    if (job.estimateSec < 0) {
        err = "job '" + job.id + "': estimate_sec must be 0 or greater";
        return false;
    }
    std::string idleObj = JsonFieldObj(j, "idle");
    job.idle.outputSec = static_cast<int>(JsonFieldInt(idleObj, "output_sec", 0));
    job.idle.cpuSec = static_cast<int>(JsonFieldInt(idleObj, "cpu_sec", 0));
    if (job.idle.outputSec < 0 || job.idle.cpuSec < 0) {
        err = "job '" + job.id + "': idle values must not be negative";
        return false;
    }
    job.maxAttempts = static_cast<int>(JsonFieldInt(j, "max_attempts", defaultMaxAttempts));
    if (job.maxAttempts < 1) job.maxAttempts = 1;
    job.retryDelaySec = static_cast<int>(JsonFieldInt(j, "retry_delay_sec", 0));
    if (job.retryDelaySec < 0) job.retryDelaySec = 0;
    std::string retryErr;
    if (!ParseRetryChecked(j, job.retry, retryErr)) {
        err = "job '" + job.id + "': " + retryErr;
        return false;
    }
    std::string successObj = JsonFieldObj(j, "success");
    job.success.exitCode = static_cast<int>(JsonFieldInt(successObj, "exit_code", 0));
    job.success.filesNewer = JsonFieldStrArray(successObj, "files_newer");
    job.success.outputRegex = JsonFieldStr(successObj, "output_regex", "");
    job.success.testReport = JsonFieldStr(successObj, "test_report", "");
    job.killExclude = JsonFieldStrArray(j, "kill_exclude");
    std::string resErr;
    if (!ParseResourcesChecked(j, job.resources, resErr)) {
        err = "job '" + job.id + "': " + resErr;
        return false;
    }
    job.memory = ParseMemory(JsonFieldObj(j, "memory", ""));
    if (job.memory.inject) {
        for (const auto& s : job.memory.scopes) {
            std::string kind = s;
            auto colon = kind.find(':');
            if (colon != std::string::npos) kind = kind.substr(0, colon);
            if (kind != "job" && kind != "file" && kind != "taskqueue" && kind != "project" &&
                kind != "engine") {
                err = "job '" + job.id + "': memory.scopes entries must be job, file, taskqueue, project or engine";
                return false;
            }
        }
    }
    std::string deferErr;
    if (!ParseDeferChecked(j, job.success.exitCode, job.defer, deferErr)) {
        err = "job '" + job.id + "': " + deferErr;
        return false;
    }
    job.priority = static_cast<int>(JsonFieldInt(j, "priority", 0));
    job.env = ParseJsonStringMap(JsonFieldObj(j, "env"));
    {
        size_t aStart = 0;
        size_t aEnd = 0;
        if (FindArrayBounds(j, "depends_on", aStart, aEnd)) {
            for (const auto& item : SplitTopLevelArray(j, aStart, aEnd)) {
                std::string t = TrimJsonStr(item);
                if (t.size() >= 2 && t.front() == '{') {
                    TaskQueueDependency dep;
                    dep.id = JsonFieldStr(t, "id", "");
                    dep.on = JsonFieldStr(t, "on", "success");
                    if (dep.on != "success" && dep.on != "done") {
                        err = "job '" + job.id + "': depends_on[].on must be 'success' or 'done'";
                        return false;
                    }
                    if (!dep.id.empty()) job.dependsOn.push_back(dep);
                } else {
                    std::string id = StripQuotes(t);
                    if (!id.empty()) {
                        TaskQueueDependency dep;
                        dep.id = id;
                        dep.on = "success";
                        job.dependsOn.push_back(dep);
                    }
                }
            }
        }
    }

    std::string instrFile = JsonFieldStr(j, "instructions_file", "");
    std::string instrText = JsonFieldStr(j, "instructions_text", "");
    if (!instrFile.empty()) {
        std::wstring pathW;
        bool absolute = instrFile.size() > 1 && (instrFile[1] == ':' || instrFile[0] == '\\' || instrFile[0] == '/');
        if (absolute) {
            pathW = ResolveAbsolutePathWide(Utf8ToWide(instrFile));
        } else {
            std::wstring base = job.cwd.empty() ? GetCurrentDirectoryWide() : ResolveAbsolutePathWide(Utf8ToWide(job.cwd));
            pathW = ResolveAbsolutePathWide(PathJoinWide(base, Utf8ToWide(instrFile)));
        }
        if (!PathExistsWide(pathW)) {
            err = "job '" + job.id + "': instructions_file not found: " + instrFile;
            return false;
        }
        std::string text = ReadFileUtf8Wide(pathW);
        if (text.size() > 1024 * 1024) {
            err = "job '" + job.id + "': instructions exceed the 1 MB limit";
            return false;
        }
        job.instructions.source = instrFile;
        job.instructions.text = text;
        job.instructions.sha256 = Sha256Hex(text);
    } else if (!instrText.empty()) {
        if (instrText.size() > 1024 * 1024) {
            err = "job '" + job.id + "': instructions exceed the 1 MB limit";
            return false;
        }
        job.instructions.text = instrText;
        job.instructions.sha256 = Sha256Hex(instrText);
    }
    job.instructions.rev = 1;
    job.instructions.capturedAt = GetCurrentTimestampStr();
    job.state = "pending";
    job.createdAt = GetCurrentTimestampStr();
    job.updatedAt = job.createdAt;
    return true;
}

} // namespace pc

namespace pc {

static std::wstring ResolveJobPathWide(const TaskQueueJob& job, const std::string& path) {
    bool absolute = path.size() > 1 && (path[1] == ':' || path[0] == '\\' || path[0] == '/');
    if (absolute) return ResolveAbsolutePathWide(Utf8ToWide(path));
    std::wstring base = job.cwd.empty() ? GetCurrentDirectoryWide() : ResolveAbsolutePathWide(Utf8ToWide(job.cwd));
    return ResolveAbsolutePathWide(PathJoinWide(base, Utf8ToWide(path)));
}

std::string TaskQueueManager::TaskQueueAdd(const std::string& taskqueue, const std::string& jobsJson,
                                           int64_t concurrency, const std::string& notifyFile, std::string& outError,
                                           const std::string& originJson) {
    outError.clear();
    const TaskQueueOrigin origin = ParseOriginJson(originJson);
    if (taskqueue.empty()) {
        outError = "taskqueue is required";
        return BuildError(outError);
    }
    std::vector<std::string> jobObjs = ParseObjectArrayDirect(jobsJson);
    if (jobObjs.empty()) {
        outError = "jobs must be a non-empty array of job objects";
        return BuildError(outError);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    TaskQueue tq;
    auto existing = queues_.find(taskqueue);
    if (existing != queues_.end()) tq = existing->second;
    if (tq.name.empty()) tq.name = taskqueue;
    if (tq.concurrency <= 0) tq.concurrency = 1;
    if (tq.maxTimeoutSec <= 0) tq.maxTimeoutSec = 7200;
    if (tq.defaultMaxAttempts <= 0) tq.defaultMaxAttempts = 2;
    if (concurrency > 0) tq.concurrency = static_cast<int>(concurrency);
    if (!notifyFile.empty()) tq.notifyFile = notifyFile;

    std::vector<TaskQueueJob> newJobs;
    for (const auto& jo : jobObjs) {
        TaskQueueJob job;
        std::string err;
        if (!ParseNewJob(jo, taskqueue, tq.defaultMaxAttempts, tq.maxTimeoutSec, job, err)) {
            outError = err;
            return BuildError(outError);
        }
        job.origin = origin;
        if (FindJob(job.id, nullptr) != nullptr) {
            outError = "duplicate job id '" + job.id + "'";
            return BuildError(outError);
        }
        for (const auto& other : newJobs) {
            if (other.id == job.id) {
                outError = "duplicate job id '" + job.id + "'";
                return BuildError(outError);
            }
        }
        newJobs.push_back(job);
    }

    TaskQueue merged = tq;
    for (const auto& j : newJobs) merged.jobs.push_back(j);
    for (const auto& job : newJobs) {
        for (const auto& dep : job.dependsOn) {
            bool exists = false;
            for (const auto& mj : merged.jobs) {
                if (mj.id == dep.id) { exists = true; break; }
            }
            if (!exists) {
                outError = "job '" + job.id + "': unknown dependency '" + dep.id + "'";
                return BuildError(outError);
            }
        }
        if (HasDependencyCycle(merged, job.id, job.dependsOn, "")) {
            outError = "job '" + job.id + "': dependency cycle detected";
            return BuildError(outError);
        }
    }

    for (auto& j : newJobs) tq.jobs.push_back(j);
    for (auto& j : tq.jobs) {
        if (j.num <= 0) j.num = nextJobNum_++;
    }
    queues_[taskqueue] = tq;
    ReevaluateAll();
    SaveQueue(queues_[taskqueue]);
    cv_.notify_all();

    std::ostringstream oss;
    oss << "{\"ok\":true,\"taskqueue\":\"" << JsonEscape(taskqueue) << "\",\"added\":[";
    for (size_t i = 0; i < newJobs.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << JsonEscape(newJobs[i].id) << "\"";
    }
    oss << "],\"concurrency\":" << queues_[taskqueue].concurrency << "}";
    return oss.str();
}

std::string TaskQueueManager::AddBtJob(const std::string& taskqueue, const std::string& jobJson,
                                       const TaskQueueBtRef& ref, std::string& outError) {
    outError.clear();
    if (taskqueue.empty()) {
        outError = "taskqueue is required";
        return BuildError(outError);
    }
    if (ref.id.empty()) {
        outError = "bt.id is required for AddBtJob";
        return BuildError(outError);
    }

    std::string jo = TrimJsonStr(jobJson);
    if (!jo.empty() && jo.front() == '[') {
        auto arr = ParseObjectArrayDirect(jo);
        if (arr.empty()) {
            outError = "invalid BT job JSON";
            return BuildError(outError);
        }
        jo = arr.front();
    }

    std::lock_guard<std::mutex> lock(mutex_);
    TaskQueue tq;
    auto existing = queues_.find(taskqueue);
    if (existing != queues_.end()) tq = existing->second;
    if (tq.name.empty()) tq.name = taskqueue;
    if (tq.concurrency <= 0) tq.concurrency = 1;
    if (tq.maxTimeoutSec <= 0) tq.maxTimeoutSec = 7200;
    if (tq.defaultMaxAttempts <= 0) tq.defaultMaxAttempts = 1;

    TaskQueueJob job;
    std::string err;
    if (!ParseNewJob(jo, taskqueue, /*defaultMaxAttempts=*/1, /*maxTimeoutSec=*/0, job, err)) {
        outError = err;
        return BuildError(outError);
    }
    job.bt = ref;
    auto btOrigin = btOrigins_.find(ref.id);
    if (btOrigin != btOrigins_.end()) job.origin = btOrigin->second;
    if (FindJob(job.id, nullptr) != nullptr) {
        outError = "duplicate job id '" + job.id + "'";
        return BuildError(outError);
    }
    job.num = nextJobNum_++;
    tq.jobs.push_back(job);
    queues_[taskqueue] = tq;
    ReevaluateAll();
    SaveQueue(queues_[taskqueue]);
    cv_.notify_all();

    return "{\"ok\":true,\"taskqueue\":\"" + JsonEscape(taskqueue) +
           "\",\"id\":\"" + JsonEscape(job.id) +
           "\",\"added\":[\"" + JsonEscape(job.id) + "\"]}";
}

std::string TaskQueueManager::TaskQueueList(const std::string& taskqueue, const std::string& state, int64_t sinceSec, std::string& outError,
                                            const std::string& origin, const std::string& btId) {
    outError.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    struct ListedJob { std::string queue; TaskQueueJob job; };
    std::vector<ListedJob> listed;
    const int64_t cutoff = static_cast<int64_t>(time(nullptr)) - sinceSec;
    for (const auto& [name, tq] : queues_) {
        if (!taskqueue.empty() && name != taskqueue) continue;
        for (const auto& job : tq.jobs) {
            if (!state.empty() && job.state != state) continue;
            if (!origin.empty() && !OriginMatches(job.origin, origin)) continue;
            if (!btId.empty() && job.bt.id != btId) continue;
            if (sinceSec > 0) {
                const int64_t ts = ParseTimestampStrToEpoch(job.updatedAt);
                if (ts <= 0 || ts < cutoff) continue;
            }
            listed.push_back({name, job});
        }
    }
    std::stable_sort(listed.begin(), listed.end(), [](const ListedJob& a, const ListedJob& b) {
        if (a.job.createdAt != b.job.createdAt) return a.job.createdAt > b.job.createdAt;
        return false;
    });
    std::ostringstream oss;
    oss << "{\"ok\":true,\"jobs\":[";
    bool first = true;
    const int64_t nowMs = static_cast<int64_t>(GetNowTickMs());
    for (const auto& item : listed) {
        if (!first) oss << ",";
        first = false;
        // Same resource_wait field the graph reports, so hint consumers reading the list see it too.
        // Null when the job is not waiting on a resource; the list is the only place it is added.
        bool allSat = true;
        for (const auto& d : item.job.dependsOn) {
            const TaskQueueJob* dep = FindJobConst(d.id);
            if (!dep || !IsDepSatisfied(*dep, d.on)) { allSat = false; break; }
        }
        std::string resourceWait = ResourceWaitLocked(item.job, allSat, nowMs);
        std::string jj = JobToJson(item.job, false);
        if (!jj.empty() && jj.back() == '}') jj.pop_back();
        oss << jj << ",\"resource_wait\":";
        if (resourceWait.empty()) oss << "null";
        else oss << "\"" << JsonEscape(resourceWait) << "\"";
        oss << "}";
    }
    oss << "]}";
    return oss.str();
}

} // namespace pc

namespace pc {

bool TaskQueueManager::GetJobSnapshot(const std::string& id, TaskQueueJob& outJob) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const TaskQueueJob* found = FindJobConst(id);
    if (!found) return false;
    outJob = *found;
    return true;
}

std::string TaskQueueManager::TaskQueueGet(const std::string& id, bool includeInstructions, int rev, std::string& outError) {
    outError.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    TaskQueueJob* found = FindJob(id, nullptr);
    if (!found) {
        outError = "job not found: " + id;
        return BuildError(outError);
    }
    TaskQueueJob job = *found;

    if (rev > 0 && rev != job.instructions.rev) {
        std::string text = ReadFileUtf8Wide(GetInstructionsFileWide(job.taskqueue, job.id, rev));
        if (!text.empty()) {
            job.instructions.text = text;
            job.instructions.rev = rev;
        }
    }

    bool sourceMissing = false;
    bool sourceChanged = false;
    if (!job.instructions.source.empty()) {
        std::wstring pathW = ResolveJobPathWide(job, job.instructions.source);
        if (!PathExistsWide(pathW)) {
            sourceMissing = true;
        } else {
            std::string text = ReadFileUtf8Wide(pathW);
            sourceChanged = Sha256Hex(text) != job.instructions.sha256;
        }
    }

    std::ostringstream oss;
    oss << "{\"ok\":true,\"source_changed\":" << (sourceChanged ? "true" : "false")
        << ",\"source_missing\":" << (sourceMissing ? "true" : "false")
        << ",\"job\":" << JobToJson(job, includeInstructions) << "}";
    return oss.str();
}

void TaskQueueManager::RequeueJobLocked(TaskQueue& tq, TaskQueueJob& job, bool resetAttempts, bool refreshInstructions) {
    if (refreshInstructions && !job.instructions.source.empty()) {
        std::wstring pathW = ResolveJobPathWide(job, job.instructions.source);
        if (PathExistsWide(pathW)) {
            std::string text = ReadFileUtf8Wide(pathW);
            std::string sha = Sha256Hex(text);
            if (sha != job.instructions.sha256) {
                job.instructions.text = text;
                job.instructions.sha256 = sha;
                job.instructions.rev++;
                job.instructions.capturedAt = GetCurrentTimestampStr();
                EmitEventLocked(tq, "instructionsUpdated", job.id, 0);
            }
        }
    }

    if (resetAttempts) job.attempts.clear();
    job.state = "pending";
    job.blockedBy.clear();
    job.retryAtMs = 0;
    job.updatedAt = GetCurrentTimestampStr();
}

std::string TaskQueueManager::TaskQueueRetry(const std::string& id, bool resetAttempts, bool refreshInstructions, std::string& outError) {
    return TaskQueueRetry(id, resetAttempts, refreshInstructions, TaskQueueCaller::Gui, outError);
}

std::string TaskQueueManager::TaskQueueRetry(const std::string& id, bool resetAttempts, bool refreshInstructions,
                                             TaskQueueCaller caller, std::string& outError) {
    outError.clear();
    std::unique_lock<std::mutex> lock(mutex_);
    TaskQueue* tq = nullptr;
    TaskQueueJob* job = FindJob(id, &tq);
    if (!job) {
        outError = "job not found: " + id;
        return BuildError(outError);
    }
    if (job->state == "running") {
        outError = "job '" + id + "' is running; cancel it before retrying";
        return BuildError(outError);
    }
    if (job->state == "succeeded") {
        outError = "job '" + id + "' already succeeded";
        return BuildError(outError);
    }
    if (!job->bt.id.empty()) {
        if (caller == TaskQueueCaller::External) {
            outError = "this job is managed by a behavior tree; use bt_retry";
            return BuildError(outError);
        }
        TaskQueueBtRef ref = job->bt;
        TaskQueueJob snap = *job;
        BtRetryCallback cb = btRetryCb_;
        lock.unlock();
        if (!cb) {
            outError = "behavior tree retry handler not registered";
            return BuildError(outError);
        }
        if (!cb(ref, resetAttempts, outError)) {
            return BuildError(outError.empty() ? "behavior tree retry failed" : outError);
        }
        TaskQueueJob updated = snap;
        GetJobSnapshot(id, updated);
        return "{\"ok\":true,\"source_changed\":false,\"bt_retried\":true,\"job\":" + JobToJson(updated, true) + "}";
    }

    RequeueJobLocked(*tq, *job, resetAttempts, refreshInstructions);
    ReevaluateAll();
    SaveQueue(*tq);
    cv_.notify_all();
    return "{\"ok\":true,\"source_changed\":false,\"job\":" + JobToJson(*job, true) + "}";
}

std::string TaskQueueManager::TaskQueueRetryFailed(const std::string& taskqueue, int64_t sinceSec, bool resetAttempts, bool refreshInstructions, std::string& outError) {
    return TaskQueueRetryFailed(taskqueue, sinceSec, resetAttempts, refreshInstructions, TaskQueueCaller::Gui, outError);
}

std::string TaskQueueManager::TaskQueueRetryFailed(const std::string& taskqueue, int64_t sinceSec, bool resetAttempts,
                                                   bool refreshInstructions, TaskQueueCaller caller, std::string& outError) {
    outError.clear();
    std::vector<std::string> retried;
    std::vector<std::pair<std::string, TaskQueueBtRef>> btToRetry;
    BtRetryCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!taskqueue.empty() && queues_.find(taskqueue) == queues_.end()) {
            outError = "task queue not found: " + taskqueue;
            return BuildError(outError);
        }
        cb = btRetryCb_;
        const int64_t cutoff = static_cast<int64_t>(time(nullptr)) - sinceSec;
        for (auto& [name, tq] : queues_) {
            if (!taskqueue.empty() && name != taskqueue) continue;
            if (taskqueue.empty() && name == "plain") continue;
            bool changed = false;
            for (auto& job : tq.jobs) {
                if (job.state != "failed") continue;
                if (sinceSec > 0) {
                    const int64_t ts = ParseTimestampStrToEpoch(job.updatedAt);
                    if (ts <= 0 || ts < cutoff) continue;
                }
                if (!job.bt.id.empty()) {
                    if (caller == TaskQueueCaller::Gui) {
                        btToRetry.push_back({job.id, job.bt});
                    }
                    continue;
                }
                RequeueJobLocked(tq, job, resetAttempts, refreshInstructions);
                retried.push_back(job.id);
                changed = true;
            }
            if (changed) {
                ReevaluateAll();
                SaveQueue(tq);
            }
        }
        if (!retried.empty()) cv_.notify_all();
    }
    if (cb) {
        for (const auto& item : btToRetry) {
            std::string btErr;
            if (cb(item.second, resetAttempts, btErr)) {
                retried.push_back(item.first);
            }
        }
    }
    std::ostringstream oss;
    oss << "{\"ok\":true,\"retried_count\":" << retried.size() << ",\"retried\":[";
    for (size_t i = 0; i < retried.size(); ++i) {
        if (i) oss << ",";
        oss << "\"" << JsonEscape(retried[i]) << "\"";
    }
    oss << "]}";
    return oss.str();
}

} // namespace pc

namespace pc {

std::string TaskQueueManager::TaskQueueUpdate(const std::string& id, const std::string& rawPatchJson, std::string& outError) {
    return TaskQueueUpdate(id, rawPatchJson, TaskQueueCaller::Gui, outError);
}

std::string TaskQueueManager::TaskQueueUpdate(const std::string& id, const std::string& rawPatchJson,
                                              TaskQueueCaller caller, std::string& outError) {
    outError.clear();
    const std::string patchJson = NormalizeJobJsonKeys(rawPatchJson);
    std::lock_guard<std::mutex> lock(mutex_);
    TaskQueue* tq = nullptr;
    TaskQueueJob* job = FindJob(id, &tq);
    if (!job) {
        outError = "job not found: " + id;
        return BuildError(outError);
    }
    if (!job->bt.id.empty() && caller == TaskQueueCaller::External) {
        outError = "this job is managed by a behavior tree; use bt_retry";
        return BuildError(outError);
    }
    bool onlyNotes = HasJsonKey(patchJson, "notes");
    // Only "notes" may be updated after a job has finished or is running.
    if (onlyNotes) {
        for (const char* k : {"summary","command","cwd","host","timeout_sec","idle","max_attempts",
                              "retry_delay_sec","retry","defer","priority","kill_exclude","resources","memory","env","success",
                              "instructions_text","instructions_file","depends_on"}) {
            if (HasJsonKey(patchJson, k)) { onlyNotes = false; break; }
        }
    }
    if ((job->state == "running" || job->state == "succeeded" || job->state == "cancelled") && !onlyNotes) {
        outError = "job '" + id + "' can only be updated when pending, failed or blocked (except notes)";
        return BuildError(outError);
    }

    if (HasJsonKey(patchJson, "summary")) {
        std::string s = JsonFieldStr(patchJson, "summary", "");
        if (s.empty() || s.find('\n') != std::string::npos || s.find('\r') != std::string::npos || s.size() > 200) {
            outError = "summary must be a single line of 1..200 characters";
            return BuildError(outError);
        }
        job->summary = s;
    }
    if (HasJsonKey(patchJson, "command")) {
        std::string c = JsonFieldStr(patchJson, "command", "");
        if (c.empty()) {
            outError = "command must not be empty";
            return BuildError(outError);
        }
        std::string chainErr;
        if (!CheckChainGuard(c, chainErr)) {
            outError = chainErr;
            return BuildError(outError);
        }
        job->command = c;
    }
    if (HasJsonKey(patchJson, "cwd")) job->cwd = JsonFieldStr(patchJson, "cwd", "");
    if (HasJsonKey(patchJson, "host")) {
        std::string h = JsonFieldStr(patchJson, "host", "");
        for (char c : h) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '-') {
                outError = "host may only contain letters, digits, '.', '_' and '-'";
                return BuildError(outError);
            }
        }
        job->host = h;
    }
    if (HasJsonKey(patchJson, "timeout_sec")) {
        int t = static_cast<int>(JsonFieldInt(patchJson, "timeout_sec", 0));
        if (t <= 0) {
            outError = "timeout_sec must be greater than 0";
            return BuildError(outError);
        }
        if (tq->maxTimeoutSec > 0 && t > tq->maxTimeoutSec) {
            outError = "timeout_sec exceeds max_timeout_sec (" + std::to_string(tq->maxTimeoutSec) + ")";
            return BuildError(outError);
        }
        job->timeoutSec = t;
    }
    if (HasJsonKey(patchJson, "idle")) {
        std::string idleObj = JsonFieldObj(patchJson, "idle");
        job->idle.outputSec = static_cast<int>(JsonFieldInt(idleObj, "output_sec", job->idle.outputSec));
        job->idle.cpuSec = static_cast<int>(JsonFieldInt(idleObj, "cpu_sec", job->idle.cpuSec));
    }
    if (HasJsonKey(patchJson, "max_attempts")) {
        int m = static_cast<int>(JsonFieldInt(patchJson, "max_attempts", job->maxAttempts));
        if (m >= 1) job->maxAttempts = m;
    }
    if (HasJsonKey(patchJson, "retry_delay_sec")) {
        int r = static_cast<int>(JsonFieldInt(patchJson, "retry_delay_sec", job->retryDelaySec));
        job->retryDelaySec = (r > 0) ? r : 0;
    }
    if (HasJsonKey(patchJson, "retry")) {
        struct TaskQueueRetry r;
        std::string retryErr;
        if (!ParseRetryChecked(patchJson, r, retryErr)) {
            outError = retryErr;
            return BuildError(outError);
        }
        job->retry = r;
    }
    if (HasJsonKey(patchJson, "priority")) job->priority = static_cast<int>(JsonFieldInt(patchJson, "priority", job->priority));
    if (HasJsonKey(patchJson, "kill_exclude")) job->killExclude = JsonFieldStrArray(patchJson, "kill_exclude");
    if (HasJsonKey(patchJson, "resources")) {
        TaskQueueResources res;
        std::string resErr;
        if (!ParseResourcesChecked(patchJson, res, resErr)) {
            outError = resErr;
            return BuildError(outError);
        }
        job->resources = res;
    }
    if (HasJsonKey(patchJson, "memory")) {
        job->memory = ParseMemory(JsonFieldObj(patchJson, "memory", ""));
    }
    if (HasJsonKey(patchJson, "env")) job->env = ParseJsonStringMap(JsonFieldObj(patchJson, "env"));
    if (HasJsonKey(patchJson, "success")) {
        std::string successObj = JsonFieldObj(patchJson, "success");
        job->success.exitCode = static_cast<int>(JsonFieldInt(successObj, "exit_code", job->success.exitCode));
        job->success.filesNewer = JsonFieldStrArray(successObj, "files_newer");
        job->success.outputRegex = JsonFieldStr(successObj, "output_regex", "");
        job->success.testReport = JsonFieldStr(successObj, "test_report", "");
    }
    if (HasJsonKey(patchJson, "defer")) {
        TaskQueueDefer d;
        std::string deferErr;
        if (!ParseDeferChecked(patchJson, job->success.exitCode, d, deferErr)) {
            outError = deferErr;
            return BuildError(outError);
        }
        job->defer = d;
    }
    if (HasJsonKey(patchJson, "notes")) {
        job->notes = JsonFieldStr(patchJson, "notes", "");
    }

    std::string newText;
    bool haveNewText = false;
    if (HasJsonKey(patchJson, "instructions_text")) {
        newText = JsonFieldStr(patchJson, "instructions_text", "");
        haveNewText = true;
    } else if (HasJsonKey(patchJson, "instructions_file")) {
        std::string f = JsonFieldStr(patchJson, "instructions_file", "");
        std::wstring pathW = ResolveJobPathWide(*job, f);
        if (!PathExistsWide(pathW)) {
            outError = "instructions_file not found: " + f;
            return BuildError(outError);
        }
        newText = ReadFileUtf8Wide(pathW);
        job->instructions.source = f;
        haveNewText = true;
    }
    if (haveNewText) {
        if (newText.size() > 1024 * 1024) {
            outError = "instructions exceed the 1 MB limit";
            return BuildError(outError);
        }
        job->instructions.text = newText;
        job->instructions.sha256 = Sha256Hex(newText);
        job->instructions.rev++;
        job->instructions.capturedAt = GetCurrentTimestampStr();
        EmitEventLocked(*tq, "instructionsUpdated", job->id, 0);
    }

    job->updatedAt = GetCurrentTimestampStr();
    ReevaluateAll();
    SaveQueue(*tq);
    cv_.notify_all();
    return "{\"ok\":true,\"job\":" + JobToJson(*job, true) + "}";
}

} // namespace pc

namespace pc {

std::string TaskQueueManager::TaskQueueCancel(const std::string& id, std::string& outError) {
    outError.clear();
    std::string resp;
    ProcessManager* pm = nullptr;
    int pid = 0;
    std::string remoteHost;
    int remotePid = 0;
    RemoteKillCallback remoteKill;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        TaskQueue* tq = nullptr;
        TaskQueueJob* job = FindJob(id, &tq);
        if (!job) {
            outError = "job not found: " + id;
            return BuildError(outError);
        }
        if (job->state == "cancelled") {
            return "{\"ok\":true,\"job\":" + JobToJson(*job, true) + "}";
        }
        if (job->state == "succeeded") {
            outError = "job '" + id + "' already succeeded";
            return BuildError(outError);
        }
        if (job->state == "running" && !job->attempts.empty()) {
            pid = job->attempts.back().processId;
            remoteHost = job->attempts.back().remoteHost;
            remotePid = job->attempts.back().remoteProcessId;
        }
        job->state = "cancelled";
        job->blockedBy.clear();
        job->retryAtMs = 0;
        job->updatedAt = GetCurrentTimestampStr();
        EmitEventLocked(*tq, "jobCancelled", id, job->attempts.empty() ? 0 : job->attempts.back().n);
        ReevaluateAll();
        SaveQueue(*tq);
        if (schedulerRunning_.load()) {
            pendingFinished_.push_back({*job, "cancelled"});
        }
        resp = "{\"ok\":true,\"job\":" + JobToJson(*job, true) + "}";
        pm = pm_;
        remoteKill = remoteKillCb_;
    }
    if (!remoteHost.empty() && remotePid > 0) {
        if (remoteKill) {
            std::string err;
            remoteKill(remoteHost, remotePid, err);
        }
    } else if (pm && pid > 0) {
        std::string err;
        pm->KillProcess(pid, err);
    }
    FlushFinishedCallbacks();
    return resp;
}

std::string TaskQueueManager::TaskQueuePause(const std::string& taskqueue, bool paused, std::string& outError) {
    outError.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = queues_.find(taskqueue);
    if (it == queues_.end()) {
        outError = "task queue not found: " + taskqueue;
        return BuildError(outError);
    }
    it->second.paused = paused;
    it->second.version++;
    SaveQueue(it->second);
    if (!paused) cv_.notify_all();
    return "{\"ok\":true,\"taskqueue\":\"" + JsonEscape(taskqueue) + "\",\"paused\":" +
           (paused ? "true" : "false") + ",\"version\":" + std::to_string(it->second.version) + "}";
}

} // namespace pc

namespace pc {

std::string TaskQueueManager::TaskQueueWait(const std::string& taskqueue, int sinceVersion, int timeoutMs, std::string& outError) {
    outError.clear();
    uint64_t start = GetNowTickMs();
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
        auto it = queues_.find(taskqueue);
        int64_t ver = (it == queues_.end()) ? 0 : it->second.version;
        if (ver != sinceVersion) break;
        if (timeoutMs >= 0 && static_cast<int64_t>(GetNowTickMs() - start) >= timeoutMs) break;
        cv_.wait_for(lock, std::chrono::milliseconds(100));
    }
    auto it = queues_.find(taskqueue);
    if (it == queues_.end()) {
        outError = "task queue not found: " + taskqueue;
        return BuildError(outError);
    }
    std::ostringstream oss;
    oss << "{\"ok\":true,\"taskqueue\":\"" << JsonEscape(taskqueue) << "\""
        << ",\"version\":" << it->second.version
        << ",\"since_version\":" << sinceVersion << ",\"jobs\":[";
    for (size_t i = 0; i < it->second.jobs.size(); ++i) {
        if (i > 0) oss << ",";
        oss << JobToJson(it->second.jobs[i], false);
    }
    oss << "]}";
    return oss.str();
}

std::string TaskQueueManager::TaskQueuePurge(const std::string& taskqueue, const std::string& statesJson, std::string& outError) {
    outError.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = queues_.find(taskqueue);
    if (it == queues_.end()) {
        outError = "task queue not found: " + taskqueue;
        return BuildError(outError);
    }
    std::vector<std::string> states;
    if (!statesJson.empty()) {
        states = JsonFieldStrArray("{\"states\":" + statesJson + "}", "states");
        if (states.empty()) states = JsonFieldStrArray(statesJson, "states");
    }
    if (states.empty()) states = {"succeeded", "failed", "cancelled"};

    size_t before = it->second.jobs.size();
    auto& jobs = it->second.jobs;
    jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [&](const TaskQueueJob& j) {
        return std::find(states.begin(), states.end(), j.state) != states.end();
    }), jobs.end());
    size_t purged = before - jobs.size();
    it->second.version++;
    SaveQueue(it->second);
    return "{\"ok\":true,\"taskqueue\":\"" + JsonEscape(taskqueue) + "\",\"purged\":" + std::to_string(purged) + "}";
}

std::string TaskQueueManager::TaskQueuePurgeOlderThan(const std::string& taskqueue, int64_t maxAgeSec, std::string& outError) {
    outError.clear();
    if (maxAgeSec < 0) {
        outError = "older_than_sec must be >= 0";
        return BuildError(outError);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (!taskqueue.empty() && queues_.find(taskqueue) == queues_.end()) {
        outError = "task queue not found: " + taskqueue;
        return BuildError(outError);
    }
    const int64_t cutoff = static_cast<int64_t>(time(nullptr)) - maxAgeSec;
    const std::vector<std::string> finished = {"succeeded", "failed", "cancelled"};
    size_t purged = 0;
    for (auto& [name, tq] : queues_) {
        if (!taskqueue.empty() && name != taskqueue) continue;
        size_t before = tq.jobs.size();
        tq.jobs.erase(std::remove_if(tq.jobs.begin(), tq.jobs.end(), [&](const TaskQueueJob& j) {
            if (std::find(finished.begin(), finished.end(), j.state) == finished.end()) return false;
            int64_t ts = ParseTimestampStrToEpoch(j.updatedAt);
            return ts > 0 && ts < cutoff;
        }), tq.jobs.end());
        size_t removed = before - tq.jobs.size();
        if (removed > 0) {
            purged += removed;
            tq.version++;
            SaveQueue(tq);
        }
    }
    return "{\"ok\":true,\"taskqueue\":\"" + JsonEscape(taskqueue) + "\",\"purged\":" + std::to_string(purged) + "}";
}

} // namespace pc

namespace pc {

std::string TaskQueueManager::TaskQueueLink(const std::string& jobId, const std::string& depId, const std::string& on, std::string& outError) {
    outError.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    TaskQueue* tq = nullptr;
    TaskQueueJob* job = FindJob(jobId, &tq);
    if (!job) {
        outError = "job not found: " + jobId;
        return BuildError(outError);
    }
    if (job->state == "running" || job->state == "succeeded" || job->state == "cancelled") {
        outError = "dependencies can only be changed while pending, blocked or failed";
        return BuildError(outError);
    }
    if (depId.empty() || depId == jobId) {
        outError = "invalid dependency id";
        return BuildError(outError);
    }
    if (!FindJobConst(depId)) {
        outError = "unknown dependency: " + depId;
        return BuildError(outError);
    }
    for (const auto& d : job->dependsOn) {
        if (d.id == depId) {
            outError = "dependency already exists: " + depId;
            return BuildError(outError);
        }
    }
    std::string onVal = on.empty() ? "success" : on;
    if (onVal != "success" && onVal != "done") {
        outError = "on must be 'success' or 'done'";
        return BuildError(outError);
    }
    std::vector<TaskQueueDependency> newDeps = job->dependsOn;
    TaskQueueDependency nd;
    nd.id = depId;
    nd.on = onVal;
    newDeps.push_back(nd);
    if (HasDependencyCycle(*tq, jobId, newDeps, "")) {
        outError = "dependency cycle detected";
        return BuildError(outError);
    }
    job->dependsOn = newDeps;
    job->updatedAt = GetCurrentTimestampStr();
    EmitEventLocked(*tq, "dependencyLinked", jobId, 0);
    ReevaluateAll();
    SaveQueue(*tq);
    cv_.notify_all();
    return "{\"ok\":true,\"job\":" + JobToJson(*job, true) + "}";
}

std::string TaskQueueManager::TaskQueueUnlink(const std::string& jobId, const std::string& depId, std::string& outError) {
    outError.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    TaskQueue* tq = nullptr;
    TaskQueueJob* job = FindJob(jobId, &tq);
    if (!job) {
        outError = "job not found: " + jobId;
        return BuildError(outError);
    }
    if (job->state == "running" || job->state == "succeeded" || job->state == "cancelled") {
        outError = "dependencies can only be changed while pending, blocked or failed";
        return BuildError(outError);
    }
    auto& deps = job->dependsOn;
    auto it = std::remove_if(deps.begin(), deps.end(), [&](const TaskQueueDependency& d) { return d.id == depId; });
    if (it == deps.end()) {
        outError = "dependency not found: " + depId;
        return BuildError(outError);
    }
    deps.erase(it, deps.end());
    job->updatedAt = GetCurrentTimestampStr();
    EmitEventLocked(*tq, "dependencyUnlinked", jobId, 0);
    ReevaluateAll();
    SaveQueue(*tq);
    cv_.notify_all();
    return "{\"ok\":true,\"job\":" + JobToJson(*job, true) + "}";
}

} // namespace pc

namespace pc {

void TaskQueueManager::SaveAllLocked() {
    for (const auto& [name, tq] : queues_) {
        (void)name;
        SaveQueue(tq);
    }
}

bool TaskQueueManager::ReevaluateJob(TaskQueue& tq, TaskQueueJob& job) {
    if (job.state == "running" || job.state == "succeeded" || job.state == "cancelled") return false;

    std::string blocker;
    for (const auto& d : job.dependsOn) {
        const TaskQueueJob* dep = FindJobConst(d.id);
        if (!dep) {
            for (const auto& tj : tq.jobs) {
                if (tj.id == d.id) { dep = &tj; break; }
            }
        }
        if (!dep) { blocker = d.id; break; }
        if (d.on == "success" &&
            (dep->state == "failed" || dep->state == "cancelled" || dep->state == "blocked")) {
            blocker = dep->id;
            break;
        }
    }

    if (!blocker.empty()) {
        if (job.state != "blocked" || job.blockedBy != blocker) {
            job.state = "blocked";
            job.blockedBy = blocker;
            job.updatedAt = GetCurrentTimestampStr();
            EmitEventLocked(tq, "jobBlocked", job.id, 0);
            return true;
        }
        return false;
    }

    if (job.state == "blocked") {
        job.state = "pending";
        job.blockedBy.clear();
        job.updatedAt = GetCurrentTimestampStr();
        return true;
    }
    return false;
}

void TaskQueueManager::ReevaluateAll() {
    for (int iter = 0; iter < 64; ++iter) {
        bool changed = false;
        for (auto& [name, tq] : queues_) {
            (void)name;
            for (auto& job : tq.jobs) {
                if (ReevaluateJob(tq, job)) changed = true;
            }
        }
        if (!changed) break;
    }
}

void TaskQueueManager::ReevaluateDependents(const std::string& jobId) {
    (void)jobId;
    ReevaluateAll();
}

void TaskQueueManager::CheckQueueNotices(TaskQueue& tq) {
    int pendingCount = 0;
    int runningCount = 0;
    bool anyReady = false;
    for (const auto& job : tq.jobs) {
        if (job.state == "pending") {
            pendingCount++;
            bool allSat = true;
            for (const auto& d : job.dependsOn) {
                const TaskQueueJob* dep = FindJobConst(d.id);
                if (!dep || !IsDepSatisfied(*dep, d.on)) { allSat = false; break; }
            }
            // A job waiting out retryDelaySec / defer.delaySec starts on its own: not stalled.
            if (allSat) anyReady = true;
        } else if (job.state == "running") {
            runningCount++;
        }
    }

    std::string notice;
    if (!tq.jobs.empty() && pendingCount == 0 && runningCount == 0) {
        notice = "taskqueueDrained";
    } else if (pendingCount > 0 && !anyReady && !tq.paused) {
        notice = "taskqueueStalled";
    }

    std::string& last = lastNotice_[tq.name];
    if (notice != last) {
        last = notice;
        if (!notice.empty()) EmitEventLocked(tq, notice, "", 0);
    }
}

} // namespace pc

namespace pc {

namespace {

void CollectUpstream(const std::map<std::string, TaskQueue>& queues, const std::string& id, std::set<std::string>& keep) {
    if (!keep.insert(id).second) return;
    for (const auto& [name, tq] : queues) {
        (void)name;
        for (const auto& job : tq.jobs) {
            if (job.id != id) continue;
            for (const auto& d : job.dependsOn) CollectUpstream(queues, d.id, keep);
        }
    }
}

void CollectDownstream(const std::map<std::string, TaskQueue>& queues, const std::string& id, std::set<std::string>& keep) {
    if (!keep.insert(id).second) return;
    for (const auto& [name, tq] : queues) {
        (void)name;
        for (const auto& job : tq.jobs) {
            for (const auto& d : job.dependsOn) {
                if (d.id == id) CollectDownstream(queues, job.id, keep);
            }
        }
    }
}

} // namespace

std::string TaskQueueManager::TaskQueueGraph(const std::string& taskqueue, const std::string& root, std::string& outError) {
    outError.clear();
    std::lock_guard<std::mutex> lock(mutex_);

    std::set<std::string> keep;
    if (!root.empty()) {
        if (!FindJobConst(root)) {
            outError = "job not found: " + root;
            return BuildError(outError);
        }
        CollectUpstream(queues_, root, keep);
        CollectDownstream(queues_, root, keep);
    }

    std::ostringstream oss;
    oss << "{\"ok\":true,\"resource_locks\":" << (resourceLocksEnabled_.load() ? "true" : "false") << ",\"jobs\":[";
    bool first = true;
    int64_t now = static_cast<int64_t>(GetNowTickMs());
    for (const auto& [name, tq] : queues_) {
        if (!taskqueue.empty() && name != taskqueue) continue;
        for (const auto& job : tq.jobs) {
            if (!root.empty() && keep.find(job.id) == keep.end()) continue;

            std::vector<std::string> waiting;
            bool allSat = true;
            for (const auto& d : job.dependsOn) {
                const TaskQueueJob* dep = FindJobConst(d.id);
                if (!dep || !IsDepSatisfied(*dep, d.on)) {
                    allSat = false;
                    waiting.push_back(d.id);
                }
            }
            bool ready = (job.state == "pending") && allSat && job.retryAtMs <= now;
            std::string resourceWait = ResourceWaitLocked(job, allSat, now);
            std::vector<std::string> dependents;
            for (const auto& [n2, tq2] : queues_) {
                (void)n2;
                for (const auto& j2 : tq2.jobs) {
                    for (const auto& d2 : j2.dependsOn) {
                        if (d2.id == job.id) { dependents.push_back(j2.id); break; }
                    }
                }
            }

            std::string jj = JobToJson(job, false);
            if (!jj.empty() && jj.back() == '}') jj.pop_back();
            if (!first) oss << ",";
            first = false;
            oss << jj << ",\"ready\":" << (ready ? "true" : "false")
                << ",\"waiting_for\":" << StrArrayToJson(waiting)
                << ",\"resource_wait\":";
            if (resourceWait.empty()) oss << "null";
            else oss << "\"" << JsonEscape(resourceWait) << "\"";
            // Seconds until a pending job may start again (retryDelaySec or a deferral).
            oss << ",\"retry_in_sec\":";
            if (job.state == "pending" && job.retryAtMs > now) oss << (job.retryAtMs - now + 999) / 1000;
            else oss << "null";
            oss << ",\"dependents\":" << StrArrayToJson(dependents) << "}";
        }
    }
    oss << "]}";
    return oss.str();
}

} // namespace pc

namespace pc {

static uint64_t NowFiletime() {
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

using RemoteFilesCheckCallback = std::function<bool(
    const std::string& host,
    const std::string& cwd,
    int remoteProcessId,
    const std::vector<std::string>& files,
    uint64_t startFiletime,
    std::string& outDetail)>;

static RemoteFilesCheckCallback g_remoteFilesCheckCb;

void SetRemoteFilesCheckCallback(RemoteFilesCheckCallback cb) {
    g_remoteFilesCheckCb = std::move(cb);
}

bool TaskQueueManager::EvaluateSuccess(const TaskQueueJob& job, TaskQueueAttempt& attempt,
                                       const std::string& endKind, int exitCode,
                                       const std::string& logFile, std::string& detail) {
    if (endKind != "completed") {
        detail = endKind;
        return false;
    }
    if (exitCode != job.success.exitCode) {
        detail = "exit code " + std::to_string(exitCode) + " (expected " + std::to_string(job.success.exitCode) + ")";
        return false;
    }
    // filesNewer names paths on the machine that ran the job. For a remote job those
    // paths are evaluated on the remote host (doc/design-v3-todo.md 6, card 057).
    if (!job.host.empty()) {
        if (!job.success.filesNewer.empty()) {
            bool evaluatedFromDone = false;
            std::wstring donePath = GetAttemptDonePathWide(job.id, attempt.n);
            std::string doneContent = ReadFileUtf8Wide(donePath);
            if (!doneContent.empty()) {
                nlohmann::json d = nlohmann::json::parse(doneContent, nullptr, false);
                if (d.is_object() && d.contains("files_newer") && d["files_newer"].is_object()) {
                    const auto& fn = d["files_newer"];
                    bool ok = fn.value("success", false);
                    if (!ok) {
                        detail = fn.value("detail", std::string("file not newer than attempt start"));
                        return false;
                    }
                    evaluatedFromDone = true;
                }
            }

            if (!evaluatedFromDone) {
                if (!g_remoteFilesCheckCb) {
                    detail = "remote files check is not available";
                    return false;
                }
                if (!g_remoteFilesCheckCb(job.host, job.cwd, attempt.remoteProcessId,
                                          job.success.filesNewer, attempt.startFiletime, detail)) {
                    return false;
                }
            }
        }
    } else {
        for (const auto& rel : job.success.filesNewer) {
            std::wstring pathW = ResolveJobPathWide(job, rel);
            if (!PathExistsWide(pathW)) {
                detail = "missing file: " + rel;
                return false;
            }
            uint64_t fileTime = 0;
            if (!GetFileMtimeWide(pathW, fileTime)) {
                detail = "cannot read mtime: " + rel;
                return false;
            }
            if (attempt.startFiletime > 0 && fileTime <= attempt.startFiletime) {
                detail = "file not newer than attempt start: " + rel;
                return false;
            }
        }
    }
    if (!job.success.outputRegex.empty()) {
        std::string text;
        if (!logFile.empty()) text = ReadFileUtf8Wide(Utf8ToWide(logFile));
        text = GetTailLines(text, 200);
        try {
            std::regex re(job.success.outputRegex);
            if (!std::regex_search(text, re)) {
                detail = "output_regex did not match";
                return false;
            }
        } catch (const std::regex_error&) {
            detail = "invalid output_regex";
            return false;
        }
    }
    if (!job.success.testReport.empty() && job.host.empty()) {
        const std::string& rel = job.success.testReport;
        std::wstring pathW = ResolveJobPathWide(job, rel);
        uint64_t fileTime = 0;
        if (!PathExistsWide(pathW)) {
            detail = "test report missing: " + rel;
            return false;
        }
        if (GetFileMtimeWide(pathW, fileTime) && attempt.startFiletime > 0 && fileTime <= attempt.startFiletime) {
            detail = "test report not newer than attempt start: " + rel;
            return false;
        }
        TestReportSummary sum;
        if (!ParseTestReport(ReadFileUtf8Wide(pathW), sum)) {
            detail = "test report has no test cases: " + rel;
            return false;
        }
        if (sum.failed > 0) {
            detail = "test report: " + std::to_string(sum.failed) + " of " + std::to_string(sum.total) +
                     " tests failed";
            for (size_t i = 0; i < sum.failedNames.size(); ++i) {
                detail += (i == 0 ? ": " : ", ") + sum.failedNames[i];
            }
            if (static_cast<int>(sum.failedNames.size()) < sum.failed) detail += ", ...";
            return false;
        }
    }
    detail = "ok";
    return true;
}

TaskQueueRetryDecision DecideTaskQueueRetry(const TaskQueueJob& job, const std::string& failureClass,
                                            int countedAttempts, bool allowRateLimitWait) {
    auto listed = [&failureClass](const std::vector<std::string>& classes) {
        for (const auto& c : classes) {
            if (LowerAscii(c) == LowerAscii(failureClass)) return true;
        }
        return false;
    };

    TaskQueueRetryDecision out;
    if (!job.retry.configured) {
        out.action = countedAttempts < job.maxAttempts ? TaskQueueRetryAction::Retry
                                                       : TaskQueueRetryAction::GiveUp;
        return out;
    }
    if (listed(job.retry.never)) {
        out.action = TaskQueueRetryAction::GiveUp;
        return out;
    }
    if (allowRateLimitWait && LowerAscii(failureClass) == "rate_limited" && job.retry.waitForRateLimit) {
        out.action = TaskQueueRetryAction::WaitForRateLimit;
        out.consumesAttempt = false;
        return out;
    }
    bool allowed = job.retry.on.empty() || listed(job.retry.on);
    out.action = allowed && countedAttempts < job.maxAttempts ? TaskQueueRetryAction::Retry
                                                             : TaskQueueRetryAction::GiveUp;
    return out;
}

void TaskQueueManager::ApplyAttemptOutcome(TaskQueue& tq, TaskQueueJob& job, bool success,
                                           const std::string& reason, int exitCode, const std::string& checkDetail,
                                           const TaskQueueFailureInput* failureInput) {
    if (job.attempts.empty()) return;
    TaskQueueAttempt& a = job.attempts.back();
    a.end = GetCurrentTimestampStr();
    a.reason = reason;
    a.exitCode = exitCode;
    a.checkDetail = checkDetail;
    job.updatedAt = a.end;

    if (job.state == "cancelled") {
        SaveQueue(tq);
        cv_.notify_all();
        return;
    }

    if (success) {
        job.state = "succeeded";
        job.blockedBy.clear();
        job.retryAtMs = 0;
        EmitEventLocked(tq, "jobSucceeded", job.id, a.n);
        ReevaluateAll();
        SaveAllLocked();
        CheckQueueNotices(tq);
        if (schedulerRunning_.load()) {
            pendingFinished_.push_back({job, "succeeded"});
        }
        cv_.notify_all();
        return;
    }

    // A deferral exit code (defer.exitCodes, 75 by default) means the command did not run its
    // work and asks to be started again later: back to pending after defer.delaySec, without
    // using up max_attempts, until defer.limit deferrals have happened.
    // The process exited on its own ("failed": non-zero exit, "checkFailed": exit code other
    // than success.exitCode); timeouts, kills and launch failures are never deferrals.
    if ((reason == "failed" || reason == "checkFailed") &&
        std::find(job.defer.exitCodes.begin(), job.defer.exitCodes.end(), exitCode) != job.defer.exitCodes.end()) {
        int deferrals = 0;
        for (const auto& att : job.attempts) {
            if (att.reason == "deferred") deferrals++;
        }
        if (job.defer.limit == 0 || deferrals < job.defer.limit) {
            a.reason = "deferred";
            job.state = "pending";
            job.retryAtMs = static_cast<int64_t>(GetNowTickMs()) + static_cast<int64_t>(job.defer.delaySec) * 1000;
            EmitEventLocked(tq, "jobDeferred", job.id, a.n);
            SaveAllLocked();
            CheckQueueNotices(tq);
            cv_.notify_all();
            return;
        }
    }

    // Classify only real failures: a deferral did not run the job's work and is not one.
    if (!success && failureInput) {
        a.failure = ClassifyTaskQueueFailure(*failureInput);
    }

    // An attempt cut short by a Hancho restart is not the job's fault,
    // so it does not use up max_attempts (spec: interrupted jobs are re-run).
    // A deferred attempt did not run the job's work either.
    int countedAttempts = 0;
    for (const auto& att : job.attempts) {
        if (att.reason != "interrupted" && att.reason != "deferred") countedAttempts++;
    }

    // rate_limited waits like a deferral: pending until the reset without using up
    // max_attempts, until defer.limit deferrals have happened.
    TaskQueueRetryDecision decision =
        DecideTaskQueueRetry(job, a.failure.failureClass, countedAttempts);
    if (decision.action == TaskQueueRetryAction::WaitForRateLimit) {
        int deferrals = 0;
        for (const auto& att : job.attempts) {
            if (att.reason == "deferred") deferrals++;
        }
        if (job.defer.limit == 0 || deferrals < job.defer.limit) {
            a.reason = "deferred";
            job.state = "pending";
            int64_t nowMs = static_cast<int64_t>(GetNowTickMs());
            int64_t resets = a.failure.detail.value("resetsAtEpochSec", static_cast<int64_t>(0));
            job.retryAtMs = resets > 0 ? (resets + 30) * 1000
                                       : nowMs + static_cast<int64_t>(job.defer.delaySec) * 1000;
            EmitEventLocked(tq, "jobDeferred", job.id, a.n, &a.failure);
            SaveAllLocked();
            CheckQueueNotices(tq);
            cv_.notify_all();
            return;
        }
        // The deferral limit is used up: treat it as an ordinary failure from here.
        decision = DecideTaskQueueRetry(job, a.failure.failureClass, countedAttempts, false);
    }
    if (reason == "interrupted") {
        decision = TaskQueueRetryDecision();
        decision.action = countedAttempts < job.maxAttempts ? TaskQueueRetryAction::Retry
                                                           : TaskQueueRetryAction::GiveUp;
    }

    EmitEventLocked(tq, "jobAttemptFailed", job.id, a.n, &a.failure);
    if (decision.action == TaskQueueRetryAction::Retry) {
        job.state = "pending";
        job.retryAtMs = static_cast<int64_t>(GetNowTickMs()) + static_cast<int64_t>(job.retryDelaySec) * 1000;
    } else {
        job.state = "failed";
        job.retryAtMs = 0;
        EmitEventLocked(tq, "jobFailed", job.id, a.n, &a.failure);
        ReevaluateAll();
        if (schedulerRunning_.load()) {
            pendingFinished_.push_back({job, "failed"});
        }
    }
    SaveAllLocked();
    CheckQueueNotices(tq);
    cv_.notify_all();
}

} // namespace pc

namespace pc {

static std::wstring GetTaskQueueLogDirWide() {
    std::wstring dir = PathJoinWide(GetLogsDirectoryWide(), L"taskqueue");
    EnsureDirectoryWide(dir);
    return dir;
}

static std::wstring GetAttemptLogPathWide(const std::string& jobId, int n) {
    return PathJoinWide(GetTaskQueueLogDirWide(), Utf8ToWide(SanitizeFileNameComponent(jobId))) +
           L"." + std::to_wstring(n) + L".log";
}

static std::wstring GetAttemptEventsPathWide(const std::string& jobId, int n) {
    return PathJoinWide(GetTaskQueueLogDirWide(), Utf8ToWide(SanitizeFileNameComponent(jobId))) +
           L"." + std::to_wstring(n) + L".events.jsonl";
}

static std::wstring GetAttemptDonePathWide(const std::string& jobId, int n) {
    return PathJoinWide(GetTaskQueueLogDirWide(), Utf8ToWide(SanitizeFileNameComponent(jobId))) +
           L"." + std::to_wstring(n) + L".done";
}

std::wstring TaskQueueManager::GetAttemptLogFileWide(const std::string& jobId, int n) {
    return GetAttemptLogPathWide(jobId, n);
}

static std::string QuoteCommandArgLocal(const std::string& s) {
    return platform::QuoteCommandArg(s);
}

bool TaskQueueManager::BuildLaunchRequest(TaskQueue& tq, TaskQueueJob& job, int attemptN, LaunchItem& out) {
    out.taskqueue = tq.name;
    out.jobId = job.id;
    out.attemptN = attemptN;

    std::string instrPathUtf8;
    int rev = job.instructions.rev > 0 ? job.instructions.rev : 1;
    std::string instructionText = job.instructions.text;
    if (job.memory.inject) {
        memory::BriefOptions bo;
        bo.jobName = job.name.empty() ? job.id : job.name;
        bo.taskqueue = tq.name;
        bo.budgetChars = job.memory.budgetChars;
        bo.scopes = job.memory.scopes;
        bo.includeAgentNotes = job.memory.includeAgentNotes;
        for (const auto* list : { &job.resources.write, &job.resources.read }) {
            for (const auto& path : *list) {
                if (path.empty() || path[0] == '@') continue;
                if (path.find('*') != std::string::npos || path.find('?') != std::string::npos) continue;
                bo.files.push_back(path);
            }
        }
        memory::BriefResult brief = memory::BuildBrief(bo);
        if (brief.ok) {
            out.memoryRefs = brief.ids;
            const std::string injected = brief.ids.empty() ? std::string() : brief.text;
            bool expanded = false;
            size_t pos = 0;
            while ((pos = instructionText.find("{memory}", pos)) != std::string::npos) {
                instructionText.replace(pos, 8, injected);
                pos += injected.size();
                expanded = true;
            }
            if (!expanded && !injected.empty()) {
                if (!instructionText.empty()) instructionText += "\n\n";
                instructionText += injected;
            }
            job.instructions.text = instructionText;
        }
    }
    if (!instructionText.empty()) {
        std::wstring instrW = GetInstructionsFileWide(tq.name, job.id, rev);
        WriteFileAtomicWide(instrW, instructionText);
        instrPathUtf8 = WideToUtf8(instrW);
    }

    std::string cmd = job.command;
    if (job.host.empty()) {
        size_t pos = 0;
        while ((pos = cmd.find("{instructions}", pos)) != std::string::npos) {
            cmd.replace(pos, 14, instrPathUtf8);
            pos += instrPathUtf8.size();
        }
    }
    // A remote job sends the instruction text instead of a local path; the
    // remote host writes it under its own data directory and expands the
    // placeholder there.
    size_t pos = 0;
    std::string quotedSummary = QuoteCommandArgLocal(job.summary);
    while ((pos = cmd.find("{summary}", pos)) != std::string::npos) {
        cmd.replace(pos, 9, quotedSummary);
        pos += quotedSummary.size();
    }

    StartProcessRequest req;
    req.name = job.name.empty() ? job.id : job.name;
    req.commandLine = cmd;
    req.workingDir = job.cwd;
    req.mode = ExecMode::Pipe;
    req.openStdin = false;
    req.timeoutSec = job.timeoutSec;
    req.logFile = WideToUtf8(GetAttemptLogPathWide(job.id, attemptN));
    req.jobKey = job.id;
    req.jobId = job.num;
    req.attemptN = attemptN;
    if (!job.host.empty() && !instructionText.empty()) {
        req.instructionsText = instructionText;
        req.instructionsRev = rev;
    }
    req.supervisorGoal = job.summary;
    req.supervisorNotes = "Task ID: " + job.id + (job.summary.empty() ? "" : " - " + job.summary);
    req.agentType = DetectAgentType(req.commandLine, req.name);
    if (!req.agentType.empty()) {
        req.jsonStream = true;
    }
    req.env = job.env;
    auto adapterIt = req.env.find("HANCHO_ADAPTER");
    if (adapterIt != req.env.end()) {
        if (!adapterIt->second.empty()) {
            req.agentType = adapterIt->second;
            req.jsonStream = true;
        }
        req.env.erase(adapterIt);
    }
    req.killExclude = job.killExclude;
    if (job.idle.outputSec > 0 && job.idle.cpuSec > 0) {
        req.idleTimeoutSec = job.idle.outputSec;
        req.idleCpuSec = job.idle.cpuSec;
    }

    req.env["PC_JOB_ID"] = job.id;
    req.env["PC_ATTEMPT"] = std::to_string(attemptN);
    req.env["PC_JOB_SUMMARY"] = job.summary;
    if (job.host.empty() && !instrPathUtf8.empty()) {
        req.env["PC_JOB_INSTRUCTIONS"] = instrPathUtf8;
        req.env["PC_JOB_INSTRUCTIONS_REV"] = std::to_string(rev);
    }
    if (job.attempts.size() >= 2) {
        const TaskQueueAttempt& prev = job.attempts[job.attempts.size() - 2];
        req.env["PC_PREV_REASON"] = prev.reason;
        req.env["PC_PREV_EXIT"] = std::to_string(prev.exitCode);
        if (job.host.empty() && !prev.lastLinesFile.empty()) req.env["PC_PREV_TAIL"] = prev.lastLinesFile;
    }

    out.req = std::move(req);
    return true;
}

} // namespace pc

namespace pc {

void TaskQueueManager::ProcessSchedulerTick() {
    PollRemoteJobs();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pm_) return;
        int64_t now = static_cast<int64_t>(GetNowTickMs());

        // Ready jobs of all queues in one global order (priority, then registration order), so jobs
        // that share a resource start in that order even when they live in different queues.
        struct Candidate { TaskQueue* tq; TaskQueueJob* job; };
        std::vector<Candidate> ready;
        std::map<std::string, int> running;
        for (auto& [name, tq] : queues_) {
            int r = 0;
            for (const auto& j : tq.jobs) {
                if (j.state == "running") r++;
            }
            running[name] = r;
            if (tq.paused) continue;
            for (auto& j : tq.jobs) {
                if (IsReadyLocked(j, now)) ready.push_back({ &tq, &j });
            }
        }
        std::stable_sort(ready.begin(), ready.end(), [](const Candidate& a, const Candidate& b) {
            return RunsBefore(*a.job, *b.job);
        });

        // A job that has to wait (full queue or a running conflict) keeps its place: later jobs that
        // conflict with it wait behind it instead of overtaking it.
        std::vector<const TaskQueueJob*> waiters;
        bool locks = resourceLocksEnabled_.load();
        bool changed = false;
        for (const auto& c : ready) {
            TaskQueue& tq = *c.tq;
            TaskQueueJob& job = *c.job;
            std::string holder, resource;
            bool wait = running[tq.name] >= tq.concurrency ||
                        FindRunningResourceConflictLocked(job, holder, resource);
            for (size_t w = 0; !wait && locks && w < waiters.size(); ++w) {
                wait = FindResourceConflict(job.resources, job.cwd, job.host,
                                            waiters[w]->resources, waiters[w]->cwd,
                                            waiters[w]->host, resource);
            }
            if (wait) {
                if (!job.resources.empty()) waiters.push_back(&job);
                continue;
            }
            int n = static_cast<int>(job.attempts.size()) + 1;
            LaunchItem item;
            BuildLaunchRequest(tq, job, n, item);
            TaskQueueAttempt a;
            a.n = n;
            a.start = GetCurrentTimestampStr();
            a.instructionsRev = job.instructions.rev;
            a.memoryRefs = item.memoryRefs;
            a.startFiletime = NowFiletime();
            job.attempts.push_back(a);
            job.state = "running";
            job.updatedAt = a.start;
            EmitEventLocked(tq, "jobStarted", job.id, n);
            job.attempts.back().startEventSeq = static_cast<uint64_t>(tq.version);

            std::string err;
            int pid = 0;
            if (!job.host.empty()) {
                if (!remoteStartCb_) {
                    err = "job '" + job.id + "' targets host '" + job.host +
                          "' but remote dispatch is not available";
                } else {
                    // Local paths in the request belong to this machine; the remote
                    // host picks its own log and done files.
                    StartProcessRequest remoteReq = item.req;
                    remoteReq.logFile.clear();
                    remoteReq.doneFile.clear();
                    pid = remoteStartCb_(job.host, remoteReq, err);
                    if (pid > 0) {
                        job.attempts.back().remoteHost = job.host;
                        job.attempts.back().remoteProcessId = pid;
                        job.attempts.back().remoteJobId = job.id;
                        job.attempts.back().remoteAttempt = n;
                        running[tq.name]++;
                    }
                }
            } else {
                pid = pm_->StartProcess(item.req, err);
                if (pid > 0) {
                    job.attempts.back().processId = pid;
                    running[tq.name]++;
                }
            }
            if (pid <= 0) {
                TaskQueueFailureInput fin;
                FillFailureInputBase(fin, job, job.attempts.back());
                fin.launchFailed = true;
                fin.endKind = "failed";
                fin.exitCode = -1;
                fin.checkDetail = err;
                fin.logFile = WideToUtf8(GetAttemptLogPathWide(job.id, n));
                fin.fallbackEventsFile = WideToUtf8(GetTaskEventsFileWide(tq.name));
                fin.fallbackEventSeq = job.attempts.back().startEventSeq;
                ApplyAttemptOutcome(tq, job, false, "launchFailed", -1, err, &fin);
            }
            changed = true;
        }
        if (changed) SaveAllLocked();
    }
    FlushFinishedCallbacks();
}

void TaskQueueManager::ReconnectRemoteJobs() {
    struct Target {
        std::string host;
        int remoteId = 0;
        std::string jobId;
        int attemptN = 0;
    };
    std::vector<Target> targets;
    RemotePollCallback poll;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!remotePollCb_) return;
        poll = remotePollCb_;
        for (auto& [name, tq] : queues_) {
            (void)name;
            for (auto& job : tq.jobs) {
                if (job.state != "running" || job.attempts.empty()) continue;
                TaskQueueAttempt& a = job.attempts.back();
                if (!a.needsRemoteReconnect) continue;
                targets.push_back({a.remoteHost, a.remoteProcessId, job.id, a.n});
            }
        }
    }
    if (targets.empty()) return;

    for (const auto& target : targets) {
        RemotePollResult result;
        std::string err;
        bool ok = poll(target.host, target.remoteId, result, err);

        std::lock_guard<std::mutex> lock(mutex_);
        TaskQueue* tq = nullptr;
        TaskQueueJob* job = FindJob(target.jobId, &tq);
        if (!job || !tq || job->state != "running" || job->attempts.empty()) continue;
        TaskQueueAttempt& a = job->attempts.back();
        if (a.n != target.attemptN || a.remoteProcessId != target.remoteId) continue;

        if (!ok) {
            TaskQueueFailureInput fin;
            FillFailureInputBase(fin, *job, a);
            fin.interrupted = true;
            fin.endKind = "killed";
            fin.exitCode = -1;
            fin.checkDetail = "Remote reconnect failed: " + (err.empty() ? "host unreachable" : err);
            std::wstring logPath = GetAttemptLogPathWide(job->id, a.n);
            fin.logFile = WideToUtf8(logPath);
            FillFailureLog(fin, ReadFileUtf8Wide(logPath));
            fin.fallbackEventsFile = WideToUtf8(GetTaskEventsFileWide(tq->name));
            fin.fallbackEventSeq = static_cast<uint64_t>(tq->version + 1);
            ApplyAttemptOutcome(*tq, *job, false, "interrupted", -1, fin.checkDetail, &fin);
            AppendRunRecordForAttempt(*tq, *job, a, fin, ReadFileUtf8Wide(logPath));
            ReevaluateAll();
            SaveQueue(*tq);
            WriteAuditLog("REMOTE_RECONNECT_FAILED", target.host + " job " + target.jobId + ": " + err);
        } else {
            a.needsRemoteReconnect = false;
            WriteAuditLog("REMOTE_RECONNECT", target.host + " job " + target.jobId + " reconnected");
            if (result.finished) {
                FinishRemoteAttemptLocked(*tq, *job, result);
            }
            SaveQueue(*tq);
        }
    }
    FlushFinishedCallbacks();
}

void TaskQueueManager::PollRemoteJobs() {
    ReconnectRemoteJobs();
    struct Target {
        std::string host;
        int remoteId = 0;
        std::string jobId;
        int attemptN = 0;
    };
    std::vector<Target> targets;
    RemotePollCallback poll;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!remotePollCb_) return;
        poll = remotePollCb_;
        const int64_t now = static_cast<int64_t>(GetNowTickMs());
        for (auto& [name, tq] : queues_) {
            (void)name;
            for (auto& job : tq.jobs) {
                if (job.state != "running" || job.attempts.empty()) continue;
                TaskQueueAttempt& a = job.attempts.back();
                if (a.remoteHost.empty() || a.remoteProcessId <= 0) continue;
                // Keep the remote host from being polled several times per second.
                if (a.lastRemotePollMs != 0 && now - static_cast<int64_t>(a.lastRemotePollMs) < 750) {
                    continue;
                }
                a.lastRemotePollMs = static_cast<uint64_t>(now);
                targets.push_back({a.remoteHost, a.remoteProcessId, job.id, a.n});
            }
        }
    }

    for (const auto& target : targets) {
        RemotePollResult result;
        std::string err;
        if (!poll(target.host, target.remoteId, result, err)) {
            if (!err.empty()) {
                WriteAuditLog("REMOTE_POLL", target.host + " job " + target.jobId + ": " + err);
            }
            continue;
        }
        if (!result.finished) continue;

        std::lock_guard<std::mutex> lock(mutex_);
        TaskQueue* tq = nullptr;
        TaskQueueJob* job = FindJob(target.jobId, &tq);
        if (!job || !tq || job->state != "running" || job->attempts.empty()) continue;
        TaskQueueAttempt& a = job->attempts.back();
        if (a.n != target.attemptN || a.remoteProcessId != target.remoteId) continue;
        FinishRemoteAttemptLocked(*tq, *job, result);
        SaveAllLocked();
    }
    FlushFinishedCallbacks();
}

bool TaskQueueManager::FinishRemoteAttemptLocked(TaskQueue& tq, TaskQueueJob& job,
                                                 const RemotePollResult& result) {
    if (job.attempts.empty()) return false;
    TaskQueueAttempt& a = job.attempts.back();

    const std::wstring logPath = GetAttemptLogPathWide(job.id, a.n);
    if (!result.output.empty()) {
        WriteFileAtomicWide(logPath, result.output);
    }
    const std::string logText = ReadFileUtf8Wide(logPath);

    const std::wstring eventsPath = GetAttemptEventsPathWide(job.id, a.n);
    if (!result.eventsJsonl.empty()) {
        WriteFileAtomicWide(eventsPath, result.eventsJsonl);
    }
    if (!result.doneContent.empty()) {
        WriteFileAtomicWide(GetAttemptDonePathWide(job.id, a.n), result.doneContent);
    }

    if (!logText.empty()) {
        std::wstring tailPath = PathJoinWide(
            GetTaskQueueLogDirWide(),
            Utf8ToWide(SanitizeFileNameComponent(job.id)) + L"." + std::to_wstring(a.n) + L".tail.txt");
        WriteFileAtomicWide(tailPath, GetTailLines(logText, 50));
        a.lastLinesFile = WideToUtf8(tailPath);
    }

    a.end = GetCurrentTimestampStr();
    a.exitCode = result.exitCode;
    job.updatedAt = a.end;

    TaskQueueFailureInput fin;
    FillFailureInputBase(fin, job, a);
    fin.endKind = result.endKind.empty() ? "completed" : result.endKind;
    fin.exitCode = result.exitCode;
    fin.logFile = WideToUtf8(logPath);
    FillFailureLog(fin, logText);
    if (!result.eventsJsonl.empty()) {
        fin.eventsFile = WideToUtf8(eventsPath);
        fin.events = ReadHanchoEventsFile(fin.eventsFile, 500, "", 0);
    }
    fin.fallbackEventsFile = WideToUtf8(GetTaskEventsFileWide(tq.name));
    fin.fallbackEventSeq = static_cast<uint64_t>(tq.version + 1);

    std::string detail;
    const bool ok = EvaluateSuccess(job, a, fin.endKind, result.exitCode, WideToUtf8(logPath), detail);
    fin.success = ok;
    fin.checkDetail = detail;

    std::string reason;
    if (ok) reason = "ok";
    else if (fin.endKind == "completed") reason = "checkFailed";
    else reason = fin.endKind;

    ApplyAttemptOutcome(tq, job, ok, reason, result.exitCode, detail, ok ? nullptr : &fin);
    AppendRunRecordForAttempt(tq, job, a, fin, logText);
    return true;
}

void TaskQueueManager::SchedulerLoop() {
    while (schedulerRunning_.load()) {
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait_for(lock, std::chrono::milliseconds(300));
        }
        if (!schedulerRunning_.load()) break;
        ProcessSchedulerTick();
    }
}

// One L1 RunRecord per finished attempt (doc/design-v3.md 3.3). The record is built from
// the same taskqueue facts and .events.jsonl that `memory rebuild` uses, so a live record
// and a rebuilt one are identical.
static void AppendRunRecordForAttempt(const TaskQueue& tq, const TaskQueueJob& job,
                                      const TaskQueueAttempt& attempt,
                                      const TaskQueueFailureInput& fin,
                                      const std::string& logText) {
    memory::RunRecordInput in;
    in.jobId = job.id;
    in.taskqueue = tq.name;
    in.jobName = job.name;
    in.summary = job.summary;
    in.instructions = job.instructions.text;
    in.command = job.command;
    in.cwd = job.cwd;
    in.btId = job.bt.id;
    in.btNode = job.bt.node;
    in.btRun = job.bt.run;
    in.attempt = attempt.n;
    in.originClient = job.origin.client;
    in.originSession = job.origin.session;
    in.originCwd = job.origin.cwd;
    in.start = attempt.start;
    in.end = attempt.end;
    in.exitCode = attempt.exitCode;
    in.reason = attempt.reason;
    in.jobState = job.state;
    in.failureClass = attempt.failure.failureClass;
    in.failureSummary = attempt.failure.summary;
    in.checkDetail = attempt.checkDetail;
    in.timeoutSec = job.timeoutSec;
    in.processId = attempt.processId;
    in.eventsFile = fin.eventsFile;
    in.logFile = fin.logFile;
    in.lastLinesFile = attempt.lastLinesFile;
    in.logText = logText;
    memory::AppendRunRecordJson(in);
    CheckAndRecordAutoNotes(job, attempt, fin);
}

namespace {

struct AutoNoteRun {
    std::string id;
    std::string jobId;
    int attempt = 1;
    std::string jobName;
    std::string outcome;
    int64_t exitCode = 0;
    std::string failureClass;
    std::string failureSummary;
    std::string condition;
    int64_t durationSec = 0;
    int timeoutSec = 0;
    std::vector<std::string> filesEdited;
    std::string startIso;
    int64_t startEpoch = 0;
    bool success = false;
};

static bool AutoNoteIEquals(const std::string& a, const std::string& b) {
    return LowerAscii(a) == LowerAscii(b);
}

static double AutoNotePercentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) return 0.0;
    if (sorted.size() == 1) return sorted.front();
    size_t index = static_cast<size_t>(std::ceil(p * static_cast<double>(sorted.size()))) - 1;
    if (index >= sorted.size()) index = sorted.size() - 1;
    return sorted[index];
}

static std::vector<AutoNoteRun> LoadRunsForJob(const std::string& jobName) {
    std::vector<AutoNoteRun> runs;
    std::error_code ec;
    std::filesystem::path recDir = std::filesystem::path(GetEffectiveDataDirectoryWide()) / L"memory" / L"records";
    if (!std::filesystem::is_directory(recDir, ec)) return runs;

    for (const auto& ent : std::filesystem::directory_iterator(recDir, ec)) {
        if (ec) break;
        if (!ent.is_regular_file(ec) || ent.path().extension() != L".jsonl") continue;
        std::ifstream in(ent.path(), std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
            if (j.is_discarded() || !j.is_object()) continue;
            std::string jName = j.value("jobName", "");
            if (!AutoNoteIEquals(jName, jobName)) continue;

            AutoNoteRun r;
            r.id = j.value("id", "");
            r.jobId = j.value("jobId", "");
            r.attempt = static_cast<int>(j.value("attempt", 1));
            r.jobName = jName;
            r.outcome = j.value("outcome", "");
            r.exitCode = j.value("exitCode", 0);
            r.durationSec = j.value("durationSec", 0);
            r.timeoutSec = static_cast<int>(j.value("timeoutSec", 0));
            r.startIso = j.value("start", j.value("createdAt", j.value("end", "")));
            r.startEpoch = memory_inject_detail::ParseIsoEpoch(r.startIso);

            auto failureIt = j.find("failure");
            if (failureIt != j.end() && failureIt->is_object()) {
                r.failureClass = failureIt->value("class", "");
                r.failureSummary = failureIt->value("summary", "");
            }
            if (r.failureClass.empty()) {
                r.failureClass = j.value("failureClass", "");
            }

            if (AutoNoteIEquals(r.failureClass, "success_check_failed")) {
                if (r.failureSummary.find("filesNewer") != std::string::npos ||
                    r.failureSummary.find("file not newer") != std::string::npos ||
                    r.failureSummary.find("missing file:") != std::string::npos ||
                    r.failureSummary.find("cannot read mtime") != std::string::npos) {
                    r.condition = "filesNewer";
                } else if (r.failureSummary.find("output_regex") != std::string::npos ||
                           r.failureSummary.find("outputRegex") != std::string::npos) {
                    r.condition = "outputRegex";
                } else if (r.failureSummary.find("test report") != std::string::npos ||
                           r.failureSummary.find("testReport") != std::string::npos) {
                    r.condition = "testReport";
                } else if (r.failureSummary.find("exit code") != std::string::npos ||
                           r.failureSummary.find("exitCode") != std::string::npos) {
                    r.condition = "exitCode";
                }
            }

            auto feIt = j.find("filesEdited");
            if (feIt != j.end() && feIt->is_array()) {
                for (const auto& item : *feIt) {
                    if (item.is_string()) r.filesEdited.push_back(item.get<std::string>());
                }
            }

            std::string lo = LowerAscii(r.outcome);
            bool hasFailure = !r.failureClass.empty() || (failureIt != j.end() && !failureIt->empty());
            r.success = !hasFailure && (lo == "success" || lo == "succeeded" || lo == "completed" || lo == "ok" || lo == "passed" || (lo.empty() && r.exitCode == 0));
            runs.push_back(std::move(r));
        }
    }

    std::sort(runs.begin(), runs.end(), [](const AutoNoteRun& a, const AutoNoteRun& b) {
        if (a.startEpoch != b.startEpoch) return a.startEpoch < b.startEpoch;
        if (a.startIso != b.startIso) return a.startIso < b.startIso;
        return a.id < b.id;
    });

    return runs;
}

} // namespace

int CheckAndRecordAutoNotes(const TaskQueueJob& job, const TaskQueueAttempt& attempt,
                           const TaskQueueFailureInput& fin) {
    if (job.name.empty()) return 0;
    auto runs = LoadRunsForJob(job.name);
    const size_t N = runs.size();
    if (N == 0) return 0;

    // Augment the latest run with condition or timeout if missing from on-disk JSON
    if (runs.back().condition.empty() && AutoNoteIEquals(runs.back().failureClass, "success_check_failed")) {
        std::string cond = SuccessConditionOf(fin.checkDetail);
        if (cond.empty()) cond = SuccessConditionOf(attempt.checkDetail);
        if (!cond.empty()) runs.back().condition = cond;
    }
    if (runs.back().timeoutSec <= 0 && job.timeoutSec > 0) {
        runs.back().timeoutSec = job.timeoutSec;
    }

    int notesCreated = 0;

    // Rule 1: Same job name failed 3 times consecutively with the same failure classification.
    // "同じジョブ名が同じ失敗分類で 3 回続いたとき、「build は直近 3 回 success_check_failed（filesNewer）で失敗」のようなノートを書く。"
    // "同じノートを繰り返し作らない（条件が継続している間は 1 度だけ出す）。"
    if (N >= 3) {
        const auto& r1 = runs[N - 3];
        const auto& r2 = runs[N - 2];
        const auto& r3 = runs[N - 1];
        if (!r1.success && !r2.success && !r3.success &&
            !r3.failureClass.empty() &&
            AutoNoteIEquals(r1.failureClass, r3.failureClass) &&
            AutoNoteIEquals(r2.failureClass, r3.failureClass)) {

            bool alreadyContinuing = false;
            if (N >= 4) {
                const auto& r0 = runs[N - 4];
                if (!r0.success && AutoNoteIEquals(r0.failureClass, r3.failureClass)) {
                    alreadyContinuing = true;
                }
            }

            if (!alreadyContinuing && notesCreated < 5) {
                std::string condition = r3.condition;
                if (condition.empty()) condition = r2.condition;
                if (condition.empty()) condition = r1.condition;

                std::string text = job.name + " は直近 3 回 " + r3.failureClass;
                if (!condition.empty()) {
                    text += "（" + condition + "）";
                }
                text += "で失敗";

                memory::NoteInput note;
                note.text = text;
                note.scope = "job:" + job.name;
                note.authorType = "hancho";
                note.sources = {r1.id, r2.id, r3.id};
                note.tags = {"auto", "failure_streak", r3.failureClass};
                std::string res = memory::NoteJson(note);
                nlohmann::json r = nlohmann::json::parse(res, nullptr, false);
                if (r.value("ok", false) && !r.value("duplicate", false)) {
                    ++notesCreated;
                }
            }
        }
    }

    // Rule 2: Duration exceeds 80% of timeout across consecutive attempts.
    // "所要時間がタイムアウトの 80% を超える試行が続いたとき、p90 と上限を本文にしたノートを書く。"
    // "同じノートを繰り返し作らない（条件が継続している間は 1 度だけ出す）。"
    int effTimeout = job.timeoutSec;
    if (effTimeout <= 0 && runs.back().timeoutSec > 0) effTimeout = runs.back().timeoutSec;

    if (effTimeout > 0 && N >= 2) {
        double threshold = 0.8 * static_cast<double>(effTimeout);
        const auto& curr = runs[N - 1];
        const auto& prev = runs[N - 2];
        if (static_cast<double>(curr.durationSec) >= threshold &&
            static_cast<double>(prev.durationSec) >= threshold) {

            bool alreadyContinuing = false;
            if (N >= 3) {
                const auto& prev2 = runs[N - 3];
                if (static_cast<double>(prev2.durationSec) >= threshold) {
                    alreadyContinuing = true;
                }
            }

            if (!alreadyContinuing && notesCreated < 5) {
                std::vector<double> durations;
                for (const auto& r : runs) {
                    if (r.durationSec > 0) durations.push_back(static_cast<double>(r.durationSec));
                }
                std::sort(durations.begin(), durations.end());
                double p90 = AutoNotePercentile(durations, 0.9);
                int64_t p90Sec = static_cast<int64_t>(p90 + 0.5);

                std::string text = job.name + " は所要時間がタイムアウトに近い（p90 " +
                                   std::to_string(p90Sec) + " 秒 / 上限 " +
                                   std::to_string(effTimeout) + " 秒）";

                memory::NoteInput note;
                note.text = text;
                note.scope = "job:" + job.name;
                note.authorType = "hancho";
                note.sources = {prev.id, curr.id};
                note.tags = {"auto", "timeout_warning"};
                std::string res = memory::NoteJson(note);
                nlohmann::json r = nlohmann::json::parse(res, nullptr, false);
                if (r.value("ok", false) && !r.value("duplicate", false)) {
                    ++notesCreated;
                }
            }
        }
    }

    // Rule 3: Success after failure.
    // "失敗の後に成功したとき、失敗試行と成功試行の filesEdited の差を「直した可能性のあるファイル」としてノートに書く。"
    if (N >= 2) {
        const auto& curr = runs[N - 1];
        const auto& prev = runs[N - 2];
        if (curr.success && !prev.success) {
            std::vector<std::string> diff;
            for (const auto& f : curr.filesEdited) {
                if (std::find(prev.filesEdited.begin(), prev.filesEdited.end(), f) == prev.filesEdited.end()) {
                    diff.push_back(f);
                }
            }
            for (const auto& f : prev.filesEdited) {
                if (std::find(curr.filesEdited.begin(), curr.filesEdited.end(), f) == curr.filesEdited.end()) {
                    if (std::find(diff.begin(), diff.end(), f) == diff.end()) {
                        diff.push_back(f);
                    }
                }
            }

            if (!diff.empty() && notesCreated < 5) {
                std::string fileList;
                for (size_t i = 0; i < diff.size(); ++i) {
                    if (i > 0) fileList += ", ";
                    fileList += diff[i];
                }

                std::string text = job.name + " は失敗の後に成功。直した可能性のあるファイル: " + fileList;

                memory::NoteInput note;
                note.text = text;
                note.scope = "job:" + job.name;
                note.authorType = "hancho";
                note.sources = {prev.id, curr.id};
                note.tags = {"auto", "fix_diff"};
                std::string res = memory::NoteJson(note);
                nlohmann::json r = nlohmann::json::parse(res, nullptr, false);
                if (r.value("ok", false) && !r.value("duplicate", false)) {
                    ++notesCreated;
                }
            }
        }
    }

    return notesCreated;
}

int CheckAndRecordAutoNotes(const std::string& jobName, int timeoutSec) {
    TaskQueueJob dummy;
    dummy.name = jobName;
    dummy.timeoutSec = timeoutSec;
    TaskQueueAttempt a;
    TaskQueueFailureInput fin;
    return CheckAndRecordAutoNotes(dummy, a, fin);
}


bool TaskQueueManager::RecordPlainRunLocked(const PlainRunState& state, std::string& outError) {
    outError.clear();
    std::string qName = state.taskqueue.empty() ? "plain" : state.taskqueue;
    TaskQueue& tq = queues_[qName];
    tq.name = qName;

    std::string jobId = state.jobId;
    if (jobId.empty()) {
        jobId = IssuePlainJobId(state.processId, state.name);
    }

    std::string startTs = !state.start.empty() ? state.start : GetCurrentTimestampStr();
    std::string endTs = !state.end.empty() ? state.end : GetCurrentTimestampStr();
    bool ok = (state.exitCode == 0);

    TaskQueueAttempt a;
    a.n = state.attempt <= 0 ? 1 : state.attempt;
    a.processId = state.processId;
    a.start = startTs;
    a.end = endTs;
    a.exitCode = state.exitCode;
    a.reason = !state.reason.empty() ? state.reason : (ok ? "ok" : "failed");
    a.logFile = state.logFile;
    a.lastLinesFile = state.lastLinesFile;
    a.checkDetail = state.checkDetail;

    auto it = std::find_if(tq.jobs.begin(), tq.jobs.end(), [&](const TaskQueueJob& j) {
        return j.id == jobId;
    });
    if (it != tq.jobs.end()) {
        it->state = ok ? "succeeded" : "failed";
        it->updatedAt = endTs;
        if (!state.command.empty()) it->command = state.command;
        if (!state.cwd.empty()) it->cwd = state.cwd;
        if (!state.summary.empty()) it->summary = state.summary;
        it->attempts.push_back(a);
    } else {
        TaskQueueJob job;
        job.id = jobId;
        job.taskqueue = qName;
        job.name = !state.name.empty() ? state.name : "plain";
        job.summary = state.summary;
        job.command = state.command;
        job.cwd = state.cwd;
        job.timeoutSec = 0;
        job.maxAttempts = 1;
        job.state = ok ? "succeeded" : "failed";
        job.createdAt = startTs;
        job.updatedAt = endTs;
        job.attempts.push_back(a);
        tq.jobs.push_back(std::move(job));
    }

    if (!state.logFile.empty()) {
        std::wstring dstLog = GetAttemptLogPathWide(jobId, a.n);
        std::error_code ec;
        if (!std::filesystem::exists(std::filesystem::path(dstLog), ec) &&
            std::filesystem::exists(std::filesystem::path(Utf8ToWide(state.logFile)), ec)) {
            EnsureDirectoryWide(PathJoinWide(GetLogsDirectoryWide(), L"taskqueue"));
            std::filesystem::copy_file(std::filesystem::path(Utf8ToWide(state.logFile)),
                                       std::filesystem::path(dstLog),
                                       std::filesystem::copy_options::overwrite_existing, ec);
        }
    }

    tq.version++;
    SaveQueue(tq);
    return true;
}

bool TaskQueueManager::RecordPlainRun(const PlainRunState& state, std::string& outError) {
    std::lock_guard<std::mutex> lock(mutex_);
    return RecordPlainRunLocked(state, outError);
}

void TaskQueueManager::OnProcessFinished(int processId, int exitCode, const std::string& endKind) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        bool handled = false;
        for (auto& [name, tq] : queues_) {
            (void)name;
            for (auto& job : tq.jobs) {
                if (job.attempts.empty()) continue;
                TaskQueueAttempt& a = job.attempts.back();
                if (a.processId != processId) continue;
                handled = true;
                if (job.state != "running" && job.state != "cancelled") return;

                std::wstring logPath = GetAttemptLogPathWide(job.id, a.n);
                std::string logText = ReadFileUtf8Wide(logPath);
                if (!logText.empty()) {
                    std::wstring tailPath = PathJoinWide(GetTaskQueueLogDirWide(),
                        Utf8ToWide(SanitizeFileNameComponent(job.id)) + L"." + std::to_wstring(a.n) + L".tail.txt");
                    WriteFileAtomicWide(tailPath, GetTailLines(logText, 50));
                    a.lastLinesFile = WideToUtf8(tailPath);
                }

                // Inputs for the classifier: attempt facts, normalized events and the log tail.
                TaskQueueFailureInput fin;
                FillFailureInputBase(fin, job, a);
                fin.endKind = endKind;
                fin.exitCode = exitCode;
                fin.cancelled = (job.state == "cancelled");
                fin.logFile = WideToUtf8(logPath);
                FillFailureLog(fin, logText);
                fin.fallbackEventsFile = WideToUtf8(GetTaskEventsFileWide(tq.name));
                fin.fallbackEventSeq = static_cast<uint64_t>(tq.version + 1);
                ProcessSnapshot snap;
                if (pm_ && pm_->GetSnapshot(processId, snap)) {
                    fin.elapsedSec = static_cast<int64_t>(snap.elapsedMs / 1000);
                    if (!snap.activity.empty() || !snap.activityTarget.empty()) {
                        fin.lastActivity = snap.activity;
                        fin.lastActivityTarget = snap.activityTarget;
                        if (snap.activitySinceEpochMs > 0) {
                            int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                            fin.lastActivitySinceSec = nowMs > snap.activitySinceEpochMs
                                ? (nowMs - snap.activitySinceEpochMs) / 1000 : 0;
                        }
                    }
                    fin.eventsFile = HanchoEventsFilePathForStream(snap.streamFilePath);
                    if (!fin.eventsFile.empty()) {
                        fin.events = ReadHanchoEventsFile(fin.eventsFile, 500, "", 0);
                    }
                }

                if (job.state == "cancelled") {
                    a.end = GetCurrentTimestampStr();
                    a.reason = "killed";
                    a.exitCode = exitCode;
                    a.checkDetail = "cancelled";
                    job.updatedAt = a.end;
                    fin.checkDetail = "cancelled";
                    a.failure = ClassifyTaskQueueFailure(fin);
                    tq.version++;
                    std::ostringstream evs;
                    evs << "{\"version\":" << tq.version
                        << ",\"seq\":" << tq.version
                        << ",\"type\":\"jobCancelled\",\"taskqueue\":\"" << JsonEscape(tq.name)
                        << "\",\"job_id\":\"" << JsonEscape(job.id)
                        << "\",\"attempt\":" << a.n
                        << ",\"timestamp\":\"" << GetCurrentTimestampStr() << "\"";
                    std::string failureJson = TaskQueueFailureDiagnosticToJson(a.failure);
                    if (!failureJson.empty()) evs << ",\"failure\":" << failureJson;
                    evs << "}\r\n";
                    std::string ev = evs.str();
                    AppendToLogFileWide(GetTaskEventsFileWide(tq.name), ev);
                    if (!tq.notifyFile.empty()) {
                        std::string notify = ev;
                        if (notify.size() >= 2 && notify[notify.size()-2]=='\r' && notify[notify.size()-1]=='\n') notify = notify.substr(0, notify.size()-2);
                        WriteFileAtomicWide(Utf8ToWide(tq.notifyFile), notify);
                    }
                    SaveQueue(tq);
                    CheckQueueNotices(tq);
                    AppendRunRecordForAttempt(tq, job, a, fin, logText);
                    cv_.notify_all();
                    return;
                }

                std::string detail;
                bool ok = EvaluateSuccess(job, a, endKind, exitCode, WideToUtf8(logPath), detail);
                fin.success = ok;
                fin.checkDetail = detail;
                std::string reason;
                if (ok) reason = "ok";
                else if (endKind == "completed") reason = "checkFailed";
                else reason = endKind;
                ApplyAttemptOutcome(tq, job, ok, reason, exitCode, detail, ok ? nullptr : &fin);
                AppendRunRecordForAttempt(tq, job, a, fin, logText);
                break;
            }
            if (handled) break;
        }

        if (!handled) {
            PlainRunState plain;
            plain.processId = processId;
            plain.exitCode = exitCode;
            plain.reason = (exitCode == 0 ? "ok" : (endKind.empty() ? "failed" : endKind));
            ProcessSnapshot snap;
            if (pm_ && pm_->GetSnapshot(processId, snap)) {
                plain.name = snap.name;
                plain.command = snap.commandLine;
                plain.cwd = snap.workingDir;
                plain.start = snap.startTimeStr;
                plain.end = !snap.endTimeStr.empty() ? snap.endTimeStr : GetCurrentTimestampStr();
                plain.summary = snap.supervisorGoal;
                plain.logFile = snap.logFilePath;
                if (!snap.jobKey.empty()) plain.jobId = snap.jobKey;
                if (!snap.lastLinePreview.empty()) {
                    std::string jobSafe = SanitizeFileNameComponent(plain.jobId.empty() ? ("plain_" + std::to_string(processId)) : plain.jobId);
                    std::wstring tailPath = PathJoinWide(GetTaskQueueLogDirWide(),
                        Utf8ToWide(jobSafe) + L".1.tail.txt");
                    EnsureDirectoryWide(GetTaskQueueLogDirWide());
                    WriteFileAtomicWide(tailPath, snap.lastLinePreview);
                    plain.lastLinesFile = WideToUtf8(tailPath);
                }
            } else {
                plain.start = GetCurrentTimestampStr();
                plain.end = plain.start;
            }
            std::string err;
            RecordPlainRunLocked(plain, err);
        }
    }
    FlushFinishedCallbacks();
}

} // namespace pc
