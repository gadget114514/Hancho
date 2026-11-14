#include "term_client.h"

#include "common.h"

#include <windows.h>

#include <string>
#include <vector>

namespace pc {

bool LaunchTerminalClientWindow() {
    const std::wstring windowClass = TerminalClientWindowClass();
    HWND existing = FindWindowW(windowClass.c_str(), nullptr);
    if (existing) {
        if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
        else ShowWindow(existing, SW_SHOW);
        SetForegroundWindow(existing);
        return true;
    }

    wchar_t exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;

    // HanchoTerm.exe sits next to Hancho.exe in the install/output directory.
    std::wstring termPath(exePath, len);
    const size_t slash = termPath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    termPath.resize(slash + 1);
    termPath += L"HanchoTerm.exe";

    std::wstring command = L'"' + termPath + L'"';
    const std::wstring dataDir = GetDataDirectoryOverride();
    if (!dataDir.empty()) {
        command += L" --data-dir \"";
        command += dataDir;
        command += L'"';
    }
    std::vector<wchar_t> cmdBuf(command.begin(), command.end());
    cmdBuf.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(termPath.c_str(), cmdBuf.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace pc
