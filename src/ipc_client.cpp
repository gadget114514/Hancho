#include "ipc_client.h"
#include "common.h"
#include <wincrypt.h>

namespace pc {

// How long a client waits for a starting core's pipe. A core restoring many jobs can
// take well over the old 4 seconds, and giving up early made every caller spawn yet
// another `Hancho.exe gui`, which piled up processes until the machine hung.
static const int kCoreStartWaitMs = 30000;
// After an auto-start fails, further auto-starts are refused for this long so that
// callers that retry (MCP clients, hooks, scripts) cannot turn it into a spawn storm.
static const ULONGLONG kAutoStartCooldownMs = 60000;

static bool TryConnectPipe(HANDLE& outPipe) {
    const std::wstring pipeName = GetPipeNameWide();
    // Bounded: a pipe that stays busy must not spin this caller forever.
    for (int busyTries = 0; busyTries < 20; ++busyTries) {
        HANDLE hPipe = CreateFileW(
            pipeName.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            0,
            nullptr
        );
        if (hPipe != INVALID_HANDLE_VALUE) {
            DWORD mode = PIPE_READMODE_MESSAGE;
            SetNamedPipeHandleState(hPipe, &mode, nullptr, nullptr);
            outPipe = hPipe;
            return true;
        }
        if (GetLastError() != ERROR_PIPE_BUSY) {
            return false;
        }
        if (!WaitNamedPipeW(pipeName.c_str(), 500)) {
            return false;
        }
    }
    return false;
}

static bool WaitForPipe(HANDLE& outPipe, int timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    while (true) {
        if (TryConnectPipe(outPipe)) return true;
        if (GetTickCount64() >= deadline) return false;
        Sleep(100);
    }
}

static std::wstring AutoStartFailureMarkerPath() {
    return GetLogsDirectoryWide() + L"\\core-autostart-failed.marker";
}

// True when an auto-start failed less than kAutoStartCooldownMs ago; outRemainingMs
// is how long the cooldown still lasts.
static bool RecentAutoStartFailure(ULONGLONG& outRemainingMs) {
    WIN32_FILE_ATTRIBUTE_DATA fa = {};
    if (!GetFileAttributesExW(AutoStartFailureMarkerPath().c_str(), GetFileExInfoStandard, &fa)) {
        return false;
    }
    FILETIME nowFt = {};
    GetSystemTimeAsFileTime(&nowFt);
    ULARGE_INTEGER then, now;
    then.LowPart = fa.ftLastWriteTime.dwLowDateTime;
    then.HighPart = fa.ftLastWriteTime.dwHighDateTime;
    now.LowPart = nowFt.dwLowDateTime;
    now.HighPart = nowFt.dwHighDateTime;
    if (now.QuadPart < then.QuadPart) return false;  // clock moved back
    const ULONGLONG ageMs = (now.QuadPart - then.QuadPart) / 10000ULL;
    if (ageMs >= kAutoStartCooldownMs) return false;
    outRemainingMs = kAutoStartCooldownMs - ageMs;
    return true;
}

static void MarkAutoStartFailed() {
    HANDLE h = CreateFileW(AutoStartFailureMarkerPath().c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME nowFt = {};
    GetSystemTimeAsFileTime(&nowFt);
    SetFileTime(h, nullptr, nullptr, &nowFt);
    CloseHandle(h);
}

static void ClearAutoStartFailure() {
    DeleteFileW(AutoStartFailureMarkerPath().c_str());
}

static void LaunchGuiOutsideCallerTree();

// Resolve the GUI daemon executable. hancho-cli.exe (console edition) lives next to
// Hancho.exe, so prefer the sibling GUI binary when present.
static std::wstring ResolveGuiExePath() {
    wchar_t exePath[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exePath, MAX_PATH);

    std::wstring current = exePath;
    auto slash = current.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        std::wstring candidate = current.substr(0, slash + 1) + L"Hancho.exe";
        if (_wcsicmp(candidate.c_str(), current.c_str()) == 0) {
            return current;
        }
        DWORD attr = GetFileAttributesW(candidate.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
            return candidate;
        }
    }
    return current;
}

// Starts `Hancho.exe gui` via WMI Win32_Process.Create so that it is created by
// WmiPrvSE.exe completely outside the CLI caller's process tree, Job Object, and
// stdout/stderr pipes (preventing Claude's Bash tool from blocking).
static void LaunchGuiOutsideCallerTree() {
    std::wstring guiExePath = ResolveGuiExePath();

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

    std::wstring guiCmdLine = L"\"" + guiExePath + L"\" gui";
    if (HasCustomDataDirectory()) {
        guiCmdLine += L" --data-dir \"" + GetEffectiveDataDirectoryWide() + L"\"";
    }

    auto Base64Encode = [](const std::wstring& in) -> std::wstring {
        DWORD base64Len = 0;
        CryptBinaryToStringW(
            reinterpret_cast<const BYTE*>(in.data()),
            static_cast<DWORD>(in.size() * sizeof(wchar_t)),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            nullptr,
            &base64Len
        );
        if (base64Len == 0) return L"";
        std::wstring out(base64Len, L'\0');
        CryptBinaryToStringW(
            reinterpret_cast<const BYTE*>(in.data()),
            static_cast<DWORD>(in.size() * sizeof(wchar_t)),
            CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF,
            &out[0],
            &base64Len
        );
        // CryptBinaryToStringW includes the null terminator in base64Len when writing to buffer
        if (base64Len > 0 && out[base64Len - 1] == L'\0') {
            out.resize(base64Len - 1);
        }
        // However, the returned base64Len from the second call doesn't include the null terminator
        // on some Windows versions, so we use string's c_str() to find the real length.
        out.resize(wcslen(out.c_str()));
        return out;
    };

    std::wstring b64CmdLine = Base64Encode(guiCmdLine);

    // Win32_Process.Create gives a console-subsystem GUI a new console window when no
    // startup information is supplied, so a grandchild `Hancho.exe gui` flashes a
    // terminal at the user. Create that console hidden instead (SW_HIDE).
    std::wstring psScript = L"$cmd = [System.Text.Encoding]::Unicode.GetString([System.Convert]::FromBase64String('";
    psScript += b64CmdLine;
    psScript += L"')); "
                L"try { "
                L"$si = New-CimInstance -ClassName Win32_ProcessStartup -ClientOnly "
                L"-Property @{CreateFlags=[uint32]0x10; ShowWindow=[uint16]0}; "
                L"$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create "
                L"-Arguments @{CommandLine=$cmd; ProcessStartupInformation=$si}; "
                L"if ($r.ReturnValue -ne 0) { "
                L"Invoke-CimMethod -ClassName Win32_Process -MethodName Create "
                L"-Arguments @{CommandLine=$cmd} | Out-Null } "
                L"} catch { "
                L"Invoke-CimMethod -ClassName Win32_Process -MethodName Create "
                L"-Arguments @{CommandLine=$cmd} | Out-Null }";

    std::wstring b64Script = Base64Encode(psScript);

    wchar_t psPath[MAX_PATH] = {};
    ExpandEnvironmentStringsW(L"%SystemRoot%\\System32\\WindowsPowerShell\\v1.0\\powershell.exe",
                              psPath, MAX_PATH);

    std::wstring wmiCmd = psPath;
    wmiCmd += L" -NoLogo -NoProfile -NonInteractive -WindowStyle Hidden -EncodedCommand ";
    wmiCmd += b64Script;

    std::vector<wchar_t> wmiBuf(wmiCmd.begin(), wmiCmd.end());
    wmiBuf.push_back(L'\0');

    if (CreateProcessW(nullptr, wmiBuf.data(), nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 4000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

// Connects to the core, starting it at most once across all concurrent callers.
static bool ConnectOrAutoStart(HANDLE& hPipe, std::string& outError) {
    // A core that already holds the instance mutex is still starting: wait for its
    // pipe and never launch a second one.
    if (IsHanchoRunningForDataDirWide()) {
        if (WaitForPipe(hPipe, kCoreStartWaitMs)) return true;
        outError = "Hancho core is running but did not open its IPC pipe within " +
                   std::to_string(kCoreStartWaitMs / 1000) + " seconds (still starting or stuck).";
        return false;
    }

    ULONGLONG remainingMs = 0;
    if (RecentAutoStartFailure(remainingMs)) {
        outError = "Hancho core failed to start recently; not starting it again for " +
                   std::to_string((remainingMs + 999) / 1000) +
                   " seconds. Start it manually with: Hancho.exe --headless";
        return false;
    }

    // Only one client launches the core; the others wait for the pipe it opens.
    const std::wstring startMutexName = GetGuiMutexNameWide() + L"_autostart";
    HANDLE startMutex = CreateMutexW(nullptr, FALSE, startMutexName.c_str());
    DWORD waitResult = startMutex ? WaitForSingleObject(startMutex, 0) : WAIT_FAILED;
    const bool owner = (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED);

    bool connected = false;
    if (!owner) {
        connected = WaitForPipe(hPipe, kCoreStartWaitMs + 10000);
    } else {
        // Someone may have finished starting it while we took the mutex.
        if (TryConnectPipe(hPipe)) {
            connected = true;
        } else if (IsHanchoRunningForDataDirWide()) {
            connected = WaitForPipe(hPipe, kCoreStartWaitMs);
        } else {
            LaunchGuiOutsideCallerTree();
            connected = WaitForPipe(hPipe, kCoreStartWaitMs + 5000);
            if (connected) ClearAutoStartFailure();
            else MarkAutoStartFailed();
        }
        ReleaseMutex(startMutex);
    }
    if (startMutex) CloseHandle(startMutex);

    if (!connected) {
        outError = "Failed to connect to Hancho Named Pipe server after auto-starting GUI.";
    }
    return connected;
}

bool SendIpcRequest(const std::string& reqJson, std::string& outRespJson, std::string& outError, bool autoStartGui) {
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    if (!TryConnectPipe(hPipe)) {
        if (!autoStartGui) {
            outError = "Hancho GUI daemon is not running.";
            return false;
        }
        if (!ConnectOrAutoStart(hPipe, outError)) return false;
    }

    DWORD written = 0;
    if (!WriteFile(hPipe, reqJson.data(), static_cast<DWORD>(reqJson.size()), &written, nullptr)) {
        CloseHandle(hPipe);
        outError = "WriteFile to Named Pipe failed.";
        return false;
    }

    outRespJson.clear();
    char buf[16384];
    DWORD bytesRead = 0;
    while (true) {
        BOOL ok = ReadFile(hPipe, buf, sizeof(buf), &bytesRead, nullptr);
        if (bytesRead > 0) {
            outRespJson.append(buf, bytesRead);
        }
        if (ok) break;
        if (GetLastError() != ERROR_MORE_DATA) break;
    }

    CloseHandle(hPipe);
    if (outRespJson.empty()) {
        outError = "Named Pipe read returned empty response (GetLastError=" + std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
}

} // namespace pc
