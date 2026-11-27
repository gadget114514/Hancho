#pragma once

// Command lines for CreateProcessW on Windows.
//
// CreateProcess starts .exe / .com files only. npm installs tools such as `claude`, `opencode`
// and `codex` as batch shims (claude.cmd, opencode.cmd): a bare `claude -p ...` then fails with
// "failed to launch engine command" even though the shell runs it fine. WinCommandLineForCreate
// finds out whether the program is a batch shim (through PATHEXT, the same order cmd.exe uses)
// and wraps such a command line in `cmd.exe /d /s /c "..."`. Everything else is returned as is.

#include <cwctype>
#include <functional>
#include <string>

#ifdef _WIN32
#include <windows.h>
#endif

namespace pc {

// Program token of a command line: the text inside the quotes when it starts with a quote,
// otherwise the text up to the first space or tab. Empty when the line is empty.
inline std::wstring WinCommandProgramToken(const std::wstring& cmd) {
    size_t start = cmd.find_first_not_of(L" \t");
    if (start == std::wstring::npos) return L"";
    if (cmd[start] == L'"') {
        size_t end = cmd.find(L'"', start + 1);
        if (end == std::wstring::npos) return cmd.substr(start + 1);
        return cmd.substr(start + 1, end - start - 1);
    }
    size_t end = cmd.find_first_of(L" \t", start);
    return (end == std::wstring::npos) ? cmd.substr(start) : cmd.substr(start, end - start);
}

// Lower-cased extension of the last path component, including the dot ("" when none).
inline std::wstring WinPathExtension(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return L"";
    if (slash != std::wstring::npos && dot < slash) return L"";
    std::wstring ext = path.substr(dot);
    for (wchar_t& ch : ext) ch = static_cast<wchar_t>(std::towlower(ch));
    return ext;
}

inline bool WinIsBatchExtension(const std::wstring& ext) {
    return ext == L".cmd" || ext == L".bat";
}

// finder(name, ext) returns the full path that the Windows search finds for name with the
// extension ext appended (ext may be empty), or "" when there is none.
using WinCommandFinder = std::function<std::wstring(const std::wstring& name, const std::wstring& ext)>;

inline std::wstring WinWrapBatchCommand(const std::wstring& cmd) {
    return L"cmd.exe /d /s /c \"" + cmd + L"\"";
}

// The command line to pass to CreateProcessW for cmd. See the header comment.
inline std::wstring WinCommandLineForCreate(const std::wstring& cmd, const WinCommandFinder& finder) {
    std::wstring prog = WinCommandProgramToken(cmd);
    if (prog.empty()) return cmd;

    const std::wstring ext = WinPathExtension(prog);
    if (!ext.empty()) {
        return WinIsBatchExtension(ext) ? WinWrapBatchCommand(cmd) : cmd;
    }

    // No extension: follow PATHEXT order (.COM, .EXE, .BAT, .CMD); the first hit decides.
    for (const wchar_t* candidate : {L".com", L".exe", L".bat", L".cmd"}) {
        const std::wstring found = finder(prog, candidate);
        if (found.empty()) continue;
        return WinIsBatchExtension(candidate) ? WinWrapBatchCommand(cmd) : cmd;
    }
    return cmd;
}

#ifdef _WIN32
// Default finder: the Win32 search over PATH (SearchPathW appends the extension itself).
inline std::wstring WinSearchPathFinder(const std::wstring& name, const std::wstring& ext) {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = SearchPathW(nullptr, name.c_str(), ext.empty() ? nullptr : ext.c_str(), MAX_PATH, buf, nullptr);
    if (n == 0 || n >= MAX_PATH) return L"";
    return buf;
}

inline std::wstring WinCommandLineForCreate(const std::wstring& cmd) {
    return WinCommandLineForCreate(cmd, WinSearchPathFinder);
}
#endif

} // namespace pc
