#include "common.h"
#include <tlhelp32.h>
#include <iterator>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <ctime>
#include <cstring>
#include <filesystem>

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
        case ProcState::Running:      return "running";
        case ProcState::Completed:    return "completed";
        case ProcState::Failed:       return "failed";
        case ProcState::TimedOut:     return "timed_out";
        case ProcState::IdleTimedOut: return "idle_timed_out";
        case ProcState::Killed:       return "killed";
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
        case ProcState::Completed:    return L"COMPLETED (OK)";
        case ProcState::Failed:       return L"FAILED";
        case ProcState::TimedOut:     return L"TIMEOUT";
        case ProcState::IdleTimedOut: return L"HANG TIMEOUT";
        case ProcState::Killed:       return L"KILLED";
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

static size_t FindJsonKeyValuePos(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t pos = 0;
    while ((pos = json.find(pattern, pos)) != std::string::npos) {
        size_t afterKey = pos + pattern.size();
        while (afterKey < json.size() && (json[afterKey] == ' ' || json[afterKey] == '\t' || json[afterKey] == '\r' || json[afterKey] == '\n')) {
            afterKey++;
        }
        if (afterKey < json.size() && json[afterKey] == ':') {
            afterKey++;
            while (afterKey < json.size() && (json[afterKey] == ' ' || json[afterKey] == '\t' || json[afterKey] == '\r' || json[afterKey] == '\n')) {
                afterKey++;
            }
            return afterKey;
        }
        pos += pattern.size();
    }
    return std::string::npos;
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

std::string GetCurrentTimestampStr() {
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    char buf[64] = {};
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

int64_t ParseTimestampStrToEpoch(const std::string& s) {
    if (s.empty()) return 0;
    int y, mo, d, h, mi, se;
    if (sscanf(s.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &se) != 6) return 0;
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

static std::wstring GetDefaultLogsDirectoryWide() {
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

std::wstring GetEffectiveDataDirectoryWide() {
    if (!g_dataDirOverride.empty()) return g_dataDirOverride;

    wchar_t envBuf[MAX_PATH] = {};
    DWORD envLen = GetEnvironmentVariableW(L"HANCHO_LOG_DIR", envBuf, MAX_PATH);
    if (envLen > 0 && envLen < MAX_PATH) {
        return ResolveAbsolutePathWide(envBuf);
    }
    return GetDefaultLogsDirectoryWide();
}

bool HasCustomDataDirectory() {
    if (!g_dataDirOverride.empty()) return true;
    wchar_t buf[2] = {};
    return GetEnvironmentVariableW(L"HANCHO_LOG_DIR", buf, 2) > 0;
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

} // namespace pc
