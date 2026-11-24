#include "taskqueue.h"
#include "parse_util.h"
#include "common.h"
#include "chain_guard.h"
#include "event_schema.h"
#include "memory_search.h"
#include "memory_record.h"
#include "llm_config.h"
#include "permit_config.h"
#include "worker_engines.h"
#include "translation_store.h"
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
#include <unordered_set>
#include <stdexcept>
#include <iomanip>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <signal.h>
#include <poll.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#endif

namespace pc {

// Defined with the process/log helpers further down; the restart path in the constructor
// classifies the interrupted attempt before those definitions.
static std::wstring GetTaskQueueLogDirWide();
static std::wstring GetAttemptLogPathWide(const std::string& jobId, int n);
static std::wstring GetAttemptDonePathWide(const std::string& jobId, int n);
// Defined with the launch helpers; TaskQueueUpdate gates model changes before them.
static std::string ExtractCommandModel(const std::string& command);
static bool ModelChangeRequiresApproval();
static bool IsApprovedWorkerModel(const std::string& model);
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
        case TaskQueueState::Closed:    return "closed";
        case TaskQueueState::TakenOver: return "takenOver";
        case TaskQueueState::WaitingForPermit: return "waitingForPermit";
    }
    return "pending";
}

TaskQueueState TaskQueueStateFromString(const std::string& s) {
    if (s == "running") return TaskQueueState::Running;
    if (s == "succeeded") return TaskQueueState::Succeeded;
    if (s == "failed") return TaskQueueState::Failed;
    if (s == "blocked") return TaskQueueState::Blocked;
    if (s == "cancelled") return TaskQueueState::Cancelled;
    if (s == "closed") return TaskQueueState::Closed;
    if (s == "takenOver") return TaskQueueState::TakenOver;
    if (s == "waitingForPermit") return TaskQueueState::WaitingForPermit;
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
        {"firstEventSec", "first_event_sec"},
        {"jsonStream", "json_stream"},
        {"exitCode", "exit_code"},
        {"filesNewer", "files_newer"},
        {"outputRegex", "output_regex"},
        {"testReport", "test_report"},
        {"contentFile", "content_file"},
        {"minBytes", "min_bytes"},
        {"contentRegex", "content_regex"},
        {"engineResult", "engine_result"},
        {"budgetChars", "budget_chars"},
        {"includeAgentNotes", "include_agent_notes"},
        {"waitForRateLimit", "wait_for_rate_limit"},
        {"suspendWhenDone", "suspend_when_done"},
        {"suspendDelaySec", "suspend_delay_sec"},
        {"suspendHibernate", "suspend_hibernate"},
        {"shutdownWhenDone", "shutdown_when_done"},
        {"shutdownDelaySec", "shutdown_delay_sec"},
        {"taskId", "task_id"},
        {"directionId", "direction_id"},
        {"supersededBy", "superseded_by"},
        {"supersedeReason", "supersede_reason"}
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
// Failure classifier (doc/obsoleted/design-v3.md 4.2-4.4)
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
// used in doc/obsoleted/design-v3.md. std::regex's ECMAScript grammar does not parse it.
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
    if (!ParseDateTimeFields(s, y, mo, d, h, mi, se)) return 0;
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

// 091-6: wrapper commands such as ww-run carry the engine/model as -Engine/-Model options.
// Only those two explicit forms are used; a plain build command yields an empty pair and is
// never subject to engine-health handling.
static bool ParseCommandNamedValue(const std::string& command, const std::string& flag,
                                   std::string& out) {
    std::vector<std::string> tokens;
    std::string current;
    bool quoted = false;
    for (size_t i = 0; i < command.size(); ++i) {
        const char c = command[i];
        if (c == '"') { quoted = !quoted; continue; }
        if (!quoted && (c == ' ' || c == '\t')) {
            if (!current.empty()) { tokens.push_back(current); current.clear(); }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) tokens.push_back(current);
    for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i] == flag) { out = tokens[i + 1]; return true; }
        if (tokens[i].rfind(flag + "=", 0) == 0) { out = tokens[i].substr(flag.size() + 1); return true; }
    }
    return false;
}

static bool ParseCommandEngineModel(const std::string& command, const std::string& name,
                                    std::string& engine, std::string& model) {
    engine = DetectAgentType(command, name);
    model = ExtractCommandModel(command);
    std::string wrapperEngine, wrapperModel;
    if (ParseCommandNamedValue(command, "-Engine", wrapperEngine) && !wrapperEngine.empty() &&
        wrapperEngine != "{engine}") engine = wrapperEngine;
    if (ParseCommandNamedValue(command, "-Model", wrapperModel) && !wrapperModel.empty() &&
        wrapperModel != "{model}") model = wrapperModel;
    if (engine == "{engine}") engine.clear();
    if (model == "{model}") model.clear();
    return !engine.empty() && !model.empty();
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
    in.idleFirstEventSec = job.idle.firstEventSec;
    in.filesNewerConfigured = !job.success.filesNewer.empty();
    in.outputRegexConfigured = !job.success.outputRegex.empty();
    in.nowEpochSec = GetNowEpochSec();
    in.externalRules = LoadTaskQueueFailureRules(TaskQueueFailureRulesPath());
    in.command = job.command;
    ParseCommandEngineModel(job.command, job.name, in.engine, in.model);
    in.client = job.origin.client;
    if (!in.engine.empty() && !in.model.empty()) {
        using namespace engine_health;
        Store store;
        std::string healthErr;
        if (Load(DefaultPath(), store, healthErr)) {
            const Entry* e = Find(store, in.engine, in.model);
            if (e) {
                const int64_t now = NowEpochMs();
                const int64_t windowMs = static_cast<int64_t>(LoadSettings().recentSuccessSec) * 1000;
                in.recentSuccess = e->lastSuccessMs > 0 && now - e->lastSuccessMs <= windowMs;
                in.otherZeroEventFailure = e->zeroEventFailures > 0;
                in.engineDown = e->status == "down";
            }
        }
    }
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
    if (detail.rfind("content file", 0) == 0 || detail.rfind("content_regex", 0) == 0) {
        return "content";
    }
    if (detail.rfind("engine result", 0) == 0) return "engineResult";
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

// 091-7: the exact tool-error wording opencode uses when a permission rule blocks a call,
// plus claude's equivalent. The broader kPermissionPattern below handles generic log lines.
static const char* kPermissionToolPattern =
    "(rejected permission to use this specific tool call|"
    "rule which prevents you from using this specific tool call|"
    "blocked by a permission rule|blocked by configuration policy|"
    "permission denied|request rejected by the user|permission_denied)";

struct PermissionDenial {
    const nlohmann::json* event = nullptr;
    std::string tool;
    std::string target;
    int64_t count = 0;
};

static std::string TargetFromToolInput(const nlohmann::json& call) {
    auto data = call.find("data");
    if (data == call.end() || !data->is_object()) return "";
    auto input = data->find("input");
    if (input == data->end()) return "";
    if (input->is_string()) return input->get<std::string>();
    if (!input->is_object()) return "";
    for (const char* key : {"filePath", "path", "command", "pattern", "url", "target", "resource"}) {
        auto it = input->find(key);
        if (it == input->end() || !it->is_string()) continue;
        if (!it->get<std::string>().empty()) return it->get<std::string>();
    }
    return "";
}

static PermissionDenial FindPermissionDenial(const std::vector<nlohmann::json>& events) {
    PermissionDenial out;
    std::map<std::string, const nlohmann::json*> calls;
    for (const auto& ev : events) {
        if (!ev.is_object() || ev.value("kind", std::string()) != "tool.call") continue;
        std::string id = EventDataString(ev, "toolUseId");
        if (!id.empty()) calls[id] = &ev;
    }
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        if (!it->is_object() || it->value("kind", std::string()) != "tool.result") continue;
        if (EventDataBool(*it, "ok", true)) continue;
        std::string err = EventDataString(*it, "error");
        if (!MatchesNoCase(err, kPermissionToolPattern)) continue;
        out.event = &(*it);
        std::string id = EventDataString(*it, "toolUseId");
        auto call = calls.find(id);
        if (call != calls.end()) {
            out.tool = EventDataString(*call->second, "tool");
            out.target = EventDataString(*call->second, "target");
            if (out.target.empty()) out.target = TargetFromToolInput(*call->second);
        }
        return out;
    }
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        if (!it->is_object() || it->value("kind", std::string()) != "error") continue;
        std::string msg = EventDataString(*it, "message");
        if (!MatchesNoCase(msg, kPermissionToolPattern)) continue;
        out.event = &(*it);
        return out;
    }
    for (auto it = events.rbegin(); it != events.rend(); ++it) {
        if (!it->is_object() || it->value("kind", std::string()) != "run.ended") continue;
        int64_t n = EventDataInt(*it, "permissionDenials", 0);
        if (n <= 0) continue;
        out.event = &(*it);
        out.count = n;
        return out;
    }
    return out;
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
        if (nowEpochSec > 0 && ts > 0 && nowEpochSec >= ts) {
            d.lastActivity.sinceSec = nowEpochSec - ts;
        } else if (in.lastActivitySinceSec >= 0) {
            d.lastActivity.sinceSec = in.lastActivitySinceSec;
        }
        return;
    }
    if (in.lastActivity.empty() && in.lastActivityTarget.empty()) return;
    d.lastActivity.activity = in.lastActivity;
    d.lastActivity.target = in.lastActivityTarget;
    d.lastActivity.sinceSec = in.lastActivitySinceSec;
}

static bool HasWorkerActivityEvents(const std::vector<nlohmann::json>& events) {
    for (const auto& ev : events) {
        if (!ev.is_object()) continue;
        std::string kind = ev.value("kind", std::string());
        if (kind.empty()) continue;
        if (kind != "run.started") {
            return true;
        }
    }
    return false;
}

static const char* kSupervisorNoteTakeover =
    "Before implementing this task yourself, record the decision: "
    "escalate with answer \"takeover\" and a rationale, or run taskqueue takeover "
    "(see doc/obsoleted/HanchoExpectations.md 7.5).";

static std::vector<TaskQueueFailureLikelyCause> DetectLikelyCauses(const TaskQueueFailureInput& in) {
    std::vector<TaskQueueFailureLikelyCause> causes;

    // 1. no_model: command launches opencode without --model, and job.model is empty
    const std::string eng = !in.engine.empty() ? in.engine : DetectAgentType(in.command, in.jobName);
    const bool isOpencode = (LowerAscii(eng) == "opencode") || (LowerAscii(in.command).find("opencode") != std::string::npos);
    const std::string cmdModel = ExtractCommandModel(in.command);
    const bool hasModelArg = !cmdModel.empty() || in.command.find("--model") != std::string::npos || in.command.find("-m") != std::string::npos;
    if (isOpencode && in.model.empty() && !hasModelArg) {
        causes.push_back({"no_model", "opencode was launched without --model"});
    }

    // 2. same_engine_nesting: job engine and client (origin.client) are the same
    if (!eng.empty() && !in.client.empty()) {
        if (LowerAscii(in.client).find(LowerAscii(eng)) != std::string::npos) {
            causes.push_back({"same_engine_nesting", "supervisor and worker are both " + eng});
        }
    }

    // 3. waiting_for_input: stderr is empty, no events emitted (CPU near 0 if present)
    const bool stderrEmpty = TrimJsonStr(in.stderrText).empty();
    const bool noEvents = !HasWorkerActivityEvents(in.events);
    const bool cpuNearZero = (in.cpuSec < 0.0) || (in.cpuSec <= 1.0);
    if (stderrEmpty && noEvents && cpuNearZero) {
        causes.push_back({"waiting_for_input", "the process tree used almost no CPU; it may be waiting for a prompt or stdin"});
    }

    // 4. quota_suspected (091-6): recent success on this engine, then a sudden 0-event
    // failure, and another job/probe on the same engine failed the same way recently.
    if (in.recentSuccess && in.otherZeroEventFailure && noEvents && !eng.empty()) {
        causes.push_back({"quota_suspected",
                          eng + " worked recently but stopped emitting events across multiple jobs; "
                          "the provider may have exhausted its token quota"});
    }

    // 5. stderr_present: stderr has content (last line in why)
    if (!stderrEmpty) {
        std::string text = in.stderrText;
        while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) text.pop_back();
        size_t lastNl = text.find_last_of("\r\n");
        std::string lastLine = (lastNl == std::string::npos) ? text : text.substr(lastNl + 1);
        causes.push_back({"stderr_present", lastLine});
    }

    return causes;
}

static std::vector<TaskQueueFailureNextStep> BuildNextSteps(
    const TaskQueueFailureInput& in, const std::string& failureClass,
    const std::vector<TaskQueueFailureLikelyCause>& likelyCauses) {

    std::vector<TaskQueueFailureNextStep> steps;
    if (in.engineDown) {
        steps.push_back({"engine_down", nullptr, false,
                         "Hancho is probing the engine for recovery. Wait and it will resume automatically."});
    }
    const int attempt = in.attempt > 0 ? in.attempt : 1;
    const std::string stall = failureClass == "startup_hang" ? "hung at startup"
                            : failureClass == "idle_stall" ? "stalled without activity"
                            : "stalled before the timeout";

    bool hasNoModel = false;
    for (const auto& c : likelyCauses) {
        if (c.id == "no_model") {
            hasNoModel = true;
            break;
        }
    }

    const std::string eng = !in.engine.empty() ? in.engine : DetectAgentType(in.command, in.jobName);
    const std::string altEngine = (LowerAscii(eng) == "claude" ? "agy" : "claude");

    nlohmann::json primaryChange = hasNoModel
        ? nlohmann::json({{"model", "<explicit model>"}})
        : nlohmann::json({{"engine", altEngine}});
    nlohmann::json secondaryChange = hasNoModel
        ? nlohmann::json({{"engine", altEngine}})
        : nlohmann::json({{"model", "<explicit model>"}});

    if (attempt <= 1) {
        steps.push_back({"retry", nullptr, false, ""});
        steps.push_back({"retry", primaryChange, true, ""});
        steps.push_back({"escalate", nullptr, false,
                         "Worker " + stall + ". Retry with another " +
                         (hasNoModel ? std::string("model") : std::string("engine")) + ", or take over?"});
    } else if (attempt == 2) {
        steps.push_back({"retry", primaryChange, true, ""});
        steps.push_back({"retry", secondaryChange, true, ""});
        steps.push_back({"escalate", nullptr, false,
                         "Worker " + stall + " twice. Retry with another engine, or take over?"});
    } else {
        steps.push_back({"escalate", nullptr, false,
                         "Worker " + stall + " multiple times. Retry with another engine, or take over?"});
        steps.push_back({"retry", primaryChange, true, ""});
    }

    return steps;
}

// 091-7: one shared writer for every permission-denial path, so the tool/target/engine
// evidence and the three next steps are identical whether the denial came from a tool
// result, an error event, an engine counter or a log line.
static void FillPermissionDiagnostic(TaskQueueFailureDiagnostic& d, const TaskQueueFailureInput& in,
                                     const PermissionDenial& denial, const std::string& message,
                                     const std::string& confidence, const std::string& source) {
    d.failureClass = "permission_denied";
    d.confidence = confidence;
    d.detail["rule"] = "builtin.permission_denied";
    d.detail["source"] = source;
    std::string tool = denial.tool;
    std::string target = denial.target;
    std::string engine = !in.engine.empty() ? in.engine
                         : DetectAgentType(in.command, in.jobName);
    if (!message.empty()) {
        d.detail["message"] = message;
        d.summary = "Permission denied: " + message;
    } else if (denial.count > 0) {
        d.detail["permissionDenials"] = denial.count;
        d.summary = "The agent was denied permission " + std::to_string(denial.count) + " time(s).";
    } else {
        d.summary = "A tool call was denied by permissions.";
    }
    if (!tool.empty()) d.detail["tool"] = tool;
    if (!target.empty()) d.detail["target"] = target;
    if (!engine.empty()) d.detail["engine"] = engine;
    d.detail["permission"] = nlohmann::json{{"tool", tool}, {"target", target}, {"engine", engine}};
    d.suggestedAction = "fix_spec";
    d.likelyCauses = DetectLikelyCauses(in);
    d.nextSteps.clear();
    d.nextSteps.push_back({"fix_spec", nullptr, false,
                           "Rewrite the instructions so the work stays inside the project (cwd)."});
    d.nextSteps.push_back({"permit_request", nlohmann::json{{"kind", tool == "bash" || tool == "Bash" ? "command" : "read_path"},
                                                            {"target", target}},
                           true, "Apply for this permission with `hancho-cli permit request` if it is genuinely needed."});
    d.nextSteps.push_back({"escalate", nullptr, false,
                           "Ask the user to approve the permit or change the instructions."});
    if (denial.event) {
        AddEventEvidence(d, in.eventsFile, EventSeqOf(*denial.event));
        if (!d.evidence.empty()) {
            d.evidence.back().tool = tool;
            d.evidence.back().target = target;
            d.evidence.back().engine = engine;
        }
    }
    EnsureEvidence(d, in);
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
    // The log-line form of the same wording. It only applies to LLM worker jobs and needs
    // word boundaries: a plain job that prints job JSON such as "on_permission_denied":""
    // (taskqueue_smoke.exe, 2026-11-23 lineage-100-1-test-1) must fail as a normal failure,
    // not be parked in waitingForPermit.
    static const char* kPermissionLogPattern =
        "\\b(permission denied|permission_denied|access is denied|not permitted|eacces|operation not permitted)\\b";
    const bool workerJob = !in.engine.empty() || !DetectAgentType(in.command, in.jobName).empty();
    static const char* kTurnPattern =
        "(max_turns|maximum number of turns|turn limit|context length|context window|maximum context|"
        "prompt is too long|too many tokens|token limit)";
    static const char* kRootCauseNotFoundPattern =
        "^HANCHO-RESULT:[ \\t]*root_cause_not_found([ \\t]|$)";

    TaskQueueFailureDiagnostic d;
    d.detail = nlohmann::json::object();
    const int64_t now = in.nowEpochSec > 0 ? in.nowEpochSec : GetNowEpochSec();
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
    const PermissionDenial permissionDenial = FindPermissionDenial(in.events);
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

    // 2.5 startup_hang: timed out or idle timed out with 0 worker events (or only run.started), or startup_timed_out
    const bool noWorkerActivity = !HasWorkerActivityEvents(in.events);
    if (in.endKind == "startup_timed_out" ||
        ((in.endKind == "timed_out" || in.endKind == "idle_timed_out") && noWorkerActivity)) {
        d.failureClass = "startup_hang";
        d.confidence = "high";
        int64_t sec = 0;
        if (in.endKind == "startup_timed_out" && in.idleFirstEventSec > 0) {
            sec = in.idleFirstEventSec;
        } else if (in.elapsedSec > 0) {
            sec = in.elapsedSec;
        } else if (in.timeoutSec > 0) {
            sec = in.timeoutSec;
        } else if (in.idleFirstEventSec > 0) {
            sec = in.idleFirstEventSec;
        } else {
            sec = in.idleOutputSec;
        }
        d.summary = "The worker started but emitted no events in " + std::to_string(sec) + " s.";
        d.detail["rule"] = "builtin.startup_hang";
        d.detail["endKind"] = in.endKind;
        if (in.idleFirstEventSec > 0) d.detail["idleFirstEventSec"] = in.idleFirstEventSec;
        if (in.timeoutSec > 0) d.detail["timeoutSec"] = in.timeoutSec;
        if (in.elapsedSec > 0) d.detail["elapsedSec"] = in.elapsedSec;
        if (in.idleOutputSec > 0) d.detail["idleOutputSec"] = in.idleOutputSec;
        if (!in.processTree.is_null()) d.detail["processTree"] = in.processTree;
        d.suggestedAction = "retry_with_change";
        d.likelyCauses = DetectLikelyCauses(in);
        d.nextSteps = BuildNextSteps(in, "startup_hang", d.likelyCauses);
        d.supervisorNote = kSupervisorNoteTakeover;
        AddLogEvidence(d, in.logFile, in.logLastLine);
        EnsureEvidence(d, in);
        return d;
    }

    // 3. timeout: the attempt exceeded timeoutSec and was terminated.
    if (in.endKind == "timed_out") {
        d.failureClass = "timeout";
        d.confidence = "high";
        d.detail["timeoutSec"] = in.timeoutSec;
        if (in.elapsedSec > 0) d.detail["elapsedSec"] = in.elapsedSec;
        if (!in.processTree.is_null()) d.detail["processTree"] = in.processTree;

        const int64_t threshold = std::max<int64_t>(30, in.timeoutSec / 10);
        const bool stalledMidRun = (d.lastActivity.sinceSec > threshold);
        if (!stalledMidRun) {
            d.summary = "The attempt did not finish within " + std::to_string(in.timeoutSec) + " s.";
            d.detail["rule"] = "builtin.timeout";
            d.suggestedAction = "raise_timeout";
        } else {
            d.summary = "The attempt stalled before the timeout (" + std::to_string(in.timeoutSec) + " s).";
            d.detail["rule"] = "builtin.timeout_stalled";
            d.detail["stalled"] = true;
            d.suggestedAction = (in.attempt > 1 ? "retry_with_change" : "retry");
            d.likelyCauses = DetectLikelyCauses(in);
            d.nextSteps = BuildNextSteps(in, "timeout", d.likelyCauses);
            d.supervisorNote = kSupervisorNoteTakeover;
            const nlohmann::json* lastEv = nullptr;
            for (auto it = in.events.rbegin(); it != in.events.rend(); ++it) {
                if (it->is_object() && it->value("kind", "") != "run.started") {
                    lastEv = &(*it);
                    break;
                }
            }
            if (lastEv) {
                AddEventEvidence(d, in.eventsFile, EventSeqOf(*lastEv));
                std::string tool = EventDataString(*lastEv, "tool");
                if (!tool.empty()) d.detail["lastTool"] = tool;
                std::string act = EventDataString(*lastEv, "activity");
                if (!act.empty()) d.detail["lastActivity"] = act;
            }
        }
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
        if (!in.processTree.is_null()) d.detail["processTree"] = in.processTree;
        d.suggestedAction = (in.attempt > 1 ? "retry_with_change" : "retry");
        d.likelyCauses = DetectLikelyCauses(in);
        d.nextSteps = BuildNextSteps(in, "idle_stall", d.likelyCauses);
        d.supervisorNote = kSupervisorNoteTakeover;
        const nlohmann::json* lastEv = nullptr;
        for (auto it = in.events.rbegin(); it != in.events.rend(); ++it) {
            if (it->is_object() && it->value("kind", "") != "run.started") {
                lastEv = &(*it);
                break;
            }
        }
        if (lastEv) {
            AddEventEvidence(d, in.eventsFile, EventSeqOf(*lastEv));
            std::string tool = EventDataString(*lastEv, "tool");
            if (!tool.empty()) d.detail["lastTool"] = tool;
        }
        AddLogEvidence(d, in.logFile, in.logLastLine);
        EnsureEvidence(d, in);
        return d;
    }

    // 4.5 permission_denied (091-7): a denied tool call outranks log-string rules such as
    // rate_limited. The 2026-11-19 cards 090-2/090-3 were misdiagnosed because a quota line
    // elsewhere in the log won before the denial was considered.
    {
        int64_t denials = runEnded ? EventDataInt(*runEnded, "permissionDenials", 0) : 0;
        if (permissionDenial.event) {
            std::string msg;
            if (permissionDenial.event->value("kind", std::string()) == "tool.result") {
                msg = EventDataString(*permissionDenial.event, "error");
            } else if (permissionDenial.event->value("kind", std::string()) == "error") {
                msg = EventDataString(*permissionDenial.event, "message");
            }
            FillPermissionDiagnostic(d, in, permissionDenial, msg, "high", "event");
            return d;
        }
        if (permissionEvent || denials > 0 || permissionDenial.count > 0) {
            PermissionDenial fallback;
            fallback.count = denials > 0 ? denials : permissionDenial.count;
            std::string msg = permissionEvent ? EventDataString(*permissionEvent, "message") : "";
            FillPermissionDiagnostic(d, in, fallback, msg, permissionEvent ? "high" : "medium",
                                     permissionEvent ? "event" : "counter");
            if (permissionEvent) AddEventEvidence(d, in.eventsFile, EventSeqOf(*permissionEvent));
            return d;
        }
        int line = workerJob ? FindLogLineNoCase(in, kPermissionLogPattern) : 0;
        if (line > 0) {
            PermissionDenial fallback;
            FillPermissionDiagnostic(d, in, fallback, "", "high", "log");
            AddLogEvidence(d, in.logFile, line);
            return d;
        }
    }

    // 4.7 (102-2): a structured class from the error object (HTTP status or error code, set by
    // llm_stream). It outranks the log-wording rules below, so a token expiry is not misread as
    // a quota line elsewhere in the log. doc/llm-error-class-cards.md has the mapping.
    {
        const nlohmann::json* structured = FindLastEvent(in.events, [&](const nlohmann::json& ev) {
            if (ev.value("kind", std::string()) != "error") return false;
            const std::string c = EventDataString(ev, "class");
            return c == "auth_error" || c == "rate_limited" || c == "provider_down";
        });
        if (structured) {
            const std::string cls = EventDataString(*structured, "class");
            const std::string message = EventDataString(*structured, "message");
            const std::string code = EventDataString(*structured, "code");
            const int64_t httpStatus = EventDataInt(*structured, "httpStatus", 0);
            d.failureClass = cls;
            d.confidence = "high";
            d.detail["rule"] = "structured." + cls;
            if (!message.empty()) d.detail["message"] = message;
            if (!code.empty()) d.detail["code"] = code;
            if (httpStatus > 0) d.detail["httpStatus"] = httpStatus;
            if (cls == "auth_error") {
                d.summary = message.empty() ? "Authentication failed." : "Authentication failed: " + message;
                d.suggestedAction = "human";
            } else if (cls == "rate_limited") {
                d.summary = "The provider rejected the request for rate or quota; wait before retrying.";
                d.suggestedAction = "retry_after";
            } else {
                d.summary = "The provider is overloaded or unavailable; retry later.";
                d.suggestedAction = "retry";
            }
            AddEventEvidence(d, in.eventsFile, EventSeqOf(*structured));
            EnsureEvidence(d, in);
            return d;
        }
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

    // 7. permission_denied: remaining wording not covered by the higher-priority 4.5 rule.
    // The shared writer keeps evidence and nextSteps identical across both rules.
    {
        int64_t denials = runEnded ? EventDataInt(*runEnded, "permissionDenials", 0) : 0;
        if (permissionEvent || denials > 0) {
            PermissionDenial fallback;
            fallback.count = denials;
            std::string msg = permissionEvent ? EventDataString(*permissionEvent, "message") : "";
            FillPermissionDiagnostic(d, in, fallback, msg, permissionEvent ? "high" : "medium",
                                     permissionEvent ? "event" : "counter");
            if (permissionEvent) AddEventEvidence(d, in.eventsFile, EventSeqOf(*permissionEvent));
            return d;
        }
        int line = workerJob ? FindLogLineNoCase(in, kPermissionLogPattern) : 0;
        if (line > 0) {
            PermissionDenial fallback;
            FillPermissionDiagnostic(d, in, fallback, "", "high", "log");
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

    // 12.5 root_cause_not_found: worker self-reported it could not find the root cause (line start only).
    {
        int line = FindLogLineRegex(in, kRootCauseNotFoundPattern);
        if (line > 0) {
            d.failureClass = "root_cause_not_found";
            d.confidence = "high";
            d.summary = "The worker reported that the root cause was not found.";
            d.detail["rule"] = "builtin.root_cause_not_found";
            d.detail["source"] = "log";
            d.suggestedAction = "retry";
            AddLogEvidence(d, in.logFile, line);
            EnsureEvidence(d, in);
            return d;
        }
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
        if (!e.tool.empty()) item["tool"] = e.tool;
        if (!e.target.empty()) item["target"] = e.target;
        if (!e.engine.empty()) item["engine"] = e.engine;
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
    if (!d.likelyCauses.empty()) {
        nlohmann::json causes = nlohmann::json::array();
        for (const auto& c : d.likelyCauses) {
            nlohmann::json item = nlohmann::json::object();
            item["id"] = c.id;
            item["why"] = c.why;
            causes.push_back(std::move(item));
        }
        o["likelyCauses"] = std::move(causes);
    }
    if (!d.nextSteps.empty()) {
        nlohmann::json steps = nlohmann::json::array();
        for (const auto& s : d.nextSteps) {
            nlohmann::json item = nlohmann::json::object();
            item["action"] = s.action;
            if (s.change.is_object() && !s.change.empty()) {
                item["change"] = s.change;
            }
            if (s.needsApproval) {
                item["needsApproval"] = true;
            }
            if (!s.question.empty()) {
                item["question"] = s.question;
            }
            steps.push_back(std::move(item));
        }
        o["nextSteps"] = std::move(steps);
    }
    if (!d.supervisorNote.empty()) {
        o["supervisorNote"] = d.supervisorNote;
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
            e.tool = item.value("tool", std::string());
            e.target = item.value("target", std::string());
            e.engine = item.value("engine", std::string());
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
    auto causes = o.find("likelyCauses");
    if (causes != o.end() && causes->is_array()) {
        for (const auto& item : *causes) {
            if (!item.is_object()) continue;
            TaskQueueFailureLikelyCause c;
            c.id = item.value("id", std::string());
            c.why = item.value("why", std::string());
            d.likelyCauses.push_back(std::move(c));
        }
    }
    auto steps = o.find("nextSteps");
    if (steps != o.end() && steps->is_array()) {
        for (const auto& item : *steps) {
            if (!item.is_object()) continue;
            TaskQueueFailureNextStep s;
            s.action = item.value("action", std::string());
            auto ch = item.find("change");
            if (ch != item.end() && ch->is_object()) s.change = *ch;
            s.needsApproval = item.value("needsApproval", false);
            s.question = item.value("question", std::string());
            d.nextSteps.push_back(std::move(s));
        }
    }
    d.supervisorNote = readString("supervisorNote");
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
    if (a.testReportSummary.total > 0) {
        oss << ",\"test_report_summary\":{\"total\":" << a.testReportSummary.total
            << ",\"failed\":" << a.testReportSummary.failed
            << ",\"skipped\":" << a.testReportSummary.skipped
            << ",\"failed_names\":" << StrArrayToJson(a.testReportSummary.failedNames) << "}";
    }
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
    if (!s.contentFile.empty()) oss << ",\"content_file\":\"" << JsonEscape(s.contentFile) << "\"";
    if (s.minBytes > 0) oss << ",\"min_bytes\":" << s.minBytes;
    if (!s.contentRegex.empty()) oss << ",\"content_regex\":\"" << JsonEscape(s.contentRegex) << "\"";
    if (s.engineResult) oss << ",\"engine_result\":true";
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
        "launch_error", "timeout", "idle_stall", "startup_hang", "exit_nonzero", "crash",
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
    const std::string summaryObj = JsonFieldObj(j, "test_report_summary", "");
    if (!summaryObj.empty()) {
        a.testReportSummary.total = static_cast<int>(JsonFieldInt(summaryObj, "total", 0));
        a.testReportSummary.failed = static_cast<int>(JsonFieldInt(summaryObj, "failed", 0));
        a.testReportSummary.skipped = static_cast<int>(JsonFieldInt(summaryObj, "skipped", 0));
        a.testReportSummary.failedNames = JsonFieldStrArray(summaryObj, "failed_names");
    }
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
    job.summaryLang = JsonFieldStr(j, "summaryLang", "");
    {
        const nlohmann::json src = nlohmann::json::parse(JsonFieldObj(j, "summarySource"), nullptr, false);
        if (src.is_object()) {
            job.summarySourceLang = src.value("lang", std::string());
            job.summarySource = src.value("text", std::string());
        }
    }
    job.instructions = ParseInstructions(JsonFieldObj(j, "instructions"));
    job.command = JsonFieldStr(j, "command", "");
    job.cwd = JsonFieldStr(j, "cwd", "");
    job.host = JsonFieldStr(j, "host", "");
    job.env = ParseJsonStringMap(JsonFieldObj(j, "env"));
    for (const auto& needJson : JsonFieldObjArray(j, "needs")) {
        TaskQueueNeed need;
        need.kind = JsonFieldStr(needJson, "kind", "");
        need.target = JsonFieldStr(needJson, "target", "");
        if (!need.kind.empty() && !need.target.empty()) job.needs.push_back(need);
    }
    job.onPermissionDenied = JsonFieldStr(j, "on_permission_denied", "");
    job.waitPermits = JsonFieldStrArray(j, "wait_permits");
    job.resumeSession = JsonFieldStr(j, "resume_session", "");
    job.permissionResumeCount = static_cast<int>(JsonFieldInt(j, "permission_resume_count", 0));
    job.permissionResumeLimit = static_cast<int>(JsonFieldInt(j, "permission_resume_limit", 2));
    job.supersedes = JsonFieldStr(j, "supersedes", "");
    job.taskId = JsonFieldStr(j, "task_id", "");
    job.directionId = JsonFieldStr(j, "direction_id", "");
    job.supersededBy = JsonFieldStr(j, "superseded_by", "");
    job.supersedeReason = JsonFieldStr(j, "supersede_reason", "");
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
    job.idle.firstEventSec = static_cast<int>(JsonFieldInt(idleObj, "first_event_sec", 0));
    job.maxAttempts = static_cast<int>(JsonFieldInt(j, "max_attempts", 2));
    job.retryDelaySec = static_cast<int>(JsonFieldInt(j, "retry_delay_sec", 0));
    std::string retryErr;
    if (!ParseRetryChecked(j, job.retry, retryErr)) job.retry = TaskQueueRetry();
    std::string successObj = JsonFieldObj(j, "success");
    job.success.exitCode = static_cast<int>(JsonFieldInt(successObj, "exit_code", 0));
    job.success.filesNewer = JsonFieldStrArray(successObj, "files_newer");
    job.success.outputRegex = JsonFieldStr(successObj, "output_regex", "");
    job.success.testReport = JsonFieldStr(successObj, "test_report", "");
    job.success.contentFile = JsonFieldStr(successObj, "content_file", "");
    job.success.minBytes = JsonFieldInt(successObj, "min_bytes", 0);
    job.success.contentRegex = JsonFieldStr(successObj, "content_regex", "");
    job.success.engineResult = JsonFieldBool(successObj, "engine_result", false);
    std::string deferErr;
    if (!ParseDeferChecked(j, job.success.exitCode, job.defer, deferErr)) job.defer = TaskQueueDefer();
    job.killExclude = JsonFieldStrArray(j, "kill_exclude");
    job.resources = ParseResources(JsonFieldObj(j, "resources"));
    job.memory = ParseMemory(JsonFieldObj(j, "memory", ""));
    job.priority = static_cast<int>(JsonFieldInt(j, "priority", 0));
    job.hidden = JsonFieldBool(j, "hidden", false);
    job.paused = JsonFieldBool(j, "paused", false);
    job.suspendWhenDone = JsonFieldBool(j, "suspend_when_done", false);
    job.suspendDelaySec = static_cast<int>(JsonFieldInt(j, "suspend_delay_sec", 0));
    job.shutdownWhenDone = JsonFieldBool(j, "shutdown_when_done", false);
    job.shutdownDelaySec = static_cast<int>(JsonFieldInt(j, "shutdown_delay_sec", 0));
    if (job.suspendDelaySec < 0) job.suspendDelaySec = 0;
    if (job.suspendDelaySec > 86400) job.suspendDelaySec = 86400;
    job.suspendHibernate = JsonFieldBool(j, "suspend_hibernate", false);
    job.state = JsonFieldStr(j, "state", "pending");
    if (HasJsonKey(j, "takeover")) {
        const std::string t = JsonFieldObj(j, "takeover", "");
        if (!t.empty()) {
            job.takeover.present = true;
            job.takeover.by = JsonFieldStr(t, "by", "");
            job.takeover.reason = JsonFieldStr(t, "reason", "");
            job.takeover.at = JsonFieldStr(t, "at", "");
            job.takeover.completedAt = JsonFieldStr(t, "completed_at", "");
            job.takeover.report = JsonFieldStr(t, "report", "");
            job.takeover.result = JsonFieldStr(t, "result", "");
            job.takeover.completionReason = JsonFieldStr(t, "completion_reason", "");
        }
    }
    for (const auto& attJson : JsonFieldObjArray(j, "attempts")) {
        job.attempts.push_back(ParseAttempt(attJson));
    }
    job.blockedBy = JsonFieldStr(j, "blocked_by", "");
    job.createdAt = JsonFieldStr(j, "created_at", "");
    job.updatedAt = JsonFieldStr(j, "updated_at", "");
    job.notes = JsonFieldStr(j, "notes", "");
    job.engineSwitch = !HasJsonKey(j, "engine_switch") || JsonFieldBool(j, "engine_switch", true);
    job.holdReason = JsonFieldStr(j, "hold_reason", "");
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
    const int64_t parsedRev = JsonFieldInt(j, "rev", 0);
    job.rev = parsedRev > 0 ? static_cast<uint64_t>(parsedRev) : 0;
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
    taskStore_.Open(PathJoinWide(GetEffectiveDataDirectoryWide(), L"tasks"));
    directionStore_.Open(PathJoinWide(GetEffectiveDataDirectoryWide(), L"directions"));
    LoadQueues();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        InitRevLocked();
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
                    MarkJobChangedLocked(job);
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
    if (clientWatchRunning_.exchange(false)) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            WakeClientWatchLocked();
        }
        if (clientWatchThread_.joinable()) {
            clientWatchThread_.join();
        }
    }
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& w : clientWatches_) {
#ifdef _WIN32
        if (w.handle) { CloseHandle(static_cast<HANDLE>(w.handle)); w.handle = nullptr; }
#else
        if (w.pidfd >= 0) { close(w.pidfd); w.pidfd = -1; }
#endif
    }
    clientWatches_.clear();
#ifdef _WIN32
    if (clientWatchWakeEvent_) {
        CloseHandle(static_cast<HANDLE>(clientWatchWakeEvent_));
        clientWatchWakeEvent_ = nullptr;
    }
#endif
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

void TaskQueueManager::SetBtCancelCallback(BtCancelCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    btCancelCb_ = std::move(cb);
}

void TaskQueueManager::SetSuspendCallback(SuspendCallback cb) {
    std::lock_guard<std::mutex> lock(mutex_);
    suspendCb_ = std::move(cb);
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
            const TaskQueueJob* dep = ResolveDepJobLocked(d.id);
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
                        if (ParseDateTimeFields(lastAtt.start, y, mo, d, h, mi, sec)) {
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
                                time_t nowEpoch = static_cast<time_t>(GetNowEpochSec());
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
                    const TaskQueueJob* dep = ResolveDepJobLocked(d.id, &tq);
                    if (!dep || !IsDepSatisfied(*dep, d.on)) {
                        allSat = false;
                        break;
                    }
                }
                if (allSat) {
                    counts.waiting++;
                } else {
                    counts.pending++;
                    if (isNeverRunnable(job, 0)) counts.neverRunnablePending++;
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
            } else if (job.state == "closed") {
                counts.closed++;
            } else if (job.state == "takenOver") {
                counts.takenOver++;
            } else if (job.state == "waitingForPermit") {
                counts.waitingForPermit++;
            }
        }
    }
    if (!anyRemain) counts.sumRemainingSec = -1;
    return counts;
}

namespace {

constexpr int kDefaultSuspendDelaySec = 10;

bool IsTerminalJobState(const std::string& state) {
    return state == "succeeded" || state == "failed" || state == "cancelled" ||
           state == "closed" || state == "blocked" || state == "takenOver";
}

} // namespace

bool TaskQueueManager::HasActiveOtherWork() {
    TaskQueueCounts counts = GetJobCounts();
    int activeJobs = counts.running + counts.waiting +
                     (counts.pending - counts.neverRunnablePending);
    if (activeJobs > 0) return true;
    if (!pm_) return false;
    for (const auto& snap : pm_->GetAllSnapshots()) {
        if (snap.state == ProcState::Running) return true;
    }
    return false;
}

static std::string ResolvePowerAction(bool shutdown, bool hibernate) {
    // A shutdown is stronger than a suspend, so it wins when both are armed.
    if (shutdown) return "shutdown";
    return hibernate ? "hibernate" : "sleep";
}

void TaskQueueManager::CheckSuspendWhenDone() {
    int delaySec = kDefaultSuspendDelaySec;
    std::string action;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!suspendCb_) {
            powerPhase_.clear();
            powerDueMs_ = 0;
            return;
        }

        bool any = false;
        bool unfinished = false;
        bool hibernate = false;
        bool shutdown = false;
        delaySec = 0;
        for (const auto& [name, tq] : queues_) {
            (void)name;
            for (const auto& job : tq.jobs) {
                if (job.suspendWhenDone) {
                    any = true;
                    if (!IsTerminalJobState(job.state)) unfinished = true;
                    int d = job.suspendDelaySec > 0 ? job.suspendDelaySec : kDefaultSuspendDelaySec;
                    if (d > delaySec) delaySec = d;
                    if (job.suspendHibernate) hibernate = true;
                }
                if (job.shutdownWhenDone) {
                    any = true;
                    if (!IsTerminalJobState(job.state)) unfinished = true;
                    int d = job.shutdownDelaySec > 0 ? job.shutdownDelaySec : kDefaultSuspendDelaySec;
                    if (d > delaySec) delaySec = d;
                    shutdown = true;
                }
            }
        }
        if (!any) {
            powerPhase_.clear();
            powerDueMs_ = 0;
            powerDelaySec_ = 0;
            powerAction_.clear();
            return;
        }
        action = ResolvePowerAction(shutdown, hibernate);
        powerDelaySec_ = delaySec;
        powerAction_ = action;
        if (unfinished) {
            if (powerPhase_ != "waiting_target") {
                WriteAuditLog("POWER_WAITING", "waiting for the armed job to finish");
            }
            powerPhase_ = "waiting_target";
            powerDueMs_ = 0;
            return;
        }
    }

    if (HasActiveOtherWork()) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (powerPhase_ != "waiting_others") {
            WriteAuditLog("POWER_WAITING", "waiting for other jobs and managed processes");
        }
        powerPhase_ = "waiting_others";
        powerDueMs_ = 0;
        return;
    }

    bool shouldFire = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Re-scan after the lock was released: flags may have changed.
        bool any = false;
        bool unfinished = false;
        bool hibernate = false;
        bool shutdown = false;
        delaySec = 0;
        for (const auto& [name, tq] : queues_) {
            (void)name;
            for (const auto& job : tq.jobs) {
                if (job.suspendWhenDone) {
                    any = true;
                    if (!IsTerminalJobState(job.state)) unfinished = true;
                    int d = job.suspendDelaySec > 0 ? job.suspendDelaySec : kDefaultSuspendDelaySec;
                    if (d > delaySec) delaySec = d;
                    if (job.suspendHibernate) hibernate = true;
                }
                if (job.shutdownWhenDone) {
                    any = true;
                    if (!IsTerminalJobState(job.state)) unfinished = true;
                    int d = job.shutdownDelaySec > 0 ? job.shutdownDelaySec : kDefaultSuspendDelaySec;
                    if (d > delaySec) delaySec = d;
                    shutdown = true;
                }
            }
        }
        if (!any) {
            powerPhase_.clear();
            powerDueMs_ = 0;
            powerDelaySec_ = 0;
            powerAction_.clear();
            return;
        }
        if (unfinished) {
            powerPhase_ = "waiting_target";
            powerDueMs_ = 0;
            return;
        }
        action = ResolvePowerAction(shutdown, hibernate);
        powerDelaySec_ = delaySec;
        powerAction_ = action;
        if (powerPhase_ != "countdown") {
            powerPhase_ = "countdown";
            powerDueMs_ = static_cast<int64_t>(GetNowTickMs()) +
                          static_cast<int64_t>(delaySec) * 1000;
            WriteAuditLog("POWER_ARMED", "all work idle; " + action + " in " +
                                             std::to_string(delaySec) + "s");
            return;
        }
        if (static_cast<int64_t>(GetNowTickMs()) < powerDueMs_) return;
        shouldFire = true;
    }

    if (!shouldFire) return;

    // Final check as close to the power action as possible.
    if (HasActiveOtherWork()) {
        std::lock_guard<std::mutex> lock(mutex_);
        powerPhase_ = "waiting_others";
        powerDueMs_ = 0;
        return;
    }

    std::string fireJob;
    std::string fireState;
    std::string fireAction;
    SuspendCallback cb;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [name, tq] : queues_) {
            (void)name;
            for (const auto& job : tq.jobs) {
                if (!job.suspendWhenDone && !job.shutdownWhenDone) continue;
                if (!IsTerminalJobState(job.state)) {
                    powerPhase_ = "waiting_target";
                    powerDueMs_ = 0;
                    return;
                }
            }
        }
        for (auto& [name, tq] : queues_) {
            (void)name;
            bool changed = false;
            for (auto& job : tq.jobs) {
                if (!job.suspendWhenDone && !job.shutdownWhenDone) continue;
                job.suspendWhenDone = false;
                job.suspendDelaySec = 0;
                job.suspendHibernate = false;
                job.shutdownWhenDone = false;
                job.shutdownDelaySec = 0;
                job.updatedAt = GetCurrentTimestampStr();
                MarkJobChangedLocked(job);
                if (fireJob.empty()) {
                    fireJob = job.id;
                    fireState = job.state;
                }
                changed = true;
            }
            if (changed) SaveQueue(tq);
        }
        fireAction = powerAction_.empty() ? "sleep" : powerAction_;
        powerPhase_.clear();
        powerDueMs_ = 0;
        powerDelaySec_ = 0;
        powerAction_.clear();
        cb = suspendCb_;
    }

    if (fireJob.empty()) return;
    WriteAuditLog("POWER_" + (fireAction == "shutdown" ? std::string("SHUTDOWN")
                                                       : std::string("SUSPEND")),
                  "job=" + fireJob + " state=" + fireState + " action=" + fireAction);
    if (cb) cb(fireJob, fireState, fireAction);
}

std::string TaskQueueManager::SetJobPowerWhenDone(const std::string& id, const std::string& action,
                                                   int delaySec, std::string& outError) {
    outError.clear();
    std::string normalized = action;
    if (normalized == "suspend") normalized = "sleep";
    if (normalized != "sleep" && normalized != "hibernate" && normalized != "shutdown") {
        outError = "action must be suspend, hibernate or shutdown";
        return BuildError(outError);
    }
    if (delaySec < 0) delaySec = 0;
    if (delaySec > 86400) delaySec = 86400;
    std::lock_guard<std::mutex> lock(mutex_);
    TaskQueue* tq = nullptr;
    TaskQueueJob* job = FindJob(id, &tq);
    if (!job) {
        outError = "job not found: " + id;
        return BuildError(outError);
    }
    job->suspendWhenDone = normalized == "sleep" || normalized == "hibernate";
    job->suspendDelaySec = normalized == "shutdown" ? 0 : delaySec;
    job->suspendHibernate = normalized == "hibernate";
    job->shutdownWhenDone = normalized == "shutdown";
    job->shutdownDelaySec = normalized == "shutdown" ? delaySec : 0;
    job->updatedAt = GetCurrentTimestampStr();
    MarkJobChangedLocked(*job);
    SaveQueue(*tq);
    powerPhase_.clear();
    powerDueMs_ = 0;
    cv_.notify_all();
    WriteAuditLog("POWER_ARM", "job=" + id + " state=" + job->state + " delay=" +
                                   std::to_string(delaySec) + " action=" + normalized);
    nlohmann::json out;
    out["ok"] = true;
    out["armed"] = true;
    out["id"] = job->id;
    out["state"] = job->state;
    out["terminal"] = IsTerminalJobState(job->state);
    out["delay_sec"] = delaySec;
    out["action"] = normalized;
    out["hibernate"] = normalized == "hibernate";
    out["shutdown"] = normalized == "shutdown";
    return out.dump();
}

std::string TaskQueueManager::ClearPowerWhenDone(std::string& outError) {
    outError.clear();
    int cleared = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [name, tq] : queues_) {
            (void)name;
            bool changed = false;
            for (auto& job : tq.jobs) {
                if (!job.suspendWhenDone && !job.shutdownWhenDone) continue;
                job.suspendWhenDone = false;
                job.suspendDelaySec = 0;
                job.suspendHibernate = false;
                job.shutdownWhenDone = false;
                job.shutdownDelaySec = 0;
                job.updatedAt = GetCurrentTimestampStr();
                MarkJobChangedLocked(job);
                ++cleared;
                changed = true;
            }
            if (changed) SaveQueue(tq);
        }
        powerPhase_.clear();
        powerDueMs_ = 0;
        powerDelaySec_ = 0;
        powerAction_.clear();
    }
    if (cleared > 0) {
        WriteAuditLog("POWER_CANCEL", "cleared=" + std::to_string(cleared));
    }
    nlohmann::json out;
    out["ok"] = true;
    out["cleared"] = cleared;
    out["armed"] = false;
    return out.dump();
}

std::string TaskQueueManager::PowerStatusJson() const {
    std::string phase;
    int64_t dueMs = 0;
    int delaySec = 0;
    std::string action;
    nlohmann::json jobs = nlohmann::json::array();
    {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
        phase = powerPhase_;
        dueMs = powerDueMs_;
        delaySec = powerDelaySec_;
        action = powerAction_;
        for (const auto& [name, tq] : queues_) {
            (void)name;
            for (const auto& job : tq.jobs) {
                if (!job.suspendWhenDone && !job.shutdownWhenDone) continue;
                nlohmann::json item;
                item["id"] = job.id;
                item["state"] = job.state;
                item["terminal"] = IsTerminalJobState(job.state);
                if (job.shutdownWhenDone) {
                    item["action"] = "shutdown";
                    item["delay_sec"] = job.shutdownDelaySec;
                } else {
                    item["action"] = job.suspendHibernate ? "hibernate" : "sleep";
                    item["delay_sec"] = job.suspendDelaySec;
                    item["hibernate"] = job.suspendHibernate;
                }
                jobs.push_back(std::move(item));
            }
        }
    }

    TaskQueueCounts counts = GetJobCounts();
    int activeJobs = counts.running + counts.waiting +
                     (counts.pending - counts.neverRunnablePending);
    if (activeJobs < 0) activeJobs = 0;
    int runningProcesses = 0;
    if (pm_) {
        for (const auto& snap : pm_->GetAllSnapshots()) {
            if (snap.state == ProcState::Running) ++runningProcesses;
        }
    }

    nlohmann::json out;
    out["ok"] = true;
    out["armed"] = !jobs.empty();
    out["phase"] = phase.empty() ? "idle" : phase;
    out["action"] = action.empty() ? "none" : action;
    out["jobs"] = std::move(jobs);
    out["delay_sec"] = delaySec;
    out["hibernate"] = action == "hibernate";
    out["shutdown"] = action == "shutdown";
    out["other_jobs"] = activeJobs;
    out["other_processes"] = runningProcesses;
    if (phase == "countdown" && dueMs > 0) {
        int64_t now = static_cast<int64_t>(GetNowTickMs());
        int64_t remaining = dueMs > now ? dueMs - now : 0;
        out["seconds_until_power"] = (remaining + 999) / 1000;
    } else {
        out["seconds_until_power"] = 0;
    }
    return out.dump();
}

bool TaskQueueManager::FindUnfinishedJobByCommand(const std::string& command, std::string& outJobId) const {
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(mutex_));
    for (const auto& [name, tq] : queues_) {
        if (name == "plain") continue;
        for (const auto& job : tq.jobs) {
            if (job.state == "succeeded" || job.state == "cancelled" || job.state == "takenOver" ||
                job.state == "closed") continue;
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
    // Events of a job that belongs to a task carry the task id (100-2b). Empty taskId: left out.
    for (const auto& j : tq.jobs) {
        if (j.id != jobId) continue;
        if (!j.taskId.empty()) {
            oss << ",\"task_id\":\"" << JsonEscape(j.taskId) << "\"";
            LogAttemptToTaskLocked(j, type, attempt);
        }
        break;
    }
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

void TaskQueueManager::LogAttemptToTaskLocked(const TaskQueueJob& job, const std::string& eventType,
                                              int64_t attempt) {
    const bool attemptEnd = eventType == "jobSucceeded" || eventType == "jobAttemptFailed" ||
                            eventType == "jobDeferred" || eventType == "jobWaitingForPermit";
    const bool gaveUp = eventType == "jobFailed";
    if (!attemptEnd && !gaveUp) return;

    const TaskQueueAttempt* a = nullptr;
    for (const auto& att : job.attempts) {
        if (att.n == attempt) a = &att;
    }
    // Waiting for a dead engine follows jobAttemptFailed for the same attempt: already logged.
    if (eventType == "jobDeferred" && a && a->reason == "engineWaiting") return;
    std::string result = eventType == "jobSucceeded" ? "succeeded"
                       : eventType == "jobWaitingForPermit" ? "waitingForPermit"
                       : (a && !a->reason.empty()) ? a->reason : "failed";
    nlohmann::json extra = nlohmann::json::object();
    extra["attempt"] = attempt;
    extra["result"] = result;
    std::ostringstream text;
    if (gaveUp) {
        text << "gave up after attempt " << attempt;
    } else {
        text << "attempt " << attempt << ": " << result;
    }
    if (a && eventType != "jobSucceeded") {
        std::vector<std::string> parts;
        if (a->exitCode >= 0 && !a->end.empty()) {
            extra["exitCode"] = a->exitCode;
            parts.push_back("exit " + std::to_string(a->exitCode));
        }
        if (!a->failure.failureClass.empty()) {
            extra["failureClass"] = a->failure.failureClass;
            parts.push_back(a->failure.failureClass);
        }
        if (!parts.empty()) {
            text << " (";
            for (size_t i = 0; i < parts.size(); ++i) text << (i ? ", " : "") << parts[i];
            text << ")";
        }
    }
    TaskLog(job.taskId, gaveUp ? "job_failed" : "attempt_finished", job.id, "", text.str(), "", &extra);
    if ((eventType == "jobSucceeded" || gaveUp) && !job.directionId.empty()) {
        MaybeLogDirectionFinishedLocked(job.directionId, job.taskId);
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
    jobIndexDirty_ = true;
}

void TaskQueueManager::InitRevLocked() {
    uint64_t lastRev = 0;
    const std::wstring revPath = PathJoinWide(GetEffectiveDataDirectoryWide(), L"taskqueue_rev.json");
    const std::string revText = ReadFileUtf8Wide(revPath);
    if (!revText.empty()) {
        nlohmann::json doc = nlohmann::json::parse(revText, nullptr, false);
        if (doc.is_object() && doc.contains("last_rev") && doc["last_rev"].is_number_unsigned()) {
            lastRev = doc["last_rev"].get<uint64_t>();
        }
    }
    uint64_t maxJobRev = 0;
    for (const auto& [name, tq] : queues_) {
        (void)name;
        for (const auto& job : tq.jobs) {
            if (job.rev > maxJobRev) maxJobRev = job.rev;
        }
    }
    const uint64_t maxTaskRev = taskStore_.MaxRev();
    const uint64_t maxDirRev = directionStore_.MaxRev();
    currentRev_ = std::max({lastRev, maxJobRev, maxTaskRev, maxDirRev});
}

void TaskQueueManager::PersistLastRevLocked() {
    const std::wstring revPath = PathJoinWide(GetEffectiveDataDirectoryWide(), L"taskqueue_rev.json");
    WriteFileAtomicWide(revPath, "{\"last_rev\":" + std::to_string(currentRev_) + "}\n");
}

uint64_t TaskQueueManager::BumpRevLocked() {
    ++currentRev_;
    PersistLastRevLocked();
    return currentRev_;
}

void TaskQueueManager::MarkJobChangedLocked(TaskQueueJob& job) {
    job.rev = BumpRevLocked();
    job.cachedJsonFull_.clear();
    job.cachedJsonNoInstr_.clear();
}

void TaskQueueManager::RecordJobTombstoneLocked(const std::string& jobId) {
    if (jobId.empty()) return;
    const uint64_t delRev = BumpRevLocked();
    if (maxTombstones_ == 0) {
        if (delRev > evictedBeforeRev_) evictedBeforeRev_ = delRev;
        return;
    }
    while (tombstones_.size() >= maxTombstones_) {
        if (tombstones_.front().rev > evictedBeforeRev_) {
            evictedBeforeRev_ = tombstones_.front().rev;
        }
        tombstones_.pop_front();
    }
    tombstones_.push_back({jobId, delRev});
}

uint64_t TaskQueueManager::GetCurrentRev() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return currentRev_;
}

void TaskQueueManager::SetMaxTombstonesForTest(size_t maxTombstones) {
    std::lock_guard<std::mutex> lock(mutex_);
    maxTombstones_ = maxTombstones;
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
    if (job.state != "pending" || (job.holdReason.empty() && job.retryAtMs == 0)) {
        const std::string& cached = includeInstructions ? job.cachedJsonFull_ : job.cachedJsonNoInstr_;
        if (!cached.empty()) return cached;
    }
    std::ostringstream oss;
    oss << "{\"id\":\"" << JsonEscape(job.id) << "\""
        << ",\"taskqueue\":\"" << JsonEscape(job.taskqueue) << "\""
        << ",\"name\":\"" << JsonEscape(job.name) << "\""
        << ",\"summary\":\"" << JsonEscape(job.summary) << "\"";
    if (!job.summaryLang.empty()) oss << ",\"summaryLang\":\"" << JsonEscape(job.summaryLang) << "\"";
    if (!job.summarySource.empty()) {
        oss << ",\"summarySource\":{\"lang\":\"" << JsonEscape(job.summarySourceLang) << "\",\"text\":\""
            << JsonEscape(job.summarySource) << "\"}";
    }
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
        << ",\"needs\":[";
    for (size_t i = 0; i < job.needs.size(); ++i) {
        if (i) oss << ",";
        oss << "{\"kind\":\"" << JsonEscape(job.needs[i].kind)
            << "\",\"target\":\"" << JsonEscape(job.needs[i].target) << "\"}";
    }
    oss << "]"
        << ",\"on_permission_denied\":\"" << JsonEscape(job.onPermissionDenied) << "\""
        << ",\"wait_permits\":" << StrArrayToJson(job.waitPermits)
        << ",\"resume_session\":\"" << JsonEscape(job.resumeSession) << "\""
        << ",\"permission_resume_count\":" << job.permissionResumeCount
        << ",\"permission_resume_limit\":" << job.permissionResumeLimit
        << ",\"depends_on\":" << DepsToJson(job.dependsOn)
        << ",\"timeout_sec\":" << job.timeoutSec
        << ",\"estimate_sec\":" << job.estimateSec
        << ",\"idle\":{\"output_sec\":" << job.idle.outputSec << ",\"cpu_sec\":" << job.idle.cpuSec << ",\"first_event_sec\":" << job.idle.firstEventSec << "}"
        << ",\"max_attempts\":" << job.maxAttempts
        << ",\"retry_delay_sec\":" << job.retryDelaySec;
    if (job.retry.configured) oss << ",\"retry\":" << RetryToJson(job.retry);
    oss << ",\"defer\":" << DeferToJson(job.defer)
        << ",\"success\":" << SuccessToJson(job.success)
        << ",\"kill_exclude\":" << StrArrayToJson(job.killExclude)
        << ",\"resources\":" << ResourcesToJson(job.resources)
        << ",\"memory\":" << MemoryToJson(job.memory)
        << ",\"priority\":" << job.priority
        << ",\"hidden\":" << (job.hidden ? "true" : "false")
        << ",\"paused\":" << (job.paused ? "true" : "false")
        << ",\"suspend_when_done\":" << (job.suspendWhenDone ? "true" : "false")
        << ",\"suspend_delay_sec\":" << job.suspendDelaySec
        << ",\"suspend_hibernate\":" << (job.suspendHibernate ? "true" : "false")
        << ",\"shutdown_when_done\":" << (job.shutdownWhenDone ? "true" : "false")
        << ",\"shutdown_delay_sec\":" << job.shutdownDelaySec
        << ",\"state\":\"" << JsonEscape(job.state) << "\"";
    if (job.takeover.present) {
        oss << ",\"takeover\":{\"by\":\"" << JsonEscape(job.takeover.by)
            << "\",\"reason\":\"" << JsonEscape(job.takeover.reason)
            << "\",\"at\":\"" << JsonEscape(job.takeover.at) << "\"";
        if (!job.takeover.completedAt.empty())
            oss << ",\"completed_at\":\"" << JsonEscape(job.takeover.completedAt) << "\"";
        if (!job.takeover.report.empty())
            oss << ",\"report\":\"" << JsonEscape(job.takeover.report) << "\"";
        if (!job.takeover.result.empty())
            oss << ",\"result\":\"" << JsonEscape(job.takeover.result) << "\"";
        if (!job.takeover.completionReason.empty())
            oss << ",\"completion_reason\":\"" << JsonEscape(job.takeover.completionReason) << "\"";
        oss << "}";
    }
    if (!job.supersedes.empty())
        oss << ",\"supersedes\":\"" << JsonEscape(job.supersedes) << "\"";
    if (!job.taskId.empty())
        oss << ",\"task_id\":\"" << JsonEscape(job.taskId) << "\"";
    if (!job.directionId.empty())
        oss << ",\"direction_id\":\"" << JsonEscape(job.directionId) << "\"";
    if (!job.supersededBy.empty())
        oss << ",\"superseded_by\":\"" << JsonEscape(job.supersededBy) << "\"";
    if (!job.supersedeReason.empty())
        oss << ",\"supersede_reason\":\"" << JsonEscape(job.supersedeReason) << "\"";
    oss << ",\"attempts\":" << AttemptsToJson(job.attempts)
        << ",\"blocked_by\":";
    if (job.blockedBy.empty()) oss << "null";
    else oss << "\"" << JsonEscape(job.blockedBy) << "\"";
    oss << ",\"created_at\":\"" << JsonEscape(job.createdAt) << "\""
        << ",\"updated_at\":\"" << JsonEscape(job.updatedAt) << "\""
        << ",\"notes\":\"" << JsonEscape(job.notes) << "\""
        << ",\"engine_switch\":" << (job.engineSwitch ? "true" : "false");
    const std::string hold = JobHoldReasonLocked(job, static_cast<int64_t>(GetNowTickMs()));
    if (!hold.empty()) oss << ",\"hold_reason\":\"" << JsonEscape(hold) << "\"";
    if (!job.bt.id.empty()) {
        oss << ",\"bt\":{\"id\":\"" << JsonEscape(job.bt.id)
            << "\",\"node\":\"" << JsonEscape(job.bt.node)
            << "\",\"run\":" << job.bt.run << "}";
    }
    if (!job.origin.empty()) oss << ",\"origin\":" << OriginToJson(job.origin);
    std::string tf = TimingFeedbackToJson(job);
    if (!tf.empty()) oss << ",\"timing_feedback\":" << tf;
    oss << ",\"rev\":" << job.rev << "}";
    std::string result = oss.str();
    if (hold.empty()) {
        if (includeInstructions) job.cachedJsonFull_ = result;
        else job.cachedJsonNoInstr_ = result;
    }
    return result;
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

std::string TaskQueueManager::BtOriginJson(const std::string& btId) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = btOrigins_.find(btId);
    if (it == btOrigins_.end()) return "";
    return OriginToJson(it->second);
}

void TaskQueueManager::RebuildJobIndexLocked() const {
    jobIndex_.clear();
    for (const auto& [name, tq] : queues_) {
        for (size_t i = 0; i < tq.jobs.size(); ++i) {
            jobIndex_.emplace(tq.jobs[i].id, JobIndexEntry{name, i});
        }
    }
    jobIndexDirty_ = false;
}

TaskQueueJob* TaskQueueManager::FindJob(const std::string& id, TaskQueue** outQueue) {
    if (jobIndexDirty_) RebuildJobIndexLocked();
    auto entry = jobIndex_.find(id);
    if (entry == jobIndex_.end()) return nullptr;
    auto q = queues_.find(entry->second.queue);
    if (q == queues_.end() || entry->second.index >= q->second.jobs.size() ||
        q->second.jobs[entry->second.index].id != id) {
        jobIndexDirty_ = true;
        RebuildJobIndexLocked();
        entry = jobIndex_.find(id);
        if (entry == jobIndex_.end()) return nullptr;
        q = queues_.find(entry->second.queue);
        if (q == queues_.end() || entry->second.index >= q->second.jobs.size()) return nullptr;
    }
    if (outQueue) *outQueue = &q->second;
    return &q->second.jobs[entry->second.index];
}

const TaskQueueJob* TaskQueueManager::FindJobConst(const std::string& id) const {
    if (jobIndexDirty_) RebuildJobIndexLocked();
    auto entry = jobIndex_.find(id);
    if (entry == jobIndex_.end()) return nullptr;
    auto q = queues_.find(entry->second.queue);
    if (q == queues_.end() || entry->second.index >= q->second.jobs.size() ||
        q->second.jobs[entry->second.index].id != id) {
        jobIndexDirty_ = true;
        RebuildJobIndexLocked();
        entry = jobIndex_.find(id);
        if (entry == jobIndex_.end()) return nullptr;
        q = queues_.find(entry->second.queue);
        if (q == queues_.end() || entry->second.index >= q->second.jobs.size()) return nullptr;
    }
    return &q->second.jobs[entry->second.index];
}


const TaskQueueJob* TaskQueueManager::ResolveDepJobLocked(const std::string& depId, const TaskQueue* extraQueue) const {
    auto findIn = [this, extraQueue](const std::string& id) -> const TaskQueueJob* {
        if (const TaskQueueJob* j = FindJobConst(id)) return j;
        if (extraQueue) {
            for (const auto& tj : extraQueue->jobs) {
                if (tj.id == id) return &tj;
            }
        }
        return nullptr;
    };
    const TaskQueueJob* dep = findIn(depId);
    if (!dep) return nullptr;
    for (int step = 0; step < 1000 && !dep->supersededBy.empty(); ++step) {
        const TaskQueueJob* next = findIn(dep->supersededBy);
        if (!next) break;
        dep = next;
    }
    return dep;
}

namespace {

struct LineageFailureNode {
    std::string id;
    std::string state;
    std::string supersedes;
    std::string supersededBy;
};

void CollectLineageNodesForTask(const std::map<std::string, TaskQueue>& queues,
                                const std::string& taskId,
                                const nlohmann::json& record,
                                bool allQueues,
                                std::vector<LineageFailureNode>& outNodes) {
    std::unordered_set<std::string> seenIds;
    for (const auto& [name, tq] : queues) {
        (void)name;
        for (const auto& job : tq.jobs) {
            if (!allQueues) {
                const bool inTask = job.taskId == taskId || (job.taskId.empty() && job.id == taskId);
                if (!inTask) continue;
            }
            if (seenIds.insert(job.id).second) {
                outNodes.push_back({job.id, job.state, job.supersedes, job.supersededBy});
            }
        }
    }
    if (record.is_object() && record.contains("removedJobs") && record["removedJobs"].is_array()) {
        for (const auto& r : record["removedJobs"]) {
            if (!r.is_object()) continue;
            const std::string rid = r.value("id", std::string());
            if (rid.empty() || !seenIds.insert(rid).second) continue;
            outNodes.push_back({rid, r.value("state", std::string()),
                                r.value("supersedes", std::string()),
                                r.value("supersededBy", std::string())});
        }
    }
}

int CountLineageFailuresFrom(const std::vector<LineageFailureNode>& nodes,
                             const std::string& startId,
                             std::unordered_set<std::string>* outVisited = nullptr) {
    if (startId.empty()) return 0;
    std::unordered_map<std::string, const LineageFailureNode*> byId;
    for (const auto& n : nodes) byId.emplace(n.id, &n);
    if (byId.find(startId) == byId.end()) return 0;
    std::unordered_set<std::string> localVisited;
    std::unordered_set<std::string>& visited = outVisited ? *outVisited : localVisited;
    std::vector<std::string> stack{startId};
    int failed = 0;
    while (!stack.empty()) {
        std::string cur = std::move(stack.back());
        stack.pop_back();
        if (!visited.insert(cur).second) continue;
        const auto it = byId.find(cur);
        if (it == byId.end()) continue;
        const LineageFailureNode* node = it->second;
        if (node->state == "failed") ++failed;
        if (!node->supersedes.empty() && byId.count(node->supersedes)) stack.push_back(node->supersedes);
        if (!node->supersededBy.empty() && byId.count(node->supersededBy)) stack.push_back(node->supersededBy);
    }
    return failed;
}

} // namespace

std::string TaskQueueManager::TaskSummaryJsonLocked(const TaskQueueJob& job) const {
    const std::string effectiveTaskId = job.taskId.empty() ? job.id : job.taskId;
    nlohmann::json taskObj = nlohmann::json::object();
    taskObj["id"] = effectiveTaskId;
    nlohmann::json record;
    if (taskStore_.Get(effectiveTaskId, record)) {
        taskObj["title"] = record.value("title", std::string());
    }
    const nlohmann::json status = TaskStateLocked(effectiveTaskId);
    taskObj["state"] = status.value("state", std::string());
    return taskObj.dump();
}

std::string TaskQueueManager::TaskSummaryJsonCachedLocked(const TaskQueueJob& job) const {
    if (taskSummaryCacheRev_ != currentRev_) {
        taskSummaryCache_.clear();
        taskSummaryCacheRev_ = currentRev_;
    }
    const std::string effectiveTaskId = job.taskId.empty() ? job.id : job.taskId;
    auto it = taskSummaryCache_.find(effectiveTaskId);
    if (it != taskSummaryCache_.end()) return it->second;
    std::string json = TaskSummaryJsonLocked(job);
    taskSummaryCache_.emplace(effectiveTaskId, json);
    return json;
}

bool TaskQueueManager::IsDepSatisfied(const TaskQueueJob& dep, const std::string& on) {
    if (dep.state == "takenOver") {
        // The supervisor owns the work now. Dependents wait (stay pending) until the takeover
        // is completed; only a recorded "succeeded" result satisfies a success dependency.
        if (!dep.takeover.present || dep.takeover.result.empty()) return false;
        if (on == "done") return true;
        return dep.takeover.result == "succeeded";
    }
    if (on == "done") {
        return dep.state == "succeeded" || dep.state == "failed" || dep.state == "cancelled" ||
               dep.state == "closed";
    }
    return dep.state == "succeeded";
}

// ---------------------------------------------------------------------------
// Resource locks (doc/obsoleted/resource-locks.md)
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
    if (job.paused) return false;
    if (job.state != "pending" || job.retryAtMs > nowMs) return false;
    for (const auto& d : job.dependsOn) {
        const TaskQueueJob* dep = ResolveDepJobLocked(d.id);
        if (!dep || !IsDepSatisfied(*dep, d.on)) return false;
    }
    // 091-6: a job on a down engine stays pending (held), never failed.
    if (!JobHoldReasonLocked(job, nowMs).empty()) return false;
    return true;
}

static void ReplaceTextAll(std::string& text, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
}

bool TaskQueueManager::JobEngineModel(const TaskQueueJob& job, std::string& engine,
                                      std::string& model) {
    return ParseCommandEngineModel(job.command, job.name, engine, model);
}

std::string TaskQueueManager::JobHoldReasonLocked(const TaskQueueJob& job, int64_t nowMs) const {
    std::string engine, model;
    if (!JobEngineModel(job, engine, model)) return "";
    engine_health::Store store;
    std::string err;
    if (!engine_health::Load(engine_health::DefaultPath(), store, err)) return "";
    const engine_health::Entry* e = engine_health::Find(store, engine, model);
    if (!e || e->status != "down") return "";
    (void)nowMs;
    return e->HoldReason();
}

bool TaskQueueManager::TryEngineSwitchLocked(TaskQueueJob& job, const std::string& engine,
                                             const std::string& model, int64_t nowMs) {
    if (!job.engineSwitch) return false;
    const std::string& cmd = job.command;
    const bool switchable = cmd.find("{engine}") != std::string::npos ||
                            cmd.find("{model}") != std::string::npos ||
                            cmd.find("-Engine") != std::string::npos ||
                            cmd.find("-Model") != std::string::npos;
    if (!switchable) return false;
    llm::Config cfg;
    std::string err;
    if (!llm::Load(llm::DefaultPath(), cfg, err)) return false;
    const std::vector<llm::Entry> usable = llm::Usable(cfg, llm::kWorker);
    if (usable.empty()) return false;
    engine_health::Store health;
    if (!engine_health::Load(engine_health::DefaultPath(), health, err)) return false;

    size_t currentIndex = usable.size();
    for (size_t i = 0; i < usable.size(); ++i) {
        if (usable[i].engine == engine && (usable[i].model == model || model.empty())) {
            currentIndex = i;
            break;
        }
    }
    if (currentIndex == usable.size()) return false;
    for (size_t i = currentIndex + 1; i < usable.size(); ++i) {
        const auto& candidate = usable[i];
        if (candidate.engine == engine && candidate.model == model) continue;
        if (engine_health::IsDown(health, candidate.engine, candidate.model)) continue;
        const std::string oldEngine = engine;
        const std::string oldModel = model;
        ReplaceTextAll(job.command, oldEngine, candidate.engine);
        if (!oldModel.empty()) ReplaceTextAll(job.command, oldModel, candidate.model);
        job.holdReason.clear();
        std::ostringstream note;
        note << "[engineSwitch] worker list #" << candidate.rank << " (approved via "
             << candidate.approvedVia << "): " << oldEngine << "/" << oldModel << " -> "
             << candidate.engine << "/" << candidate.model << " (engine down)";
        job.notes = job.notes.empty() ? note.str() : (job.notes + "\n" + note.str());
        job.updatedAt = GetCurrentTimestampStr();
        (void)nowMs;
        return true;
    }
    return false;
}

void TaskQueueManager::RefreshEngineHealthLocked(int64_t nowMs) {
    if (nowMs - lastHealthCheckTickMs_ < 1000) return;
    lastHealthCheckTickMs_ = nowMs;
    engine_health::RunDueHealthProbes(engine_health::DefaultPath(), engine_health::NowEpochMs(),
                                      engine_health::LoadSettings());
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

namespace {

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
// Worker idle defaults & detection (090-2)
// ---------------------------------------------------------------------------

struct WorkerIdleDefaults {
    int firstEventSec = 180;
    int outputSec = 600;
};

static WorkerIdleDefaults LoadWorkerIdleDefaults() {
    WorkerIdleDefaults defs;
    std::string content = ReadFileUtf8Wide(PathJoinWide(GetEffectiveDataDirectoryWide(), L"config.json"));
    if (!content.empty()) {
        nlohmann::json cfg = nlohmann::json::parse(content, nullptr, false);
        if (cfg.is_object()) {
            if (cfg.contains("llm_idle_timeout_sec") && cfg["llm_idle_timeout_sec"].is_number()) {
                int v = cfg["llm_idle_timeout_sec"].get<int>();
                if (v >= 0) defs.outputSec = v;
            }
            if (cfg.contains("taskqueue") && cfg["taskqueue"].is_object()) {
                const auto& tq = cfg["taskqueue"];
                if (tq.contains("worker_idle_defaults") && tq["worker_idle_defaults"].is_object()) {
                    const auto& wid = tq["worker_idle_defaults"];
                    if (wid.contains("first_event_sec") && wid["first_event_sec"].is_number()) {
                        int v = wid["first_event_sec"].get<int>();
                        if (v >= 0) defs.firstEventSec = v;
                    }
                    if (wid.contains("output_sec") && wid["output_sec"].is_number()) {
                        int v = wid["output_sec"].get<int>();
                        if (v >= 0) defs.outputSec = v;
                    }
                }
            }
        }
    }
    return defs;
}

static bool IsWorkerJob(const TaskQueueJob& job, const std::string& rawJson = "") {
    if (!DetectAgentType(job.command, job.name).empty()) return true;
    if (job.command.find("--json-stream") != std::string::npos) return true;
    auto adapterIt = job.env.find("HANCHO_ADAPTER");
    if (adapterIt != job.env.end() && !adapterIt->second.empty()) return true;
    if (!rawJson.empty()) {
        if (JsonFieldBool(rawJson, "json_stream", false)) return true;
        std::string eng = JsonFieldStr(rawJson, "engine", "");
        if (!eng.empty() && eng != "none") return true;
        std::string agent = JsonFieldStr(rawJson, "agent", "");
        if (!agent.empty()) return true;
    }
    std::string lcmd = LowerAscii(job.command);
    if (lcmd.find("codestral") != std::string::npos) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Job definition parsing + validation
// ---------------------------------------------------------------------------

// Card 106-5: copies the result of NormalizeEnglishField on a job summary back into the job.
static void CopySummaryNormalization(TaskQueueJob& job, const nlohmann::json& obj) {
    job.summary = obj.value("summary", job.summary);
    job.summaryLang = obj.value("summaryLang", job.summaryLang);
    auto src = obj.find("summarySource");
    if (src != obj.end() && src->is_object()) {
        job.summarySourceLang = src->value("lang", std::string());
        job.summarySource = src->value("text", std::string());
    }
}

// Card 106-5: applies a landed English translation to a foreign job summary, or asks for one again
// while it is pending. Returns true when the summary changed.
static bool RefreshJobSummaryTranslation(TaskQueueJob& job) {
    if (job.summaryLang.empty() || job.summaryLang == "en") return false;
    const std::string before = job.summary;
    nlohmann::json obj = {{"summary", job.summary}};
    NormalizeEnglishField(obj, "summary", job.summaryLang, "", ActiveTranslationStore());
    CopySummaryNormalization(job, obj);
    return job.summary != before;
}

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
    // Card 106-5: a summary that is not English is kept as the original, and its English translation
    // replaces it once the core has one. source_lang names the language of Latin-script text.
    const std::string summarySourceLang = JsonFieldStr(j, "source_lang", "");
    if (IsForeignText(job.summary, summarySourceLang)) {
        nlohmann::json obj = {{"summary", job.summary}};
        NormalizeEnglishField(obj, "summary", summarySourceLang, "", ActiveTranslationStore());
        CopySummaryNormalization(job, obj);
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
    const bool hasExplicitIdle = HasJsonKey(j, "idle");
    std::string idleObj = JsonFieldObj(j, "idle");
    if (hasExplicitIdle && (JsonFieldInt(j, "idle", -1) == 0 || (idleObj == "{}" && JsonFieldStr(j, "idle", "") == "0"))) {
        job.idle.outputSec = 0;
        job.idle.cpuSec = 0;
        job.idle.firstEventSec = 0;
        job.idle.explicitZero = true;
    } else {
        job.idle.outputSec = static_cast<int>(JsonFieldInt(idleObj, "output_sec", 0));
        job.idle.cpuSec = static_cast<int>(JsonFieldInt(idleObj, "cpu_sec", 0));
        job.idle.firstEventSec = static_cast<int>(JsonFieldInt(idleObj, "first_event_sec", 0));
        if (hasExplicitIdle && job.idle.outputSec == 0 && job.idle.firstEventSec == 0) {
            job.idle.explicitZero = true;
        }
    }
    if (job.idle.outputSec < 0 || job.idle.cpuSec < 0 || job.idle.firstEventSec < 0) {
        err = "job '" + job.id + "': idle values must not be negative";
        return false;
    }
    job.engineSwitch = !HasJsonKey(j, "engine_switch") || JsonFieldBool(j, "engine_switch", true);
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
    job.success.contentFile = JsonFieldStr(successObj, "content_file", "");
    job.success.minBytes = JsonFieldInt(successObj, "min_bytes", 0);
    job.success.contentRegex = JsonFieldStr(successObj, "content_regex", "");
    job.success.engineResult = JsonFieldBool(successObj, "engine_result", false);
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
    job.hidden = JsonFieldBool(j, "hidden", false);
    job.suspendWhenDone = JsonFieldBool(j, "suspend_when_done", false);
    job.suspendDelaySec = static_cast<int>(JsonFieldInt(j, "suspend_delay_sec", 0));
    if (job.suspendDelaySec < 0) {
        err = "job '" + job.id + "': suspend_delay_sec must not be negative";
        return false;
    }
    if (job.suspendDelaySec > 86400) {
        err = "job '" + job.id + "': suspend_delay_sec must be 86400 or less";
        return false;
    }
    job.suspendHibernate = JsonFieldBool(j, "suspend_hibernate", false);
    job.shutdownWhenDone = JsonFieldBool(j, "shutdown_when_done", false);
    job.shutdownDelaySec = static_cast<int>(JsonFieldInt(j, "shutdown_delay_sec", 0));
    if (job.shutdownDelaySec < 0) {
        err = "job '" + job.id + "': shutdown_delay_sec must not be negative";
        return false;
    }
    if (job.shutdownDelaySec > 86400) {
        err = "job '" + job.id + "': shutdown_delay_sec must be 86400 or less";
        return false;
    }
    job.env = ParseJsonStringMap(JsonFieldObj(j, "env"));
    for (const auto& needJson : JsonFieldObjArray(j, "needs")) {
        TaskQueueNeed need;
        need.kind = JsonFieldStr(needJson, "kind", "");
        need.target = JsonFieldStr(needJson, "target", "");
        if (!permit::IsValidKind(need.kind)) {
            err = "job '" + job.id + "': needs.kind must be read_path, write_path, command or network";
            return false;
        }
        if (need.target.empty()) {
            err = "job '" + job.id + "': needs.target is required";
            return false;
        }
        job.needs.push_back(need);
    }
    job.onPermissionDenied = JsonFieldStr(j, "on_permission_denied", "");
    if (!job.onPermissionDenied.empty() && job.onPermissionDenied != "fail" &&
        job.onPermissionDenied != "wait") {
        err = "job '" + job.id + "': on_permission_denied must be fail or wait";
        return false;
    }
    job.permissionResumeLimit = static_cast<int>(JsonFieldInt(j, "permission_resume_limit", 2));
    if (job.permissionResumeLimit < 0) job.permissionResumeLimit = 0;
    // Task lineage: only `supersedes` comes from the caller; task_id / superseded_by are set by Hancho.
    job.supersedes = JsonFieldStr(j, "supersedes", "");
    // A reason only means something with `supersedes`; without it the value is dropped silently.
    job.supersedeReason = JsonFieldStr(j, "supersede_reason", "");
    if (job.supersedes.empty()) {
        job.supersedeReason.clear();
    } else if (job.supersedeReason.size() > 2000) {
        err = "job '" + job.id + "': supersede_reason exceeds 2000 bytes";
        return false;
    }
    if (!hasExplicitIdle && IsWorkerJob(job, j)) {
        WorkerIdleDefaults defs = LoadWorkerIdleDefaults();
        job.idle.firstEventSec = defs.firstEventSec;
        job.idle.outputSec = defs.outputSec;
        job.idle.cpuSec = 1;
    }
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
                                           const std::string& originJson, const std::string& taskId) {
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

    // Task records (100-2a): the task must exist and still be open.
    if (!taskId.empty()) {
        nlohmann::json taskRecord;
        if (!taskStore_.Get(taskId, taskRecord)) {
            outError = "task not found: " + taskId;
            return BuildError(outError);
        }
        if (taskRecord.contains("closed") && taskRecord["closed"].is_object()) {
            outError = "task '" + taskId + "' is closed";
            return BuildError(outError);
        }
    }
    std::vector<TaskQueueJob> newJobs;
    for (const auto& jo : jobObjs) {
        TaskQueueJob job;
        std::string err;
        if (!ParseNewJob(jo, taskqueue, tq.defaultMaxAttempts, tq.maxTimeoutSec, job, err)) {
            outError = err;
            return BuildError(outError);
        }
        job.origin = origin;
        // ParseNewJob reads task_id / direction_id / superseded_by from the JSON, which an external
        // add must not honor (100-1: lineage is set by `supersedes`; 100-2b: only `task` links a
        // job; 103-1: direction_id is assigned by Hancho).
        job.taskId.clear();
        job.directionId.clear();
        job.supersededBy.clear();
        if (job.id.rfind("task_", 0) == 0) {
            outError = "job '" + job.id + "': ids starting with 'task_' are reserved for tasks";
            return BuildError(outError);
        }
        if (!taskId.empty()) {
            if (job.hidden) {
                outError = "job '" + job.id + "': a hidden job cannot belong to a task";
                return BuildError(outError);
            }
            job.taskId = taskId;
        }
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

    // Task lineage (100-1): check every `supersedes` link before any queue is changed.
    struct LineageLink {
        std::string headId;
        std::string newId;
    };
    std::vector<LineageLink> lineageLinks;
    for (auto& job : newJobs) {
        if (job.supersedes.empty()) continue;
        const std::string target = job.supersedes;
        for (const auto& other : newJobs) {
            if (other.id == target) {
                outError = "job '" + job.id + "': supersedes must name an existing job";
                return BuildError(outError);
            }
        }
        const TaskQueueJob* found = FindJobConst(target);
        if (!found) {
            outError = "job '" + job.id + "': unknown supersedes job '" + target + "'";
            return BuildError(outError);
        }
        if (!found->bt.id.empty()) {
            outError = "job '" + job.id + "': cannot supersede a behavior tree job; use bt_retry";
            return BuildError(outError);
        }
        // Walk to the newest job of the lineage. A successor that no longer exists is skipped over.
        const TaskQueueJob* head = found;
        for (int step = 0; step < 1000 && !head->supersededBy.empty(); ++step) {
            const TaskQueueJob* next = FindJobConst(head->supersededBy);
            if (!next) break;
            head = next;
        }
        if (head->state != "failed" && head->state != "blocked" && head->state != "cancelled" &&
            head->state != "closed" && head->state != "takenOver") {
            outError = "job '" + job.id + "': can only supersede a finished, unsuccessful job ('" +
                       head->id + "' is " + head->state + ")";
            return BuildError(outError);
        }
        for (const auto& link : lineageLinks) {
            if (link.headId == head->id) {
                outError = "job '" + job.id + "': '" + head->id + "' is already superseded by '" +
                           link.newId + "' in this request";
                return BuildError(outError);
            }
        }
        // The lineage's task. A task given to this add must be the same one (100-2a).
        const std::string headTask = head->taskId.empty() ? head->id : head->taskId;
        if (!taskId.empty() && headTask != taskId) {
            outError = "job '" + job.id + "': supersedes '" + head->id + "', which belongs to task '" +
                       headTask + "', not '" + taskId + "'";
            return BuildError(outError);
        }
        nlohmann::json headTaskRecord;
        if (taskStore_.Get(headTask, headTaskRecord) && headTaskRecord.contains("closed") &&
            headTaskRecord["closed"].is_object()) {
            outError = "job '" + job.id + "': task '" + headTask + "' is closed";
            return BuildError(outError);
        }
        job.supersedes = head->id;
        job.taskId = headTask;
        lineageLinks.push_back({head->id, job.id});
    }

    // Card 103-1: when multiple jobs are added together without --task, share one implicit taskId
    // across them so they stay grouped as one task instead of splitting into separate implicit tasks.
    if (taskId.empty() && newJobs.size() > 1) {
        std::string sharedImplicitTaskId;
        for (const auto& j : newJobs) {
            if (!j.taskId.empty()) {
                sharedImplicitTaskId = j.taskId;
                break;
            }
        }
        if (sharedImplicitTaskId.empty()) sharedImplicitTaskId = newJobs[0].id;
        for (auto& j : newJobs) {
            if (!j.hidden && j.taskId.empty()) j.taskId = sharedImplicitTaskId;
        }
    }

    // Card 103-1: create one direction record per non-hidden taskqueue add call.
    std::string dirId;
    std::string dirTitle;
    bool anyVisible = false;
    for (const auto& j : newJobs) {
        if (!j.hidden) {
            anyVisible = true;
            if (dirTitle.empty()) dirTitle = j.summary.empty() ? j.id : j.summary;
        }
    }
    if (anyVisible) {
        if (dirTitle.size() > DirectionStore::kTitleMaxBytes) {
            dirTitle.resize(DirectionStore::kTitleMaxBytes);
        }
        dirId = directionStore_.NextId();
        nlohmann::json originRecord = nlohmann::json::parse(OriginToJson(origin), nullptr, false);
        if (!originRecord.is_object()) originRecord = nlohmann::json::object();
        const std::string now = GetCurrentTimestampStr();
        nlohmann::json dirRecord = {
            {"id", dirId},
            {"kind", "jobs"},
            {"taskId", taskId},
            {"btId", ""},
            {"createdAt", now},
            {"updatedAt", now},
            {"origin", originRecord},
            {"title", dirTitle},
            {"closed", nullptr},
            {"removedJobs", nlohmann::json::array()},
            {"rev", BumpRevLocked()}
        };
        std::string dirErr;
        if (directionStore_.Create(dirRecord, dirErr)) {
            for (auto& j : newJobs) {
                if (!j.hidden) j.directionId = dirId;
            }
        } else {
            dirId.clear();
        }
    }

    for (auto& j : newJobs) {
        MarkJobChangedLocked(j);
        tq.jobs.push_back(j);
    }
    for (auto& j : tq.jobs) {
        if (j.num <= 0) j.num = nextJobNum_++;
    }
    queues_[taskqueue] = tq;
    jobIndexDirty_ = true;
    // Point each superseded job at its successor. Done after the assignment above, so the
    // pointers come from the stored queues; a head in another queue is saved separately.
    for (const auto& link : lineageLinks) {
        TaskQueue* headQueue = nullptr;
        TaskQueueJob* head = FindJob(link.headId, &headQueue);
        if (!head || !headQueue) continue;
        head->supersededBy = link.newId;
        head->updatedAt = GetCurrentTimestampStr();
        MarkJobChangedLocked(*head);
        if (headQueue != &queues_[taskqueue]) SaveQueue(*headQueue);
    }
    ReevaluateAll();
    SaveQueue(queues_[taskqueue]);
    cv_.notify_all();
    // Task log (100-2a). Written after the queue is saved, so a log failure never undoes the add.
    if (!dirId.empty() && !taskId.empty()) {
        nlohmann::json extra = {{"directionId", dirId}, {"directionKind", "jobs"}, {"title", dirTitle}};
        TaskLog(taskId, "direction_started", "", "", dirTitle, origin.session, &extra);
    }
    for (const auto& job : newJobs) {
        if (job.taskId.empty()) continue;
        TaskLog(job.taskId, "job_added", job.id, "", "", origin.session);
        if (!job.supersedes.empty()) {
            TaskLog(job.taskId, "superseded", job.id, "", job.supersedeReason, origin.session);
        }
    }

    std::ostringstream oss;
    oss << "{\"ok\":true,\"taskqueue\":\"" << JsonEscape(taskqueue) << "\"";
    if (!dirId.empty()) oss << ",\"direction_id\":\"" << JsonEscape(dirId) << "\"";
    oss << ",\"added\":[";
    for (size_t i = 0; i < newJobs.size(); ++i) {
        if (i > 0) oss << ",";
        oss << "\"" << JsonEscape(newJobs[i].id) << "\"";
    }
    oss << "]";
    // Warnings for this add. The permit warnings below go into the same array.
    nlohmann::json warnings = nlohmann::json::array();
    {
        bool anyLineage = false;
        for (const auto& job : newJobs) {
            if (job.supersedes.empty()) continue;
            oss << (anyLineage ? "," : ",\"lineage\":[") << "{\"id\":\"" << JsonEscape(job.id)
                << "\",\"supersedes\":\"" << JsonEscape(job.supersedes) << "\",\"task_id\":\""
                << JsonEscape(job.taskId) << "\"";
            if (!job.supersedeReason.empty())
                oss << ",\"reason\":\"" << JsonEscape(job.supersedeReason) << "\"";
            oss << "}";
            anyLineage = true;
            if (job.supersedeReason.empty())
                warnings.push_back(nlohmann::json{{"job", job.id}, {"warning", "supersede_without_reason"}});
        }
        if (anyLineage) oss << "]";
    }
    oss << ",\"concurrency\":" << queues_[taskqueue].concurrency
        << ",\"data_dir\":\"" << JsonEscape(WideToUtf8(GetEffectiveDataDirectoryWide())) << "\"";
    // 091-7: warn about declared permissions that have not been approved yet.
    {
        permit::Config cfg;
        std::string err;
        permit::Load(permit::DefaultPath(), cfg, err);
        const std::string scope = "taskqueue:" + taskqueue;
        for (const auto& job : newJobs) {
            for (const auto& need : job.needs) {
                if (permit::IsApproved(cfg, need.kind, need.target, "", scope)) continue;
                warnings.push_back(nlohmann::json{
                    {"code", "permit_not_approved"}, {"job", job.id},
                    {"kind", need.kind}, {"target", need.target},
                    {"suggestion", "hancho-cli permit request --kind " + need.kind +
                                   " --target \"" + need.target + "\" --reason \"...\""}});
            }
        }
        oss << ",\"warnings\":" << warnings.dump();
    }
    oss << "}";
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
    // BT jobs are not part of a task lineage; only taskqueue_add links jobs.
    // The exception is the task of the BT itself (task_id, set by BtManager from the BT's taskId):
    // every job the tree adds belongs to that task. External taskqueue_add ignores task_id.
    const std::string btTaskId = JsonFieldStr(jo, "task_id", "");
    const std::string btDirectionId = JsonFieldStr(jo, "direction_id", "");
    if (!btTaskId.empty() && job.hidden) {
        outError = "job '" + job.id + "': a hidden job cannot belong to a task";
        return BuildError(outError);
    }
    job.supersedes.clear();
    job.supersedeReason.clear();
    job.taskId = btTaskId;
    job.directionId = btDirectionId;
    job.supersededBy.clear();
    auto btOrigin = btOrigins_.find(ref.id);
    if (btOrigin != btOrigins_.end()) job.origin = btOrigin->second;
    if (FindJob(job.id, nullptr) != nullptr) {
        outError = "duplicate job id '" + job.id + "'";
        return BuildError(outError);
    }
    job.num = nextJobNum_++;
    MarkJobChangedLocked(job);
    tq.jobs.push_back(job);
    queues_[taskqueue] = tq;
    jobIndexDirty_ = true;
    ReevaluateAll();
    SaveQueue(queues_[taskqueue]);
    cv_.notify_all();

    return "{\"ok\":true,\"taskqueue\":\"" + JsonEscape(taskqueue) +
           "\",\"id\":\"" + JsonEscape(job.id) +
           "\",\"added\":[\"" + JsonEscape(job.id) + "\"]}";
}

namespace {

bool IsFinishedJobState(const std::string& state) {
    return state == "succeeded" || state == "cancelled" || state == "closed" || state == "takenOver";
}

bool IsFinishedTaskState(const std::string& state) {
    return state == "succeeded" || state == "cancelled" || state == "closed";
}

char AsciiLowerChar(unsigned char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : static_cast<char>(c);
}

bool ContainsCaseInsensitive(const std::string& haystack, const std::string& needle) {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    auto it = std::search(
        haystack.begin(), haystack.end(),
        needle.begin(), needle.end(),
        [](char a, char b) {
            return AsciiLowerChar(static_cast<unsigned char>(a)) ==
                   AsciiLowerChar(static_cast<unsigned char>(b));
        });
    return it != haystack.end();
}

void ParseBeforeCursor(const std::string& before, std::string& outTs, std::string& outId) {
    const size_t pos = before.rfind('|');
    if (pos == std::string::npos) {
        outTs = before;
        outId.clear();
    } else {
        outTs = before.substr(0, pos);
        outId = before.substr(pos + 1);
    }
}

std::string MakeBeforeCursor(const std::string& ts, const std::string& id) {
    return ts + "|" + id;
}

bool IsOlderThanCursor(const std::string& ts, const std::string& id,
                       const std::string& cursorTs, const std::string& cursorId) {
    if (ts != cursorTs) return ts < cursorTs;
    return id < cursorId;
}

} // namespace

std::string TaskQueueManager::TaskQueueList(const std::string& taskqueue, const std::string& state, int64_t sinceSec, std::string& outError,
                                            const std::string& origin, const std::string& btId, bool includeHidden,
                                            const std::string& taskId, int64_t finishedLimit,
                                            const std::string& before, const std::string& query) {
    outError.clear();
    const int64_t effectiveLimit = (finishedLimit < 0) ? GetTaskListRecentCount() : finishedLimit;
    std::lock_guard<std::mutex> lock(mutex_);
    // Card 106-5: the sweep. A job summary whose English translation has landed is replaced here, and
    // a pending one is asked for again. Done on each list, so no thread is needed. A changed queue is
    // saved and its version moves, so watchers see the new summary.
    for (auto& entry : queues_) {
        TaskQueue& queue = entry.second;
        bool changed = false;
        for (auto& job : queue.jobs) {
            if (RefreshJobSummaryTranslation(job)) {
                job.cachedJsonFull_.clear();
                job.cachedJsonNoInstr_.clear();
                changed = true;
            }
        }
        if (changed) {
            queue.version++;
            SaveQueue(queue);
            cv_.notify_all();
        }
    }

    struct TaskSearchInfo { std::string title; std::string intent; };
    struct DirSearchInfo { std::string title; std::string cardTitle; };
    std::unordered_map<std::string, TaskSearchInfo> taskSearch;
    std::unordered_map<std::string, DirSearchInfo> dirSearch;
    if (!query.empty()) {
        for (const auto& rec : taskStore_.All()) {
            const std::string tid = rec.value("id", std::string());
            if (!tid.empty()) {
                taskSearch[tid] = {rec.value("title", std::string()), rec.value("intent", std::string())};
            }
        }
        for (const auto& rec : directionStore_.All()) {
            const std::string did = rec.value("id", std::string());
            if (!did.empty()) {
                std::string cardTitle;
                if (rec.contains("card") && rec["card"].is_object()) {
                    cardTitle = rec["card"].value("title", std::string());
                }
                dirSearch[did] = {rec.value("title", std::string()), std::move(cardTitle)};
            }
        }
    }

    struct ListedJob { std::string queue; TaskQueueJob job; std::string finishedAt; };
    std::vector<ListedJob> unfinished;
    std::vector<ListedJob> finished;
    const int64_t cutoff = GetNowEpochSec() - sinceSec;
    for (const auto& [name, tq] : queues_) {
        if (!taskqueue.empty() && name != taskqueue) continue;
        for (const auto& job : tq.jobs) {
            if (job.hidden && !includeHidden) continue;
            if (!state.empty() && job.state != state) continue;
            if (!origin.empty() && !OriginMatches(job.origin, origin)) continue;
            if (!btId.empty() && job.bt.id != btId) continue;
            const std::string effectiveTask = job.taskId.empty() ? job.id : job.taskId;
            if (!taskId.empty() && effectiveTask != taskId) continue;
            if (sinceSec > 0) {
                const int64_t ts = ParseTimestampStrToEpoch(job.updatedAt);
                if (ts <= 0 || ts < cutoff) continue;
            }
            if (!query.empty()) {
                bool matched = ContainsCaseInsensitive(job.id, query) ||
                               ContainsCaseInsensitive(job.summary, query);
                if (!matched) {
                    auto tit = taskSearch.find(effectiveTask);
                    if (tit != taskSearch.end()) {
                        matched = ContainsCaseInsensitive(tit->second.title, query) ||
                                  ContainsCaseInsensitive(tit->second.intent, query);
                    }
                }
                if (!matched && !job.directionId.empty()) {
                    auto dit = dirSearch.find(job.directionId);
                    if (dit != dirSearch.end()) {
                        matched = ContainsCaseInsensitive(dit->second.title, query) ||
                                  ContainsCaseInsensitive(dit->second.cardTitle, query);
                    }
                }
                if (!matched) continue;
            }
            if (IsFinishedJobState(job.state)) {
                const std::string finAt = !job.updatedAt.empty() ? job.updatedAt : job.createdAt;
                finished.push_back({name, job, finAt});
            } else {
                unfinished.push_back({name, job, ""});
            }
        }
    }
    std::stable_sort(unfinished.begin(), unfinished.end(), [](const ListedJob& a, const ListedJob& b) {
        if (a.job.createdAt != b.job.createdAt) return a.job.createdAt > b.job.createdAt;
        return false;
    });
    std::stable_sort(finished.begin(), finished.end(), [](const ListedJob& a, const ListedJob& b) {
        if (a.finishedAt != b.finishedAt) return a.finishedAt > b.finishedAt;
        return a.job.id > b.job.id;
    });
    if (!before.empty()) {
        // Cursor pagination steps through older finished items without repeating unfinished ones.
        unfinished.clear();
        std::string cursorTs, cursorId;
        ParseBeforeCursor(before, cursorTs, cursorId);
        std::vector<ListedJob> older;
        older.reserve(finished.size());
        for (auto& item : finished) {
            if (IsOlderThanCursor(item.finishedAt, item.job.id, cursorTs, cursorId)) {
                older.push_back(std::move(item));
            }
        }
        finished = std::move(older);
    }
    bool more = false;
    int64_t remaining = 0;
    std::string nextBefore;
    if (effectiveLimit > 0 && static_cast<int64_t>(finished.size()) > effectiveLimit) {
        more = true;
        remaining = static_cast<int64_t>(finished.size()) - effectiveLimit;
        finished.resize(static_cast<size_t>(effectiveLimit));
        nextBefore = MakeBeforeCursor(finished.back().finishedAt, finished.back().job.id);
    }
    std::vector<ListedJob> listed;
    listed.reserve(unfinished.size() + finished.size());
    for (auto& item : unfinished) listed.push_back(std::move(item));
    for (auto& item : finished) listed.push_back(std::move(item));
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
            const TaskQueueJob* dep = ResolveDepJobLocked(d.id);
            if (!dep || !IsDepSatisfied(*dep, d.on)) { allSat = false; break; }
        }
        std::string resourceWait = ResourceWaitLocked(item.job, allSat, nowMs);
        std::string jj = JobToJson(item.job, false);
        if (!jj.empty() && jj.back() == '}') jj.pop_back();
        oss << jj << ",\"resource_wait\":";
        if (resourceWait.empty()) oss << "null";
        else oss << "\"" << JsonEscape(resourceWait) << "\"";
        oss << ",\"task\":" << TaskSummaryJsonCachedLocked(item.job) << "}";
    }
    oss << "],\"more\":" << (more ? "true" : "false")
        << ",\"next_before\":\"" << JsonEscape(nextBefore) << "\""
        << ",\"remaining\":" << remaining << "}";
    return oss.str();
}

std::string TaskQueueManager::TaskQueueChanges(uint64_t since, bool includeHidden, std::string& outError) {
    outError.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    const bool isReset = (evictedBeforeRev_ > 0 && since < evictedBeforeRev_);
    const bool includeAll = isReset || (since == 0);

    if (!includeAll && since >= currentRev_) {
        return "{\"ok\":true,\"rev\":" + std::to_string(currentRev_) +
               ",\"jobs\":[],\"removed\":[],\"tasks\":[],\"directions\":[]}";
    }

    std::vector<const TaskQueueJob*> changedJobs;
    std::unordered_set<std::string> changedTaskIds;
    std::unordered_set<std::string> changedDirIds;
    for (const auto& [name, tq] : queues_) {
        (void)name;
        for (const auto& job : tq.jobs) {
            if (job.hidden && !includeHidden) continue;
            if (includeAll || job.rev > since) {
                changedJobs.push_back(&job);
                const std::string effTask = job.taskId.empty() ? job.id : job.taskId;
                changedTaskIds.insert(effTask);
                if (!job.directionId.empty()) changedDirIds.insert(job.directionId);
            }
        }
    }

    std::ostringstream oss;
    oss << "{\"ok\":true,\"rev\":" << currentRev_;
    if (isReset) oss << ",\"reset\":true";

    oss << ",\"jobs\":[";
    const int64_t nowMs = static_cast<int64_t>(GetNowTickMs());
    for (size_t i = 0; i < changedJobs.size(); ++i) {
        if (i > 0) oss << ",";
        const TaskQueueJob& job = *changedJobs[i];
        bool allSat = true;
        for (const auto& d : job.dependsOn) {
            const TaskQueueJob* dep = ResolveDepJobLocked(d.id);
            if (!dep || !IsDepSatisfied(*dep, d.on)) { allSat = false; break; }
        }
        std::string resourceWait = ResourceWaitLocked(job, allSat, nowMs);
        std::string jj = JobToJson(job, false);
        if (!jj.empty() && jj.back() == '}') jj.pop_back();
        oss << jj << ",\"resource_wait\":";
        if (resourceWait.empty()) oss << "null";
        else oss << "\"" << JsonEscape(resourceWait) << "\"";
        oss << ",\"task\":" << TaskSummaryJsonCachedLocked(job) << "}";
    }
    oss << "]";

    oss << ",\"removed\":[";
    if (!isReset) {
        bool firstRem = true;
        for (const auto& t : tombstones_) {
            if (t.rev > since) {
                if (!firstRem) oss << ",";
                firstRem = false;
                oss << "\"" << JsonEscape(t.id) << "\"";
            }
        }
    }
    oss << "]";

    nlohmann::json tasksArr = nlohmann::json::array();
    for (const auto& record : taskStore_.All()) {
        const uint64_t r = (record.is_object() && record.contains("rev") && record["rev"].is_number_unsigned())
            ? record["rev"].get<uint64_t>() : 0;
        const std::string id = record.value("id", std::string());
        if (includeAll || r > since || changedTaskIds.count(id) > 0) {
            const nlohmann::json status = TaskStateLocked(id);
            tasksArr.push_back({
                {"id", id},
                {"title", record.value("title", std::string())},
                {"state", status.value("state", std::string())},
                {"createdAt", record.value("createdAt", std::string())},
                {"updatedAt", record.value("updatedAt", std::string())},
                {"rev", r}
            });
        }
    }
    oss << ",\"tasks\":" << tasksArr.dump();

    nlohmann::json dirsArr = nlohmann::json::array();
    for (const auto& record : directionStore_.All()) {
        const uint64_t r = (record.is_object() && record.contains("rev") && record["rev"].is_number_unsigned())
            ? record["rev"].get<uint64_t>() : 0;
        const std::string id = record.value("id", std::string());
        if (includeAll || r > since || changedDirIds.count(id) > 0) {
            const nlohmann::json status = DirectionStateLocked(id);
            nlohmann::json item = {
                {"id", id},
                {"kind", record.value("kind", std::string())},
                {"taskId", record.value("taskId", std::string())},
                {"btId", record.value("btId", std::string())},
                {"title", record.value("title", std::string())},
                {"state", status.value("state", std::string())},
                {"createdAt", record.value("createdAt", std::string())},
                {"updatedAt", record.value("updatedAt", std::string())},
                {"rev", r}
            };
            if (status.contains("result")) item["result"] = status["result"];
            if (record.contains("card") && record["card"].is_object()) {
                const auto& card = record["card"];
                item["card"] = {
                    {"number", card.value("number", std::string())},
                    {"title", card.value("title", std::string())},
                    {"template", card.value("template", std::string())}
                };
            }
            if (record.contains("after") && record["after"].is_array()) {
                item["after"] = record["after"];
            }
            const std::string supersedes = record.value("supersedes", std::string());
            if (!supersedes.empty()) item["supersedes"] = supersedes;
            const std::string supersededBy = record.value("supersededBy", std::string());
            if (!supersededBy.empty()) item["supersededBy"] = supersededBy;
            dirsArr.push_back(std::move(item));
        }
    }
    oss << ",\"directions\":" << dirsArr.dump() << "}";
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
            job.cachedJsonFull_.clear();
            job.cachedJsonNoInstr_.clear();
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

    std::vector<LineageFailureNode> lineageNodes;
    nlohmann::json taskRecord;
    if (!job.taskId.empty()) taskStore_.Get(job.taskId, taskRecord);
    CollectLineageNodesForTask(queues_, job.taskId, taskRecord, true, lineageNodes);
    const int repeatedFailures = CountLineageFailuresFrom(lineageNodes, job.id);

    std::string jj = JobToJson(job, includeInstructions);
    if (!jj.empty() && jj.back() == '}') jj.pop_back();
    std::ostringstream oss;
    oss << "{\"ok\":true,\"source_changed\":" << (sourceChanged ? "true" : "false")
        << ",\"source_missing\":" << (sourceMissing ? "true" : "false")
        << ",\"job\":" << jj << ",\"task\":" << TaskSummaryJsonLocked(job)
        << ",\"repeatedFailures\":" << repeatedFailures << "}}";
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
    job.paused = false;
    job.blockedBy.clear();
    job.retryAtMs = 0;
    job.updatedAt = GetCurrentTimestampStr();
    MarkJobChangedLocked(job);
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
        const int64_t cutoff = GetNowEpochSec() - sinceSec;
        for (auto& [name, tq] : queues_) {
            if (!taskqueue.empty() && name != taskqueue) continue;
            if (taskqueue.empty() && name == "plain") continue;
            bool changed = false;
            for (auto& job : tq.jobs) {
                if (job.state != "failed" && job.state != "closed") continue;
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

static std::string ExtractCommandModel(const std::string& command);
static bool ModelChangeRequiresApproval();

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
                               "needs","on_permission_denied","permission_resume_limit",
                              "suspend_when_done","suspend_delay_sec","suspend_hibernate",
                              "shutdown_when_done","shutdown_delay_sec",
                              "instructions_text","instructions_file","depends_on"}) {
            if (HasJsonKey(patchJson, k)) { onlyNotes = false; break; }
        }
    }
    if ((job->state == "running" || job->state == "succeeded" || job->state == "cancelled" ||
         job->state == "closed") && !onlyNotes) {
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
        job->summaryLang.clear();
        job->summarySourceLang.clear();
        job->summarySource.clear();
        const std::string sourceLang = JsonFieldStr(patchJson, "source_lang", "");
        if (IsForeignText(s, sourceLang)) {
            nlohmann::json obj = {{"summary", s}};
            NormalizeEnglishField(obj, "summary", sourceLang, "", ActiveTranslationStore());
            CopySummaryNormalization(*job, obj);
        }
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
        const std::string oldModel = ExtractCommandModel(job->command);
        const std::string newModel = ExtractCommandModel(c);
        const bool newModelPreApproved = !newModel.empty() && IsApprovedWorkerModel(newModel);
        if (oldModel != newModel && ModelChangeRequiresApproval() && !newModelPreApproved) {
            std::string approvedBy = JsonFieldStr(patchJson, "approved_by", "");
            if (approvedBy.empty()) approvedBy = JsonFieldStr(patchJson, "approvedBy", "");
            std::string approvalReason = JsonFieldStr(patchJson, "approval_reason", "");
            if (approvalReason.empty()) approvalReason = JsonFieldStr(patchJson, "approvalReason", "");
            if (approvedBy.empty() || approvalReason.empty()) {
                outError = "changing the model requires human approval: add approved_by and approval_reason "
                           "(hancho-cli taskqueue update --approve-model-change --reason \"...\")";
                return BuildError(outError);
            }
            const std::string line = "[model] " + (oldModel.empty() ? std::string("(none)") : oldModel) +
                                     " -> " + (newModel.empty() ? std::string("(none)") : newModel) +
                                     " approved by " + approvedBy + ": " + approvalReason;
            job->notes = job->notes.empty() ? line : job->notes + "\n" + line;
        } else if (oldModel != newModel && newModelPreApproved) {
            const std::string line = "[model] " + (oldModel.empty() ? std::string("(none)") : oldModel) +
                                     " -> " + newModel +
                                     " allowed without extra approval: approved in the worker LLM list";
            job->notes = job->notes.empty() ? line : job->notes + "\n" + line;
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
        if (JsonFieldInt(patchJson, "idle", -1) == 0 || JsonFieldStr(patchJson, "idle", "") == "0") {
            job->idle.outputSec = 0;
            job->idle.cpuSec = 0;
            job->idle.firstEventSec = 0;
            job->idle.explicitZero = true;
        } else {
            std::string idleObj = JsonFieldObj(patchJson, "idle");
            job->idle.outputSec = static_cast<int>(JsonFieldInt(idleObj, "output_sec", job->idle.outputSec));
            job->idle.cpuSec = static_cast<int>(JsonFieldInt(idleObj, "cpu_sec", job->idle.cpuSec));
            job->idle.firstEventSec = static_cast<int>(JsonFieldInt(idleObj, "first_event_sec", job->idle.firstEventSec));
            if (job->idle.outputSec == 0 && job->idle.firstEventSec == 0) {
                job->idle.explicitZero = true;
            } else {
                job->idle.explicitZero = false;
            }
        }
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
    if (HasJsonKey(patchJson, "needs")) {
        job->needs.clear();
        for (const auto& needJson : JsonFieldObjArray(patchJson, "needs")) {
            TaskQueueNeed need;
            need.kind = JsonFieldStr(needJson, "kind", "");
            need.target = JsonFieldStr(needJson, "target", "");
            if (!permit::IsValidKind(need.kind) || need.target.empty()) {
                outError = "needs entries require a valid kind and target";
                return BuildError(outError);
            }
            job->needs.push_back(need);
        }
    }
    if (HasJsonKey(patchJson, "on_permission_denied")) {
        job->onPermissionDenied = JsonFieldStr(patchJson, "on_permission_denied", "");
    }
    if (HasJsonKey(patchJson, "permission_resume_limit")) {
        job->permissionResumeLimit = static_cast<int>(JsonFieldInt(patchJson, "permission_resume_limit", 2));
    }
    if (HasJsonKey(patchJson, "success")) {
        std::string successObj = JsonFieldObj(patchJson, "success");
        job->success.exitCode = static_cast<int>(JsonFieldInt(successObj, "exit_code", job->success.exitCode));
        job->success.filesNewer = JsonFieldStrArray(successObj, "files_newer");
        job->success.outputRegex = JsonFieldStr(successObj, "output_regex", "");
        job->success.testReport = JsonFieldStr(successObj, "test_report", "");
        job->success.contentFile = JsonFieldStr(successObj, "content_file", "");
        job->success.minBytes = JsonFieldInt(successObj, "min_bytes", 0);
        job->success.contentRegex = JsonFieldStr(successObj, "content_regex", "");
        job->success.engineResult = JsonFieldBool(successObj, "engine_result", false);
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
    if (HasJsonKey(patchJson, "suspend_when_done")) {
        job->suspendWhenDone = JsonFieldBool(patchJson, "suspend_when_done", false);
    }
    if (HasJsonKey(patchJson, "suspend_delay_sec")) {
        int d = static_cast<int>(JsonFieldInt(patchJson, "suspend_delay_sec", job->suspendDelaySec));
        if (d < 0 || d > 86400) {
            outError = "suspend_delay_sec must be between 0 and 86400";
            return BuildError(outError);
        }
        job->suspendDelaySec = d;
    }
    if (HasJsonKey(patchJson, "suspend_hibernate")) {
        job->suspendHibernate = JsonFieldBool(patchJson, "suspend_hibernate", false);
    }
    if (HasJsonKey(patchJson, "shutdown_when_done")) {
        job->shutdownWhenDone = JsonFieldBool(patchJson, "shutdown_when_done", false);
    }
    if (HasJsonKey(patchJson, "shutdown_delay_sec")) {
        int d = static_cast<int>(JsonFieldInt(patchJson, "shutdown_delay_sec", job->shutdownDelaySec));
        if (d < 0 || d > 86400) {
            outError = "shutdown_delay_sec must be between 0 and 86400";
            return BuildError(outError);
        }
        job->shutdownDelaySec = d;
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
    MarkJobChangedLocked(*job);
    ReevaluateAll();
    SaveQueue(*tq);
    cv_.notify_all();
    return "{\"ok\":true,\"job\":" + JobToJson(*job, true) + "}";
}

std::string TaskQueueManager::TaskQueueSupersede(const std::string& id, const std::string& reason,
                                                 const std::string& newId, const std::string& rawPatchJson,
                                                 const std::string& originJson, std::string& outError) {
    outError.clear();
    if (id.empty()) {
        outError = "id is required";
        return BuildError(outError);
    }
    if (reason.empty()) {
        outError = "reason is required";
        return BuildError(outError);
    }
    if (reason.size() > 2000) {
        outError = "supersede_reason exceeds 2000 bytes";
        return BuildError(outError);
    }

    const std::string patchTrimmed = TrimJsonStr(rawPatchJson);
    const std::string patchJson =
        (!patchTrimmed.empty() && patchTrimmed.front() == '{') ? NormalizeJobJsonKeys(patchTrimmed) : "{}";

    std::lock_guard<std::mutex> lock(mutex_);
    TaskQueue* srcQueue = nullptr;
    TaskQueueJob* src = FindJob(id, &srcQueue);
    if (!src || !srcQueue) {
        outError = "job not found: " + id;
        return BuildError(outError);
    }
    if (!src->bt.id.empty()) {
        outError = "cannot supersede a behavior tree job; use bt_retry";
        return BuildError(outError);
    }

    // Walk to the newest job of the lineage (skipping missing successors).
    TaskQueueJob* head = src;
    TaskQueue* headQueue = srcQueue;
    for (int step = 0; step < 1000 && !head->supersededBy.empty(); ++step) {
        TaskQueue* nextQ = nullptr;
        TaskQueueJob* next = FindJob(head->supersededBy, &nextQ);
        if (!next || !nextQ) break;
        head = next;
        headQueue = nextQ;
    }
    if (head->state != "failed" && head->state != "blocked" && head->state != "cancelled" &&
        head->state != "closed" && head->state != "takenOver") {
        outError = "can only supersede a finished, unsuccessful job ('" + head->id + "' is " + head->state + ")";
        return BuildError(outError);
    }

    const std::string headTask = head->taskId.empty() ? head->id : head->taskId;
    nlohmann::json headTaskRecord;
    if (taskStore_.Get(headTask, headTaskRecord) && headTaskRecord.contains("closed") &&
        headTaskRecord["closed"].is_object()) {
        outError = "task '" + headTask + "' is closed";
        return BuildError(outError);
    }

    std::string finalNewId = newId;
    if (!finalNewId.empty()) {
        if (finalNewId.rfind("task_", 0) == 0) {
            outError = "job '" + finalNewId + "': ids starting with 'task_' are reserved for tasks";
            return BuildError(outError);
        }
        if (FindJobConst(finalNewId) != nullptr) {
            outError = "duplicate job id '" + finalNewId + "'";
            return BuildError(outError);
        }
    } else {
        for (int n = 1;; ++n) {
            const std::string candidate = src->id + "-s" + std::to_string(n);
            if (FindJobConst(candidate) == nullptr) {
                finalNewId = candidate;
                break;
            }
        }
    }

    TaskQueueJob newJob = *src;
    newJob.id = finalNewId;
    newJob.taskqueue = srcQueue->name;
    newJob.num = nextJobNum_++;
    newJob.state = "pending";
    newJob.paused = false;
    newJob.takeover = TaskQueueTakeoverInfo();
    newJob.attempts.clear();
    newJob.blockedBy.clear();
    newJob.createdAt = GetCurrentTimestampStr();
    newJob.updatedAt = newJob.createdAt;
    newJob.notes.clear();
    newJob.holdReason.clear();
    newJob.retryAtMs = 0;
    newJob.waitPermits.clear();
    newJob.resumeSession.clear();
    newJob.permissionResumeCount = 0;
    newJob.bt = TaskQueueBtRef();
    const TaskQueueOrigin parsedOrigin = ParseOriginJson(originJson);
    if (!parsedOrigin.empty()) newJob.origin = parsedOrigin;
    newJob.supersedes = head->id;
    newJob.taskId = headTask;
    newJob.directionId = head->directionId.empty() ? src->directionId : head->directionId;
    newJob.supersededBy.clear();
    newJob.supersedeReason = reason;
    newJob.instructions.rev = 1;
    newJob.instructions.capturedAt = newJob.createdAt;

    if (HasJsonKey(patchJson, "summary")) {
        std::string s = JsonFieldStr(patchJson, "summary", "");
        if (s.empty() || s.find('\n') != std::string::npos || s.find('\r') != std::string::npos || s.size() > 200) {
            outError = "summary must be a single line of 1..200 characters";
            return BuildError(outError);
        }
        newJob.summary = s;
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
        newJob.command = c;
    }
    if (HasJsonKey(patchJson, "cwd")) newJob.cwd = JsonFieldStr(patchJson, "cwd", "");
    if (HasJsonKey(patchJson, "host")) {
        std::string h = JsonFieldStr(patchJson, "host", "");
        for (char c : h) {
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '-') {
                outError = "host may only contain letters, digits, '.', '_' and '-'";
                return BuildError(outError);
            }
        }
        newJob.host = h;
    }
    if (HasJsonKey(patchJson, "timeout_sec")) {
        int t = static_cast<int>(JsonFieldInt(patchJson, "timeout_sec", 0));
        if (t <= 0) {
            outError = "timeout_sec must be greater than 0";
            return BuildError(outError);
        }
        if (srcQueue->maxTimeoutSec > 0 && t > srcQueue->maxTimeoutSec) {
            outError = "timeout_sec exceeds max_timeout_sec (" + std::to_string(srcQueue->maxTimeoutSec) + ")";
            return BuildError(outError);
        }
        newJob.timeoutSec = t;
    }
    if (HasJsonKey(patchJson, "estimate_sec")) {
        int est = static_cast<int>(JsonFieldInt(patchJson, "estimate_sec", 0));
        if (est < 0) {
            outError = "estimate_sec must be 0 or greater";
            return BuildError(outError);
        }
        newJob.estimateSec = est;
    }
    if (HasJsonKey(patchJson, "idle")) {
        if (JsonFieldInt(patchJson, "idle", -1) == 0 || JsonFieldStr(patchJson, "idle", "") == "0") {
            newJob.idle.outputSec = 0;
            newJob.idle.cpuSec = 0;
            newJob.idle.firstEventSec = 0;
            newJob.idle.explicitZero = true;
        } else {
            std::string idleObj = JsonFieldObj(patchJson, "idle");
            newJob.idle.outputSec = static_cast<int>(JsonFieldInt(idleObj, "output_sec", newJob.idle.outputSec));
            newJob.idle.cpuSec = static_cast<int>(JsonFieldInt(idleObj, "cpu_sec", newJob.idle.cpuSec));
            newJob.idle.firstEventSec = static_cast<int>(JsonFieldInt(idleObj, "first_event_sec", newJob.idle.firstEventSec));
            newJob.idle.explicitZero = (newJob.idle.outputSec == 0 && newJob.idle.firstEventSec == 0);
        }
    }
    if (HasJsonKey(patchJson, "max_attempts")) {
        int m = static_cast<int>(JsonFieldInt(patchJson, "max_attempts", newJob.maxAttempts));
        if (m >= 1) newJob.maxAttempts = m;
    }
    if (HasJsonKey(patchJson, "retry_delay_sec")) {
        int r = static_cast<int>(JsonFieldInt(patchJson, "retry_delay_sec", newJob.retryDelaySec));
        newJob.retryDelaySec = (r > 0) ? r : 0;
    }
    if (HasJsonKey(patchJson, "retry")) {
        struct TaskQueueRetry r;
        std::string retryErr;
        if (!ParseRetryChecked(patchJson, r, retryErr)) {
            outError = retryErr;
            return BuildError(outError);
        }
        newJob.retry = r;
    }
    if (HasJsonKey(patchJson, "priority")) {
        newJob.priority = static_cast<int>(JsonFieldInt(patchJson, "priority", newJob.priority));
    }
    if (HasJsonKey(patchJson, "kill_exclude")) newJob.killExclude = JsonFieldStrArray(patchJson, "kill_exclude");
    if (HasJsonKey(patchJson, "resources")) {
        TaskQueueResources res;
        std::string resErr;
        if (!ParseResourcesChecked(patchJson, res, resErr)) {
            outError = resErr;
            return BuildError(outError);
        }
        newJob.resources = res;
    }
    if (HasJsonKey(patchJson, "memory")) {
        newJob.memory = ParseMemory(JsonFieldObj(patchJson, "memory", ""));
    }
    if (HasJsonKey(patchJson, "env")) newJob.env = ParseJsonStringMap(JsonFieldObj(patchJson, "env"));
    if (HasJsonKey(patchJson, "needs")) {
        newJob.needs.clear();
        for (const auto& needJson : JsonFieldObjArray(patchJson, "needs")) {
            TaskQueueNeed need;
            need.kind = JsonFieldStr(needJson, "kind", "");
            need.target = JsonFieldStr(needJson, "target", "");
            if (!permit::IsValidKind(need.kind) || need.target.empty()) {
                outError = "needs entries require a valid kind and target";
                return BuildError(outError);
            }
            newJob.needs.push_back(need);
        }
    }
    if (HasJsonKey(patchJson, "on_permission_denied")) {
        newJob.onPermissionDenied = JsonFieldStr(patchJson, "on_permission_denied", "");
    }
    if (HasJsonKey(patchJson, "permission_resume_limit")) {
        newJob.permissionResumeLimit = static_cast<int>(JsonFieldInt(patchJson, "permission_resume_limit", 2));
    }
    if (HasJsonKey(patchJson, "success")) {
        std::string successObj = JsonFieldObj(patchJson, "success");
        newJob.success.exitCode = static_cast<int>(JsonFieldInt(successObj, "exit_code", newJob.success.exitCode));
        newJob.success.filesNewer = JsonFieldStrArray(successObj, "files_newer");
        newJob.success.outputRegex = JsonFieldStr(successObj, "output_regex", "");
        newJob.success.testReport = JsonFieldStr(successObj, "test_report", "");
        newJob.success.contentFile = JsonFieldStr(successObj, "content_file", "");
        newJob.success.minBytes = JsonFieldInt(successObj, "min_bytes", 0);
        newJob.success.contentRegex = JsonFieldStr(successObj, "content_regex", "");
        newJob.success.engineResult = JsonFieldBool(successObj, "engine_result", false);
    }
    if (HasJsonKey(patchJson, "defer")) {
        TaskQueueDefer d;
        std::string deferErr;
        if (!ParseDeferChecked(patchJson, newJob.success.exitCode, d, deferErr)) {
            outError = deferErr;
            return BuildError(outError);
        }
        newJob.defer = d;
    }
    if (HasJsonKey(patchJson, "suspend_when_done")) {
        newJob.suspendWhenDone = JsonFieldBool(patchJson, "suspend_when_done", false);
    }
    if (HasJsonKey(patchJson, "suspend_delay_sec")) {
        int d = static_cast<int>(JsonFieldInt(patchJson, "suspend_delay_sec", newJob.suspendDelaySec));
        if (d < 0 || d > 86400) {
            outError = "suspend_delay_sec must be between 0 and 86400";
            return BuildError(outError);
        }
        newJob.suspendDelaySec = d;
    }
    if (HasJsonKey(patchJson, "suspend_hibernate")) {
        newJob.suspendHibernate = JsonFieldBool(patchJson, "suspend_hibernate", false);
    }
    if (HasJsonKey(patchJson, "shutdown_when_done")) {
        newJob.shutdownWhenDone = JsonFieldBool(patchJson, "shutdown_when_done", false);
    }
    if (HasJsonKey(patchJson, "shutdown_delay_sec")) {
        int d = static_cast<int>(JsonFieldInt(patchJson, "shutdown_delay_sec", newJob.shutdownDelaySec));
        if (d < 0 || d > 86400) {
            outError = "shutdown_delay_sec must be between 0 and 86400";
            return BuildError(outError);
        }
        newJob.shutdownDelaySec = d;
    }
    if (HasJsonKey(patchJson, "notes")) {
        newJob.notes = JsonFieldStr(patchJson, "notes", "");
    }
    if (HasJsonKey(patchJson, "instructions_text")) {
        std::string text = JsonFieldStr(patchJson, "instructions_text", "");
        if (text.size() > 1024 * 1024) {
            outError = "instructions exceed the 1 MB limit";
            return BuildError(outError);
        }
        newJob.instructions.source.clear();
        newJob.instructions.text = text;
        newJob.instructions.sha256 = Sha256Hex(text);
        newJob.instructions.rev = 1;
        newJob.instructions.capturedAt = newJob.createdAt;
    } else if (HasJsonKey(patchJson, "instructions_file")) {
        std::string f = JsonFieldStr(patchJson, "instructions_file", "");
        std::wstring pathW = ResolveJobPathWide(newJob, f);
        if (!PathExistsWide(pathW)) {
            outError = "instructions_file not found: " + f;
            return BuildError(outError);
        }
        std::string text = ReadFileUtf8Wide(pathW);
        if (text.size() > 1024 * 1024) {
            outError = "instructions exceed the 1 MB limit";
            return BuildError(outError);
        }
        newJob.instructions.source = f;
        newJob.instructions.text = text;
        newJob.instructions.sha256 = Sha256Hex(text);
        newJob.instructions.rev = 1;
        newJob.instructions.capturedAt = newJob.createdAt;
    }

    head->supersededBy = newJob.id;
    head->updatedAt = newJob.createdAt;
    MarkJobChangedLocked(*head);
    if (headQueue != srcQueue) SaveQueue(*headQueue);

    MarkJobChangedLocked(newJob);
    srcQueue->jobs.push_back(newJob);
    jobIndexDirty_ = true;
    ReevaluateAll();
    SaveQueue(*srcQueue);
    cv_.notify_all();

    if (!newJob.taskId.empty()) {
        TaskLog(newJob.taskId, "job_added", newJob.id, "", "", newJob.origin.session);
        TaskLog(newJob.taskId, "superseded", newJob.id, "", newJob.supersedeReason, newJob.origin.session);
    }

    const TaskQueueJob* stored = FindJobConst(newJob.id);
    const TaskQueueJob& outJob = stored ? *stored : newJob;
    std::ostringstream oss;
    oss << "{\"ok\":true,\"id\":\"" << JsonEscape(outJob.id) << "\""
        << ",\"supersedes\":\"" << JsonEscape(outJob.supersedes) << "\""
        << ",\"taskqueue\":\"" << JsonEscape(outJob.taskqueue) << "\""
        << ",\"task_id\":\"" << JsonEscape(outJob.taskId) << "\"";
    if (!outJob.directionId.empty())
        oss << ",\"direction_id\":\"" << JsonEscape(outJob.directionId) << "\"";
    oss << ",\"reason\":\"" << JsonEscape(outJob.supersedeReason) << "\""
        << ",\"job\":" << JobToJson(outJob, true) << "}";
    return oss.str();
}

} // namespace pc

namespace pc {

std::string TaskQueueManager::TaskQueuePauseJob(const std::string& id, bool paused, std::string& outError) {
    outError.clear();
    std::lock_guard<std::mutex> lock(mutex_);
    TaskQueue* tq = nullptr;
    TaskQueueJob* job = FindJob(id, &tq);
    if (!job) {
        outError = "job not found: " + id;
        return BuildError(outError);
    }
    if (job->paused == paused) {
        return "{\"ok\":true,\"job\":" + JobToJson(*job, true) + "}";
    }
    if (job->state == "succeeded" || job->state == "cancelled" || job->state == "failed" ||
        job->state == "closed") {
        outError = "job '" + id + "' has already finished (" + job->state + ")";
        return BuildError(outError);
    }

    const int attemptN = job->attempts.empty() ? 0 : job->attempts.back().n;
    if (job->state == "running" && !job->attempts.empty()) {
        const TaskQueueAttempt& attempt = job->attempts.back();
        if (!attempt.remoteHost.empty() || attempt.remoteProcessId > 0) {
            outError = "pausing a remote job is not supported yet";
            return BuildError(outError);
        }
        if (attempt.processId > 0) {
            if (!pm_) {
                outError = "process manager is not available";
                return BuildError(outError);
            }
            std::string pmErr;
            if (!pm_->SuspendProcess(attempt.processId, paused, pmErr)) {
                outError = pmErr;
                return BuildError(outError);
            }
        }
    }

    job->paused = paused;
    job->updatedAt = GetCurrentTimestampStr();
    MarkJobChangedLocked(*job);
    EmitEventLocked(*tq, paused ? "jobPaused" : "jobResumed", id, attemptN);
    if (!paused) ReevaluateAll();
    SaveQueue(*tq);
    if (schedulerRunning_.load()) cv_.notify_all();
    return "{\"ok\":true,\"job\":" + JobToJson(*job, true) + "}";
}

std::string TaskQueueManager::TaskQueueCancel(const std::string& id, std::string& outError,
                                            const std::string& reason) {
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
            if (!reason.empty()) job->attempts.back().reason = reason;
        }
        job->state = "cancelled";
        job->paused = false;
        job->blockedBy.clear();
        job->retryAtMs = 0;
        job->updatedAt = GetCurrentTimestampStr();
        MarkJobChangedLocked(*job);
        EmitEventLocked(*tq, "jobCancelled", id, job->attempts.empty() ? 0 : job->attempts.back().n);
        ReevaluateAll();
        SaveQueue(*tq);
        if (!job->taskId.empty()) {
            TaskLog(job->taskId, "job_cancelled", id, "", reason.empty() ? "cancelled" : reason, "");
        }
        if (!job->directionId.empty()) {
            MaybeLogDirectionFinishedLocked(job->directionId, job->taskId);
        }
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

int TaskQueueManager::CancelAllActiveJobs(const std::string& reason) {
    (void)reason;
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& kv : queues_) {
            for (const auto& job : kv.second.jobs) {
                if (job.state != "succeeded" && job.state != "failed" &&
                    job.state != "cancelled" && job.state != "closed" &&
                    job.state != "takenOver") {
                    ids.push_back(job.id);
                }
            }
        }
    }
    int cancelled = 0;
    for (const auto& id : ids) {
        std::string err;
        TaskQueueCancel(id, err);
        ++cancelled;
    }
    return cancelled;
}

std::string TaskQueueManager::TaskQueueClose(const std::string& id, std::string& outError) {
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
        if (job->state == "closed") {
            return "{\"ok\":true,\"job\":" + JobToJson(*job, true) + "}";
        }
        if (job->state == "running" && !job->attempts.empty()) {
            pid = job->attempts.back().processId;
            remoteHost = job->attempts.back().remoteHost;
            remotePid = job->attempts.back().remoteProcessId;
        }
        job->state = "closed";
        job->paused = false;
        job->blockedBy.clear();
        job->retryAtMs = 0;
        job->updatedAt = GetCurrentTimestampStr();
        MarkJobChangedLocked(*job);
        EmitEventLocked(*tq, "jobClosed", id, job->attempts.empty() ? 0 : job->attempts.back().n);
        ReevaluateAll();
        SaveQueue(*tq);
        if (!job->taskId.empty()) {
            TaskLog(job->taskId, "job_cancelled", id, "", "closed", "");
            if (!job->directionId.empty()) {
                MaybeLogDirectionFinishedLocked(job->directionId, job->taskId);
            }
        }
        if (schedulerRunning_.load()) {
            pendingFinished_.push_back({*job, "closed"});
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

std::string TaskQueueManager::TaskQueueTakeover(const std::string& id, const std::string& by,
                                                const std::string& reason, std::string& outError) {
    outError.clear();
    if (reason.empty()) {
        outError = "takeover requires a non-empty reason";
        return BuildError(outError);
    }
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
        const std::string& st = job->state;
        if (st == "succeeded" || st == "cancelled" || st == "takenOver" || st == "closed") {
            outError = "job '" + id + "' cannot be taken over from state " + st;
            return BuildError(outError);
        }
        if (st != "failed" && st != "blocked" && st != "waiting" && st != "pending" && st != "running") {
            outError = "job '" + id + "' cannot be taken over from state " + st;
            return BuildError(outError);
        }
        if (st == "running" && !job->attempts.empty()) {
            pid = job->attempts.back().processId;
            remoteHost = job->attempts.back().remoteHost;
            remotePid = job->attempts.back().remoteProcessId;
        }
        job->takeover.present = true;
        job->takeover.by = by.empty() ? "cli" : by;
        job->takeover.reason = reason;
        job->takeover.at = GetCurrentTimestampStr();
        job->takeover.completedAt.clear();
        job->takeover.report.clear();
        job->takeover.result.clear();
        job->takeover.completionReason.clear();
        job->state = "takenOver";
        job->paused = false;
        job->blockedBy.clear();
        job->retryAtMs = 0;
        job->updatedAt = job->takeover.at;
        MarkJobChangedLocked(*job);
        EmitEventLocked(*tq, "jobTakenOver", id, job->attempts.empty() ? 0 : job->attempts.back().n);
        ReevaluateAll();
        SaveQueue(*tq);
        if (!job->taskId.empty()) {
            TaskLog(job->taskId, "takeover", id, "", job->takeover.by + ": " + job->takeover.reason, "");
        }
        if (schedulerRunning_.load()) {
            pendingFinished_.push_back({*job, "takenOver"});
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

std::string TaskQueueManager::TaskQueueComplete(const std::string& id, const std::string& report,
                                                const std::string& result, const std::string& reason,
                                                std::string& outError) {
    outError.clear();
    if (reason.empty()) {
        outError = "complete requires a non-empty reason";
        return BuildError(outError);
    }
    std::string finalResult = result.empty() ? "succeeded" : result;
    if (finalResult != "succeeded" && finalResult != "failed") {
        outError = "complete --result must be succeeded or failed";
        return BuildError(outError);
    }
    std::string resp;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        TaskQueue* tq = nullptr;
        TaskQueueJob* job = FindJob(id, &tq);
        if (!job) {
            outError = "job not found: " + id;
            return BuildError(outError);
        }
        if (job->state != "takenOver") {
            outError = "job '" + id + "' is not taken over";
            return BuildError(outError);
        }
        if (job->takeover.present && !job->takeover.completedAt.empty()) {
            outError = "job '" + id + "' takeover is already completed";
            return BuildError(outError);
        }
        if (!job->takeover.present) {
            job->takeover.present = true;
            job->takeover.by = "cli";
        }
        job->takeover.completedAt = GetCurrentTimestampStr();
        job->takeover.report = report;
        job->takeover.result = finalResult;
        job->takeover.completionReason = reason;
        job->updatedAt = job->takeover.completedAt;
        MarkJobChangedLocked(*job);
        EmitEventLocked(*tq, "jobTakeoverCompleted", id, job->attempts.empty() ? 0 : job->attempts.back().n);
        ReevaluateAll();
        SaveAllLocked();
        if (!job->taskId.empty()) {
            if (!job->directionId.empty()) {
                MaybeLogDirectionFinishedLocked(job->directionId, job->taskId);
            }
            TaskLog(job->taskId, "takeover_completed", id, "", finalResult + ": " + reason, "");
        }
        CheckQueueNotices(*tq);
        cv_.notify_all();
        resp = "{\"ok\":true,\"job\":" + JobToJson(*job, true) + "}";
    }
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
    if (states.empty()) states = {"succeeded", "failed", "cancelled", "closed"};

    std::vector<TaskQueueJob> gone;
    for (const auto& j : it->second.jobs) {
        if (std::find(states.begin(), states.end(), j.state) != states.end()) gone.push_back(j);
    }
    RecordRemovedJobsLocked(gone);
    size_t before = it->second.jobs.size();
    auto& jobs = it->second.jobs;
    jobs.erase(std::remove_if(jobs.begin(), jobs.end(), [&](const TaskQueueJob& j) {
        return std::find(states.begin(), states.end(), j.state) != states.end();
    }), jobs.end());
    size_t purged = before - jobs.size();
    if (purged > 0) jobIndexDirty_ = true;
    it->second.version++;
    SaveQueue(it->second);
    return "{\"ok\":true,\"taskqueue\":\"" + JsonEscape(taskqueue) + "\",\"purged\":" + std::to_string(purged) + "}";
}

std::string TaskQueueManager::TaskQueueRemove(const std::string& id, bool force, std::string& outError) {
    outError.clear();
    if (id.empty()) {
        outError = "id is required";
        return BuildError(outError);
    }
    if (id.rfind("btplan:", 0) == 0 || id.rfind("group:", 0) == 0 || id.rfind("direction:", 0) == 0) {
        outError = "this is a virtual direction row, not a queue job; stop the direction instead";
        return BuildError(outError);
    }
    ProcessManager* pm = nullptr;
    int pid = 0;
    std::string remoteHost;
    int remotePid = 0;
    RemoteKillCallback remoteKill;
    std::string resp;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        TaskQueue* tq = nullptr;
        TaskQueueJob* job = FindJob(id, &tq);
        if (!job || !tq) {
            outError = "job not found: " + id;
            return BuildError(outError);
        }
        const bool finished = job->state == "succeeded" || job->state == "failed" ||
                              job->state == "cancelled" || job->state == "closed";
        if (!finished && !force) {
            outError = "cannot remove a " + job->state + " job; use force or cancel it first";
            return BuildError(outError);
        }
        if (job->state == "running" && !job->attempts.empty()) {
            pid = job->attempts.back().processId;
            remoteHost = job->attempts.back().remoteHost;
            remotePid = job->attempts.back().remoteProcessId;
        }
        const std::string queueName = tq->name;
        const std::string removedState = job->state;
        RecordRemovedJobsLocked(std::vector<TaskQueueJob>{*job});
        auto& jobs = tq->jobs;
        jobs.erase(std::remove_if(jobs.begin(), jobs.end(),
                                  [&](const TaskQueueJob& j) { return j.id == id; }), jobs.end());
        jobIndexDirty_ = true;
        tq->version++;
        SaveQueue(*tq);
        cv_.notify_all();
        WriteAuditLog("TASKQUEUE_REMOVE", "id=" + id + " queue=" + queueName + " state=" + removedState +
                                              (force ? " force=1" : ""));
        resp = "{\"ok\":true,\"id\":\"" + JsonEscape(id) + "\",\"taskqueue\":\"" + JsonEscape(queueName) +
               "\",\"state\":\"" + JsonEscape(removedState) + "\"}";
        pm = pm_;
        remoteKill = remoteKillCb_;
    }
    // A force-removed running job must not keep its process alive: otherwise the process
    // finishes later and is recorded again as a plain run, so the row reappears.
    if (!remoteHost.empty() && remotePid > 0) {
        if (remoteKill) {
            std::string err;
            remoteKill(remoteHost, remotePid, err);
        }
    } else if (pm && pid > 0) {
        std::string err;
        pm->KillProcess(pid, err);
    }
    return resp;
}

std::string TaskQueueManager::RemoveJobsForTree(const std::string& btId, std::string& outError) {
    outError.clear();
    if (btId.empty()) {
        outError = "bt id is required";
        return BuildError(outError);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    size_t removed = 0;
    for (auto& [name, tq] : queues_) {
        (void)name;
        std::vector<TaskQueueJob> gone;
        for (const auto& j : tq.jobs) {
            if (j.bt.id == btId) gone.push_back(j);
        }
        RecordRemovedJobsLocked(gone);
        const size_t before = tq.jobs.size();
        tq.jobs.erase(std::remove_if(tq.jobs.begin(), tq.jobs.end(),
                                     [&](const TaskQueueJob& j) { return j.bt.id == btId; }),
                      tq.jobs.end());
        const size_t n = before - tq.jobs.size();
        if (n > 0) {
            removed += n;
            tq.version++;
            SaveQueue(tq);
        }
    }
    if (removed > 0) {
        jobIndexDirty_ = true;
        cv_.notify_all();
        WriteAuditLog("TASKQUEUE_REMOVE_TREE", "bt=" + btId + " removed=" + std::to_string(removed));
    }
    return "{\"ok\":true,\"bt\":\"" + JsonEscape(btId) + "\",\"removed\":" + std::to_string(removed) + "}";
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
    const int64_t cutoff = GetNowEpochSec() - maxAgeSec;
    const std::vector<std::string> finished = {"succeeded", "failed", "cancelled", "closed"};
    size_t purged = 0;
    for (auto& [name, tq] : queues_) {
        if (!taskqueue.empty() && name != taskqueue) continue;
        std::vector<TaskQueueJob> gone;
        for (const auto& j : tq.jobs) {
            if (std::find(finished.begin(), finished.end(), j.state) == finished.end()) continue;
            const int64_t ts = ParseTimestampStrToEpoch(j.updatedAt);
            if (ts > 0 && ts < cutoff) gone.push_back(j);
        }
        RecordRemovedJobsLocked(gone);
        size_t before = tq.jobs.size();
        tq.jobs.erase(std::remove_if(tq.jobs.begin(), tq.jobs.end(), [&](const TaskQueueJob& j) {
            if (std::find(finished.begin(), finished.end(), j.state) == finished.end()) return false;
            int64_t ts = ParseTimestampStrToEpoch(j.updatedAt);
            return ts > 0 && ts < cutoff;
        }), tq.jobs.end());
        size_t removed = before - tq.jobs.size();
        if (removed > 0) {
            jobIndexDirty_ = true;
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
    MarkJobChangedLocked(*job);
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
    MarkJobChangedLocked(*job);
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
    if (job.state == "running" || job.state == "succeeded" || job.state == "cancelled" ||
        job.state == "takenOver" || job.state == "closed") return false;

    std::string blocker;
    for (const auto& d : job.dependsOn) {
        const TaskQueueJob* dep = ResolveDepJobLocked(d.id, &tq);
        if (!dep) { blocker = d.id; break; }
        if (d.on == "success" &&
            (dep->state == "failed" || dep->state == "cancelled" || dep->state == "blocked" ||
             (dep->state == "takenOver" && dep->takeover.present && dep->takeover.result == "failed"))) {
            blocker = dep->id;
            break;
        }
    }

    if (!blocker.empty()) {
        if (job.state != "blocked" || job.blockedBy != blocker) {
            job.state = "blocked";
            job.blockedBy = blocker;
            job.updatedAt = GetCurrentTimestampStr();
            MarkJobChangedLocked(job);
            EmitEventLocked(tq, "jobBlocked", job.id, 0);
            return true;
        }
        return false;
    }

    if (job.state == "blocked") {
        job.state = "pending";
        job.blockedBy.clear();
        job.updatedAt = GetCurrentTimestampStr();
        MarkJobChangedLocked(job);
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
                const TaskQueueJob* dep = ResolveDepJobLocked(d.id);
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
                const TaskQueueJob* dep = ResolveDepJobLocked(d.id);
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

// True when the job log's last LLM result line reports an error. Handles the
// claude/opencode {"type":"result",...} line, the agy {"event":"result",...} line and codex
// turn.failed. Only the final terminal line counts, so an error the engine recovered from is
// ignored. The engine can exit 0 after such a line, so this is checked separately.
static bool EngineResultReportedError(const std::string& text, std::string& detail) {
    bool sawTerminal = false;
    bool failed = false;
    std::string reason;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? text.size() + 1 : nl + 1;
        size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] != '{') continue;
        auto j = nlohmann::json::parse(line.substr(first), nullptr, false);
        if (!j.is_object()) continue;

        const std::string type = j.value("type", std::string());
        const std::string event = j.value("event", std::string());
        bool terminal = false;
        bool err = false;
        std::string lineReason;
        if (type == "result") {
            terminal = true;
            if (j.contains("is_error") && j["is_error"].is_boolean()) err = j["is_error"].get<bool>();
            if (j.contains("subtype") && j["subtype"].is_string()) {
                const std::string sub = j["subtype"].get<std::string>();
                if (sub != "success" && sub != "completed") err = true;
                lineReason = sub;
            }
            if (j.contains("api_error_status") && !j["api_error_status"].is_null()) err = true;
            if (j.contains("error") && j["error"].is_string()) {
                err = true;
                const std::string e = j["error"].get<std::string>();
                if (!e.empty()) lineReason = e;
            }
            if (err && j.contains("result") && j["result"].is_string()) {
                const std::string r = j["result"].get<std::string>();
                if (!r.empty()) lineReason = r;
            }
        } else if (event == "result") {
            terminal = true;
            if (j.contains("result") && j["result"].is_object()) {
                const auto& r = j["result"];
                if (r.contains("status") && r["status"].is_string()) {
                    const std::string st = r["status"].get<std::string>();
                    const std::string lst = LowerAscii(st);
                    err = lst != "success" && lst != "completed" && lst != "ok";
                    lineReason = st;
                }
                if (err && r.contains("response") && r["response"].is_string()) {
                    const std::string resp = r["response"].get<std::string>();
                    if (!resp.empty()) lineReason = resp;
                }
            }
        } else if (type == "turn.completed") {
            terminal = true;
        } else if (type == "turn.failed") {
            terminal = true;
            err = true;
            lineReason = "turn failed";
            if (j.contains("error") && j["error"].is_object() && j["error"].contains("message") &&
                j["error"]["message"].is_string()) {
                lineReason = j["error"]["message"].get<std::string>();
            }
        } else if (type == "error") {
            terminal = true;
            err = true;
            for (const char* k : {"message", "content", "error"}) {
                if (j.contains(k) && j[k].is_string()) {
                    lineReason = j[k].get<std::string>();
                    break;
                }
            }
        }
        if (!terminal) continue;
        sawTerminal = true;
        failed = err;
        reason = err ? lineReason : std::string();
    }
    if (sawTerminal && failed) {
        detail = "engine result reported an error";
        if (!reason.empty()) detail += ": " + reason;
        return true;
    }
    return false;
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
    // paths are evaluated on the remote host (doc/obsoleted/design-v3-todo.md 6, card 057).
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
    // A report can be touched early (just the headings) and then the worker dies. The content
    // check makes the file have to carry what the job promised, not only a new mtime.
    if (!job.success.contentFile.empty()) {
        if (!job.host.empty()) {
            detail = "content check is not available for remote jobs: " + job.success.contentFile;
            return false;
        }
        const std::string& rel = job.success.contentFile;
        std::wstring pathW = ResolveJobPathWide(job, rel);
        if (!PathExistsWide(pathW)) {
            detail = "content file missing: " + rel;
            return false;
        }
        uint64_t fileTime = 0;
        if (!GetFileMtimeWide(pathW, fileTime)) {
            detail = "cannot read mtime: " + rel;
            return false;
        }
        if (attempt.startFiletime > 0 && fileTime <= attempt.startFiletime) {
            detail = "content file not newer than attempt start: " + rel;
            return false;
        }
        const std::string content = ReadFileUtf8Wide(pathW);
        if (job.success.minBytes > 0 && static_cast<long long>(content.size()) < job.success.minBytes) {
            detail = "content file too small: " + rel + " (" + std::to_string(content.size()) + " < " +
                     std::to_string(job.success.minBytes) + " bytes)";
            return false;
        }
        if (!job.success.contentRegex.empty()) {
            std::regex re;
            if (!PrepareRegex(job.success.contentRegex, false, re)) {
                detail = "invalid content_regex";
                return false;
            }
            if (!std::regex_search(content, re)) {
                detail = "content_regex did not match: " + rel;
                return false;
            }
        }
    }
    std::string logText;
    if (!job.success.outputRegex.empty() || job.success.engineResult) {
        if (!logFile.empty()) logText = ReadFileUtf8Wide(Utf8ToWide(logFile));
    }
    if (!job.success.outputRegex.empty()) {
        const std::string text = GetTailLines(logText, 200);
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
    if (job.success.engineResult) {
        std::string engineDetail;
        // The terminal result is at the end; a large stream file does not need re-reading whole.
        if (EngineResultReportedError(GetTailLines(logText, 1000), engineDetail)) {
            detail = engineDetail;
            return false;
        }
    }
    TestReportSummary reportSummary;
    bool reportParsed = false;
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
        if (!ParseTestReport(ReadFileUtf8Wide(pathW), reportSummary)) {
            detail = "test report has no test cases: " + rel;
            return false;
        }
        reportParsed = true;
        if (reportSummary.failed > 0) {
            detail = "test report: " + std::to_string(reportSummary.failed) + " of " +
                     std::to_string(reportSummary.total) + " tests failed";
            for (size_t i = 0; i < reportSummary.failedNames.size(); ++i) {
                detail += (i == 0 ? ": " : ", ") + reportSummary.failedNames[i];
            }
            if (static_cast<int>(reportSummary.failedNames.size()) < reportSummary.failed) detail += ", ...";
            return false;
        }
    }
    detail = "ok";
    if (reportParsed) attempt.testReportSummary = reportSummary;
    return true;
}

// startup_timed_out classifies as startup_hang, the same kind of timeout as
// idle_timed_out (idle_stall). Let retry.on/retry.never treat them alike.
static bool RetryClassMatches(const std::string& configured, const std::string& failureClass) {
    const std::string a = LowerAscii(configured);
    const std::string b = LowerAscii(failureClass);
    if (a == b) return true;
    const bool aTimeout = (a == "idle_stall" || a == "startup_hang");
    const bool bTimeout = (b == "idle_stall" || b == "startup_hang");
    return aTimeout && bTimeout;
}

// 091-7: the engine's session id travels on normalized events (agent.sessionId). A denial
// that ended a session can be continued with `opencode run --session <id>` / `claude --resume`.
static std::string LatestSessionId(const std::vector<nlohmann::json>& events) {
    std::string found;
    for (const auto& ev : events) {
        if (!ev.is_object()) continue;
        auto agent = ev.find("agent");
        if (agent != ev.end() && agent->is_object()) {
            std::string id = agent->value("sessionId", std::string());
            if (!id.empty()) found = id;
        }
    }
    return found;
}

PermissionHoldPlan PlanPermissionHandling(const TaskQueueFailureDiagnostic& diagnostic,
                                          const std::string& sessionId, int resumeCount,
                                          int resumeLimit, const std::string& taskqueue,
                                          const std::string& onPermissionDenied) {
    PermissionHoldPlan out;
    if (diagnostic.failureClass != "permission_denied") return out;
    if (onPermissionDenied == "fail") return out; // caller fails the job immediately
    if (!sessionId.empty() && resumeCount < resumeLimit) {
        out.resume = true;
        return out;
    }
    out.wait = true;
    auto perm = diagnostic.detail.find("permission");
    if (perm != diagnostic.detail.end() && perm->is_object()) {
        out.target = perm->value("target", std::string());
        out.engine = perm->value("engine", std::string());
        const std::string tool = LowerAscii(perm->value("tool", std::string()));
        out.kind = (tool == "bash" || tool.find("bash") != std::string::npos ||
                    tool == "powershell" || tool.find("shell") != std::string::npos)
                       ? "command" : "read_path";
    }
    if (out.target.empty()) {
        for (const auto& step : diagnostic.nextSteps) {
            if (step.action == "permit_request" && step.change.is_object()) {
                out.kind = step.change.value("kind", out.kind);
                out.target = step.change.value("target", out.target);
                break;
            }
        }
    }
    out.scope = "taskqueue:" + taskqueue;
    out.permitId = permit::MakeId(out.kind, out.target, out.scope);
    return out;
}

TaskQueueRetryDecision DecideTaskQueueRetry(const TaskQueueJob& job, const std::string& failureClass,
                                             int countedAttempts, bool allowRateLimitWait) {
    auto listed = [&failureClass](const std::vector<std::string>& classes) {
        for (const auto& c : classes) {
            if (RetryClassMatches(c, failureClass)) return true;
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
    MarkJobChangedLocked(job);

    if (job.state == "cancelled") {
        SaveQueue(tq);
        cv_.notify_all();
        return;
    }

    if (success) {
        std::string eng, mod;
        if (JobEngineModel(job, eng, mod)) {
            engine_health::MarkSuccess(engine_health::DefaultPath(), eng, mod,
                                       engine_health::NowEpochMs());
        }
        job.state = "succeeded";
        job.blockedBy.clear();
        job.retryAtMs = 0;
        job.holdReason.clear();
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

        // 091-6: trip the shared circuit breaker for failures that hide a dead engine.
        std::string eng = failureInput->engine;
        std::string mod = failureInput->model;
        if (eng.empty() || mod.empty()) {
            std::string e2, m2;
            if (JobEngineModel(job, e2, m2)) { eng = e2; mod = m2; }
        }
        if (!eng.empty() && !mod.empty()) {
            using namespace engine_health;
            const int64_t nowEpoch = NowEpochMs();
            int64_t retryAtMs = 0;
            if (!a.failure.retryAt.empty()) {
                retryAtMs = ParseTimestampStrToEpoch(a.failure.retryAt) * 1000;
            }
            MarkDown(DefaultPath(), eng, mod, a.failure.failureClass, checkDetail,
                     nowEpoch, retryAtMs, LoadSettings());
            if (!HasWorkerActivityEvents(failureInput->events)) {
                RecordZeroEventFailure(DefaultPath(), eng, mod, nowEpoch);
            }
        }
    }

    // 091-7: a permission denial never becomes a plain failure by itself. First continue the
    // same engine session (up to permissionResumeLimit), then auto-request a permit and hold
    // the job in waitingForPermit until a human approves or rejects it.
    if (a.failure.failureClass == "permission_denied" && job.onPermissionDenied != "fail") {
        const std::string session = failureInput ? LatestSessionId(failureInput->events) : "";
        const PermissionHoldPlan plan = PlanPermissionHandling(
            a.failure, session, job.permissionResumeCount, job.permissionResumeLimit,
            tq.name, job.onPermissionDenied);
        if (plan.resume) {
            job.permissionResumeCount++;
            job.resumeSession = session;
            a.reason = "permissionResume";
            job.state = "pending";
            job.retryAtMs = static_cast<int64_t>(GetNowTickMs()) + 1000;
            job.holdReason.clear();
            EmitEventLocked(tq, "jobDeferred", job.id, a.n, &a.failure);
            SaveAllLocked();
            CheckQueueNotices(tq);
            cv_.notify_all();
            return;
        }
        if (plan.wait && !plan.target.empty()) {
            std::vector<std::string> engines;
            if (!plan.engine.empty()) engines.push_back(plan.engine);
            std::string reason = "auto-requested after permission_denied on job '" + job.id + "'";
            permit::Result r = permit::Request(permit::DefaultPath(), plan.kind, plan.target, engines,
                                               plan.scope, reason, "worker");
            if (r.ok && std::find(job.waitPermits.begin(), job.waitPermits.end(), plan.permitId) ==
                            job.waitPermits.end()) {
                job.waitPermits.push_back(plan.permitId);
            }
        }
        a.reason = "waitingForPermit";
        job.state = "waitingForPermit";
        job.retryAtMs = 0;
        job.holdReason = "waiting for permit approval";
        a.failure.nextSteps.push_back({"wait_permit", nullptr, true,
                                       "Approve the requested permit in the dashboard or with `permit approve`."});
        EmitEventLocked(tq, "jobWaitingForPermit", job.id, a.n, &a.failure);
        SaveAllLocked();
        CheckQueueNotices(tq);
        cv_.notify_all();
        return;
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
    if (a.failure.failureClass == "permission_denied" && job.onPermissionDenied == "fail") {
        // Explicit opt-out: no resume, no holding, no automatic retry.
        decision.action = TaskQueueRetryAction::GiveUp;
    }
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

    // 091-6: the attempt that revealed the dead engine waits for recovery instead of using
    // up its retry budget. It is started again automatically once the engine probes healthy.
    {
        std::string waitEngine, waitModel;
        if (JobEngineModel(job, waitEngine, waitModel)) {
            using namespace engine_health;
            Store health;
            std::string healthErr;
            if (Load(DefaultPath(), health, healthErr)) {
                const Entry* e = Find(health, waitEngine, waitModel);
                if (e && e->status == "down" && countedAttempts < job.maxAttempts) {
                    a.reason = "engineWaiting";
                    job.state = "pending";
                    int64_t waitMs = e->nextProbeAtMs - NowEpochMs();
                    if (waitMs < 0) waitMs = 0;
                    job.retryAtMs = static_cast<int64_t>(GetNowTickMs()) + waitMs + 1000;
                    job.holdReason = e->HoldReason();
                    EmitEventLocked(tq, "jobDeferred", job.id, a.n, &a.failure);
                    SaveAllLocked();
                    CheckQueueNotices(tq);
                    cv_.notify_all();
                    return;
                }
            }
        }
    }

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

// The model argument of a worker command ("--model X", "--model=X", "-m X").
static std::string ExtractCommandModel(const std::string& command) {
    std::vector<std::string> tokens;
    std::string current;
    bool quoted = false;
    for (size_t i = 0; i < command.size(); ++i) {
        const char c = command[i];
        if (c == '"') {
            quoted = !quoted;
            continue;
        }
        if (!quoted && (c == ' ' || c == '\t')) {
            if (!current.empty()) { tokens.push_back(current); current.clear(); }
            continue;
        }
        current.push_back(c);
    }
    if (!current.empty()) tokens.push_back(current);
    for (size_t i = 0; i < tokens.size(); ++i) {
        const std::string& t = tokens[i];
        if ((t == "--model" || t == "-m") && i + 1 < tokens.size()) return tokens[i + 1];
        if (t.rfind("--model=", 0) == 0) return t.substr(8);
        if (t.rfind("-m=", 0) == 0) return t.substr(3);
    }
    return "";
}

// Model changes are expensive and were the 2026-11-17 incident; require an explicit approval.
// require_model_change_approval in config.json (default true) can disable the gate.
static bool ModelChangeRequiresApproval() {
    std::string content = ReadFileUtf8Wide(PathJoinWide(GetEffectiveDataDirectoryWide(), L"config.json"));
    if (!content.empty()) {
        nlohmann::json cfg = nlohmann::json::parse(content, nullptr, false);
        if (cfg.is_object() && cfg.contains("require_model_change_approval") &&
            cfg["require_model_change_approval"].is_boolean()) {
            return cfg["require_model_change_approval"].get<bool>();
        }
    }
    return true;
}

// A change to a model already approved and enabled in the worker LLM list is safe: the user
// gave prior, per-row approval for exactly this kind of switch (doc/obsoleted/HanchoExpectations.md 8.3).
static bool IsApprovedWorkerModel(const std::string& model) {
    llm::Config cfg;
    std::string err;
    if (!llm::Load(llm::DefaultPath(), cfg, err)) return false;
    for (const auto& e : cfg.worker) {
        if (e.Usable() && e.model == model) return true;
    }
    return false;
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
    if (!req.agentType.empty() || req.commandLine.find("--json-stream") != std::string::npos || job.idle.firstEventSec > 0) {
        req.jsonStream = true;
    }
    req.env = job.env;
    // 091-7: hand approved permits to the worker. The user's global engine config is never
    // written; all values are process-environment overrides for this launch only.
    {
        permit::Config cfg;
        std::string permitErr;
        if (permit::Load(permit::DefaultPath(), cfg, permitErr)) {
            const std::string scope = "taskqueue:" + tq.name;
            const auto rows = permit::Approved(cfg, req.agentType, scope);
            nlohmann::json approved = nlohmann::json::array();
            for (const auto& row : rows) approved.push_back(row.ToJson());
            req.env["HANCHO_WORKER_PERMITS"] = BuildWorkerPermitsJson(approved);
            WorkerEngineRegistry registry;
            const std::wstring enginesPath = PathJoinWide(GetEffectiveDataDirectoryWide(), L"engines.json");
            std::string engErr;
            if (PathExistsWide(enginesPath)) registry.LoadFromJson(ReadFileUtf8Wide(enginesPath), engErr);
            std::vector<std::string> warnings;
            const WorkerEngineDef* def = registry.Find(req.agentType);
            const std::string envConfig = registry.BuildPermitEnvironment(req.agentType, approved, warnings);
            if (def && !def->permitEnv.empty() && !envConfig.empty()) {
                req.env[def->permitEnv] = envConfig;
            }
            const std::string permitArgs = registry.BuildPermitArgs(req.agentType, approved, warnings);
            ReplaceAllInPlace(req.commandLine, "{permitArgs}", permitArgs);
            for (const auto& warning : warnings) {
                if (!job.holdReason.empty()) job.holdReason += "; ";
                job.holdReason += "permit: " + warning;
            }
        }
    }
    auto adapterIt = req.env.find("HANCHO_ADAPTER");
    if (adapterIt != req.env.end()) {
        if (!adapterIt->second.empty()) {
            req.agentType = adapterIt->second;
            req.jsonStream = true;
        }
        req.env.erase(adapterIt);
    }
    req.killExclude = job.killExclude;
    if (job.idle.outputSec > 0) {
        req.idleTimeoutSec = job.idle.outputSec;
        req.idleCpuSec = job.idle.cpuSec > 0 ? job.idle.cpuSec : 1;
    }
    if (job.idle.firstEventSec > 0) {
        req.idleFirstEventSec = job.idle.firstEventSec;
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

    // 091-7: continue the same engine session after a denial instead of starting over.
    if (!job.resumeSession.empty()) {
        const std::string eng = LowerAscii(req.agentType);
        if (eng == "opencode") {
            req.commandLine += " --session " + QuoteCommandArgLocal(job.resumeSession);
        } else if (eng == "claude") {
            req.commandLine += " --resume " + QuoteCommandArgLocal(job.resumeSession);
        }
        std::string target;
        if (!job.attempts.empty()) {
            auto perm = job.attempts.back().failure.detail.find("permission");
            if (perm != job.attempts.back().failure.detail.end() && perm->is_object()) {
                target = perm->value("target", std::string());
            }
        }
        req.env["HANCHO_PERMIT_RESUME"] =
            "Access to '" + target + "' is not permitted. Continue using methods inside the "
            "project (cwd) only; add it to `needs` and request a permit if it is genuinely required.";
    }

    // LLM workers can hang on a silent tool call for hours (the 2026-11-17 incident:
    // 04-navigation ran 8.5 h after "Calling grep"). Default to a 10-minute idle/hang
    // timeout unless the job sets one explicitly. CPU activity keeps a busy job alive.
    if (req.idleTimeoutSec == 0 && !job.idle.explicitZero && !req.agentType.empty()) {
        WorkerIdleDefaults defs = LoadWorkerIdleDefaults();
        req.idleTimeoutSec = defs.outputSec;
        if (req.idleFirstEventSec == 0) {
            req.idleFirstEventSec = defs.firstEventSec;
        }
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
        RefreshEngineHealthLocked(now);

        // 091-6: held jobs keep their pending state and show why they are waiting. If the job
        // can be switched to the next approved worker-list candidate, it is rewritten in place.
        bool healthChanged = false;
        // 091-7: a job held in waitingForPermit is resumed automatically when a human approves
        // one of the permits auto-requested on its denial. A rejected request fails the job.
        {
            permit::Config permits;
            std::string permitErr;
            const bool permitsLoaded = permit::Load(permit::DefaultPath(), permits, permitErr);
            for (auto& [name, tq] : queues_) {
                (void)name;
                for (auto& j : tq.jobs) {
                    if (j.state != "waitingForPermit") continue;
                    if (!permitsLoaded || j.waitPermits.empty()) continue;
                    bool approvedAny = false;
                    bool rejectedAll = true;
                    for (const auto& id : j.waitPermits) {
                        bool found = false;
                        for (const auto& row : permits.permits) {
                            if (row.id != id) continue;
                            found = true;
                            if (row.status == "approved") approvedAny = true;
                            else if (row.status != "rejected" && row.status != "revoked") rejectedAll = false;
                            break;
                        }
                        if (!found) rejectedAll = false;
                    }
                    if (approvedAny) {
                        j.state = "pending";
                        j.retryAtMs = now;
                        j.holdReason.clear();
                        j.waitPermits.clear();
                        TaskQueueFailureDiagnostic* diag = j.attempts.empty()
                            ? nullptr : &j.attempts.back().failure;
                        EmitEventLocked(tq, "jobPermitApproved", j.id,
                                        j.attempts.empty() ? 1 : j.attempts.back().n, diag);
                        if (diag) diag->nextSteps.push_back({"retry", nullptr, false,
                            "A permit was approved; the job is being re-run."});
                        MarkJobChangedLocked(j);
                        healthChanged = true;
                        SaveAllLocked();
                    } else if (rejectedAll) {
                        j.state = "failed";
                        j.retryAtMs = 0;
                        j.holdReason.clear();
                        j.waitPermits.clear();
                        TaskQueueFailureDiagnostic* diag = j.attempts.empty()
                            ? nullptr : &j.attempts.back().failure;
                        if (diag) {
                            diag->nextSteps.push_back({"fix_spec", nullptr, true,
                                "The permit was rejected; change the instructions to stay inside the project."});
                        }
                        MarkJobChangedLocked(j);
                        EmitEventLocked(tq, "jobFailed", j.id,
                                        j.attempts.empty() ? 1 : j.attempts.back().n, diag);
                        healthChanged = true;
                        SaveAllLocked();
                    }
                }
            }
        }
        for (auto& [name, tq] : queues_) {
            (void)name;
            for (auto& j : tq.jobs) {
                if (j.state != "pending") continue;
                j.holdReason = JobHoldReasonLocked(j, now);
                if (!j.holdReason.empty()) {
                    std::string eng, mod;
                    if (JobEngineModel(j, eng, mod)) {
                        healthChanged = TryEngineSwitchLocked(j, eng, mod, now) || healthChanged;
                        j.holdReason = JobHoldReasonLocked(j, now);
                    }
                }
            }
        }

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
            MarkJobChangedLocked(job);
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
        if (changed || healthChanged) SaveAllLocked();
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
        CheckSuspendWhenDone();
    }
}

// One L1 RunRecord per finished attempt (doc/obsoleted/design-v3.md 3.3). The record is built from
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
                } else if (r.failureSummary.find("content file") != std::string::npos ||
                           r.failureSummary.find("content_regex") != std::string::npos) {
                    r.condition = "content";
                } else if (r.failureSummary.find("engine result") != std::string::npos) {
                    r.condition = "engineResult";
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

                std::string text = job.name + " failed 3 times in a row: " + r3.failureClass;
                if (!condition.empty()) {
                    text += " (" + condition + ")";
                }

                memory::NoteInput note;
                note.text = text;
                note.scope = "job:" + job.name;
                note.authorType = "hancho";
                note.sources = {r1.id, r2.id, r3.id};
                note.tags = {"auto", "failure_streak", r3.failureClass};
                note.data = {{"kind", "failure_streak"}, {"job", job.name},
                             {"failureClass", r3.failureClass}, {"condition", condition}};
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

                std::string text = job.name + " is close to its timeout (p90 " +
                                   std::to_string(p90Sec) + " s / limit " +
                                   std::to_string(effTimeout) + " s)";

                memory::NoteInput note;
                note.text = text;
                note.scope = "job:" + job.name;
                note.authorType = "hancho";
                note.sources = {prev.id, curr.id};
                note.tags = {"auto", "timeout_warning"};
                note.data = {{"kind", "timeout_warning"}, {"job", job.name},
                             {"p90", std::to_string(p90Sec)}, {"timeoutSec", std::to_string(effTimeout)}};
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

                std::string text = job.name + " succeeded after a failure. Files possibly fixed: " + fileList;

                memory::NoteInput note;
                note.text = text;
                note.scope = "job:" + job.name;
                note.authorType = "hancho";
                note.sources = {prev.id, curr.id};
                note.tags = {"auto", "fix_diff"};
                note.data = {{"kind", "fix_diff"}, {"job", job.name}, {"files", fileList}};
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
        MarkJobChangedLocked(*it);
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
        MarkJobChangedLocked(job);
        tq.jobs.push_back(std::move(job));
        jobIndexDirty_ = true;
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
                if (job.state != "running" && job.state != "cancelled") continue;
                handled = true;

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
                    fin.cpuSec = snap.cpuTimeMs > 0 ? (static_cast<double>(snap.cpuTimeMs) / 1000.0) : 0.0;
                    if (!snap.activity.empty() || !snap.activityTarget.empty()) {
                        fin.lastActivity = snap.activity;
                        fin.lastActivityTarget = snap.activityTarget;
                        if (snap.activitySinceEpochMs > 0) {
                            int64_t nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::system_clock::now().time_since_epoch()).count();
                            fin.lastActivitySinceSec = nowMs > snap.activitySinceEpochMs
                                ? (nowMs - snap.activitySinceEpochMs) / 1000 : 0;
                        }
                    } else {
                        // Plain commands have no activity events. The gap between the last output
                        // and the end of the attempt still tells a timeout that cut off a job still
                        // printing apart from one that had gone quiet (triage uses it).
                        const uint64_t endTick = snap.endTickMs > 0 ? snap.endTickMs : 0;
                        const uint64_t lastTick = snap.lastOutputTickMs > 0 ? snap.lastOutputTickMs : snap.startTickMs;
                        if (snap.state == ProcState::Running) {
                            fin.lastActivity = "output";
                            fin.lastActivitySinceSec = static_cast<int64_t>(snap.idleMs / 1000);
                        } else if (endTick > 0 && lastTick > 0 && endTick >= lastTick) {
                            fin.lastActivity = "output";
                            fin.lastActivitySinceSec = static_cast<int64_t>((endTick - lastTick) / 1000);
                        }
                    }
                    fin.eventsFile = HanchoEventsFilePathForStream(snap.streamFilePath);
                    if (!fin.eventsFile.empty()) {
                        fin.events = ReadHanchoEventsFile(fin.eventsFile, 500, "", 0);
                    }
                    if (!snap.processTreeSnapshot.empty()) {
                        nlohmann::json pt = nlohmann::json::array();
                        for (const auto& item : snap.processTreeSnapshot) {
                            pt.push_back({
                                {"name", item.name},
                                {"pid", item.pid},
                                {"cpuSec", item.cpuSec}
                            });
                        }
                        fin.processTree = std::move(pt);
                    }
                }

                if (fin.stderrText.empty()) {
                    std::wstring stderrFile = PathJoinWide(GetTaskQueueLogDirWide(),
                        Utf8ToWide(SanitizeFileNameComponent(job.id)) + L"." + std::to_wstring(a.n) + L".stderr.log");
                    if (std::filesystem::exists(stderrFile)) {
                        fin.stderrText = ReadFileUtf8Wide(stderrFile);
                    }
                }

                if (job.state == "cancelled") {
                    a.end = GetCurrentTimestampStr();
                    a.reason = "killed";
                    a.exitCode = exitCode;
                    a.checkDetail = "cancelled";
                    job.updatedAt = a.end;
                    MarkJobChangedLocked(job);
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
                    if (!job.taskId.empty()) evs << ",\"task_id\":\"" << JsonEscape(job.taskId) << "\"";
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

// ---------------------------------------------------------------------------
// Task records (doc/task-lineage-cards.md, card 100-2a)
// ---------------------------------------------------------------------------

namespace {

// A job of a task as the state rules see it. Jobs already removed from a queue are kept on the
// task record (removedJobs) and become nodes too.
struct TaskNode {
    std::string id;
    std::string state;
    std::string supersededBy;
    bool paused = false;
    std::string takeoverResult; // result of a completed takeover, empty otherwise
};

enum class TaskCategory { Running, Failed, Cancelled, Succeeded };

TaskCategory CategoryOfNode(const TaskNode& node) {
    if (node.state == "takenOver") {
        if (node.takeoverResult == "failed") return TaskCategory::Failed;
        if (node.takeoverResult == "succeeded") return TaskCategory::Succeeded;
        return TaskCategory::Running; // taken over, not completed yet
    }
    if (node.state == "failed" || node.state == "blocked" || node.state == "closed") {
        return TaskCategory::Failed;
    }
    if (node.state == "cancelled") return TaskCategory::Cancelled;
    if (node.state == "succeeded") return TaskCategory::Succeeded;
    // pending, waiting, running, planned, waitingForPermit, and anything unknown: not finished.
    return TaskCategory::Running;
}

// The state rules of doc/task-lineage-cards.md ("状態の決め方"), without the closed check.
nlohmann::json ComputeTaskState(const std::vector<TaskNode>& nodes,
                                const std::function<bool(const std::string&)>& externalHasNode = nullptr) {
    nlohmann::json out = nlohmann::json::object();
    if (nodes.empty()) {
        out["state"] = "open";
        return out;
    }
    auto hasNode = [&nodes](const std::string& id) {
        for (const auto& n : nodes) {
            if (n.id == id) return true;
        }
        return false;
    };
    // The latest node of each lineage has no successor inside this task/direction. A successor id
    // that is not found anywhere is skipped so that node counts as the latest; a successor in
    // another direction of the same task supersedes this node externally.
    bool anyExternallySuperseded = false;
    std::vector<const TaskNode*> latest;
    for (const auto& n : nodes) {
        if (!n.supersededBy.empty()) {
            if (hasNode(n.supersededBy)) continue;
            if (externalHasNode && externalHasNode(n.supersededBy)) {
                anyExternallySuperseded = true;
                continue;
            }
        }
        latest.push_back(&n);
    }
    if (latest.empty() && anyExternallySuperseded) {
        out["state"] = "succeeded";
        return out;
    }
    bool running = latest.empty(); // a lineage with no end must not look finished
    bool failed = false;
    bool cancelled = false;
    bool paused = false;
    for (const TaskNode* n : latest) {
        if (n->paused) paused = true;
        switch (CategoryOfNode(*n)) {
        case TaskCategory::Running: running = true; break;
        case TaskCategory::Failed: failed = true; break;
        case TaskCategory::Cancelled: cancelled = true; break;
        case TaskCategory::Succeeded: break;
        }
    }
    if (running) {
        out["state"] = "running";
        if (paused) out["paused"] = true;
    } else if (failed) {
        out["state"] = "failed";
    } else if (cancelled) {
        out["state"] = "cancelled";
    } else {
        out["state"] = "succeeded";
    }
    return out;
}

// Live jobs of a task, then the jobs already removed from the queues (kept on the record).
// A lineage's first job has an empty taskId, so it belongs to the implicit task named by its id.
std::vector<TaskNode> CollectTaskNodes(const std::map<std::string, TaskQueue>& queues,
                                       const std::string& taskId, const nlohmann::json& record) {
    std::vector<TaskNode> nodes;
    for (const auto& [name, tq] : queues) {
        (void)name;
        for (const auto& job : tq.jobs) {
            const bool inTask = job.taskId == taskId || (job.taskId.empty() && job.id == taskId);
            if (!inTask) continue;
            TaskNode node;
            node.id = job.id;
            node.state = job.state;
            node.supersededBy = job.supersededBy;
            node.paused = job.paused;
            node.takeoverResult = job.takeover.result;
            nodes.push_back(std::move(node));
        }
    }
    if (record.is_object() && record.contains("removedJobs") && record["removedJobs"].is_array()) {
        for (const auto& r : record["removedJobs"]) {
            if (!r.is_object()) continue;
            TaskNode node;
            node.id = r.value("id", std::string());
            node.state = r.value("state", std::string());
            node.supersededBy = r.value("supersededBy", std::string());
            nodes.push_back(std::move(node));
        }
    }
    return nodes;
}

// Live jobs of a direction, plus any removed jobs kept on its record.
// For an implicit direction ("job:<jobId>"), collects the matching job (with empty directionId).
std::vector<TaskNode> CollectDirectionNodes(const std::map<std::string, TaskQueue>& queues,
                                            const std::string& directionId,
                                            const nlohmann::json& record) {
    std::vector<TaskNode> nodes;
    const bool isImplicit = directionId.rfind("job:", 0) == 0;
    const std::string implicitJobId = isImplicit ? directionId.substr(4) : std::string();
    for (const auto& [name, tq] : queues) {
        (void)name;
        for (const auto& job : tq.jobs) {
            const bool match = isImplicit
                ? (job.directionId.empty() && job.id == implicitJobId)
                : (job.directionId == directionId);
            if (!match) continue;
            TaskNode node;
            node.id = job.id;
            node.state = job.state;
            node.supersededBy = job.supersededBy;
            node.paused = job.paused;
            node.takeoverResult = job.takeover.result;
            nodes.push_back(std::move(node));
        }
    }
    if (!isImplicit && record.is_object() && record.contains("removedJobs") && record["removedJobs"].is_array()) {
        for (const auto& r : record["removedJobs"]) {
            if (!r.is_object()) continue;
            TaskNode node;
            node.id = r.value("id", std::string());
            node.state = r.value("state", std::string());
            node.supersededBy = r.value("supersededBy", std::string());
            nodes.push_back(std::move(node));
        }
    }
    return nodes;
}

std::string ReadBtStateFromFile(const std::string& btId) {
    if (btId.empty()) return "";
    const std::wstring btDir = PathJoinWide(GetLogsDirectoryWide(), L"behaviortrees");
    const std::wstring path = PathJoinWide(btDir, Utf8ToWide(SanitizeFileNameComponent(btId))) + L".json";
    const std::string content = ReadFileUtf8Wide(path);
    if (content.empty()) return "";
    const nlohmann::json doc = nlohmann::json::parse(content, nullptr, false);
    if (!doc.is_object()) return "";
    return doc.value("state", std::string());
}

bool IsJobUnfinishedForDirection(const TaskQueueJob& job) {
    if (job.state == "pending" || job.state == "waiting" || job.state == "running" ||
        job.state == "planned" || job.state == "waitingForPermit") {
        return true;
    }
    if (job.state == "takenOver" && job.takeover.completedAt.empty()) {
        return true;
    }
    return false;
}

std::vector<std::string> CollectTaskDirectionIds(const std::map<std::string, TaskQueue>& queues,
                                                 const DirectionStore& directionStore,
                                                 const std::string& taskId) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const auto& rec : directionStore.All()) {
        if (rec.value("taskId", std::string()) != taskId) continue;
        const std::string id = rec.value("id", std::string());
        if (!id.empty() && seen.insert(id).second) out.push_back(id);
    }
    for (const auto& [name, tq] : queues) {
        (void)name;
        for (const auto& job : tq.jobs) {
            const bool inTask = job.taskId == taskId || (job.taskId.empty() && job.id == taskId);
            if (!inTask) continue;
            if (!job.directionId.empty()) {
                if (seen.insert(job.directionId).second) out.push_back(job.directionId);
            } else if (!job.hidden) {
                const std::string impId = "job:" + job.id;
                if (seen.insert(impId).second) out.push_back(impId);
            }
        }
    }
    return out;
}

// One entry of a task's log. Empty fields are left out.
nlohmann::json MakeTaskLogEntry(const std::string& kind, const std::string& jobId, const std::string& btId,
                                const std::string& text, const std::string& session) {
    nlohmann::json entry = nlohmann::json::object();
    entry["at"] = GetCurrentTimestampStr();
    entry["kind"] = kind;
    if (!session.empty()) entry["session"] = session;
    if (!jobId.empty()) entry["job"] = jobId;
    if (!btId.empty()) entry["bt"] = btId;
    if (!text.empty()) entry["text"] = text;
    return entry;
}

} // namespace

void TaskQueueManager::RecordRemovedJobsLocked(const std::vector<TaskQueueJob>& gone) {
    const std::string now = GetCurrentTimestampStr();
    for (const auto& job : gone) {
        RecordJobTombstoneLocked(job.id);
        const nlohmann::json entry = {{"id", job.id}, {"state", job.state},
                                      {"supersedes", job.supersedes},
                                      {"supersededBy", job.supersededBy}};
        if (!job.taskId.empty()) {
            nlohmann::json record;
            if (taskStore_.Get(job.taskId, record)) { // an implicit task keeps no record
                nlohmann::json removed = record.value("removedJobs", nlohmann::json::array());
                if (!removed.is_array()) removed = nlohmann::json::array();
                removed.push_back(entry);
                record["removedJobs"] = removed;
                record["updatedAt"] = now;
                record["rev"] = BumpRevLocked();
                std::string err;
                // The removal goes ahead even if the record cannot be saved; only the history is lost then.
                taskStore_.Save(record, err);
            }
        }
        if (!job.directionId.empty()) {
            nlohmann::json dirRecord;
            if (directionStore_.Get(job.directionId, dirRecord)) {
                nlohmann::json removed = dirRecord.value("removedJobs", nlohmann::json::array());
                if (!removed.is_array()) removed = nlohmann::json::array();
                removed.push_back(entry);
                dirRecord["removedJobs"] = removed;
                dirRecord["updatedAt"] = now;
                dirRecord["rev"] = BumpRevLocked();
                std::string err;
                directionStore_.Save(dirRecord, err);
            }
        }
    }
}

bool TaskQueueManager::EnsureBtDirection(const std::string& btId,
                                         const std::string& existingDirectionId,
                                         const std::string& taskId,
                                         const std::string& title,
                                         const std::string& originJson,
                                         std::string& outDirectionId,
                                         std::string& outError) {
    outDirectionId.clear();
    outError.clear();
    const TaskQueueOrigin origin = ParseOriginJson(originJson);
    nlohmann::json originRecord = nlohmann::json::parse(OriginToJson(origin), nullptr, false);
    if (!originRecord.is_object()) originRecord = nlohmann::json::object();

    std::string dirTitle = title.empty() ? btId : title;
    if (dirTitle.size() > DirectionStore::kTitleMaxBytes) {
        dirTitle.resize(DirectionStore::kTitleMaxBytes);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (!existingDirectionId.empty()) {
        nlohmann::json existing;
        if (directionStore_.Get(existingDirectionId, existing)) {
            const bool needsPromote =
                existing.value("kind", std::string()) == "card" ||
                existing.value("btId", std::string()).empty();
            if (needsPromote) {
                existing["kind"] = "bt";
                existing["btId"] = btId;
                existing["updatedAt"] = GetCurrentTimestampStr();
                if (!taskId.empty() && existing.value("taskId", std::string()).empty()) {
                    existing["taskId"] = taskId;
                }
                existing["rev"] = BumpRevLocked();
                if (!directionStore_.Save(existing, outError)) return false;
                const std::string effTaskId =
                    taskId.empty() ? existing.value("taskId", std::string()) : taskId;
                if (!effTaskId.empty()) {
                    nlohmann::json extra = {
                        {"directionId", existingDirectionId},
                        {"directionKind", "bt"},
                        {"title", existing.value("title", dirTitle)}
                    };
                    if (existing.contains("card") && existing["card"].is_object()) {
                        const std::string cardNum = existing["card"].value("number", std::string());
                        if (!cardNum.empty()) extra["cardNumber"] = cardNum;
                    }
                    TaskLog(effTaskId, "direction_started", "", btId,
                            existing.value("title", dirTitle), origin.session, &extra);
                }
            }
            outDirectionId = existingDirectionId;
            return true;
        }
        if (!DirectionStore::ValidateId(existingDirectionId, outError)) {
            return false;
        }
        const std::string now = GetCurrentTimestampStr();
        nlohmann::json record = {
            {"id", existingDirectionId},
            {"kind", "bt"},
            {"taskId", taskId},
            {"btId", btId},
            {"createdAt", now},
            {"updatedAt", now},
            {"origin", originRecord},
            {"title", dirTitle},
            {"closed", nullptr},
            {"removedJobs", nlohmann::json::array()},
            {"rev", BumpRevLocked()}
        };
        if (!directionStore_.Create(record, outError)) return false;
        outDirectionId = existingDirectionId;
    } else {
        const std::string newId = directionStore_.NextId();
        const std::string now = GetCurrentTimestampStr();
        nlohmann::json record = {
            {"id", newId},
            {"kind", "bt"},
            {"taskId", taskId},
            {"btId", btId},
            {"createdAt", now},
            {"updatedAt", now},
            {"origin", originRecord},
            {"title", dirTitle},
            {"closed", nullptr},
            {"removedJobs", nlohmann::json::array()},
            {"rev", BumpRevLocked()}
        };
        if (!directionStore_.Create(record, outError)) return false;
        outDirectionId = newId;
    }

    if (!taskId.empty()) {
        nlohmann::json extra = {{"directionId", outDirectionId}, {"directionKind", "bt"}, {"title", dirTitle}};
        TaskLog(taskId, "direction_started", "", btId, dirTitle, origin.session, &extra);
    }
    return true;
}

bool TaskQueueManager::DirectionGet(const std::string& id, nlohmann::json& outRecord) const {
    return directionStore_.Get(id, outRecord);
}

std::vector<nlohmann::json> TaskQueueManager::DirectionAll() const {
    return directionStore_.All();
}

bool JobOrRemovedExists(const std::map<std::string, TaskQueue>& queues,
                        const TaskStore& taskStore,
                        const DirectionStore& directionStore,
                        const std::string& jobId) {
    if (jobId.empty()) return false;
    for (const auto& [name, tq] : queues) {
        (void)name;
        for (const auto& job : tq.jobs) {
            if (job.id == jobId) return true;
        }
    }
    auto hasRemoved = [&jobId](const std::vector<nlohmann::json>& records) {
        for (const auto& rec : records) {
            if (!rec.is_object() || !rec.contains("removedJobs") || !rec["removedJobs"].is_array()) continue;
            for (const auto& r : rec["removedJobs"]) {
                if (r.is_object() && r.value("id", std::string()) == jobId) return true;
            }
        }
        return false;
    };
    if (hasRemoved(directionStore.All())) return true;
    if (hasRemoved(taskStore.All())) return true;
    return false;
}

nlohmann::json TaskQueueManager::DirectionStateLocked(const std::string& id) const {
    auto externalHas = [this](const std::string& jobId) {
        return JobOrRemovedExists(queues_, taskStore_, directionStore_, jobId);
    };
    if (id.rfind("job:", 0) == 0) {
        const std::vector<TaskNode> nodes = CollectDirectionNodes(queues_, id, nlohmann::json());
        if (nodes.empty()) return nlohmann::json{{"state", "open"}};
        return ComputeTaskState(nodes, externalHas);
    }
    nlohmann::json record;
    directionStore_.Get(id, record);
    if (record.is_object() && record.contains("closed") && record["closed"].is_object()) {
        return nlohmann::json{
            {"state", "closed"},
            {"result", record["closed"].value("result", std::string())},
            {"reason", record["closed"].value("reason", std::string())}
        };
    }
    if (record.is_object() && record.value("kind", std::string()) == "bt") {
        const std::string btState = ReadBtStateFromFile(record.value("btId", std::string()));
        if (!btState.empty()) {
            nlohmann::json out = {{"state", btState}};
            if (btState == "paused") out["paused"] = true;
            return out;
        }
    }
    const std::vector<TaskNode> nodes = CollectDirectionNodes(queues_, id, record);
    if (nodes.empty()) {
        return nlohmann::json{{"state", "planned"}};
    }
    return ComputeTaskState(nodes, externalHas);
}

nlohmann::json TaskQueueManager::DirectionState(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return DirectionStateLocked(id);
}

void TaskQueueManager::MaybeLogDirectionFinishedLocked(const std::string& directionId, const std::string& taskId) {
    if (directionId.empty()) return;
    nlohmann::json dirRecord;
    if (!directionStore_.Get(directionId, dirRecord)) return;
    if (dirRecord.contains("closed") && dirRecord["closed"].is_object()) {
        UnwatchDirectionClientLocked(directionId);
        return;
    }
    const nlohmann::json st = DirectionStateLocked(directionId);
    const std::string s = st.value("state", std::string());
    if (s != "succeeded" && s != "failed" && s != "cancelled") return;
    UnwatchDirectionClientLocked(directionId);
    if (taskId.empty()) return;
    const std::string btId = dirRecord.value("btId", std::string());
    nlohmann::json extra = {{"directionId", directionId}, {"state", s}};
    TaskLog(taskId, "direction_finished", "", btId, s, "", &extra);
}

std::string TaskQueueManager::DirectionShow(const std::string& id, std::string& outError) {
    outError.clear();
    if (id.empty()) {
        outError = "direction id is required";
        return BuildError(outError);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (id.rfind("job:", 0) == 0) {
        const std::string jobId = id.substr(4);
        const TaskQueueJob* job = FindJobConst(jobId);
        if (!job || !job->directionId.empty()) {
            outError = "direction not found: " + id;
            return BuildError(outError);
        }
        const std::string title = job->summary.empty() ? job->name : job->summary;
        const std::string taskId = job->taskId.empty() ? job->id : job->taskId;
        nlohmann::json dir = {
            {"id", id},
            {"kind", "job"},
            {"implicit", true},
            {"taskId", taskId},
            {"title", title},
            {"createdAt", job->createdAt},
            {"updatedAt", job->updatedAt},
            {"closed", nullptr}
        };
        nlohmann::json jobs = nlohmann::json::array();
        jobs.push_back({{"id", job->id}, {"taskqueue", job->taskqueue}, {"name", job->name},
                        {"summary", job->summary}, {"state", job->state},
                        {"supersedes", job->supersedes}, {"supersededBy", job->supersededBy}});
        return nlohmann::json{{"ok", true}, {"direction", dir},
                              {"status", DirectionStateLocked(id)}, {"jobs", jobs}}.dump();
    }
    nlohmann::json record;
    if (!directionStore_.Get(id, record)) {
        outError = "direction not found: " + id;
        return BuildError(outError);
    }
    nlohmann::json jobs = nlohmann::json::array();
    for (const auto& [name, tq] : queues_) {
        (void)name;
        for (const auto& job : tq.jobs) {
            if (job.directionId != id) continue;
            jobs.push_back({{"id", job.id}, {"taskqueue", job.taskqueue}, {"name", job.name},
                            {"summary", job.summary}, {"state", job.state},
                            {"supersedes", job.supersedes}, {"supersededBy", job.supersededBy}});
        }
    }
    return nlohmann::json{{"ok", true}, {"direction", record},
                          {"status", DirectionStateLocked(id)}, {"jobs", jobs}}.dump();
}

namespace {

bool CompareCardNumberNatural(const std::string& a, const std::string& b) {
    size_t ia = 0;
    size_t ib = 0;
    while (ia < a.size() && ib < b.size()) {
        const bool da = std::isdigit(static_cast<unsigned char>(a[ia])) != 0;
        const bool db = std::isdigit(static_cast<unsigned char>(b[ib])) != 0;
        if (da && db) {
            size_t za = ia;
            while (za < a.size() && a[za] == '0') ++za;
            size_t ea = za;
            while (ea < a.size() && std::isdigit(static_cast<unsigned char>(a[ea]))) ++ea;

            size_t zb = ib;
            while (zb < b.size() && b[zb] == '0') ++zb;
            size_t eb = zb;
            while (eb < b.size() && std::isdigit(static_cast<unsigned char>(b[eb]))) ++eb;

            const size_t la = ea - za;
            const size_t lb = eb - zb;
            if (la != lb) return la < lb;
            const int cmp = a.compare(za, la, b, zb, lb);
            if (cmp != 0) return cmp < 0;
            // Tie-break on leading zero count so ordering is strict.
            if ((ea - ia) != (eb - ib)) return (ea - ia) < (eb - ib);
            ia = ea;
            ib = eb;
        } else {
            if (a[ia] != b[ib]) return static_cast<unsigned char>(a[ia]) < static_cast<unsigned char>(b[ib]);
            ++ia;
            ++ib;
        }
    }
    return a.size() < b.size();
}

bool IsDirectionSatisfiedForCardDep(const nlohmann::json& status) {
    const std::string state = status.value("state", std::string());
    if (state == "succeeded") return true;
    if (state == "closed" && status.value("result", std::string()) == "succeeded") return true;
    return false;
}

nlohmann::json FollowCardHeadLocked(const DirectionStore& store, const nlohmann::json& start) {
    nlohmann::json cur = start;
    std::unordered_set<std::string> seen;
    seen.insert(cur.value("id", std::string()));
    for (int step = 0; step < 1000; ++step) {
        const std::string nextId = cur.value("supersededBy", std::string());
        if (nextId.empty() || !seen.insert(nextId).second) break;
        nlohmann::json nextRec;
        if (!store.Get(nextId, nextRec) || !nextRec.is_object()) break;
        cur = std::move(nextRec);
    }
    return cur;
}

} // namespace

std::string TaskQueueManager::DirectionList(const std::string& taskIdFilter, const std::string& stateFilter,
                                            int64_t limit, std::string& outError, int64_t sinceVersion) {
    outError.clear();
    static const std::vector<std::string> kStates = {
        "planned", "open", "running", "paused", "failed", "cancelled", "succeeded", "closed"
    };
    if (!stateFilter.empty() && std::find(kStates.begin(), kStates.end(), stateFilter) == kStates.end()) {
        outError = "unknown direction state: " + stateFilter;
        return BuildError(outError);
    }
    const int64_t version = directionStore_.Version();
    if (sinceVersion >= 0 && sinceVersion == version && taskIdFilter.empty() &&
        stateFilter.empty() && limit <= 0) {
        return nlohmann::json{{"ok", true}, {"version", version}, {"unchanged", true}}.dump();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::unordered_map<std::string, std::unordered_map<std::string, std::string>> numberToDirIdByTask;
    for (const auto& rec : directionStore_.All()) {
        if (!rec.contains("card") || !rec["card"].is_object()) continue;
        const std::string tId = rec.value("taskId", std::string());
        const std::string num = rec["card"].value("number", std::string());
        if (tId.empty() || num.empty()) continue;
        const nlohmann::json head = FollowCardHeadLocked(directionStore_, rec);
        const std::string headId = head.value("id", std::string());
        if (!headId.empty()) numberToDirIdByTask[tId][num] = headId;
    }
    nlohmann::json directions = nlohmann::json::array();
    for (const auto& record : directionStore_.All()) {
        const std::string id = record.value("id", std::string());
        std::string recTaskId = record.value("taskId", std::string());
        if (!taskIdFilter.empty() && recTaskId != taskIdFilter) {
            bool jobMatches = false;
            for (const auto& [name, tq] : queues_) {
                (void)name;
                for (const auto& job : tq.jobs) {
                    if (job.directionId == id && job.taskId == taskIdFilter) {
                        jobMatches = true;
                        break;
                    }
                }
                if (jobMatches) break;
            }
            if (!jobMatches) continue;
        }
        const nlohmann::json status = DirectionStateLocked(id);
        const std::string state = status.value("state", std::string());
        if (!stateFilter.empty() && state != stateFilter) continue;
        nlohmann::json item = {
            {"id", id},
            {"kind", record.value("kind", std::string())},
            {"taskId", recTaskId},
            {"btId", record.value("btId", std::string())},
            {"title", record.value("title", std::string())},
            {"state", state},
            {"createdAt", record.value("createdAt", std::string())},
            {"updatedAt", record.value("updatedAt", std::string())}
        };
        if (status.contains("result")) item["result"] = status["result"];
        if (record.contains("card") && record["card"].is_object()) {
            const auto& card = record["card"];
            item["card"] = {
                {"number", card.value("number", std::string())},
                {"title", card.value("title", std::string())},
                {"template", card.value("template", std::string())}
            };
        }
        if (record.contains("after") && record["after"].is_array()) {
            item["after"] = record["after"];
            if (record.contains("card") && record["card"].is_object()) {
                nlohmann::json waitingFor = nlohmann::json::array();
                const auto taskIt = numberToDirIdByTask.find(recTaskId);
                for (const auto& dep : record["after"]) {
                    if (!dep.is_string()) continue;
                    const std::string depNum = dep.get<std::string>();
                    if (taskIt == numberToDirIdByTask.end()) {
                        waitingFor.push_back(depNum);
                        continue;
                    }
                    auto depIt = taskIt->second.find(depNum);
                    if (depIt == taskIt->second.end()) {
                        waitingFor.push_back(depNum);
                        continue;
                    }
                    const nlohmann::json depStatus = DirectionStateLocked(depIt->second);
                    if (!IsDirectionSatisfiedForCardDep(depStatus)) {
                        waitingFor.push_back(depNum);
                    }
                }
                item["waitingFor"] = std::move(waitingFor);
            }
        }
        const std::string supersedes = record.value("supersedes", std::string());
        if (!supersedes.empty()) item["supersedes"] = supersedes;
        const std::string supersededBy = record.value("supersededBy", std::string());
        if (!supersededBy.empty()) item["supersededBy"] = supersededBy;
        directions.push_back(std::move(item));
        if (limit > 0 && static_cast<int64_t>(directions.size()) >= limit) break;
    }
    if (limit <= 0 || static_cast<int64_t>(directions.size()) < limit) {
        for (const auto& [name, tq] : queues_) {
            (void)name;
            for (const auto& job : tq.jobs) {
                if (!job.directionId.empty() || job.hidden) continue;
                const std::string effTask = job.taskId.empty() ? job.id : job.taskId;
                if (!taskIdFilter.empty() && effTask != taskIdFilter) continue;
                const std::string impId = "job:" + job.id;
                const nlohmann::json status = DirectionStateLocked(impId);
                const std::string state = status.value("state", std::string());
                if (!stateFilter.empty() && state != stateFilter) continue;
                directions.push_back({
                    {"id", impId},
                    {"kind", "job"},
                    {"implicit", true},
                    {"taskId", effTask},
                    {"btId", job.bt.id},
                    {"title", job.summary.empty() ? job.name : job.summary},
                    {"state", state},
                    {"createdAt", job.createdAt},
                    {"updatedAt", job.updatedAt}
                });
                if (limit > 0 && static_cast<int64_t>(directions.size()) >= limit) break;
            }
            if (limit > 0 && static_cast<int64_t>(directions.size()) >= limit) break;
        }
    }
    return nlohmann::json{{"ok", true}, {"version", version}, {"directions", directions}}.dump();
}

std::string TaskQueueManager::DirectionClose(const std::string& id, const std::string& result,
                                             const std::string& reason, std::string& outError) {
    outError.clear();
    if (result != "succeeded" && result != "failed" && result != "ignored") {
        outError = "result must be succeeded, failed, or ignored";
        return BuildError(outError);
    }
    if (reason.empty()) {
        outError = "reason is required";
        return BuildError(outError);
    }
    if (!DirectionStore::ValidateSize("reason", reason, DirectionStore::kReasonMaxBytes, outError)) {
        return BuildError(outError);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json record;
    if (!directionStore_.Get(id, record)) {
        outError = "direction not found: " + id;
        return BuildError(outError);
    }
    if (record.contains("closed") && record["closed"].is_object()) {
        outError = "direction '" + id + "' is already closed";
        return BuildError(outError);
    }
    for (const auto& [name, tq] : queues_) {
        (void)name;
        for (const auto& job : tq.jobs) {
            if (job.directionId != id) continue;
            if (IsJobUnfinishedForDirection(job)) {
                outError = "direction '" + id + "' has active job '" + job.id + "'; cancel it first";
                return BuildError(outError);
            }
        }
    }
    if (record.value("kind", std::string()) == "bt") {
        const std::string btState = ReadBtStateFromFile(record.value("btId", std::string()));
        if (btState == "running" || btState == "paused") {
            outError = "direction '" + id + "' behavior tree is still " + btState + "; cancel it first";
            return BuildError(outError);
        }
    }
    const std::string now = GetCurrentTimestampStr();
    record["closed"] = nlohmann::json{{"result", result}, {"reason", reason}, {"at", now}};
    record["updatedAt"] = now;
    record["rev"] = BumpRevLocked();
    if (!directionStore_.Save(record, outError)) return BuildError(outError);
    const std::string taskId = record.value("taskId", std::string());
    if (!taskId.empty()) {
        nlohmann::json extra = {{"directionId", id}, {"result", result}, {"reason", reason}};
        TaskLog(taskId, "direction_closed", "", record.value("btId", std::string()),
                result + ": " + reason, "", &extra);
    }
    return nlohmann::json{{"ok", true}, {"id", id}, {"state", "closed"}, {"result", result}}.dump();
}

std::string TaskQueueManager::DirectionReopen(const std::string& id, const std::string& reason,
                                              std::string& outError) {
    outError.clear();
    if (reason.empty()) {
        outError = "reason is required";
        return BuildError(outError);
    }
    if (!DirectionStore::ValidateSize("reason", reason, DirectionStore::kReasonMaxBytes, outError)) {
        return BuildError(outError);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json record;
    if (!directionStore_.Get(id, record)) {
        outError = "direction not found: " + id;
        return BuildError(outError);
    }
    if (!record.contains("closed") || !record["closed"].is_object()) {
        outError = "direction '" + id + "' is not closed";
        return BuildError(outError);
    }
    record["closed"] = nullptr;
    record["updatedAt"] = GetCurrentTimestampStr();
    record["rev"] = BumpRevLocked();
    if (!directionStore_.Save(record, outError)) return BuildError(outError);
    const std::string taskId = record.value("taskId", std::string());
    if (!taskId.empty()) {
        nlohmann::json extra = {{"directionId", id}, {"reason", reason}};
        TaskLog(taskId, "direction_reopened", "", record.value("btId", std::string()),
                reason, "", &extra);
    }
    const nlohmann::json status = DirectionStateLocked(id);
    return nlohmann::json{{"ok", true}, {"id", id}, {"state", status.value("state", std::string())}}.dump();
}

std::string TaskQueueManager::DirectionPurgeOlderThan(int64_t olderThanSec, std::string& outError) {
    outError.clear();
    if (olderThanSec < 0) {
        outError = "older_than_sec must be >= 0";
        return BuildError(outError);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t cutoff = GetNowEpochSec() - olderThanSec;
    int64_t purged = 0;
    for (const auto& record : directionStore_.All()) {
        const std::string id = record.value("id", std::string());
        bool shouldPurge = false;
        if (record.contains("closed") && record["closed"].is_object()) {
            const int64_t closedAt = ParseTimestampStrToEpoch(record["closed"].value("at", std::string()));
            if (closedAt > 0 && closedAt < cutoff) shouldPurge = true;
        } else if (record.value("taskId", std::string()).empty()) {
            // Implicit-task direction: purge if finished and updatedAt is older than cutoff.
            const std::string st = DirectionStateLocked(id).value("state", std::string());
            if (st == "succeeded" || st == "failed" || st == "cancelled") {
                const int64_t updatedAt = ParseTimestampStrToEpoch(record.value("updatedAt", std::string()));
                if (updatedAt > 0 && updatedAt < cutoff) shouldPurge = true;
            }
        }
        if (!shouldPurge) continue;
        std::string err;
        if (directionStore_.Remove(id, err)) {
            ++purged;
            BumpRevLocked();
        }
    }
    return nlohmann::json{{"ok", true}, {"purged", purged}}.dump();
}

nlohmann::json TaskQueueManager::TaskStateLocked(const std::string& taskId) const {
    nlohmann::json record;
    taskStore_.Get(taskId, record);
    if (record.is_object() && record.contains("closed") && record["closed"].is_object()) {
        return nlohmann::json{
            {"state", "closed"},
            {"result", record["closed"].value("result", std::string())},
            {"reason", record["closed"].value("reason", std::string())}
        };
    }
    const std::vector<std::string> dirIds = CollectTaskDirectionIds(queues_, directionStore_, taskId);
    if (dirIds.empty()) {
        return ComputeTaskState(CollectTaskNodes(queues_, taskId, record));
    }
    bool anyRunning = false;
    bool anyPaused = false;
    bool anyFailed = false;
    bool anyPlanned = false;
    bool anyCancelled = false;
    int64_t closedDirections = 0;
    for (const std::string& dirId : dirIds) {
        if (dirId.rfind("job:", 0) != 0) {
            nlohmann::json dirRecord;
            if (directionStore_.Get(dirId, dirRecord) && dirRecord.is_object()) {
                if (dirRecord.contains("closed") && dirRecord["closed"].is_object()) {
                    ++closedDirections;
                    continue;
                }
                if (!dirRecord.value("supersededBy", std::string()).empty()) {
                    continue;
                }
            }
        }
        const nlohmann::json ds = DirectionStateLocked(dirId);
        const std::string s = ds.value("state", std::string("open"));
        if (ds.value("paused", false) || s == "paused") anyPaused = true;
        if (s == "running" || s == "paused" || s == "idle") {
            anyRunning = true;
        } else if (s == "failed") {
            anyFailed = true;
        } else if (s == "planned" || s == "open") {
            anyPlanned = true;
        } else if (s == "cancelled") {
            anyCancelled = true;
        }
    }
    nlohmann::json out = nlohmann::json::object();
    if (anyRunning) {
        out["state"] = "running";
        if (anyPaused) out["paused"] = true;
    } else if (anyFailed) {
        out["state"] = "failed";
    } else if (anyPlanned) {
        out["state"] = "open";
    } else if (anyCancelled) {
        out["state"] = "cancelled";
    } else {
        out["state"] = "succeeded";
    }
    if (closedDirections > 0) out["closedDirections"] = closedDirections;
    return out;
}

nlohmann::json TaskQueueManager::TaskState(const std::string& taskId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return TaskStateLocked(taskId);
}

void TaskQueueManager::TaskLog(const std::string& taskId, const std::string& kind, const std::string& jobId,
                               const std::string& btId, const std::string& text, const std::string& session,
                               const nlohmann::json* extra) {
    if (taskId.empty()) return;
    nlohmann::json entry = MakeTaskLogEntry(kind, jobId, btId, text, session);
    if (extra && extra->is_object()) {
        for (auto it = extra->begin(); it != extra->end(); ++it) entry[it.key()] = it.value();
    }
    std::string err;
    taskStore_.AppendLog(taskId, entry, err);
}

std::string TaskQueueManager::TaskCreate(const std::string& title, const std::string& intent,
                                         const std::string& id, const std::string& originJson,
                                         std::string& outError, const std::string& cardDefaultsJson) {
    outError.clear();
    if (title.empty()) {
        outError = "title is required";
        return BuildError(outError);
    }
    if (!TaskStore::ValidateSize("title", title, TaskStore::kTitleMaxBytes, outError) ||
        !TaskStore::ValidateSize("intent", intent, TaskStore::kIntentMaxBytes, outError)) {
        return BuildError(outError);
    }
    if (!id.empty() && !TaskStore::ValidateId(id, outError)) return BuildError(outError);
    nlohmann::json cardDefaultsObj;
    if (!cardDefaultsJson.empty()) {
        cardDefaultsObj = nlohmann::json::parse(cardDefaultsJson, nullptr, false);
        if (cardDefaultsObj.is_discarded() || !cardDefaultsObj.is_object()) {
            outError = "cardDefaults must be a JSON object";
            return BuildError(outError);
        }
    }
    const TaskQueueOrigin origin = ParseOriginJson(originJson);
    nlohmann::json originRecord = nlohmann::json::parse(OriginToJson(origin), nullptr, false);
    if (!originRecord.is_object()) originRecord = nlohmann::json::object();

    std::lock_guard<std::mutex> lock(mutex_);
    const std::string newId = id.empty() ? taskStore_.NextId() : id;
    const std::string now = GetCurrentTimestampStr();
    nlohmann::json record = {{"id", newId}, {"title", title}, {"intent", intent},
                             {"createdAt", now}, {"updatedAt", now}, {"origin", originRecord},
                             {"closed", nullptr}, {"removedJobs", nlohmann::json::array()}};
    if (cardDefaultsObj.is_object()) {
        record["cardDefaults"] = cardDefaultsObj;
    }
    record["rev"] = BumpRevLocked();
    if (!taskStore_.Create(record, outError)) return BuildError(outError);
    TaskLog(newId, "created", "", "", title, origin.session);
    return nlohmann::json{{"ok", true}, {"id", newId}, {"title", title}, {"state", "open"}}.dump();
}

std::string TaskQueueManager::TaskUpdate(const std::string& id,
                                         bool hasTitle, const std::string& title,
                                         bool hasIntent, const std::string& intent,
                                         bool hasCardDefaults, const std::string& cardDefaultsJson,
                                         const std::string& session, std::string& outError) {
    outError.clear();
    if (id.empty()) {
        outError = "task id is required";
        return BuildError(outError);
    }
    if (hasTitle) {
        if (title.empty()) {
            outError = "title cannot be empty";
            return BuildError(outError);
        }
        if (!TaskStore::ValidateSize("title", title, TaskStore::kTitleMaxBytes, outError)) {
            return BuildError(outError);
        }
    }
    if (hasIntent && !TaskStore::ValidateSize("intent", intent, TaskStore::kIntentMaxBytes, outError)) {
        return BuildError(outError);
    }
    nlohmann::json cardDefaultsObj;
    if (hasCardDefaults) {
        cardDefaultsObj = cardDefaultsJson.empty()
            ? nlohmann::json::object()
            : nlohmann::json::parse(cardDefaultsJson, nullptr, false);
        if (cardDefaultsObj.is_discarded() || !cardDefaultsObj.is_object()) {
            outError = "cardDefaults must be a JSON object";
            return BuildError(outError);
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json record;
    if (!taskStore_.Get(id, record)) {
        outError = "task not found: " + id;
        return BuildError(outError);
    }
    if (hasTitle) record["title"] = title;
    if (hasIntent) record["intent"] = intent;
    if (hasCardDefaults) record["cardDefaults"] = cardDefaultsObj;
    record["updatedAt"] = GetCurrentTimestampStr();
    record["rev"] = BumpRevLocked();
    if (!taskStore_.Save(record, outError)) return BuildError(outError);

    TaskLog(id, "task_updated", "", "", record.value("title", std::string()), session);
    nlohmann::json out = {
        {"ok", true},
        {"id", id},
        {"title", record.value("title", std::string())},
        {"task", record}
    };
    return out.dump();
}

bool TaskQueueManager::TaskGet(const std::string& id, nlohmann::json& outRecord) const {
    return taskStore_.Get(id, outRecord);
}

std::string TaskQueueManager::TaskShow(const std::string& id, int64_t logLimit, std::string& outError) {
    outError.clear();
    nlohmann::json record;
    if (id.empty() || !taskStore_.Get(id, record)) {
        outError = "task not found: " + id;
        return BuildError(outError);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json jobs = nlohmann::json::array();
    for (const auto& [name, tq] : queues_) {
        (void)name;
        for (const auto& job : tq.jobs) {
            if (job.taskId != id) continue;
            jobs.push_back({{"id", job.id}, {"taskqueue", job.taskqueue}, {"name", job.name},
                            {"state", job.state}, {"supersedes", job.supersedes},
                            {"supersededBy", job.supersededBy}});
        }
    }
    std::vector<LineageFailureNode> lineageNodes;
    CollectLineageNodesForTask(queues_, id, record, false, lineageNodes);
    int repeatedFailures = 0;
    std::unordered_set<std::string> visitedLineage;
    for (const auto& node : lineageNodes) {
        if (visitedLineage.count(node.id)) continue;
        repeatedFailures = std::max(repeatedFailures,
                                    CountLineageFailuresFrom(lineageNodes, node.id, &visitedLineage));
    }
    const size_t limit = logLimit > 0 ? static_cast<size_t>(logLimit) : 200;
    const auto entries = taskStore_.ReadLog(id, limit);
    nlohmann::json log = nlohmann::json::array();
    for (const auto& entry : entries) log.push_back(entry);
    std::unordered_map<std::string, std::string> numberToDirId;
    for (const auto& rec : directionStore_.All()) {
        if (rec.value("taskId", std::string()) != id) continue;
        if (!rec.contains("card") || !rec["card"].is_object()) continue;
        const std::string num = rec["card"].value("number", std::string());
        const nlohmann::json head = FollowCardHeadLocked(directionStore_, rec);
        const std::string headId = head.value("id", std::string());
        if (!num.empty() && !headId.empty()) numberToDirId[num] = headId;
    }
    nlohmann::json directions = nlohmann::json::array();
    std::vector<nlohmann::json> cardItems;
    for (const std::string& dirId : CollectTaskDirectionIds(queues_, directionStore_, id)) {
        const nlohmann::json ds = DirectionStateLocked(dirId);
        std::string kind;
        std::string title;
        std::string cardNumber;
        std::string supersedes;
        std::string supersededBy;
        nlohmann::json afterArr = nlohmann::json::array();
        nlohmann::json waitingFor = nlohmann::json::array();
        bool isCard = false;
        if (dirId.rfind("job:", 0) == 0) {
            kind = "job";
            const TaskQueueJob* job = FindJobConst(dirId.substr(4));
            if (job) title = job->summary.empty() ? job->name : job->summary;
        } else {
            nlohmann::json dirRec;
            if (directionStore_.Get(dirId, dirRec) && dirRec.is_object()) {
                kind = dirRec.value("kind", std::string());
                title = dirRec.value("title", std::string());
                supersedes = dirRec.value("supersedes", std::string());
                supersededBy = dirRec.value("supersededBy", std::string());
                if (dirRec.contains("card") && dirRec["card"].is_object()) {
                    isCard = true;
                    cardNumber = dirRec["card"].value("number", std::string());
                    if (title.empty()) title = dirRec["card"].value("title", std::string());
                    if (dirRec.contains("after") && dirRec["after"].is_array()) {
                        afterArr = dirRec["after"];
                        for (const auto& dep : afterArr) {
                            if (!dep.is_string()) continue;
                            const std::string depNum = dep.get<std::string>();
                            auto it = numberToDirId.find(depNum);
                            if (it == numberToDirId.end()) {
                                waitingFor.push_back(depNum);
                                continue;
                            }
                            const nlohmann::json depStatus = DirectionStateLocked(it->second);
                            if (!IsDirectionSatisfiedForCardDep(depStatus)) {
                                waitingFor.push_back(depNum);
                            }
                        }
                    }
                }
            }
        }
        const std::string dState = ds.value("state", std::string());
        nlohmann::json d = {
            {"id", dirId},
            {"kind", kind},
            {"title", title},
            {"state", dState}
        };
        if (!cardNumber.empty()) d["cardNumber"] = cardNumber;
        if (!afterArr.empty()) d["after"] = afterArr;
        if (!waitingFor.empty()) d["waitingFor"] = waitingFor;
        if (!supersedes.empty()) d["supersedes"] = supersedes;
        if (!supersededBy.empty()) d["supersededBy"] = supersededBy;
        if (ds.contains("result")) d["result"] = ds["result"];
        if (isCard && supersededBy.empty()) {
            nlohmann::json cItem = {
                {"directionId", dirId},
                {"number", cardNumber},
                {"title", title},
                {"state", dState},
                {"after", afterArr},
                {"waitingFor", waitingFor}
            };
            if (!supersedes.empty()) cItem["supersedes"] = supersedes;
            if (ds.contains("result")) cItem["result"] = ds["result"];
            cardItems.push_back(std::move(cItem));
        }
        directions.push_back(std::move(d));
    }
    std::sort(cardItems.begin(), cardItems.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return CompareCardNumberNatural(a.value("number", std::string()), b.value("number", std::string()));
    });
    nlohmann::json cards = nlohmann::json::array();
    for (auto& c : cardItems) cards.push_back(std::move(c));
    return nlohmann::json{{"ok", true}, {"task", record}, {"status", TaskStateLocked(id)},
                          {"directions", directions}, {"cards", cards}, {"jobs", jobs}, {"log", log},
                          {"repeatedFailures", repeatedFailures}}.dump();
}

std::string TaskQueueManager::TaskList(const std::string& stateFilter, int64_t limit, std::string& outError,
                                       int64_t sinceVersion, int64_t finishedLimit,
                                       const std::string& before, const std::string& query) {
    outError.clear();
    static const std::vector<std::string> kStates = {"open", "running", "failed", "cancelled", "succeeded", "closed"};
    if (!stateFilter.empty() && std::find(kStates.begin(), kStates.end(), stateFilter) == kStates.end()) {
        outError = "unknown task state: " + stateFilter;
        return BuildError(outError);
    }
    const int64_t version = taskStore_.Version();
    if (sinceVersion >= 0 && sinceVersion == version && stateFilter.empty() && limit <= 0 &&
        finishedLimit == 0 && before.empty() && query.empty()) {
        return nlohmann::json{{"ok", true}, {"version", version}, {"unchanged", true}}.dump();
    }
    const int64_t effectiveLimit = (finishedLimit < 0) ? GetTaskListRecentCount() : finishedLimit;
    std::lock_guard<std::mutex> lock(mutex_);

    std::unordered_map<std::string, std::vector<std::string>> taskDirTitles;
    std::unordered_map<std::string, std::vector<std::pair<std::string, std::string>>> taskJobs;
    if (!query.empty()) {
        for (const auto& rec : directionStore_.All()) {
            const std::string tid = rec.value("taskId", std::string());
            if (tid.empty()) continue;
            const std::string dtitle = rec.value("title", std::string());
            if (!dtitle.empty()) taskDirTitles[tid].push_back(dtitle);
            if (rec.contains("card") && rec["card"].is_object()) {
                const std::string ctitle = rec["card"].value("title", std::string());
                if (!ctitle.empty()) taskDirTitles[tid].push_back(ctitle);
            }
        }
        for (const auto& [qname, tq] : queues_) {
            (void)qname;
            for (const auto& job : tq.jobs) {
                const std::string effTask = job.taskId.empty() ? job.id : job.taskId;
                taskJobs[effTask].emplace_back(job.id, job.summary);
            }
        }
    }

    struct ListedTask {
        std::string id;
        std::string finishedAt;
        nlohmann::json item;
    };
    std::vector<ListedTask> unfinished;
    std::vector<ListedTask> finished;
    for (const auto& record : taskStore_.All()) {
        const std::string id = record.value("id", std::string());
        const nlohmann::json status = TaskStateLocked(id);
        const std::string state = status.value("state", std::string());
        if (!stateFilter.empty() && state != stateFilter) continue;
        const std::string title = record.value("title", std::string());
        const std::string intent = record.value("intent", std::string());
        if (!query.empty()) {
            bool matched = ContainsCaseInsensitive(id, query) ||
                           ContainsCaseInsensitive(title, query) ||
                           ContainsCaseInsensitive(intent, query);
            if (!matched) {
                auto dit = taskDirTitles.find(id);
                if (dit != taskDirTitles.end()) {
                    for (const auto& dt : dit->second) {
                        if (ContainsCaseInsensitive(dt, query)) { matched = true; break; }
                    }
                }
            }
            if (!matched) {
                auto jit = taskJobs.find(id);
                if (jit != taskJobs.end()) {
                    for (const auto& [jid, jsum] : jit->second) {
                        if (ContainsCaseInsensitive(jid, query) || ContainsCaseInsensitive(jsum, query)) {
                            matched = true;
                            break;
                        }
                    }
                }
            }
            if (!matched) continue;
        }
        const std::string createdAt = record.value("createdAt", std::string());
        const std::string updatedAt = record.value("updatedAt", std::string());
        const std::string sortTs = !updatedAt.empty() ? updatedAt : createdAt;
        nlohmann::json item = {{"id", id}, {"title", title}, {"state", state},
                               {"createdAt", createdAt}, {"updatedAt", updatedAt}};
        if (IsFinishedTaskState(state)) {
            finished.push_back({id, sortTs, std::move(item)});
        } else {
            unfinished.push_back({id, sortTs, std::move(item)});
        }
    }
    auto byNewest = [](const ListedTask& a, const ListedTask& b) {
        if (a.finishedAt != b.finishedAt) return a.finishedAt > b.finishedAt;
        return a.id > b.id;
    };
    std::stable_sort(unfinished.begin(), unfinished.end(), byNewest);
    std::stable_sort(finished.begin(), finished.end(), byNewest);
    if (!before.empty()) {
        unfinished.clear();
        std::string cursorTs, cursorId;
        ParseBeforeCursor(before, cursorTs, cursorId);
        std::vector<ListedTask> older;
        older.reserve(finished.size());
        for (auto& t : finished) {
            if (IsOlderThanCursor(t.finishedAt, t.id, cursorTs, cursorId)) {
                older.push_back(std::move(t));
            }
        }
        finished = std::move(older);
    }
    bool more = false;
    int64_t remaining = 0;
    std::string nextBefore;
    if (effectiveLimit > 0 && static_cast<int64_t>(finished.size()) > effectiveLimit) {
        more = true;
        remaining = static_cast<int64_t>(finished.size()) - effectiveLimit;
        finished.resize(static_cast<size_t>(effectiveLimit));
        nextBefore = MakeBeforeCursor(finished.back().finishedAt, finished.back().id);
    }
    nlohmann::json tasks = nlohmann::json::array();
    for (auto& t : unfinished) {
        if (limit > 0 && static_cast<int64_t>(tasks.size()) >= limit) break;
        tasks.push_back(std::move(t.item));
    }
    for (auto& t : finished) {
        if (limit > 0 && static_cast<int64_t>(tasks.size()) >= limit) break;
        tasks.push_back(std::move(t.item));
    }
    return nlohmann::json{{"ok", true}, {"version", version}, {"tasks", tasks},
                          {"more", more}, {"next_before", nextBefore}, {"remaining", remaining}}.dump();
}

std::string TaskQueueManager::TaskNote(const std::string& id, const std::string& text, const std::string& session,
                                       std::string& outError) {
    outError.clear();
    if (text.empty()) {
        outError = "text is required";
        return BuildError(outError);
    }
    if (!TaskStore::ValidateSize("note", text, TaskStore::kNoteMaxBytes, outError)) return BuildError(outError);
    if (!taskStore_.AppendLog(id, MakeTaskLogEntry("note", "", "", text, session), outError)) {
        return BuildError(outError);
    }
    return nlohmann::json{{"ok", true}, {"id", id}}.dump();
}

std::string TaskQueueManager::TaskClose(const std::string& id, const std::string& result, const std::string& reason,
                                        std::string& outError) {
    outError.clear();
    if (result != "succeeded" && result != "failed") {
        outError = "result must be succeeded or failed";
        return BuildError(outError);
    }
    if (reason.empty()) {
        outError = "reason is required";
        return BuildError(outError);
    }
    if (!TaskStore::ValidateSize("reason", reason, TaskStore::kReasonMaxBytes, outError)) return BuildError(outError);
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json record;
    if (!taskStore_.Get(id, record)) {
        outError = "task not found: " + id;
        return BuildError(outError);
    }
    if (record.contains("closed") && record["closed"].is_object()) {
        outError = "task '" + id + "' is already closed";
        return BuildError(outError);
    }
    const std::string now = GetCurrentTimestampStr();
    record["closed"] = nlohmann::json{{"result", result}, {"reason", reason}, {"at", now}};
    record["updatedAt"] = now;
    record["rev"] = BumpRevLocked();
    if (!taskStore_.Save(record, outError)) return BuildError(outError);
    TaskLog(id, "closed", "", "", result + ": " + reason, "");
    return nlohmann::json{{"ok", true}, {"id", id}, {"state", "closed"}, {"result", result}}.dump();
}

std::string TaskQueueManager::TaskReopen(const std::string& id, const std::string& reason, std::string& outError) {
    outError.clear();
    if (reason.empty()) {
        outError = "reason is required";
        return BuildError(outError);
    }
    if (!TaskStore::ValidateSize("reason", reason, TaskStore::kReasonMaxBytes, outError)) return BuildError(outError);
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json record;
    if (!taskStore_.Get(id, record)) {
        outError = "task not found: " + id;
        return BuildError(outError);
    }
    if (!record.contains("closed") || !record["closed"].is_object()) {
        outError = "task '" + id + "' is not closed";
        return BuildError(outError);
    }
    record["closed"] = nullptr;
    record["updatedAt"] = GetCurrentTimestampStr();
    record["rev"] = BumpRevLocked();
    if (!taskStore_.Save(record, outError)) return BuildError(outError);
    TaskLog(id, "reopened", "", "", reason, "");
    const nlohmann::json status = TaskStateLocked(id);
    return nlohmann::json{{"ok", true}, {"id", id}, {"state", status.value("state", std::string())}}.dump();
}

std::string TaskQueueManager::TaskPurgeOlderThan(int64_t olderThanSec, std::string& outError) {
    outError.clear();
    if (olderThanSec < 0) {
        outError = "older_than_sec must be >= 0";
        return BuildError(outError);
    }
    // Only closed tasks are removed. Their closing time decides the age.
    std::lock_guard<std::mutex> lock(mutex_);
    const int64_t cutoff = GetNowEpochSec() - olderThanSec;
    int64_t purged = 0;
    for (const auto& record : taskStore_.All()) {
        if (!record.contains("closed") || !record["closed"].is_object()) continue;
        const int64_t closedAt = ParseTimestampStrToEpoch(record["closed"].value("at", std::string()));
        if (closedAt <= 0 || closedAt >= cutoff) continue;
        std::string err;
        if (taskStore_.Remove(record.value("id", std::string()), err)) {
            ++purged;
            BumpRevLocked();
        }
    }
    return nlohmann::json{{"ok", true}, {"purged", purged}}.dump();
}

bool TaskQueueManager::ValidateTaskActive(const std::string& id, std::string& outError) const {
    outError.clear();
    nlohmann::json record;
    if (id.empty() || !taskStore_.Get(id, record)) {
        outError = "task not found: " + id;
        return false;
    }
    if (record.contains("closed") && record["closed"].is_object()) {
        outError = "task '" + id + "' is closed";
        return false;
    }
    return true;
}

std::string TaskQueueManager::TaskWait(const std::string& id, int timeoutMs, int64_t logLimit,
                                       std::string& outError) {
    outError.clear();
    nlohmann::json record;
    if (id.empty() || !taskStore_.Get(id, record)) {
        outError = "task not found: " + id;
        return BuildError(outError);
    }
    uint64_t start = GetNowTickMs();
    bool timedOut = false;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            bool anyActive = false;
            for (const auto& [name, tq] : queues_) {
                (void)name;
                for (const auto& job : tq.jobs) {
                    if (job.taskId != id) continue;
                    if (job.state == "pending" || job.state == "waiting" ||
                        job.state == "running" || job.state == "planned" ||
                        job.state == "waitingForPermit") {
                        anyActive = true;
                        break;
                    }
                }
                if (anyActive) break;
            }
            if (!anyActive) break;
            if (timeoutMs >= 0 && static_cast<int64_t>(GetNowTickMs() - start) >= timeoutMs) {
                timedOut = true;
                break;
            }
            cv_.wait_for(lock, std::chrono::milliseconds(100));
        }
    }
    const std::string shown = TaskShow(id, logLimit, outError);
    if (!outError.empty()) return shown;
    nlohmann::json out = nlohmann::json::parse(shown, nullptr, false);
    if (out.is_object()) {
        out["timedOut"] = timedOut;
        return out.dump();
    }
    return shown;
}

std::string TaskQueueManager::DirectionCancelJobs(const std::string& id, std::string& outError,
                                                  const std::string& reason) {
    outError.clear();
    nlohmann::json record;
    if (!directionStore_.Get(id, record)) {
        outError = "direction not found: " + id;
        return BuildError(outError);
    }
    std::vector<std::string> jobIds;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [name, tq] : queues_) {
            (void)name;
            for (const auto& job : tq.jobs) {
                if (job.directionId != id) continue;
                if (job.state == "succeeded" || job.state == "failed" ||
                    job.state == "cancelled" || job.state == "closed" || job.state == "takenOver") {
                    continue;
                }
                jobIds.push_back(job.id);
            }
        }
    }
    int cancelled = 0;
    for (const auto& jid : jobIds) {
        std::string err;
        TaskQueueCancel(jid, err, reason);
        if (err.empty()) ++cancelled;
    }
    return nlohmann::json{{"ok", true}, {"id", id}, {"cancelled", cancelled}}.dump();
}

std::string TaskQueueManager::DirectionRetryJobs(const std::string& id, bool resetAttempts, bool refreshInstructions,
                                                 TaskQueueCaller caller, std::string& outError) {
    outError.clear();
    nlohmann::json record;
    if (!directionStore_.Get(id, record)) {
        outError = "direction not found: " + id;
        return BuildError(outError);
    }
    std::vector<std::string> jobIds;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [name, tq] : queues_) {
            (void)name;
            for (const auto& job : tq.jobs) {
                if (job.directionId != id) continue;
                if (job.state == "failed" || job.state == "blocked" ||
                    job.state == "cancelled" || job.state == "closed") {
                    jobIds.push_back(job.id);
                }
            }
        }
    }
    int retried = 0;
    for (const auto& jid : jobIds) {
        std::string err;
        TaskQueueRetry(jid, resetAttempts, refreshInstructions, caller, err);
        if (err.empty()) ++retried;
    }
    return nlohmann::json{{"ok", true}, {"id", id}, {"retried", retried}}.dump();
}

std::string TaskQueueManager::DirectionPauseJobs(const std::string& id, bool paused, std::string& outError) {
    outError.clear();
    nlohmann::json record;
    if (!directionStore_.Get(id, record)) {
        outError = "direction not found: " + id;
        return BuildError(outError);
    }
    std::vector<std::string> jobIds;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [name, tq] : queues_) {
            (void)name;
            for (const auto& job : tq.jobs) {
                if (job.directionId != id) continue;
                if (job.state == "succeeded" || job.state == "failed" ||
                    job.state == "cancelled" || job.state == "closed" || job.state == "takenOver") {
                    continue;
                }
                jobIds.push_back(job.id);
            }
        }
    }
    int updated = 0;
    for (const auto& jid : jobIds) {
        std::string err;
        TaskQueuePauseJob(jid, paused, err);
        if (err.empty()) ++updated;
    }
    return nlohmann::json{{"ok", true}, {"id", id}, {"paused", paused}, {"updated", updated}}.dump();
}

std::string TaskQueueManager::DirectionRemoveJobs(const std::string& id, bool force, std::string& outError) {
    outError.clear();
    nlohmann::json record;
    if (!directionStore_.Get(id, record)) {
        outError = "direction not found: " + id;
        return BuildError(outError);
    }
    std::vector<std::string> jobIds;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [name, tq] : queues_) {
            (void)name;
            for (const auto& job : tq.jobs) {
                if (job.directionId == id) jobIds.push_back(job.id);
            }
        }
    }
    int removed = 0;
    for (const auto& jid : jobIds) {
        std::string err;
        TaskQueueRemove(jid, force, err);
        if (err.empty()) {
            ++removed;
        } else if (outError.empty()) {
            outError = err;
        }
    }
    if (removed == 0 && !jobIds.empty() && !outError.empty()) {
        return BuildError(outError);
    }
    outError.clear();
    return nlohmann::json{{"ok", true}, {"id", id}, {"removed", removed}}.dump();
}

namespace {

std::vector<std::string> DedupeAfterList(const std::vector<std::string>& raw) {
    std::vector<std::string> out;
    std::unordered_set<std::string> seen;
    for (const auto& s : raw) {
        if (s.empty()) continue;
        if (seen.insert(s).second) out.push_back(s);
    }
    return out;
}

constexpr int kDefaultMaxCardRescueDepth = 2;

bool FindExactCardInTask(const DirectionStore& store, const std::string& taskId,
                         const std::string& number, nlohmann::json& outRecord) {
    bool foundSuperseded = false;
    nlohmann::json supersededRec;
    for (const auto& rec : store.All()) {
        if (rec.value("taskId", std::string()) != taskId) continue;
        if (!rec.contains("card") || !rec["card"].is_object()) continue;
        if (rec["card"].value("number", std::string()) != number) continue;
        if (rec.value("supersededBy", std::string()).empty()) {
            outRecord = rec;
            return true;
        }
        if (!foundSuperseded) {
            supersededRec = rec;
            foundSuperseded = true;
        }
    }
    if (foundSuperseded) {
        outRecord = std::move(supersededRec);
        return true;
    }
    return false;
}

bool ResolveCardLatestInTask(const DirectionStore& store, const std::string& taskId,
                             const std::string& number, nlohmann::json& outRecord) {
    nlohmann::json rec;
    if (!FindExactCardInTask(store, taskId, number, rec)) return false;
    outRecord = FollowCardHeadLocked(store, rec);
    return true;
}

bool HasCardDepCycle(const DirectionStore& store, const std::string& taskId,
                     const std::string& targetNumber, const std::vector<std::string>& initialAfter) {
    std::unordered_map<std::string, std::string> numberToLatestNumber;
    const auto allRecs = store.All();
    for (const auto& rec : allRecs) {
        if (rec.value("taskId", std::string()) != taskId) continue;
        if (!rec.contains("card") || !rec["card"].is_object()) continue;
        const std::string num = rec["card"].value("number", std::string());
        if (num.empty()) continue;
        const nlohmann::json head = FollowCardHeadLocked(store, rec);
        const std::string headNum =
            (head.contains("card") && head["card"].is_object())
                ? head["card"].value("number", num)
                : num;
        numberToLatestNumber[num] = headNum;
    }
    auto resolveNum = [&](const std::string& n) -> std::string {
        auto it = numberToLatestNumber.find(n);
        return it != numberToLatestNumber.end() ? it->second : n;
    };

    std::unordered_map<std::string, std::vector<std::string>> adj;
    for (const auto& rec : allRecs) {
        if (rec.value("taskId", std::string()) != taskId) continue;
        if (!rec.contains("card") || !rec["card"].is_object()) continue;
        if (!rec.value("supersededBy", std::string()).empty()) continue;
        const std::string num = rec["card"].value("number", std::string());
        std::vector<std::string> deps;
        if (rec.contains("after") && rec["after"].is_array()) {
            for (const auto& d : rec["after"]) {
                if (d.is_string()) deps.push_back(resolveNum(d.get<std::string>()));
            }
        }
        adj[num] = std::move(deps);
    }
    const std::string resolvedTarget = resolveNum(targetNumber);
    std::vector<std::string> resolvedInitial;
    resolvedInitial.reserve(initialAfter.size());
    for (const auto& d : initialAfter) resolvedInitial.push_back(resolveNum(d));
    adj[resolvedTarget] = resolvedInitial;

    std::unordered_set<std::string> visited;
    std::vector<std::string> stack = resolvedInitial;
    while (!stack.empty()) {
        const std::string cur = stack.back();
        stack.pop_back();
        if (cur == resolvedTarget) return true;
        if (!visited.insert(cur).second) continue;
        auto it = adj.find(cur);
        if (it == adj.end()) continue;
        for (const auto& nxt : it->second) {
            if (nxt == resolvedTarget) return true;
            if (visited.count(nxt) == 0) stack.push_back(nxt);
        }
    }
    return false;
}

} // namespace

std::string TaskQueueManager::CardAdd(const std::string& taskId, const std::string& number,
                                      const std::string& title, const std::string& body,
                                      const std::vector<std::string>& after, const std::string& tmpl,
                                      const std::string& paramsJson, const std::string& originJson,
                                      std::string& outError) {
    outError.clear();
    if (!ValidateTaskActive(taskId, outError)) return BuildError(outError);
    if (!DirectionStore::ValidateCardNumber(number, outError)) return BuildError(outError);
    if (title.empty()) {
        outError = "title is required";
        return BuildError(outError);
    }
    if (!DirectionStore::ValidateSize("title", title, DirectionStore::kTitleMaxBytes, outError)) {
        return BuildError(outError);
    }
    if (!DirectionStore::ValidateSize("body", body, DirectionStore::kCardBodyMaxBytes, outError)) {
        return BuildError(outError);
    }
    const std::string effTmpl = tmpl.empty() ? std::string("card") : tmpl;
    if (!DirectionStore::ValidateSize("template", effTmpl, DirectionStore::kCardTemplateMaxBytes, outError)) {
        return BuildError(outError);
    }
    nlohmann::json paramsObj = nlohmann::json::object();
    if (!paramsJson.empty()) {
        paramsObj = nlohmann::json::parse(paramsJson, nullptr, false);
        if (paramsObj.is_discarded() || !paramsObj.is_object()) {
            outError = "params must be a JSON object";
            return BuildError(outError);
        }
    }
    const std::vector<std::string> dedupAfter = DedupeAfterList(after);
    const TaskQueueOrigin origin = ParseOriginJson(originJson);
    nlohmann::json originRecord = nlohmann::json::parse(OriginToJson(origin), nullptr, false);
    if (!originRecord.is_object()) originRecord = nlohmann::json::object();

    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json existing;
    if (FindExactCardInTask(directionStore_, taskId, number, existing)) {
        outError = "card '" + number + "' already exists in task '" + taskId + "'";
        return BuildError(outError);
    }
    for (const auto& dep : dedupAfter) {
        if (dep == number) {
            outError = "card '" + number + "' cannot depend on itself";
            return BuildError(outError);
        }
        nlohmann::json depRec;
        if (!ResolveCardLatestInTask(directionStore_, taskId, dep, depRec)) {
            outError = "unknown dependency card '" + dep + "' in task '" + taskId + "'";
            return BuildError(outError);
        }
    }
    if (HasCardDepCycle(directionStore_, taskId, number, dedupAfter)) {
        outError = "dependency cycle detected involving card '" + number + "'";
        return BuildError(outError);
    }

    const std::string newId = directionStore_.NextId();
    const std::string now = GetCurrentTimestampStr();
    nlohmann::json afterArr = nlohmann::json::array();
    for (const auto& dep : dedupAfter) afterArr.push_back(dep);

    nlohmann::json record = {
        {"id", newId},
        {"kind", "card"},
        {"taskId", taskId},
        {"title", title},
        {"createdAt", now},
        {"updatedAt", now},
        {"origin", originRecord},
        {"closed", nullptr},
        {"removedJobs", nlohmann::json::array()},
        {"after", afterArr},
        {"card", {
            {"number", number},
            {"title", title},
            {"body", body},
            {"template", effTmpl},
            {"params", paramsObj}
        }},
        {"rev", BumpRevLocked()}
    };
    if (!directionStore_.Create(record, outError)) return BuildError(outError);
    nlohmann::json extra = {{"directionId", newId}, {"cardNumber", number}, {"title", title}};
    TaskLog(taskId, "card_added", "", "", number + ": " + title, origin.session, &extra);
    return nlohmann::json{
        {"ok", true},
        {"id", newId},
        {"directionId", newId},
        {"taskId", taskId},
        {"number", number},
        {"title", title},
        {"state", "planned"}
    }.dump();
}

std::string TaskQueueManager::CardList(const std::string& taskId, std::string& outError) {
    outError.clear();
    nlohmann::json taskRec;
    if (taskId.empty() || !taskStore_.Get(taskId, taskRec)) {
        outError = "task not found: " + taskId;
        return BuildError(outError);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<nlohmann::json> cardRecs;
    std::unordered_map<std::string, std::string> numberToDirId;
    for (const auto& rec : directionStore_.All()) {
        if (rec.value("taskId", std::string()) != taskId) continue;
        if (!rec.contains("card") || !rec["card"].is_object()) continue;
        const std::string num = rec["card"].value("number", std::string());
        const nlohmann::json head = FollowCardHeadLocked(directionStore_, rec);
        const std::string headId = head.value("id", std::string());
        if (!num.empty() && !headId.empty()) numberToDirId[num] = headId;
        if (!rec.value("supersededBy", std::string()).empty()) continue;
        cardRecs.push_back(rec);
    }
    std::sort(cardRecs.begin(), cardRecs.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return CompareCardNumberNatural(
            a["card"].value("number", std::string()),
            b["card"].value("number", std::string()));
    });

    nlohmann::json cards = nlohmann::json::array();
    for (const auto& rec : cardRecs) {
        const std::string dirId = rec.value("id", std::string());
        const auto& card = rec["card"];
        const std::string num = card.value("number", std::string());
        const nlohmann::json status = DirectionStateLocked(dirId);
        const std::string state = status.value("state", std::string("planned"));

        nlohmann::json afterArr = nlohmann::json::array();
        nlohmann::json waitingFor = nlohmann::json::array();
        if (rec.contains("after") && rec["after"].is_array()) {
            afterArr = rec["after"];
            for (const auto& dep : afterArr) {
                if (!dep.is_string()) continue;
                const std::string depNum = dep.get<std::string>();
                auto it = numberToDirId.find(depNum);
                if (it == numberToDirId.end()) {
                    waitingFor.push_back(depNum);
                    continue;
                }
                const nlohmann::json depStatus = DirectionStateLocked(it->second);
                if (!IsDirectionSatisfiedForCardDep(depStatus)) {
                    waitingFor.push_back(depNum);
                }
            }
        }

        // Omit card.body from list payloads to keep responses small.
        nlohmann::json item = {
            {"id", dirId},
            {"directionId", dirId},
            {"kind", rec.value("kind", std::string("card"))},
            {"taskId", taskId},
            {"number", num},
            {"title", card.value("title", std::string())},
            {"template", card.value("template", std::string("card"))},
            {"after", afterArr},
            {"state", state},
            {"waitingFor", waitingFor},
            {"ready", state == "planned" && waitingFor.empty()},
            {"createdAt", rec.value("createdAt", std::string())},
            {"updatedAt", rec.value("updatedAt", std::string())}
        };
        if (status.contains("result")) item["result"] = status["result"];
        const std::string btId = rec.value("btId", std::string());
        if (!btId.empty()) item["btId"] = btId;
        const std::string supersedes = rec.value("supersedes", std::string());
        if (!supersedes.empty()) item["supersedes"] = supersedes;
        const std::string supersedeReason = rec.value("supersedeReason", std::string());
        if (!supersedeReason.empty()) item["supersedeReason"] = supersedeReason;
        cards.push_back(std::move(item));
    }
    return nlohmann::json{{"ok", true}, {"taskId", taskId}, {"cards", cards}}.dump();
}

std::string TaskQueueManager::CardShow(const std::string& idOrNumber, const std::string& taskId,
                                       std::string& outError) {
    outError.clear();
    if (idOrNumber.empty()) {
        outError = "card number or direction id is required";
        return BuildError(outError);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json record;
    nlohmann::json directRec;
    if (directionStore_.Get(idOrNumber, directRec)) {
        if (!taskId.empty() && directRec.value("taskId", std::string()) != taskId) {
            outError = "card not found: " + idOrNumber;
            return BuildError(outError);
        }
        record = std::move(directRec);
    } else {
        std::vector<nlohmann::json> matches;
        std::vector<nlohmann::json> supersededMatches;
        for (const auto& rec : directionStore_.All()) {
            if (!taskId.empty() && rec.value("taskId", std::string()) != taskId) continue;
            if (!rec.contains("card") || !rec["card"].is_object()) continue;
            if (rec["card"].value("number", std::string()) != idOrNumber) continue;
            if (!rec.value("supersededBy", std::string()).empty()) {
                supersededMatches.push_back(rec);
                continue;
            }
            matches.push_back(rec);
        }
        if (matches.empty()) matches = std::move(supersededMatches);
        if (matches.empty()) {
            outError = "card not found: " + idOrNumber;
            return BuildError(outError);
        }
        if (matches.size() > 1) {
            outError = "multiple tasks have card '" + idOrNumber + "'; specify --task <id>";
            return BuildError(outError);
        }
        record = std::move(matches[0]);
    }

    const std::string dirId = record.value("id", std::string());
    const std::string recTaskId = record.value("taskId", std::string());
    const nlohmann::json status = DirectionStateLocked(dirId);

    nlohmann::json waitingFor = nlohmann::json::array();
    if (record.contains("after") && record["after"].is_array() && !recTaskId.empty()) {
        for (const auto& dep : record["after"]) {
            if (!dep.is_string()) continue;
            const std::string depNum = dep.get<std::string>();
            nlohmann::json depRec;
            if (!ResolveCardLatestInTask(directionStore_, recTaskId, depNum, depRec)) {
                waitingFor.push_back(depNum);
                continue;
            }
            const nlohmann::json depStatus = DirectionStateLocked(depRec.value("id", std::string()));
            if (!IsDirectionSatisfiedForCardDep(depStatus)) {
                waitingFor.push_back(depNum);
            }
        }
    }

    nlohmann::json jobs = nlohmann::json::array();
    for (const auto& [name, tq] : queues_) {
        (void)name;
        for (const auto& job : tq.jobs) {
            if (job.directionId != dirId) continue;
            jobs.push_back({{"id", job.id}, {"taskqueue", job.taskqueue}, {"name", job.name},
                            {"summary", job.summary}, {"state", job.state},
                            {"supersedes", job.supersedes}, {"supersededBy", job.supersededBy}});
        }
    }

    nlohmann::json out = {
        {"ok", true},
        {"direction", record},
        {"after", record.value("after", nlohmann::json::array())},
        {"status", status},
        {"waitingFor", waitingFor},
        {"jobs", jobs}
    };
    if (record.contains("card") && record["card"].is_object()) {
        out["card"] = record["card"];
    }
    return out.dump();
}

std::string TaskQueueManager::CardEdit(const std::string& taskId, const std::string& number,
                                       bool hasTitle, const std::string& title,
                                       bool hasBody, const std::string& body,
                                       bool hasAfter, const std::vector<std::string>& after,
                                       bool hasTemplate, const std::string& tmpl,
                                       bool hasParams, const std::string& paramsJson,
                                       const std::string& session, std::string& outError) {
    outError.clear();
    if (!ValidateTaskActive(taskId, outError)) return BuildError(outError);
    if (number.empty()) {
        outError = "card number is required";
        return BuildError(outError);
    }
    if (hasTitle) {
        if (title.empty()) {
            outError = "title cannot be empty";
            return BuildError(outError);
        }
        if (!DirectionStore::ValidateSize("title", title, DirectionStore::kTitleMaxBytes, outError)) {
            return BuildError(outError);
        }
    }
    if (hasBody && !DirectionStore::ValidateSize("body", body, DirectionStore::kCardBodyMaxBytes, outError)) {
        return BuildError(outError);
    }
    if (hasTemplate) {
        if (tmpl.empty()) {
            outError = "template cannot be empty";
            return BuildError(outError);
        }
        if (!DirectionStore::ValidateSize("template", tmpl, DirectionStore::kCardTemplateMaxBytes, outError)) {
            return BuildError(outError);
        }
    }
    nlohmann::json paramsObj;
    if (hasParams) {
        paramsObj = paramsJson.empty() ? nlohmann::json::object() : nlohmann::json::parse(paramsJson, nullptr, false);
        if (paramsObj.is_discarded() || !paramsObj.is_object()) {
            outError = "params must be a JSON object";
            return BuildError(outError);
        }
    }

    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json record;
    if (!FindExactCardInTask(directionStore_, taskId, number, record)) {
        outError = "card '" + number + "' not found in task '" + taskId + "'";
        return BuildError(outError);
    }
    const std::string dirId = record.value("id", std::string());
    const std::string state = DirectionStateLocked(dirId).value("state", std::string());
    if (record.value("kind", std::string()) != "card" || state != "planned") {
        outError = "card '" + number + "' can only be edited while planned (current state: " + state + ")";
        return BuildError(outError);
    }

    if (hasAfter) {
        const std::vector<std::string> dedupAfter = DedupeAfterList(after);
        for (const auto& dep : dedupAfter) {
            if (dep == number) {
                outError = "card '" + number + "' cannot depend on itself";
                return BuildError(outError);
            }
            nlohmann::json depRec;
            if (!ResolveCardLatestInTask(directionStore_, taskId, dep, depRec)) {
                outError = "unknown dependency card '" + dep + "' in task '" + taskId + "'";
                return BuildError(outError);
            }
        }
        if (HasCardDepCycle(directionStore_, taskId, number, dedupAfter)) {
            outError = "dependency cycle detected involving card '" + number + "'";
            return BuildError(outError);
        }
        nlohmann::json afterArr = nlohmann::json::array();
        for (const auto& dep : dedupAfter) afterArr.push_back(dep);
        record["after"] = afterArr;
    }

    nlohmann::json card = record["card"];
    if (hasTitle) {
        card["title"] = title;
        record["title"] = title;
    }
    if (hasBody) card["body"] = body;
    if (hasTemplate) card["template"] = tmpl;
    if (hasParams) card["params"] = paramsObj;
    record["card"] = card;
    record["updatedAt"] = GetCurrentTimestampStr();
    record["rev"] = BumpRevLocked();

    if (!directionStore_.Save(record, outError)) return BuildError(outError);
    const std::string newTitle = record.value("title", std::string());
    nlohmann::json extra = {{"directionId", dirId}, {"cardNumber", number}, {"title", newTitle}};
    TaskLog(taskId, "card_edited", "", "", number + ": " + newTitle, session, &extra);
    return nlohmann::json{
        {"ok", true},
        {"id", dirId},
        {"directionId", dirId},
        {"taskId", taskId},
        {"number", number},
        {"title", newTitle},
        {"state", "planned"}
    }.dump();
}

std::string TaskQueueManager::CardRemove(const std::string& taskId, const std::string& number,
                                         const std::string& session, std::string& outError) {
    outError.clear();
    if (!ValidateTaskActive(taskId, outError)) return BuildError(outError);
    if (number.empty()) {
        outError = "card number is required";
        return BuildError(outError);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json record;
    if (!FindExactCardInTask(directionStore_, taskId, number, record)) {
        outError = "card '" + number + "' not found in task '" + taskId + "'";
        return BuildError(outError);
    }
    const std::string dirId = record.value("id", std::string());
    const std::string state = DirectionStateLocked(dirId).value("state", std::string());
    if (record.value("kind", std::string()) != "card" || state != "planned") {
        outError = "card '" + number + "' can only be removed while planned (current state: " + state + ")";
        return BuildError(outError);
    }
    const std::string title = record.value("title", std::string());
    if (!directionStore_.Remove(dirId, outError)) return BuildError(outError);
    BumpRevLocked();
    nlohmann::json extra = {{"directionId", dirId}, {"cardNumber", number}, {"title", title}};
    TaskLog(taskId, "card_removed", "", "", number + ": " + title, session, &extra);
    return nlohmann::json{
        {"ok", true},
        {"id", dirId},
        {"directionId", dirId},
        {"taskId", taskId},
        {"number", number}
    }.dump();
}

std::string TaskQueueManager::CardRedo(const std::string& taskId, const std::string& idOrNumber,
                                       bool hasBody, const std::string& body,
                                       bool hasParams, const std::string& paramsJson,
                                       const std::string& reason, bool force,
                                       const std::string& originJson, std::string& outError) {
    outError.clear();
    if (!ValidateTaskActive(taskId, outError)) return BuildError(outError);
    if (idOrNumber.empty()) {
        outError = "card number or direction id is required";
        return BuildError(outError);
    }
    if (!DirectionStore::ValidateSize("reason", reason, DirectionStore::kReasonMaxBytes, outError)) {
        return BuildError(outError);
    }
    if (hasBody && !DirectionStore::ValidateSize("body", body, DirectionStore::kCardBodyMaxBytes, outError)) {
        return BuildError(outError);
    }
    nlohmann::json paramsObj;
    if (hasParams) {
        paramsObj = paramsJson.empty() ? nlohmann::json::object() : nlohmann::json::parse(paramsJson, nullptr, false);
        if (paramsObj.is_discarded() || !paramsObj.is_object()) {
            outError = "params must be a JSON object";
            return BuildError(outError);
        }
    }

    const TaskQueueOrigin origin = ParseOriginJson(originJson);
    nlohmann::json originRecord = nlohmann::json::parse(OriginToJson(origin), nullptr, false);
    if (!originRecord.is_object()) originRecord = nlohmann::json::object();

    std::lock_guard<std::mutex> lock(mutex_);
    nlohmann::json record;
    nlohmann::json directRec;
    if (directionStore_.Get(idOrNumber, directRec)) {
        if (directRec.value("taskId", std::string()) != taskId ||
            !directRec.contains("card") || !directRec["card"].is_object()) {
            outError = "card '" + idOrNumber + "' not found in task '" + taskId + "'";
            return BuildError(outError);
        }
        record = std::move(directRec);
    } else if (!FindExactCardInTask(directionStore_, taskId, idOrNumber, record)) {
        outError = "card '" + idOrNumber + "' not found in task '" + taskId + "'";
        return BuildError(outError);
    }

    nlohmann::json head = FollowCardHeadLocked(directionStore_, record);
    const std::string headId = head.value("id", std::string());
    const nlohmann::json headCard = head.value("card", nlohmann::json::object());
    const std::string headNumber = headCard.value("number", std::string());
    const std::string headState = DirectionStateLocked(headId).value("state", std::string());
    if (headState != "failed" && headState != "closed" && headState != "cancelled") {
        outError = "card '" + headNumber + "' can only be redone when failed, cancelled, or closed (current state: " +
                   headState + ")";
        return BuildError(outError);
    }

    int depth = 0;
    nlohmann::json root = head;
    std::unordered_set<std::string> seenBack;
    seenBack.insert(headId);
    for (int step = 0; step < 1000; ++step) {
        const std::string prevId = root.value("supersedes", std::string());
        if (prevId.empty() || !seenBack.insert(prevId).second) break;
        nlohmann::json prevRec;
        if (!directionStore_.Get(prevId, prevRec) || !prevRec.is_object()) break;
        root = std::move(prevRec);
        ++depth;
    }

    if (!force && depth >= kDefaultMaxCardRescueDepth) {
        outError = "rescue chain limit (" + std::to_string(kDefaultMaxCardRescueDepth) +
                   ") exceeded for card '" + headNumber + "'; pass --force to override";
        return BuildError(outError);
    }

    std::unordered_set<std::string> usedNumbers;
    for (const auto& rec : directionStore_.All()) {
        if (rec.value("taskId", std::string()) != taskId) continue;
        if (!rec.contains("card") || !rec["card"].is_object()) continue;
        const std::string num = rec["card"].value("number", std::string());
        if (!num.empty()) usedNumbers.insert(num);
    }

    const std::string baseNumber =
        (root.contains("card") && root["card"].is_object())
            ? root["card"].value("number", headNumber)
            : headNumber;
    int suffix = depth + 1;
    if (suffix < 1) suffix = 1;
    std::string newNumber = baseNumber + "-r" + std::to_string(suffix);
    while (usedNumbers.count(newNumber) > 0) {
        ++suffix;
        newNumber = baseNumber + "-r" + std::to_string(suffix);
    }
    if (!DirectionStore::ValidateCardNumber(newNumber, outError)) {
        return BuildError(outError);
    }

    const std::string newId = directionStore_.NextId();
    const std::string now = GetCurrentTimestampStr();
    const std::string title = headCard.value("title", head.value("title", std::string()));
    const std::string effBody = hasBody ? body : headCard.value("body", std::string());
    const std::string effTmpl = headCard.value("template", std::string("card"));
    const nlohmann::json effParams =
        hasParams ? paramsObj : headCard.value("params", nlohmann::json::object());

    nlohmann::json newRecord = {
        {"id", newId},
        {"kind", "card"},
        {"taskId", taskId},
        {"title", title},
        {"createdAt", now},
        {"updatedAt", now},
        {"origin", originRecord},
        {"closed", nullptr},
        {"removedJobs", nlohmann::json::array()},
        {"after", head.value("after", nlohmann::json::array())},
        {"supersedes", headId},
        {"card", {
            {"number", newNumber},
            {"title", title},
            {"body", effBody},
            {"template", effTmpl},
            {"params", effParams}
        }}
    };
    if (!reason.empty()) newRecord["supersedeReason"] = reason;
    newRecord["rev"] = BumpRevLocked();

    if (!directionStore_.Create(newRecord, outError)) return BuildError(outError);
    head["supersededBy"] = newId;
    head["updatedAt"] = now;
    head["rev"] = BumpRevLocked();
    std::string saveErr;
    if (!directionStore_.Save(head, saveErr)) {
        std::string ignoreErr;
        directionStore_.Remove(newId, ignoreErr);
        outError = saveErr;
        return BuildError(outError);
    }

    nlohmann::json extra = {
        {"directionId", newId},
        {"cardNumber", newNumber},
        {"supersedes", headId},
        {"supersedesNumber", headNumber},
        {"title", title}
    };
    if (!reason.empty()) extra["reason"] = reason;
    const std::string logText = newNumber + " supersedes " + headNumber +
                                (reason.empty() ? std::string() : (": " + reason));
    TaskLog(taskId, "card_redone", "", "", logText, origin.session, &extra);

    nlohmann::json out = {
        {"ok", true},
        {"id", newId},
        {"directionId", newId},
        {"taskId", taskId},
        {"number", newNumber},
        {"title", title},
        {"state", "planned"},
        {"supersedes", headId},
        {"supersedesNumber", headNumber},
        {"rescueDepth", depth + 1}
    };
    if (!reason.empty()) out["reason"] = reason;
    return out.dump();
}

// ---------------------------------------------------------------------------
// Client-exit watch for --cancel-on-exit (card 105-4)
// ---------------------------------------------------------------------------

bool TaskQueueManager::QueryProcessStartTimePortable(uint32_t pid, uint64_t& outStartTime) {
    outStartTime = 0;
    if (pid == 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    FILETIME ct{}, et{}, kt{}, ut{};
    bool ok = GetProcessTimes(h, &ct, &et, &kt, &ut) != FALSE;
    CloseHandle(h);
    if (ok) {
        outStartTime = (static_cast<uint64_t>(ct.dwHighDateTime) << 32) | ct.dwLowDateTime;
    }
    return true;
#else
    if (kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) return false;
#ifdef __linux__
    std::ifstream in("/proc/" + std::to_string(pid) + "/stat");
    std::string line;
    if (std::getline(in, line)) {
        size_t rparen = line.rfind(')');
        if (rparen != std::string::npos && rparen + 2 < line.size()) {
            std::istringstream iss(line.substr(rparen + 2));
            std::string tok;
            // Field 3 is state; field 22 is starttime (index 19 after ')').
            for (int idx = 3; idx <= 22 && (iss >> tok); ++idx) {
                if (idx == 22) {
                    try { outStartTime = std::stoull(tok); } catch (...) { outStartTime = 0; }
                }
            }
        }
    }
#endif
    return true;
#endif
}

bool TaskQueueManager::IsWatchedProcessAlive(uint32_t pid, uint64_t expectedStartTime) {
    if (pid == 0) return false;
#ifdef _WIN32
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!h) return false;
    if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
        CloseHandle(h);
        return false;
    }
    if (expectedStartTime != 0) {
        FILETIME ct{}, et{}, kt{}, ut{};
        if (GetProcessTimes(h, &ct, &et, &kt, &ut)) {
            uint64_t actual = (static_cast<uint64_t>(ct.dwHighDateTime) << 32) | ct.dwLowDateTime;
            if (actual != expectedStartTime) {
                CloseHandle(h);
                return false;
            }
        }
    }
    CloseHandle(h);
    return true;
#else
    if (kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) return false;
    if (expectedStartTime != 0) {
        uint64_t actual = 0;
        if (!QueryProcessStartTimePortable(pid, actual)) return false;
        if (actual != 0 && actual != expectedStartTime) return false;
    }
    return true;
#endif
}

void TaskQueueManager::WakeClientWatchLocked() {
#ifdef _WIN32
    if (clientWatchWakeEvent_) {
        SetEvent(static_cast<HANDLE>(clientWatchWakeEvent_));
    }
#endif
}

void TaskQueueManager::UnwatchDirectionClientLocked(const std::string& directionId) {
    if (directionId.empty()) return;
    bool removed = false;
    for (auto it = clientWatches_.begin(); it != clientWatches_.end(); ) {
        if (it->directionId == directionId) {
#ifdef _WIN32
            if (it->handle) { CloseHandle(static_cast<HANDLE>(it->handle)); it->handle = nullptr; }
#else
            if (it->pidfd >= 0) { close(it->pidfd); it->pidfd = -1; }
#endif
            it = clientWatches_.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    if (removed) WakeClientWatchLocked();

    nlohmann::json dirRec;
    if (directionStore_.Get(directionId, dirRec) && dirRec.contains("clientWatch")) {
        dirRec.erase("clientWatch");
        dirRec["updatedAt"] = GetCurrentTimestampStr();
        dirRec["rev"] = BumpRevLocked();
        std::string err;
        directionStore_.Save(dirRec, err);
    }
}

void TaskQueueManager::UnwatchDirectionClient(const std::string& directionId) {
    if (directionId.empty()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    UnwatchDirectionClientLocked(directionId);
}

bool TaskQueueManager::WatchDirectionClient(const std::string& directionId, uint32_t clientPid,
                                            std::string& outError) {
    outError.clear();
    if (directionId.empty() || clientPid == 0) return true;

    uint64_t startTime = 0;
#ifdef _WIN32
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                           static_cast<DWORD>(clientPid));
    if (!h) {
        CancelDirectionOnClientExit(directionId);
        return true;
    }
    if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0) {
        CloseHandle(h);
        CancelDirectionOnClientExit(directionId);
        return true;
    }
    FILETIME ct{}, et{}, kt{}, ut{};
    if (GetProcessTimes(h, &ct, &et, &kt, &ut)) {
        startTime = (static_cast<uint64_t>(ct.dwHighDateTime) << 32) | ct.dwLowDateTime;
    }
#else
    if (!QueryProcessStartTimePortable(clientPid, startTime)) {
        CancelDirectionOnClientExit(directionId);
        return true;
    }
    int pidfd = -1;
#ifdef __linux__
#ifdef SYS_pidfd_open
    pidfd = static_cast<int>(syscall(SYS_pidfd_open, static_cast<pid_t>(clientPid), 0));
#endif
#endif
#endif

    {
        std::lock_guard<std::mutex> lock(mutex_);
        nlohmann::json dirRec;
        if (!directionStore_.Get(directionId, dirRec)) {
#ifdef _WIN32
            CloseHandle(h);
#else
            if (pidfd >= 0) close(pidfd);
#endif
            outError = "direction not found: " + directionId;
            return false;
        }
        dirRec["clientWatch"] = {{"pid", clientPid}, {"startTime", startTime}};
        dirRec["updatedAt"] = GetCurrentTimestampStr();
        dirRec["rev"] = BumpRevLocked();
        if (!directionStore_.Save(dirRec, outError)) {
#ifdef _WIN32
            CloseHandle(h);
#else
            if (pidfd >= 0) close(pidfd);
#endif
            return false;
        }

        for (auto it = clientWatches_.begin(); it != clientWatches_.end(); ) {
            if (it->directionId == directionId) {
#ifdef _WIN32
                if (it->handle) CloseHandle(static_cast<HANDLE>(it->handle));
#else
                if (it->pidfd >= 0) close(it->pidfd);
#endif
                it = clientWatches_.erase(it);
            } else {
                ++it;
            }
        }
        ClientWatchEntry entry;
        entry.directionId = directionId;
        entry.pid = clientPid;
        entry.startTime = startTime;
#ifdef _WIN32
        entry.handle = h;
        if (!clientWatchWakeEvent_) {
            clientWatchWakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        }
#else
        entry.pidfd = pidfd;
#endif
        clientWatches_.push_back(entry);
        if (!clientWatchRunning_.exchange(true)) {
            if (clientWatchThread_.joinable()) clientWatchThread_.join();
            clientWatchThread_ = std::thread(&TaskQueueManager::ClientWatchLoop, this);
        }
        WakeClientWatchLocked();
    }
    return true;
}

void TaskQueueManager::CancelDirectionOnClientExit(const std::string& directionId) {
    if (directionId.empty()) return;
    std::string kind;
    std::string btId;
    std::string taskId;
    BtCancelCallback btCancel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        nlohmann::json dirRec;
        if (!directionStore_.Get(directionId, dirRec)) {
            UnwatchDirectionClientLocked(directionId);
            return;
        }
        const nlohmann::json st = DirectionStateLocked(directionId);
        const std::string s = st.value("state", std::string());
        if (s == "succeeded" || s == "failed" || s == "cancelled" || s == "closed") {
            UnwatchDirectionClientLocked(directionId);
            return;
        }
        kind = dirRec.value("kind", std::string());
        btId = dirRec.value("btId", std::string());
        taskId = dirRec.value("taskId", std::string());
        btCancel = btCancelCb_;
        UnwatchDirectionClientLocked(directionId);
    }

    if (kind == "bt" && !btId.empty() && btCancel) {
        btCancel(btId);
    }
    std::string err;
    DirectionCancelJobs(directionId, err, "client exited");
    if (!taskId.empty()) {
        nlohmann::json extra = {{"directionId", directionId}, {"reason", "client exited"}};
        TaskLog(taskId, "direction_cancelled", "", btId, "client exited", "", &extra);
    }
    WriteAuditLog("CLIENT_EXIT_CANCEL", "Cancelled direction " + directionId + " (client exited)");
}

void TaskQueueManager::ReconcileDirectionClientWatches() {
    std::vector<nlohmann::json> all = directionStore_.All();
    std::vector<std::string> toCancel;
    std::vector<std::pair<std::string, uint32_t>> toWatch;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& rec : all) {
            if (!rec.contains("clientWatch") || !rec["clientWatch"].is_object()) continue;
            const std::string dirId = rec.value("id", std::string());
            if (dirId.empty()) continue;
            const nlohmann::json st = DirectionStateLocked(dirId);
            const std::string s = st.value("state", std::string());
            if (s == "succeeded" || s == "failed" || s == "cancelled" || s == "closed") {
                UnwatchDirectionClientLocked(dirId);
                continue;
            }
            const uint32_t pid = rec["clientWatch"].value("pid", static_cast<uint32_t>(0));
            const uint64_t expectedStart = rec["clientWatch"].value("startTime", static_cast<uint64_t>(0));
            if (pid == 0 || !IsWatchedProcessAlive(pid, expectedStart)) {
                toCancel.push_back(dirId);
            } else {
                toWatch.push_back({dirId, pid});
            }
        }
    }
    for (const auto& dirId : toCancel) {
        CancelDirectionOnClientExit(dirId);
    }
    for (const auto& item : toWatch) {
        std::string err;
        WatchDirectionClient(item.first, item.second, err);
    }
}

void TaskQueueManager::ClientWatchLoop() {
    while (clientWatchRunning_.load()) {
#ifdef _WIN32
        HANDLE wakeEv = nullptr;
        std::vector<std::pair<std::string, HANDLE>> items;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            wakeEv = static_cast<HANDLE>(clientWatchWakeEvent_);
            for (const auto& w : clientWatches_) {
                if (w.handle) items.push_back({w.directionId, static_cast<HANDLE>(w.handle)});
            }
        }
        if (items.empty()) {
            if (wakeEv) WaitForSingleObject(wakeEv, INFINITE);
            else std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }
        const size_t maxBatch = MAXIMUM_WAIT_OBJECTS - 1;
        const DWORD timeoutMs = (items.size() <= maxBatch) ? INFINITE : 200;
        for (size_t offset = 0; offset < items.size() && clientWatchRunning_.load(); offset += maxBatch) {
            size_t count = std::min(maxBatch, items.size() - offset);
            std::vector<HANDLE> handles;
            handles.reserve(count + 1);
            if (wakeEv) handles.push_back(wakeEv);
            for (size_t i = 0; i < count; ++i) handles.push_back(items[offset + i].second);
            DWORD res = WaitForMultipleObjects(static_cast<DWORD>(handles.size()),
                                               handles.data(), FALSE, timeoutMs);
            if (!clientWatchRunning_.load()) break;
            const size_t base = wakeEv ? 1 : 0;
            if (wakeEv && res == WAIT_OBJECT_0) {
                break; // Watch list changed; re-snapshot.
            }
            if (res >= WAIT_OBJECT_0 + base && res < WAIT_OBJECT_0 + handles.size()) {
                size_t idx = offset + static_cast<size_t>(res - WAIT_OBJECT_0 - base);
                CancelDirectionOnClientExit(items[idx].first);
                break;
            }
        }
#else
        std::vector<ClientWatchEntry> items;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            items = clientWatches_;
        }
        if (items.empty()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            continue;
        }
        std::vector<struct pollfd> pfds;
        std::vector<size_t> pfdToItem;
        bool hasPollFallback = false;
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].pidfd >= 0) {
                struct pollfd p{};
                p.fd = items[i].pidfd;
                p.events = POLLIN;
                pfds.push_back(p);
                pfdToItem.push_back(i);
            } else {
                hasPollFallback = true;
            }
        }
        if (!pfds.empty()) {
            int timeoutMs = hasPollFallback ? 1000 : 500;
            int r = poll(pfds.data(), static_cast<nfds_t>(pfds.size()), timeoutMs);
            if (r > 0) {
                for (size_t j = 0; j < pfds.size(); ++j) {
                    if (pfds[j].revents & (POLLIN | POLLHUP | POLLERR)) {
                        CancelDirectionOnClientExit(items[pfdToItem[j]].directionId);
                    }
                }
            }
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1000));
        }
        if (hasPollFallback && clientWatchRunning_.load()) {
            for (const auto& item : items) {
                if (item.pidfd >= 0) continue;
                if (!IsWatchedProcessAlive(item.pid, item.startTime)) {
                    CancelDirectionOnClientExit(item.directionId);
                }
            }
        }
#endif
    }
}

} // namespace pc
