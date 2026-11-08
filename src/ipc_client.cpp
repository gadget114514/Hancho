#include "ipc_client.h"
#include "common.h"
#include <wincrypt.h>

namespace pc {

static bool TryConnectPipe(HANDLE& outPipe) {
    const std::wstring pipeName = GetPipeNameWide();
    while (true) {
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
}

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

bool SendIpcRequest(const std::string& reqJson, std::string& outRespJson, std::string& outError, bool autoStartGui) {
    HANDLE hPipe = INVALID_HANDLE_VALUE;
    if (!TryConnectPipe(hPipe)) {
        if (!autoStartGui) {
            outError = "Hancho GUI daemon is not running.";
            return false;
        }
        // Auto-spawn GUI Dashboard process via WMI Win32_Process.Create so that it is
        // created by WmiPrvSE.exe completely outside the CLI caller's process tree,
        // Job Object, and stdout/stderr pipes (preventing Claude's Bash tool from blocking).
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

        std::wstring psScript = L"$cmd = [System.Text.Encoding]::Unicode.GetString([System.Convert]::FromBase64String('";
        psScript += b64CmdLine;
        psScript += L"')); Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{CommandLine=$cmd} | Out-Null";

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

        // Wait up to 4 seconds for GUI Named Pipe server to come online
        bool connected = false;
        for (int attempt = 0; attempt < 40; ++attempt) {
            Sleep(100);
            if (TryConnectPipe(hPipe)) {
                connected = true;
                break;
            }
        }
        if (!connected) {
            outError = "Failed to connect to Hancho Named Pipe server after auto-starting GUI.";
            return false;
        }
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
    return !outRespJson.empty();
}

} // namespace pc
