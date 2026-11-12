#pragma once

// GUI Platform Services Layer (doc/design-v3.md 2.3, 2.4, Card 060-4 / 060d)
//
// Separates non-drawing, non-IPC OS-dependent desktop facilities:
//   - System clipboard (copying text)
//   - Host shell execution (opening files in external editor, opening folders)
//   - System notifications & Tray management
//   - Dialogs & User prompts (alerts, confirmations, open file picker)
//   - Notification sounds
// from GUI dashboard state rendering and IPC client logic.
//
// Future Linux / macOS ports (e.g. Card 061 browser UI, TUI, or native ports)
// reference this interface to provide platform-equivalent features.

#include <string>
#include <memory>
#include <functional>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#include <commdlg.h>
#include <mmsystem.h>
#else
typedef void* HWND;
struct NOTIFYICONDATAW {
    void* hWnd;
    unsigned int uID;
    unsigned int uFlags;
    unsigned int uCallbackMessage;
    void* hIcon;
    wchar_t szTip[128];
    wchar_t szInfo[256];
    wchar_t szInfoTitle[64];
    unsigned long dwInfoFlags;
};
#endif

namespace pc {
namespace gui_platform {

enum class MessageBoxType {
    Info,
    Warning,
    Error,
    QuestionYesNo,
};

enum class MessageBoxResult {
    Ok,
    Yes,
    No,
    Cancel,
};

class IGuiPlatformServices {
public:
    virtual ~IGuiPlatformServices() = default;

    // Clipboard operations
    virtual bool CopyToClipboard(HWND hwnd, const std::wstring& text) = 0;

    // Shell / external application launch
    virtual bool OpenFileInEditor(HWND hwnd, const std::wstring& filePath) = 0;
    virtual bool OpenFolder(HWND hwnd, const std::wstring& folderPath) = 0;

    // User prompts and dialogs
    virtual MessageBoxResult ShowMessageBox(HWND hwnd,
                                            const std::wstring& title,
                                            const std::wstring& message,
                                            MessageBoxType type,
                                            bool defaultNo = false) = 0;
    virtual bool PickOpenFileName(HWND hwnd,
                                  const std::wstring& filter,
                                  std::wstring& outPath,
                                  const std::wstring& defaultPath = L"") = 0;

    // System Tray & Notifications
    virtual bool TrayAdd(NOTIFYICONDATAW& nid) = 0;
    virtual bool TrayDelete(NOTIFYICONDATAW& nid) = 0;
    virtual bool TrayShowBalloon(NOTIFYICONDATAW& nid,
                                 const std::wstring& title,
                                 const std::wstring& message,
                                 bool isWarning) = 0;

    // Audio / Sounds
    virtual bool PlayNotificationSound(const std::wstring& customPath, bool isFailed) = 0;
};

#ifdef _WIN32
class WindowsGuiPlatformServices : public IGuiPlatformServices {
public:
    bool CopyToClipboard(HWND hwnd, const std::wstring& text) override {
        if (!OpenClipboard(hwnd)) return false;
        EmptyClipboard();
        size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (hg) {
            void* p = GlobalLock(hg);
            if (p) {
                memcpy(p, text.c_str(), bytes);
                GlobalUnlock(hg);
                SetClipboardData(CF_UNICODETEXT, hg);
            }
        }
        CloseClipboard();
        return true;
    }

    bool OpenFileInEditor(HWND hwnd, const std::wstring& filePath) override {
        if (filePath.empty()) return false;
        HINSTANCE res = ShellExecuteW(hwnd, L"open", L"notepad.exe", filePath.c_str(), nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<intptr_t>(res) > 32;
    }

    bool OpenFolder(HWND hwnd, const std::wstring& folderPath) override {
        if (folderPath.empty()) return false;
        HINSTANCE res = ShellExecuteW(hwnd, L"open", folderPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<intptr_t>(res) > 32;
    }

    MessageBoxResult ShowMessageBox(HWND hwnd,
                                    const std::wstring& title,
                                    const std::wstring& message,
                                    MessageBoxType type,
                                    bool defaultNo = false) override {
        UINT uType = 0;
        switch (type) {
        case MessageBoxType::Info:
            uType = MB_OK | MB_ICONINFORMATION;
            break;
        case MessageBoxType::Warning:
            uType = MB_OK | MB_ICONWARNING;
            break;
        case MessageBoxType::Error:
            uType = MB_OK | MB_ICONERROR;
            break;
        case MessageBoxType::QuestionYesNo:
            uType = MB_YESNO | MB_ICONQUESTION | (defaultNo ? MB_DEFBUTTON2 : 0);
            break;
        }
        int ret = MessageBoxW(hwnd, message.c_str(), title.c_str(), uType);
        if (ret == IDYES) return MessageBoxResult::Yes;
        if (ret == IDNO) return MessageBoxResult::No;
        if (ret == IDCANCEL) return MessageBoxResult::Cancel;
        return MessageBoxResult::Ok;
    }

    bool PickOpenFileName(HWND hwnd,
                          const std::wstring& filter,
                          std::wstring& outPath,
                          const std::wstring& defaultPath = L"") override {
        wchar_t file[MAX_PATH] = {};
        if (!defaultPath.empty()) {
            wcsncpy_s(file, defaultPath.c_str(), _TRUNCATE);
        }
        OPENFILENAMEW ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = hwnd;
        ofn.lpstrFilter = filter.c_str();
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
        if (GetOpenFileNameW(&ofn)) {
            outPath = file;
            return true;
        }
        return false;
    }

    bool TrayAdd(NOTIFYICONDATAW& nid) override {
        return Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    }

    bool TrayDelete(NOTIFYICONDATAW& nid) override {
        return Shell_NotifyIconW(NIM_DELETE, &nid) != FALSE;
    }

    bool TrayShowBalloon(NOTIFYICONDATAW& nid,
                         const std::wstring& title,
                         const std::wstring& message,
                         bool isWarning) override {
        NOTIFYICONDATAW copy = nid;
        copy.uFlags |= NIF_INFO;
        copy.dwInfoFlags = isWarning ? NIIF_WARNING : NIIF_INFO;
        wcsncpy_s(copy.szInfoTitle, title.c_str(), _TRUNCATE);
        wcsncpy_s(copy.szInfo, message.c_str(), _TRUNCATE);
        return Shell_NotifyIconW(NIM_MODIFY, &copy) != FALSE;
    }

    bool PlayNotificationSound(const std::wstring& customPath, bool isFailed) override {
        if (!customPath.empty()) {
            if (PlaySoundW(customPath.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT)) {
                return true;
            }
        }
        if (isFailed) {
            if (PlaySoundW(L"SystemHand", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT)) return true;
        } else {
            if (PlaySoundW(L"Notification.Default", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT)) return true;
            if (PlaySoundW(L"SystemAsterisk", nullptr, SND_ALIAS | SND_ASYNC | SND_NODEFAULT)) return true;
        }
        return false;
    }
};
#endif

// Registry / active instance accessors
inline std::shared_ptr<IGuiPlatformServices>& GetGuiPlatformInstance() {
    static std::shared_ptr<IGuiPlatformServices> s_instance;
    return s_instance;
}

inline IGuiPlatformServices* GetGuiPlatform() {
    auto& instance = GetGuiPlatformInstance();
    if (!instance) {
#ifdef _WIN32
        instance = std::make_shared<WindowsGuiPlatformServices>();
#endif
    }
    return instance.get();
}

inline void SetGuiPlatform(std::shared_ptr<IGuiPlatformServices> platform) {
    GetGuiPlatformInstance() = std::move(platform);
}

inline void ResetGuiPlatform() {
    GetGuiPlatformInstance().reset();
}

} // namespace gui_platform
} // namespace pc
