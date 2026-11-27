#include "common.h"
#include "parse_util.h"
#include "nlohmann/json.hpp"
#include <tlhelp32.h>
#include <iterator>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <ctime>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <set>

namespace pc {

std::wstring Utf8ToWide(const std::string& utf8) {
    if (utf8.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (len <= 0) return L"";
    std::wstring result(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), result.data(), len);
    return result;
}

std::string WideToUtf8(const std::wstring& wide) {
    if (wide.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return "";
    std::string result(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), result.data(), len, nullptr, nullptr);
    return result;
}

std::wstring GetCurrentDirectoryWide() {
    DWORD len = GetCurrentDirectoryW(0, nullptr);
    if (len == 0) return L"";
    std::wstring buf(len, L'\0');
    DWORD n = GetCurrentDirectoryW(len, buf.data());
    if (n == 0) return L"";
    buf.resize(n);
    return buf;
}

std::wstring ResolveAbsolutePathWide(const std::wstring& path) {
    if (path.empty()) return GetCurrentDirectoryWide();
    DWORD len = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (len == 0) return path;
    std::wstring out(len, L'\0');
    DWORD n = GetFullPathNameW(path.c_str(), len, out.data(), nullptr);
    if (n == 0) return path;
    out.resize(n);
    return out;
}

std::string ResolveAbsolutePathUtf8(const std::string& path) {
    return WideToUtf8(ResolveAbsolutePathWide(Utf8ToWide(path)));
}

std::string ExecModeToString(ExecMode mode) {
    switch (mode) {
        case ExecMode::Pipe:    return "pipe";
        case ExecMode::ConPTY:  return "pty";
        case ExecMode::Console: return "console";
    }
    return "pipe";
}

ExecMode StringToExecMode(const std::string& s) {
    std::string lower = s;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (lower == "pty" || lower == "conpty" || lower == "interactive") return ExecMode::ConPTY;
    if (lower == "console" || lower == "window") return ExecMode::Console;
    return ExecMode::Pipe;
}

std::string ProcStateToString(ProcState state) {
    switch (state) {
        case ProcState::Running:         return "running";
        case ProcState::Completed:       return "completed";
        case ProcState::Failed:          return "failed";
        case ProcState::TimedOut:        return "timed_out";
        case ProcState::IdleTimedOut:    return "idle_timed_out";
        case ProcState::StartupTimedOut: return "startup_timed_out";
        case ProcState::Killed:          return "killed";
    }
    return "unknown";
}

ProcState StringToProcState(const std::string& s) {
    if (s == "running") return ProcState::Running;
    if (s == "completed") return ProcState::Completed;
    if (s == "failed") return ProcState::Failed;
    if (s == "timed_out") return ProcState::TimedOut;
    if (s == "idle_timed_out") return ProcState::IdleTimedOut;
    if (s == "startup_timed_out") return ProcState::StartupTimedOut;
    if (s == "killed") return ProcState::Killed;
    return ProcState::Completed;
}

std::wstring ProcStateToDisplayWide(ProcState state, bool isIdleWarning) {
    switch (state) {
        case ProcState::Running:
            return isIdleWarning ? L"IDLE / WAITING" : L"RUNNING";
        case ProcState::Completed:       return L"COMPLETED (OK)";
        case ProcState::Failed:          return L"FAILED";
        case ProcState::TimedOut:        return L"TIMEOUT";
        case ProcState::IdleTimedOut:    return L"HANG TIMEOUT";
        case ProcState::StartupTimedOut: return L"STARTUP TIMEOUT";
        case ProcState::Killed:          return L"KILLED";
    }
    return L"UNKNOWN";
}

std::string StripAnsiCodes(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    size_t i = 0;
    const size_t n = input.size();

    while (i < n) {
        unsigned char c = static_cast<unsigned char>(input[i]);
        if (c == 0x1B) { // ESC
            if (i + 1 < n) {
                char next = input[i + 1];
                if (next == '[') {
                    // CSI sequence: ESC [ ... final byte (0x40 - 0x7E)
                    i += 2;
                    while (i < n) {
                        unsigned char fc = static_cast<unsigned char>(input[i]);
                        if (fc >= 0x40 && fc <= 0x7E) {
                            // If cursor move right/down or line clear in ConPTY, preserve space/newline if needed
                            if (fc == 'H' || fc == 'f') {
                                // Cursor position often acts as line break in TUI redraws if not already at BOL
                                if (!out.empty() && out.back() != '\n') {
                                    out.push_back('\n');
                                }
                            }
                            i++;
                            break;
                        }
                        i++;
                    }
                    continue;
                } else if (next == ']') {
                    // OSC sequence: ESC ] ... (BEL 0x07 or ST ESC \)
                    i += 2;
                    while (i < n) {
                        if (input[i] == '\x07') {
                            i++;
                            break;
                        }
                        if (input[i] == '\x1B' && i + 1 < n && input[i + 1] == '\\') {
                            i += 2;
                            break;
                        }
                        i++;
                    }
                    continue;
                } else {
                    // Two-character ESC sequence
                    i += 2;
                    continue;
                }
            } else {
                i++;
                continue;
            }
        } else if (c == '\b') {
            if (!out.empty() && out.back() != '\n') {
                out.pop_back();
            }
            i++;
            continue;
        } else if (c < 0x20 && c != '\n' && c != '\r' && c != '\t') {
            // Skip other non-printable control characters
            i++;
            continue;
        }
        out.push_back(static_cast<char>(c));
        i++;
    }
    return out;
}

std::wstring NormalizeCrLfWide(const std::string& utf8Clean) {
    std::string norm;
    norm.reserve(utf8Clean.size() + 128);
    for (size_t i = 0; i < utf8Clean.size(); ++i) {
        if (utf8Clean[i] == '\r') {
            if (i + 1 < utf8Clean.size() && utf8Clean[i + 1] == '\n') {
                norm += "\r\n";
                ++i;
            } else {
                norm += "\r\n";
            }
        } else if (utf8Clean[i] == '\n') {
            norm += "\r\n";
        } else {
            norm.push_back(utf8Clean[i]);
        }
    }
    return Utf8ToWide(norm);
}

std::string Trim(const std::string& s) {
    size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) ++start;
    size_t end = s.size();
    while (end > start && std::isspace(static_cast<unsigned char>(s[end - 1]))) --end;
    return s.substr(start, end - start);
}

std::string GetLastNonEmptyLine(const std::string& text, size_t maxLen) {
    if (text.empty()) return "";
    int end = static_cast<int>(text.size()) - 1;
    while (end >= 0 && (text[end] == '\n' || text[end] == '\r' || text[end] == ' ' || text[end] == '\t')) {
        end--;
    }
    if (end < 0) return "";
    int start = end;
    while (start > 0 && text[start - 1] != '\n' && text[start - 1] != '\r') {
        start--;
    }
    std::string line = text.substr(start, end - start + 1);
    // Trim leading whitespace
    size_t first = line.find_first_not_of(" \t");
    if (first == std::string::npos) return "";
    line = line.substr(first);
    if (line.size() > maxLen) {
        line = line.substr(0, maxLen - 3) + "...";
    }
    return line;
}

std::string GetTailLines(const std::string& text, int maxLines) {
    if (maxLines <= 0 || text.empty()) return text;
    int count = 0;
    for (int i = static_cast<int>(text.size()) - 1; i >= 0; --i) {
        if (text[i] == '\n') {
            count++;
            if (count > maxLines) {
                return text.substr(i + 1);
            }
        }
    }
    return text;
}

bool IsNonEnglishText(const std::string& text) {
    // Walk the UTF-8 code points; any kana, kanji, CJK punctuation or hangul counts.
    for (size_t i = 0; i < text.size();) {
        const unsigned char c = static_cast<unsigned char>(text[i]);
        uint32_t cp = c;
        size_t len = 1;
        if (c >= 0xF0 && c < 0xF8) { cp = c & 0x07; len = 4; }
        else if (c >= 0xE0) { cp = c & 0x0F; len = 3; }
        else if (c >= 0xC0) { cp = c & 0x1F; len = 2; }
        if (len > 1) {
            if (i + len > text.size()) break;
            for (size_t k = 1; k < len; ++k) {
                cp = (cp << 6) | (static_cast<unsigned char>(text[i + k]) & 0x3F);
            }
        }
        i += len;
        if ((cp >= 0x3000 && cp <= 0x30FF) ||  // CJK punctuation, hiragana, katakana
            (cp >= 0x31F0 && cp <= 0x31FF) ||  // katakana phonetic extensions
            (cp >= 0x3130 && cp <= 0x318F) ||  // hangul compatibility jamo
            (cp >= 0x3400 && cp <= 0x4DBF) ||  // CJK extension A
            (cp >= 0x4E00 && cp <= 0x9FFF) ||  // CJK unified ideographs (kanji)
            (cp >= 0xAC00 && cp <= 0xD7AF) ||  // hangul syllables
            (cp >= 0xF900 && cp <= 0xFAFF) ||  // CJK compatibility ideographs
            (cp >= 0xFF66 && cp <= 0xFF9D)) {  // halfwidth katakana
            return true;
        }
    }
    return false;
}

std::string AppendNonEnglishWarnings(const std::string& responseJson, const std::string& argsJson) {
    const nlohmann::json args = nlohmann::json::parse(argsJson, nullptr, false);
    if (!args.is_object()) return responseJson;

    nlohmann::json warnings = nlohmann::json::array();
    auto check = [&warnings](const nlohmann::json& obj) {
        if (!obj.is_object()) return;
        for (const char* field : {"summary", "title"}) {
            auto it = obj.find(field);
            if (it != obj.end() && it->is_string() && IsNonEnglishText(it->get<std::string>())) {
                warnings.push_back(std::string(field) + " is not English: " + it->get<std::string>());
            }
        }
    };
    check(args);
    auto jobs = args.find("jobs");
    if (jobs != args.end() && jobs->is_array()) {
        for (const auto& job : *jobs) check(job);
    }
    if (warnings.empty()) return responseJson;

    nlohmann::json resp = nlohmann::json::parse(responseJson, nullptr, false);
    if (!resp.is_object()) return responseJson;
    if (resp.contains("warnings") && resp["warnings"].is_array()) {
        for (const auto& w : warnings) resp["warnings"].push_back(w);
    } else {
        resp["warnings"] = warnings;
    }
    return resp.dump();
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + s.size() / 8); // Heuristic to reduce allocations
    for (unsigned char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\b': out += "\\b";  break;
            case '\f': out += "\\f";  break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", static_cast<int>(c));
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

// Finds the value of `key`, preferring the shallowest occurrence. nlohmann writes keys in
// sorted order, so a job's "bt":{"id":...} comes before the job's own "id"; taking the first
// match anywhere made every job of a tree read as the tree's id. The first key at depth 1 (an
// object's own keys) wins at once; otherwise the shallowest one does. So a key that exists only
// in a nested object is still found, and a fragment cut at `"bt":{` still reads the bt's "id".
static size_t FindJsonKeyValuePos(const std::string& json, const std::string& key) {
    auto isWs = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    const size_t n = json.size();
    size_t best = std::string::npos;
    int bestDepth = 0;
    int depth = 0;
    size_t i = 0;
    while (i < n) {
        const char c = json[i];
        if (c == '{' || c == '[') {
            ++depth;
            ++i;
        } else if (c == '}' || c == ']') {
            --depth;
            ++i;
        } else if (c == '"') {
            const size_t start = ++i;
            while (i < n && json[i] != '"') {
                i += (json[i] == '\\') ? 2 : 1;
            }
            if (i >= n) break;
            const size_t end = i++;  // closing quote
            if (end - start != key.size() || json.compare(start, key.size(), key) != 0) continue;
            size_t p = i;
            while (p < n && isWs(json[p])) ++p;
            if (p >= n || json[p] != ':') continue;  // a string value, not a key
            if (best == std::string::npos || depth < bestDepth) {
                ++p;
                while (p < n && isWs(json[p])) ++p;
                best = p;
                bestDepth = depth;
                if (depth <= 1) break;  // nothing can be shallower than the object's own keys
            }
        } else {
            ++i;
        }
    }
    return best;
}

std::string JsonGetString(const std::string& json, const std::string& key, const std::string& defaultVal) {
    size_t valPos = FindJsonKeyValuePos(json, key);
    if (valPos == std::string::npos || valPos >= json.size()) return defaultVal;
    if (json[valPos] != '"') {
        // Might be number or literal
        size_t endPos = valPos;
        while (endPos < json.size() && json[endPos] != ',' && json[endPos] != '}' && json[endPos] != ']' && !std::isspace(static_cast<unsigned char>(json[endPos]))) {
            endPos++;
        }
        std::string raw = json.substr(valPos, endPos - valPos);
        if (raw == "null") return defaultVal;
        return raw;
    }
    valPos++; // skip opening quote
    std::string out;
    while (valPos < json.size()) {
        char c = json[valPos++];
        if (c == '"') {
            return out;
        }
        if (c == '\\' && valPos < json.size()) {
            char esc = json[valPos++];
            switch (esc) {
                case '"':  out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                case '/':  out.push_back('/'); break;
                case 'b':  out.push_back('\b'); break;
                case 'f':  out.push_back('\f'); break;
                case 'n':  out.push_back('\n'); break;
                case 'r':  out.push_back('\r'); break;
                case 't':  out.push_back('\t'); break;
                case 'u':
                    if (valPos + 4 <= json.size()) {
                        int code = std::stoi(json.substr(valPos, 4), nullptr, 16);
                        if (code < 0x80) out.push_back(static_cast<char>(code));
                        valPos += 4;
                    }
                    break;
                default: out.push_back(esc); break;
            }
        } else {
            out.push_back(c);
        }
    }
    return out;
}

int64_t JsonGetInt(const std::string& json, const std::string& key, int64_t defaultVal) {
    std::string s = JsonGetString(json, key, "");
    if (s.empty()) return defaultVal;
    try {
        return std::stoll(s);
    } catch (...) {
        return defaultVal;
    }
}

bool JsonGetBool(const std::string& json, const std::string& key, bool defaultVal) {
    std::string s = JsonGetString(json, key, "");
    if (s == "true" || s == "1") return true;
    if (s == "false" || s == "0") return false;
    return defaultVal;
}

uint64_t GetNowTickMs() {
    return GetTickCount64();
}

#ifdef HANCHO_DEMO_BUILD
namespace {
std::atomic<bool> g_demoMode{false};
std::atomic<int64_t> g_demoFixedNowSec{0};
int64_t g_demoBaseSec = 0;
std::chrono::steady_clock::time_point g_demoStart{};
std::once_flag g_demoEnvOnce;

std::string EpochToTimestampLocal(int64_t epoch) {
    time_t t = static_cast<time_t>(epoch);
    std::tm tm = {};
    if (!LocalTimeFromSec(&t, &tm)) return "";
    char buf[64] = {};
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

void ApplyDemoEnvOnce() {
    std::call_once(g_demoEnvOnce, [] {
        const char* v = std::getenv("HANCHO_DEMO");
        if (!v) return;
        std::string s(v);
        if (s != "1" && s != "true" && s != "TRUE") return;
        int64_t fixed = 0;
        const char* n = std::getenv("HANCHO_DEMO_NOW");
        if (n) fixed = ParseTimestampStrToEpoch(n);
        SetDemoMode(true, fixed);
    });
}
} // namespace

bool IsDemoMode() {
    ApplyDemoEnvOnce();
    return g_demoMode.load();
}

void SetDemoMode(bool enabled, int64_t fixedNowSec) {
    g_demoFixedNowSec.store(enabled ? fixedNowSec : 0);
    if (enabled) {
        g_demoBaseSec = fixedNowSec > 0 ? fixedNowSec : ParseTimestampStrToEpoch("2026-11-21 15:00:00");
        if (g_demoBaseSec <= 0) g_demoBaseSec = static_cast<int64_t>(::time(nullptr));
        g_demoStart = std::chrono::steady_clock::now();
    }
    g_demoMode.store(enabled);
}

void EnableDemoModeFromArgs(int64_t fixedNowSec) {
#ifdef _WIN32
    _putenv_s("HANCHO_DEMO", "1");
    if (fixedNowSec > 0) {
        std::string nowStr = EpochToTimestampLocal(fixedNowSec);
        if (!nowStr.empty()) _putenv_s("HANCHO_DEMO_NOW", nowStr.c_str());
    }
#else
    setenv("HANCHO_DEMO", "1", 1);
    if (fixedNowSec > 0) {
        std::string nowStr = EpochToTimestampLocal(fixedNowSec);
        if (!nowStr.empty()) setenv("HANCHO_DEMO_NOW", nowStr.c_str(), 1);
    }
#endif
    SetDemoMode(true, fixedNowSec);
}

int64_t GetNowEpochSec() {
    ApplyDemoEnvOnce();
    if (!g_demoMode.load()) return static_cast<int64_t>(::time(nullptr));
    int64_t fixed = g_demoFixedNowSec.load();
    if (fixed > 0) return fixed;
    return g_demoBaseSec + std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now() - g_demoStart).count();
}

int64_t GetNowEpochMs() {
    ApplyDemoEnvOnce();
    if (!g_demoMode.load()) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch()).count();
    }
    int64_t fixed = g_demoFixedNowSec.load();
    if (fixed > 0) return fixed * 1000;
    return g_demoBaseSec * 1000 + std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - g_demoStart).count();
}

std::string GetDummyLlmExecutableUtf8() {
    std::filesystem::path p(Utf8ToWide(GetExecutablePathUtf8()));
#ifdef _WIN32
    p.replace_filename(L"hancho-dummy.exe");
#else
    p.replace_filename(L"hancho-dummy");
#endif
    return WideToUtf8(p.wstring());
}
#endif

std::string GetCurrentTimestampStr() {
    time_t t = static_cast<time_t>(GetNowEpochSec());
    std::tm st = {};
    if (!LocalTimeFromSec(&t, &st)) return "";
    char buf[64] = {};
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             st.tm_year + 1900, st.tm_mon + 1, st.tm_mday, st.tm_hour, st.tm_min, st.tm_sec);
    return buf;
}

int64_t ParseTimestampStrToEpoch(const std::string& s) {
    if (s.empty()) return 0;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (!ParseDateTimeFields(s, y, mo, d, h, mi, se)) return 0;
    std::tm tmv = {};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = se;
    tmv.tm_isdst = -1;
    time_t t = std::mktime(&tmv);
    if (t == (time_t)-1) return 0;
    return static_cast<int64_t>(t);
}

bool ParseDurationToSec(const std::string& s, int64_t& outSec) {
    outSec = 0;
    if (s.size() < 2) return false;
    const char unit = static_cast<char>(std::tolower(static_cast<unsigned char>(s.back())));
    int64_t multiplier = 0;
    if (unit == 'm') multiplier = 60;
    else if (unit == 'h') multiplier = 3600;
    else if (unit == 'd') multiplier = 86400;
    else return false;
    const std::string digits = s.substr(0, s.size() - 1);
    for (char c : digits) {
        if (c < '0' || c > '9') return false;
    }
    try {
        outSec = std::stoll(digits) * multiplier;
    } catch (...) {
        outSec = 0;
        return false;
    }
    return true;
}

std::string GetFileTimestampStr() {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char buf[64] = {};
    snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

static std::wstring g_dataDirOverride;

void SetDataDirectoryOverride(const std::wstring& dir) {
    g_dataDirOverride = dir.empty() ? L"" : ResolveAbsolutePathWide(dir);
}

std::wstring GetDataDirectoryOverride() {
    return g_dataDirOverride;
}

std::wstring GetDefaultLogsDirectoryWide() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    std::wstring dir = exePath;
    auto slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        dir = dir.substr(0, slash);
    }

    // If running inside build/Release or build/Debug, place logs/ in project root
    std::wstring parent1 = dir;
    auto s1 = parent1.find_last_of(L"\\/");
    if (s1 != std::wstring::npos) {
        std::wstring leaf1 = parent1.substr(s1 + 1);
        if (_wcsicmp(leaf1.c_str(), L"Release") == 0 || _wcsicmp(leaf1.c_str(), L"Debug") == 0) {
            std::wstring buildDir = parent1.substr(0, s1);
            auto s2 = buildDir.find_last_of(L"\\/");
            if (s2 != std::wstring::npos) {
                std::wstring leaf2 = buildDir.substr(s2 + 1);
                if (_wcsicmp(leaf2.c_str(), L"build") == 0) {
                    dir = buildDir.substr(0, s2);
                }
            }
        }
    }

    std::wstring logsDir = dir + L"\\logs";
    CreateDirectoryW(logsDir.c_str(), nullptr);
    return logsDir;
}

namespace {

enum class DataDirSource { Default, Env, Project, Explicit };
DataDirSource g_dataDirSource = DataDirSource::Default;
bool g_projectDirChecked = false;
std::wstring g_projectDir;

std::wstring DetectProjectDataDir() {
    if (g_projectDirChecked) return g_projectDir;
    g_projectDirChecked = true;
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetCurrentDirectoryW(MAX_PATH, buf);
    if (n == 0 || n >= MAX_PATH) return g_projectDir;
    // Only the current (project) folder counts. Walking up would silently pick up a
    // parent project's .hancho when running from a subfolder of an unrelated project.
    const std::wstring cand = PathJoinWide(std::wstring(buf), L".hancho");
    const DWORD attr = GetFileAttributesW(cand.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
        g_projectDir = ResolveAbsolutePathWide(cand);
    }
    return g_projectDir;
}

} // namespace

std::wstring GetEffectiveDataDirectoryWide() {
    if (!g_dataDirOverride.empty()) {
        g_dataDirSource = DataDirSource::Explicit;
        return g_dataDirOverride;
    }

    wchar_t envBuf[MAX_PATH] = {};
    DWORD envLen = GetEnvironmentVariableW(L"HANCHO_LOG_DIR", envBuf, MAX_PATH);
    if (envLen > 0 && envLen < MAX_PATH) {
        g_dataDirSource = DataDirSource::Env;
        return ResolveAbsolutePathWide(envBuf);
    }
    const std::wstring projectDir = DetectProjectDataDir();
    if (!projectDir.empty()) {
        g_dataDirSource = DataDirSource::Project;
        return projectDir;
    }
    g_dataDirSource = DataDirSource::Default;
    return GetDefaultLogsDirectoryWide();
}

const char* GetDataDirSourceName() {
    GetEffectiveDataDirectoryWide();
    switch (g_dataDirSource) {
    case DataDirSource::Explicit: return "explicit";
    case DataDirSource::Env: return "env";
    case DataDirSource::Project: return "project";
    default: return "default";
    }
}

static std::wstring NormalizeDashboardTitle(const std::wstring& s) {
    std::wstring out = s;
    for (wchar_t& ch : out) {
        if (ch == L'/') ch = L'\\';
    }
    while (out.size() > 1 && out.back() == L'\\') out.pop_back();
    return out;
}

std::wstring DashboardWindowTitle(const std::wstring& dataDir) {
    return std::wstring(L"Hancho - ") + dataDir;
}

bool DashboardTitleMatches(const std::wstring& title, const std::wstring& dataDir) {
    if (WideEqualsIgnoreCase(NormalizeDashboardTitle(title),
                             NormalizeDashboardTitle(DashboardWindowTitle(dataDir)))) {
        return true;
    }
    // Old dashboards used the plain "Hancho" title for the default data directory.
    return WideEqualsIgnoreCase(NormalizeDashboardTitle(title), L"Hancho") &&
           WideEqualsIgnoreCase(NormalizeDashboardTitle(dataDir),
                                NormalizeDashboardTitle(GetDefaultLogsDirectoryWide()));
}

bool HasCustomDataDirectory() {
    if (!g_dataDirOverride.empty()) return true;
    wchar_t buf[2] = {};
    if (GetEnvironmentVariableW(L"HANCHO_LOG_DIR", buf, 2) > 0) return true;
    // An auto-detected project .hancho is just as custom as an explicit --data-dir: the
    // pipe and mutex names must match those of a core that chose the same directory.
    return !DetectProjectDataDir().empty();
}

std::wstring GetLogsDirectoryWide() {
    std::wstring dir = GetEffectiveDataDirectoryWide();
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

static std::wstring DataDirectoryTag() {
    std::wstring path = GetEffectiveDataDirectoryWide();
    for (auto& ch : path) {
        ch = static_cast<wchar_t>(towlower(ch));
    }
    uint32_t hash = 2166136261u;
    for (wchar_t ch : path) {
        hash ^= static_cast<uint32_t>(ch);
        hash *= 16777619u;
    }
    wchar_t buf[16] = {};
    swprintf_s(buf, 16, L"%08x", hash);
    return buf;
}

std::wstring GetPipeNameWide() {
    if (!HasCustomDataDirectory()) return kPipeName;
    return std::wstring(L"\\\\.\\pipe\\Hancho_IPC_v1_") + DataDirectoryTag();
}

std::wstring GetGuiMutexNameWide() {
    if (!HasCustomDataDirectory()) return kMutexName;
    return std::wstring(L"Local\\Hancho_GUI_Mutex_v1_") + DataDirectoryTag();
}

namespace {

std::wstring ActiveCoresDirectory() {
    wchar_t buf[MAX_PATH] = {};
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    const std::wstring root = (n > 0 && n < MAX_PATH) ? std::wstring(buf)
                                                      : GetDefaultLogsDirectoryWide();
    return PathJoinWide(root, L"Hancho\\active-cores");
}

std::wstring ActiveCoreFile(const std::wstring& tag) {
    return PathJoinWide(ActiveCoresDirectory(), L"core-" + tag + L".json");
}

bool ProcessIsAlive(DWORD pid) {
    if (pid == 0) return false;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD code = 0;
    const bool alive = GetExitCodeProcess(h, &code) != 0 && code == STILL_ACTIVE;
    CloseHandle(h);
    return alive;
}

} // namespace

void RegisterActiveCore() {
    const std::wstring dir = ActiveCoresDirectory();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dir), ec);
    if (ec) return;
    const std::wstring tag = DataDirectoryTag();
    const nlohmann::json record = {
        {"dataDir", WideToUtf8(GetEffectiveDataDirectoryWide())},
        {"pid", static_cast<uint64_t>(GetCurrentProcessId())},
        {"pipe", WideToUtf8(GetPipeNameWide())},
        {"source", GetDataDirSourceName()},
        {"startedAt", GetCurrentTimestampStr()}
    };
    WriteFileAtomicWide(ActiveCoreFile(tag), record.dump(2));
}

void UnregisterActiveCore() {
    std::error_code ec;
    std::filesystem::remove(std::filesystem::path(ActiveCoreFile(DataDirectoryTag())), ec);
}

bool AdoptActiveCoreDataDir(std::string& outWarning) {
    outWarning.clear();
    if (!g_dataDirOverride.empty()) return false;
    wchar_t buf[2] = {};
    if (GetEnvironmentVariableW(L"HANCHO_LOG_DIR", buf, 2) > 0) return false;

    const std::wstring dir = ActiveCoresDirectory();
    std::vector<std::wstring> found;
    std::set<std::string> dirs;
    for (const auto& path : ListDirectoryWide(dir, L"core-*.json")) {
        const nlohmann::json doc = nlohmann::json::parse(ReadFileUtf8Wide(path), nullptr, false);
        if (!doc.is_object()) {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(path), ec);
            continue;
        }
        const DWORD pid = static_cast<DWORD>(doc.value("pid", static_cast<uint64_t>(0)));
        if (!ProcessIsAlive(pid)) {
            std::error_code ec;
            std::filesystem::remove(std::filesystem::path(path), ec);
            continue;
        }
        const std::string dataDir = doc.value("dataDir", std::string());
        if (!dataDir.empty()) dirs.insert(dataDir);
        found.push_back(path);
    }
    if (dirs.empty()) return false;
    if (dirs.size() > 1) {
        outWarning = "multiple Hancho cores are running; pass --data-dir explicitly";
        return false;
    }
    SetDataDirectoryOverride(Utf8ToWide(*dirs.begin()));
    return true;
}

std::string GetLogsDirectoryUtf8() {
    return WideToUtf8(GetLogsDirectoryWide());
}

void EnsureDirectoryWide(const std::wstring& dir) {
    if (dir.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(dir), ec);
}

bool PathExistsWide(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDirectoryWide(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool RemoveFileWide(const std::wstring& path) {
    if (path.empty()) return false;
    return DeleteFileW(path.c_str()) != 0;
}

std::vector<std::wstring> ListDirectoryWide(const std::wstring& dir,
                                            const std::wstring& pattern) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW fd = {};
    std::wstring search = dir + L"\\" + pattern;
    HANDLE h = FindFirstFileW(search.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        out.push_back(dir + L"\\" + name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end());
    return out;
}

bool GetFileMtimeWide(const std::wstring& path, uint64_t& outFiletime) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    FILETIME ft = {};
    BOOL ok = GetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
    if (!ok) return false;
    outFiletime = (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return true;
}

uint64_t GetSystemTimeAsFiletime100ns() {
    FILETIME ft = {};
    GetSystemTimeAsFileTime(&ft);
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

std::string GetTempDirectoryUtf8() {
    wchar_t buf[MAX_PATH] = {};
    DWORD len = GetTempPathW(MAX_PATH, buf);
    if (len == 0 || len >= MAX_PATH) return ".";
    std::wstring path(buf, len);
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
    return WideToUtf8(path);
}

std::string GetExecutablePathUtf8() {
    wchar_t buf[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0) return "";
    return WideToUtf8(std::wstring(buf, len));
}

uint32_t GetCurrentProcessIdPortable() {
    return static_cast<uint32_t>(GetCurrentProcessId());
}

std::string GetIpcEndpointUtf8() {
    return WideToUtf8(GetPipeNameWide());
}

bool WideEqualsIgnoreCase(const std::wstring& a, const std::wstring& b) {
    return _wcsicmp(a.c_str(), b.c_str()) == 0;
}

std::wstring ToNativePathWide(const std::wstring& path) {
    return path;
}

std::wstring PathJoinWide(const std::wstring& left, const std::wstring& right) {
    return left + L"\\" + right;
}

std::string SanitizeFileNameComponent(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.') {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('_');
        }
    }
    if (out.empty()) return "proc";
    if (out.size() > 32) out.resize(32);
    return out;
}

void AppendToLogFileWide(const std::wstring& filePath, const std::string& data) {
    if (filePath.empty() || data.empty()) return;
    HANDLE hFile = CreateFileW(
        filePath.c_str(),
        FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (hFile == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(hFile, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(hFile);
}

void WriteFileAtomicWide(const std::wstring& filePath, const std::string& data) {
    if (filePath.empty()) return;
    std::wstring tmpPath = filePath + L".tmp";
    HANDLE hFile = CreateFileW(
        tmpPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (hFile != INVALID_HANDLE_VALUE) {
        DWORD written = 0;
        if (!data.empty()) {
            WriteFile(hFile, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
        }
        FlushFileBuffers(hFile);
        CloseHandle(hFile);
        MoveFileExW(tmpPath.c_str(), filePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    } else {
        // Fallback direct write
        HANDLE hDirect = CreateFileW(
            filePath.c_str(),
            GENERIC_WRITE,
            FILE_SHARE_READ,
            nullptr,
            CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL,
            nullptr
        );
        if (hDirect != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            if (!data.empty()) {
                WriteFile(hDirect, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
            }
            CloseHandle(hDirect);
        }
    }
}

std::string ReadFileUtf8Wide(const std::wstring& filePath) {
    if (filePath.empty()) return "";
    HANDLE hFile = CreateFileW(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (hFile == INVALID_HANDLE_VALUE) return "";
    LARGE_INTEGER fileSize = {};
    if (!GetFileSizeEx(hFile, &fileSize) || fileSize.QuadPart <= 0) {
        CloseHandle(hFile);
        return "";
    }
    constexpr int64_t kMaxRead = 4 * 1024 * 1024;
    int64_t toRead = fileSize.QuadPart;
    if (toRead > kMaxRead) {
        LARGE_INTEGER offset = {};
        offset.QuadPart = fileSize.QuadPart - kMaxRead;
        SetFilePointerEx(hFile, offset, nullptr, FILE_BEGIN);
        toRead = kMaxRead;
    }
    std::string buf(static_cast<size_t>(toRead), '\0');
    DWORD bytesRead = 0;
    ReadFile(hFile, buf.data(), static_cast<DWORD>(toRead), &bytesRead, nullptr);
    CloseHandle(hFile);
    buf.resize(bytesRead);
    return buf;
}

void WriteAuditLog(const std::string& category, const std::string& message) {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char dateBuf[32] = {};
    snprintf(dateBuf, sizeof(dateBuf), "%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);

    std::wstring auditPath = GetLogsDirectoryWide() + L"\\hancho_" + Utf8ToWide(dateBuf) + L".log";
    std::ostringstream oss;
    oss << "[" << GetCurrentTimestampStr() << "] [PID:" << GetCurrentProcessId() << "] [" << category << "] " << message << "\r\n";
    AppendToLogFileWide(auditPath, oss.str());
}

std::string FormatUptimeHMS(uint64_t ms) {
    uint64_t totalSec = ms / 1000;
    uint64_t hrs = totalSec / 3600;
    uint64_t mins = (totalSec % 3600) / 60;
    uint64_t secs = totalSec % 60;
    char buf[64] = {};
    snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu",
             static_cast<unsigned long long>(hrs),
             static_cast<unsigned long long>(mins),
             static_cast<unsigned long long>(secs));
    return buf;
}

std::string FormatDurationSec(uint64_t ms) {
    uint64_t totalSec = ms / 1000;
    uint64_t mins = totalSec / 60;
    uint64_t secs = totalSec % 60;
    std::ostringstream oss;
    if (mins > 0) {
        oss << mins << "m " << std::setw(2) << std::setfill('0') << secs << "s";
    } else {
        oss << secs << "." << ((ms % 1000) / 100) << "s";
    }
    return oss.str();
}

std::string FormatBytes(uint64_t bytes) {
    if (bytes == 0) return "0 MB";
    double mb = static_cast<double>(bytes) / (1024.0 * 1024.0);
    std::ostringstream oss;
    if (mb < 1.0) {
        double kb = static_cast<double>(bytes) / 1024.0;
        oss << std::fixed << std::setprecision(0) << kb << " KB";
    } else {
        oss << std::fixed << std::setprecision(1) << mb << " MB";
    }
    return oss.str();
}

// ---------------------------------------------------------------------------
// Project data reset
// ---------------------------------------------------------------------------

static bool RemoveTreeWide(const std::wstring& path, std::string& outError) {
    DWORD attr = GetFileAttributesW(path.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) return true;
    if (attr & FILE_ATTRIBUTE_DIRECTORY) {
        WIN32_FIND_DATAW fd = {};
        HANDLE hFind = FindFirstFileW((path + L"\\*").c_str(), &fd);
        if (hFind != INVALID_HANDLE_VALUE) {
            do {
                std::wstring name = fd.cFileName;
                if (name == L"." || name == L"..") continue;
                if (!RemoveTreeWide(path + L"\\" + name, outError)) {
                    FindClose(hFind);
                    return false;
                }
            } while (FindNextFileW(hFind, &fd));
            FindClose(hFind);
        }
        if (attr & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_DIRECTORY);
        if (!RemoveDirectoryW(path.c_str())) {
            outError = "failed to remove directory: " + WideToUtf8(path) + " (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        return true;
    }
    if (attr & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (!DeleteFileW(path.c_str())) {
        outError = "failed to delete file: " + WideToUtf8(path) + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}

std::vector<std::wstring> ListProjectDataEntriesWide() {
    std::vector<std::wstring> out;
    std::wstring dir = GetEffectiveDataDirectoryWide();
    WIN32_FIND_DATAW fd = {};
    HANDLE hFind = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        out.push_back(dir + L"\\" + name);
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    std::sort(out.begin(), out.end());
    return out;
}

bool IsHanchoRunningForDataDirWide() {
    HANDLE h = OpenMutexW(SYNCHRONIZE, FALSE, GetGuiMutexNameWide().c_str());
    if (h) {
        CloseHandle(h);
        return true;
    }
    return false;
}

size_t ResetProjectDataWide(std::string& outError) {
    outError.clear();
    std::wstring dir = GetEffectiveDataDirectoryWide();
    // Refuse to wipe a drive root or an empty path.
    if (dir.size() < 4 || dir.find(L':') == std::wstring::npos) {
        outError = "refusing to reset a suspicious data directory: " + WideToUtf8(dir);
        return 0;
    }
    size_t removed = 0;
    for (const auto& entry : ListProjectDataEntriesWide()) {
        if (!RemoveTreeWide(entry, outError)) return removed;
        ++removed;
    }
    return removed;
}

static std::string GetEnvUtf8(const wchar_t* name) {
    wchar_t buf[1024] = {};
    DWORD len = GetEnvironmentVariableW(name, buf, static_cast<DWORD>(std::size(buf)));
    if (len == 0 || len >= std::size(buf)) return "";
    return WideToUtf8(std::wstring(buf, len));
}

static DWORD GetParentProcessIdOfSelf() {
    DWORD self = GetCurrentProcessId();
    DWORD parent = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return 0;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (pe.th32ProcessID == self) { parent = pe.th32ParentProcessID; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return parent;
}

static std::string CallerSessionId() {
    std::string s = GetEnvUtf8(L"PC_ORIGIN_SESSION");
    if (s.empty()) s = GetEnvUtf8(L"CLAUDE_CODE_SESSION_ID");
    return s;
}

static int64_t CallerPid() {
    std::string s = GetEnvUtf8(L"CLAUDE_PID");
    if (!s.empty()) {
        try { return std::stoll(s); } catch (...) {}
    }
    return static_cast<int64_t>(GetParentProcessIdOfSelf());
}

std::string BuildCallerOriginJson(const std::string& client, const std::string& label) {
    std::string lbl = label.empty() ? GetEnvUtf8(L"PC_ORIGIN_LABEL") : label;
    wchar_t cwdBuf[MAX_PATH * 2] = {};
    DWORD cwdLen = GetCurrentDirectoryW(static_cast<DWORD>(std::size(cwdBuf)), cwdBuf);
    std::string cwd = (cwdLen > 0 && cwdLen < std::size(cwdBuf)) ? WideToUtf8(std::wstring(cwdBuf, cwdLen)) : "";
    std::ostringstream oss;
    oss << "{\"label\":\"" << JsonEscape(lbl) << "\""
        << ",\"client\":\"" << JsonEscape(client) << "\""
        << ",\"session\":\"" << JsonEscape(CallerSessionId()) << "\""
        << ",\"host_session\":\"" << JsonEscape(GetEnvUtf8(L"CLAUDE_CODE_HOST_SESSION_ID")) << "\""
        << ",\"cwd\":\"" << JsonEscape(cwd) << "\""
        << ",\"pid\":" << CallerPid() << "}";
    return oss.str();
}

std::string CallerSelfOriginFilter() {
    std::string s = CallerSessionId();
    if (!s.empty()) return s;
    return "pid:" + std::to_string(CallerPid());
}

static std::string PowerShellSingleQuote(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        if (c == '\'') out += "''";
        else out.push_back(c);
    }
    return out;
}

std::string BuildQuickFixPowerShell(const QuickFixSpec& spec) {
    std::ostringstream ps;
    ps << "$ErrorActionPreference = 'Stop'\r\n";
    ps << "$path = '" << PowerShellSingleQuote(spec.file) << "'\r\n";
    ps << "$find = '" << PowerShellSingleQuote(spec.find) << "'\r\n";
    ps << "$replace = '" << PowerShellSingleQuote(spec.replace) << "'\r\n";
    ps << "$text = [IO.File]::ReadAllText($path)\r\n";
    ps << "$index = $text.IndexOf($find, [StringComparison]::Ordinal)\r\n";
    ps << "if ($index -lt 0) { [Console]::Error.WriteLine('quickfix: find text not found'); exit 3 }\r\n";
    if (spec.all) ps << "$text = $text.Replace($find, $replace)\r\n";
    else ps << "$text = $text.Remove($index, $find.Length).Insert($index, $replace)\r\n";
    ps << "[IO.File]::WriteAllText($path, $text)\r\n";
    if (!spec.check.empty()) {
        ps << "if (-not (Select-String -Path $path -Pattern '" << PowerShellSingleQuote(spec.check)
           << "' -SimpleMatch:" << (spec.checkLiteral ? "$true" : "$false") << " -Quiet)) {\r\n";
        ps << "  [Console]::Error.WriteLine('quickfix: check did not match'); exit 4\r\n";
        ps << "}\r\n";
    }
    ps << "exit 0\r\n";
    return ps.str();
}

std::string BuildQuickFixCommandLine(const std::wstring& scriptPath) {
    return "powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass -File \"" +
           WideToUtf8(scriptPath) + "\"";
}

bool WriteHanchoEventLine(const std::string& endpoint, const std::string& line,
                          std::string& outError) {
    std::wstring fullPipe = Utf8ToWide(endpoint);
    if (fullPipe.rfind(L"\\\\.\\pipe\\", 0) != 0) fullPipe = L"\\\\.\\pipe\\" + fullPipe;
    HANDLE h = CreateFileW(fullPipe.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        outError = "cannot open event pipe " + endpoint + " (Win32 error " +
                   std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD written = 0;
    BOOL ok = WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    DWORD lastError = ok ? ERROR_SUCCESS : GetLastError();
    CloseHandle(h);
    if (!ok) {
        outError = "cannot write to event pipe (Win32 error " + std::to_string(lastError) + ")";
        return false;
    }
    return true;
}

// Self-contained SHA-256 (moved here from taskqueue.cpp so routines can hash their inputs too).
namespace {

struct Sha256Ctx {
    uint32_t h[8];
    uint64_t len = 0;
    uint8_t buf[64];
    size_t bufLen = 0;
};

uint32_t Rotr32(uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); }

void Sha256Init(Sha256Ctx& c) {
    static const uint32_t kInit[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    for (int i = 0; i < 8; ++i) c.h[i] = kInit[i];
}

void Sha256Block(Sha256Ctx& c, const uint8_t* p) {
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
        w[i] = (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
               (uint32_t(p[i * 4 + 2]) << 8) | uint32_t(p[i * 4 + 3]);
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

void Sha256Update(Sha256Ctx& c, const uint8_t* data, size_t len) {
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

} // namespace

std::string Sha256Hex(const std::string& input) {
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

AppConfig LoadAppConfig() {
    AppConfig cfg;
    const std::string content = ReadFileUtf8Wide(PathJoinWide(GetEffectiveDataDirectoryWide(), L"config.json"));
    if (!content.empty()) {
        const int64_t v = JsonGetInt(content, "task_list_recent_count", kDefaultTaskListRecentCount);
        if (v > 0) cfg.taskListRecentCount = v;
    }
    return cfg;
}

int64_t GetTaskListRecentCount() {
    return LoadAppConfig().taskListRecentCount;
}

} // namespace pc
