#include "term_client.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <commctrl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include "common.h"
#include "ipc_client.h"
#include "gui_theme.h"
#include "TerminalView.h"
#include "wcsp/remote_session.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <memory>
#include <set>
#include <string>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace pc {
namespace {

constexpr wchar_t kConnectDlgClass[] = L"HanchoTermConnectDlg";
constexpr wchar_t kAttachDlgClass[] = L"HanchoTermAttachDlg";

// One client window per data directory, so two Hancho instances with different
// --data-dir values do not focus each other's terminal client.
const std::wstring& ClientWindowClass() {
    static const std::wstring name = [] {
        uint64_t hash = 1469598103934665603ULL;
        for (wchar_t c : GetEffectiveDataDirectoryWide()) {
            hash ^= static_cast<uint64_t>(c);
            hash *= 1099511628211ULL;
        }
        wchar_t buf[64] = {};
        std::swprintf(buf, 64, L"HanchoTerminalClientWnd_%016llx",
                      static_cast<unsigned long long>(hash));
        return std::wstring(buf);
    }();
    return name;
}

constexpr UINT WM_CLIENT_SESSION_EXIT = WM_APP + 21;
constexpr UINT WM_CLIENT_SESSION_ERROR = WM_APP + 22;

constexpr int IDC_CLIENT_TAB = 5001;
constexpr int IDC_CLIENT_BTN_NEW = 5002;
constexpr int IDC_CLIENT_BTN_ATTACH = 5003;
constexpr int IDC_CLIENT_BTN_CONNECT = 5004;
constexpr int IDC_CLIENT_BTN_CLOSE = 5005;
constexpr int IDC_CLIENT_STATUS = 5006;

constexpr int IDM_NEW_PWSH = 5101;
constexpr int IDM_NEW_CMD = 5102;
constexpr int IDM_NEW_BASH = 5103;
constexpr int IDM_ATTACH = 5104;
constexpr int IDM_CONNECT = 5105;
constexpr int IDM_CLOSE = 5106;
constexpr int IDM_RECONNECT = 5107;
constexpr int IDM_AUTOATTACH = 5108;
constexpr int IDM_EXIT = 5109;
constexpr int IDM_EDIT_COPY = 5110;
constexpr int IDM_EDIT_PASTE = 5111;
constexpr int IDM_SET_TOPMOST = 5112;
constexpr int IDM_SET_REMEMBER = 5113;
constexpr int IDM_SET_RESET_POS = 5114;
constexpr int IDM_SCROLLBACK_64 = 5120;
constexpr int IDM_SCROLLBACK_128 = 5121;
constexpr int IDM_SCROLLBACK_256 = 5122;
constexpr int IDM_SCROLLBACK_512 = 5123;
constexpr int IDM_SCROLLBACK_1024 = 5124;
constexpr int IDM_SCROLLBACK_2048 = 5125;

constexpr int kStatusHeight = 26;

enum class Lang { Ja, En, Es };

Lang g_lang = Lang::En;
HFONT g_uiFont = nullptr;
DashboardPalette g_palette;
HBRUSH g_bgBrush = nullptr;
HBRUSH g_panelBrush = nullptr;
bool g_themeApplied = false;

const wchar_t* T(const wchar_t* ja, const wchar_t* en, const wchar_t* es) {
    if (g_lang == Lang::Ja) return ja;
    if (g_lang == Lang::Es) return es ? es : en;
    return en;
}

void DetectLanguage() {
    std::string code;
    const std::wstring cfgPath =
        PathJoinWide(GetEffectiveDataDirectoryWide(), L"config.json");
    const std::string content = ReadFileUtf8Wide(cfgPath);
    if (!content.empty()) code = JsonGetString(content, "language", "");
    if (code.empty()) {
        wchar_t buf[16] = {};
        DWORD n = GetEnvironmentVariableW(L"PC_UI_LANG", buf, 16);
        if (n > 0) code = WideToUtf8(buf);
    }
    if (code.rfind("ja", 0) == 0) {
        g_lang = Lang::Ja;
    } else if (code.rfind("es", 0) == 0) {
        g_lang = Lang::Es;
    } else if (!code.empty()) {
        g_lang = Lang::En;
    } else {
        switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_JAPANESE: g_lang = Lang::Ja; break;
        case LANG_SPANISH: g_lang = Lang::Es; break;
        default: g_lang = Lang::En; break;
        }
    }
}

std::wstring GetControlText(HWND hwnd) {
    if (!hwnd) return {};
    int len = GetWindowTextLengthW(hwnd);
    if (len <= 0) return {};
    std::wstring text(static_cast<size_t>(len) + 1, L'\0');
    int copied = GetWindowTextW(hwnd, text.data(), len + 1);
    text.resize(copied > 0 ? static_cast<size_t>(copied) : 0);
    return text;
}

void CenterWindowOn(HWND hwnd, HWND owner) {
    if (!hwnd) return;
    RECT rc = {};
    RECT ro = {};
    GetWindowRect(hwnd, &rc);
    GetWindowRect(owner ? owner : GetDesktopWindow(), &ro);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int x = ro.left + (ro.right - ro.left - w) / 2;
    int y = ro.top + (ro.bottom - ro.top - h) / 2;
    SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

std::wstring ClientIniPath() {
    return PathJoinWide(GetEffectiveDataDirectoryWide(), L"terminal-client.ini");
}

struct RemoteSettings {
    std::wstring host = L"127.0.0.1";
    int port = 0;
    std::wstring token;
};

struct SessionTab {
    enum class State { Connecting, Connected, Exited, Failed };

    int id = 0;
    std::unique_ptr<TerminalView> view;
    RemoteSettings settings;
    std::wstring target;
    std::wstring baseLabel;
    std::wstring osTitle;
    std::wstring detail;
    State state = State::Connecting;
    int exitCode = 0;
    int localJobId = 0;
};

struct ClientState {
    HWND hwndMain = nullptr;
    HWND hwndTab = nullptr;
    HWND hwndStatus = nullptr;
    HWND btnNew = nullptr;
    HWND btnAttach = nullptr;
    HWND btnConnect = nullptr;
    HWND btnClose = nullptr;
    HMENU hMenuFile = nullptr;
    HMENU hMenuEdit = nullptr;
    HMENU hMenuSettings = nullptr;
    HMENU hMenuScrollback = nullptr;

    std::vector<std::unique_ptr<SessionTab>> tabs;
    int nextId = 1;
    int activeId = 0;

    RemoteSettings lastRemote;
    std::wstring lastTarget = L"powershell";

    bool localEndpointKnown = false;
    std::wstring localHost = L"127.0.0.1";
    int localPort = 0;
    std::wstring localToken;
    uint64_t localRetryAt = 0;

    bool autoAttach = true;
    unsigned autoAttachTicks = 0;
    unsigned themeTicks = 0;
    std::set<int> autoFailedJobs;

    bool topmost = false;
    bool rememberWindow = true;
    bool hasSavedRect = false;
    RECT savedRect = {};
    int scrollbackMb = 1024;
    std::wstring statusLeft;
    std::wstring statusRight;

    bool closing = false;
};

// ---------------------------------------------------------------------------
// Language / settings helpers
// ---------------------------------------------------------------------------

void LoadClientSettings(ClientState* st) {
    const std::wstring ini = ClientIniPath();
    wchar_t buf[1024] = {};

    GetPrivateProfileStringW(L"remote", L"host", L"127.0.0.1", buf, 1024, ini.c_str());
    st->lastRemote.host = buf;
    st->lastRemote.port = static_cast<int>(GetPrivateProfileIntW(L"remote", L"port", 9139, ini.c_str()));
    GetPrivateProfileStringW(L"remote", L"token", L"", buf, 1024, ini.c_str());
    st->lastRemote.token = buf;
    GetPrivateProfileStringW(L"remote", L"target", L"powershell", buf, 1024, ini.c_str());
    st->lastTarget = buf;

    st->autoAttach = GetPrivateProfileIntW(L"client", L"auto_attach", 1, ini.c_str()) != 0;
    st->topmost = GetPrivateProfileIntW(L"client", L"topmost", 0, ini.c_str()) != 0;
    st->rememberWindow = GetPrivateProfileIntW(L"client", L"remember_window", 1, ini.c_str()) != 0;
    st->scrollbackMb = static_cast<int>(
        GetPrivateProfileIntW(L"client", L"scrollback_mb", 1024, ini.c_str()));
    if (st->scrollbackMb < 16) st->scrollbackMb = 16;

    const int x = static_cast<int>(GetPrivateProfileIntW(L"window", L"x", 0, ini.c_str()));
    const int y = static_cast<int>(GetPrivateProfileIntW(L"window", L"y", 0, ini.c_str()));
    const int w = static_cast<int>(GetPrivateProfileIntW(L"window", L"w", 0, ini.c_str()));
    const int h = static_cast<int>(GetPrivateProfileIntW(L"window", L"h", 0, ini.c_str()));
    if (w >= 400 && h >= 300) {
        st->savedRect = {x, y, x + w, y + h};
        st->hasSavedRect = true;
    }
}

void SaveClientSettings(const ClientState* st) {
    const std::wstring ini = ClientIniPath();
    WritePrivateProfileStringW(L"remote", L"host", st->lastRemote.host.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"remote", L"port",
                               std::to_wstring(st->lastRemote.port).c_str(), ini.c_str());
    WritePrivateProfileStringW(L"remote", L"token", st->lastRemote.token.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"remote", L"target", st->lastTarget.c_str(), ini.c_str());
    WritePrivateProfileStringW(L"client", L"auto_attach", st->autoAttach ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"client", L"topmost", st->topmost ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"client", L"remember_window",
                               st->rememberWindow ? L"1" : L"0", ini.c_str());
    WritePrivateProfileStringW(L"client", L"scrollback_mb",
                               std::to_wstring(st->scrollbackMb).c_str(), ini.c_str());
    if (st->rememberWindow && st->hasSavedRect) {
        const RECT& rc = st->savedRect;
        WritePrivateProfileStringW(L"window", L"x", std::to_wstring(rc.left).c_str(), ini.c_str());
        WritePrivateProfileStringW(L"window", L"y", std::to_wstring(rc.top).c_str(), ini.c_str());
        WritePrivateProfileStringW(L"window", L"w",
                                   std::to_wstring(rc.right - rc.left).c_str(), ini.c_str());
        WritePrivateProfileStringW(L"window", L"h",
                                   std::to_wstring(rc.bottom - rc.top).c_str(), ini.c_str());
    }
}

void SaveWindowState(ClientState* st) {
    if (!st || !st->hwndMain || !st->rememberWindow) return;
    WINDOWPLACEMENT wp = {};
    wp.length = sizeof(wp);
    if (GetWindowPlacement(st->hwndMain, &wp)) {
        RECT rc = wp.rcNormalPosition;
        if (rc.right - rc.left >= 300 && rc.bottom - rc.top >= 200) {
            st->savedRect = rc;
            st->hasSavedRect = true;
        }
    }
    SaveClientSettings(st);
}

// ---------------------------------------------------------------------------
// Theme
// ---------------------------------------------------------------------------

void ApplyChildTheme(HWND root, bool dark) {
    for (HWND child = GetWindow(root, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        wchar_t cls[64] = {};
        GetClassNameW(child, cls, 64);
        if (_wcsicmp(cls, L"Button") == 0 || _wcsicmp(cls, L"SysTabControl32") == 0 ||
            _wcsicmp(cls, L"Edit") == 0 || _wcsicmp(cls, L"ListBox") == 0 ||
            _wcsicmp(cls, L"Static") == 0) {
            SetWindowTheme(child, dark ? L"DarkMode_Explorer" : L"", dark ? nullptr : L"");
            SendMessageW(child, WM_THEMECHANGED, 0, 0);
        }
    }
}

void ApplyClientTheme(ClientState* st, bool force) {
    const DashboardPalette palette = GetDashboardPalette();
    if (!force && g_themeApplied && palette == g_palette) return;
    g_palette = palette;
    g_themeApplied = true;
    if (g_bgBrush) DeleteObject(g_bgBrush);
    if (g_panelBrush) DeleteObject(g_panelBrush);
    g_bgBrush = CreateSolidBrush(palette.windowBg);
    g_panelBrush = CreateSolidBrush(palette.panel);

    if (!st || !st->hwndMain) return;
    ApplyDashboardNativeTheme(st->hwndMain);
    ApplyChildTheme(st->hwndMain, palette.dark);
    InvalidateRect(st->hwndMain, nullptr, TRUE);
    if (st->hwndTab) InvalidateRect(st->hwndTab, nullptr, TRUE);
    if (st->hwndStatus) InvalidateRect(st->hwndStatus, nullptr, TRUE);
    for (HWND button : {st->btnNew, st->btnAttach, st->btnConnect, st->btnClose}) {
        if (button) InvalidateRect(button, nullptr, TRUE);
    }
}

// ---------------------------------------------------------------------------
// Tab bookkeeping
// ---------------------------------------------------------------------------

SessionTab* FindTab(ClientState* st, int id) {
    for (auto& tab : st->tabs) {
        if (tab && tab->id == id) return tab.get();
    }
    return nullptr;
}

int FindTabIndex(const ClientState* st, int id) {
    for (size_t i = 0; i < st->tabs.size(); ++i) {
        if (st->tabs[i] && st->tabs[i]->id == id) return static_cast<int>(i);
    }
    return -1;
}

std::wstring StateSuffix(const SessionTab& tab) {
    switch (tab.state) {
    case SessionTab::State::Connecting:
        return L" ...";
    case SessionTab::State::Connected:
        return L"";
    case SessionTab::State::Exited: {
        std::wstring suffix = L" [";
        suffix += T(L"終了", L"exit", L"salida");
        if (tab.exitCode != 0) suffix += L" " + std::to_wstring(tab.exitCode);
        suffix += L"]";
        return suffix;
    }
    case SessionTab::State::Failed:
        return std::wstring(L" [") + T(L"エラー", L"error", L"error") + L"]";
    }
    return L"";
}

std::wstring TabDisplayText(const SessionTab& tab) {
    std::wstring text = tab.osTitle.empty() ? tab.baseLabel : tab.osTitle;
    if (text.empty()) text = T(L"ターミナル", L"Terminal", L"Terminal");
    return text + StateSuffix(tab);
}

void SetTabItemText(ClientState* st, int index) {
    if (!st->hwndTab || index < 0 || index >= static_cast<int>(st->tabs.size())) return;
    std::wstring text = TabDisplayText(*st->tabs[static_cast<size_t>(index)]);
    if (text.size() > 240) text.resize(240);
    TCITEMW item = {};
    item.mask = TCIF_TEXT;
    item.pszText = text.data();
    TabCtrl_SetItem(st->hwndTab, index, &item);
}

void RebuildTabControl(ClientState* st) {
    if (!st->hwndTab) return;
    SendMessageW(st->hwndTab, WM_SETREDRAW, FALSE, 0);
    TabCtrl_DeleteAllItems(st->hwndTab);
    for (size_t i = 0; i < st->tabs.size(); ++i) {
        std::wstring text = TabDisplayText(*st->tabs[i]);
        if (text.size() > 240) text.resize(240);
        TCITEMW item = {};
        item.mask = TCIF_TEXT;
        item.pszText = text.data();
        TabCtrl_InsertItem(st->hwndTab, static_cast<int>(i), &item);
    }
    const int activeIndex = FindTabIndex(st, st->activeId);
    if (activeIndex >= 0) TabCtrl_SetCurSel(st->hwndTab, activeIndex);
    SendMessageW(st->hwndTab, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(st->hwndTab, nullptr, TRUE);
}

// ---------------------------------------------------------------------------
// Dialogs
// ---------------------------------------------------------------------------

struct ConnectDlgState {
    RemoteSettings settings;
    std::wstring target;
    bool ok = false;
    HWND hHost = nullptr;
    HWND hPort = nullptr;
    HWND hToken = nullptr;
    HWND hTarget = nullptr;
};

HWND AddDialogLabel(HWND parent, const wchar_t* text, int x, int y, int w, int h) {
    HWND label = CreateWindowExW(0, L"STATIC", text, WS_CHILD | WS_VISIBLE | SS_LEFT,
                                 x, y, w, h, parent, nullptr, nullptr, nullptr);
    if (label && g_uiFont) SendMessageW(label, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    return label;
}

HWND AddDialogEdit(HWND parent, const wchar_t* text, int x, int y, int w, int h,
                   bool password = false) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL;
    if (password) style |= ES_PASSWORD;
    HWND edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", text, style,
                                x, y, w, h, parent, nullptr, nullptr, nullptr);
    if (edit && g_uiFont) SendMessageW(edit, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    return edit;
}

HWND AddDialogButton(HWND parent, const wchar_t* text, int id, int x, int y, int w, int h,
                     bool defButton, bool ownerDraw = false) {
    DWORD style = WS_CHILD | WS_VISIBLE | WS_TABSTOP;
    if (ownerDraw) style |= BS_OWNERDRAW;
    else style |= defButton ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON;
    HWND button = CreateWindowExW(0, L"BUTTON", text, style,
                                  x, y, w, h, parent,
                                  reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                                  nullptr, nullptr);
    if (button && g_uiFont) SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
    return button;
}

LONG_PTR ReadDialogPort(HWND edit) {
    std::wstring text = GetControlText(edit);
    wchar_t* end = nullptr;
    long value = std::wcstol(text.c_str(), &end, 10);
    if (!end || *end != L'\0' || value <= 0 || value > 65535) return -1;
    return value;
}

LRESULT CALLBACK ConnectDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<ConnectDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<ConnectDlgState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        if (!state) return -1;

        AddDialogLabel(hwnd, T(L"ホスト", L"Host", L"Host"), 16, 21, 110, 20);
        AddDialogLabel(hwnd, T(L"ポート", L"Port", L"Puerto"), 16, 53, 110, 20);
        AddDialogLabel(hwnd, T(L"トークン", L"Token", L"Token"), 16, 85, 110, 20);
        AddDialogLabel(hwnd, T(L"ターゲット", L"Target", L"Destino"), 16, 117, 110, 20);

        state->hHost = AddDialogEdit(hwnd, state->settings.host.c_str(), 132, 18, 288, 24);
        std::wstring port = std::to_wstring(state->settings.port > 0 ? state->settings.port : 9139);
        state->hPort = AddDialogEdit(hwnd, port.c_str(), 132, 50, 100, 24);
        state->hToken = AddDialogEdit(hwnd, state->settings.token.c_str(), 132, 82, 288, 24);
        state->hTarget = AddDialogEdit(hwnd, state->target.c_str(), 132, 114, 288, 24);

        AddDialogLabel(hwnd,
            T(L"ターゲット: powershell / cmd / bash / attach:<ジョブID>",
              L"Target: powershell / cmd / bash / attach:<job id>",
              L"Destino: powershell / cmd / bash / attach:<id de trabajo>"),
            16, 148, 404, 20);
        AddDialogLabel(hwnd,
            T(L"例: 192.168.1.10 / 9139 / 設定ファイルの terminal.token",
              L"Example: 192.168.1.10 / 9139 / terminal.token from config.json",
              L"Ejemplo: 192.168.1.10 / 9139 / terminal.token de config.json"),
            16, 168, 404, 20);

        AddDialogButton(hwnd, T(L"接続", L"Connect", L"Conectar"), IDOK, 244, 200, 86, 28, true);
        AddDialogButton(hwnd, T(L"キャンセル", L"Cancel", L"Cancelar"), IDCANCEL,
                        340, 200, 86, 28, false);
        SetFocus(state->hHost);
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == IDOK) {
            if (!state) return 0;
            std::wstring host = GetControlText(state->hHost);
            LONG_PTR port = ReadDialogPort(state->hPort);
            std::wstring token = GetControlText(state->hToken);
            std::wstring target = GetControlText(state->hTarget);
            if (host.empty() || port <= 0 || target.empty()) {
                MessageBoxW(hwnd,
                    T(L"ホスト・ポート・ターゲットを入力してください。",
                      L"Enter a host, a valid port and a target.",
                      L"Introduce un host, un puerto válido y un destino."),
                    T(L"接続", L"Connect", L"Conectar"), MB_OK | MB_ICONWARNING);
                return 0;
            }
            state->settings.host = host;
            state->settings.port = static_cast<int>(port);
            state->settings.token = token;
            state->target = target;
            state->ok = true;
            DestroyWindow(hwnd);
        } else if (id == IDCANCEL) {
            DestroyWindow(hwnd);
        }
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wParam), &rc,
                 g_bgBrush ? g_bgBrush : reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, g_palette.text);
        SetBkColor(hdc, g_palette.windowBg);
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(g_bgBrush ? g_bgBrush
                                                   : GetStockObject(WHITE_BRUSH));
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, g_palette.listText);
        SetBkColor(hdc, g_palette.panel);
        return reinterpret_cast<LRESULT>(g_panelBrush ? g_panelBrush
                                                      : GetStockObject(WHITE_BRUSH));
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RunConnectDialog(HWND owner, RemoteSettings& settings, std::wstring& target) {
    ConnectDlgState state;
    state.settings = settings;
    state.target = target.empty() ? L"powershell" : target;

    RECT rc = {0, 0, 436, 242};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    HWND dlg = CreateWindowExW(exStyle, kConnectDlgClass,
                               T(L"WinConsoleServer に接続", L"Connect to WinConsoleServer",
                                 L"Conectar a WinConsoleServer"),
                               style, CW_USEDEFAULT, CW_USEDEFAULT,
                               rc.right - rc.left, rc.bottom - rc.top,
                               owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (!dlg) return false;

    ApplyDashboardNativeTheme(dlg);
    ApplyChildTheme(dlg, g_palette.dark);
    CenterWindowOn(dlg, owner);
    ShowWindow(dlg, SW_SHOW);
    EnableWindow(owner, FALSE);
    while (IsWindow(dlg)) {
        MSG msg;
        BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
        if (ret <= 0) break;
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (state.ok) {
        settings = state.settings;
        target = state.target;
    }
    return state.ok;
}

struct AttachItem {
    int id = 0;
    bool running = false;
    std::wstring label;
};

struct AttachDlgState {
    std::vector<AttachItem> items;
    int selected = -1;
    bool ok = false;
    HWND list = nullptr;
};

LRESULT CALLBACK AttachDlgProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<AttachDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<AttachDlgState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
        if (!state) return -1;

        state->list = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
            16, 16, 584, 252, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(1001)),
            nullptr, nullptr);
        if (state->list && g_uiFont) {
            SendMessageW(state->list, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
        }
        for (const auto& item : state->items) {
            SendMessageW(state->list, LB_ADDSTRING, 0,
                         reinterpret_cast<LPARAM>(item.label.c_str()));
        }
        if (!state->items.empty()) {
            SendMessageW(state->list, LB_SETCURSEL, 0, 0);
            state->selected = 0;
        }
        AddDialogButton(hwnd, T(L"接続", L"Attach", L"Conectar"), IDOK, 424, 278, 86, 28, true);
        AddDialogButton(hwnd, T(L"キャンセル", L"Cancel", L"Cancelar"), IDCANCEL,
                        520, 278, 80, 28, false);
        SetFocus(state->list);
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == IDOK) {
            if (!state || !state->list) return 0;
            int sel = static_cast<int>(SendMessageW(state->list, LB_GETCURSEL, 0, 0));
            if (sel < 0 || sel >= static_cast<int>(state->items.size())) {
                MessageBoxW(hwnd,
                    T(L"ジョブを選択してください。", L"Select a job first.",
                      L"Selecciona un trabajo primero."),
                    T(L"接続", L"Attach", L"Conectar"), MB_OK | MB_ICONWARNING);
                return 0;
            }
            state->selected = sel;
            state->ok = true;
            DestroyWindow(hwnd);
        } else if (id == IDCANCEL) {
            DestroyWindow(hwnd);
        } else if (id == 1001 && HIWORD(wParam) == LBN_DBLCLK) {
            SendMessageW(hwnd, WM_COMMAND, IDOK, 0);
        }
        return 0;
    }
    case WM_ERASEBKGND: {
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        FillRect(reinterpret_cast<HDC>(wParam), &rc,
                 g_bgBrush ? g_bgBrush : reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1));
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, g_palette.text);
        SetBkColor(hdc, g_palette.windowBg);
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(g_bgBrush ? g_bgBrush
                                                   : GetStockObject(WHITE_BRUSH));
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, g_palette.listText);
        SetBkColor(hdc, g_palette.panel);
        return reinterpret_cast<LRESULT>(g_panelBrush ? g_panelBrush
                                                      : GetStockObject(WHITE_BRUSH));
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool RunAttachDialog(HWND owner, const std::vector<AttachItem>& items, int& selected) {
    if (items.empty()) return false;
    AttachDlgState state;
    state.items = items;

    RECT rc = {0, 0, 616, 322};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU;
    const DWORD exStyle = WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT;
    AdjustWindowRectEx(&rc, style, FALSE, exStyle);
    HWND dlg = CreateWindowExW(exStyle, kAttachDlgClass,
                               T(L"ローカルジョブに接続", L"Attach to a local job",
                                 L"Conectar a un trabajo local"),
                               style, CW_USEDEFAULT, CW_USEDEFAULT,
                               rc.right - rc.left, rc.bottom - rc.top,
                               owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (!dlg) return false;

    ApplyDashboardNativeTheme(dlg);
    ApplyChildTheme(dlg, g_palette.dark);
    CenterWindowOn(dlg, owner);
    ShowWindow(dlg, SW_SHOW);
    EnableWindow(owner, FALSE);
    while (IsWindow(dlg)) {
        MSG msg;
        BOOL ret = GetMessageW(&msg, nullptr, 0, 0);
        if (ret <= 0) break;
        if (!IsDialogMessageW(dlg, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (state.ok && state.selected >= 0 &&
        state.selected < static_cast<int>(state.items.size())) {
        selected = state.items[static_cast<size_t>(state.selected)].id;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// IPC helpers (local core)
// ---------------------------------------------------------------------------

std::string JsonStringValue(const nlohmann::json& j, const char* key) {
    if (!j.is_object() || !j.contains(key) || !j[key].is_string()) return {};
    return j[key].get<std::string>();
}

bool SendCoreRequest(const std::string& request, nlohmann::json& out, std::wstring& error) {
    std::string response;
    std::string err;
    if (!SendIpcRequest(request, response, err, false)) {
        error = Utf8ToWide(err.empty() ? "IPC request failed" : err);
        return false;
    }
    out = nlohmann::json::parse(response, nullptr, false);
    if (out.is_discarded() || !out.is_object()) {
        error = Utf8ToWide("invalid JSON response");
        return false;
    }
    return true;
}

bool EnsureLocalEndpoint(ClientState* st, std::wstring& error) {
    if (st->localEndpointKnown && st->localPort > 0) return true;
    const uint64_t now = GetNowTickMs();
    if (st->localRetryAt != 0 && now < st->localRetryAt) {
        error = T(L"Hancho コアに接続できません。", L"Hancho core is not available.",
                  L"El núcleo de Hancho no está disponible.");
        return false;
    }

    nlohmann::json response;
    std::wstring ipcError;
    if (!SendCoreRequest("{\"action\":\"term_endpoint\"}", response, ipcError) ||
        !response.value("ok", false)) {
        error = ipcError;
        if (error.empty()) {
            std::string msg = JsonStringValue(response, "error");
            error = Utf8ToWide(msg.empty() ? "terminal server is not running" : msg);
        }
        st->localEndpointKnown = false;
        st->localPort = 0;
        st->localRetryAt = now + 5000;
        return false;
    }

    st->localHost = Utf8ToWide(response.value("host", std::string("127.0.0.1")));
    int port = response.value("port", 0);
    st->localPort = (port > 0 && port <= 65535) ? port : 0;
    st->localToken = Utf8ToWide(response.value("token", std::string()));
    if (st->localPort <= 0) {
        error = T(L"ターミナルサーバーが無効です。", L"Terminal server is disabled.",
                  L"El servidor de terminal está desactivado.");
        st->localRetryAt = now + 5000;
        return false;
    }
    st->localEndpointKnown = true;
    st->localRetryAt = 0;
    return true;
}

bool FetchLocalConPtyJobs(std::vector<AttachItem>& out, std::wstring& error) {
    nlohmann::json response;
    std::wstring ipcError;
    if (!SendCoreRequest("{\"action\":\"list\"}", response, ipcError)) {
        error = ipcError;
        return false;
    }
    if (!response.value("ok", false) || !response.contains("processes") ||
        !response["processes"].is_array()) {
        error = T(L"プロセス一覧を取得できません。", L"Cannot read the process list.",
                  L"No se puede leer la lista de procesos.");
        return false;
    }

    for (const auto& item : response["processes"]) {
        if (!item.is_object()) continue;
        const std::string mode = item.value("mode", std::string());
        const std::string state = item.value("state", std::string());
        if (mode != "conpty" && mode != "pty" && mode != "interactive") continue;
        const int id = item.value("id", 0);
        if (id <= 0) continue;
        const std::string name = item.value("name", std::string());
        std::wstring label = L"#" + std::to_wstring(id) + L"  " + Utf8ToWide(name);
        label += L"  [" + Utf8ToWide(state) + L"]";
        AttachItem entry;
        entry.id = id;
        entry.running = (state == "running");
        entry.label = label;
        out.push_back(std::move(entry));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Session management
// ---------------------------------------------------------------------------

std::wstring DefaultLabelFor(const std::wstring& target, const std::wstring& host,
                             bool local) {
    if (target.rfind(L"attach:", 0) == 0) {
        std::wstring label = target;
        if (!local) label += L" @ " + host;
        return label;
    }
    std::wstring label = target;
    if (!label.empty()) label[0] = static_cast<wchar_t>(towupper(label[0]));
    if (!local) label += L" @ " + host;
    else label += L" (local)";
    return label;
}

void ApplyActiveLayout(ClientState* st) {
    if (!st || !st->hwndMain) return;
    RECT rc = {};
    GetClientRect(st->hwndMain, &rc);
    int width = rc.right - rc.left;
    int height = rc.bottom - rc.top;

    const int statusHeight = kStatusHeight;
    if (st->hwndStatus) {
        MoveWindow(st->hwndStatus, 0, (std::max)(0, height - statusHeight),
                   width, statusHeight, TRUE);
    }

    const int margin = 6;
    const int buttonY = 4;
    const int buttonH = 26;
    int x = margin;
    for (HWND button : {st->btnNew, st->btnAttach, st->btnConnect, st->btnClose}) {
        if (!button) continue;
        RECT br = {};
        GetWindowRect(button, &br);
        int bw = br.right - br.left;
        if (bw <= 0) bw = 90;
        MoveWindow(button, x, buttonY, bw, buttonH, TRUE);
        x += bw + 6;
    }

    const int tabTop = buttonY + buttonH + 4;
    const int tabH = 28;
    if (st->hwndTab) {
        MoveWindow(st->hwndTab, margin, tabTop, (std::max)(100, width - margin * 2), tabH, TRUE);
    }

    const int viewTop = tabTop + tabH + 2;
    const int viewBottom = height - statusHeight - 2;
    const int viewW = (std::max)(200, width - margin * 2);
    const int viewH = (std::max)(120, viewBottom - viewTop);

    for (auto& tab : st->tabs) {
        if (!tab || !tab->view) continue;
        HWND view = tab->view->Hwnd();
        if (!view || !IsWindow(view)) continue;
        if (tab->id == st->activeId) {
            MoveWindow(view, margin, viewTop, viewW, viewH, TRUE);
            ShowWindow(view, SW_SHOW);
        } else {
            ShowWindow(view, SW_HIDE);
        }
    }
}

void SetActiveTab(ClientState* st, int id) {
    if (!st) return;
    if (FindTabIndex(st, id) < 0) id = st->tabs.empty() ? 0 : st->tabs.front()->id;
    st->activeId = id;

    const int index = FindTabIndex(st, id);
    if (st->hwndTab && index >= 0 && TabCtrl_GetCurSel(st->hwndTab) != index) {
        TabCtrl_SetCurSel(st->hwndTab, index);
    }
    ApplyActiveLayout(st);
    SessionTab* tab = FindTab(st, id);
    if (tab && tab->view && tab->view->Hwnd() && IsWindow(tab->view->Hwnd())) {
        SetFocus(tab->view->Hwnd());
    }
    InvalidateRect(st->hwndMain, nullptr, FALSE);
}

void DestroyTabView(SessionTab* tab) {
    if (!tab || !tab->view) return;
    tab->view->SetRemoteIo(nullptr);
    HWND hwnd = tab->view->Hwnd();
    if (hwnd && IsWindow(hwnd)) DestroyWindow(hwnd);
    tab->view.reset();
}

void CloseTabById(ClientState* st, int id) {
    if (!st) return;
    const int index = FindTabIndex(st, id);
    if (index < 0) return;

    const bool wasActive = (st->activeId == id);
    DestroyTabView(st->tabs[static_cast<size_t>(index)].get());
    st->tabs.erase(st->tabs.begin() + index);

    if (wasActive) {
        int nextId = 0;
        if (!st->tabs.empty()) {
            int nextIndex = (std::min)(index, static_cast<int>(st->tabs.size()) - 1);
            nextId = st->tabs[static_cast<size_t>(nextIndex)]->id;
        }
        st->activeId = nextId;
    }
    RebuildTabControl(st);
    SetActiveTab(st, st->activeId);
}

int AddSession(ClientState* st, const RemoteSettings& settings, const std::wstring& target,
               const std::wstring& label, int localJobId, std::wstring& error) {
    if (!st || !st->hwndMain) return 0;
    if (target.empty()) {
        error = T(L"接続先が指定されていません。", L"No connection target was specified.",
                  L"No se especificó un destino.");
        return 0;
    }

    RemoteConfig cfg;
    cfg.host = WideToUtf8(settings.host.empty() ? L"127.0.0.1" : settings.host);
    cfg.port = static_cast<uint16_t>(settings.port > 0 ? settings.port : 9139);
    cfg.token = WideToUtf8(settings.token);

    auto tab = std::make_unique<SessionTab>();
    tab->id = st->nextId++;
    tab->settings = settings;
    tab->target = target;
    tab->baseLabel = label;
    tab->localJobId = localJobId;
    tab->state = SessionTab::State::Connecting;
    tab->view = std::make_unique<TerminalView>();

    HWND viewHwnd = tab->view->Create(st->hwndMain);
    if (!viewHwnd) {
        error = T(L"ターミナルウィンドウを作成できません。",
                  L"Could not create the terminal window.",
                  L"No se pudo crear la ventana de terminal.");
        return 0;
    }
    ShowWindow(viewHwnd, SW_HIDE);

    auto session = std::make_unique<RemoteSession>(cfg);
    const HWND mainHwnd = st->hwndMain;
    const int tabId = tab->id;
    session->onExit = [mainHwnd, tabId](const wcs::ExitInfo& info) {
        PostMessageW(mainHwnd, WM_CLIENT_SESSION_EXIT,
                     static_cast<WPARAM>(tabId),
                     static_cast<LPARAM>(info.exit_code));
    };
    session->onError = [mainHwnd, tabId](const wcs::ErrorInfo&) {
        PostMessageW(mainHwnd, WM_CLIENT_SESSION_ERROR, static_cast<WPARAM>(tabId), 0);
    };
    tab->view->SetRemoteIo(std::move(session));

    st->tabs.push_back(std::move(tab));
    SessionTab* added = st->tabs.back().get();

    if (!added->view->StartRemote(target, &error)) {
        DestroyTabView(added);
        st->tabs.pop_back();
        if (error.empty()) {
            error = T(L"接続に失敗しました。", L"Connection failed.",
                      L"Error al conectar.");
        }
        WriteAuditLog("TERM_CLIENT_CONNECT_FAILED",
                      WideToUtf8(settings.host) + ":" + std::to_string(settings.port) +
                          " " + WideToUtf8(target) + " - " + WideToUtf8(error));
        return 0;
    }

    added->state = SessionTab::State::Connected;
    added->view->SetScrollbackBudget(
        1000000, static_cast<size_t>(st->scrollbackMb) * 1024 * 1024);
    WriteAuditLog("TERM_CLIENT_CONNECT",
                  WideToUtf8(settings.host) + ":" + std::to_string(settings.port) +
                      " " + WideToUtf8(target));
    RebuildTabControl(st);
    SetActiveTab(st, added->id);
    return added->id;
}

// ---------------------------------------------------------------------------
// Commands
// ---------------------------------------------------------------------------

void ConnectLocalShell(ClientState* st, const wchar_t* shell) {
    std::wstring error;
    if (!EnsureLocalEndpoint(st, error)) {
        MessageBoxW(st->hwndMain, error.c_str(),
                    T(L"ターミナル", L"Terminal", L"Terminal"), MB_OK | MB_ICONERROR);
        return;
    }
    RemoteSettings settings;
    settings.host = st->localHost;
    settings.port = st->localPort;
    settings.token = st->localToken;
    std::wstring target = shell;
    int id = AddSession(st, settings, target, DefaultLabelFor(target, settings.host, true),
                        0, error);
    if (id == 0) {
        MessageBoxW(st->hwndMain, error.c_str(),
                    T(L"ターミナル", L"Terminal", L"Terminal"), MB_OK | MB_ICONERROR);
    }
}

void ConnectLocalAttach(ClientState* st, int jobId, const std::wstring& label) {
    std::wstring error;
    if (!EnsureLocalEndpoint(st, error)) {
        MessageBoxW(st->hwndMain, error.c_str(),
                    T(L"ターミナル", L"Terminal", L"Terminal"), MB_OK | MB_ICONERROR);
        return;
    }
    RemoteSettings settings;
    settings.host = st->localHost;
    settings.port = st->localPort;
    settings.token = st->localToken;
    std::wstring target = L"attach:" + std::to_wstring(jobId);
    std::wstring tabLabel = label.empty() ? target : label;
    if (AddSession(st, settings, target, tabLabel, jobId, error) == 0) {
        MessageBoxW(st->hwndMain, error.c_str(),
                    T(L"ターミナル", L"Terminal", L"Terminal"), MB_OK | MB_ICONERROR);
    }
}

void ShowAttachPicker(ClientState* st) {
    std::vector<AttachItem> items;
    std::wstring error;
    if (!FetchLocalConPtyJobs(items, error)) {
        MessageBoxW(st->hwndMain, error.c_str(),
                    T(L"ターミナル", L"Terminal", L"Terminal"), MB_OK | MB_ICONERROR);
        return;
    }
    if (items.empty()) {
        MessageBoxW(st->hwndMain,
            T(L"接続できる ConPTY ジョブがありません。",
              L"There are no ConPTY jobs to attach to.",
              L"No hay trabajos ConPTY a los que conectarse."),
            T(L"ターミナル", L"Terminal", L"Terminal"), MB_OK | MB_ICONINFORMATION);
        return;
    }
    int selectedId = 0;
    if (!RunAttachDialog(st->hwndMain, items, selectedId)) return;
    std::wstring label;
    for (const auto& item : items) {
        if (item.id == selectedId) {
            label = item.label;
            break;
        }
    }
    ConnectLocalAttach(st, selectedId, label);
}

void ShowRemoteConnect(ClientState* st) {
    RemoteSettings settings = st->lastRemote;
    if (settings.port <= 0) settings.port = 9139;
    std::wstring target = st->lastTarget.empty() ? L"powershell" : st->lastTarget;
    if (!RunConnectDialog(st->hwndMain, settings, target)) return;

    st->lastRemote = settings;
    st->lastTarget = target;
    SaveClientSettings(st);

    std::wstring error;
    const bool local = settings.host == L"127.0.0.1" || settings.host == L"localhost";
    if (AddSession(st, settings, target, DefaultLabelFor(target, settings.host, local),
                   0, error) == 0) {
        MessageBoxW(st->hwndMain, error.c_str(),
                    T(L"ターミナル", L"Terminal", L"Terminal"), MB_OK | MB_ICONERROR);
    }
}

void ReconnectActive(ClientState* st) {
    SessionTab* tab = FindTab(st, st->activeId);
    if (!tab) return;
    RemoteSettings settings = tab->settings;
    std::wstring target = tab->target;
    std::wstring label = tab->baseLabel;
    const int jobId = tab->localJobId;
    CloseTabById(st, tab->id);

    std::wstring error;
    if (AddSession(st, settings, target, label, jobId, error) == 0) {
        MessageBoxW(st->hwndMain, error.c_str(),
                    T(L"ターミナル", L"Terminal", L"Terminal"), MB_OK | MB_ICONERROR);
    }
}

// Attaches to local ConPTY jobs that have no tab yet. This preserves the old
// dashboard behavior where "+ Launch" immediately showed a terminal.
void AutoAttachLocalJobs(ClientState* st) {
    if (!st->autoAttach || st->closing) return;
    std::wstring error;
    if (!EnsureLocalEndpoint(st, error)) return;

    std::vector<AttachItem> items;
    if (!FetchLocalConPtyJobs(items, error)) return;

    std::set<int> known;
    for (const auto& tab : st->tabs) {
        if (tab && tab->localJobId > 0) known.insert(tab->localJobId);
    }

    int opened = 0;
    const int previousActive = st->activeId;
    for (const auto& item : items) {
        if (opened >= 8) break;
        if (!item.running) continue;
        if (known.count(item.id) != 0) continue;
        if (st->autoFailedJobs.count(item.id) != 0) continue;
        if (item.label.find(L"wcsp-") != std::wstring::npos) continue;

        RemoteSettings settings;
        settings.host = st->localHost;
        settings.port = st->localPort;
        settings.token = st->localToken;
        std::wstring target = L"attach:" + std::to_wstring(item.id);
        if (AddSession(st, settings, target, item.label, item.id, error) != 0) {
            ++opened;
        } else {
            st->autoFailedJobs.insert(item.id);
        }
    }
    if (opened > 0 && previousActive != 0 && FindTabIndex(st, previousActive) >= 0) {
        SetActiveTab(st, previousActive);
    }
}

void UpdateTabTitles(ClientState* st) {
    for (size_t i = 0; i < st->tabs.size(); ++i) {
        SessionTab* tab = st->tabs[i].get();
        if (!tab || !tab->view) continue;

        std::wstring title = tab->view->Title();
        if (title != tab->osTitle) {
            tab->osTitle = title;
            SetTabItemText(st, static_cast<int>(i));
        }

        if (tab->state == SessionTab::State::Connected && !tab->view->IsStarted()) {
            tab->state = SessionTab::State::Exited;
            tab->detail = tab->view->LastError();
            SetTabItemText(st, static_cast<int>(i));
        }
    }
}

void UpdateStatus(ClientState* st) {
    if (!st->hwndStatus) return;
    SessionTab* tab = FindTab(st, st->activeId);
    std::wstring left;
    std::wstring right;

    if (tab) {
        left = tab->settings.host + L":" + std::to_wstring(tab->settings.port);
        left += L"  " + tab->target;
        switch (tab->state) {
        case SessionTab::State::Connecting:
            right = T(L"接続中...", L"Connecting...", L"Conectando...");
            break;
        case SessionTab::State::Connected:
            right = T(L"接続済み", L"Connected", L"Conectado");
            break;
        case SessionTab::State::Exited:
            right = T(L"終了", L"Exited", L"Finalizado");
            if (tab->exitCode != 0) right += L" (" + std::to_wstring(tab->exitCode) + L")";
            if (!tab->detail.empty()) right += L" - " + tab->detail;
            break;
        case SessionTab::State::Failed:
            right = T(L"エラー", L"Error", L"Error");
            if (!tab->detail.empty()) right += L" - " + tab->detail;
            break;
        }
    } else {
        left = T(L"セッションなし", L"No sessions", L"Sin sesiones");
        right = T(L"ファイル > 新しいローカルシェル で接続します。",
                  L"Use File > New local shell to connect.",
                  L"Usa Archivo > Nuevo shell local para conectar.");
    }

    if (st->statusLeft != left || st->statusRight != right) {
        st->statusLeft = left;
        st->statusRight = right;
        InvalidateRect(st->hwndStatus, nullptr, TRUE);
    }
}

void CloseAllTabs(ClientState* st) {
    st->closing = true;
    for (auto& tab : st->tabs) DestroyTabView(tab.get());
    st->tabs.clear();
    st->activeId = 0;
}

// ---------------------------------------------------------------------------
// Menu / window
// ---------------------------------------------------------------------------

HMENU BuildMenu(ClientState* st) {
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_NEW_PWSH,
                T(L"新しいローカルシェル (PowerShell)	Ctrl+T",
                  L"New local shell (PowerShell)	Ctrl+T",
                  L"Nuevo shell local (PowerShell)	Ctrl+T"));
    AppendMenuW(file, MF_STRING, IDM_NEW_CMD,
                T(L"新しいローカルシェル (cmd)", L"New local shell (cmd)",
                  L"Nuevo shell local (cmd)"));
    AppendMenuW(file, MF_STRING, IDM_NEW_BASH,
                T(L"新しいローカルシェル (bash)", L"New local shell (bash)",
                  L"Nuevo shell local (bash)"));
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_ATTACH,
                T(L"ローカルジョブに接続...", L"Attach to local job...",
                  L"Conectar a un trabajo local..."));
    AppendMenuW(file, MF_STRING, IDM_CONNECT,
                T(L"リモートに接続...", L"Connect to remote...",
                  L"Conectar a remoto..."));
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_RECONNECT,
                T(L"現在のタブを再接続", L"Reconnect current tab",
                  L"Reconectar la pestaña actual"));
    AppendMenuW(file, MF_STRING, IDM_CLOSE,
                T(L"現在のタブを閉じる	Ctrl+W", L"Close current tab	Ctrl+W",
                  L"Cerrar la pestaña actual	Ctrl+W"));
    AppendMenuW(file, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(file, MF_STRING, IDM_EXIT,
                T(L"終了", L"Exit", L"Salir"));

    HMENU edit = CreatePopupMenu();
    AppendMenuW(edit, MF_STRING, IDM_EDIT_COPY, T(L"コピー", L"Copy", L"Copiar"));
    AppendMenuW(edit, MF_STRING, IDM_EDIT_PASTE,
                T(L"貼り付け", L"Paste", L"Pegar"));

    HMENU scroll = CreatePopupMenu();
    AppendMenuW(scroll, MF_STRING, IDM_SCROLLBACK_64, L"64 MB");
    AppendMenuW(scroll, MF_STRING, IDM_SCROLLBACK_128, L"128 MB");
    AppendMenuW(scroll, MF_STRING, IDM_SCROLLBACK_256, L"256 MB");
    AppendMenuW(scroll, MF_STRING, IDM_SCROLLBACK_512, L"512 MB");
    AppendMenuW(scroll, MF_STRING, IDM_SCROLLBACK_1024, L"1024 MB");
    AppendMenuW(scroll, MF_STRING, IDM_SCROLLBACK_2048, L"2048 MB");
    int scrollId = IDM_SCROLLBACK_1024;
    switch (st->scrollbackMb) {
    case 64: scrollId = IDM_SCROLLBACK_64; break;
    case 128: scrollId = IDM_SCROLLBACK_128; break;
    case 256: scrollId = IDM_SCROLLBACK_256; break;
    case 512: scrollId = IDM_SCROLLBACK_512; break;
    case 1024: scrollId = IDM_SCROLLBACK_1024; break;
    case 2048: scrollId = IDM_SCROLLBACK_2048; break;
    default: scrollId = IDM_SCROLLBACK_1024; break;
    }
    CheckMenuRadioItem(scroll, IDM_SCROLLBACK_64, IDM_SCROLLBACK_2048,
                       scrollId, MF_BYCOMMAND);

    HMENU settings = CreatePopupMenu();
    AppendMenuW(settings, MF_STRING, IDM_AUTOATTACH,
                T(L"新しいローカルジョブを自動で開く",
                  L"Automatically open new local jobs",
                  L"Abrir automáticamente nuevos trabajos locales"));
    AppendMenuW(settings, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(settings, MF_STRING, IDM_SET_TOPMOST,
                T(L"常に手前に表示", L"Always on top", L"Siempre visible"));
    AppendMenuW(settings, MF_STRING, IDM_SET_REMEMBER,
                T(L"ウィンドウ位置を記憶", L"Remember window position",
                  L"Recordar posición de la ventana"));
    AppendMenuW(settings, MF_STRING, IDM_SET_RESET_POS,
                T(L"ウィンドウ位置をリセット", L"Reset window position",
                  L"Restablecer posición de la ventana"));
    AppendMenuW(settings, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(settings, MF_POPUP, reinterpret_cast<UINT_PTR>(scroll),
                T(L"スクロールバック", L"Scrollback", L"Desplazamiento"));
    CheckMenuItem(settings, IDM_AUTOATTACH,
                  MF_BYCOMMAND | (st->autoAttach ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(settings, IDM_SET_TOPMOST,
                  MF_BYCOMMAND | (st->topmost ? MF_CHECKED : MF_UNCHECKED));
    CheckMenuItem(settings, IDM_SET_REMEMBER,
                  MF_BYCOMMAND | (st->rememberWindow ? MF_CHECKED : MF_UNCHECKED));

    HMENU menu = CreateMenu();
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(file),
                T(L"ファイル", L"File", L"Archivo"));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(edit),
                T(L"編集", L"Edit", L"Editar"));
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(settings),
                T(L"設定", L"Settings", L"Ajustes"));
    st->hMenuFile = file;
    st->hMenuEdit = edit;
    st->hMenuSettings = settings;
    st->hMenuScrollback = scroll;
    return menu;
}

void DrawClientButton(DRAWITEMSTRUCT* dis) {
    if (!dis) return;
    const bool pressed = (dis->itemState & ODS_SELECTED) != 0;
    COLORREF bg = pressed ? g_palette.border : g_palette.panel;
    HBRUSH bgBrush = CreateSolidBrush(bg);
    FillRect(dis->hDC, &dis->rcItem, bgBrush);
    DeleteObject(bgBrush);

    HPEN borderPen = CreatePen(PS_SOLID, 1, g_palette.border);
    HGDIOBJ oldPen = SelectObject(dis->hDC, borderPen);
    HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
    Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top,
              dis->rcItem.right, dis->rcItem.bottom);
    SelectObject(dis->hDC, oldBrush);
    SelectObject(dis->hDC, oldPen);
    DeleteObject(borderPen);

    wchar_t text[256] = {};
    GetWindowTextW(dis->hwndItem, text, 256);
    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, (dis->itemState & ODS_DISABLED) ? g_palette.muted : g_palette.text);
    HGDIOBJ oldFont = SelectObject(dis->hDC,
        g_uiFont ? g_uiFont : GetStockObject(DEFAULT_GUI_FONT));
    RECT rc = dis->rcItem;
    InflateRect(&rc, -4, 0);
    DrawTextW(dis->hDC, text, -1, &rc,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dis->hDC, oldFont);
}

void DrawClientTab(DRAWITEMSTRUCT* dis) {
    if (!dis) return;
    wchar_t text[256] = {};
    TCITEMW item = {};
    item.mask = TCIF_TEXT;
    item.pszText = text;
    item.cchTextMax = 256;
    TabCtrl_GetItem(dis->hwndItem, dis->itemID, &item);

    const bool selected = (dis->itemState & ODS_SELECTED) != 0 ||
                          TabCtrl_GetCurSel(dis->hwndItem) == static_cast<int>(dis->itemID);
    HBRUSH bgBrush = CreateSolidBrush(selected ? g_palette.panel : g_palette.windowBg);
    FillRect(dis->hDC, &dis->rcItem, bgBrush);
    DeleteObject(bgBrush);

    HPEN borderPen = CreatePen(PS_SOLID, 1, g_palette.border);
    HGDIOBJ oldPen = SelectObject(dis->hDC, borderPen);
    HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
    Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top,
              dis->rcItem.right, dis->rcItem.bottom);
    SelectObject(dis->hDC, oldBrush);
    SelectObject(dis->hDC, oldPen);
    DeleteObject(borderPen);

    if (selected) {
        RECT bar = dis->rcItem;
        bar.top = bar.bottom - 2;
        HBRUSH accentBrush = CreateSolidBrush(g_palette.accent);
        FillRect(dis->hDC, &bar, accentBrush);
        DeleteObject(accentBrush);
    }

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, selected ? g_palette.accent : g_palette.text);
    HGDIOBJ oldFont = SelectObject(dis->hDC,
        g_uiFont ? g_uiFont : GetStockObject(DEFAULT_GUI_FONT));
    RECT rcText = dis->rcItem;
    InflateRect(&rcText, -6, 0);
    DrawTextW(dis->hDC, text, -1, &rcText,
              DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dis->hDC, oldFont);
}

void DrawClientStatus(ClientState* st, DRAWITEMSTRUCT* dis) {
    if (!dis) return;
    HBRUSH bgBrush = CreateSolidBrush(g_palette.panel);
    FillRect(dis->hDC, &dis->rcItem, bgBrush);
    DeleteObject(bgBrush);

    HPEN borderPen = CreatePen(PS_SOLID, 1, g_palette.border);
    HGDIOBJ oldPen = SelectObject(dis->hDC, borderPen);
    MoveToEx(dis->hDC, dis->rcItem.left, dis->rcItem.top, nullptr);
    LineTo(dis->hDC, dis->rcItem.right, dis->rcItem.top);
    SelectObject(dis->hDC, oldPen);
    DeleteObject(borderPen);

    SetBkMode(dis->hDC, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dis->hDC,
        g_uiFont ? g_uiFont : GetStockObject(DEFAULT_GUI_FONT));

    RECT rcAll = dis->rcItem;
    InflateRect(&rcAll, -8, 0);
    RECT rcRight = rcAll;
    if (!st->statusRight.empty()) {
        DrawTextW(dis->hDC, st->statusRight.c_str(), -1, &rcRight,
                  DT_CALCRECT | DT_RIGHT | DT_SINGLELINE);
        const int rightW = rcRight.right - rcRight.left;
        rcRight = rcAll;
        rcRight.left = (std::max)(rcAll.left, rcAll.right - rightW);
        SetTextColor(dis->hDC, g_palette.text);
        DrawTextW(dis->hDC, st->statusRight.c_str(), -1, &rcRight,
                  DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        rcAll.right = (std::max)(rcAll.left, rcRight.left - 12);
    }
    SetTextColor(dis->hDC, g_palette.muted);
    DrawTextW(dis->hDC, st->statusLeft.c_str(), -1, &rcAll,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dis->hDC, oldFont);
}

void ShowClientContextMenu(ClientState* st, POINT pt) {
    if (!st) return;
    HMENU menu = CreatePopupMenu();
    SessionTab* tab = FindTab(st, st->activeId);
    const bool canCopy = tab && tab->view && tab->view->HasSelection();
    const bool canPaste = tab && IsClipboardFormatAvailable(CF_UNICODETEXT);
    AppendMenuW(menu, MF_STRING | (canCopy ? 0 : MF_GRAYED), IDM_EDIT_COPY,
                T(L"コピー", L"Copy", L"Copiar"));
    AppendMenuW(menu, MF_STRING | (canPaste ? 0 : MF_GRAYED), IDM_EDIT_PASTE,
                T(L"貼り付け", L"Paste", L"Pegar"));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, IDM_NEW_PWSH,
                T(L"新しいローカルシェル", L"New local shell", L"Nuevo shell local"));
    AppendMenuW(menu, MF_STRING, IDM_ATTACH,
                T(L"ローカルジョブに接続...", L"Attach to local job...",
                  L"Conectar a un trabajo local..."));
    AppendMenuW(menu, MF_STRING, IDM_CONNECT,
                T(L"リモートに接続...", L"Connect to remote...",
                  L"Conectar a remoto..."));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (tab ? 0 : MF_GRAYED), IDM_RECONNECT,
                T(L"現在のタブを再接続", L"Reconnect current tab",
                  L"Reconectar la pestaña actual"));
    AppendMenuW(menu, MF_STRING | (tab ? 0 : MF_GRAYED), IDM_CLOSE,
                T(L"現在のタブを閉じる", L"Close current tab",
                  L"Cerrar la pestaña actual"));

    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
                             pt.x, pt.y, 0, st->hwndMain, nullptr);
    DestroyMenu(menu);
    if (cmd != 0) SendMessageW(st->hwndMain, WM_COMMAND, static_cast<WPARAM>(cmd), 0);
}

LRESULT CALLBACK ClientWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<ClientState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_CREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        st = reinterpret_cast<ClientState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
        if (!st) return -1;
        st->hwndMain = hwnd;

        st->btnNew = AddDialogButton(hwnd, T(L"+ 新規", L"+ New", L"+ Nuevo"),
                                     IDC_CLIENT_BTN_NEW, 0, 0, 92, 26, false, true);
        st->btnAttach = AddDialogButton(hwnd, T(L"接続...", L"Attach...", L"Conectar..."),
                                        IDC_CLIENT_BTN_ATTACH, 0, 0, 92, 26, false, true);
        st->btnConnect = AddDialogButton(hwnd, T(L"リモート...", L"Remote...", L"Remoto..."),
                                         IDC_CLIENT_BTN_CONNECT, 0, 0, 100, 26, false, true);
        st->btnClose = AddDialogButton(hwnd, T(L"閉じる", L"Close", L"Cerrar"),
                                       IDC_CLIENT_BTN_CLOSE, 0, 0, 82, 26, false, true);

        st->hwndTab = CreateWindowExW(0, WC_TABCONTROLW, L"",
            WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_TABSTOP | TCS_OWNERDRAWFIXED,
            0, 0, 100, 28, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CLIENT_TAB)),
            GetModuleHandleW(nullptr), nullptr);
        if (st->hwndTab && g_uiFont) {
            SendMessageW(st->hwndTab, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
        }

        st->hwndStatus = CreateWindowExW(0, L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_OWNERDRAW,
            0, 0, 100, kStatusHeight, hwnd,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_CLIENT_STATUS)),
            GetModuleHandleW(nullptr), nullptr);
        if (st->hwndStatus && g_uiFont) {
            SendMessageW(st->hwndStatus, WM_SETFONT, reinterpret_cast<WPARAM>(g_uiFont), TRUE);
        }

        ApplyClientTheme(st, true);
        SetTimer(hwnd, 1, 1000, nullptr);
        UpdateStatus(st);
        return 0;
    }

    case WM_SIZE:
        if (st) {
            ApplyActiveLayout(st);
            UpdateStatus(st);
        }
        return 0;

    case WM_GETMINMAXINFO: {
        auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
        mmi->ptMinTrackSize.x = 640;
        mmi->ptMinTrackSize.y = 400;
        return 0;
    }

    case WM_ERASEBKGND: {
        if (g_bgBrush) {
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, g_bgBrush);
            return 1;
        }
        break;
    }

    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, g_palette.text);
        SetBkColor(hdc, g_palette.windowBg);
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(g_bgBrush ? g_bgBrush
                                                   : GetStockObject(WHITE_BRUSH));
    }

    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetTextColor(hdc, g_palette.listText);
        SetBkColor(hdc, g_palette.panel);
        return reinterpret_cast<LRESULT>(g_panelBrush ? g_panelBrush
                                                      : GetStockObject(WHITE_BRUSH));
    }

    case WM_DRAWITEM: {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (!dis) break;
        if (dis->CtlType == ODT_BUTTON) {
            DrawClientButton(dis);
            return TRUE;
        }
        if (dis->CtlType == ODT_TAB) {
            DrawClientTab(dis);
            return TRUE;
        }
        if (dis->CtlType == ODT_STATIC && st && dis->hwndItem == st->hwndStatus) {
            DrawClientStatus(st, dis);
            return TRUE;
        }
        break;
    }

    case WM_CONTEXTMENU: {
        if (!st) break;
        POINT pt = {};
        if (lParam == static_cast<LPARAM>(-1)) {
            GetCursorPos(&pt);
        } else {
            pt.x = GET_X_LPARAM(lParam);
            pt.y = GET_Y_LPARAM(lParam);
        }
        HWND target = WindowFromPoint(pt);
        wchar_t cls[64] = {};
        if (target) GetClassNameW(target, cls, 64);
        // The tab strip shows its own menu from NM_RCLICK.
        if (_wcsicmp(cls, L"SysTabControl32") == 0) return 0;
        ShowClientContextMenu(st, pt);
        return 0;
    }

    case WM_SETFOCUS:
        if (st) {
            SessionTab* tab = FindTab(st, st->activeId);
            if (tab && tab->view && tab->view->Hwnd()) {
                SetFocus(tab->view->Hwnd());
                return 0;
            }
        }
        break;

    case WM_COMMAND: {
        if (!st) break;
        const int id = LOWORD(wParam);
        switch (id) {
        case IDC_CLIENT_BTN_NEW:
        case IDM_NEW_PWSH:
            ConnectLocalShell(st, L"powershell");
            return 0;
        case IDM_NEW_CMD:
            ConnectLocalShell(st, L"cmd");
            return 0;
        case IDM_NEW_BASH:
            ConnectLocalShell(st, L"bash");
            return 0;
        case IDC_CLIENT_BTN_ATTACH:
        case IDM_ATTACH:
            ShowAttachPicker(st);
            return 0;
        case IDC_CLIENT_BTN_CONNECT:
        case IDM_CONNECT:
            ShowRemoteConnect(st);
            return 0;
        case IDC_CLIENT_BTN_CLOSE:
        case IDM_CLOSE:
            CloseTabById(st, st->activeId);
            return 0;
        case IDM_RECONNECT:
            ReconnectActive(st);
            return 0;
        case IDM_EDIT_COPY: {
            SessionTab* tab = FindTab(st, st->activeId);
            if (tab && tab->view) tab->view->CopySelection();
            return 0;
        }
        case IDM_EDIT_PASTE: {
            SessionTab* tab = FindTab(st, st->activeId);
            if (tab && tab->view) tab->view->PasteClipboard();
            return 0;
        }
        case IDM_AUTOATTACH:
            st->autoAttach = !st->autoAttach;
            if (st->hMenuSettings) {
                CheckMenuItem(st->hMenuSettings, IDM_AUTOATTACH,
                              MF_BYCOMMAND | (st->autoAttach ? MF_CHECKED : MF_UNCHECKED));
            }
            SaveClientSettings(st);
            return 0;
        case IDM_SET_TOPMOST:
            st->topmost = !st->topmost;
            SetWindowPos(hwnd, st->topmost ? HWND_TOPMOST : HWND_NOTOPMOST,
                         0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            if (st->hMenuSettings) {
                CheckMenuItem(st->hMenuSettings, IDM_SET_TOPMOST,
                              MF_BYCOMMAND | (st->topmost ? MF_CHECKED : MF_UNCHECKED));
            }
            SaveClientSettings(st);
            return 0;
        case IDM_SET_REMEMBER:
            st->rememberWindow = !st->rememberWindow;
            if (st->hMenuSettings) {
                CheckMenuItem(st->hMenuSettings, IDM_SET_REMEMBER,
                              MF_BYCOMMAND | (st->rememberWindow ? MF_CHECKED : MF_UNCHECKED));
            }
            SaveClientSettings(st);
            return 0;
        case IDM_SET_RESET_POS: {
            RECT wa = {};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            int w = 1100;
            int h = 720;
            if (w > wa.right - wa.left) w = wa.right - wa.left;
            if (h > wa.bottom - wa.top) h = wa.bottom - wa.top;
            int x = wa.left + ((wa.right - wa.left) - w) / 2;
            int y = wa.top + ((wa.bottom - wa.top) - h) / 2;
            SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
            st->hasSavedRect = false;
            SaveWindowState(st);
            return 0;
        }
        case IDM_SCROLLBACK_64:
        case IDM_SCROLLBACK_128:
        case IDM_SCROLLBACK_256:
        case IDM_SCROLLBACK_512:
        case IDM_SCROLLBACK_1024:
        case IDM_SCROLLBACK_2048: {
            int mb = 1024;
            if (id == IDM_SCROLLBACK_64) mb = 64;
            else if (id == IDM_SCROLLBACK_128) mb = 128;
            else if (id == IDM_SCROLLBACK_256) mb = 256;
            else if (id == IDM_SCROLLBACK_512) mb = 512;
            else if (id == IDM_SCROLLBACK_1024) mb = 1024;
            else if (id == IDM_SCROLLBACK_2048) mb = 2048;
            st->scrollbackMb = mb;
            const size_t bytes = static_cast<size_t>(mb) * 1024 * 1024;
            for (auto& tab : st->tabs) {
                if (tab && tab->view) tab->view->SetScrollbackBudget(1000000, bytes);
            }
            if (st->hMenuScrollback) {
                CheckMenuRadioItem(st->hMenuScrollback, IDM_SCROLLBACK_64,
                                   IDM_SCROLLBACK_2048, id, MF_BYCOMMAND);
            }
            SaveClientSettings(st);
            return 0;
        }
        case IDM_EXIT:
            SaveWindowState(st);
            DestroyWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;
    }

    case WM_NOTIFY: {
        if (!st) break;
        auto* hdr = reinterpret_cast<NMHDR*>(lParam);
        if (hdr && hdr->idFrom == IDC_CLIENT_TAB && hdr->hwndFrom == st->hwndTab) {
            if (hdr->code == TCN_SELCHANGE) {
                int index = TabCtrl_GetCurSel(st->hwndTab);
                if (index >= 0 && index < static_cast<int>(st->tabs.size())) {
                    st->activeId = st->tabs[static_cast<size_t>(index)]->id;
                    ApplyActiveLayout(st);
                    UpdateStatus(st);
                }
            } else if (hdr->code == NM_RCLICK) {
                TCHITTESTINFO hit = {};
                GetCursorPos(&hit.pt);
                ScreenToClient(st->hwndTab, &hit.pt);
                int index = TabCtrl_HitTest(st->hwndTab, &hit);
                if (index >= 0 && index < static_cast<int>(st->tabs.size())) {
                    st->activeId = st->tabs[static_cast<size_t>(index)]->id;
                    TabCtrl_SetCurSel(st->hwndTab, index);
                    ApplyActiveLayout(st);
                    UpdateStatus(st);

                    POINT pt = {};
                    GetCursorPos(&pt);
                    ShowClientContextMenu(st, pt);
                }
            }
        }
        break;
    }

    case WM_CLIENT_SESSION_EXIT: {
        if (st) {
            SessionTab* tab = FindTab(st, static_cast<int>(wParam));
            if (tab) {
                tab->state = SessionTab::State::Exited;
                tab->exitCode = static_cast<int>(lParam);
                tab->detail = tab->view ? tab->view->LastError() : std::wstring();
                const int index = FindTabIndex(st, tab->id);
                SetTabItemText(st, index);
                if (st->activeId == tab->id) UpdateStatus(st);
            }
        }
        return 0;
    }

    case WM_CLIENT_SESSION_ERROR: {
        if (st) {
            SessionTab* tab = FindTab(st, static_cast<int>(wParam));
            if (tab) {
                tab->state = SessionTab::State::Failed;
                tab->detail = tab->view ? tab->view->LastError() : std::wstring();
                const int index = FindTabIndex(st, tab->id);
                SetTabItemText(st, index);
                if (st->activeId == tab->id) UpdateStatus(st);
            }
        }
        return 0;
    }

    case WM_TIMER:
        if (st) {
            UpdateTabTitles(st);
            if (++st->autoAttachTicks >= 2) {
                st->autoAttachTicks = 0;
                AutoAttachLocalJobs(st);
            }
            if (++st->themeTicks >= 3) {
                st->themeTicks = 0;
                ApplyClientTheme(st, false);
            }
            UpdateStatus(st);
        }
        return 0;

    case WM_CLOSE:
        if (st) SaveWindowState(st);
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (st) {
            KillTimer(hwnd, 1);
            CloseAllTabs(st);
        }
        if (g_uiFont) {
            DeleteObject(g_uiFont);
            g_uiFont = nullptr;
        }
        PostQuitMessage(0);
        return 0;

    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void RegisterWindowClasses(HINSTANCE hInst) {
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = ClientWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    wc.lpszClassName = ClientWindowClass().c_str();
    RegisterClassExW(&wc);

    WNDCLASSEXW dlg = {};
    dlg.cbSize = sizeof(dlg);
    dlg.lpfnWndProc = ConnectDlgProc;
    dlg.hInstance = hInst;
    dlg.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    dlg.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    dlg.lpszClassName = kConnectDlgClass;
    RegisterClassExW(&dlg);

    dlg.lpfnWndProc = AttachDlgProc;
    dlg.lpszClassName = kAttachDlgClass;
    RegisterClassExW(&dlg);
}

void CreateUiFont() {
    NONCLIENTMETRICSW metrics = {};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        g_uiFont = CreateFontIndirectW(&metrics.lfMessageFont);
    }
    if (!g_uiFont) {
        g_uiFont = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
    }
}

} // namespace

bool LaunchTerminalClientWindow() {
    HWND existing = FindWindowW(ClientWindowClass().c_str(), nullptr);
    if (existing) {
        if (IsIconic(existing)) ShowWindow(existing, SW_RESTORE);
        else ShowWindow(existing, SW_SHOW);
        SetForegroundWindow(existing);
        return true;
    }

    wchar_t exePath[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return false;

    std::wstring command;
    command.reserve(len + 64);
    command += L'"';
    command += exePath;
    command += L"\" terminal";
    const std::wstring dataDir = GetDataDirectoryOverride();
    if (!dataDir.empty()) {
        command += L" --data-dir \"";
        command += dataDir;
        command += L'"';
    }

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    if (!ok) return false;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

int RunTerminalClient(const TermClientOptions& options) {
    SetProcessDPIAware();

    // The client is a GUI app; hide the console when it was started from one.
    if (HWND console = GetConsoleWindow()) ShowWindow(console, SW_HIDE);

    INITCOMMONCONTROLSEX icex = {};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_TAB_CLASSES | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES;
    InitCommonControlsEx(&icex);

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    TerminalView::RegisterWindowClass(hInst);
    DetectLanguage();
    g_palette = GetDashboardPalette();
    InitDashboardDarkMode(g_palette.dark);
    CreateUiFont();
    RegisterWindowClasses(hInst);

    ClientState state;
    LoadClientSettings(&state);
    if (!options.autoAttach) state.autoAttach = false;

    HMENU menu = BuildMenu(&state);
    HWND hwnd = CreateWindowExW(
        0, ClientWindowClass().c_str(),
        T(L"Hancho ターミナル - WinConsoleServer",
          L"Hancho Terminal - WinConsoleServer",
          L"Hancho Terminal - WinConsoleServer"),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1100, 720,
        nullptr, menu, hInst, &state);
    if (!hwnd) return 1;

    if (state.rememberWindow && state.hasSavedRect) {
        RECT rc = state.savedRect;
        if (MonitorFromRect(&rc, MONITOR_DEFAULTTONULL)) {
            SetWindowPos(hwnd, nullptr, rc.left, rc.top,
                         rc.right - rc.left, rc.bottom - rc.top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }
    if (state.topmost) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    if (options.autoConnect) {
        const std::wstring target =
            Utf8ToWide(options.target.empty() ? "powershell" : options.target);
        std::wstring error;
        if (options.local && options.host.empty() && options.port == 0) {
            if (target.rfind(L"attach:", 0) == 0) {
                std::wstring number = target.substr(7);
                int jobId = static_cast<int>(std::wcstol(number.c_str(), nullptr, 10));
                ConnectLocalAttach(&state, jobId, target);
            } else {
                ConnectLocalShell(&state, target.c_str());
            }
        } else {
            RemoteSettings settings;
            settings.host = Utf8ToWide(options.host.empty() ? "127.0.0.1" : options.host);
            settings.port = options.port > 0 ? options.port : 9139;
            settings.token = Utf8ToWide(options.token);
            state.lastRemote = settings;
            state.lastTarget = target;
            SaveClientSettings(&state);
            if (AddSession(&state, settings, target,
                           DefaultLabelFor(target, settings.host, false), 0, error) == 0) {
                MessageBoxW(hwnd, error.c_str(),
                            T(L"ターミナル", L"Terminal", L"Terminal"),
                            MB_OK | MB_ICONERROR);
            }
        }
    }

    ACCEL accels[2] = {};
    accels[0].fVirt = FVIRTKEY | FCONTROL;
    accels[0].key = 'T';
    accels[0].cmd = IDM_NEW_PWSH;
    accels[1].fVirt = FVIRTKEY | FCONTROL;
    accels[1].key = 'W';
    accels[1].cmd = IDM_CLOSE;
    HACCEL hAccel = CreateAcceleratorTableW(accels, 2);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (hAccel && TranslateAcceleratorW(hwnd, hAccel, &msg)) continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (hAccel) DestroyAcceleratorTable(hAccel);
    return static_cast<int>(msg.wParam);
}

} // namespace pc
