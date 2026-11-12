// Unit / smoke test for GUI platform abstraction layer (Card 060-4 / 060d).
//
// Verifies that OS-dependent desktop calls (clipboard, shell open, message boxes,
// file dialogs, tray, audio) can be intercepted and mocked cleanly without
// triggering native OS GUI popups or requiring an active desktop session.

#include "gui_platform.h"
#include <cstdio>
#include <string>
#include <vector>

using namespace pc;
using namespace pc::gui_platform;

namespace {

int g_passes = 0;
int g_failures = 0;

void Check(bool ok, const std::string& msg) {
    if (ok) {
        ++g_passes;
    } else {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
    }
}

class TestMockGuiPlatformServices : public IGuiPlatformServices {
public:
    std::wstring lastClipboardText;
    std::wstring lastOpenedFile;
    std::wstring lastOpenedFolder;
    std::wstring lastMsgBoxTitle;
    std::wstring lastMsgBoxMessage;
    MessageBoxType lastMsgBoxType = MessageBoxType::Info;
    MessageBoxResult msgBoxResultToReturn = MessageBoxResult::Yes;
    std::wstring pickFileResultToReturn = L"C:\\test\\custom.wav";
    bool trayAddCalled = false;
    bool trayDeleteCalled = false;
    std::wstring lastBalloonTitle;
    std::wstring lastBalloonMsg;
    bool lastBalloonWarning = false;
    std::wstring lastSoundPath;
    bool lastSoundFailed = false;

    bool CopyToClipboard(HWND, const std::wstring& text) override {
        lastClipboardText = text;
        return true;
    }

    bool OpenFileInEditor(HWND, const std::wstring& filePath) override {
        lastOpenedFile = filePath;
        return true;
    }

    bool OpenFolder(HWND, const std::wstring& folderPath) override {
        lastOpenedFolder = folderPath;
        return true;
    }

    MessageBoxResult ShowMessageBox(HWND, const std::wstring& title,
                                    const std::wstring& message,
                                    MessageBoxType type,
                                    bool /*defaultNo*/) override {
        lastMsgBoxTitle = title;
        lastMsgBoxMessage = message;
        lastMsgBoxType = type;
        return msgBoxResultToReturn;
    }

    bool PickOpenFileName(HWND, const std::wstring& /*filter*/,
                          std::wstring& outPath,
                          const std::wstring& /*defaultPath*/) override {
        outPath = pickFileResultToReturn;
        return true;
    }

    bool TrayAdd(NOTIFYICONDATAW& /*nid*/) override {
        trayAddCalled = true;
        return true;
    }

    bool TrayDelete(NOTIFYICONDATAW& /*nid*/) override {
        trayDeleteCalled = true;
        return true;
    }

    bool TrayShowBalloon(NOTIFYICONDATAW& /*nid*/,
                         const std::wstring& title,
                         const std::wstring& message,
                         bool isWarning) override {
        lastBalloonTitle = title;
        lastBalloonMsg = message;
        lastBalloonWarning = isWarning;
        return true;
    }

    bool PlayNotificationSound(const std::wstring& customPath, bool isFailed) override {
        lastSoundPath = customPath;
        lastSoundFailed = isFailed;
        return true;
    }
};

} // namespace

int main() {
    // 1. Initial default instance
    IGuiPlatformServices* def = GetGuiPlatform();
    Check(def != nullptr, "Default GUI platform instance is non-null");

    // 2. Install mock implementation
    auto mock = std::make_shared<TestMockGuiPlatformServices>();
    SetGuiPlatform(mock);
    Check(GetGuiPlatform() == mock.get(), "Installed custom GUI platform instance");

    // 3. Test clipboard redirection
    GetGuiPlatform()->CopyToClipboard(nullptr, L"hello platform clipboard");
    Check(mock->lastClipboardText == L"hello platform clipboard", "Clipboard routed to platform layer");

    // 4. Test editor & folder opening
    GetGuiPlatform()->OpenFileInEditor(nullptr, L"C:\\temp\\log.txt");
    Check(mock->lastOpenedFile == L"C:\\temp\\log.txt", "OpenFileInEditor routed to platform layer");

    GetGuiPlatform()->OpenFolder(nullptr, L"C:\\data\\folder");
    Check(mock->lastOpenedFolder == L"C:\\data\\folder", "OpenFolder routed to platform layer");

    // 5. Test message box
    mock->msgBoxResultToReturn = MessageBoxResult::Yes;
    MessageBoxResult res = GetGuiPlatform()->ShowMessageBox(nullptr, L"Confirm Title", L"Confirm Action?",
                                                            MessageBoxType::QuestionYesNo, true);
    Check(res == MessageBoxResult::Yes, "ShowMessageBox returned expected result");
    Check(mock->lastMsgBoxTitle == L"Confirm Title", "ShowMessageBox routed title");
    Check(mock->lastMsgBoxMessage == L"Confirm Action?", "ShowMessageBox routed message");
    Check(mock->lastMsgBoxType == MessageBoxType::QuestionYesNo, "ShowMessageBox routed type");

    // 6. Test file picker
    std::wstring picked;
    bool pickOk = GetGuiPlatform()->PickOpenFileName(nullptr, L"WAV (*.wav)", picked);
    Check(pickOk && picked == L"C:\\test\\custom.wav", "PickOpenFileName routed to platform layer");

    // 7. Test tray notifications
    NOTIFYICONDATAW nid = {};
    GetGuiPlatform()->TrayAdd(nid);
    Check(mock->trayAddCalled, "TrayAdd routed to platform layer");

    GetGuiPlatform()->TrayShowBalloon(nid, L"Warn Title", L"Warn Message", true);
    Check(mock->lastBalloonTitle == L"Warn Title", "TrayShowBalloon routed title");
    Check(mock->lastBalloonMsg == L"Warn Message", "TrayShowBalloon routed message");
    Check(mock->lastBalloonWarning == true, "TrayShowBalloon routed isWarning flag");

    GetGuiPlatform()->TrayDelete(nid);
    Check(mock->trayDeleteCalled, "TrayDelete routed to platform layer");

    // 8. Test audio notification
    GetGuiPlatform()->PlayNotificationSound(L"custom.wav", true);
    Check(mock->lastSoundPath == L"custom.wav", "PlayNotificationSound routed path");
    Check(mock->lastSoundFailed == true, "PlayNotificationSound routed isFailed flag");

    // 9. Reset restores default
    ResetGuiPlatform();
    Check(GetGuiPlatform() != mock.get(), "ResetGuiPlatform restored default instance");

    std::printf("%s (%d passed, %d failures)\n", g_failures ? "FAILED" : "ALL PASSED",
                g_passes, g_failures);
    return g_failures ? 1 : 0;
}
