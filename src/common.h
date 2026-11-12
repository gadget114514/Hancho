#pragma once

// Product version; CMake passes PROJECT_VERSION. The fallback is deliberately not a
// real version so a non-CMake build never reports a stale number.
#ifndef PC_VERSION
#define PC_VERSION "unknown"
#endif

#ifdef _WIN32
#include <windows.h>
#else
// Minimal type aliases for the remaining shared declarations. The POSIX
// implementations do not call Win32; these only keep the public structs in
// process_manager.h / ipc_server.h source-compatible.
#include <cstddef>
#include <cstdint>
using DWORD = uint32_t;
using BOOL = int;
using HANDLE = void*;
using HWND = void*;
using WPARAM = uintptr_t;
using UINT = unsigned int;
#ifndef WM_APP
#define WM_APP 0x8000
#endif
template <typename A, typename B>
inline void PostMessageW(HWND, unsigned int, A, B) {}
#ifndef STILL_ACTIVE
#define STILL_ACTIVE 259
#endif
#ifndef MAX_PATH
#define MAX_PATH 260
#endif
#endif

#include <string>
#include <vector>
#include <map>
#include <cstdint>
#include <ctime>

namespace pc {

#ifdef _WIN32
constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\Hancho_IPC_v1";
constexpr const wchar_t* kMutexName = L"Local\\Hancho_GUI_Mutex_v1";
#else
constexpr const wchar_t* kPipeName = L"/tmp/Hancho_IPC_v1.sock";
constexpr const wchar_t* kMutexName = L"Hancho_GUI_Mutex_v1";
#endif

enum class ExecMode {
    Pipe = 0,     // Anonymous pipes (non-interactive / standard CLI)
    ConPTY = 1,   // Windows Pseudo Console (interactive TUI: opencode, claude)
    Console = 2   // Separate visible console window (CREATE_NEW_CONSOLE)
};

enum class ProcState {
    Running = 0,
    Completed = 1,
    Failed = 2,
    TimedOut = 3,
    IdleTimedOut = 4,
    Killed = 5
};

std::wstring Utf8ToWide(const std::string& utf8);
std::string WideToUtf8(const std::wstring& wide);

// Absolute path / current-directory helpers
std::wstring GetCurrentDirectoryWide();
std::wstring ResolveAbsolutePathWide(const std::wstring& path);
std::string ResolveAbsolutePathUtf8(const std::string& path);

// Portable filesystem helpers. Windows keeps its Win32 semantics; POSIX maps
// backslashes to the native separator before touching the filesystem.
#ifdef _WIN32
inline void SleepMs(uint32_t ms) { ::Sleep(ms); }
#else
void SleepMs(uint32_t ms);
#endif
void EnsureDirectoryWide(const std::wstring& dir);
bool PathExistsWide(const std::wstring& path);
bool IsDirectoryWide(const std::wstring& path);
bool RemoveFileWide(const std::wstring& path);
std::vector<std::wstring> ListDirectoryWide(const std::wstring& dir,
                                            const std::wstring& pattern = L"*");
// File mtime in 100 ns units since 1601-01-01, the same clock the behaviour
// tree uses for filesNewer comparisons.
bool GetFileMtimeWide(const std::wstring& path, uint64_t& outFiletime);
uint64_t GetSystemTimeAsFiletime100ns();
std::string GetTempDirectoryUtf8();
std::string GetExecutablePathUtf8();
uint32_t GetCurrentProcessIdPortable();
// Unix domain socket on POSIX, Named Pipe name on Windows.
std::string GetIpcEndpointUtf8();
bool WideEqualsIgnoreCase(const std::wstring& a, const std::wstring& b);
std::wstring ToNativePathWide(const std::wstring& path);
std::wstring PathJoinWide(const std::wstring& left, const std::wstring& right);

// Write one hancho.event/1 line to HANCHO_EVENT_PIPE. The endpoint is a
// Named Pipe on Windows and a Unix domain socket on POSIX.
bool WriteHanchoEventLine(const std::string& endpoint, const std::string& line,
                          std::string& outError);

std::string ExecModeToString(ExecMode mode);
ExecMode StringToExecMode(const std::string& s);
std::string ProcStateToString(ProcState state);
ProcState StringToProcState(const std::string& s);
std::wstring ProcStateToDisplayWide(ProcState state, bool isIdleWarning);

// Strip ANSI/VT100 escape codes from ConPTY output for clean display & LLM reading
std::string StripAnsiCodes(const std::string& input);

// Trims leading and trailing whitespace characters (space, tab, cr, lf)
std::string Trim(const std::string& s);

inline bool LocalTimeFromSec(const time_t* t, std::tm* out) {
#ifdef _WIN32
    return localtime_s(out, t) == 0;
#else
    return localtime_r(t, out) != nullptr;
#endif
}

// Normalize line endings (\r\n) for Win32 Edit control display
std::wstring NormalizeCrLfWide(const std::string& utf8Clean);

// Extract last meaningful non-empty line for dashboard preview
std::string GetLastNonEmptyLine(const std::string& text, size_t maxLen = 120);

// Tail helper: return last N lines of text
std::string GetTailLines(const std::string& text, int maxLines);

// Simple JSON escaping and lightweight key-value extraction for IPC & MCP
std::string JsonEscape(const std::string& s);
std::string JsonGetString(const std::string& json, const std::string& key, const std::string& defaultVal = "");
int64_t JsonGetInt(const std::string& json, const std::string& key, int64_t defaultVal = 0);
bool JsonGetBool(const std::string& json, const std::string& key, bool defaultVal = false);

uint64_t GetNowTickMs();
std::string GetCurrentTimestampStr();
int64_t ParseTimestampStrToEpoch(const std::string& s);
// Parses a relative duration such as "30m", "6h" or "2d" into seconds.
// Returns false for empty, malformed, or negative input.
bool ParseDurationToSec(const std::string& s, int64_t& outSec);
std::string GetFileTimestampStr();
std::string FormatDurationSec(uint64_t ms);
std::string FormatUptimeHMS(uint64_t ms);
std::string FormatBytes(uint64_t bytes);

// Persistent logging helpers (used by MCP, CLI, IPC, and ProcessManager)
std::wstring GetLogsDirectoryWide();
std::string GetLogsDirectoryUtf8();

// Data directory override: logs, state, task queues and config all live here.
// Set from --data-dir (or HANCHO_LOG_DIR) before starting IPC/GUI.
// A custom data directory gets its own Named Pipe and GUI mutex, so several
// projects can run independent dashboards at the same time.
void SetDataDirectoryOverride(const std::wstring& dir);
std::wstring GetDataDirectoryOverride();
bool HasCustomDataDirectory();
std::wstring GetEffectiveDataDirectoryWide();
std::wstring GetPipeNameWide();
std::wstring GetGuiMutexNameWide();
std::string SanitizeFileNameComponent(const std::string& s);
void AppendToLogFileWide(const std::wstring& filePath, const std::string& data);
void WriteFileAtomicWide(const std::wstring& filePath, const std::string& data);
std::string ReadFileUtf8Wide(const std::wstring& filePath);
void WriteAuditLog(const std::string& category, const std::string& message);

// Project data reset (hancho-cli reset). The targets are every entry directly under the
// effective data directory (taskqueues/, logs, proc_* files, config.json, ...).
std::vector<std::wstring> ListProjectDataEntriesWide();
// True while a Hancho GUI/IPC instance holds this data directory's mutex.
bool IsHanchoRunningForDataDirWide();
// Removes all entries under the data directory; the directory itself is kept.
// Returns the number of top-level entries removed; outError is set if any removal failed.
size_t ResetProjectDataWide(std::string& outError);

// Who is calling hancho-cli, for task queue job origins: a JSON object with label, client,
// session (PC_ORIGIN_SESSION or CLAUDE_CODE_SESSION_ID), host_session
// (CLAUDE_CODE_HOST_SESSION_ID), cwd and pid (CLAUDE_PID, else the parent process).
std::string BuildCallerOriginJson(const std::string& client, const std::string& label);
// The filter that means "jobs this caller added": its session id, or "pid:<n>" without one.
std::string CallerSelfOriginFilter();

} // namespace pc
