#include "common.h"
#include "parse_util.h"
#include "platform/platform.h"

#ifndef _WIN32

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cwchar>
#include <cwctype>
#include <thread>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <dirent.h>
#include <fnmatch.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

namespace pc {

namespace {

std::string ToUtf8FromWide(const std::wstring& wide) {
    std::string out;
    out.reserve(wide.size() * 3);
    for (size_t i = 0; i < wide.size(); ++i) {
        uint32_t cp = static_cast<uint32_t>(wide[i]);
        if (sizeof(wchar_t) == 2 && cp >= 0xD800 && cp <= 0xDBFF && i + 1 < wide.size()) {
            uint32_t low = static_cast<uint32_t>(wide[i + 1]);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                ++i;
            }
        }
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }
    return out;
}

std::wstring ToWideFromUtf8(const std::string& utf8) {
    std::wstring out;
    out.reserve(utf8.size());
    size_t i = 0;
    while (i < utf8.size()) {
        unsigned char c = static_cast<unsigned char>(utf8[i]);
        uint32_t cp = 0xFFFD;
        size_t extra = 0;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; extra = 1; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; extra = 2; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; extra = 3; }
        if (i + extra >= utf8.size() && extra > 0) {
            out.push_back(static_cast<wchar_t>(0xFFFD));
            break;
        }
        bool valid = true;
        for (size_t k = 1; k <= extra; ++k) {
            unsigned char cc = static_cast<unsigned char>(utf8[i + k]);
            if ((cc & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (cc & 0x3F);
        }
        if (!valid) {
            out.push_back(static_cast<wchar_t>(0xFFFD));
            ++i;
            continue;
        }
        i += extra + 1;
        if (sizeof(wchar_t) == 2 && cp > 0xFFFF) {
            cp -= 0x10000;
            out.push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out.push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
        } else {
            out.push_back(static_cast<wchar_t>(cp));
        }
    }
    return out;
}

fs::path ToPath(const std::wstring& path) {
    return fs::path(ToUtf8FromWide(platform::ToNativePath(path)));
}

std::wstring FromPath(const fs::path& path) {
    return ToWideFromUtf8(path.string());
}

constexpr int64_t kWindowsToUnixEpochSec = 11644473600LL;

std::wstring g_dataDirOverride;

std::wstring GetDefaultLogsDirectoryWide() {
    fs::path exe(GetExecutablePathUtf8());
    fs::path dir = exe.parent_path();
    const std::string leaf = dir.filename().string();
    if (leaf == "Release" || leaf == "Debug") {
        fs::path buildDir = dir.parent_path();
        if (buildDir.filename().string() == "build") dir = buildDir.parent_path();
    }
    fs::path logs = dir / "logs";
    EnsureDirectoryWide(FromPath(logs));
    return FromPath(logs);
}

std::wstring DataDirectoryTag() {
    std::wstring path = GetEffectiveDataDirectoryWide();
    for (auto& ch : path) ch = static_cast<wchar_t>(towlower(ch));
    uint32_t hash = 2166136261u;
    for (wchar_t ch : path) {
        hash ^= static_cast<uint32_t>(ch);
        hash *= 16777619u;
    }
    wchar_t buf[16] = {};
    swprintf(buf, 16, L"%08x", hash);
    return std::wstring(buf);
}

bool RemoveTreeWide(const std::wstring& path, std::string& outError) {
    std::error_code ec;
    fs::remove_all(ToPath(path), ec);
    if (ec) {
        outError = "failed to remove: " + WideToUtf8(path) + " (" + ec.message() + ")";
        return false;
    }
    return true;
}

std::string GetEnvUtf8(const char* name) {
    const char* value = std::getenv(name);
    return value ? std::string(value) : std::string();
}

std::string CallerSessionId() {
    std::string s = GetEnvUtf8("PC_ORIGIN_SESSION");
    if (s.empty()) s = GetEnvUtf8("CLAUDE_CODE_SESSION_ID");
    return s;
}

int64_t CallerPid() {
    std::string s = GetEnvUtf8("CLAUDE_PID");
    if (!s.empty()) {
        try { return std::stoll(s); } catch (...) {}
    }
    return static_cast<int64_t>(getppid());
}

} // namespace

std::wstring Utf8ToWide(const std::string& utf8) { return ToWideFromUtf8(utf8); }
std::string WideToUtf8(const std::wstring& wide) { return ToUtf8FromWide(wide); }

std::wstring GetCurrentDirectoryWide() {
    std::error_code ec;
    fs::path p = fs::current_path(ec);
    return ec ? std::wstring() : FromPath(p);
}

std::wstring ResolveAbsolutePathWide(const std::wstring& path) {
    if (path.empty()) return GetCurrentDirectoryWide();
    std::error_code ec;
    fs::path p = fs::absolute(ToPath(path), ec);
    if (ec) return path;
    fs::path canonical = fs::weakly_canonical(p, ec);
    return FromPath(ec ? p : canonical);
}

std::string ResolveAbsolutePathUtf8(const std::string& path) {
    return WideToUtf8(ResolveAbsolutePathWide(Utf8ToWide(path)));
}

void SleepMs(uint32_t ms) {
    if (ms == 0) return;
    std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

void EnsureDirectoryWide(const std::wstring& dir) {
    if (dir.empty()) return;
    std::error_code ec;
    fs::create_directories(ToPath(dir), ec);
}

bool PathExistsWide(const std::wstring& path) {
    std::error_code ec;
    return fs::exists(ToPath(path), ec) && !ec;
}

bool IsDirectoryWide(const std::wstring& path) {
    std::error_code ec;
    return fs::is_directory(ToPath(path), ec) && !ec;
}

bool RemoveFileWide(const std::wstring& path) {
    if (path.empty()) return false;
    std::error_code ec;
    return fs::remove(ToPath(path), ec) && !ec;
}

std::vector<std::wstring> ListDirectoryWide(const std::wstring& dir,
                                            const std::wstring& pattern) {
    std::vector<std::wstring> out;
    std::error_code ec;
    fs::path base = ToPath(dir);
    if (!fs::is_directory(base, ec)) return out;
    const std::string pat = WideToUtf8(pattern);
    for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = WideToUtf8(FromPath(it->path().filename()));
        if (fnmatch(pat.c_str(), name.c_str(), 0) != 0) continue;
        out.push_back(FromPath(it->path()));
    }
    std::sort(out.begin(), out.end());
    return out;
}

uint64_t GetSystemTimeAsFiletime100ns() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(now).count();
    return static_cast<uint64_t>(us) * 10ull +
           static_cast<uint64_t>(kWindowsToUnixEpochSec) * 10000000ull;
}

bool GetFileMtimeWide(const std::wstring& path, uint64_t& outFiletime) {
    std::error_code ec;
    fs::path p = ToPath(path);
    if (!fs::exists(p, ec) || ec) return false;
    auto ft = fs::last_write_time(p, ec);
    if (ec) return false;
    using FileClock = decltype(ft)::clock;
    const auto systemTime = std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        ft - FileClock::now() + std::chrono::system_clock::now());
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
        systemTime.time_since_epoch()).count();
    outFiletime = static_cast<uint64_t>(us) * 10ull +
                  static_cast<uint64_t>(kWindowsToUnixEpochSec) * 10000000ull;
    return true;
}

std::string GetTempDirectoryUtf8() {
    const char* tmp = std::getenv("TMPDIR");
    if (!tmp || !*tmp) tmp = "/tmp";
    return tmp;
}

std::string GetExecutablePathUtf8() {
#if defined(__APPLE__)
    char buf[4096] = {};
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) return std::string(buf);
#else
    char buf[4096] = {};
    ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) return std::string(buf, static_cast<size_t>(n));
#endif
    return ResolveAbsolutePathUtf8("./Hancho");
}

uint32_t GetCurrentProcessIdPortable() {
    return static_cast<uint32_t>(getpid());
}

bool WideEqualsIgnoreCase(const std::wstring& a, const std::wstring& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::towlower(a[i]) != std::towlower(b[i])) return false;
    }
    return true;
}

std::wstring ToNativePathWide(const std::wstring& path) {
    return platform::ToNativePath(path);
}

std::wstring PathJoinWide(const std::wstring& left, const std::wstring& right) {
    std::wstring a = ToNativePathWide(left);
    std::wstring b = ToNativePathWide(right);
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() == L'/') a.pop_back();
    while (!b.empty() && b.front() == L'/') b.erase(b.begin());
    return a + L"/" + b;
}

std::string ExecModeToString(ExecMode mode) {
    switch (mode) {
        case ExecMode::Pipe: return "pipe";
        case ExecMode::ConPTY: return "pty";
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
        case ProcState::Running: return "running";
        case ProcState::Completed: return "completed";
        case ProcState::Failed: return "failed";
        case ProcState::TimedOut: return "timed_out";
        case ProcState::IdleTimedOut: return "idle_timed_out";
        case ProcState::Killed: return "killed";
    }
    return "unknown";
}

ProcState StringToProcState(const std::string& s) {
    if (s == "running") return ProcState::Running;
    if (s == "completed") return ProcState::Completed;
    if (s == "failed") return ProcState::Failed;
    if (s == "timed_out") return ProcState::TimedOut;
    if (s == "idle_timed_out") return ProcState::IdleTimedOut;
    if (s == "killed") return ProcState::Killed;
    return ProcState::Completed;
}

std::wstring ProcStateToDisplayWide(ProcState state, bool isIdleWarning) {
    switch (state) {
        case ProcState::Running:
            return isIdleWarning ? L"IDLE / WAITING" : L"RUNNING";
        case ProcState::Completed: return L"COMPLETED (OK)";
        case ProcState::Failed: return L"FAILED";
        case ProcState::TimedOut: return L"TIMEOUT";
        case ProcState::IdleTimedOut: return L"HANG TIMEOUT";
        case ProcState::Killed: return L"KILLED";
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
        if (c == 0x1B) {
            if (i + 1 < n) {
                char next = input[i + 1];
                if (next == '[') {
                    i += 2;
                    while (i < n) {
                        unsigned char fc = static_cast<unsigned char>(input[i]);
                        if (fc >= 0x40 && fc <= 0x7E) {
                            if (fc == 'H' || fc == 'f') {
                                if (!out.empty() && out.back() != '\n') out.push_back('\n');
                            }
                            i++;
                            break;
                        }
                        i++;
                    }
                    continue;
                }
                if (next == ']') {
                    i += 2;
                    while (i < n && input[i] != '\a') {
                        if (input[i] == 0x1B && i + 1 < n && input[i + 1] == '\\') {
                            i += 2;
                            break;
                        }
                        i++;
                    }
                    if (i < n && input[i] == '\a') i++;
                    continue;
                }
                i += 2;
                continue;
            }
            i++;
            continue;
        }
        if (c == '\r') {
            if (i + 1 < n && input[i + 1] == '\n') {
                out.push_back('\n');
                i += 2;
            } else {
                i++;
            }
            continue;
        }
        if (c == 0x08) {
            if (!out.empty() && out.back() != '\n') out.pop_back();
            i++;
            continue;
        }
        out.push_back(static_cast<char>(c));
        i++;
    }
    return out;
}

std::wstring NormalizeCrLfWide(const std::string& utf8Clean) {
    std::wstring out;
    out.reserve(utf8Clean.size());
    for (size_t i = 0; i < utf8Clean.size(); ++i) {
        char c = utf8Clean[i];
        if (c == '\r') {
            if (i + 1 < utf8Clean.size() && utf8Clean[i + 1] == '\n') continue;
            out.push_back(L'\n');
        } else if (c == '\n') {
            out.push_back(L'\n');
        } else {
            out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
        }
    }
    return out;
}

std::string GetLastNonEmptyLine(const std::string& text, size_t maxLen) {
    size_t end = text.find_last_not_of("\r\n \t");
    if (end == std::string::npos) return "";
    size_t begin = text.find_last_of("\r\n", end);
    begin = (begin == std::string::npos) ? 0 : begin + 1;
    std::string line = text.substr(begin, end - begin + 1);
    if (line.size() > maxLen) line = line.substr(line.size() - maxLen);
    return line;
}

std::string GetTailLines(const std::string& text, int maxLines) {
    if (maxLines <= 0) return "";
    int lines = 0;
    size_t pos = text.size();
    while (pos > 0 && lines < maxLines) {
        pos = text.rfind('\n', pos == text.size() ? pos : pos - 1);
        if (pos == std::string::npos) {
            pos = 0;
            ++lines;
            break;
        }
        ++lines;
        if (pos == 0) break;
    }
    return pos == std::string::npos ? text : text.substr(pos + 1);
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8] = {};
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
        }
    }
    return out;
}

static size_t FindJsonValueStart(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t keyPos = json.find(pattern);
    if (keyPos == std::string::npos) return std::string::npos;
    size_t colon = json.find(':', keyPos + pattern.size());
    if (colon == std::string::npos) return std::string::npos;
    size_t p = colon + 1;
    while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) p++;
    return p;
}

std::string JsonGetString(const std::string& json, const std::string& key,
                          const std::string& defaultVal) {
    size_t p = FindJsonValueStart(json, key);
    if (p == std::string::npos || p >= json.size()) return defaultVal;
    if (json[p] != '"') {
        size_t end = p;
        while (end < json.size() && json[end] != ',' && json[end] != '}' &&
               json[end] != ']' && !std::isspace(static_cast<unsigned char>(json[end]))) {
            end++;
        }
        std::string raw = json.substr(p, end - p);
        if (raw == "null" || raw.empty()) return defaultVal;
        return raw;
    }
    std::string out;
    for (size_t i = p + 1; i < json.size(); ++i) {
        char c = json[i];
        if (c == '"') return out;
        if (c == '\' && i + 1 < json.size()) {
            char e = json[++i];
            switch (e) {
                case '"': out.push_back('"'); break;
                case '\': out.push_back('\'); break;
                case '/': out.push_back('/'); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                    if (i + 4 <= json.size()) {
                        try {
                            int code = std::stoi(json.substr(i + 1, 4), nullptr, 16);
                            if (code >= 0 && code < 0x80) out.push_back(static_cast<char>(code));
                        } catch (...) {}
                        i += 4;
                    }
                    break;
                default: out.push_back(e); break;
            }
        } else {
            out.push_back(c);
        }
    }
    return defaultVal;
}

int64_t JsonGetInt(const std::string& json, const std::string& key, int64_t defaultVal) {
    size_t p = FindJsonValueStart(json, key);
    if (p == std::string::npos) return defaultVal;
    try {
        return std::stoll(json.substr(p));
    } catch (...) {
        return defaultVal;
    }
}

bool JsonGetBool(const std::string& json, const std::string& key, bool defaultVal) {
    std::string s = JsonGetString(json, key, "");
    if (s == "true" || s == "1") return true;
    if (s == "false" || s == "0") return false;
    int64_t n = JsonGetInt(json, key, -1);
    if (n == 1) return true;
    if (n == 0) return false;
    return defaultVal;
}

uint64_t GetNowTickMs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

std::string GetCurrentTimestampStr() {
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&now, &tmv);
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

int64_t ParseTimestampStrToEpoch(const std::string& s) {
    if (s.empty()) return 0;
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, se = 0;
    if (!ParseDateTimeFields(s, y, mo, d, h, mi, se)) return 0;
    std::tm tmv{};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = se;
    tmv.tm_isdst = -1;
    time_t t = std::mktime(&tmv);
    if (t == static_cast<time_t>(-1)) return 0;
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
    std::time_t now = std::time(nullptr);
    std::tm tmv{};
    localtime_r(&now, &tmv);
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d_%02d%02d%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

void SetDataDirectoryOverride(const std::wstring& dir) {
    g_dataDirOverride = dir.empty() ? L"" : ResolveAbsolutePathWide(dir);
}

std::wstring GetDataDirectoryOverride() { return g_dataDirOverride; }

std::wstring GetEffectiveDataDirectoryWide() {
    if (!g_dataDirOverride.empty()) return g_dataDirOverride;
    const char* env = std::getenv("HANCHO_LOG_DIR");
    if (env && *env) return ResolveAbsolutePathWide(Utf8ToWide(env));
    return GetDefaultLogsDirectoryWide();
}

bool HasCustomDataDirectory() {
    if (!g_dataDirOverride.empty()) return true;
    const char* env = std::getenv("HANCHO_LOG_DIR");
    return env && *env;
}

std::wstring GetLogsDirectoryWide() {
    std::wstring dir = GetEffectiveDataDirectoryWide();
    EnsureDirectoryWide(dir);
    return dir;
}

std::wstring GetPipeNameWide() {
    return Utf8ToWide(GetIpcEndpointUtf8());
}

std::wstring GetGuiMutexNameWide() {
    if (!HasCustomDataDirectory()) return kMutexName;
    return std::wstring(L"Hancho_GUI_Mutex_v1_") + DataDirectoryTag();
}

std::string GetIpcEndpointUtf8() {
    if (!HasCustomDataDirectory()) {
        return "/tmp/Hancho_IPC_v1-" + std::to_string(static_cast<long long>(getuid())) + ".sock";
    }
    return WideToUtf8(GetEffectiveDataDirectoryWide()) + "/hancho-ipc.sock";
}

std::string GetLogsDirectoryUtf8() { return WideToUtf8(GetLogsDirectoryWide()); }

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
    std::ofstream out(ToPath(filePath), std::ios::binary | std::ios::app);
    if (out) out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

void WriteFileAtomicWide(const std::wstring& filePath, const std::string& data) {
    if (filePath.empty()) return;
    fs::path finalPath = ToPath(filePath);
    fs::path tmpPath = finalPath;
    tmpPath += ".tmp";
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out) return;
        if (!data.empty()) out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
    }
    std::error_code ec;
    fs::rename(tmpPath, finalPath, ec);
    if (ec) {
        fs::remove(finalPath, ec);
        ec.clear();
        fs::rename(tmpPath, finalPath, ec);
    }
}

std::string ReadFileUtf8Wide(const std::wstring& filePath) {
    if (filePath.empty()) return "";
    std::ifstream in(ToPath(filePath), std::ios::binary);
    if (!in) return "";
    std::error_code ec;
    const auto size = fs::file_size(ToPath(filePath), ec);
    constexpr uint64_t kMaxRead = 4 * 1024 * 1024;
    if (!ec && size > kMaxRead) {
        in.seekg(static_cast<std::streamoff>(size - kMaxRead));
    }
    std::ostringstream buf;
    buf << in.rdbuf();
    std::string out = buf.str();
    if (out.size() > kMaxRead) out.erase(0, out.size() - kMaxRead);
    return out;
}

void WriteAuditLog(const std::string& category, const std::string& message) {
    std::string date = GetCurrentTimestampStr().substr(0, 10);
    std::wstring auditPath = GetLogsDirectoryWide() + L"/hancho_" + Utf8ToWide(date) + L".log";
    std::ostringstream oss;
    oss << "[" << GetCurrentTimestampStr() << "] [PID:" << getpid() << "] [" << category << "] "
        << message << "\r\n";
    AppendToLogFileWide(auditPath, oss.str());
}

std::string FormatUptimeHMS(uint64_t ms) {
    uint64_t totalSec = ms / 1000;
    uint64_t hrs = totalSec / 3600;
    uint64_t mins = (totalSec % 3600) / 60;
    uint64_t secs = totalSec % 60;
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu",
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

std::vector<std::wstring> ListProjectDataEntriesWide() {
    std::vector<std::wstring> out;
    std::wstring dir = GetEffectiveDataDirectoryWide();
    for (const auto& entry : ListDirectoryWide(dir, L"*")) out.push_back(entry);
    std::sort(out.begin(), out.end());
    return out;
}

bool IsHanchoRunningForDataDirWide() {
    return platform::IsInstanceLockHeld(WideToUtf8(GetGuiMutexNameWide()));
}

size_t ResetProjectDataWide(std::string& outError) {
    outError.clear();
    std::wstring dir = GetEffectiveDataDirectoryWide();
    if (dir.size() < 2 || dir == L"/") {
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

std::string BuildCallerOriginJson(const std::string& client, const std::string& label) {
    std::string lbl = label.empty() ? GetEnvUtf8("PC_ORIGIN_LABEL") : label;
    std::ostringstream oss;
    oss << "{\"label\":\"" << JsonEscape(lbl) << "\""
        << ",\"client\":\"" << JsonEscape(client) << "\""
        << ",\"session\":\"" << JsonEscape(CallerSessionId()) << "\""
        << ",\"host_session\":\"" << JsonEscape(GetEnvUtf8("CLAUDE_CODE_HOST_SESSION_ID")) << "\""
        << ",\"cwd\":\"" << JsonEscape(GetCurrentDirectoryWide().empty()
                                            ? std::string()
                                            : WideToUtf8(GetCurrentDirectoryWide())) << "\""
        << ",\"pid\":" << CallerPid() << "}";
    return oss.str();
}

std::string CallerSelfOriginFilter() {
    std::string s = CallerSessionId();
    if (!s.empty()) return s;
    return "pid:" + std::to_string(CallerPid());
}

bool WriteHanchoEventLine(const std::string& endpoint, const std::string& line,
                          std::string& outError) {
    std::string err;
    auto conn = platform::ConnectIpc(endpoint, 2000, err);
    if (!conn) {
        outError = err.empty() ? ("cannot connect to event pipe " + endpoint) : err;
        return false;
    }
    std::string payload = line;
    if (payload.empty() || payload.back() != '\n') payload.push_back('\n');
    if (!conn->WriteAll(payload.data(), payload.size(), err)) {
        outError = "cannot write to event pipe: " + err;
        return false;
    }
    conn->Close();
    return true;
}

} // namespace pc

#endif // !_WIN32
