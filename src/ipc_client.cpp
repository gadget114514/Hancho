#include "ipc_client.h"
#include "common.h"
#include <wincrypt.h>
#include <atomic>
#include <cctype>

namespace pc {

static std::atomic<bool> g_noAutoStart{false};

void SetNoAutoStart(bool noAutoStart) {
    g_noAutoStart.store(noAutoStart);
}

bool GetNoAutoStart() {
    return g_noAutoStart.load();
}

static std::string ReadEnvUtf8(const wchar_t* name) {
    wchar_t buf[512] = {};
    DWORD len = GetEnvironmentVariableW(name, buf, 512);
    if (len == 0 || len >= 512) return "";
    return WideToUtf8(std::wstring(buf, len));
}

bool HasAutoStartEnvVar() {
    return !Trim(ReadEnvUtf8(L"HANCHO_AUTOSTART")).empty();
}

bool ShouldSuppressDialogs() {
    if (HasAutoStartEnvVar()) return true;
    return Trim(ReadEnvUtf8(L"HANCHO_NO_DIALOGS")) == "1";
}

AutoStartMode ResolveAutoStartMode(bool noAutoStartFlag, const char* envValue) {
    if (noAutoStartFlag) return AutoStartMode::Off;
    std::string val = envValue ? std::string(envValue) : ReadEnvUtf8(L"HANCHO_AUTOSTART");
    val = Trim(val);
    for (char& ch : val) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    if (val == "headless") return AutoStartMode::Headless;
    if (val == "off") return AutoStartMode::Off;
    return AutoStartMode::Gui;
}

// How long a client waits for a starting core's pipe. A core restoring many jobs can
// take well over the old 4 seconds, and giving up early made every caller spawn yet
// another `Hancho.exe gui`, which piled up processes until the machine hung.
static const int kCoreStartWaitMs = 30000;
// After an auto-start fails, further auto-starts are refused for this long so that
// callers that retry (MCP clients, hooks, scripts) cannot turn it into a spawn storm.
static const ULONGLONG kAutoStartCooldownMs = 60000;

static bool TryConnectPipe(HANDLE& outPipe, DWORD openFlags) {
    const std::wstring pipeName = GetPipeNameWide();
    // Bounded: a pipe that stays busy must not spin this caller forever.
    for (int busyTries = 0; busyTries < 20; ++busyTries) {
        HANDLE hPipe = CreateFileW(
            pipeName.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            0,
            nullptr,
            OPEN_EXISTING,
            openFlags,
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

static bool WaitForPipe(HANDLE& outPipe, int timeoutMs, DWORD openFlags) {
    const ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutMs);
    while (true) {
        if (TryConnectPipe(outPipe, openFlags)) return true;
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

static void LaunchOutsideCallerTree(const std::wstring& targetCmdLine);
static void LaunchGuiOutsideCallerTree();
static void LaunchHeadlessOutsideCallerTree();

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

static std::wstring ResolveCoreExePath() {
    const std::wstring hanchoExe = ResolveGuiExePath();
    auto slash = hanchoExe.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return hanchoExe;
    const std::wstring coreExe = hanchoExe.substr(0, slash + 1) + L"HanchoCore.exe";
    WIN32_FILE_ATTRIBUTE_DATA a = {}, b = {};
    const bool haveCore = GetFileAttributesExW(coreExe.c_str(), GetFileExInfoStandard, &a) != FALSE;
    const bool haveHancho = GetFileAttributesExW(hanchoExe.c_str(), GetFileExInfoStandard, &b) != FALSE;
    if (haveCore && haveHancho &&
        a.nFileSizeHigh == b.nFileSizeHigh && a.nFileSizeLow == b.nFileSizeLow &&
        a.ftLastWriteTime.dwHighDateTime == b.ftLastWriteTime.dwHighDateTime &&
        a.ftLastWriteTime.dwLowDateTime == b.ftLastWriteTime.dwLowDateTime) {
        return coreExe;
    }
    if (haveHancho) {
        DeleteFileW(coreExe.c_str());
        if (CopyFileW(hanchoExe.c_str(), coreExe.c_str(), FALSE)) {
            return coreExe;
        }
    }
    if (haveCore) return coreExe;
    return hanchoExe;
}

// Starts a command via WMI Win32_Process.Create so that it is created by
// WmiPrvSE.exe completely outside the CLI caller's process tree, Job Object, and
// stdout/stderr pipes (preventing Claude's Bash tool from blocking).
static void LaunchOutsideCallerTree(const std::wstring& targetCmdLine) {
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};

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

    std::wstring b64CmdLine = Base64Encode(targetCmdLine);

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

static void LaunchGuiOutsideCallerTree() {
    std::wstring guiExePath = ResolveGuiExePath();
    std::wstring guiCmdLine = L"\"" + guiExePath + L"\" gui";
    if (HasCustomDataDirectory()) {
        guiCmdLine += L" --data-dir \"" + GetEffectiveDataDirectoryWide() + L"\"";
    }
    LaunchOutsideCallerTree(guiCmdLine);
}

static void LaunchHeadlessOutsideCallerTree() {
    std::wstring coreExePath = ResolveCoreExePath();
    std::wstring coreCmdLine = L"\"" + coreExePath + L"\" --headless";
    if (HasCustomDataDirectory()) {
        coreCmdLine += L" --data-dir \"" + GetEffectiveDataDirectoryWide() + L"\"";
    }
    LaunchOutsideCallerTree(coreCmdLine);
}

// Connects to the core, starting it at most once across all concurrent callers.
static bool ConnectOrAutoStart(HANDLE& hPipe, std::string& outError, DWORD openFlags) {
    // A core that already holds the instance mutex is still starting: wait for its
    // pipe and never launch a second one.
    if (IsHanchoRunningForDataDirWide()) {
        if (WaitForPipe(hPipe, kCoreStartWaitMs, openFlags)) return true;
        outError = "Hancho core is running but did not open its IPC pipe within " +
                   std::to_string(kCoreStartWaitMs / 1000) + " seconds (still starting or stuck).";
        return false;
    }

    const AutoStartMode mode = ResolveAutoStartMode();
    if (mode == AutoStartMode::Off) {
        outError = "Hancho core is not running (auto-start disabled). Start it with: Hancho.exe --headless";
        if (HasCustomDataDirectory()) {
            outError += " --data-dir \"" + WideToUtf8(GetEffectiveDataDirectoryWide()) + "\"";
        }
        return false;
    }

    if (!HasAutoStartEnvVar()) {
        ULONGLONG remainingMs = 0;
        if (RecentAutoStartFailure(remainingMs)) {
            outError = "Hancho core failed to start recently; not starting it again for " +
                       std::to_string((remainingMs + 999) / 1000) +
                       " seconds. Start it manually with: Hancho.exe --headless";
            return false;
        }
    }

    // Only one client launches the core; the others wait for the pipe it opens.
    const std::wstring startMutexName = GetGuiMutexNameWide() + L"_autostart";
    HANDLE startMutex = CreateMutexW(nullptr, FALSE, startMutexName.c_str());
    DWORD waitResult = startMutex ? WaitForSingleObject(startMutex, 0) : WAIT_FAILED;
    const bool owner = (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_ABANDONED);

    bool connected = false;
    if (!owner) {
        connected = WaitForPipe(hPipe, kCoreStartWaitMs + 10000, openFlags);
    } else {
        // Someone may have finished starting it while we took the mutex.
        if (TryConnectPipe(hPipe, openFlags)) {
            connected = true;
        } else if (IsHanchoRunningForDataDirWide()) {
            connected = WaitForPipe(hPipe, kCoreStartWaitMs, openFlags);
        } else {
            if (mode == AutoStartMode::Headless) {
                LaunchHeadlessOutsideCallerTree();
            } else {
                LaunchGuiOutsideCallerTree();
            }
            connected = WaitForPipe(hPipe, kCoreStartWaitMs + 5000, openFlags);
            if (connected) ClearAutoStartFailure();
            else MarkAutoStartFailed();
        }
        ReleaseMutex(startMutex);
    }
    if (startMutex) CloseHandle(startMutex);

    if (!connected) {
        outError = (mode == AutoStartMode::Headless)
            ? "Failed to connect to Hancho Named Pipe server after auto-starting headless core."
            : "Failed to connect to Hancho Named Pipe server after auto-starting GUI.";
    }
    return connected;
}

bool SendIpcRequest(const std::string& reqJson, std::string& outRespJson, std::string& outError,
                    bool autoStartGui, int timeoutMs, bool* outTimedOut) {
    if (outTimedOut) *outTimedOut = false;
    // Bounded calls open the pipe in overlapped mode so the answer is awaited on
    // an event instead of polling PeekNamedPipe with Sleep(10).
    const DWORD openFlags = (timeoutMs > 0) ? FILE_FLAG_OVERLAPPED : 0;
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    if (!TryConnectPipe(hPipe, openFlags)) {
        if (!autoStartGui) {
            outError = "Hancho GUI daemon is not running.";
            return false;
        }
        if (!ConnectOrAutoStart(hPipe, outError, openFlags)) return false;
    }

    OVERLAPPED writeOv = {};
    writeOv.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (writeOv.hEvent == nullptr) {
        CloseHandle(hPipe);
        outError = "CreateEventW for overlapped pipe write failed.";
        return false;
    }
    // Ask for a compact msgpack + deflate response. Old cores ignore the unknown fields
    // and answer with plain JSON, which the caller also accepts after IpcDecodeResponse.
    // An empty object gets no trailing comma, which would make the request invalid JSON.
    std::string wireReq = reqJson;
    const size_t open = reqJson.find_first_not_of(" \t\r\n");
    if (open != std::string::npos && reqJson[open] == '{') {
        const size_t next = reqJson.find_first_not_of(" \t\r\n", open + 1);
        const bool emptyObject = next == std::string::npos || reqJson[next] == '}';
        wireReq = "{\"encoding\":\"msgpack\",\"compression\":\"deflate\"" +
                  std::string(emptyObject ? "" : ",") + reqJson.substr(open + 1);
    }
    DWORD written = 0;
    BOOL writeOk = WriteFile(hPipe, wireReq.data(), static_cast<DWORD>(wireReq.size()), &written, &writeOv);
    if (!writeOk && GetLastError() == ERROR_IO_PENDING) {
        writeOk = GetOverlappedResult(hPipe, &writeOv, &written, TRUE);
    }
    CloseHandle(writeOv.hEvent);
    if (!writeOk) {
        CloseHandle(hPipe);
        outError = "WriteFile to Named Pipe failed.";
        return false;
    }

    OVERLAPPED readOv = {};
    readOv.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (readOv.hEvent == nullptr) {
        CloseHandle(hPipe);
        outError = "CreateEventW for overlapped pipe read failed.";
        return false;
    }

    outRespJson.clear();
    char buf[16384];
    bool firstRead = true;
    while (true) {
        OVERLAPPED ov = {};
        ov.hEvent = readOv.hEvent;
        ResetEvent(readOv.hEvent);
        DWORD bytesRead = 0;
        BOOL ok = ReadFile(hPipe, buf, sizeof(buf), &bytesRead, &ov);
        DWORD err = ok ? ERROR_SUCCESS : GetLastError();
        if (!ok && err == ERROR_IO_PENDING) {
            // Only the first read is bounded: a slow core must not hold the
            // caller forever, but once it answered, later chunks can wait.
            const DWORD waitMs = (firstRead && timeoutMs > 0)
                ? static_cast<DWORD>(timeoutMs) : INFINITE;
            const DWORD waitResult = WaitForSingleObject(ov.hEvent, waitMs);
            if (waitResult == WAIT_TIMEOUT) {
                CancelIoEx(hPipe, &ov);
                // Drain the cancelled operation before its OVERLAPPED dies.
                GetOverlappedResult(hPipe, &ov, &bytesRead, TRUE);
                CloseHandle(readOv.hEvent);
                CloseHandle(hPipe);
                outError = "The core did not answer within " + std::to_string(timeoutMs) + " ms.";
                if (outTimedOut) *outTimedOut = true;
                return false;
            }
            ok = GetOverlappedResult(hPipe, &ov, &bytesRead, FALSE);
            err = ok ? ERROR_SUCCESS : GetLastError();
        } else if (!ok && err == ERROR_MORE_DATA) {
            // An overlapped read that completes immediately with more data pending still
            // carries its byte count in the OVERLAPPED result. Skipping this call silently
            // drops the chunk (typically the head of a large response).
            ok = GetOverlappedResult(hPipe, &ov, &bytesRead, FALSE);
            err = ok ? ERROR_SUCCESS : GetLastError();
        } else if (!ok && err != ERROR_BROKEN_PIPE) {
            CloseHandle(readOv.hEvent);
            CloseHandle(hPipe);
            outError = "ReadFile from Named Pipe failed (GetLastError=" + std::to_string(err) + ")";
            return false;
        }
        firstRead = false;
        if (bytesRead > 0) outRespJson.append(buf, bytesRead);
        if (err == ERROR_MORE_DATA) continue;
        if (ok || err == ERROR_BROKEN_PIPE) break;
        if (!ok) break;
    }
    CloseHandle(readOv.hEvent);
    CloseHandle(hPipe);

    if (outRespJson.empty()) {
        outError = "Named Pipe read returned empty response (GetLastError=" + std::to_string(GetLastError()) + ")";
        return false;
    }
    if (!IpcDecodeResponse(outRespJson, outRespJson)) {
        outError = "IPC response could not be decoded.";
        return false;
    }
    return true;
}

} // namespace pc
