#include "gui_dashboard.h"
#include "common.h"
#include "llm_stream.h"
#include "event_schema.h"
#include "activity_tree.h"
#include "bt_debug_view.h"
#include "memory_record.h"
#include "memory_search.h"
#include "TerminalView.h"
#include "TerminalGridView.h"
#include "SettingsManager.h"
#include "remote_hosts.h"
#include "ipc_client.h"
#include "../tests/remote_ws_test.h"
#include "../tests/gui_platform.h"
#include "nlohmann/json.hpp"
#include <commctrl.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <map>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <chrono>
#include <unordered_map>
#include <unordered_set>
#include <ctime>
#include <cstdio>
#include <cmath>

// ---------------------------------------------------------------------------
// Globals required by src/Terminal/src/TerminalGridView.cpp
// ---------------------------------------------------------------------------
struct AppTabInfo {
    HWND hwnd = nullptr;
    std::wstring label;
    int type;
    void* data = nullptr;
    HANDLE hProcess = nullptr;
};
std::vector<AppTabInfo> g_appTabs;
int g_activeAppTab = 0;
HWND g_mainHwnd = nullptr;
HWND g_tabHwnd = nullptr;
bool g_suppressTabChange = false;
void UpdateMenu(HWND) {}
void UpdateTabs(HWND) {}

#define TAB_TYPE_TERMINAL 10
#define WM_DEFERRED_FOCUS (WM_USER + 207)

namespace pc {

constexpr UINT WM_PC_IPC_EVENT        = WM_APP + 101;
constexpr UINT WM_PC_CREATE_TERMVIEW  = WM_APP + 102;
constexpr UINT WM_PC_DESTROY_TERMVIEW = WM_APP + 103;
constexpr UINT WM_PC_TRAYICON         = WM_APP + 104;
constexpr UINT_PTR TIMER_REFRESH_ID   = 1;
// Periodic dashboard / terminal-grid refresh interval (ms). User-configurable in the Settings dialog.
constexpr int kDefaultRefreshIntervalMs = 2000;
constexpr int kMinRefreshIntervalMs = 100;
constexpr int kMaxRefreshIntervalMs = 60000;
static int ClampRefreshIntervalMs(int ms) {
    return (std::clamp)(ms, kMinRefreshIntervalMs, kMaxRefreshIntervalMs);
}
constexpr UINT_PTR TIMER_KPI_ANIM_ID  = 2;
constexpr UINT_PTR TIMER_END_BLINK_ID = 3;     // list rows blinking after a process/task ends
constexpr UINT_PTR TIMER_HINT_COUNTDOWN_ID = 4; // countdown for supervisor guidance hint popup
constexpr UINT kEndBlinkTimerMs = 200;         // repaint period while a blink is running
constexpr ULONGLONG kEndBlinkHalfPeriodMs = 400;  // on/off toggle period of the blink
constexpr int kDefaultEndBlinkSec = 5;
constexpr UINT TRAY_ICON_ID           = 1;
constexpr int  APP_ICON_RESOURCE_ID   = 1;

static HICON LoadAppIcon() {
    HICON hIcon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(APP_ICON_RESOURCE_ID),
        IMAGE_ICON, 0, 0, LR_DEFAULTSIZE | LR_SHARED));
    return hIcon ? hIcon : LoadIconW(nullptr, IDI_APPLICATION);
}

static HICON LoadAppSmallIcon() {
    HICON hIcon = static_cast<HICON>(LoadImageW(
        GetModuleHandleW(nullptr), MAKEINTRESOURCEW(APP_ICON_RESOURCE_ID),
        IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
        LR_SHARED));
    return hIcon ? hIcon : LoadIconW(nullptr, IDI_APPLICATION);
}

enum class UiLanguage { Japanese, English, Spanish };
enum class UiTheme { System, Dark, Light, Pastel, HighContrast, Midnight, Ember, Berry, Crimson, Forest, Mist,
    PastelBlue, PastelOrange, PastelPink, PastelRed, PastelGreen, PastelPurple, PastelYellow,
    DeepPurple, DeepYellow };

struct ThemeColors {
    COLORREF windowBg;
    COLORREF panel;
    COLORREF border;
    COLORREF text;
    COLORREF muted;
    COLORREF accent;
    COLORREF terminalBg;
    COLORREF emptyBg;
    COLORREF listText;
    COLORREF rowSelected;
    COLORREF rowRunning;
    COLORREF rowIdle;
    COLORREF rowCompleted;
    COLORREF rowFailed;
};

static UiLanguage g_uiLanguage = UiLanguage::English;
static UiTheme g_uiTheme = UiTheme::System;
static float g_kpiSaturation = 1.0f;
// Colour the execution monitor rows by activity type and show icons in activity labels.
static bool g_activityStyle = true;
static bool g_darkThemeActive = false;  // cached by ApplyTheme for cheap per-row colour picks
static ThemeColors g_theme = {};
static HBRUSH g_dialogBgBrush = nullptr;

static UiLanguage DetectDefaultLanguage() {
    wchar_t buf[16] = {};
    DWORD n = GetEnvironmentVariableW(L"PC_UI_LANG", buf, 16);
    if (n > 0) {
        if (_wcsnicmp(buf, L"ja", 2) == 0) return UiLanguage::Japanese;
        if (_wcsnicmp(buf, L"es", 2) == 0) return UiLanguage::Spanish;
        return UiLanguage::English;
    }
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
        case LANG_JAPANESE: return UiLanguage::Japanese;
        case LANG_SPANISH:  return UiLanguage::Spanish;
        default:            return UiLanguage::English;
    }
}

static bool IsSystemDarkMode() {
    DWORD value = 1;
    DWORD size = sizeof(value);
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            0, KEY_READ, &key) == ERROR_SUCCESS) {
        RegQueryValueExW(key, L"AppsUseLightTheme", nullptr, nullptr,
                         reinterpret_cast<LPBYTE>(&value), &size);
        RegCloseKey(key);
    }
    return value == 0;
}

static COLORREF GetSystemAccentColor() {
    DWORD color = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&color, &opaque))) {
        return RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
    }
    return GetSysColor(COLOR_HIGHLIGHT);
}

static ThemeColors ThemeFor(UiTheme theme) {
    switch (theme) {
        case UiTheme::System: {
            ThemeColors base = ThemeFor(IsSystemDarkMode() ? UiTheme::Dark : UiTheme::Light);
            base.accent = GetSystemAccentColor();
            return base;
        }
        case UiTheme::Light:
            return {
                RGB(241, 245, 249), RGB(255, 255, 255), RGB(203, 213, 225),
                RGB(15, 23, 42), RGB(71, 85, 105), RGB(2, 132, 199),
                RGB(15, 23, 42), RGB(226, 232, 240), RGB(15, 23, 42), RGB(229, 231, 235),
                RGB(220, 252, 231), RGB(254, 243, 199), RGB(219, 234, 254), RGB(254, 226, 226)
            };
        case UiTheme::Pastel:
            return {
                RGB(243, 235, 220), RGB(250, 246, 236), RGB(214, 204, 224),
                RGB(74, 66, 84), RGB(130, 118, 140), RGB(178, 148, 210),
                RGB(34, 30, 40), RGB(243, 236, 248), RGB(74, 66, 84), RGB(232, 230, 235),
                RGB(220, 244, 226), RGB(252, 238, 210), RGB(220, 235, 250), RGB(250, 220, 224)
            };
        case UiTheme::HighContrast:
            return {
                RGB(0, 0, 0), RGB(20, 20, 20), RGB(255, 255, 255),
                RGB(255, 255, 255), RGB(220, 220, 220), RGB(255, 255, 0),
                RGB(0, 0, 0), RGB(0, 0, 0), RGB(255, 255, 255), RGB(60, 60, 60),
                RGB(0, 40, 0), RGB(50, 40, 0), RGB(0, 20, 50), RGB(50, 0, 0)
            };
        case UiTheme::Midnight:
            return {
                RGB(12, 22, 44), RGB(20, 34, 64), RGB(48, 68, 104),
                RGB(220, 228, 240), RGB(140, 156, 184), RGB(96, 165, 250),
                RGB(8, 15, 32), RGB(16, 28, 54), RGB(220, 228, 240), RGB(38, 62, 104),
                RGB(20, 52, 58), RGB(40, 44, 64), RGB(24, 40, 80), RGB(60, 30, 46)
            };
        case UiTheme::Ember:
            return {
                RGB(38, 20, 8), RGB(60, 32, 14), RGB(104, 62, 32),
                RGB(250, 232, 214), RGB(190, 150, 120), RGB(249, 115, 22),
                RGB(26, 13, 4), RGB(46, 24, 10), RGB(250, 232, 214), RGB(110, 58, 20),
                RGB(40, 62, 30), RGB(70, 50, 20), RGB(40, 40, 70), RGB(80, 28, 24)
            };
        case UiTheme::Berry:
            return {
                RGB(38, 14, 30), RGB(60, 22, 46), RGB(104, 46, 80),
                RGB(250, 226, 240), RGB(196, 150, 176), RGB(236, 72, 153),
                RGB(26, 8, 20), RGB(46, 18, 38), RGB(250, 226, 240), RGB(110, 36, 80),
                RGB(34, 60, 50), RGB(70, 50, 30), RGB(40, 36, 80), RGB(84, 26, 44)
            };
        case UiTheme::Crimson:
            return {
                RGB(36, 10, 12), RGB(60, 18, 22), RGB(108, 40, 46),
                RGB(250, 226, 226), RGB(196, 140, 144), RGB(239, 68, 68),
                RGB(24, 6, 8), RGB(44, 14, 16), RGB(250, 226, 226), RGB(110, 30, 36),
                RGB(34, 58, 44), RGB(70, 52, 24), RGB(36, 40, 72), RGB(96, 24, 28)
            };
        case UiTheme::Forest:
            return {
                RGB(10, 30, 20), RGB(18, 50, 34), RGB(46, 96, 70),
                RGB(220, 244, 232), RGB(140, 186, 162), RGB(34, 197, 94),
                RGB(6, 20, 13), RGB(14, 40, 27), RGB(220, 244, 232), RGB(36, 96, 64),
                RGB(26, 80, 50), RGB(60, 64, 28), RGB(24, 52, 70), RGB(70, 36, 30)
            };
        case UiTheme::Mist:
            return {
                RGB(226, 228, 231), RGB(240, 241, 243), RGB(190, 194, 200),
                RGB(31, 36, 44), RGB(92, 99, 112), RGB(70, 100, 140),
                RGB(28, 31, 38), RGB(222, 225, 230), RGB(31, 36, 44), RGB(206, 212, 222),
                RGB(220, 242, 228), RGB(246, 236, 212), RGB(220, 230, 246), RGB(248, 222, 222)
            };
        case UiTheme::PastelBlue:
            return {
                RGB(220, 235, 250), RGB(238, 246, 253), RGB(188, 208, 232),
                RGB(40, 58, 84), RGB(92, 112, 140), RGB(86, 140, 200),
                RGB(24, 32, 44), RGB(230, 240, 251), RGB(40, 58, 84), RGB(204, 222, 242),
                RGB(220, 244, 226), RGB(252, 238, 210), RGB(220, 235, 250), RGB(250, 220, 224)
            };
        case UiTheme::PastelOrange:
            return {
                RGB(253, 232, 212), RGB(254, 244, 234), RGB(232, 204, 176),
                RGB(84, 58, 36), RGB(140, 108, 80), RGB(222, 136, 72),
                RGB(40, 30, 22), RGB(253, 238, 224), RGB(84, 58, 36), RGB(245, 218, 192),
                RGB(220, 244, 226), RGB(252, 238, 210), RGB(220, 235, 250), RGB(250, 220, 224)
            };
        case UiTheme::PastelPink:
            return {
                RGB(252, 226, 238), RGB(254, 242, 248), RGB(232, 192, 212),
                RGB(84, 44, 66), RGB(140, 96, 120), RGB(216, 104, 160),
                RGB(40, 26, 34), RGB(253, 234, 243), RGB(84, 44, 66), RGB(244, 210, 226),
                RGB(220, 244, 226), RGB(252, 238, 210), RGB(220, 235, 250), RGB(250, 220, 224)
            };
        case UiTheme::PastelRed:
            return {
                RGB(250, 218, 218), RGB(253, 238, 238), RGB(228, 184, 184),
                RGB(84, 40, 42), RGB(140, 92, 94), RGB(208, 88, 92),
                RGB(40, 24, 24), RGB(251, 228, 228), RGB(84, 40, 42), RGB(240, 202, 202),
                RGB(220, 244, 226), RGB(252, 238, 210), RGB(220, 235, 250), RGB(250, 220, 224)
            };
        case UiTheme::PastelGreen:
            return {
                RGB(220, 242, 222), RGB(238, 249, 239), RGB(186, 218, 190),
                RGB(36, 68, 44), RGB(88, 124, 96), RGB(84, 164, 104),
                RGB(22, 36, 26), RGB(230, 246, 232), RGB(36, 68, 44), RGB(204, 232, 208),
                RGB(220, 244, 226), RGB(252, 238, 210), RGB(220, 235, 250), RGB(250, 220, 224)
            };
        case UiTheme::PastelPurple:
            return {
                RGB(232, 224, 250), RGB(245, 241, 253), RGB(206, 194, 232),
                RGB(58, 44, 92), RGB(112, 98, 146), RGB(140, 108, 210),
                RGB(30, 24, 46), RGB(239, 233, 251), RGB(58, 44, 92), RGB(220, 208, 244),
                RGB(220, 244, 226), RGB(252, 238, 210), RGB(220, 235, 250), RGB(250, 220, 224)
            };
        case UiTheme::PastelYellow:
            return {
                RGB(252, 245, 200), RGB(254, 251, 228), RGB(228, 216, 150),
                RGB(78, 66, 20), RGB(130, 116, 64), RGB(204, 164, 30),
                RGB(38, 34, 18), RGB(253, 248, 214), RGB(78, 66, 20), RGB(244, 232, 170),
                RGB(220, 244, 226), RGB(252, 238, 210), RGB(220, 235, 250), RGB(250, 220, 224)
            };
        case UiTheme::DeepPurple:
            return {
                RGB(36, 18, 64), RGB(54, 30, 92), RGB(96, 68, 146),
                RGB(236, 226, 252), RGB(170, 150, 204), RGB(168, 120, 250),
                RGB(22, 10, 42), RGB(44, 24, 78), RGB(236, 226, 252), RGB(90, 54, 150),
                RGB(30, 66, 62), RGB(72, 52, 54), RGB(40, 44, 96), RGB(90, 30, 62)
            };
        case UiTheme::DeepYellow:
            return {
                RGB(84, 66, 4), RGB(108, 86, 10), RGB(168, 138, 40),
                RGB(255, 247, 208), RGB(216, 196, 130), RGB(250, 204, 21),
                RGB(34, 26, 0), RGB(72, 56, 2), RGB(255, 247, 208), RGB(150, 118, 14),
                RGB(52, 92, 40), RGB(128, 92, 12), RGB(46, 74, 96), RGB(124, 46, 26)
            };
        case UiTheme::Dark:
        default:
            return {
                RGB(10, 10, 12), RGB(18, 18, 20), RGB(50, 50, 54),
                RGB(202, 202, 208), RGB(145, 145, 150), RGB(95, 125, 145),
                RGB(0, 0, 0), RGB(12, 12, 14), RGB(202, 202, 208), RGB(55, 55, 60),
                RGB(18, 30, 24), RGB(34, 30, 20), RGB(20, 28, 36), RGB(36, 22, 24)
            };
    }
}

static bool IsPastelTheme(UiTheme theme) {
    return theme == UiTheme::Pastel || (theme >= UiTheme::PastelBlue && theme <= UiTheme::PastelYellow);
}

static bool IsDarkUiTheme(UiTheme theme) {
    switch (theme) {
        case UiTheme::Dark:
        case UiTheme::HighContrast:
        case UiTheme::Midnight:
        case UiTheme::Ember:
        case UiTheme::Berry:
        case UiTheme::Crimson:
        case UiTheme::Forest:
        case UiTheme::DeepPurple:
        case UiTheme::DeepYellow:
            return true;
        case UiTheme::System:
            return IsSystemDarkMode();
        default:
            return false;
    }
}

static void ApplyTheme() {
    g_theme = ThemeFor(g_uiTheme);
    g_darkThemeActive = IsDarkUiTheme(g_uiTheme);
    if (g_dialogBgBrush) {
        DeleteObject(g_dialogBgBrush);
    }
    g_dialogBgBrush = CreateSolidBrush(g_theme.panel);
}
static bool IsDarkThemeActive() {
    return IsDarkUiTheme(g_uiTheme);
}

using SetPreferredAppModeFn = int(WINAPI*)(int);
using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);

static void ApplyDarkModeApi(HWND hwnd, bool dark) {
    static bool initialized = false;
    static SetPreferredAppModeFn setPreferredAppMode = nullptr;
    static AllowDarkModeForWindowFn allowDarkModeForWindow = nullptr;
    if (!initialized) {
        HMODULE ux = LoadLibraryW(L"uxtheme.dll");
        if (ux) {
            setPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(
                GetProcAddress(ux, MAKEINTRESOURCEA(135)));
            allowDarkModeForWindow = reinterpret_cast<AllowDarkModeForWindowFn>(
                GetProcAddress(ux, MAKEINTRESOURCEA(133)));
        }
        initialized = true;
    }
    if (setPreferredAppMode) setPreferredAppMode(dark ? 2 : 3);
    if (allowDarkModeForWindow && hwnd) allowDarkModeForWindow(hwnd, dark ? TRUE : FALSE);
}

static void ApplyNativeControlTheme(HWND hwnd, bool dark) {
    if (!hwnd) return;
    ApplyDarkModeApi(hwnd, dark);
    SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : L"", dark ? nullptr : L"");
}

static void ApplyNativeThemeToWindow(HWND hwnd) {
    if (!hwnd) return;
    bool dark = IsDarkThemeActive();
    BOOL darkFlag = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20, &darkFlag, sizeof(darkFlag));
    DwmSetWindowAttribute(hwnd, 19, &darkFlag, sizeof(darkFlag));
    ApplyNativeControlTheme(hwnd, dark);
    for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        ApplyNativeControlTheme(child, dark);
        if (GetWindow(child, GW_CHILD)) {
            for (HWND grand = GetWindow(child, GW_CHILD); grand; grand = GetWindow(grand, GW_HWNDNEXT)) {
                ApplyNativeControlTheme(grand, dark);
            }
        }
    }
}

static COLORREF BlendColor(COLORREF from, COLORREF to, float t);

// List header background: the window colour with a slight tint of the theme accent.
static COLORREF HeaderBgColor() {
    return BlendColor(g_theme.windowBg, g_theme.accent, 0.12f);
}

static void DrawHeaderItem(DRAWITEMSTRUCT* dis) {
    if (!dis || !dis->hwndItem) return;
    wchar_t text[256] = {};
    HDITEMW hdiText = {};
    hdiText.mask = HDI_TEXT;
    hdiText.pszText = text;
    hdiText.cchTextMax = 256;
    SendMessageW(dis->hwndItem, HDM_GETITEMW, dis->itemID, reinterpret_cast<LPARAM>(&hdiText));
    HBRUSH bgBrush = CreateSolidBrush(HeaderBgColor());
    FillRect(dis->hDC, &dis->rcItem, bgBrush);
    DeleteObject(bgBrush);

    SetBkMode(dis->hDC, TRANSPARENT);
    SetTextColor(dis->hDC, g_theme.text);
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(GetParent(dis->hwndItem), WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = SelectObject(dis->hDC, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    RECT rc = dis->rcItem;
    InflateRect(&rc, -8, -1);
    DrawTextW(dis->hDC, text, -1, &rc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dis->hDC, oldFont);

    HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
    HGDIOBJ oldPen = SelectObject(dis->hDC, borderPen);
    MoveToEx(dis->hDC, dis->rcItem.right - 1, dis->rcItem.top, nullptr);
    LineTo(dis->hDC, dis->rcItem.right - 1, dis->rcItem.bottom);
    MoveToEx(dis->hDC, dis->rcItem.left, dis->rcItem.bottom - 1, nullptr);
    LineTo(dis->hDC, dis->rcItem.right, dis->rcItem.bottom - 1);
    SelectObject(dis->hDC, oldPen);
    DeleteObject(borderPen);
}

static LRESULT CALLBACK HeaderSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                           UINT_PTR, DWORD_PTR) {
    if (msg == WM_ERASEBKGND) {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        HBRUSH brush = CreateSolidBrush(HeaderBgColor());
        FillRect(hdc, &rc, brush);
        DeleteObject(brush);
        return 1;
    }
    LRESULT res = DefSubclassProc(hwnd, msg, wParam, lParam);
    if (msg == WM_PAINT) {
        int count = Header_GetItemCount(hwnd);
        if (count > 0) {
            RECT rcLast = {};
            if (Header_GetItemRect(hwnd, count - 1, &rcLast)) {
                RECT rcClient = {};
                GetClientRect(hwnd, &rcClient);
                if (rcLast.right < rcClient.right) {
                    HDC hdc = GetDC(hwnd);
                    RECT rcEmpty = { rcLast.right, rcClient.top, rcClient.right, rcClient.bottom };
                    HBRUSH brush = CreateSolidBrush(HeaderBgColor());
                    FillRect(hdc, &rcEmpty, brush);
                    DeleteObject(brush);
                    HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
                    HGDIOBJ oldPen = SelectObject(hdc, borderPen);
                    MoveToEx(hdc, rcEmpty.left, rcEmpty.bottom - 1, nullptr);
                    LineTo(hdc, rcEmpty.right, rcEmpty.bottom - 1);
                    SelectObject(hdc, oldPen);
                    DeleteObject(borderPen);
                    ReleaseDC(hwnd, hdc);
                }
            }
        }
    }
    return res;
}

// Draws the thumb of one native scrollbar inside its track rectangle, from the control's scroll info.
static void PaintListScrollThumb(HDC hdc, HWND hwnd, int which, const RECT& bar, HBRUSH thumbBrush) {
    SCROLLINFO si = {};
    si.cbSize = sizeof(si);
    si.fMask = SIF_ALL;
    if (!GetScrollInfo(hwnd, which, &si)) return;
    int range = si.nMax - si.nMin + 1;
    if (si.nPage == 0 || range <= 0 || static_cast<int>(si.nPage) >= range) return;
    bool vert = which == SB_VERT;
    int trackLen = vert ? (bar.bottom - bar.top) : (bar.right - bar.left);
    int thumbLen = (std::max)(16, trackLen * static_cast<int>(si.nPage) / range);
    int scrollable = range - static_cast<int>(si.nPage);
    int thumbPos = (trackLen - thumbLen) * (si.nPos - si.nMin) / scrollable;
    thumbPos = (std::clamp)(thumbPos, 0, trackLen - thumbLen);
    RECT thumb = bar;
    if (vert) {
        thumb.top += thumbPos;
        thumb.bottom = thumb.top + thumbLen;
        thumb.left += 3;
        thumb.right -= 3;
    } else {
        thumb.left += thumbPos;
        thumb.right = thumb.left + thumbLen;
        thumb.top += 3;
        thumb.bottom -= 3;
    }
    FillRect(hdc, &thumb, thumbBrush);
}

// Composes one scrollbar off screen and copies it in one step, so the bare track is never visible.
static void PaintListScrollBarBuffered(HDC hdc, HWND hwnd, int which, const RECT& bar,
                                       HBRUSH trackBrush, HBRUSH thumbBrush) {
    int w = bar.right - bar.left;
    int h = bar.bottom - bar.top;
    if (w <= 0 || h <= 0) return;
    HDC memDc = CreateCompatibleDC(hdc);
    HBITMAP bmp = CreateCompatibleBitmap(hdc, w, h);
    if (memDc && bmp) {
        HGDIOBJ oldBmp = SelectObject(memDc, bmp);
        RECT local = {0, 0, w, h};
        FillRect(memDc, &local, trackBrush);
        PaintListScrollThumb(memDc, hwnd, which, local, thumbBrush);
        BitBlt(hdc, bar.left, bar.top, w, h, memDc, 0, 0, SRCCOPY);
        SelectObject(memDc, oldBmp);
    }
    if (bmp) DeleteObject(bmp);
    if (memDc) DeleteDC(memDc);
}

// The native list scrollbars are painted by the system in the theme's default colours. Repaint them
// over with the active theme after each message, so the track and thumb follow the theme.
static void PaintThemedListScrollBars(HWND hwnd) {
    DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    bool vert = (style & WS_VSCROLL) != 0;
    bool horz = (style & WS_HSCROLL) != 0;
    if (!vert && !horz) return;
    HDC hdc = GetWindowDC(hwnd);
    if (!hdc) return;
    RECT wr = {};
    GetWindowRect(hwnd, &wr);
    RECT cr = {};
    GetClientRect(hwnd, &cr);
    POINT origin = {0, 0};
    ClientToScreen(hwnd, &origin);
    int offX = origin.x - wr.left;
    int offY = origin.y - wr.top;
    int cw = cr.right - cr.left;
    int ch = cr.bottom - cr.top;
    int sbw = GetSystemMetrics(SM_CXVSCROLL);
    int sbh = GetSystemMetrics(SM_CYHSCROLL);
    HBRUSH trackBrush = CreateSolidBrush(g_theme.panel);
    HBRUSH thumbBrush = CreateSolidBrush(g_theme.border);
    if (vert) {
        RECT bar = {offX + cw, offY, offX + cw + sbw, offY + ch - (horz ? sbh : 0)};
        PaintListScrollBarBuffered(hdc, hwnd, SB_VERT, bar, trackBrush, thumbBrush);
    }
    if (horz) {
        RECT bar = {offX, offY + ch, offX + cw - (vert ? sbw : 0), offY + ch + sbh};
        PaintListScrollBarBuffered(hdc, hwnd, SB_HORZ, bar, trackBrush, thumbBrush);
    }
    if (vert && horz) {
        RECT corner = {offX + cw, offY + ch, offX + cw + sbw, offY + ch + sbh};
        HBRUSH cornerBrush = CreateSolidBrush(g_theme.windowBg);
        FillRect(hdc, &corner, cornerBrush);
        DeleteObject(cornerBrush);
    }
    DeleteObject(trackBrush);
    DeleteObject(thumbBrush);
    ReleaseDC(hwnd, hdc);
}

// The control whose scrollbar is held with the mouse. Timers still fire inside the scrollbar's modal
// loop, so refreshes check this and leave that control alone until the button is released.
static HWND g_scrollDragWnd = nullptr;

static LRESULT CALLBACK ListViewSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                             UINT_PTR, DWORD_PTR) {
    if (msg == WM_DRAWITEM) {
        auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
        if (dis && dis->CtlType == ODT_HEADER) {
            DrawHeaderItem(dis);
            return TRUE;
        }
    }
    bool scrollBarPress = msg == WM_NCLBUTTONDOWN && (wParam == HTVSCROLL || wParam == HTHSCROLL);
    if (scrollBarPress) g_scrollDragWnd = hwnd;
    LRESULT res = DefSubclassProc(hwnd, msg, wParam, lParam);
    if (scrollBarPress) g_scrollDragWnd = nullptr;
    if (msg != WM_NCDESTROY && msg != WM_DESTROY) PaintThemedListScrollBars(hwnd);
    return res;
}

static LRESULT CALLBACK TabSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                        UINT_PTR, DWORD_PTR) {
    if (msg == WM_ERASEBKGND) {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        HBRUSH brush = CreateSolidBrush(g_theme.windowBg);
        FillRect(hdc, &rc, brush);
        DeleteObject(brush);
        return 1;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static COLORREF BlendColor(COLORREF from, COLORREF to, float t) {
    auto mix = [t](int a, int b) { return static_cast<int>(a + (b - a) * t + 0.5f); };
    return RGB(mix(GetRValue(from), GetRValue(to)),
               mix(GetGValue(from), GetGValue(to)),
               mix(GetBValue(from), GetBValue(to)));
}

// Thin row separator drawn in place of LVS_EX_GRIDLINES, softened toward the window background.
static void DrawListRowSeparator(const NMLVCUSTOMDRAW* lvcd) {
    RECT rc = {};
    if (!ListView_GetItemRect(lvcd->nmcd.hdr.hwndFrom, lvcd->nmcd.dwItemSpec, &rc, LVIR_BOUNDS)) return;
    HDC hdc = lvcd->nmcd.hdc;
    HPEN pen = CreatePen(PS_SOLID, 1, BlendColor(g_theme.border, g_theme.windowBg, 0.5f));
    HGDIOBJ oldPen = SelectObject(hdc, pen);
    MoveToEx(hdc, rc.left, rc.bottom - 1, nullptr);
    LineTo(hdc, rc.right, rc.bottom - 1);
    SelectObject(hdc, oldPen);
    DeleteObject(pen);
}

// Low-saturation variant of a theme row color (keeps brightness, mutes the hue).
static COLORREF MutedRowColor(COLORREF c) {
    int r = GetRValue(c);
    int g = GetGValue(c);
    int b = GetBValue(c);
    int gray = (r * 299 + g * 587 + b * 114) / 1000;
    return BlendColor(c, RGB(gray, gray, gray), 0.6f);
}


static const wchar_t* Tr(const wchar_t* ja, const wchar_t* en, const wchar_t* es = nullptr) {
    if (g_uiLanguage == UiLanguage::Spanish) return es ? es : en;
    if (g_uiLanguage == UiLanguage::English) return en;
    return ja;
}

// Small caption bar above each list so it is clear which list is which.
static void DrawListCaption(HDC hdc, const RECT& rc, const wchar_t* text, COLORREF accent, HFONT font) {
    if (rc.bottom <= rc.top || rc.right <= rc.left) return;
    HBRUSH bg = CreateSolidBrush(g_theme.panel);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    RECT bar = {rc.left, rc.top, rc.left + 4, rc.bottom - 1};
    HBRUSH accentBrush = CreateSolidBrush(accent);
    FillRect(hdc, &bar, accentBrush);
    DeleteObject(accentBrush);
    HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
    HGDIOBJ oldPen = SelectObject(hdc, borderPen);
    MoveToEx(hdc, rc.left, rc.bottom - 1, nullptr);
    LineTo(hdc, rc.right, rc.bottom - 1);
    SelectObject(hdc, oldPen);
    DeleteObject(borderPen);
    RECT textRc = rc;
    textRc.left += 12;
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, g_theme.text);
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, font));
    DrawTextW(hdc, text, -1, &textRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(hdc, oldFont);
}

static const char* LanguageCode(UiLanguage lang) {
    switch (lang) {
        case UiLanguage::Japanese: return "ja";
        case UiLanguage::Spanish:  return "es";
        default:                   return "en";
    }
}

static UiLanguage ParseLanguage(const std::string& code) {
    if (code == "ja") return UiLanguage::Japanese;
    if (code == "es") return UiLanguage::Spanish;
    if (code == "en") return UiLanguage::English;
    return DetectDefaultLanguage();
}

static const char* ThemeCode(UiTheme theme) {
    switch (theme) {
        case UiTheme::Dark:         return "dark";
        case UiTheme::Light:        return "light";
        case UiTheme::Pastel:       return "pastel";
        case UiTheme::HighContrast: return "high_contrast";
        case UiTheme::Midnight:     return "midnight";
        case UiTheme::Ember:        return "ember";
        case UiTheme::Berry:        return "berry";
        case UiTheme::Crimson:      return "crimson";
        case UiTheme::Forest:       return "forest";
        case UiTheme::Mist:         return "mist";
        case UiTheme::PastelBlue: return "pastel_blue";
        case UiTheme::PastelOrange: return "pastel_orange";
        case UiTheme::PastelPink: return "pastel_pink";
        case UiTheme::PastelRed: return "pastel_red";
        case UiTheme::PastelGreen: return "pastel_green";
        case UiTheme::PastelPurple: return "pastel_purple";
        case UiTheme::PastelYellow: return "pastel_yellow";
        case UiTheme::DeepPurple: return "deep_purple";
        case UiTheme::DeepYellow: return "deep_yellow";
        default:                    return "system";
    }
}
static UiTheme ParseTheme(const std::string& code) {
    if (code == "dark") return UiTheme::Dark;
    if (code == "light") return UiTheme::Light;
    if (code == "pastel") return UiTheme::Pastel;
    if (code == "high_contrast") return UiTheme::HighContrast;
    if (code == "midnight") return UiTheme::Midnight;
    if (code == "ember") return UiTheme::Ember;
    if (code == "berry") return UiTheme::Berry;
    if (code == "crimson") return UiTheme::Crimson;
    if (code == "forest") return UiTheme::Forest;
    if (code == "mist") return UiTheme::Mist;
    if (code == "pastel_blue") return UiTheme::PastelBlue;
    if (code == "pastel_orange") return UiTheme::PastelOrange;
    if (code == "pastel_pink") return UiTheme::PastelPink;
    if (code == "pastel_red") return UiTheme::PastelRed;
    if (code == "pastel_green") return UiTheme::PastelGreen;
    if (code == "pastel_purple") return UiTheme::PastelPurple;
    if (code == "pastel_yellow") return UiTheme::PastelYellow;
    if (code == "deep_purple") return UiTheme::DeepPurple;
    if (code == "deep_yellow") return UiTheme::DeepYellow;
    return UiTheme::System;
}

// Compact dashboard header / splitter layout constants
constexpr int kHeaderKpiTop        = 6;
constexpr int kHeaderKpiRow1Bottom = 26;
constexpr int kHeaderKpiRow2Top    = 30;
constexpr int kHeaderKpiBottom     = 50;
constexpr int kToolbarTop          = 56;
constexpr int kToolbarH            = 24;
constexpr int kListTop             = 98;
constexpr int kSplitterH           = 6;
constexpr int kRowSplitterH        = 6;
constexpr int kMinRowListH         = 64;
constexpr int kListCaptionH        = 20;
constexpr int kTabH                = 26;
constexpr int kStatusBarH          = 26;
constexpr int kMinListH            = 60;

// Font height (px) of the process list and the task queue list.
constexpr int kDefaultListFontPx = 13;
constexpr int kMinListFontPx = 9;
constexpr int kMaxListFontPx = 24;
constexpr int kListFontSizes[] = {10, 11, 12, 13, 14, 16, 18, 20, 22, 24};
constexpr int kListFontSizeCount = static_cast<int>(sizeof(kListFontSizes) / sizeof(kListFontSizes[0]));

static int ClampListFontPx(int px) {
    return (std::max)(kMinListFontPx, (std::min)(kMaxListFontPx, px));
}

// Index of the largest preset that does not exceed px (or the first one).
static int ListFontSizeIndex(int px) {
    int idx = 0;
    for (int i = 0; i < kListFontSizeCount; ++i) {
        if (kListFontSizes[i] <= px) idx = i;
    }
    return idx;
}
constexpr int kMinTerminalH        = 200;
constexpr int kNumKpiCards         = 14;
constexpr int kUsagePanelH         = 110;
constexpr int kMinStreamPaneH      = 160;
constexpr int64_t kLongStepMs      = 30000;

enum ControlId {
    IDC_BTN_START = 1005,
    IDC_BTN_KILL_SEL = 1006,
    IDC_BTN_KILL_ALL = 1007,
    IDC_BTN_CLEAR = 1008,
    IDC_LIST_PROCS = 1009,
    IDC_BTN_GRID_VIEW = 1015,
    IDC_TAB_TERMINALS = 1016,
    IDC_BTN_HIDE_TRAY = 1017,
    IDC_BTN_HIDE_ENDED = 1019,
    IDC_BTN_TOGGLE_LIST = 1020,
    IDC_BTN_TOGGLE_TERM = 1021,
    IDC_BTN_SETTINGS = 1023,
    IDC_CLOCK = 1024,
    IDC_BTN_VIEW_TASKS = 1025,
    IDC_LIST_TASKS = 1026,
    IDC_BTN_USAGE = 1027,
    IDC_BTN_STREAM_PANE = 1028,
    IDC_BTN_REFRESH = 1029,
    IDC_BT_DEBUG = 1030,  // "BT" toolbar button / Ctrl+Shift+B: behavior tree view
    IDC_BTN_HINT = 1040,
    IDC_BTN_SUPERVISOR = 1041,
    IDC_HINT_COPY = 1042,
    IDC_HINT_LAUNCH = 1043,
    IDC_HINT_CLOSE = 1044,
    IDC_SET_LANG_JA = 6001,
    IDC_SET_LANG_EN = 6002,
    IDC_SET_LANG_ES = 6003,
    IDC_SET_THEME_SYSTEM = 6010,
    IDC_SET_THEME_DARK = 6011,
    IDC_SET_THEME_LIGHT = 6012,
    IDC_SET_THEME_HC = 6013,
    IDC_SET_THEME_PASTEL = 6014,
    IDC_SET_THEME_MIDNIGHT = 6015,
    IDC_SET_THEME_EMBER = 6016,
    IDC_SET_THEME_BERRY = 6017,
    IDC_SET_THEME_CRIMSON = 6018,
    IDC_SET_THEME_FOREST = 6019,
    IDC_SET_THEME_MIST = 6020,
    IDC_SET_THEME_PASTEL_BLUE = 6021,
    IDC_SET_THEME_PASTEL_ORANGE = 6022,
    IDC_SET_THEME_PASTEL_PINK = 6023,
    IDC_SET_THEME_PASTEL_RED = 6024,
    IDC_SET_THEME_PASTEL_GREEN = 6025,
    IDC_SET_THEME_DEEP_PURPLE = 6026,
    IDC_SET_THEME_PASTEL_PURPLE = 6027,
    IDC_SET_THEME_DEEP_YELLOW = 6028,
    IDC_SET_THEME_PASTEL_YELLOW = 6029,
    IDC_SET_SHELL_CONFIG = 6030,
    IDC_SET_RESET_IDS = 6031,
    IDC_SET_CLEAR_ENDED = 6032,
    IDC_SET_HIDE_ENDED = 6033,
    IDC_SET_SOUND_ENABLED = 6034,
    IDC_SET_SOUND_PATH = 6035,
    IDC_SET_SOUND_BROWSE = 6036,
    IDC_SET_SOUND_TEST = 6037,
    IDC_SET_KPI_SAT = 6038,
    IDC_SET_USAGE_GRAPH_MODE = 6039,
    IDC_SET_RESTORE_USAGE_GRAPH = 6040,
    IDC_SET_RESOURCE_LOCKS = 6041,
    IDC_SET_ACTIVITY_STYLE = 6042,
    IDC_SET_LLM_MAX_EVENTS = 6043,
    IDC_SET_REFRESH_INTERVAL = 6044,
    IDC_SET_LIST_BG_RUNNING = 6045,   // 6045..6049: running, idle, completed, timeout, failed
    IDC_SET_LIST_BG_IDLE = 6046,
    IDC_SET_LIST_BG_COMPLETED = 6047,
    IDC_SET_LIST_BG_TIMEOUT = 6048,
    IDC_SET_LIST_BG_FAILED = 6049,
    IDC_SET_LIST_BG_RESET = 6050,
    IDC_SET_END_BLINK_ENABLED = 6051,
    IDC_SET_END_BLINK_SECONDS = 6052,
    IDC_SET_TAB = 6053,
    IDC_SET_PROC_FONT = 6054,
    IDC_SET_TASK_FONT = 6055,
    IDC_SET_SHOW_SUPERVISOR_HINTS = 6056,
    IDC_SET_HINT_AUTO_DISMISS_SEC = 6057,
    IDC_SET_SUPERVISOR_COMMAND = 6058,
    IDC_SET_WEBUI_ENABLED = 6059,
    IDC_SET_WEBUI_SCOPE = 6060,
    IDC_SET_WEBUI_ALLOWED_HOSTS = 6061,
    IDM_TRAY_RESTORE = 4001,
    IDM_TRAY_GRID_VIEW = 4002,
    IDM_TRAY_HIDE = 4003,
    IDM_TRAY_EXIT = 4004,
    IDM_TRAY_USAGE = 4005,
    IDM_TRAY_MEMORY = 4006
};

enum class ShellType {
    Cmd = 0,
    PowerShell = 1,
    Bash = 2
};

struct CreateTermViewParams {
    std::wstring resolvedCmd;
    std::wstring cwd;
    std::function<void(const char*, size_t)> onOutput;
    TerminalView* outView = nullptr;
    std::wstring outError;
};

struct TermSlotBanner {
    RECT rcBanner;
    RECT rcBorder;
    int procId;
};

enum class ListDisplayMode {
    TwoRows = 0,     // Default: top is Process List, bottom is Task Queue List
    ProcessOnly = 1, // Only Process List
    TaskOnly = 2     // Only Task Queue List
};

// Sort state of one list column. Clicking a header walks the cycle
// ascending -> descending -> unsorted -> ascending.
enum class SortDirection {
    None = 0,
    Ascending = 1,
    Descending = 2
};

// The next state of the cycle when the same header is clicked again.
static SortDirection NextSortDirection(SortDirection dir) {
    switch (dir) {
        case SortDirection::Ascending: return SortDirection::Descending;
        case SortDirection::Descending: return SortDirection::None;
        default: return SortDirection::Ascending;
    }
}

struct LlmStreamDlgState;

struct DashboardState {
    ProcessManager* pm = nullptr;
    IpcServer* ipc = nullptr;
    TaskQueueManager* tqm = nullptr;
    DWORD guiThreadId = 0;

    HWND hwndMain = nullptr;
    HWND hwndBtnStart = nullptr;
    HWND hwndBtnSettings = nullptr;
    HWND hwndBtnKillSel = nullptr;
    HWND hwndBtnKillAll = nullptr;
    HWND hwndBtnClear = nullptr;
    HWND hwndBtnGridView = nullptr;
    HWND hwndBtnHideTray = nullptr;
    HWND hwndBtnToggleList = nullptr;
    HWND hwndBtnToggleTerm = nullptr;
    HWND hwndBtnUsage = nullptr;
    HWND hwndBtnHint = nullptr;
    HWND hwndBtnSupervisor = nullptr;
    HWND hwndHintWnd = nullptr;
    bool showSupervisorHints = true;
    int hintAutoDismissSec = 6;
    std::string supervisorCommand = "claude";
    std::string lastHintSignature;
    std::wstring currentHintCategory;
    std::wstring currentHintSummary;
    std::wstring currentHintPrompt;
    int hintDismissCountdown = 0;
    HWND hwndUsagePanel = nullptr;
    bool showUsagePanel = true;
    int usagePanelHeight = 110;
    int usageRangeIndex = 0; // 0: 24h (default), 1: 7d, 2: 30d, 3: 12m
    int usageGraphMode = 0; // 0: token kinds (default), 1: by provider/model
    bool restoreUsageGraph = false;
    int64_t llmMaxEvents = kDefaultLlmMaxEvents;  // events kept per stream tracker; -1 = unlimited
    bool resourceLocks = true; // task queue resource locks (doc/resource-locks.md)
    int64_t memoryMaxBytes = 0; // memory/ capacity limit in bytes; 0 = unlimited
    uint64_t lastMemoryCheckTickMs = 0;
    bool memoryNearLimit = false;
    bool memoryTrayWarned = false;
    std::wstring memoryWarningText;
    int refreshIntervalMs = kDefaultRefreshIntervalMs; // dashboard + terminal grid refresh period (ms)
    int runningTotalCount = 0;  // processes in Running state + running queue jobs, from the last refresh
    int appliedRefreshMs = 0;   // period currently set on TIMER_REFRESH_ID
    RECT rcUsagePanel = {};
    RECT rcUsageSplitter = {};
    bool usageSplitterDragging = false;
    HWND hwndList = nullptr;
    HWND hwndTaskList = nullptr;
    HWND hwndBtnViewTasks = nullptr;
    ListDisplayMode listDisplayMode = ListDisplayMode::TwoRows;
    float procRowRatio = 0.5f;
    RECT rcRowsSplitter = {};
    RECT rcProcCaption = {};
    RECT rcTaskCaption = {};
    bool rowsSplitterDragging = false;
    int lastTotalListHeight = 0;
    std::vector<std::string> taskJobIds;
    std::vector<std::string> taskJobStates;
    std::vector<std::wstring> taskOriginTips;  // per task row: full origin details for the hover tip
    std::vector<int> taskJobProcIds;     // latest attempt's process per task row (0 = none)
    int lastFollowedProcId = -1;         // process the monitor pane last auto-followed
    // What the lists and KPI cards showed last time, so an idle refresh tick can skip redrawing them.
    std::vector<std::vector<std::wstring>> procListCells;  // per row: lParam id, then 14 column texts
    std::vector<COLORREF> procListRowBg;
    std::string lastTaskListJson;
    int lastTaskListLang = -1;
    std::vector<std::wstring> lastTaskRows;
    std::wstring lastKpiSignature;
    std::wstring taskFailureSummary;  // "failed: exit_nonzero ×2" shown in the task list caption
    int lastUsagePanelProcId = -2;
    // JSON stream bottom pane below the process / task lists
    HWND hwndBtnStreamPane = nullptr;
    HWND hwndBtnBtView = nullptr;
    HWND hwndBtnRefresh = nullptr;
    HWND hwndStreamPane = nullptr;
    LlmStreamDlgState* streamPane = nullptr; // owned by hwndStreamPane
    bool showStreamPane = true;
    int streamPaneHeight = 240;
    RECT rcStreamSplitter = {};
    bool streamSplitterDragging = false;
    int streamPaneBottom = 0;
    HWND hwndTab = nullptr;
    HWND hwndClock = nullptr;
    HWND hwndKpiTip = nullptr;
    HWND hwndToolbarTip = nullptr;
    RECT kpiCardRects[kNumKpiCards] = {};
    std::wstring kpiTipText[kNumKpiCards];
    int activeRunningCount = 0;

    bool hideProcessList = false;
    bool hideTerminalGrid = true;

    ShellType defaultShell = ShellType::PowerShell;
    std::string cmdCommand = "cmd.exe";
    std::string powershellCommand = "powershell.exe";
    std::string bashCommand = "bash.exe";

    bool hideEndedProcesses = false;
    int sortColumn = 0;        // Default: Column 0 (ID)
    SortDirection sortDirection = SortDirection::Descending; // Default: Descending (newest processes at the top)

    int taskSortColumn = -1;   // -1: the task list keeps its natural order
    SortDirection taskSortDirection = SortDirection::None;

    bool completionSoundEnabled = true;
    std::string completionSoundPath;
    std::unordered_map<int, ProcState> lastProcStates;

    bool endBlinkEnabled = true;                    // blink list rows when a process/task ends
    int endBlinkSeconds = kDefaultEndBlinkSec;
    std::unordered_map<int, ULONGLONG> procEndBlinkUntil;        // process id -> GetTickCount64 end time
    std::unordered_map<std::string, ULONGLONG> taskEndBlinkUntil; // task job id -> GetTickCount64 end time
    std::unordered_set<std::string> runningTaskJobs;             // jobs that were running at the last refresh

    std::unique_ptr<TerminalGridView> gridView;

    HFONT hFontUi = nullptr;
    HFONT hFontUiBold = nullptr;
    HFONT hFontKpiVal = nullptr;
    int processListFontPx = kDefaultListFontPx;
    int taskListFontPx = kDefaultListFontPx;
    HFONT hFontProcList = nullptr;
    HFONT hFontTaskList = nullptr;

    int selectedProcId = -1;
    std::string selectedJobKey;
    bool allowExit = false;
    NOTIFYICONDATAW nid = {};
    bool trayAdded = false;

    std::string dashboardStartTimeStr;
    uint64_t dashboardStartTickMs = 0;
    int termProgressState = 0;
    int termProgressValue = 0;

    std::vector<ProcessSnapshot> allSnapshots;
    std::vector<ProcessSnapshot> cachedSnapshots;
    RECT rcKpiArea = {};
    RECT rcTerminalArea = {10, 300, 1380, 810};
    RECT rcStatusBar = {0, 814, 1400, 840};
    RECT rcSplitter = {};
    int processListHeight = 260;
    int processListTop = kListTop;
    bool splitterDragging = false;
    std::vector<TermSlotBanner> termBanners;
};

static DashboardState* GetState(HWND hwnd) {
    return reinterpret_cast<DashboardState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

// ---------------------------------------------------------------------------
// IPC client helpers for GUI dashboard (Card 060-2)
// Replaces direct state reads (st->pm, st->tqm, memory, config) with Named Pipe IPC calls
// ---------------------------------------------------------------------------

static bool GuiIpcCall(DashboardState* st, const nlohmann::json& req, nlohmann::json& outResp, std::string* outError = nullptr) {
    std::string resp;
    std::string err;
    bool ok = false;
    if (SendIpcRequest(req.dump(), resp, err, false)) {
        ok = true;
    } else if (st && st->ipc) {
        resp = st->ipc->HandleRequestJson(req.dump());
        ok = !resp.empty();
    }
    if (!ok) {
        if (outError) *outError = err.empty() ? "IPC request failed" : err;
        return false;
    }
    outResp = nlohmann::json::parse(resp, nullptr, false);
    if (outResp.is_discarded()) {
        if (outError) *outError = "Invalid JSON response: " + resp;
        return false;
    }
    return true;
}

static ProcessSnapshot ProcessSnapshotFromJson(const nlohmann::json& j) {
    ProcessSnapshot s;
    if (!j.is_object()) return s;
    s.id = j.value("id", 0);
    s.name = j.value("name", "");
    s.commandLine = j.value("command", "");
    s.workingDir = j.value("cwd", "");
    s.mode = StringToExecMode(j.value("mode", "pipe"));
    s.state = StringToProcState(j.value("state", "completed"));
    s.rootPid = static_cast<DWORD>(j.value("pid", 0));
    s.exitCode = (s.state == ProcState::Running) ? STILL_ACTIVE : static_cast<DWORD>(j.value("exit_code", 0));
    s.startTimeStr = j.value("start_time", "");
    s.endTimeStr = j.value("end_time", "");
    s.estimatedEndTimeStr = j.value("estimated_end_time", "");
    s.elapsedMs = j.value("elapsed_ms", 0ULL);
    s.cpuTimeMs = j.value("cpu_time_ms", 0ULL);
    s.idleMs = j.value("idle_ms", 0ULL);
    s.timeoutSec = j.value("timeout_sec", 0);
    s.idleTimeoutSec = j.value("idle_timeout_sec", 0);
    s.treeProcessCount = j.value("tree_procs", 0U);
    s.treeMemoryBytes = j.value("memory_bytes", 0ULL);
    s.totalBytesRead = j.value("total_bytes_read", 0ULL);
    s.logFilePath = j.value("log_file", "");
    s.doneFilePath = j.value("done_file", "");
    s.lastLinePreview = j.value("last_line", "");
    s.jobKey = j.value("job_key", "");
    s.jobId = j.value("job_id", 0);
    s.attemptN = j.value("attempt", 0);
    s.agentType = j.value("agent_type", "");
    s.agentStatus = j.value("agent_status", "");
    s.currentTool = j.value("current_tool", "");
    s.currentToolAction = j.value("current_tool_action", "");
    s.lastThought = j.value("last_thought", "");
    s.lastMessage = j.value("last_message", "");
    s.stepCount = j.value("step_count", 0);
    s.streamEventCount = j.value("stream_event_count", 0);
    s.streamFilePath = j.value("stream_file", "");
    s.supervisorNotes = j.value("supervisor_notes", "");
    s.supervisorGoal = j.value("supervisor_goal", "");
    return s;
}

static std::vector<ProcessSnapshot> FetchProcessSnapshotsIpc(DashboardState* st) {
    nlohmann::json req = {{"action", "list"}};
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp) && resp.value("ok", false) &&
        resp.contains("processes") && resp["processes"].is_array()) {
        std::vector<ProcessSnapshot> list;
        list.reserve(resp["processes"].size());
        for (const auto& item : resp["processes"]) {
            list.push_back(ProcessSnapshotFromJson(item));
        }
        return list;
    }
    if (st && st->pm) {
        return st->pm->GetAllSnapshots();
    }
    return {};
}

static bool FetchProcessSnapshotIpc(DashboardState* st, int id, ProcessSnapshot& outSnap) {
    if (id <= 0) return false;
    nlohmann::json req = {{"action", "list"}, {"id", id}};
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp) && resp.value("ok", false) &&
        resp.contains("processes") && resp["processes"].is_array() &&
        !resp["processes"].empty()) {
        outSnap = ProcessSnapshotFromJson(resp["processes"][0]);
        return true;
    }
    if (st && st->pm) {
        return st->pm->GetSnapshot(id, outSnap);
    }
    return false;
}

static bool KillProcessIpc(DashboardState* st, int procId, std::string& outErr) {
    nlohmann::json req = {{"action", "kill"}, {"id", procId}};
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp, &outErr) && resp.value("ok", false)) {
        return true;
    }
    if (st && st->pm) {
        return st->pm->KillProcess(procId, outErr);
    }
    return false;
}

static int KillAllRunningIpc(DashboardState* st) {
    nlohmann::json req = {{"action", "kill_all"}};
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp) && resp.value("ok", false)) {
        return resp.value("killed_count", 0);
    }
    if (st && st->pm) {
        return st->pm->KillAllRunning();
    }
    return 0;
}

static int ClearFinishedIpc(DashboardState* st) {
    nlohmann::json req = {{"action", "clear"}};
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp) && resp.value("ok", false)) {
        return resp.value("cleared_count", 0);
    }
    if (st && st->pm) {
        return st->pm->ClearFinished();
    }
    return 0;
}

static bool RemoveProcessIpc(DashboardState* st, int procId) {
    nlohmann::json req = {{"action", "remove"}, {"id", procId}};
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp) && resp.value("ok", false)) {
        return true;
    }
    if (st && st->pm) {
        return st->pm->RemoveProcess(procId);
    }
    return false;
}

static int ResetProcessIdsIpc(DashboardState* st, std::string& outErr) {
    nlohmann::json req = {{"action", "reset_ids"}};
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp, &outErr) && resp.value("ok", false)) {
        return resp.value("count", 0);
    }
    if (st && st->pm) {
        return st->pm->ResetProcessIds(outErr);
    }
    return -1;
}

static bool GetProcessOutputIpc(DashboardState* st, int procId, int tail, std::string& outOutput) {
    nlohmann::json req = {{"action", "logs"}, {"id", procId}, {"tail", tail}};
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp) && resp.value("ok", false)) {
        outOutput = resp.value("output", "");
        return true;
    }
    if (st && st->pm) {
        return st->pm->GetOutput(procId, true, tail, outOutput);
    }
    return false;
}

static bool GetAgentStateIpc(DashboardState* st, int procId, LlmAgentState& outState) {
    nlohmann::json req = {{"action", "get_agent_state"}, {"id", procId}};
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp) && resp.value("ok", false)) {
        outState.agentType = resp.value("agent_type", "");
        outState.status = resp.value("status", "");
        outState.currentTool = resp.value("current_tool", "");
        outState.currentToolAction = resp.value("current_tool_action", "");
        outState.lastThought = resp.value("last_thought", "");
        outState.lastMessage = resp.value("last_message", "");
        outState.stepCount = resp.value("step_count", 0);
        outState.eventCount = resp.value("event_count", 0);
        outState.totalTokens = resp.value("total_tokens", 0ULL);
        outState.costUsd = resp.value("cost_usd", 0.0);
        outState.streamFilePath = resp.value("stream_file", "");
        outState.supervisorNotes = resp.value("supervisor_notes", "");
        outState.supervisorGoal = resp.value("supervisor_goal", "");
        outState.updatedAt = resp.value("updated_at", "");
        return true;
    }
    if (st && st->pm) {
        std::vector<LlmStreamEvent> dummy;
        return st->pm->GetAgentStream(procId, 1, "", 0, dummy, outState);
    }
    return false;
}

static TaskQueueCounts FetchTaskQueueCountsIpc(DashboardState* st) {
    nlohmann::json req = {{"action", "taskqueue_counts"}};
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp) && resp.value("ok", false)) {
        TaskQueueCounts c;
        if (resp.contains("counts") && resp["counts"].is_object()) {
            const auto& cnt = resp["counts"];
            c.running = cnt.value("running", 0);
            c.waiting = cnt.value("waiting", 0);
            c.pending = cnt.value("pending", 0);
            c.blocked = cnt.value("blocked", 0);
            c.succeeded = cnt.value("succeeded", 0);
            c.failed = cnt.value("failed", 0);
            c.cancelled = cnt.value("cancelled", 0);
            c.total = cnt.value("total", 0);
        } else {
            c.running = resp.value("running", 0);
            c.waiting = resp.value("waiting", 0);
            c.pending = resp.value("pending", 0);
            c.blocked = resp.value("blocked", 0);
            c.succeeded = resp.value("succeeded", 0);
            c.failed = resp.value("failed", 0);
            c.cancelled = resp.value("cancelled", 0);
            c.total = resp.value("total", 0);
        }
        return c;
    }
    if (st && st->tqm) {
        return st->tqm->GetJobCounts();
    }
    return TaskQueueCounts{};
}

static std::string TaskQueueListIpc(DashboardState* st, const std::string& taskqueue, const std::string& state, int64_t sinceSec, std::string& outErr, const std::string& origin = "", const std::string& bt = "") {
    nlohmann::json req = {
        {"action", "taskqueue_list"},
        {"taskqueue", taskqueue},
        {"state", state}
    };
    if (sinceSec > 0) req["since_sec"] = sinceSec;
    if (!origin.empty()) req["origin"] = origin;
    if (!bt.empty()) req["bt"] = bt;
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp, &outErr)) {
        return resp.dump();
    }
    if (st && st->tqm) {
        return st->tqm->TaskQueueList(taskqueue, state, sinceSec, outErr, origin, bt);
    }
    return "";
}

static std::string TaskQueueGetIpc(DashboardState* st, const std::string& id, bool includeInstructions, int rev, std::string& outErr) {
    nlohmann::json req = {
        {"action", "taskqueue_get"},
        {"id", id},
        {"include_instructions", includeInstructions}
    };
    if (rev > 0) req["rev"] = rev;
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp, &outErr)) {
        return resp.dump();
    }
    if (st && st->tqm) {
        return st->tqm->TaskQueueGet(id, includeInstructions, rev, outErr);
    }
    return "";
}

static std::string TaskQueueCancelIpc(DashboardState* st, const std::string& id, std::string& outErr) {
    nlohmann::json req = {{"action", "taskqueue_cancel"}, {"id", id}};
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp, &outErr)) {
        return resp.dump();
    }
    if (st && st->tqm) {
        return st->tqm->TaskQueueCancel(id, outErr);
    }
    return "";
}

static std::string TaskQueueRetryIpc(DashboardState* st, const std::string& id, bool resetAttempts, bool refreshInstr, std::string& outErr) {
    nlohmann::json req = {
        {"action", "taskqueue_retry"},
        {"id", id},
        {"reset_attempts", resetAttempts},
        {"refresh_instructions", refreshInstr}
    };
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp, &outErr)) {
        return resp.dump();
    }
    if (st && st->tqm) {
        return st->tqm->TaskQueueRetry(id, resetAttempts, refreshInstr, TaskQueueCaller::External, outErr);
    }
    return "";
}

static std::string TaskQueuePauseIpc(DashboardState* st, const std::string& queue, bool pause, std::string& outErr) {
    nlohmann::json req = {
        {"action", pause ? "taskqueue_pause" : "taskqueue_resume"},
        {"taskqueue", queue}
    };
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp, &outErr)) {
        return resp.dump();
    }
    if (st && st->tqm) {
        return st->tqm->TaskQueuePause(queue, pause, outErr);
    }
    return "";
}

static std::string TaskQueuePurgeIpc(DashboardState* st, const std::string& queue, const std::string& statesJson, std::string& outErr) {
    nlohmann::json req = {
        {"action", "taskqueue_purge"},
        {"taskqueue", queue}
    };
    auto parsedStates = nlohmann::json::parse(statesJson, nullptr, false);
    if (!parsedStates.is_discarded()) {
        req["states"] = parsedStates;
    }
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp, &outErr)) {
        return resp.dump();
    }
    if (st && st->tqm) {
        return st->tqm->TaskQueuePurge(queue, statesJson, outErr);
    }
    return "";
}

static void SetResourceLocksEnabledIpc(DashboardState* st, bool enabled) {
    nlohmann::json req = {{"action", "taskqueue_resource_locks"}, {"enabled", enabled}};
    nlohmann::json resp;
    GuiIpcCall(st, req, resp);
    if (st && st->tqm) {
        st->tqm->SetResourceLocksEnabled(enabled);
    }
}

static pc::memory::StorageStats GetMemoryStorageStatsIpc(DashboardState* st) {
    nlohmann::json req = {{"action", "memory_storage_stats"}};
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp) && resp.value("ok", false)) {
        pc::memory::StorageStats s;
        s.totalBytes = resp.value("total_bytes", 0ULL);
        s.limitBytes = resp.value("limit_bytes", 0ULL);
        s.l1Bytes = resp.value("l1_bytes", 0ULL);
        s.l2Bytes = resp.value("l2_bytes", 0ULL);
        s.l3Bytes = resp.value("l3_bytes", 0ULL);
        s.indexBytes = resp.value("index_bytes", 0ULL);
        s.otherBytes = resp.value("other_bytes", 0ULL);
        s.nearLimit = resp.value("near_limit", false);
        s.exceeded = resp.value("exceeded", false);
        s.warning = resp.value("warning", "");
        return s;
    }
    return pc::memory::GetMemoryStorageStats();
}

static std::string MemorySearchIpc(DashboardState* st, const memory::Query& q) {
    nlohmann::json req = {
        {"action", "memory_search"},
        {"text", q.text},
        {"job_name", q.jobName},
        {"taskqueue", q.taskqueue},
        {"limit", q.limit}
    };
    if (!q.kinds.empty()) {
        req["kinds"] = q.kinds;
    }
    if (!q.tags.empty()) {
        req["tags"] = q.tags;
    }
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp)) {
        return resp.dump();
    }
    return memory::SearchJson(q);
}

static std::string MemoryGetIpc(DashboardState* st, const std::string& id, const std::string& kind) {
    nlohmann::json req = {{"action", "memory_get"}, {"id", id}, {"kind", kind}};
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp)) {
        return resp.dump();
    }
    return memory::GetJson(id, kind);
}

static std::string MemoryStatsIpc(DashboardState* st, const memory::Query& q) {
    nlohmann::json req = {
        {"action", "memory_stats"},
        {"job_name", q.jobName},
        {"taskqueue", q.taskqueue}
    };
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp)) {
        return resp.dump();
    }
    return memory::StatsJson(q);
}

static std::string MemoryInjectCompareIpc(DashboardState* st, const std::string& job, const std::string& tq, const std::string& since) {
    nlohmann::json req = {
        {"action", "memory_compare"},
        {"job", job},
        {"taskqueue", tq},
        {"since", since}
    };
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp)) {
        return resp.dump();
    }
    return MemoryInjectCompareJson(job, tq, since);
}

static std::wstring StateLabelWide(ProcState state, bool idleWarning) {
    switch (state) {
        case ProcState::Running:
            return idleWarning ? Tr(L"アイドル / 待機", L"IDLE / WAITING", L"INACTIVO / ESPERANDO")
                               : Tr(L"稼働中", L"RUNNING", L"EN EJECUCIÓN");
        case ProcState::Completed:    return Tr(L"完了 (OK)", L"COMPLETED (OK)", L"COMPLETADO (OK)");
        case ProcState::Failed:       return Tr(L"失敗", L"FAILED", L"FALLIDO");
        case ProcState::TimedOut:     return Tr(L"タイムアウト", L"TIMEOUT", L"TIEMPO AGOTADO");
        case ProcState::IdleTimedOut: return Tr(L"ハングタイムアウト", L"HANG TIMEOUT", L"TIEMPO DE INACTIVIDAD");
        case ProcState::Killed:       return Tr(L"強制終了", L"KILLED", L"TERMINADO");
    }
    return Tr(L"不明", L"UNKNOWN", L"DESCONOCIDO");
}

static std::wstring FormatRemainingTime(int64_t remainingSec) {
    if (remainingSec <= 0) {
        return Tr(L"タイムアウト直前", L"expiring now", L"a punto de expirar");
    }
    int64_t hrs = remainingSec / 3600;
    int64_t mins = (remainingSec % 3600) / 60;
    int64_t secs = remainingSec % 60;
    wchar_t buf[128] = {};
    if (hrs > 0) {
        swprintf_s(buf, Tr(L"残り %lld時間%lld分", L"%lldh %lldm left", L"Quedan %lldh %lldm"), hrs, mins);
    } else if (mins > 0) {
        swprintf_s(buf, Tr(L"残り %lld分%lld秒", L"%lldm %llds left", L"Quedan %lldm %llds"), mins, secs);
    } else {
        swprintf_s(buf, Tr(L"残り %lld秒", L"%llds left", L"Quedan %llds"), secs);
    }
    return buf;
}

static std::wstring FormatRemainingTimeShort(int64_t remainingSec) {
    if (remainingSec <= 0) {
        return L"0s";
    }
    int64_t hrs = remainingSec / 3600;
    int64_t mins = (remainingSec % 3600) / 60;
    int64_t secs = remainingSec % 60;
    wchar_t buf[64] = {};
    if (hrs > 0) {
        swprintf_s(buf, L"~%lldh%lldm", hrs, mins);
    } else if (mins > 0) {
        swprintf_s(buf, L"~%lldm", mins);
    } else {
        swprintf_s(buf, L"~%llds", secs);
    }
    return buf;
}

static COLORREF GetStateBadgeColor(const ProcessSnapshot& s) {
    if (g_uiTheme == UiTheme::HighContrast) {
        switch (s.state) {
            case ProcState::Running:
                return (s.idleMs >= 10000) ? RGB(255, 255, 0) : RGB(0, 255, 0);
            case ProcState::Completed:
                return RGB(0, 200, 255);
            case ProcState::Failed:
            case ProcState::TimedOut:
            case ProcState::IdleTimedOut:
            case ProcState::Killed:
                return RGB(255, 0, 0);
        }
        return RGB(255, 255, 255);
    }
    if (IsPastelTheme(g_uiTheme)) {
        switch (s.state) {
            case ProcState::Running:
                return (s.idleMs >= 10000) ? RGB(190, 150, 100) : RGB(110, 160, 130);
            case ProcState::Completed:
                return RGB(110, 140, 190);
            case ProcState::Failed:
            case ProcState::TimedOut:
            case ProcState::IdleTimedOut:
            case ProcState::Killed:
                return RGB(190, 110, 120);
        }
        return RGB(140, 130, 150);
    }
    if (IsDarkThemeActive()) {
        switch (s.state) {
            case ProcState::Running:
                return (s.idleMs >= 10000) ? RGB(118, 98, 64) : RGB(68, 100, 82);
            case ProcState::Completed:
                return RGB(68, 96, 124);
            case ProcState::Failed:
            case ProcState::TimedOut:
            case ProcState::IdleTimedOut:
            case ProcState::Killed:
                return RGB(122, 74, 74);
        }
        return RGB(98, 98, 104);
    }
    switch (s.state) {
        case ProcState::Running:
            return (s.idleMs >= 10000) ? RGB(217, 119, 6) : RGB(22, 163, 74);
        case ProcState::Completed:
            return RGB(37, 99, 235);
        case ProcState::Failed:
        case ProcState::TimedOut:
        case ProcState::IdleTimedOut:
        case ProcState::Killed:
            return RGB(220, 38, 38);
    }
    return RGB(100, 116, 139);
}

// User-chosen text background colors for the process and task lists, keyed by state.
// Kept separately for dark and light themes (index 0 = dark, 1 = light). An unset key keeps the
// built-in theme color. Stored in the config as "row_bg_<dark|light>_<key>": "#RRGGBB".
enum class ListRowKey : int { Running = 0, Idle, Completed, Timeout, Failed, Count };
static const int kListRowKeyCount = static_cast<int>(ListRowKey::Count);
static const int kListRowModeCount = 2;
static const char* const kListRowKeyNames[kListRowKeyCount] = {
    "running", "idle", "completed", "timeout", "failed"
};
static COLORREF g_listRowBg[kListRowModeCount][kListRowKeyCount] = {};
static bool g_listRowBgSet[kListRowModeCount][kListRowKeyCount] = {};

static int ListRowMode() { return g_darkThemeActive ? 0 : 1; }

static std::string ListRowConfigKey(int mode, int key) {
    return std::string("row_bg_") + (mode == 0 ? "dark_" : "light_") + kListRowKeyNames[key];
}

// The user's color for this state in the active theme, if one is set.
static bool GetListRowBgOverride(ListRowKey key, COLORREF& out) {
    const int m = ListRowMode();
    const int k = static_cast<int>(key);
    if (!g_listRowBgSet[m][k]) return false;
    out = g_listRowBg[m][k];
    return true;
}

// Black or white, whichever reads better on the given background.
static COLORREF ListRowTextFor(COLORREF bg) {
    int luma = (GetRValue(bg) * 299 + GetGValue(bg) * 587 + GetBValue(bg)) / 1000;
    return luma < 140 ? RGB(255, 255, 255) : RGB(0, 0, 0);
}

static bool ParseHexColor(const std::string& text, COLORREF& out) {
    if (text.size() != 7 || text[0] != '#') return false;
    unsigned int v = 0;
    for (size_t i = 1; i < 7; ++i) {
        char c = text[i];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= static_cast<unsigned int>(c - '0');
        else if (c >= 'a' && c <= 'f') v |= static_cast<unsigned int>(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') v |= static_cast<unsigned int>(c - 'A' + 10);
        else return false;
    }
    out = RGB((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
    return true;
}

static std::string FormatHexColor(COLORREF c) {
    char buf[8] = {};
    snprintf(buf, sizeof(buf), "#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
    return buf;
}

// Returns the user's color for this key in the active theme, or the fallback when none is set.
static COLORREF ListRowBgOrDefault(ListRowKey key, COLORREF fallback) {
    COLORREF c = fallback;
    return GetListRowBgOverride(key, c) ? c : fallback;
}

// Maps a process to its row-color key. Returns false for states with no key.
static bool ProcessRowKey(const ProcessSnapshot& s, ListRowKey& key) {
    switch (s.state) {
        case ProcState::Running:
            key = (s.idleMs >= 10000) ? ListRowKey::Idle : ListRowKey::Running;
            return true;
        case ProcState::Completed:
            key = ListRowKey::Completed;
            return true;
        case ProcState::TimedOut:
        case ProcState::IdleTimedOut:
            key = ListRowKey::Timeout;
            return true;
        case ProcState::Failed:
        case ProcState::Killed:
            key = ListRowKey::Failed;
            return true;
    }
    return false;
}

static bool ProcessRowCustomBg(const ProcessSnapshot& s, COLORREF& out) {
    ListRowKey key;
    return ProcessRowKey(s, key) && GetListRowBgOverride(key, out);
}

static COLORREF GetStateRowBgColor(const ProcessSnapshot& s) {
    COLORREF custom = 0;
    if (ProcessRowCustomBg(s, custom)) return custom;
    switch (s.state) {
        case ProcState::Running:
            return (s.idleMs >= 10000) ? g_theme.rowIdle : g_theme.rowRunning;
        case ProcState::Completed:
            return g_theme.rowCompleted;
        case ProcState::Failed:
        case ProcState::TimedOut:
        case ProcState::IdleTimedOut:
        case ProcState::Killed:
            return g_theme.rowFailed;
    }
    return g_theme.panel;
}

static COLORREF KpiAccent(int index) {
    static const COLORREF darkAccents[7] = {
        RGB(118, 132, 156), RGB(60, 130, 89), RGB(142, 111, 52),
        RGB(55, 105, 151), RGB(144, 64, 64), RGB(101, 78, 134),
        RGB(172, 100, 28)
    };
    static const COLORREF lightAccents[7] = {
        RGB(100, 116, 139), RGB(22, 163, 74), RGB(217, 119, 6),
        RGB(37, 99, 235), RGB(220, 38, 38), RGB(168, 85, 247),
        RGB(234, 88, 12)
    };
    static const COLORREF pastelAccents[7] = {
        RGB(166, 180, 198), RGB(154, 198, 168), RGB(222, 190, 150),
        RGB(156, 186, 220), RGB(222, 160, 168), RGB(190, 166, 210),
        RGB(226, 180, 140)
    };
    static const COLORREF hcAccents[7] = {
        RGB(255, 255, 255), RGB(0, 255, 0), RGB(255, 255, 0),
        RGB(0, 200, 255), RGB(255, 0, 0), RGB(255, 0, 255),
        RGB(255, 128, 0)
    };
    int idx = (index >= 0) ? (index % 7) : ((index % 7 + 7) % 7);
    COLORREF base;
    if (g_uiTheme == UiTheme::HighContrast) base = hcAccents[idx];
    else if (IsPastelTheme(g_uiTheme)) base = pastelAccents[idx];
    else base = IsDarkThemeActive() ? darkAccents[idx] : lightAccents[idx];
    int r = GetRValue(base), g = GetGValue(base), b = GetBValue(base);
    int gray = (r * 299 + g * 587 + b * 114) / 1000;
    r = (int)(gray + (r - gray) * g_kpiSaturation);
    g = (int)(gray + (g - gray) * g_kpiSaturation);
    b = (int)(gray + (b - gray) * g_kpiSaturation);
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    return RGB(r, g, b);
}

static float g_kpiBarPhase = 0.0f;

static void DrawKpiCard(HDC hdc, const RECT& rc, const wchar_t* title, const std::wstring& value,
                        COLORREF accentColor, HFONT hFontUi, HFONT hFontKpiVal,
                        float marqueePhase = -1.0f, const std::wstring& subText = L"") {
    HBRUSH bgBrush = CreateSolidBrush(g_theme.panel);
    HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
    HGDIOBJ oldBrush = SelectObject(hdc, bgBrush);
    HGDIOBJ oldPen = SelectObject(hdc, borderPen);
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 6, 6);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(bgBrush);
    DeleteObject(borderPen);

    RECT rcStripe = {rc.left + 3, rc.top + 1, rc.right - 3, rc.top + 5};
    HBRUSH stripeBrush = CreateSolidBrush(accentColor);
    FillRect(hdc, &rcStripe, stripeBrush);
    DeleteObject(stripeBrush);

    if (marqueePhase >= 0.0f) {
        int stripeLeft = static_cast<int>(rcStripe.left);
        int stripeRight = static_cast<int>(rcStripe.right);
        int stripeW = stripeRight - stripeLeft;
        int bandW = (std::max)(14, stripeW / 3);
        int travel = stripeW + bandW;
        int bandLeft = stripeLeft - bandW + static_cast<int>(marqueePhase * travel);
        RECT rcBand = {(std::max)(stripeLeft, bandLeft), rcStripe.top,
                       (std::min)(stripeRight, bandLeft + bandW), rcStripe.bottom};
        if (rcBand.right > rcBand.left) {
            HBRUSH bandBrush = CreateSolidBrush(BlendColor(accentColor, RGB(255, 255, 255), 0.55f));
            FillRect(hdc, &rcBand, bandBrush);
            DeleteObject(bandBrush);
        }
    }

    SetBkMode(hdc, TRANSPARENT);

    // Measure value text width with hFontKpiVal
    HGDIOBJ oldFont = SelectObject(hdc, hFontKpiVal);
    SIZE szVal = {};
    GetTextExtentPoint32W(hdc, value.c_str(), static_cast<int>(value.size()), &szVal);

    // Measure subText if provided
    SIZE szSub = {};
    bool showSub = false;
    if (!subText.empty()) {
        SelectObject(hdc, hFontUi);
        GetTextExtentPoint32W(hdc, subText.c_str(), static_cast<int>(subText.size()), &szSub);
        int cardW = rc.right - rc.left;
        if (cardW - 16 - szVal.cx - szSub.cx >= 45) {
            showSub = true;
        }
    }

    int padX = 7;
    int rightEdge = rc.right - padX;

    // Draw Value on the right (bold, g_theme.text)
    RECT rcVal = {rightEdge - szVal.cx, rc.top + 2, rightEdge, rc.bottom - 1};
    SetTextColor(hdc, g_theme.text);
    SelectObject(hdc, hFontKpiVal);
    DrawTextW(hdc, value.c_str(), -1, &rcVal, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    int titleRight = rcVal.left - 4;

    // Draw subText to the left of value if space permits
    if (showSub) {
        RECT rcSub = {rcVal.left - szSub.cx - 4, rc.top + 2, rcVal.left - 4, rc.bottom - 1};
        SetTextColor(hdc, g_theme.muted);
        SelectObject(hdc, hFontUi);
        DrawTextW(hdc, subText.c_str(), -1, &rcSub, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        titleRight = rcSub.left - 4;
    }

    // Draw Title on the left (g_theme.muted, hFontUi, single line with ellipsis)
    RECT rcTitle = {rc.left + padX, rc.top + 2, (std::max)(rc.left + padX + 10, static_cast<LONG>(titleRight)), rc.bottom - 1};
    SetTextColor(hdc, g_theme.muted);
    SelectObject(hdc, hFontUi);
    DrawTextW(hdc, title, -1, &rcTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(hdc, oldFont);
}

static void UpdateKpiTooltips(DashboardState* st, uint32_t totalTreeProcs,
                              int activeRunning, int idleWaiting, int completedOk, int failedOrKilled,
                              const TaskQueueCounts& tqCounts,
                              int64_t procMinRemainSec = -1, int64_t taskMinRemainSec = -1) {
    if (!st || !st->hwndKpiTip) return;

    // Row 0: Processes (Cards 0..6)
    st->kpiTipText[0] = Tr(
        L"管理対象のターミナル/プロセスの総数。",
        L"Total number of managed terminals/processes.",
        L"Total de terminales/procesos gestionados.");
    if (totalTreeProcs > 0) {
        st->kpiTipText[0] += std::wstring(L"\n") + Tr(L"全ツリー子プロセス数: ", L"Total tree child processes: ", L"Subprocesos totales: ") + std::to_wstring(totalTreeProcs);
    }

    st->kpiTipText[1] = std::wstring(Tr(
        L"出力またはCPU消費がある実行中プロセス。",
        L"Running processes producing output or consuming CPU.",
        L"Procesos en ejecución con actividad.")) + L" (" + std::to_wstring(activeRunning) + L")";

    st->kpiTipText[2] = std::wstring(Tr(
        L"10秒以上無活動の実行中プロセス (アイドル)。",
        L"Running processes with no output or CPU activity >= 10s (idle).",
        L"Procesos en ejecución sin actividad >= 10 s (inactivos).")) + L" (" + std::to_wstring(idleWaiting) + L")";

    st->kpiTipText[3] = std::wstring(Tr(
        L"正常終了したプロセス (終了コード 0)。",
        L"Processes that exited successfully (exit code 0).",
        L"Procesos finalizados correctamente (código 0).")) + L" (" + std::to_wstring(completedOk) + L")";

    st->kpiTipText[4] = std::wstring(Tr(
        L"失敗・タイムアウト・強制終了したプロセス。",
        L"Processes that failed, timed out, or were killed.",
        L"Procesos fallidos, agotados o terminados.")) + L" (" + std::to_wstring(failedOrKilled) + L")";

    st->kpiTipText[5] = Tr(
        L"全 Job Object の合計メモリ消費量とプロセス数。",
        L"Total memory and process count across all Job Objects.",
        L"Memoria total y recuento de procesos de todos los Job Objects.");

    st->kpiTipText[6] = Tr(
        L"稼働中プロセスの中で最も近いタイムアウトまでの残り時間。",
        L"Shortest remaining time before timeout among running processes.",
        L"Tiempo restante más corto antes del tiempo límite entre procesos activos.");
    if (procMinRemainSec >= 0) {
        st->kpiTipText[6] += std::wstring(L"\n") + Tr(L"残り時間: ", L"Remaining: ", L"Restante: ") + FormatRemainingTime(procMinRemainSec);
    }

    // Row 1: Tasks (Cards 7..13)
    st->kpiTipText[7] = Tr(
        L"タスクキューに登録されている全ジョブの総数。",
        L"Total number of jobs in the task queue.",
        L"Total de tareas en la cola de tareas.");

    st->kpiTipText[8] = Tr(
        L"現在実行中のタスク数。",
        L"Number of tasks currently running.",
        L"Número de tareas en ejecución actualmente.");

    std::wstring tqWaitTip = std::to_wstring(tqCounts.waiting);
    if (tqCounts.pending > 0) {
        tqWaitTip += std::wstring(L" (+") + std::to_wstring(tqCounts.pending) +
            Tr(L" 依存関係保留)", L" pending for deps)", L" pendientes por dependencias)");
    }
    st->kpiTipText[9] = std::wstring(Tr(
        L"依存関係が満たされ実行準備が整っているタスク。",
        L"Tasks ready to run with all dependencies satisfied.",
        L"Tareas listas para ejecutarse con dependencias cumplidas.")) + L" (" + tqWaitTip + L")";

    st->kpiTipText[10] = Tr(
        L"正常に成功・完了したタスク。",
        L"Tasks that finished successfully.",
        L"Tareas finalizadas con éxito.");

    st->kpiTipText[11] = Tr(
        L"試行回数上限またはタイムアウト等で失敗したタスク、またはキャンセルされたタスク。",
        L"Tasks that failed after max attempts/timeout, or were cancelled.",
        L"Tareas fallidas tras agotar intentos/tiempo límite o canceladas.");

    st->kpiTipText[12] = Tr(
        L"未完了の先行タスク待ち (依存保留)、または先行タスク失敗によりブロックされたタスク。",
        L"Tasks waiting for unfinished dependencies, or blocked by upstream failure.",
        L"Tareas en espera de dependencias o bloqueadas por fallo aguas arriba.");

    st->kpiTipText[13] = Tr(
        L"未完了タスク（実行中・待機・保留のうち実行されうるもの）の残り時間の合計。見積もり時間があればそれを使い、なければタイムアウト値で見積もります。ブロックされて実行されないタスクは含みません。",
        L"Total remaining time across unfinished tasks that can still run (running, waiting, pending). Uses the job's estimate when present, otherwise its timeout. Blocked tasks that will never run are not included.",
        L"Tiempo restante total de las tareas sin terminar que aún pueden ejecutarse (en ejecución, en espera, pendientes). Usa la estimación del trabajo si existe; si no, su tiempo límite. Las tareas bloqueadas que nunca se ejecutarán no se incluyen.");
    if (tqCounts.sumRemainingSec >= 0) {
        st->kpiTipText[13] += std::wstring(L"\n") + Tr(L"合計残り時間: ", L"Total remaining: ", L"Restante total: ") + FormatRemainingTime(tqCounts.sumRemainingSec);
    }
    if (taskMinRemainSec >= 0) {
        st->kpiTipText[13] += std::wstring(L"\n") + Tr(L"最短: ", L"Shortest: ", L"Más corto: ") + FormatRemainingTime(taskMinRemainSec);
    }

    for (int i = 0; i < kNumKpiCards; ++i) {
        TOOLINFOW ti = {};
        ti.cbSize = sizeof(ti);
        ti.hwnd = st->hwndMain;
        ti.uId = static_cast<UINT_PTR>(i + 1);
        ti.rect = st->kpiCardRects[i];
        ti.lpszText = const_cast<wchar_t*>(st->kpiTipText[i].c_str());
        SendMessageW(st->hwndKpiTip, TTM_NEWTOOLRECTW, ti.uId, reinterpret_cast<LPARAM>(&ti.rect));
        SendMessageW(st->hwndKpiTip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
    }
}

// Position the active tab's TerminalView Direct2D child window inside rcTerminalArea
// and synchronize the WC_TABCONTROLW tab bar (st->hwndTab / g_tabHwnd).
static void LayoutTerminalViews(DashboardState* st) {
    if (!st) return;

    g_appTabs.clear();
    std::vector<ProcessSnapshot> termProcs;
    int activeTabIdx = -1;
    bool foundSelected = false;

    for (const auto& s : st->cachedSnapshots) {
        if (s.terminalHwnd && IsWindow(s.terminalHwnd)) {
            AppTabInfo tab;
            tab.hwnd = s.terminalHwnd;
            std::wstring stateShort = (s.state == ProcState::Running) ? L"RUN" : L"END";
            tab.label = L"#" + std::to_wstring(s.id) + L" " + Utf8ToWide(s.name) + L" [" + stateShort + L"]";
            tab.type = TAB_TYPE_TERMINAL;
            tab.data = reinterpret_cast<void*>(static_cast<intptr_t>(s.id));
            g_appTabs.push_back(tab);

            if (s.id == st->selectedProcId) {
                activeTabIdx = static_cast<int>(g_appTabs.size()) - 1;
                foundSelected = true;
            }
            termProcs.push_back(s);
        }
    }

    if (!g_appTabs.empty()) {
        if (foundSelected) {
            g_activeAppTab = activeTabIdx;
        } else if (st->selectedProcId <= 0) {
            g_activeAppTab = 0;
            st->selectedProcId = static_cast<int>(reinterpret_cast<intptr_t>(g_appTabs[g_activeAppTab].data));
        } else if (g_activeAppTab < 0 || g_activeAppTab >= static_cast<int>(g_appTabs.size())) {
            // Keep the current terminal when the selected process has no TerminalView.
            g_activeAppTab = 0;
        }
    } else {
        g_activeAppTab = 0;
    }

    // Synchronize Win32 Tab Control (st->hwndTab)
    if (st->hwndTab && IsWindow(st->hwndTab)) {
        g_suppressTabChange = true;
        int curTabCount = TabCtrl_GetItemCount(st->hwndTab);
        int targetTabCount = static_cast<int>(g_appTabs.size());

        while (curTabCount > targetTabCount) {
            TabCtrl_DeleteItem(st->hwndTab, curTabCount - 1);
            curTabCount--;
        }
        for (int i = 0; i < targetTabCount; ++i) {
            std::wstring lbl = g_appTabs[i].label;
            TCITEMW tie = {};
            tie.mask = TCIF_TEXT | TCIF_PARAM;
            tie.pszText = lbl.data();
            tie.lParam = reinterpret_cast<LPARAM>(g_appTabs[i].data);
            if (i >= curTabCount) {
                TabCtrl_InsertItem(st->hwndTab, i, &tie);
            } else {
                wchar_t existingText[256] = {};
                TCITEMW curItem = {};
                curItem.mask = TCIF_TEXT;
                curItem.pszText = existingText;
                curItem.cchTextMax = 255;
                TabCtrl_GetItem(st->hwndTab, i, &curItem);
                if (lbl != existingText) {
                    TabCtrl_SetItem(st->hwndTab, i, &tie);
                }
            }
        }
        if (targetTabCount > 0 && TabCtrl_GetCurSel(st->hwndTab) != g_activeAppTab) {
            TabCtrl_SetCurSel(st->hwndTab, g_activeAppTab);
        }
        g_suppressTabChange = false;
    }

    st->termBanners.clear();

    if (st->hideTerminalGrid) {
        for (const auto& s : st->cachedSnapshots) {
            if (s.terminalHwnd && IsWindow(s.terminalHwnd) && IsWindowVisible(s.terminalHwnd)) {
                ShowWindow(s.terminalHwnd, SW_HIDE);
            }
        }
        return;
    }

    if (termProcs.empty()) return;

    const ProcessSnapshot& activeSnap = termProcs[g_activeAppTab];

    // Hide all other TerminalView child windows; show only the active tab's TerminalView
    for (const auto& s : termProcs) {
        if (s.id != activeSnap.id && IsWindowVisible(s.terminalHwnd)) {
            ShowWindow(s.terminalHwnd, SW_HIDE);
        }
    }

    RECT area = st->rcTerminalArea;
    int headerH = 26;
    RECT rcCell = area;
    RECT rcBanner = {rcCell.left, rcCell.top, rcCell.right, rcCell.top + headerH};
    st->termBanners.push_back({rcBanner, rcCell, activeSnap.id});

    RECT rcTv = {rcCell.left + 2, rcCell.top + headerH, rcCell.right - 2, rcCell.bottom - 2};
    int tvW = (std::max)(200L, rcTv.right - rcTv.left);
    int tvH = (std::max)(120L, rcTv.bottom - rcTv.top);

    HWND hTv = activeSnap.terminalHwnd;
    RECT rcCur;
    GetWindowRect(hTv, &rcCur);
    MapWindowPoints(nullptr, st->hwndMain, reinterpret_cast<POINT*>(&rcCur), 2);
    if (rcCur.left != rcTv.left || rcCur.top != rcTv.top ||
        (rcCur.right - rcCur.left) != tvW || (rcCur.bottom - rcCur.top) != tvH) {
        MoveWindow(hTv, rcTv.left, rcTv.top, tvW, tvH, TRUE);
    }
    if (!IsWindowVisible(hTv)) {
        ShowWindow(hTv, SW_SHOW);
    }
}

static void DrawDashboardCanvas(DashboardState* st, HDC hdcWindow) {
    RECT rcClient;
    GetClientRect(st->hwndMain, &rcClient);
    int width = rcClient.right - rcClient.left;
    int height = rcClient.bottom - rcClient.top;
    if (width <= 0 || height <= 0) return;

    HDC hdc = CreateCompatibleDC(hdcWindow);
    HBITMAP hbm = CreateCompatibleBitmap(hdcWindow, width, height);
    HGDIOBJ oldBm = SelectObject(hdc, hbm);

    HBRUSH bgBrush = CreateSolidBrush(g_theme.windowBg);
    FillRect(hdc, &rcClient, bgBrush);
    DeleteObject(bgBrush);

    const auto& kpiSnaps = st->allSnapshots.empty() ? st->cachedSnapshots : st->allSnapshots;
    int totalCount = static_cast<int>(kpiSnaps.size());
    int activeRunning = 0;
    int idleWaiting = 0;
    int completedOk = 0;
    int failedOrKilled = 0;
    uint32_t totalTreeProcs = 0;
    uint64_t totalMemBytes = 0;

    for (const auto& s : kpiSnaps) {
        if (s.state == ProcState::Running) {
            if (s.idleMs >= 10000) idleWaiting++;
            else activeRunning++;
            totalTreeProcs += s.treeProcessCount;
            totalMemBytes += s.treeMemoryBytes;
        } else if (s.state == ProcState::Completed) {
            completedOk++;
        } else {
            failedOrKilled++;
        }
    }
    TaskQueueCounts tqCounts = FetchTaskQueueCountsIpc(st);
    st->activeRunningCount = activeRunning + tqCounts.running;
    // Any process still in the Running state (including idle-waiting ones) or any running queue job.
    st->runningTotalCount = activeRunning + idleWaiting + tqCounts.running;

    int64_t minRemainingSec = -1;
    for (const auto& s : kpiSnaps) {
        if (s.state == ProcState::Running && s.timeoutSec > 0) {
            int64_t remainingMs = static_cast<int64_t>(s.timeoutSec) * 1000 - static_cast<int64_t>(s.elapsedMs);
            int64_t remSec = (remainingMs + 999) / 1000;
            if (remSec < 0) remSec = 0;
            if (minRemainingSec < 0 || remSec < minRemainingSec) {
                minRemainingSec = remSec;
            }
        }
    }

    // 1. Top KPI Cards (2 rows: Row 0 = Processes, Row 1 = Tasks)
    int kpiMargin = 10;
    int kpiGap = 7;
    int numCols = 7;
    int availW = width - kpiMargin * 2 - kpiGap * (numCols - 1);
    int cardW = (std::max)(90, availW / numCols);

    for (int row = 0; row < 2; ++row) {
        int cardTop = (row == 0) ? kHeaderKpiTop : kHeaderKpiRow2Top;
        int cardBottom = (row == 0) ? kHeaderKpiRow1Bottom : kHeaderKpiBottom;

        for (int col = 0; col < numCols; ++col) {
            int i = row * numCols + col;
            RECT rcCard;
            rcCard.left = kpiMargin + col * (cardW + kpiGap);
            rcCard.right = (col == numCols - 1) ? (width - kpiMargin) : (rcCard.left + cardW);
            rcCard.top = cardTop;
            rcCard.bottom = cardBottom;
            st->kpiCardRects[i] = rcCard;

            if (row == 0) {
                // Top Row: Processes
                if (col == 0) {
                    std::wstring sub = (totalTreeProcs > static_cast<uint32_t>(totalCount))
                        ? (std::to_wstring(totalTreeProcs) + Tr(L" 子", L" child", L" sub")) : L"";
                    DrawKpiCard(hdc, rcCard, Tr(L"プロセス総数", L"TOTAL PROCESSES", L"PROCESOS TOTALES"),
                                std::to_wstring(totalCount), KpiAccent(0), st->hFontUiBold, st->hFontKpiVal,
                                -1.0f, sub);
                } else if (col == 1) {
                    DrawKpiCard(hdc, rcCard, Tr(L"稼働中プロセス", L"RUNNING PROCS", L"PROCESOS ACTIVOS"),
                                std::to_wstring(activeRunning), KpiAccent(1), st->hFontUiBold, st->hFontKpiVal,
                                (activeRunning > 0 ? g_kpiBarPhase : -1.0f));
                } else if (col == 2) {
                    DrawKpiCard(hdc, rcCard, Tr(L"待機 / アイドル", L"WAITING / IDLE", L"ESPERANDO / INACTIVO"),
                                std::to_wstring(idleWaiting), KpiAccent(2), st->hFontUiBold, st->hFontKpiVal);
                } else if (col == 3) {
                    DrawKpiCard(hdc, rcCard, Tr(L"完了 (OK)", L"COMPLETED (OK)", L"COMPLETADO (OK)"),
                                std::to_wstring(completedOk), KpiAccent(3), st->hFontUiBold, st->hFontKpiVal);
                } else if (col == 4) {
                    DrawKpiCard(hdc, rcCard, Tr(L"失敗 / 強制終了", L"FAILED / KILLED", L"FALLIDO / TERMINADO"),
                                std::to_wstring(failedOrKilled), KpiAccent(4), st->hFontUiBold, st->hFontKpiVal);
                } else if (col == 5) {
                    std::wstring memStr = Utf8ToWide(FormatBytes(totalMemBytes));
                    DrawKpiCard(hdc, rcCard, Tr(L"ジョブメモリ", L"JOB MEMORY", L"MEMORIA TOTAL"),
                                memStr, KpiAccent(5), st->hFontUiBold, st->hFontKpiVal);
                } else if (col == 6) {
                    std::wstring val = (minRemainingSec >= 0) ? FormatRemainingTimeShort(minRemainingSec) : Tr(L"なし", L"None", L"Ninguno");
                    DrawKpiCard(hdc, rcCard, Tr(L"残り時間", L"PROC TIMEOUT", L"TIEMPO RESTANTE"),
                                val, KpiAccent(6), st->hFontUiBold, st->hFontKpiVal);
                }
            } else {
                // Bottom Row: Tasks
                if (col == 0) {
                    DrawKpiCard(hdc, rcCard, Tr(L"タスク総数", L"TOTAL TASKS", L"TAREAS TOTALES"),
                                std::to_wstring(tqCounts.total), KpiAccent(0), st->hFontUiBold, st->hFontKpiVal);
                } else if (col == 1) {
                    DrawKpiCard(hdc, rcCard, Tr(L"稼働中タスク", L"RUNNING TASKS", L"TAREAS EN EJECUCIÓN"),
                                std::to_wstring(tqCounts.running), KpiAccent(1), st->hFontUiBold, st->hFontKpiVal,
                                (tqCounts.running > 0 ? g_kpiBarPhase : -1.0f));
                } else if (col == 2) {
                    std::wstring sub = (tqCounts.pending > 0) ? (L"(+" + std::to_wstring(tqCounts.pending) + L")") : L"";
                    DrawKpiCard(hdc, rcCard, Tr(L"実行待ちタスク", L"WAITING TASKS", L"TAREAS EN ESPERA"),
                                std::to_wstring(tqCounts.waiting), KpiAccent(2), st->hFontUiBold, st->hFontKpiVal,
                                -1.0f, sub);
                } else if (col == 3) {
                    DrawKpiCard(hdc, rcCard, Tr(L"タスク成功 (OK)", L"SUCCEEDED TASKS", L"TAREAS EXITOSAS"),
                                std::to_wstring(tqCounts.succeeded), KpiAccent(3), st->hFontUiBold, st->hFontKpiVal);
                } else if (col == 4) {
                    std::wstring sub = (tqCounts.cancelled > 0) ? (L"(" + std::to_wstring(tqCounts.cancelled) + L")") : L"";
                    DrawKpiCard(hdc, rcCard, Tr(L"タスク失敗/中断", L"FAILED / CANCELLED", L"FALLIDAS / CANCELADAS"),
                                std::to_wstring(tqCounts.failed + tqCounts.cancelled), KpiAccent(4), st->hFontUiBold, st->hFontKpiVal,
                                -1.0f, sub);
                } else if (col == 5) {
                    std::wstring sub = (tqCounts.blocked > 0) ? (L"(" + std::to_wstring(tqCounts.blocked) + L")") : L"";
                    DrawKpiCard(hdc, rcCard, Tr(L"保留 / ブロック", L"PENDING / BLOCKED", L"PENDIENTES / BLOQ"),
                                std::to_wstring(tqCounts.pending + tqCounts.blocked), KpiAccent(5), st->hFontUiBold, st->hFontKpiVal,
                                ((tqCounts.pending + tqCounts.blocked) > 0 ? g_kpiBarPhase : -1.0f), sub);
                } else if (col == 6) {
                    std::wstring val = (tqCounts.sumRemainingSec >= 0) ? FormatRemainingTimeShort(tqCounts.sumRemainingSec) : Tr(L"なし", L"None", L"Ninguno");
                    DrawKpiCard(hdc, rcCard, Tr(L"残り時間", L"TASK REMAINING", L"RESTANTE TOTAL"),
                                val, KpiAccent(6), st->hFontUiBold, st->hFontKpiVal);
                }
            }
        }
    }
    UpdateKpiTooltips(st, totalTreeProcs, activeRunning, idleWaiting, completedOk, failedOrKilled,
                      tqCounts, minRemainingSec, tqCounts.minRemainingSec);

    // Draggable splitter between process list and task list (when 2 rows)
    if (st->rcRowsSplitter.bottom > st->rcRowsSplitter.top && st->listDisplayMode == ListDisplayMode::TwoRows) {
        HBRUSH spBrush = CreateSolidBrush(st->rowsSplitterDragging ? g_theme.border : g_theme.panel);
        FillRect(hdc, &st->rcRowsSplitter, spBrush);
        DeleteObject(spBrush);

        int gripY = (st->rcRowsSplitter.top + st->rcRowsSplitter.bottom) / 2;
        int gripCx = (st->rcRowsSplitter.left + st->rcRowsSplitter.right) / 2;
        HPEN gripPen = CreatePen(PS_SOLID, 1, st->rowsSplitterDragging ? g_theme.accent : g_theme.muted);
        HGDIOBJ oldGripPen = SelectObject(hdc, gripPen);
        for (int dx = -14; dx <= 14; dx += 14) {
            MoveToEx(hdc, gripCx + dx - 5, gripY, nullptr);
            LineTo(hdc, gripCx + dx + 5, gripY);
        }
        SelectObject(hdc, oldGripPen);
        DeleteObject(gripPen);
    }

    // Draggable splitter above the JSON stream pane
    if (st->rcStreamSplitter.bottom > st->rcStreamSplitter.top) {
        HBRUSH spBrush = CreateSolidBrush(st->streamSplitterDragging ? g_theme.border : g_theme.panel);
        FillRect(hdc, &st->rcStreamSplitter, spBrush);
        DeleteObject(spBrush);

        int gripY = (st->rcStreamSplitter.top + st->rcStreamSplitter.bottom) / 2;
        int gripCx = (st->rcStreamSplitter.left + st->rcStreamSplitter.right) / 2;
        HPEN gripPen = CreatePen(PS_SOLID, 1, st->streamSplitterDragging ? g_theme.accent : g_theme.muted);
        HGDIOBJ oldGripPen = SelectObject(hdc, gripPen);
        for (int dx = -14; dx <= 14; dx += 14) {
            MoveToEx(hdc, gripCx + dx - 5, gripY, nullptr);
            LineTo(hdc, gripCx + dx + 5, gripY);
        }
        SelectObject(hdc, oldGripPen);
        DeleteObject(gripPen);
    }

    // Draggable splitter between the process list and the terminal area
    if (st->rcSplitter.bottom > st->rcSplitter.top) {
        HBRUSH spBrush = CreateSolidBrush(st->splitterDragging ? g_theme.border : g_theme.panel);
        FillRect(hdc, &st->rcSplitter, spBrush);
        DeleteObject(spBrush);

        int gripY = (st->rcSplitter.top + st->rcSplitter.bottom) / 2;
        int gripCx = (st->rcSplitter.left + st->rcSplitter.right) / 2;
        HPEN gripPen = CreatePen(PS_SOLID, 1, st->splitterDragging ? g_theme.accent : g_theme.muted);
        HGDIOBJ oldGripPen = SelectObject(hdc, gripPen);
        for (int dx = -14; dx <= 14; dx += 14) {
            MoveToEx(hdc, gripCx + dx - 5, gripY, nullptr);
            LineTo(hdc, gripCx + dx + 5, gripY);
        }
        SelectObject(hdc, oldGripPen);
        DeleteObject(gripPen);
    }

    if (st->rcProcCaption.bottom > st->rcProcCaption.top) {
        DrawListCaption(hdc, st->rcProcCaption, Tr(L"プロセス", L"PROCESSES", L"PROCESOS"),
                        g_theme.accent, st->hFontUiBold);
    }
    if (st->rcTaskCaption.bottom > st->rcTaskCaption.top) {
        std::wstring caption = Tr(L"タスクキュー", L"TASK QUEUE", L"COLA DE TAREAS");
        if (!st->taskFailureSummary.empty()) {
            caption += L"    " + st->taskFailureSummary;
        }
        DrawListCaption(hdc, st->rcTaskCaption, caption.c_str(),
                        IsDarkThemeActive() ? RGB(255, 190, 90) : RGB(200, 110, 0), st->hFontUiBold);
    }

    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, g_theme.text);
    HGDIOBJ oldFont = SelectObject(hdc, st->hFontUiBold);

    // 2. Draw Headers & Borders around the active TerminalView tab
    if (st->termBanners.empty()) {
        if (st->rcTerminalArea.bottom > st->rcTerminalArea.top) {
            RECT rcEmpty = st->rcTerminalArea;
            HBRUSH emptyBrush = CreateSolidBrush(g_theme.emptyBg);
            HPEN emptyPen = CreatePen(PS_SOLID, 1, g_theme.border);
            SelectObject(hdc, emptyBrush);
            SelectObject(hdc, emptyPen);
            RoundRect(hdc, rcEmpty.left, rcEmpty.top, rcEmpty.right, rcEmpty.bottom, 6, 6);
            DeleteObject(emptyBrush);
            DeleteObject(emptyPen);

            SetTextColor(hdc, g_theme.muted);
            SelectObject(hdc, st->hFontUiBold);
            DrawTextW(hdc, Tr(L"アクティブな TerminalView がありません。[+ Launch] または Claude CLI / MCP から起動してください。",
                                  L"No active TerminalView sessions. Click [+ New Shell] above or start via Claude CLI / MCP.",
                                  L"No hay sesiones TerminalView activas. Use [+ Launch] o inicie desde Claude CLI / MCP."),
                      -1, &rcEmpty, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }
    } else {
        for (const auto& tb : st->termBanners) {
            const ProcessSnapshot* snap = nullptr;
            for (const auto& s : st->cachedSnapshots) {
                if (s.id == tb.procId) { snap = &s; break; }
            }
            if (!snap) continue;

            bool isSelected = (snap->id == st->selectedProcId);
            COLORREF borderCol = isSelected ? g_theme.accent : g_theme.border;

            // Outer frame around TerminalView
            HPEN framePen = CreatePen(PS_SOLID, isSelected ? 2 : 1, borderCol);
            HBRUSH frameBrush = CreateSolidBrush(g_theme.panel);
            SelectObject(hdc, framePen);
            SelectObject(hdc, frameBrush);
            Rectangle(hdc, tb.rcBorder.left, tb.rcBorder.top, tb.rcBorder.right, tb.rcBorder.bottom);
            DeleteObject(framePen);
            DeleteObject(frameBrush);

            // Status badge on right of banner
            COLORREF badgeCol = GetStateBadgeColor(*snap);
            RECT rcBadge = {tb.rcBanner.right - 120, tb.rcBanner.top + 3, tb.rcBanner.right - 6, tb.rcBanner.bottom - 3};
            HBRUSH badgeBrush = CreateSolidBrush(badgeCol);
            FillRect(hdc, &rcBadge, badgeBrush);
            DeleteObject(badgeBrush);

            SetTextColor(hdc, RGB(255, 255, 255));
            SelectObject(hdc, st->hFontUiBold);
            std::wstring stateStr = StateLabelWide(snap->state, snap->idleMs >= 10000);
            DrawTextW(hdc, stateStr.c_str(), -1, &rcBadge, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

            // Title text on left of banner (includes Start Time & Uptime HH:MM:SS)
            std::wstringstream ss;
            ss << L"#" << snap->id;
            if (!snap->agentType.empty()) {
                ss << L" [" << Utf8ToWide(snap->agentType) << L"]";
            }
            ss << L" [" << Utf8ToWide(snap->name) << L"]  PID: " << snap->rootPid
               << L" (" << snap->treeProcessCount << L" procs, " << Utf8ToWide(FormatBytes(snap->treeMemoryBytes))
               << Tr(L")  開始: ", L")  Start: ", L")  Inicio: ") << Utf8ToWide(snap->startTimeStr)
               << Tr(L"  稼働時間: ", L"  Uptime: ", L"  Tiempo activo: ") << Utf8ToWide(FormatUptimeHMS(snap->elapsedMs));
            if (snap->state == ProcState::Running) {
                if (snap->timeoutSec > 0) {
                    int64_t remainingMs = static_cast<int64_t>(snap->timeoutSec) * 1000 - static_cast<int64_t>(snap->elapsedMs);
                    int64_t remSec = (remainingMs + 999) / 1000;
                    if (remSec < 0) remSec = 0;
                    ss << Tr(L"  残り時間: ", L"  Left: ", L"  Restante: ") << FormatRemainingTime(remSec);
                } else if (!snap->estimatedEndTimeStr.empty()) {
                    ss << Tr(L"  予定終了: ~", L"  Est. End: ~", L"  Fin est.: ~") << Utf8ToWide(snap->estimatedEndTimeStr);
                }
            }
            ss << L"  —  " << Utf8ToWide(snap->commandLine);
            RECT rcTitle = {tb.rcBanner.left + 8, tb.rcBanner.top + 2, rcBadge.left - 8, tb.rcBanner.bottom - 2};
            SetTextColor(hdc, isSelected ? g_theme.accent : g_theme.text);
            DrawTextW(hdc, ss.str().c_str(), -1, &rcTitle, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            // Terminal viewport backing (used when PrintWindow captures parent GDI surface)
            RECT rcTv = {tb.rcBorder.left + 2, tb.rcBanner.bottom, tb.rcBorder.right - 2, tb.rcBorder.bottom - 2};
            HBRUSH tvBrush = CreateSolidBrush(g_theme.terminalBg);
            FillRect(hdc, &rcTv, tvBrush);
            DeleteObject(tvBrush);
            if (st->pm) {
                TerminalView* tv = st->pm->GetTerminalView(snap->id);
                if (tv) {
                    std::wstring termText = tv->ExtractScreenText(0);
                    if (!termText.empty()) {
                        HFONT hFontMono = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                                      FIXED_PITCH | FF_MODERN, L"Consolas");
                        HGDIOBJ oldMono = SelectObject(hdc, hFontMono);
                        SetTextColor(hdc, g_theme.text);
                        RECT rcPad = {rcTv.left + 8, rcTv.top + 6, rcTv.right - 8, rcTv.bottom - 6};
                        DrawTextW(hdc, termText.c_str(), -1, &rcPad, DT_LEFT | DT_TOP | DT_NOPREFIX);
                        SelectObject(hdc, oldMono);
                        DeleteObject(hFontMono);
                    }
                }
            }
        }
    }

    // 3. Bottom Status Bar (replaces the old Send Input / Ctrl+C / Focus Term area)
    RECT rcSb = st->rcStatusBar;
    HBRUSH sbBrush = CreateSolidBrush(g_theme.panel);
    FillRect(hdc, &rcSb, sbBrush);
    DeleteObject(sbBrush);

    HPEN sbTopPen = CreatePen(PS_SOLID, 1, g_theme.border);
    SelectObject(hdc, sbTopPen);
    MoveToEx(hdc, rcSb.left, rcSb.top, nullptr);
    LineTo(hdc, rcSb.right, rcSb.top);
    DeleteObject(sbTopPen);

    const ProcessSnapshot* selSnap = nullptr;
    for (const auto& s : st->cachedSnapshots) {
        if (s.id == st->selectedProcId) { selSnap = &s; break; }
    }

    std::wstringstream sbLeft;
    if (selSnap) {
        sbLeft << Tr(L"選択: #", L"Selected: #", L"Seleccionado: #") << selSnap->id << L" [" << Utf8ToWide(selSnap->name) << L"]  |  "
               << StateLabelWide(selSnap->state, selSnap->idleMs >= 10000)
               << L"  |  " << Tr(L"開始: ", L"Start: ", L"Inicio: ") << Utf8ToWide(selSnap->startTimeStr)
               << L"  |  " << Tr(L"稼働時間: ", L"Uptime: ", L"Tiempo activo: ") << Utf8ToWide(FormatUptimeHMS(selSnap->elapsedMs));
        if (selSnap->state == ProcState::Running && selSnap->timeoutSec > 0) {
            int64_t remainingMs = static_cast<int64_t>(selSnap->timeoutSec) * 1000 - static_cast<int64_t>(selSnap->elapsedMs);
            int64_t remSec = (remainingMs + 999) / 1000;
            if (remSec < 0) remSec = 0;
            sbLeft << L"  |  " << Tr(L"残り時間: ", L"Left: ", L"Restante: ") << FormatRemainingTime(remSec);
        }
        if (!selSnap->logFilePath.empty()) {
            sbLeft << L"  |  " << Tr(L"ログ: ", L"Log: ", L"Registro: ") << Utf8ToWide(selSnap->logFilePath);
        }
        if (st->pm) {
            TerminalView* tv = st->pm->GetTerminalView(selSnap->id);
            if (tv && !tv->Title().empty()) {
                sbLeft << L"  |  " << Tr(L"タイトル: ", L"Title: ", L"Título: ") << tv->Title();
            }
        }
        if (st->termProgressState > 0) {
            sbLeft << L"  |  " << Tr(L"進捗: ", L"Progress: ", L"Progreso: ") << st->termProgressValue << L"%";
        }
    } else {
        sbLeft << Tr(L"待機 — ログ保存先: ", L"Ready — Logs saved to: ", L"Listo — Registros en: ") << GetLogsDirectoryWide();
    }
    sbLeft << Tr(L"  |  Ctrl+T: プロセス / タスクキュー切替",
                 L"  |  Ctrl+T: switch Processes / Task Queue",
                 L"  |  Ctrl+T: cambiar Procesos / Cola de tareas");
    if (st->memoryNearLimit && !st->memoryWarningText.empty()) {
        sbLeft << L"  |  [" << st->memoryWarningText << L"]";
    }

    uint64_t dashElapsedMs = GetNowTickMs() - st->dashboardStartTickMs;
    std::wstringstream sbRight;
    sbRight << Tr(L"ダッシュボード起動: ", L"Dashboard Started: ", L"Panel iniciado: ") << Utf8ToWide(st->dashboardStartTimeStr)
            << L"  |  " << Tr(L"稼働時間: ", L"Uptime: ", L"Tiempo activo: ") << Utf8ToWide(FormatUptimeHMS(dashElapsedMs))
            << L"  |  IPC: \\\\.\\pipe\\HanchoIPC";

    SelectObject(hdc, st->hFontUi);
    RECT rcSbRight = {rcSb.right - 520, rcSb.top + 2, rcSb.right - 10, rcSb.bottom - 2};
    SetTextColor(hdc, g_theme.muted);
    DrawTextW(hdc, sbRight.str().c_str(), -1, &rcSbRight, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT rcSbLeft = {rcSb.left + 10, rcSb.top + 2, rcSbRight.left - 12, rcSb.bottom - 2};
    SetTextColor(hdc, g_theme.text);
    DrawTextW(hdc, sbLeft.str().c_str(), -1, &rcSbLeft, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (st->showUsagePanel && st->rcUsageSplitter.bottom > st->rcUsageSplitter.top) {
        RECT rc = st->rcUsageSplitter;
        HBRUSH spBrush = CreateSolidBrush(st->usageSplitterDragging ? g_theme.border : g_theme.panel);
        FillRect(hdc, &rc, spBrush);
        DeleteObject(spBrush);
        HPEN gripPen = CreatePen(PS_SOLID, 1, st->usageSplitterDragging ? g_theme.accent : g_theme.muted);
        HGDIOBJ oldPen = SelectObject(hdc, gripPen);
        int midY = (rc.top + rc.bottom) / 2;
        MoveToEx(hdc, rc.left + 40, midY, nullptr);
        LineTo(hdc, rc.right - 40, midY);
        SelectObject(hdc, oldPen);
        DeleteObject(gripPen);
    }

    SelectObject(hdc, oldFont);
    BitBlt(hdcWindow, 0, 0, width, height, hdc, 0, 0, SRCCOPY);
    SelectObject(hdc, oldBm);
    DeleteObject(hbm);
    DeleteDC(hdc);
}

static const wchar_t* ColumnTitle(int index) {
    static const wchar_t* const ja[14] = {
        L"ID", L"Windows PID", L"エージェント / 名前", L"状態", L"開始時刻", L"終了時刻 / 残り時間", L"稼働時間",
        L"ツリー内プロセス", L"メモリ", L"CPU時間", L"アイドル", L"最新出力プレビュー",
        L"コマンドライン", L"ジョブ"
    };
    static const wchar_t* const en[14] = {
        L"ID", L"Windows PID", L"Agent / Name", L"Status", L"Start Time", L"End Time / Left", L"Uptime",
        L"Tree Procs", L"Memory", L"CPU Time", L"Idle", L"Latest Output Preview",
        L"Command Line", L"Job"
    };
    static const wchar_t* const es[14] = {
        L"ID", L"PID de Windows", L"Agente / Nombre", L"Estado", L"Hora de inicio", L"Fin / Restante", L"Tiempo activo",
        L"Procs. del árbol", L"Memoria", L"Tiempo de CPU", L"Inactivo", L"Última salida",
        L"Línea de comandos", L"Trabajo"
    };
    if (g_uiLanguage == UiLanguage::Spanish) return es[index];
    if (g_uiLanguage == UiLanguage::English) return en[index];
    return ja[index];
}
static void UpdateListHeaderSortIndicator(DashboardState* st) {
    if (!st || !st->hwndList) return;
    HWND hHeader = ListView_GetHeader(st->hwndList);
    for (int i = 0; i < 14; ++i) {
        std::wstring title = ColumnTitle(i);
        bool sorted = (st->sortDirection != SortDirection::None) && (i == st->sortColumn);
        if (sorted) {
            title += (st->sortDirection == SortDirection::Ascending) ? L" ▲" : L" ▼";
        }
        LVCOLUMNW lvc = {};
        lvc.mask = LVCF_TEXT;
        lvc.pszText = title.data();
        ListView_SetColumn(st->hwndList, i, &lvc);

        if (hHeader) {
            HDITEMW hdi = {};
            hdi.mask = HDI_FORMAT;
            if (Header_GetItem(hHeader, i, &hdi)) {
                hdi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
                if (sorted) {
                    hdi.fmt |= (st->sortDirection == SortDirection::Ascending) ? HDF_SORTUP : HDF_SORTDOWN;
                }
                Header_SetItem(hHeader, i, &hdi);
            }
        }
    }
}

// Task list columns: ID, Summary, State, Start, End, Running, Depends, Queue, Origin, Resources.
static constexpr int kTaskListColumns = 10;

static const wchar_t* TaskColumnTitle(int index) {
    static const wchar_t* const ja[kTaskListColumns] = {
        L"ID", L"サマリ", L"状態", L"開始時刻", L"終了 / 残り時間", L"実行中", L"依存 / 理由", L"キュー", L"発行元", L"リソース"
    };
    static const wchar_t* const en[kTaskListColumns] = {
        L"ID", L"Summary", L"State", L"Start", L"End / Left", L"Running", L"Depends / Reason", L"Queue", L"Origin", L"Resources"
    };
    static const wchar_t* const es[kTaskListColumns] = {
        L"ID", L"Resumen", L"Estado", L"Inicio", L"Fin / Restante", L"En curso", L"Dependencias", L"Cola", L"Origen", L"Recursos"
    };
    if (g_uiLanguage == UiLanguage::Spanish) return es[index];
    if (g_uiLanguage == UiLanguage::English) return en[index];
    return ja[index];
}

static void UpdateTaskListHeaders(DashboardState* st) {
    if (!st || !st->hwndTaskList) return;
    HWND hHeader = ListView_GetHeader(st->hwndTaskList);
    for (int i = 0; i < kTaskListColumns; ++i) {
        bool sorted = (st->taskSortDirection != SortDirection::None) && (i == st->taskSortColumn);
        std::wstring title = TaskColumnTitle(i);
        if (sorted) {
            title += (st->taskSortDirection == SortDirection::Ascending) ? L" ▲" : L" ▼";
        }
        LVCOLUMNW lvc = {};
        lvc.mask = LVCF_TEXT;
        lvc.pszText = title.data();
        ListView_SetColumn(st->hwndTaskList, i, &lvc);
        if (hHeader) {
            HDITEMW hdi = {};
            hdi.mask = HDI_FORMAT;
            if (Header_GetItem(hHeader, i, &hdi)) {
                hdi.fmt &= ~(HDF_SORTUP | HDF_SORTDOWN);
                hdi.fmt |= HDF_OWNERDRAW;
                if (sorted) {
                    hdi.fmt |= (st->taskSortDirection == SortDirection::Ascending) ? HDF_SORTUP : HDF_SORTDOWN;
                }
                Header_SetItem(hHeader, i, &hdi);
            }
        }
    }
}

// One row of the task list: the cells to show plus the values a sort compares.
// The two time columns keep the full date and time, because the column itself
// only shows the time of day.
struct TaskListRow {
    std::wstring cells[kTaskListColumns];
    std::string id;
    int64_t startKey = 0;  // epoch seconds of the last attempt start (0: not started yet)
    int64_t endKey = 0;    // epoch seconds of the end, or of the estimated end (0: unknown)
    std::wstring originTip;
    std::string kind;      // running / timeout / failed / other, for the row colour
    int procId = 0;
};

// Compares two texts the way the list shows them, ignoring case.
static int CompareCellText(const std::wstring& a, const std::wstring& b) {
    const int r = _wcsicmp(a.c_str(), b.c_str());
    return (r < 0) ? -1 : (r > 0 ? 1 : 0);
}

// Compares two job ids so that "job-2" comes before "job-10": runs of digits
// compare as numbers, the rest case-insensitively.
static int CompareJobIds(const std::string& a, const std::string& b) {
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        const unsigned char ca = static_cast<unsigned char>(a[i]);
        const unsigned char cb = static_cast<unsigned char>(b[j]);
        if (isdigit(ca) && isdigit(cb)) {
            size_t si = i, sj = j;
            while (i < a.size() && isdigit(static_cast<unsigned char>(a[i]))) ++i;
            while (j < b.size() && isdigit(static_cast<unsigned char>(b[j]))) ++j;
            while (si + 1 < i && a[si] == '0') ++si;
            while (sj + 1 < j && b[sj] == '0') ++sj;
            const size_t la = i - si, lb = j - sj;
            if (la != lb) return (la < lb) ? -1 : 1;
            const int c = a.compare(si, la, b, sj, lb);
            if (c != 0) return (c < 0) ? -1 : 1;
            continue;
        }
        const int la = tolower(ca), lb = tolower(cb);
        if (la != lb) return (la < lb) ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size()) return 1;
    if (j < b.size()) return -1;
    return 0;
}

// Compares two rows by one column. A time that is not known sorts last.
static int CompareTaskRows(const TaskListRow& a, const TaskListRow& b, int col) {
    int cmp = 0;
    switch (col) {
        case 0:  // ID
            cmp = CompareJobIds(a.id, b.id);
            break;
        case 1:  // Summary
        case 2:  // State
        case 5:  // Running
        case 6:  // Depends / Reason
        case 7:  // Queue
        case 8:  // Origin
        case 9:  // Resources
            cmp = CompareCellText(a.cells[col], b.cells[col]);
            break;
        case 3:  // Start: the full date and time of the last attempt
        case 4:  // End: the full date and time of the end, or of the estimated end
        {
            const int64_t ka = (col == 3) ? a.startKey : a.endKey;
            const int64_t kb = (col == 3) ? b.startKey : b.endKey;
            if (ka == 0 && kb == 0) cmp = 0;
            else if (ka == 0) cmp = 1;
            else if (kb == 0) cmp = -1;
            else cmp = (ka < kb) ? -1 : (ka > kb ? 1 : 0);
            break;
        }
        default:
            cmp = 0;
            break;
    }
    if (cmp == 0) cmp = CompareJobIds(a.id, b.id);
    return cmp;
}

// Sorts the task rows by one column. SortDirection::None keeps the natural order.
static void SortTaskRows(std::vector<TaskListRow>& rows, int col, SortDirection dir) {
    if (dir == SortDirection::None || col < 0 || col >= kTaskListColumns) return;
    const bool asc = (dir == SortDirection::Ascending);
    std::stable_sort(rows.begin(), rows.end(), [col, asc](const TaskListRow& a, const TaskListRow& b) {
        const int cmp = CompareTaskRows(a, b, col);
        return asc ? (cmp < 0) : (cmp > 0);
    });
}

static void UpdateControlTooltip(HWND hwndTip, HWND hwndParent, HWND hwndCtrl, const wchar_t* text) {
    if (!hwndTip || !hwndCtrl) return;
    TOOLINFOW ti = {};
    ti.cbSize = sizeof(ti);
    ti.hwnd = hwndParent;
    ti.uId = reinterpret_cast<UINT_PTR>(hwndCtrl);
    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
    ti.lpszText = const_cast<wchar_t*>(text ? text : L"");
    SendMessageW(hwndTip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
}

static void UpdatePanelToggleLabels(DashboardState* st) {
    if (!st) return;
    if (st->hwndBtnToggleList) {
        SetWindowTextW(st->hwndBtnToggleList, L"☰");
    }
    if (st->hwndBtnToggleTerm) {
        SetWindowTextW(st->hwndBtnToggleTerm, L">_");
    }
    if (st->hwndToolbarTip) {
        if (st->hwndBtnToggleList) {
            UpdateControlTooltip(st->hwndToolbarTip, st->hwndMain, st->hwndBtnToggleList,
                st->hideProcessList ? Tr(L"リストを表示", L"Show List", L"Mostrar lista")
                                    : Tr(L"リストを非表示", L"Hide List", L"Ocultar lista"));
        }
        if (st->hwndBtnToggleTerm) {
            UpdateControlTooltip(st->hwndToolbarTip, st->hwndMain, st->hwndBtnToggleTerm,
                st->hideTerminalGrid ? Tr(L"ターミナルを表示", L"Show Terminal", L"Mostrar terminal")
                                     : Tr(L"ターミナルを非表示", L"Hide Terminal", L"Ocultar terminal"));
        }
        if (st->hwndBtnStreamPane) {
            UpdateControlTooltip(st->hwndToolbarTip, st->hwndMain, st->hwndBtnStreamPane,
                st->showStreamPane ? Tr(L"タスク監視ペインを非表示", L"Hide Monitor Pane", L"Ocultar panel de monitor")
                                   : Tr(L"タスク監視ペインを表示", L"Show Monitor Pane", L"Mostrar panel de monitor"));
        }
        if (st->hwndBtnBtView) {
            UpdateControlTooltip(st->hwndToolbarTip, st->hwndMain, st->hwndBtnBtView,
                Tr(L"ビヘイビアツリーを表示 (Ctrl+Shift+B)", L"Show Behavior Trees (Ctrl+Shift+B)",
                   L"Mostrar árboles de comportamiento (Ctrl+Shift+B)"));
        }
        if (st->hwndBtnRefresh) {
            UpdateControlTooltip(st->hwndToolbarTip, st->hwndMain, st->hwndBtnRefresh,
                Tr(L"すべてのリストを最新の情報に更新", L"Refresh All Lists", L"Actualizar todas las listas"));
        }
    }
    // Repaint the toggles so their latched (pressed) look follows the state.
    for (HWND b : {st->hwndBtnToggleList, st->hwndBtnToggleTerm, st->hwndBtnStreamPane}) {
        if (b) InvalidateRect(b, nullptr, FALSE);
    }
}

static void SortSnapshots(std::vector<ProcessSnapshot>& snaps, int col, SortDirection dir) {
    if (dir == SortDirection::None) return;
    bool asc = (dir == SortDirection::Ascending);
    std::sort(snaps.begin(), snaps.end(), [col, asc](const ProcessSnapshot& a, const ProcessSnapshot& b) {
        int cmp = 0;
        switch (col) {
            case 0: // ID
                cmp = (a.id < b.id) ? -1 : (a.id > b.id ? 1 : 0);
                break;
            case 1: // Windows PID
                cmp = (a.rootPid < b.rootPid) ? -1 : (a.rootPid > b.rootPid ? 1 : 0);
                break;
            case 2: // Name
                cmp = _stricmp(a.name.c_str(), b.name.c_str());
                break;
            case 3: // Status
                cmp = (static_cast<int>(a.state) < static_cast<int>(b.state)) ? -1 :
                      (static_cast<int>(a.state) > static_cast<int>(b.state) ? 1 : 0);
                break;
            case 4: // Start Time
                cmp = a.startTimeStr.compare(b.startTimeStr);
                break;
            case 5: // End Time
                cmp = a.endTimeStr.compare(b.endTimeStr);
                break;
            case 6: // Uptime
                cmp = (a.elapsedMs < b.elapsedMs) ? -1 : (a.elapsedMs > b.elapsedMs ? 1 : 0);
                break;
            case 7: // Tree process count
                cmp = (a.treeProcessCount < b.treeProcessCount) ? -1 : (a.treeProcessCount > b.treeProcessCount ? 1 : 0);
                break;
            case 8: // Memory
                cmp = (a.treeMemoryBytes < b.treeMemoryBytes) ? -1 : (a.treeMemoryBytes > b.treeMemoryBytes ? 1 : 0);
                break;
            case 9: // CPU Time
                cmp = (a.cpuTimeMs < b.cpuTimeMs) ? -1 : (a.cpuTimeMs > b.cpuTimeMs ? 1 : 0);
                break;
            case 10: // Idle
                cmp = (a.idleMs < b.idleMs) ? -1 : (a.idleMs > b.idleMs ? 1 : 0);
                break;
            case 11: // Latest Output Preview
                cmp = _stricmp(a.lastLinePreview.c_str(), b.lastLinePreview.c_str());
                break;
            case 12: // Command Line
                cmp = _stricmp(a.commandLine.c_str(), b.commandLine.c_str());
                break;
            default:
                cmp = (a.id < b.id) ? -1 : (a.id > b.id ? 1 : 0);
                break;
        }
        if (cmp == 0) {
            cmp = (a.id < b.id) ? -1 : (a.id > b.id ? 1 : 0);
        }
        return asc ? (cmp < 0) : (cmp > 0);
    });
}


static void PlayCompletionSound(const DashboardState& st) {
    if (!st.completionSoundEnabled) return;
    std::wstring path = st.completionSoundPath.empty() ? std::wstring() : Utf8ToWide(st.completionSoundPath);
    gui_platform::GetGuiPlatform()->PlayNotificationSound(path, false);
}

// Played when a process ends without completing successfully (Failed / TimedOut / IdleTimedOut / Killed).
static void PlayFailureSound(const DashboardState& st) {
    if (!st.completionSoundEnabled) return;
    gui_platform::GetGuiPlatform()->PlayNotificationSound(L"", true);
}

static void PlayProcessEndSound(const DashboardState& st, bool anyFailed) {
    if (anyFailed) {
        PlayFailureSound(st);
    } else {
        PlayCompletionSound(st);
    }
}


// ---- Row blink when a process or task ends --------------------------------
static int ClampEndBlinkSeconds(int sec) {
    if (sec < 1) return 1;
    if (sec > 60) return 60;
    return sec;
}

static bool EndBlinkHighlightOn(ULONGLONG until, ULONGLONG now) {
    if (now >= until) return false;
    return ((until - now) / kEndBlinkHalfPeriodMs) % 2 == 0;  // starts "on", then toggles
}

template <typename Key>
static bool IsEndBlinkOn(const DashboardState* st, const std::unordered_map<Key, ULONGLONG>& blinks, const Key& key) {
    if (!st || !st->endBlinkEnabled || blinks.empty()) return false;
    auto it = blinks.find(key);
    return it != blinks.end() && EndBlinkHighlightOn(it->second, GetTickCount64());
}

static void StartEndBlinkTimer(DashboardState* st) {
    if (st && st->hwndMain) SetTimer(st->hwndMain, TIMER_END_BLINK_ID, kEndBlinkTimerMs, nullptr);
}

static void StartProcEndBlink(DashboardState* st, int procId) {
    if (!st || !st->endBlinkEnabled) return;
    st->procEndBlinkUntil[procId] = GetTickCount64() + static_cast<ULONGLONG>(st->endBlinkSeconds) * 1000;
    StartEndBlinkTimer(st);
}

static void StartTaskEndBlink(DashboardState* st, const std::string& jobId) {
    if (!st || !st->endBlinkEnabled) return;
    st->taskEndBlinkUntil[jobId] = GetTickCount64() + static_cast<ULONGLONG>(st->endBlinkSeconds) * 1000;
    StartEndBlinkTimer(st);
}

// Drops finished blinks and repaints both lists. Returns false when no blink is left.
static bool TickEndBlink(DashboardState* st) {
    const ULONGLONG now = GetTickCount64();
    bool any = false;
    for (auto it = st->procEndBlinkUntil.begin(); it != st->procEndBlinkUntil.end();) {
        if (now >= it->second) {
            it = st->procEndBlinkUntil.erase(it);
        } else {
            any = true;
            ++it;
        }
    }
    for (auto it = st->taskEndBlinkUntil.begin(); it != st->taskEndBlinkUntil.end();) {
        if (now >= it->second) {
            it = st->taskEndBlinkUntil.erase(it);
        } else {
            any = true;
            ++it;
        }
    }
    if (st->hwndList) InvalidateRect(st->hwndList, nullptr, FALSE);
    if (st->hwndTaskList) InvalidateRect(st->hwndTaskList, nullptr, FALSE);
    return any;
}

static void RefreshDashboardData(DashboardState* st);
static void SyncStreamPane(DashboardState* st);
static void EnsureStreamPane(DashboardState* st);
static void SaveShellConfig(const DashboardState* st);
static void EvaluateSupervisorHint(DashboardState* st, bool forceShow);

static std::vector<std::string> SplitJsonObjectArray(const std::string& json, const std::string& key) {
    std::vector<std::string> objs;
    std::string pattern = "\"" + key + "\"";
    size_t keyPos = json.find(pattern);
    if (keyPos == std::string::npos) return objs;
    size_t arr = json.find('[', keyPos);
    if (arr == std::string::npos) return objs;
    int depth = 0;
    bool inStr = false;
    bool esc = false;
    size_t objStart = std::string::npos;
    for (size_t i = arr; i < json.size(); ++i) {
        char ch = json[i];
        if (inStr) {
            if (esc) esc = false;
            else if (ch == '\\') esc = true;
            else if (ch == '"') inStr = false;
            continue;
        }
        if (ch == '"') { inStr = true; continue; }
        if (ch == '{') {
            if (depth == 0) objStart = i;
            depth++;
        } else if (ch == '}') {
            if (depth > 0) depth--;
            if (depth == 0 && objStart != std::string::npos) {
                objs.push_back(json.substr(objStart, i - objStart + 1));
                objStart = std::string::npos;
            }
        } else if (ch == ']' && depth == 0) {
            break;
        }
    }
    return objs;
}

static std::string ExtractJsonArrayText(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t keyPos = json.find(pattern);
    if (keyPos == std::string::npos) return "";
    size_t arr = json.find('[', keyPos);
    if (arr == std::string::npos) return "";
    int depth = 0;
    bool inStr = false;
    bool esc = false;
    for (size_t i = arr; i < json.size(); ++i) {
        char ch = json[i];
        if (inStr) {
            if (esc) esc = false;
            else if (ch == '\\') esc = true;
            else if (ch == '"') inStr = false;
            continue;
        }
        if (ch == '"') { inStr = true; continue; }
        if (ch == '[') depth++;
        else if (ch == ']') { depth--; if (depth == 0) return json.substr(arr, i - arr + 1); }
    }
    return "";
}

static std::wstring TaskQueueDepsDisplay(const std::string& jobJson) {
    std::string raw = ExtractJsonArrayText(jobJson, "depends_on");
    std::wstring out;
    size_t pos = 0;
    while ((pos = raw.find("\"id\":\"", pos)) != std::string::npos) {
        pos += 6;
        size_t endQuote = raw.find('"', pos);
        if (endQuote == std::string::npos) break;
        if (!out.empty()) out += L", ";
        out += Utf8ToWide(raw.substr(pos, endQuote - pos));
        pos = endQuote;
    }
    return out;
}

static std::vector<std::string> JsonStringArrayItems(const std::string& json, const std::string& key) {
    std::vector<std::string> items;
    std::string raw = ExtractJsonArrayText(json, key);
    std::string cur;
    bool inStr = false;
    bool esc = false;
    for (char ch : raw) {
        if (!inStr) {
            if (ch == '"') { inStr = true; cur.clear(); }
        } else if (esc) {
            cur += ch;
            esc = false;
        } else if (ch == '\\') {
            esc = true;
        } else if (ch == '"') {
            inStr = false;
            items.push_back(cur);
        } else {
            cur += ch;
        }
    }
    return items;
}

static std::wstring JoinResourceNames(const std::vector<std::string>& names) {
    std::wstring out;
    for (const auto& n : names) {
        if (!out.empty()) out += L", ";
        out += Utf8ToWide(n);
    }
    return out;
}

// "write: a, b / read: c" for the job's resources object; empty when none are declared.
static std::wstring TaskQueueResourcesDisplay(const std::string& jobJson) {
    size_t keyPos = jobJson.find("\"resources\"");
    if (keyPos == std::string::npos) return L"";
    std::string res = jobJson.substr(keyPos);
    auto writes = JsonStringArrayItems(res, "write");
    auto reads = JsonStringArrayItems(res, "read");
    std::wstring out;
    if (!writes.empty()) out += std::wstring(Tr(L"書込: ", L"Write: ", L"Escritura: ")) + JoinResourceNames(writes);
    if (!reads.empty()) {
        if (!out.empty()) out += L" / ";
        out += std::wstring(Tr(L"読取: ", L"Read: ", L"Lectura: ")) + JoinResourceNames(reads);
    }
    return out;
}

// Short "who added this job" text for the Origin column: the caller's label, else the
// working folder name and the first 8 characters of the session id. outTip gets every detail.
static std::wstring TaskQueueOriginDisplay(const std::string& jobJson, std::wstring& outTip) {
    outTip.clear();
    std::string btId;
    size_t btPos = jobJson.find("\"bt\":{");
    if (btPos != std::string::npos) btId = JsonGetString(jobJson.substr(btPos), "id", "");
    size_t keyPos = jobJson.find("\"origin\":{");
    if (keyPos == std::string::npos) {
        if (btId.empty()) return L"";
        outTip = L"BT: " + Utf8ToWide(btId);
        return outTip;
    }
    // origin is the last field of a job object, so its keys are the first ones after it.
    std::string o = jobJson.substr(keyPos);
    std::string label = JsonGetString(o, "label", "");
    std::string client = JsonGetString(o, "client", "");
    std::string session = JsonGetString(o, "session", "");
    std::string hostSession = JsonGetString(o, "host_session", "");
    std::string cwd = JsonGetString(o, "cwd", "");
    int64_t pid = JsonGetInt(o, "pid", 0);

    std::string folder = cwd;
    while (!folder.empty() && (folder.back() == '\\' || folder.back() == '/')) folder.pop_back();
    size_t slash = folder.find_last_of("\\/");
    if (slash != std::string::npos) folder = folder.substr(slash + 1);
    std::string shortSession = session.substr(0, 8);

    std::wstring text;
    if (!label.empty()) {
        text = Utf8ToWide(label);
        if (!folder.empty()) text += L" (" + Utf8ToWide(folder) + L")";
    } else {
        text = Utf8ToWide(folder);
        if (!shortSession.empty()) text += (text.empty() ? L"" : L" · ") + Utf8ToWide(shortSession);
        else if (text.empty() && pid > 0) text = L"pid " + std::to_wstring(pid);
        else if (text.empty()) text = Utf8ToWide(client);
    }
    if (!btId.empty()) text += L" [BT " + Utf8ToWide(btId) + L"]";

    auto addLine = [&outTip](const wchar_t* name, const std::string& value) {
        if (value.empty()) return;
        if (!outTip.empty()) outTip += L"\n";
        outTip += name;
        outTip += Utf8ToWide(value);
    };
    addLine(Tr(L"ラベル: ", L"Label: ", L"Etiqueta: "), label);
    addLine(Tr(L"セッション: ", L"Session: ", L"Sesión: "), session);
    addLine(Tr(L"ホストセッション: ", L"Host session: ", L"Sesión del host: "), hostSession);
    addLine(Tr(L"フォルダ: ", L"Folder: ", L"Carpeta: "), cwd);
    addLine(Tr(L"クライアント: ", L"Client: ", L"Cliente: "), client);
    if (pid > 0) addLine(L"PID: ", std::to_string(pid));
    addLine(L"BT: ", btId);
    return text;
}

static std::string LastAttemptReason(const std::string& jobJson) {
    auto attempts = SplitJsonObjectArray(jobJson, "attempts");
    if (attempts.empty()) return "";
    return JsonGetString(attempts.back(), "reason", "");
}

static std::string LastAttemptField(const std::string& jobJson, const std::string& key) {
    auto attempts = SplitJsonObjectArray(jobJson, "attempts");
    if (attempts.empty()) return "";
    return JsonGetString(attempts.back(), key, "");
}

// The diagnostic attached to the last attempt (doc/design-v3.md 4.3). Empty when the
// attempt succeeded or predates the classifier.
static nlohmann::json LastFailureOfJob(const std::string& jobJson) {
    auto attempts = SplitJsonObjectArray(jobJson, "attempts");
    if (attempts.empty()) return nlohmann::json();
    nlohmann::json attempt = nlohmann::json::parse(attempts.back(), nullptr, false);
    if (!attempt.is_object()) return nlohmann::json();
    auto failure = attempt.find("failure");
    if (failure == attempt.end() || !failure->is_object() || failure->empty()) return nlohmann::json();
    return *failure;
}

static std::string LastFailureClass(const std::string& jobJson) {
    nlohmann::json failure = LastFailureOfJob(jobJson);
    return failure.is_object() ? failure.value("class", std::string()) : std::string();
}

// "exit_nonzero ×2, timeout ×1" for the task list caption.
static std::wstring FailureClassSummary(const std::map<std::string, int>& counts) {
    if (counts.empty()) return L"";
    std::wstringstream ss;
    bool first = true;
    for (const auto& kv : counts) {
        if (!first) ss << L", ";
        first = false;
        ss << Utf8ToWide(kv.first) << L" \u00D7" << kv.second;
    }
    return ss.str();
}

// Failure classes across the jobs of one queue (the task list caption shows the same
// breakdown for whatever is currently visible). Empty when the queue has no records.
static std::wstring QueueFailureBreakdown(DashboardState* dash, const std::string& queue) {
    if (!dash) return L"";
    nlohmann::json req = {{"action", "taskqueue_diagnose"}, {"taskqueue", queue}, {"breakdown", true}};
    nlohmann::json resp;
    if (GuiIpcCall(dash, req, resp) && resp.value("ok", false)) {
        std::string summary = resp.value("summary", "");
        if (!summary.empty()) {
            return Tr(L"キュー全体の失敗分類: ", L"Failure classes in this queue: ",
                      L"Clases de fallo en esta cola: ") + Utf8ToWide(summary);
        }
    }
    if (dash->tqm) {
        std::string err;
        std::string listResp = dash->tqm->TaskQueueList(queue, "failed", 0, err);
        nlohmann::json root = nlohmann::json::parse(listResp, nullptr, false);
        if (!root.is_discarded() && root.is_object()) {
            auto jobs = root.find("jobs");
            if (jobs != root.end() && jobs->is_array()) {
                std::map<std::string, int> counts;
                for (const auto& job : *jobs) {
                    if (!job.is_object()) continue;
                    auto attempts = job.find("attempts");
                    if (attempts == job.end() || !attempts->is_array() || attempts->empty()) continue;
                    const nlohmann::json& last = attempts->back();
                    if (!last.is_object()) continue;
                    auto failure = last.find("failure");
                    if (failure == last.end() || !failure->is_object()) continue;
                    std::string cls = failure->value("class", std::string());
                    if (!cls.empty()) ++counts[cls];
                }
                std::wstring summary = FailureClassSummary(counts);
                if (!summary.empty()) {
                    return Tr(L"キュー全体の失敗分類: ", L"Failure classes in this queue: ",
                              L"Clases de fallo en esta cola: ") + summary;
                }
            }
        }
    }
    return L"";
}


// The human answer to "why did it fail", used by the task and process detail windows.
static std::wstring WhyFailureText(const nlohmann::json& failure) {
    if (!failure.is_object() || failure.empty()) {
        return Tr(L"この試行の診断はありません。", L"No diagnosis was recorded for this attempt.",
                  L"No hay diagnóstico para este intento.");
    }
    std::wstringstream ss;
    std::string failureClass = failure.value("class", std::string("unknown"));
    std::string confidence = failure.value("confidence", std::string());
    ss << Tr(L"分類: ", L"Class: ", L"Clase: ") << Utf8ToWide(failureClass);
    if (!confidence.empty()) ss << L" (" << Utf8ToWide(confidence) << L")";
    ss << L"\r\n";
    std::string summary = failure.value("summary", std::string());
    if (!summary.empty()) ss << Tr(L"理由: ", L"Why: ", L"Motivo: ") << Utf8ToWide(summary) << L"\r\n";
    std::string action = failure.value("suggestedAction", std::string());
    std::string retryAt = failure.value("retryAt", std::string());
    if (!action.empty()) {
        ss << Tr(L"次の手: ", L"Next: ", L"Siguiente: ") << Utf8ToWide(action);
        if (!retryAt.empty()) ss << L" (" << Tr(L"再試行 ", L"retry at ", L"reintentar ") << Utf8ToWide(retryAt) << L")";
        ss << L"\r\n";
    }
    auto activity = failure.find("lastActivity");
    if (activity != failure.end() && activity->is_object()) {
        std::string what = activity->value("activity", std::string());
        std::string target = activity->value("target", std::string());
        if (!what.empty() || !target.empty()) {
            ss << Tr(L"最後の動作: ", L"Last activity: ", L"Última actividad: ") << Utf8ToWide(what);
            if (!target.empty()) ss << L" " << Utf8ToWide(target);
            if (activity->contains("sinceSec") && (*activity)["sinceSec"].is_number()) {
                ss << L" (" << (*activity)["sinceSec"].get<int64_t>() << L"s)";
            }
            ss << L"\r\n";
        }
    }
    auto evidence = failure.find("evidence");
    if (evidence != failure.end() && evidence->is_array() && !evidence->empty()) {
        ss << Tr(L"証拠 (選ぶと元の行を表示):", L"Evidence (select to view the source line):",
                 L"Evidencia (seleccione para ver la línea):") << L"\r\n";
        for (const auto& item : *evidence) {
            if (!item.is_object()) continue;
            if (item.contains("events") && item["events"].is_string()) {
                ss << L"  event " << Utf8ToWide(item["events"].get<std::string>())
                   << L" seq " << item.value("seq", static_cast<uint64_t>(0)) << L"\r\n";
            } else if (item.contains("log") && item["log"].is_string()) {
                ss << L"  log   " << Utf8ToWide(item["log"].get<std::string>())
                   << L" line " << item.value("line", 0) << L"\r\n";
            }
        }
    }
    auto llmGuess = failure.find("llmGuess");
    if (llmGuess != failure.end() && llmGuess->is_object()) {
        std::string guessClass = llmGuess->value("class", std::string());
        std::string guessSummary = llmGuess->value("summary", std::string());
        std::string guessAction = llmGuess->value("suggestedAction", std::string());
        std::string rationale = llmGuess->value("rationale", std::string());
        if (!guessClass.empty()) {
            ss << Tr(L"LLM の見立て (参考・低信頼): ", L"LLM guess (reference, low confidence): ",
                     L"Conjetura del LLM (referencia, baja confianza): ")
               << Utf8ToWide(guessClass);
            if (!guessSummary.empty()) ss << L" - " << Utf8ToWide(guessSummary);
            ss << L"\r\n";
            if (!guessAction.empty()) ss << L"  " << Utf8ToWide(guessAction) << L"\r\n";
            if (!rationale.empty()) ss << L"  " << Utf8ToWide(rationale) << L"\r\n";
        }
    }
    return ss.str();
}

static std::vector<std::string> ReadTextLines(const std::wstring& pathW) {
    std::vector<std::string> lines;
    std::string text = ReadFileUtf8Wide(pathW);
    if (text.empty()) return lines;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = nl == std::string::npos ? text.substr(pos) : text.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(std::move(line));
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return lines;
}

// Resolve an evidence file against the job cwd when it is relative.
static std::wstring EvidenceFilePathWide(const nlohmann::json& job, const std::string& file) {
    if (file.empty()) return L"";
    if (file.size() > 1 && (file[1] == ':' || file[0] == '\\' || file[0] == '/')) {
        return Utf8ToWide(file);
    }
    std::string cwd = job.is_object() ? job.value("cwd", std::string()) : std::string();
    if (!cwd.empty()) return Utf8ToWide(cwd + "\\" + file);
    return Utf8ToWide(file);
}

// 1-based line of the event with this seq in a .events.jsonl (0 when not found).
static int FindEventLineBySeq(const std::vector<std::string>& lines, uint64_t seq) {
    for (size_t i = 0; i < lines.size(); ++i) {
        nlohmann::json j = nlohmann::json::parse(lines[i], nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;
        auto value = j.find("seq");
        if (value == j.end() || !value->is_number_integer()) continue;
        if (value->get<uint64_t>() == seq) return static_cast<int>(i) + 1;
    }
    return 0;
}

static std::wstring EvidenceContextText(const nlohmann::json& job, const nlohmann::json& evidence) {
    std::string file;
    int line = 0;
    if (evidence.contains("events") && evidence["events"].is_string()) {
        file = evidence["events"].get<std::string>();
        uint64_t seq = evidence.value("seq", static_cast<uint64_t>(0));
        std::wstring pathW = EvidenceFilePathWide(job, file);
        std::vector<std::string> lines = ReadTextLines(pathW);
        if (lines.empty()) {
            return Tr(L"証拠のイベントファイルを読めません: ", L"Cannot read the evidence event file: ",
                      L"No se puede leer el archivo de eventos: ") + Utf8ToWide(file);
        }
        line = FindEventLineBySeq(lines, seq);
        if (line == 0) {
            return Tr(L"イベント seq ", L"Event seq ", L"Secuencia ") + std::to_wstring(seq) +
                   Tr(L" はこのファイルに見つかりません: ", L" was not found in this file: ",
                      L" no se encontró en este archivo: ") + Utf8ToWide(file);
        }
        std::wstringstream ss;
        ss << Utf8ToWide(file) << L"  seq " << seq << L"\r\n\r\n";
        int from = (std::max)(1, line - 8);
        int to = (std::min)(static_cast<int>(lines.size()), line + 8);
        for (int n = from; n <= to; ++n) {
            ss << (n == line ? L">> " : L"   ") << n << L": " << Utf8ToWide(lines[static_cast<size_t>(n - 1)]) << L"\r\n";
        }
        return ss.str();
    }
    if (evidence.contains("log") && evidence["log"].is_string()) {
        file = evidence["log"].get<std::string>();
        line = evidence.value("line", 0);
        std::vector<std::string> lines = ReadTextLines(EvidenceFilePathWide(job, file));
        if (lines.empty()) {
            return Tr(L"証拠のログを読めません: ", L"Cannot read the evidence log: ",
                      L"No se puede leer el registro: ") + Utf8ToWide(file);
        }
        if (line < 1 || line > static_cast<int>(lines.size())) {
            return Tr(L"ログ行 ", L"Log line ", L"Línea ") + std::to_wstring(line) +
                   Tr(L" はこのファイルにありません: ", L" is not in this file: ",
                      L" no está en este archivo: ") + Utf8ToWide(file);
        }
        std::wstringstream ss;
        ss << Utf8ToWide(file) << L"  line " << line << L"\r\n\r\n";
        int from = (std::max)(1, line - 8);
        int to = (std::min)(static_cast<int>(lines.size()), line + 8);
        for (int n = from; n <= to; ++n) {
            ss << (n == line ? L">> " : L"   ") << n << L": " << Utf8ToWide(lines[static_cast<size_t>(n - 1)]) << L"\r\n";
        }
        return ss.str();
    }
    return Tr(L"証拠の場所が記録されていません。", L"This evidence has no source location.",
              L"Esta evidencia no tiene ubicación.");
}


static std::string TimeOfDayText(const std::string& ts) {
    size_t space = ts.find(' ');
    if (space != std::string::npos && ts.size() >= space + 9) return ts.substr(space + 1);
    return ts;
}

static std::string AddSecondsToTimestamp(const std::string& ts, int seconds) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    if (sscanf_s(ts.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &s) != 6) return "";
    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = s;
    t.tm_isdst = -1;
    time_t tt = mktime(&t);
    if (tt == static_cast<time_t>(-1)) return "";
    tt += seconds;
    struct tm out = {};
    localtime_s(&out, &tt);
    char buf[32] = {};
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
             out.tm_year + 1900, out.tm_mon + 1, out.tm_mday,
             out.tm_hour, out.tm_min, out.tm_sec);
    return buf;
}

static std::vector<std::pair<std::string, std::string>> TaskQueueDepList(const std::string& jobJson) {
    std::vector<std::pair<std::string, std::string>> deps;
    for (const auto& d : SplitJsonObjectArray(jobJson, "depends_on")) {
        std::string id = JsonGetString(d, "id", "");
        std::string on = JsonGetString(d, "on", "success");
        if (!id.empty()) deps.emplace_back(id, on);
    }
    return deps;
}

static std::wstring TaskQueueStateLabel(const std::string& state, const std::string& jobJson,
                                        const std::unordered_map<std::string, std::string>& stateById) {
    if (state != "pending") return Utf8ToWide(state);
    bool waitingForDeps = false;
    for (const auto& dep : TaskQueueDepList(jobJson)) {
        auto it = stateById.find(dep.first);
        std::string depState = (it == stateById.end()) ? "" : it->second;
        bool satisfied = (dep.second == "done")
            ? (depState == "succeeded" || depState == "failed" || depState == "cancelled")
            : (depState == "succeeded");
        if (!satisfied) {
            waitingForDeps = true;
            break;
        }
    }
    // waiting: queued and ready to run (waiting for a free slot).
    // pending: not in the run queue (dependencies not satisfied yet).
    return waitingForDeps ? std::wstring(L"pending") : std::wstring(L"waiting");
}

// ---------------------------------------------------------------------------
// Memory dialogs (doc/design-v3.md 3.11, 3.12).
//
// "Past runs" is the second tab of a task queue job's detail window. It lists the
// L1 RunRecords for the same job name, the failure-class trend and related notes.
// The memory list lets a human retract an L2 note or promote it to L3 knowledge.
// ---------------------------------------------------------------------------

static std::wstring MemoryDataFilePathWide(const wchar_t* name) {
    return GetEffectiveDataDirectoryWide() + L"\\memory\\" + name;
}

static void EnsureMemoryDirectory() {
    std::wstring dir = GetEffectiveDataDirectoryWide() + L"\\memory";
    CreateDirectoryW(dir.c_str(), nullptr);
}

static void AppendMemoryLine(const wchar_t* name, const std::string& line) {
    EnsureMemoryDirectory();
    AppendToLogFileWide(MemoryDataFilePathWide(name), line + "\r\n");
}

static bool RetractMemoryNoteById(DashboardState* dash, const std::string& id, std::string& outError) {
    if (id.empty()) {
        outError = WideToUtf8(Tr(L"ノートが選択されていません", L"No note is selected", L"No hay una nota seleccionada"));
        return false;
    }
    nlohmann::json req = {
        {"action", "memory_retract"},
        {"id", id},
        {"reason", "retracted from gui"},
        {"retracted_by", "gui"}
    };
    nlohmann::json resp;
    if (GuiIpcCall(dash, req, resp, &outError) && resp.value("ok", false)) {
        return true;
    }
    nlohmann::json j;
    j["op"] = "retract";
    j["id"] = id;
    j["retractedAt"] = GetCurrentTimestampStr();
    j["retractedBy"] = "gui";
    AppendMemoryLine(L"notes.jsonl", j.dump());
    return true;
}

static bool PromoteMemoryNoteToL3(DashboardState* dash, const std::string& id, std::string& outError) {
    if (id.empty()) {
        outError = "note id is empty";
        return false;
    }
    nlohmann::json req = {
        {"action", "memory_promote"},
        {"id", id},
        {"approved_by", "gui:human"}
    };
    nlohmann::json resp;
    if (GuiIpcCall(dash, req, resp, &outError) && resp.value("ok", false)) {
        return true;
    }
    std::string respGet = MemoryGetIpc(dash, id, "note");
    nlohmann::json got = nlohmann::json::parse(respGet, nullptr, false);
    if (got.is_discarded() || !got.is_object() || !got.value("ok", false)) {
        outError = "note not found: " + id;
        return false;
    }
    nlohmann::json doc = nlohmann::json::object();
    auto d = got.find("document");
    if (d != got.end() && d->is_object()) doc = *d;
    if (doc.contains("note") && doc["note"].is_object()) doc = doc["note"];
    if (doc.value("text", std::string()).empty()) {
        outError = "note has no text: " + id;
        return false;
    }
    std::string status = doc.value("status", got.value("status", std::string("active")));
    if (status == "retracted") {
        outError = WideToUtf8(Tr(L"取り消されたノートは昇格できません",
                      L"A retracted note cannot be promoted",
                      L"Una nota retirada no se puede promover"));
        return false;
    }
    static int seq = 0;
    ++seq;
    nlohmann::json kn = doc;
    kn["schema"] = "hancho.memory.knowledge/1";
    kn["id"] = "kn_" + GetFileTimestampStr() + "_" + std::to_string(seq);
    kn["status"] = "active";
    kn["approvedBy"] = "gui:human";
    kn["approvedAt"] = GetCurrentTimestampStr();
    if (!kn.contains("createdAt") || !kn["createdAt"].is_string()) kn["createdAt"] = kn["approvedAt"];
    AppendMemoryLine(L"knowledge.jsonl", kn.dump());

    // The note is retired once the human-approved copy lives in knowledge.jsonl.
    nlohmann::json old = doc;
    old["status"] = "superseded";
    nlohmann::json upd;
    upd["op"] = "update";
    upd["note"] = old;
    AppendMemoryLine(L"notes.jsonl", upd.dump());
    return true;
}

struct PastRunsDlgState {
    DashboardState* dash = nullptr;
    HFONT fontUi = nullptr;
    HWND tab = nullptr;
    HWND jobEdit = nullptr;
    HWND summaryEdit = nullptr;
    HWND list = nullptr;
    HWND detailEdit = nullptr;
    HWND whyEdit = nullptr;
    HWND whyList = nullptr;
    HWND whyDetail = nullptr;
    HWND btnClose = nullptr;
    std::string jobName;
    std::string taskqueue;
    std::string jobJson;
    std::vector<nlohmann::json> runs;
    nlohmann::json job;
    nlohmann::json failure;
    std::vector<nlohmann::json> evidence;
    int activeTab = 0;
};

static void PastRunsLayout(HWND hwnd, PastRunsDlgState* ds) {
    if (!ds) return;
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int pad = 12;
    if (ds->tab) {
        MoveWindow(ds->tab, pad, pad, (std::max)(200, w - pad * 2), (std::max)(120, h - pad * 2), TRUE);
    }
    RECT page = {pad, pad, w - pad, h - pad};
    if (ds->tab) TabCtrl_AdjustRect(ds->tab, FALSE, &page);
    int pw = (std::max)(100, static_cast<int>(page.right - page.left));
    int closeY = (std::max)(static_cast<int>(page.top) + 40, h - pad - 30);
    int contentH = (std::max)(80, closeY - static_cast<int>(page.top) - 8);
    if (ds->activeTab == 0) {
        if (ds->jobEdit) MoveWindow(ds->jobEdit, page.left, page.top, pw, contentH, TRUE);
    } else if (ds->activeTab == 1) {
        int sumH = (std::min)(120, (std::max)(60, contentH / 4));
        if (ds->summaryEdit) MoveWindow(ds->summaryEdit, page.left, page.top, pw, sumH, TRUE);
        int listY = page.top + sumH + 6;
        int listH = (std::max)(70, contentH - sumH - 6 - 90);
        if (ds->list) MoveWindow(ds->list, page.left, listY, pw, listH, TRUE);
        int detailY = listY + listH + 6;
        int detailH = (std::max)(40, static_cast<int>(page.top) + contentH - detailY);
        if (ds->detailEdit) MoveWindow(ds->detailEdit, page.left, detailY, pw, detailH, TRUE);
    } else {
        int whyH = (std::min)(150, (std::max)(80, contentH / 3));
        if (ds->whyEdit) MoveWindow(ds->whyEdit, page.left, page.top, pw, whyH, TRUE);
        int listY = page.top + whyH + 6;
        int listH = (std::max)(60, (contentH - whyH - 12) / 2);
        if (ds->whyList) MoveWindow(ds->whyList, page.left, listY, pw, listH, TRUE);
        int detailY = listY + listH + 6;
        int detailH = (std::max)(40, static_cast<int>(page.top) + contentH - detailY);
        if (ds->whyDetail) MoveWindow(ds->whyDetail, page.left, detailY, pw, detailH, TRUE);
    }
    if (ds->btnClose) MoveWindow(ds->btnClose, (std::max)(pad, w - pad - 100), closeY, 100, 28, TRUE);
}

static void PastRunsSelectTab(HWND hwnd, PastRunsDlgState* ds, int tab) {
    if (!ds) return;
    ds->activeTab = tab;
    if (ds->tab) TabCtrl_SetCurSel(ds->tab, tab);
    if (ds->jobEdit) ShowWindow(ds->jobEdit, tab == 0 ? SW_SHOW : SW_HIDE);
    if (ds->summaryEdit) ShowWindow(ds->summaryEdit, tab == 1 ? SW_SHOW : SW_HIDE);
    if (ds->list) ShowWindow(ds->list, tab == 1 ? SW_SHOW : SW_HIDE);
    if (ds->detailEdit) ShowWindow(ds->detailEdit, tab == 1 ? SW_SHOW : SW_HIDE);
    if (ds->whyEdit) ShowWindow(ds->whyEdit, tab == 2 ? SW_SHOW : SW_HIDE);
    if (ds->whyList) ShowWindow(ds->whyList, tab == 2 ? SW_SHOW : SW_HIDE);
    if (ds->whyDetail) ShowWindow(ds->whyDetail, tab == 2 ? SW_SHOW : SW_HIDE);
    PastRunsLayout(hwnd, ds);
}

static void PastRunsAddColumn(HWND list, int index, const wchar_t* title, int width) {
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = const_cast<wchar_t*>(title);
    col.cx = width;
    ListView_InsertColumn(list, index, &col);
}

static void PastRunsLoad(PastRunsDlgState* ds) {
    if (!ds || !ds->list) return;
    memory::Query q;
    q.jobName = ds->jobName;
    q.taskqueue = ds->taskqueue;
    q.kinds.push_back("run");
    q.limit = 200;
    std::string resp = MemorySearchIpc(ds->dash, q);
    nlohmann::json j = nlohmann::json::parse(resp, nullptr, false);
    ds->runs.clear();
    if (j.is_object()) {
        auto results = j.find("results");
        if (results != j.end() && results->is_array()) {
            for (const auto& hit : *results) {
                if (hit.is_object() && hit.value("kind", std::string()) == "run") ds->runs.push_back(hit);
            }
        }
    }
    std::sort(ds->runs.begin(), ds->runs.end(), [](const nlohmann::json& a, const nlohmann::json& b) {
        return a.value("createdAt", std::string()) > b.value("createdAt", std::string());
    });

    SendMessageW(ds->list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(ds->list);
    for (size_t i = 0; i < ds->runs.size(); ++i) {
        const auto& r = ds->runs[i];
        std::wstring timeStr = Utf8ToWide(r.value("createdAt", std::string()));
        std::wstring outcomeStr = Utf8ToWide(r.value("outcome", std::string()));
        std::wstring failureStr = Utf8ToWide(r.value("failureClass", std::string()));
        std::wstring summaryStr = Utf8ToWide(r.value("summary", std::string()));
        LVITEMW item = {};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = timeStr.data();
        item.lParam = static_cast<LPARAM>(i);
        ListView_InsertItem(ds->list, &item);
        ListView_SetItemText(ds->list, static_cast<int>(i), 1, outcomeStr.data());
        ListView_SetItemText(ds->list, static_cast<int>(i), 2, failureStr.data());
        ListView_SetItemText(ds->list, static_cast<int>(i), 3, summaryStr.data());
    }
    SendMessageW(ds->list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(ds->list, nullptr, TRUE);

    memory::Query statsQuery = q;
    std::string statsResp = MemoryStatsIpc(ds->dash, statsQuery);
    nlohmann::json s = nlohmann::json::parse(statsResp, nullptr, false);
    double median = 0.0;
    double p90 = 0.0;
    if (s.is_object()) {
        auto dur = s.find("durationSec");
        if (dur != s.end() && dur->is_object()) {
            median = dur->value("median", 0.0);
            p90 = dur->value("p90", 0.0);
        }
    }

    std::vector<std::string> trend;
    std::map<std::string, int> counts;
    for (auto it = ds->runs.rbegin(); it != ds->runs.rend(); ++it) {
        std::string fc = it->value("failureClass", std::string());
        if (fc.empty()) continue;
        trend.push_back(fc);
        ++counts[fc];
    }

    memory::Query nq;
    nq.kinds.push_back("note");
    nq.jobName = ds->jobName;
    nq.taskqueue = ds->taskqueue;
    nq.limit = 30;
    nlohmann::json notes = nlohmann::json::parse(MemorySearchIpc(ds->dash, nq), nullptr, false);

    std::wstringstream text;
    text << Tr(L"実行数: ", L"Runs: ", L"Ejecuciones: ")
         << (s.is_object() ? s.value("runs", static_cast<int64_t>(0)) : static_cast<int64_t>(0))
         << Tr(L"  成功率: ", L"  success: ", L"  éxito: ")
         << std::fixed << std::setprecision(1)
         << (s.is_object() ? s.value("successRate", 0.0) * 100.0 : 0.0) << L"%"
         << Tr(L"  中央値: ", L"  median: ", L"  mediana: ") << std::setprecision(0) << median << L"s"
         << L"  p90: " << p90 << L"s"
         << Tr(L"  トークン: ", L"  tokens: ", L"  tokens: ")
         << (s.is_object() ? s.value("totalTokens", static_cast<int64_t>(0)) : static_cast<int64_t>(0))
         << L"\r\n";
    text << Tr(L"失敗分類の推移 (古い順): ", L"Failure classes (oldest first): ", L"Clases de fallo (más antiguo primero): ");
    if (trend.empty()) {
        text << Tr(L"(なし)", L"(none)", L"(ninguna)");
    } else {
        for (size_t i = 0; i < trend.size() && i < 20; ++i) {
            if (i) text << L" → ";
            text << Utf8ToWide(trend[i]);
        }
    }
    text << L"\r\n";
    if (!counts.empty()) {
        text << Tr(L"回数: ", L"Counts: ", L"Recuentos: ");
        bool first = true;
        for (const auto& kv : counts) {
            if (!first) text << L", ";
            first = false;
            text << Utf8ToWide(kv.first) << L" ×" << kv.second;
        }
        text << L"\r\n";
    }
    text << Tr(L"関連ノート:", L"Related notes:", L"Notas relacionadas:");
    bool anyNote = false;
    if (notes.is_object()) {
        auto results = notes.find("results");
        if (results != notes.end() && results->is_array()) {
            for (const auto& n : *results) {
                if (!n.is_object()) continue;
                if (n.value("kind", std::string()) != "note") continue;
                text << L"\r\n  - " << Utf8ToWide(n.value("id", std::string())) << L": "
                     << Utf8ToWide(n.value("summary", std::string()));
                anyNote = true;
            }
        }
    }
    if (!anyNote) text << Tr(L" (なし)", L" (none)", L" (ninguna)");

    if (ds->summaryEdit) SetWindowTextW(ds->summaryEdit, text.str().c_str());
    if (ds->detailEdit && ds->runs.empty()) {
        SetWindowTextW(ds->detailEdit,
                       Tr(L"このジョブの実行記録はまだありません。", L"No run records for this job yet.",
                          L"Aún no hay ejecuciones para esta tarea."));
    }
}

// "Why failed" tab: the diagnosis of the last attempt, plus one row per evidence item.
// Selecting a row shows the surrounding event/log lines with the source line marked.
static void WhyLoad(PastRunsDlgState* ds) {
    if (!ds) return;
    ds->job = nlohmann::json();
    ds->failure = nlohmann::json();
    ds->evidence.clear();

    nlohmann::json root = nlohmann::json::parse(ds->jobJson, nullptr, false);
    if (root.is_object()) {
        ds->job = root.contains("job") && root["job"].is_object() ? root["job"] : root;
    }
    if (ds->job.is_object()) {
        auto attempts = ds->job.find("attempts");
        if (attempts != ds->job.end() && attempts->is_array()) {
            for (const auto& attempt : *attempts) {
                if (!attempt.is_object()) continue;
                auto failure = attempt.find("failure");
                if (failure != attempt.end() && failure->is_object() && !failure->empty()) {
                    ds->failure = *failure;
                }
            }
        }
    }
    if (ds->failure.empty() && !ds->jobName.empty() && ds->dash) {
        nlohmann::json req = {{"action", "taskqueue_diagnose"}, {"id", ds->jobName}};
        nlohmann::json resp;
        if (GuiIpcCall(ds->dash, req, resp) && resp.value("ok", false) && resp.contains("failure") && resp["failure"].is_object()) {
            ds->failure = resp["failure"];
        }
    }

    std::wstring text = WhyFailureText(ds->failure);
    std::wstring breakdown = QueueFailureBreakdown(ds->dash, ds->taskqueue);
    if (!breakdown.empty()) text += L"\r\n" + breakdown + L"\r\n";
    std::string jobState = ds->job.is_object() ? ds->job.value("state", std::string()) : std::string();
    if (ds->failure.empty() && jobState != "failed" && jobState != "cancelled") {
        text = Tr(L"このジョブはまだ失敗していません。", L"This job has not failed.",
                  L"Esta tarea aún no ha fallado.");
        if (!breakdown.empty()) text += L"\r\n" + breakdown + L"\r\n";
    }
    if (ds->whyEdit) SetWindowTextW(ds->whyEdit, text.c_str());

    if (ds->whyList) {
        SendMessageW(ds->whyList, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(ds->whyList);
        auto evidence = ds->failure.find("evidence");
        if (evidence != ds->failure.end() && evidence->is_array()) {
            for (const auto& item : *evidence) {
                if (!item.is_object()) continue;
                std::string kind;
                std::string file;
                std::wstring where;
                if (item.contains("events") && item["events"].is_string()) {
                    kind = "event";
                    file = item["events"].get<std::string>();
                    where = L"seq " + std::to_wstring(item.value("seq", static_cast<uint64_t>(0)));
                } else if (item.contains("log") && item["log"].is_string()) {
                    kind = "log";
                    file = item["log"].get<std::string>();
                    where = L"line " + std::to_wstring(item.value("line", 0));
                } else {
                    continue;
                }
                int idx = static_cast<int>(ds->evidence.size());
                ds->evidence.push_back(item);
                std::wstring wKind = Utf8ToWide(kind);
                std::wstring wFile = Utf8ToWide(file);
                LVITEMW lvi = {};
                lvi.mask = LVIF_TEXT | LVIF_PARAM;
                lvi.iItem = idx;
                lvi.pszText = wKind.data();
                lvi.lParam = idx;
                ListView_InsertItem(ds->whyList, &lvi);
                ListView_SetItemText(ds->whyList, idx, 1, wFile.data());
                ListView_SetItemText(ds->whyList, idx, 2, where.data());
            }
        }
        SendMessageW(ds->whyList, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(ds->whyList, nullptr, TRUE);
        if (!ds->evidence.empty()) {
            ListView_SetItemState(ds->whyList, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        }
    }
    if (ds->whyDetail) SetWindowTextW(ds->whyDetail, L"");
}

static void WhyShowEvidence(PastRunsDlgState* ds, int index) {
    if (!ds || !ds->whyDetail) return;
    if (index < 0 || static_cast<size_t>(index) >= ds->evidence.size()) {
        SetWindowTextW(ds->whyDetail, L"");
        return;
    }
    std::wstring text = EvidenceContextText(ds->job, ds->evidence[static_cast<size_t>(index)]);
    SetWindowTextW(ds->whyDetail, NormalizeCrLfWide(WideToUtf8(text)).c_str());
}

static std::string WhyEvidenceOpenPath(PastRunsDlgState* ds, int index) {
    if (!ds || index < 0 || static_cast<size_t>(index) >= ds->evidence.size()) return "";
    const nlohmann::json& item = ds->evidence[static_cast<size_t>(index)];
    std::string file;
    if (item.contains("events") && item["events"].is_string()) file = item["events"].get<std::string>();
    else if (item.contains("log") && item["log"].is_string()) file = item["log"].get<std::string>();
    return file;
}

static LRESULT CALLBACK PastRunsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ds = reinterpret_cast<PastRunsDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ds = reinterpret_cast<PastRunsDlgState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ds));

            ds->tab = CreateWindowExW(0, WC_TABCONTROLW, L"",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                12, 12, 760, 480, hwnd, (HMENU)2700, nullptr, nullptr);
            TCITEMW tie = {};
            tie.mask = TCIF_TEXT;
            tie.pszText = const_cast<wchar_t*>(Tr(L"ジョブ", L"Job", L"Trabajo"));
            TabCtrl_InsertItem(ds->tab, 0, &tie);
            tie.pszText = const_cast<wchar_t*>(Tr(L"過去の実行", L"Past runs", L"Ejecuciones"));
            TabCtrl_InsertItem(ds->tab, 1, &tie);
            tie.pszText = const_cast<wchar_t*>(Tr(L"なぜ失敗したか", L"Why failed", L"Por qué falló"));
            TabCtrl_InsertItem(ds->tab, 2, &tie);

            ds->jobEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", Utf8ToWide(ds->jobJson).c_str(),
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                    ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                20, 60, 700, 360, hwnd, (HMENU)2701, nullptr, nullptr);

            ds->summaryEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                20, 60, 700, 100, hwnd, (HMENU)2702, nullptr, nullptr);

            ds->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                20, 166, 700, 180, hwnd, (HMENU)2703, nullptr, nullptr);
            ListView_SetExtendedListViewStyle(ds->list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            PastRunsAddColumn(ds->list, 0, Tr(L"開始", L"Started", L"Inicio"), 170);
            PastRunsAddColumn(ds->list, 1, Tr(L"結果", L"Outcome", L"Resultado"), 90);
            PastRunsAddColumn(ds->list, 2, Tr(L"失敗分類", L"Failure", L"Fallo"), 170);
            PastRunsAddColumn(ds->list, 3, Tr(L"概要", L"Summary", L"Resumen"), 520);

            ds->detailEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                    ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                20, 352, 700, 120, hwnd, (HMENU)2704, nullptr, nullptr);

            ds->whyEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                20, 60, 700, 100, hwnd, (HMENU)2705, nullptr, nullptr);

            ds->whyList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                20, 166, 700, 90, hwnd, (HMENU)2706, nullptr, nullptr);
            ListView_SetExtendedListViewStyle(ds->whyList, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            PastRunsAddColumn(ds->whyList, 0, Tr(L"種別", L"Kind", L"Tipo"), 70);
            PastRunsAddColumn(ds->whyList, 1, Tr(L"ファイル", L"File", L"Archivo"), 500);
            PastRunsAddColumn(ds->whyList, 2, Tr(L"位置", L"Location", L"Ubicación"), 150);

            ds->whyDetail = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                    ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                20, 262, 700, 180, hwnd, (HMENU)2707, nullptr, nullptr);

            ds->btnClose = CreateWindowExW(0, L"BUTTON", Tr(L"閉じる", L"Close", L"Cerrar"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 700, 500, 100, 28, hwnd, (HMENU)IDOK, nullptr, nullptr);

            HWND controls[] = {ds->tab, ds->jobEdit, ds->summaryEdit, ds->list, ds->detailEdit,
                               ds->whyEdit, ds->whyList, ds->whyDetail, ds->btnClose};
            for (HWND h : controls) {
                if (h && ds->fontUi) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(ds->fontUi), TRUE);
            }

            PastRunsLoad(ds);
            WhyLoad(ds);
            PastRunsSelectTab(hwnd, ds, ds->activeTab == 1 ? 1 : ds->activeTab == 2 ? 2 : 0);
            ApplyNativeThemeToWindow(hwnd);
            return 0;
        }
        case WM_NOTIFY: {
            if (!ds) break;
            auto* nm = reinterpret_cast<NMHDR*>(lParam);
            if (nm && nm->hwndFrom == ds->tab && nm->code == TCN_SELCHANGE) {
                PastRunsSelectTab(hwnd, ds, TabCtrl_GetCurSel(ds->tab));
                return 0;
            }
            if (nm && nm->hwndFrom == ds->list && nm->code == LVN_ITEMCHANGED) {
                int sel = ListView_GetNextItem(ds->list, -1, LVNI_SELECTED);
                if (sel >= 0 && static_cast<size_t>(sel) < ds->runs.size() && ds->detailEdit) {
                    const auto& r = ds->runs[static_cast<size_t>(sel)];
                    std::string id = r.value("id", std::string());
                    std::string full = MemoryGetIpc(ds->dash, id, "run");
                    nlohmann::json g = nlohmann::json::parse(full, nullptr, false);
                    std::string dump = full;
                    if (g.is_object()) {
                        auto doc = g.find("document");
                        if (doc != g.end()) dump = doc->dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
                    }
                    SetWindowTextW(ds->detailEdit, NormalizeCrLfWide(dump).c_str());
                }
                return 0;
            }
            if (nm && nm->hwndFrom == ds->whyList && nm->code == LVN_ITEMCHANGED) {
                int sel = ListView_GetNextItem(ds->whyList, -1, LVNI_SELECTED);
                WhyShowEvidence(ds, sel);
                return 0;
            }
            if (nm && nm->hwndFrom == ds->whyList && nm->code == NM_DBLCLK) {
                int sel = ListView_GetNextItem(ds->whyList, -1, LVNI_SELECTED);
                std::string file = WhyEvidenceOpenPath(ds, sel);
                if (!file.empty()) {
                    std::wstring pathW = EvidenceFilePathWide(ds->job, file);
                    gui_platform::GetGuiPlatform()->OpenFileInEditor(hwnd, pathW);
                }
                return 0;
            }
            break;
        }
        case WM_SIZE:
            PastRunsLayout(hwnd, ds);
            return 0;
        case WM_COMMAND:
            if (LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            break;
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, g_theme.text);
            SetBkColor(hdc, g_theme.panel);
            return reinterpret_cast<LRESULT>(g_dialogBgBrush);
        }
        case WM_ERASEBKGND: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, g_dialogBgBrush);
            return 1;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            delete ds;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowTaskQueueJobDetails(DashboardState* st, const std::string& jobId, int initialTab = 0) {
    if (!st || jobId.empty()) return;
    std::string err;
    std::string resp = TaskQueueGetIpc(st, jobId, true, 0, err);
    nlohmann::json job = nlohmann::json::parse(resp, nullptr, false);
    if (job.is_object() && job.contains("job") && job["job"].is_object()) job = job["job"];

    auto* ds = new PastRunsDlgState();
    ds->dash = st;
    ds->fontUi = st->hFontUi;
    ds->jobJson = resp;
    ds->jobName = job.is_object() ? job.value("name", std::string()) : std::string();
    if (ds->jobName.empty()) ds->jobName = jobId;
    ds->taskqueue = job.is_object() ? job.value("taskqueue", std::string()) : std::string();
    ds->activeTab = initialTab == 1 ? 1 : initialTab == 2 ? 2 : 0;

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = PastRunsWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"HanchoPastRunsWnd";
        RegisterClassExW(&wc);
        registered = true;
    }

    std::wstring title = Tr(L"タスク詳細", L"Task Details", L"Detalles de tarea");
    title += L" — " + Utf8ToWide(ds->jobName) + L" (" + Utf8ToWide(jobId) + L")";
    RECT parent = {};
    GetWindowRect(st->hwndMain, &parent);
    int dlgW = 860;
    int dlgH = 640;
    int x = parent.left + ((parent.right - parent.left) - dlgW) / 2;
    int y = parent.top + ((parent.bottom - parent.top) - dlgH) / 2;
    HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"HanchoPastRunsWnd", title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, x, y, dlgW, dlgH, st->hwndMain, nullptr, hInst, ds);
    if (hDlg) {
        ShowWindow(hDlg, SW_SHOW);
        SetForegroundWindow(hDlg);
    } else {
        delete ds;
    }
}

struct MemoryListDlgState {
    DashboardState* dash = nullptr;
    HFONT fontUi = nullptr;
    HWND search = nullptr;
    HWND layer = nullptr;
    HWND layerInfo = nullptr;
    HWND author = nullptr;
    HWND list = nullptr;
    HWND detail = nullptr;
    HWND btnSearch = nullptr;
    HWND btnRetract = nullptr;
    HWND btnPromote = nullptr;
    HWND btnRefresh = nullptr;
    HWND btnCompare = nullptr;
    HWND btnClose = nullptr;
    std::vector<nlohmann::json> rows;
    std::map<std::string, std::string> noteAuthors;
};

static void MemoryListAddColumn(HWND list, int index, const wchar_t* title, int width) {
    LVCOLUMNW col = {};
    col.mask = LVCF_TEXT | LVCF_WIDTH;
    col.pszText = const_cast<wchar_t*>(title);
    col.cx = width;
    ListView_InsertColumn(list, index, &col);
}

static void LoadMemoryNoteAuthors(MemoryListDlgState* ds) {
    ds->noteAuthors.clear();
    std::string content = ReadFileUtf8Wide(MemoryDataFilePathWide(L"notes.jsonl"));
    size_t start = 0;
    while (start <= content.size()) {
        size_t end = content.find('\n', start);
        std::string line = (end == std::string::npos) ? content.substr(start) : content.substr(start, end - start);
        start = (end == std::string::npos) ? content.size() + 1 : end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;
        nlohmann::json body = j;
        auto note = j.find("note");
        if (note != j.end() && note->is_object()) body = *note;
        std::string id = body.value("id", std::string());
        if (id.empty()) continue;
        auto author = body.find("author");
        std::string type;
        if (author != body.end()) {
            if (author->is_object()) type = author->value("type", std::string());
            else if (author->is_string()) type = author->get<std::string>();
        }
        if (!type.empty()) ds->noteAuthors[id] = type;
    }

    std::string knContent = ReadFileUtf8Wide(MemoryDataFilePathWide(L"knowledge.jsonl"));
    start = 0;
    while (start <= knContent.size()) {
        size_t end = knContent.find('\n', start);
        std::string line = (end == std::string::npos) ? knContent.substr(start) : knContent.substr(start, end - start);
        start = (end == std::string::npos) ? knContent.size() + 1 : end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        nlohmann::json j = nlohmann::json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.is_object()) continue;
        std::string id = j.value("id", std::string());
        if (id.empty()) continue;
        std::string approvedBy = j.value("approvedBy", std::string());
        if (!approvedBy.empty()) {
            ds->noteAuthors[id] = approvedBy;
        }
    }
}

// Memory layers as the GUI names them. Index 0 of the layer combo is "all"; 1..3 follow this table.
struct MemoryLayerDef {
    const char* kind;      // kind in memory_search ("run", "note", "knowledge")
    const wchar_t* label;  // same in every language
};
static const MemoryLayerDef kMemoryLayers[] = {
    {"run", L"L1 record"},
    {"note", L"L2 note"},
    {"knowledge", L"L3 knowledge"},
};

static std::wstring MemoryLayerLabel(const std::string& kind) {
    for (const auto& l : kMemoryLayers) {
        if (kind == l.kind) return l.label;
    }
    return Utf8ToWide(kind);
}

static const wchar_t* MemoryLayerDescription(int layerSel) {
    switch (layerSel) {
        case 1:
            return Tr(L"L1 record: ジョブの試行 1 回ごとに Hancho が自動で残す記録（結果、所要時間、失敗の種類、トークン、編集したファイル）。memory/records/*.jsonl",
                      L"L1 record: one record per job attempt, written by Hancho (outcome, duration, failure class, tokens, files edited). memory/records/*.jsonl",
                      L"L1 record: un registro por intento de trabajo, escrito por Hancho (resultado, duración, tipo de fallo, tokens, archivos editados). memory/records/*.jsonl");
        case 2:
            return Tr(L"L2 note: エージェントや人が書く短い事実。スコープ付きで、取り消しや置き換えができる。memory/notes.jsonl",
                      L"L2 note: a short fact written by an agent or a person, with a scope. It can be retracted or superseded. memory/notes.jsonl",
                      L"L2 note: un hecho breve escrito por un agente o una persona, con ámbito. Se puede retirar o reemplazar. memory/notes.jsonl");
        case 3:
            return Tr(L"L3 knowledge: 人が承認して L2 note から昇格させたプロジェクト知識。brief では最後まで残る。memory/knowledge.jsonl",
                      L"L3 knowledge: project knowledge a person approved by promoting an L2 note. The brief keeps it to the end. memory/knowledge.jsonl",
                      L"L3 knowledge: conocimiento del proyecto aprobado por una persona al promover una L2 note. El brief lo conserva hasta el final. memory/knowledge.jsonl");
        default:
            return Tr(L"L1 record = Hancho が残す実行ごとの記録 / L2 note = エージェントや人が書く短い事実 / L3 knowledge = 人が承認した L2 note",
                      L"L1 record = per-run record written by Hancho / L2 note = short fact from an agent or a person / L3 knowledge = an L2 note a person approved",
                      L"L1 record = registro por ejecución de Hancho / L2 note = hecho breve de un agente o persona / L3 knowledge = L2 note aprobada por una persona");
    }
}

// Retract and promote only apply to L2 notes.
static void MemoryListUpdateButtons(MemoryListDlgState* ds) {
    if (!ds || !ds->list) return;
    int sel = ListView_GetNextItem(ds->list, -1, LVNI_SELECTED);
    bool isNote = sel >= 0 && static_cast<size_t>(sel) < ds->rows.size() &&
                  ds->rows[static_cast<size_t>(sel)].value("kind", std::string()) == "note";
    if (ds->btnRetract) EnableWindow(ds->btnRetract, isNote);
    if (ds->btnPromote) EnableWindow(ds->btnPromote, isNote);
}

static void MemoryListReload(MemoryListDlgState* ds) {
    if (!ds || !ds->list) return;
    LoadMemoryNoteAuthors(ds);

    std::wstring searchText;
    if (ds->search) {
        wchar_t buf[1024] = {};
        GetWindowTextW(ds->search, buf, 1024);
        searchText = buf;
    }
    int authorFilter = ds->author ? ComboBox_GetCurSel(ds->author) : 0;
    int layerSel = ds->layer ? ComboBox_GetCurSel(ds->layer) : 0;
    if (layerSel < 0 || layerSel > 3) layerSel = 0;

    // One search per layer gives the count shown in the layer combo; the selected layer's
    // result is reused for the list. "All" ranks every layer together in its own search.
    memory::Query q;
    q.text = WideToUtf8(searchText);
    q.limit = 200;
    nlohmann::json resp;
    int64_t layerCounts[3] = {0, 0, 0};
    for (int i = 0; i < 3; ++i) {
        memory::Query lq = q;
        lq.kinds = {kMemoryLayers[i].kind};
        nlohmann::json lr = nlohmann::json::parse(MemorySearchIpc(ds->dash, lq), nullptr, false);
        if (lr.is_object()) layerCounts[i] = lr.value("totalMatched", static_cast<int64_t>(0));
        if (layerSel == i + 1) resp = lr;
    }
    if (layerSel == 0) resp = nlohmann::json::parse(MemorySearchIpc(ds->dash, q), nullptr, false);

    if (ds->layer) {
        ComboBox_ResetContent(ds->layer);
        std::wstringstream all;
        all << Tr(L"すべてのレイヤー", L"All layers", L"Todas las capas") << L" ("
            << (layerCounts[0] + layerCounts[1] + layerCounts[2]) << L")";
        ComboBox_AddString(ds->layer, all.str().c_str());
        for (int i = 0; i < 3; ++i) {
            std::wstringstream s;
            s << kMemoryLayers[i].label << L" (" << layerCounts[i] << L")";
            ComboBox_AddString(ds->layer, s.str().c_str());
        }
        ComboBox_SetCurSel(ds->layer, layerSel);
    }
    if (ds->layerInfo) {
        auto mStats = GetMemoryStorageStatsIpc(ds->dash);
        std::wstringstream info;
        info << MemoryLayerDescription(layerSel) << L"\r\n"
             << Tr(L"容量: ", L"Storage: ", L"Almacenamiento: ") << Utf8ToWide(FormatBytes(mStats.totalBytes));
        if (mStats.limitBytes > 0) {
            info << L" / " << Utf8ToWide(FormatBytes(mStats.limitBytes));
            if (mStats.exceeded) {
                info << L" " << Tr(L"[警告: 上限超過! L2/L3は保護されます]",
                                   L"[Warning: limit exceeded! L2/L3 preserved]",
                                   L"[Aviso: ¡límite excedido! L2/L3 protegidos]");
            } else if (mStats.nearLimit) {
                info << L" " << Tr(L"[警告: 上限近接 (>=80%)]",
                                   L"[Warning: near limit (>=80%)]",
                                   L"[Aviso: cerca del límite (>=80%)]");
            }
        }
        SetWindowTextW(ds->layerInfo, info.str().c_str());
    }
    ds->rows.clear();

    SendMessageW(ds->list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(ds->list);
    if (resp.is_object()) {
        auto results = resp.find("results");
        if (results != resp.end() && results->is_array()) {
            for (const auto& hit : *results) {
                if (!hit.is_object()) continue;
                std::string kind = hit.value("kind", std::string());
                std::string id = hit.value("id", std::string());
                std::string authorType;
                if (kind == "note") {
                    auto it = ds->noteAuthors.find(id);
                    authorType = it == ds->noteAuthors.end() ? hit.value("author", std::string("agent")) : it->second;
                } else if (kind == "knowledge") {
                    auto it = ds->noteAuthors.find(id);
                    if (it != ds->noteAuthors.end() && !it->second.empty()) {
                        authorType = it->second;
                    } else {
                        authorType = hit.value("approvedBy", hit.value("author", std::string("human")));
                    }
                } else {
                    authorType = "hancho";
                }
                bool isWorker = (authorType.rfind("worker:", 0) == 0);
                if (authorFilter == 1 && authorType != "agent" && !isWorker) continue;
                if (authorFilter == 2 && (authorType == "agent" || authorType == "hancho" || isWorker)) continue;
                if (authorFilter == 3 && authorType != "hancho") continue;

                nlohmann::json row = hit;
                row["_author"] = authorType;
                int idx = static_cast<int>(ds->rows.size());
                ds->rows.push_back(row);

                std::wstring wId = Utf8ToWide(id);
                std::wstring wKind = MemoryLayerLabel(kind);
                std::wstring wAuthor = Utf8ToWide(authorType);
                std::string status = hit.value("status", std::string());
                if (status.empty()) status = hit.value("outcome", std::string());
                std::wstring wStatus = Utf8ToWide(status);
                std::string scope = hit.value("scope", std::string());
                if (scope.empty()) {
                    std::string job = hit.value("jobName", std::string());
                    if (!job.empty()) scope = "job:" + job;
                }
                std::wstring wScope = Utf8ToWide(scope);
                std::wstring wText = Utf8ToWide(hit.value("summary", std::string()));
                LVITEMW item = {};
                item.mask = LVIF_TEXT | LVIF_PARAM;
                item.iItem = idx;
                item.pszText = wId.data();
                item.lParam = idx;
                ListView_InsertItem(ds->list, &item);
                ListView_SetItemText(ds->list, idx, 1, wKind.data());
                ListView_SetItemText(ds->list, idx, 2, wAuthor.data());
                ListView_SetItemText(ds->list, idx, 3, wStatus.data());
                ListView_SetItemText(ds->list, idx, 4, wScope.data());
                ListView_SetItemText(ds->list, idx, 5, wText.data());
            }
        }
    }
    SendMessageW(ds->list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(ds->list, nullptr, TRUE);
    if (ds->detail) SetWindowTextW(ds->detail, L"");
    MemoryListUpdateButtons(ds);
}

static void MemoryListLayout(HWND hwnd, MemoryListDlgState* ds) {
    if (!ds) return;
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    int pad = 12;
    int topY = pad;
    int searchW = (std::max)(120, w - pad * 2 - 86 - 186 - 146);
    if (ds->search) MoveWindow(ds->search, pad, topY, searchW, 26, TRUE);
    if (ds->btnSearch) MoveWindow(ds->btnSearch, pad + searchW + 6, topY, 80, 26, TRUE);
    if (ds->layer) MoveWindow(ds->layer, pad + searchW + 92, topY, 180, 200, TRUE);
    if (ds->author) MoveWindow(ds->author, pad + searchW + 278, topY, 140, 200, TRUE);
    int infoY = topY + 32;
    int infoH = 34;
    if (ds->layerInfo) MoveWindow(ds->layerInfo, pad, infoY, (std::max)(100, w - pad * 2), infoH, TRUE);
    int btnY = (std::max)(infoY + infoH + 40, h - pad - 30);
    int listY = infoY + infoH + 4;
    int listH = (std::max)(100, btnY - listY - 8);
    int listW = (std::max)(200, (w - pad * 2) * 52 / 100);
    if (ds->list) MoveWindow(ds->list, pad, listY, listW, listH, TRUE);
    int detailX = pad + listW + 8;
    int detailW = (std::max)(100, w - pad - detailX);
    if (ds->detail) MoveWindow(ds->detail, detailX, listY, detailW, listH, TRUE);
    int curX = pad;
    if (ds->btnRetract) { MoveWindow(ds->btnRetract, curX, btnY, 110, 28, TRUE); curX += 116; }
    if (ds->btnPromote) { MoveWindow(ds->btnPromote, curX, btnY, 150, 28, TRUE); curX += 156; }
    if (ds->btnRefresh) { MoveWindow(ds->btnRefresh, curX, btnY, 100, 28, TRUE); curX += 106; }
    if (ds->btnCompare) { MoveWindow(ds->btnCompare, curX, btnY, 170, 28, TRUE); curX += 176; }
    if (ds->btnClose) MoveWindow(ds->btnClose, (std::max)(curX + 10, w - pad - 100), btnY, 100, 28, TRUE);
}

static LRESULT CALLBACK MemoryListWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ds = reinterpret_cast<MemoryListDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ds = reinterpret_cast<MemoryListDlgState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ds));

            ds->search = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                12, 12, 400, 26, hwnd, (HMENU)2801, nullptr, nullptr);
            ds->btnSearch = CreateWindowExW(0, L"BUTTON", Tr(L"検索", L"Search", L"Buscar"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 420, 12, 80, 26, hwnd, (HMENU)2809, nullptr, nullptr);
            ds->layer = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                506, 12, 180, 200, hwnd, (HMENU)2811, nullptr, nullptr);
            ComboBox_AddString(ds->layer, Tr(L"すべてのレイヤー", L"All layers", L"Todas las capas"));
            for (const auto& l : kMemoryLayers) ComboBox_AddString(ds->layer, l.label);
            ComboBox_SetCurSel(ds->layer, 0);
            ds->layerInfo = CreateWindowExW(0, L"STATIC", L"",
                WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX,
                12, 44, 800, 34, hwnd, (HMENU)2812, nullptr, nullptr);
            ds->author = CreateWindowExW(0, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                506, 12, 160, 200, hwnd, (HMENU)2802, nullptr, nullptr);
            ComboBox_AddString(ds->author, Tr(L"すべての作者", L"All authors", L"Todos los autores"));
            ComboBox_AddString(ds->author, Tr(L"agent", L"agent", L"agent"));
            ComboBox_AddString(ds->author, Tr(L"human", L"human", L"human"));
            ComboBox_AddString(ds->author, Tr(L"hancho", L"hancho", L"hancho"));
            ComboBox_SetCurSel(ds->author, 0);

            ds->list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                12, 46, 500, 400, hwnd, (HMENU)2803, nullptr, nullptr);
            ListView_SetExtendedListViewStyle(ds->list, LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
            MemoryListAddColumn(ds->list, 0, L"ID", 150);
            MemoryListAddColumn(ds->list, 1, Tr(L"レイヤー", L"Layer", L"Capa"), 95);
            MemoryListAddColumn(ds->list, 2, Tr(L"作者", L"Author", L"Autor"), 70);
            MemoryListAddColumn(ds->list, 3, Tr(L"状態", L"Status", L"Estado"), 80);
            MemoryListAddColumn(ds->list, 4, Tr(L"スコープ", L"Scope", L"Ámbito"), 130);
            MemoryListAddColumn(ds->list, 5, Tr(L"内容", L"Text", L"Texto"), 360);
            ListView_SetBkColor(ds->list, g_theme.panel);
            ListView_SetTextBkColor(ds->list, g_theme.panel);
            ListView_SetTextColor(ds->list, g_theme.listText);
            if (HWND header = ListView_GetHeader(ds->list)) {
                ApplyNativeControlTheme(header, IsDarkThemeActive());
            }

            ds->detail = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY |
                    ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                520, 46, 300, 400, hwnd, (HMENU)2807, nullptr, nullptr);

            ds->btnRetract = CreateWindowExW(0, L"BUTTON", Tr(L"L2 note を取り消す", L"Retract L2 note", L"Retirar L2 note"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 12, 456, 110, 28, hwnd, (HMENU)2804, nullptr, nullptr);
            ds->btnPromote = CreateWindowExW(0, L"BUTTON", Tr(L"L3 knowledge に昇格", L"Promote to L3 knowledge", L"Promover a L3 knowledge"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 128, 456, 150, 28, hwnd, (HMENU)2805, nullptr, nullptr);
            ds->btnRefresh = CreateWindowExW(0, L"BUTTON", Tr(L"再読み込み", L"Reload", L"Recargar"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 284, 456, 100, 28, hwnd, (HMENU)2806, nullptr, nullptr);
            ds->btnCompare = CreateWindowExW(0, L"BUTTON", Tr(L"注入の効果を比較", L"Injection effect", L"Efecto de inyección"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 390, 456, 170, 28, hwnd, (HMENU)2810, nullptr, nullptr);
            ds->btnClose = CreateWindowExW(0, L"BUTTON", Tr(L"閉じる", L"Close", L"Cerrar"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 740, 456, 100, 28, hwnd, (HMENU)IDOK, nullptr, nullptr);

            HWND controls[] = {ds->search, ds->btnSearch, ds->layer, ds->layerInfo, ds->author, ds->list, ds->detail,
                               ds->btnRetract, ds->btnPromote, ds->btnRefresh, ds->btnCompare, ds->btnClose};
            for (HWND h : controls) {
                if (h && ds->fontUi) SendMessageW(h, WM_SETFONT, reinterpret_cast<WPARAM>(ds->fontUi), TRUE);
            }

            MemoryListReload(ds);
            ApplyNativeThemeToWindow(hwnd);
            for (HWND combo : {ds->layer, ds->author}) {
                if (combo) SetWindowTheme(combo, IsDarkThemeActive() ? L"DarkMode_CFD" : L"", nullptr);
            }
            return 0;
        }
        case WM_NOTIFY: {
            if (!ds) break;
            auto* nm = reinterpret_cast<NMHDR*>(lParam);
            if (nm && nm->hwndFrom == ds->list && nm->code == LVN_ITEMCHANGED) {
                int sel = ListView_GetNextItem(ds->list, -1, LVNI_SELECTED);
                if (sel >= 0 && static_cast<size_t>(sel) < ds->rows.size() && ds->detail) {
                    const auto& row = ds->rows[static_cast<size_t>(sel)];
                    std::string id = row.value("id", std::string());
                    std::string kind = row.value("kind", std::string());
                    std::string full = MemoryGetIpc(ds->dash, id, kind);
                    nlohmann::json g = nlohmann::json::parse(full, nullptr, false);
                    std::string dump = full;
                    if (g.is_object()) {
                        auto doc = g.find("document");
                        if (doc != g.end()) dump = doc->dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
                    }
                    SetWindowTextW(ds->detail, NormalizeCrLfWide(dump).c_str());
                }
                MemoryListUpdateButtons(ds);
                return 0;
            }
            break;
        }
        case WM_SIZE:
            MemoryListLayout(hwnd, ds);
            return 0;
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDOK || id == IDCANCEL) {
                DestroyWindow(hwnd);
                return 0;
            }
            if (!ds) return 0;
            if (id == 2809 || id == 2806) {
                MemoryListReload(ds);
                return 0;
            }
            if ((id == 2811 || id == 2802) && HIWORD(wParam) == CBN_SELCHANGE) {
                MemoryListReload(ds);
                return 0;
            }
            if (id == 2810) {
                nlohmann::json c = nlohmann::json::parse(MemoryInjectCompareIpc(ds->dash, "", "", ""), nullptr, false);
                std::wstringstream msg;
                if (c.is_object() && c.value("ok", false)) {
                    auto lineOf = [](const nlohmann::json& b) {
                        std::wstringstream line;
                        line << Tr(L"実行 ", L"runs ", L"ejecuciones ") << b.value("runs", static_cast<int64_t>(0))
                             << Tr(L"  成功率 ", L"  success ", L"  éxito ")
                             << std::fixed << std::setprecision(1) << b.value("successRate", 0.0) * 100.0 << L"%"
                             << Tr(L"  再試行成功率 ", L"  retry success ", L"  éxito de reintento ")
                             << b.value("retrySuccessRate", 0.0) * 100.0 << L"%"
                             << Tr(L"  再発率 ", L"  repeat ", L"  repetición ")
                             << b.value("failureRepeatRate", 0.0) * 100.0 << L"%"
                             << Tr(L"  トークン/回 ", L"  tokens/run ", L"  tokens/ejecución ")
                             << std::setprecision(0) << b.value("avgTokensPerRun", 0.0);
                        return line.str();
                    };
                    msg << Tr(L"注入あり: ", L"With memory:    ", L"Con memoria:    ") << lineOf(c["injected"]) << L"\r\n"
                        << Tr(L"注入なし: ", L"Without memory: ", L"Sin memoria:    ") << lineOf(c["notInjected"]) << L"\r\n\r\n";
                    msg << Tr(L"差分 (1回あたり): トークン ", L"Delta per run: tokens ", L"Delta por ejecución: tokens ")
                        << std::fixed << std::setprecision(0) << c.value("tokenDeltaPerRun", 0.0)
                        << Tr(L" / 費用 $", L" / cost $", L" / coste $")
                        << std::setprecision(4) << c.value("costDeltaPerRun", 0.0)
                        << Tr(L" / 成功率 ", L" / success ", L" / éxito ")
                        << std::setprecision(1) << c.value("successRateDelta", 0.0) * 100.0 << L"%";
                    if (c.value("insufficientData", false)) {
                        msg << L"\r\n" << Tr(L"両方の実行がまだ足りません。注入は既定で無効のままです。",
                                             L"Not enough runs on both sides yet; injection stays off by default.",
                                             L"Aún no hay suficientes ejecuciones; la inyección sigue desactivada.");
                    }
                } else {
                    msg << Tr(L"比較できませんでした: ", L"Could not compare: ", L"No se pudo comparar: ")
                        << Utf8ToWide(c.is_object() ? c.value("error", std::string()) : std::string("invalid response"));
                }
                gui_platform::GetGuiPlatform()->ShowMessageBox(
                    hwnd,
                    Tr(L"注入の効果", L"Memory injection effect", L"Efecto de la inyección"),
                    msg.str(),
                    gui_platform::MessageBoxType::Info);
                return 0;
            }
            if (id == 2804 || id == 2805) {
                int sel = ListView_GetNextItem(ds->list, -1, LVNI_SELECTED);
                if (sel < 0 || static_cast<size_t>(sel) >= ds->rows.size()) {
                    gui_platform::GetGuiPlatform()->ShowMessageBox(
                        hwnd,
                        Tr(L"メモリ", L"Memory", L"Memoria"),
                        Tr(L"ノートを選択してください", L"Select a note first", L"Selecciona una nota"),
                        gui_platform::MessageBoxType::Info);
                    return 0;
                }
                const auto& row = ds->rows[static_cast<size_t>(sel)];
                std::string kind = row.value("kind", std::string());
                std::string mid = row.value("id", std::string());
                if (kind != "note") {
                    gui_platform::GetGuiPlatform()->ShowMessageBox(
                        hwnd,
                        Tr(L"メモリ", L"Memory", L"Memoria"),
                        Tr(L"L2 note だけが対象です", L"Only L2 notes can be changed",
                           L"Solo se pueden cambiar L2 notes"),
                        gui_platform::MessageBoxType::Info);
                    return 0;
                }
                bool promote = (id == 2805);
                std::wstring question = promote
                    ? Tr(L"この L2 note を L3 knowledge に昇格しますか？",
                         L"Promote this L2 note to L3 knowledge?",
                         L"¿Promover esta L2 note a L3 knowledge?")
                    : Tr(L"この L2 note を取り消しますか？", L"Retract this L2 note?", L"¿Retirar esta L2 note?");
                if (gui_platform::GetGuiPlatform()->ShowMessageBox(
                        hwnd,
                        Tr(L"メモリ", L"Memory", L"Memoria"),
                        question,
                        gui_platform::MessageBoxType::QuestionYesNo,
                        true) != gui_platform::MessageBoxResult::Yes) {
                    return 0;
                }
                std::string err;
                bool ok = promote ? PromoteMemoryNoteToL3(ds->dash, mid, err) : RetractMemoryNoteById(ds->dash, mid, err);
                if (!ok) {
                    gui_platform::GetGuiPlatform()->ShowMessageBox(
                        hwnd,
                        Tr(L"メモリ", L"Memory", L"Memoria"),
                        Utf8ToWide(err),
                        gui_platform::MessageBoxType::Error);
                }
                MemoryListReload(ds);
                return 0;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, g_theme.text);
            SetBkColor(hdc, g_theme.panel);
            return reinterpret_cast<LRESULT>(g_dialogBgBrush);
        }
        case WM_ERASEBKGND: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, g_dialogBgBrush);
            return 1;
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            delete ds;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowMemoryListDialog(DashboardState* st) {
    if (!st || !st->hwndMain) return;
    auto* ds = new MemoryListDlgState();
    ds->dash = st;
    ds->fontUi = st->hFontUi;

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = MemoryListWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"HanchoMemoryListWnd";
        RegisterClassExW(&wc);
        registered = true;
    }

    RECT parent = {};
    GetWindowRect(st->hwndMain, &parent);
    int dlgW = 900;
    int dlgH = 560;
    int x = parent.left + ((parent.right - parent.left) - dlgW) / 2;
    int y = parent.top + ((parent.bottom - parent.top) - dlgH) / 2;
    HWND hDlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"HanchoMemoryListWnd",
        Tr(L"メモリ一覧", L"Memory List", L"Lista de memoria"),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, x, y, dlgW, dlgH, st->hwndMain, nullptr, hInst, ds);
    if (hDlg) {
        ShowWindow(hDlg, SW_SHOW);
        SetForegroundWindow(hDlg);
    } else {
        delete ds;
    }
}

static void CopyTextToClipboard(HWND hwnd, const std::wstring& text);

// Column indexes of "Origin" and "Resources" in the task list (see UpdateTaskListColumnWidths).
static const int kTaskListOriginCol = 8;
static const int kTaskListResourcesCol = 9;

// Full text of one task-list cell. The control keeps the whole string even when the column is
// too narrow to draw it, so this is the text that hover and "copy cell" should use.
static std::wstring TaskListCellText(HWND list, int row, int col) {
    wchar_t buf[4096] = {};
    ListView_GetItemText(list, row, col, buf, static_cast<int>(sizeof(buf) / sizeof(buf[0])));
    return buf;
}

static void ShowTaskQueueContextMenu(DashboardState* st, const std::string& jobId, POINT ptScreen, const std::wstring& cellText) {
    if (!st || jobId.empty()) return;
    std::string err;
    std::string resp = TaskQueueGetIpc(st, jobId, false, 0, err);
    std::string jobState = JsonGetString(resp, "state", "");

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING | (cellText.empty() ? MF_GRAYED : 0), 3110, Tr(L"セルをコピー", L"Copy cell", L"Copiar celda"));
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    if (jobState == "running") {
        AppendMenuW(hMenu, MF_STRING, 3107, Tr(L"ツリー終了 (ジョブ中断)", L"Kill Tree (Cancel Job)", L"Terminar árbol (Cancelar)"));
        AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(hMenu, MF_STRING, 3101, Tr(L"再実行 (Retry)", L"Retry", L"Reintentar"));
    AppendMenuW(hMenu, MF_STRING, 3102, Tr(L"キャンセル (Cancel)", L"Cancel", L"Cancelar"));
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, 3103, Tr(L"キューを一時停止", L"Pause queue", L"Pausar cola"));
    AppendMenuW(hMenu, MF_STRING, 3104, Tr(L"キューを再開", L"Resume queue", L"Reanudar cola"));
    AppendMenuW(hMenu, MF_STRING, 3105, Tr(L"終了済みを消去", L"Purge finished", L"Purgar finalizados"));
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, 3106, Tr(L"詳細を表示", L"Show details", L"Ver detalles"));
    AppendMenuW(hMenu, MF_STRING, 3114, Tr(L"なぜ失敗したか", L"Why failed", L"Por qué falló"));
    AppendMenuW(hMenu, MF_STRING, 3112, Tr(L"過去の実行を表示", L"Show past runs", L"Ver ejecuciones anteriores"));
    AppendMenuW(hMenu, MF_STRING, 3113, Tr(L"メモリ一覧...", L"Memory list...", L"Lista de memoria..."));
    if (resp.find("\"bt\":{") != std::string::npos) {
        AppendMenuW(hMenu, MF_STRING, 3111, Tr(L"ビヘイビアツリーで表示", L"Show in Behavior Tree", L"Ver en el árbol de comportamiento"));
    }

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, ptScreen.x, ptScreen.y, 0, st->hwndMain, nullptr);
    DestroyMenu(hMenu);

    if (cmd == 3111) {
        // The right-clicked row is the selected row, so the view opens on this job's node.
        SendMessageW(st->hwndMain, WM_COMMAND, IDC_BT_DEBUG, 0);
    } else if (cmd == 3110) {
        CopyTextToClipboard(st->hwndMain, cellText);
    } else if (cmd == 3107) {
        if (gui_platform::GetGuiPlatform()->ShowMessageBox(
                st->hwndMain,
                Tr(L"ツリー終了の確認", L"Confirm Kill Tree", L"Confirmar cierre"),
                Tr(L"このタスクのプロセスツリーを強制終了（ジョブ中断）しますか？",
                   L"Are you sure you want to kill this task's process tree?",
                   L"¿Seguro que desea terminar el árbol de procesos de esta tarea?"),
                gui_platform::MessageBoxType::QuestionYesNo,
                true) == gui_platform::MessageBoxResult::Yes) {
            TaskQueueCancelIpc(st, jobId, err);
        }
    } else if (cmd == 3101) {
        TaskQueueRetryIpc(st, jobId, true, true, err);
    } else if (cmd == 3102) {
        TaskQueueCancelIpc(st, jobId, err);
    } else if (cmd == 3103 || cmd == 3104) {
        resp = TaskQueueGetIpc(st, jobId, false, 0, err);
        std::string tqName = JsonGetString(resp, "taskqueue", "");
        if (!tqName.empty()) TaskQueuePauseIpc(st, tqName, cmd == 3103, err);
    } else if (cmd == 3105) {
        resp = TaskQueueListIpc(st, "", "", 0, err);
        auto jobs = SplitJsonObjectArray(resp, "jobs");
        std::vector<std::string> queuesDone;
        for (const auto& j : jobs) {
            std::string tqName = JsonGetString(j, "taskqueue", "");
            if (tqName.empty()) continue;
            if (std::find(queuesDone.begin(), queuesDone.end(), tqName) != queuesDone.end()) continue;
            queuesDone.push_back(tqName);
            TaskQueuePurgeIpc(st, tqName, "[\"succeeded\",\"failed\",\"cancelled\"]", err);
        }
    } else if (cmd == 3106) {
        ShowTaskQueueJobDetails(st, jobId);
    } else if (cmd == 3112) {
        ShowTaskQueueJobDetails(st, jobId, 1);
    } else if (cmd == 3114) {
        ShowTaskQueueJobDetails(st, jobId, 2);
    } else if (cmd == 3113) {
        ShowMemoryListDialog(st);
    }
    RefreshDashboardData(st);
}

static void RefreshTaskQueueData(DashboardState* st, bool force = false) {
    if (!st || !st->hwndTaskList) return;
    if (st->listDisplayMode == ListDisplayMode::ProcessOnly) return;
    std::string err;
    std::string resp = TaskQueueListIpc(st, "", "", 0, err);
    // Nothing to do when the queue is unchanged and no job is running (only running rows show a countdown).
    int lang = static_cast<int>(g_uiLanguage);
    if (!force && resp == st->lastTaskListJson && lang == st->lastTaskListLang &&
        resp.find("\"state\":\"running\"") == std::string::npos) {
        return;
    }
    st->lastTaskListJson = resp;
    st->lastTaskListLang = lang;
    auto jobs = SplitJsonObjectArray(resp, "jobs");

    std::unordered_map<std::string, std::string> stateById;
    stateById.reserve(jobs.size());
    for (const auto& j : jobs) {
        stateById[JsonGetString(j, "id", "")] = JsonGetString(j, "state", "");
    }

    auto depsSatisfied = [&](const std::string& jobJson) {
        for (const auto& dep : TaskQueueDepList(jobJson)) {
            auto it = stateById.find(dep.first);
            std::string depState = (it == stateById.end()) ? "" : it->second;
            bool satisfied = (dep.second == "done")
                ? (depState == "succeeded" || depState == "failed" || depState == "cancelled")
                : (depState == "succeeded");
            if (!satisfied) return false;
        }
        return true;
    };
    auto jobRank = [&](const std::string& jobJson) {
        std::string st = JsonGetString(jobJson, "state", "");
        if (st == "running") return 0;
        if (st == "pending") return depsSatisfied(jobJson) ? 1 : 2;
        return 3;
    };
    std::stable_sort(jobs.begin(), jobs.end(), [&](const std::string& a, const std::string& b) {
        int ra = jobRank(a);
        int rb = jobRank(b);
        if (ra != rb) return ra < rb;
        std::string ca = JsonGetString(a, "created_at", "");
        std::string cb = JsonGetString(b, "created_at", "");
        if (!ca.empty() || !cb.empty()) {
            if (ca != cb) return ca > cb;
        }
        return false;
    });

    // Work out every row first; the list is rebuilt only when some row's text differs from last time.
    std::vector<TaskListRow> rows;  // one entry per job, in the order the list shows them
    std::unordered_set<std::string> nowRunning;  // jobs running now, to spot the ones that just ended
    std::map<std::string, int> failureCounts;   // failure class -> jobs, for the list caption
    rows.reserve(jobs.size());
    for (const auto& j : jobs) {
        TaskListRow row;
        std::string jobId = JsonGetString(j, "id", "");
        std::string state = JsonGetString(j, "state", "");
        std::string lastReason = LastAttemptReason(j);
        std::string failureClass;
        if (state == "failed") {
            failureClass = LastFailureClass(j);
            if (!failureClass.empty()) ++failureCounts[failureClass];
        }
        std::string kind = "other";
        if (state == "running") {
            kind = "running";
        } else if (state == "failed") {
            kind = (lastReason == "timed_out" || lastReason == "idle_timed_out") ? "timeout" : "failed";
        }
        row.id = jobId;
        row.kind = kind;
        if (state == "running") nowRunning.insert(jobId);
        // The process of the latest attempt, so selecting this row can show it in the monitor pane.
        int64_t latestPid = 0;
        auto attemptsOfRow = SplitJsonObjectArray(j, "attempts");
        if (!attemptsOfRow.empty()) {
            latestPid = JsonGetInt(attemptsOfRow.back(), "process_id", 0);
            if (latestPid <= 0) {
                latestPid = JsonGetInt(attemptsOfRow.back(), "remote_process_id", 0);
            }
        }
        row.procId = static_cast<int>(latestPid);

        std::wstring idStr = Utf8ToWide(jobId);
        std::wstring summaryStr = Utf8ToWide(JsonGetString(j, "summary", ""));
        std::string notes = JsonGetString(j, "notes", "");
        if (!notes.empty()) summaryStr += L"  [notes: " + Utf8ToWide(notes) + L"]";
        std::wstring taskDoingStr;
        if (state == "running") {
            auto attemptsDoing = SplitJsonObjectArray(j, "attempts");
            if (!attemptsDoing.empty()) {
                int64_t pidDoing = JsonGetInt(attemptsDoing.back(), "process_id", 0);
                if (pidDoing > 0) {
                    ProcessSnapshot taskSnap;
                    if (FetchProcessSnapshotIpc(st, static_cast<int>(pidDoing), taskSnap) && taskSnap.state == ProcState::Running) {
                        taskDoingStr = Utf8ToWide(taskSnap.activity);
                        if (!taskSnap.activityTarget.empty()) taskDoingStr += L" " + Utf8ToWide(taskSnap.activityTarget);
                    }
                }
            }
        }
        std::wstring stateStr = TaskQueueStateLabel(state, j, stateById);
        if (!failureClass.empty()) stateStr += L" \u00B7 " + Utf8ToWide(failureClass);
        if (!notes.empty()) stateStr += L" \U0001F4DD";
        std::string attemptStart = LastAttemptField(j, "start");
        std::string attemptEnd = LastAttemptField(j, "end");
        // The column shows the time of day only, but the sort uses the full date and time.
        const int64_t startEpochSec = attemptStart.empty() ? 0 : ParseTimestampStrToEpoch(attemptStart);
        int64_t endEpochSec = attemptEnd.empty() ? 0 : ParseTimestampStrToEpoch(attemptEnd);
        std::wstring startStr = L"-";
        std::wstring endStr = L"-";
        if (!attemptStart.empty()) {
            startStr = Utf8ToWide(TimeOfDayText(attemptStart));
            if (!attemptEnd.empty()) {
                endStr = Utf8ToWide(TimeOfDayText(attemptEnd));
            } else if (state == "running") {
                int timeoutSec = static_cast<int>(JsonGetInt(j, "timeout_sec", 0));
                int estimateSec = static_cast<int>(JsonGetInt(j, "estimate_sec", 0));
                int budgetSec = estimateSec > 0 ? estimateSec : timeoutSec;
                if (budgetSec > 0) {
                    std::string est = AddSecondsToTimestamp(attemptStart, budgetSec);
                    int y = 0, mo = 0, d = 0, h = 0, mi = 0, sec = 0;
                    if (sscanf_s(attemptStart.c_str(), "%d-%d-%d %d:%d:%d", &y, &mo, &d, &h, &mi, &sec) == 6) {
                        struct tm t = {};
                        t.tm_year = y - 1900;
                        t.tm_mon = mo - 1;
                        t.tm_mday = d;
                        t.tm_hour = h;
                        t.tm_min = mi;
                        t.tm_sec = sec;
                        t.tm_isdst = -1;
                        time_t startEpoch = mktime(&t);
                        if (startEpoch != static_cast<time_t>(-1)) {
                            time_t nowEpoch = time(nullptr);
                            int64_t elapsed = static_cast<int64_t>(nowEpoch - startEpoch);
                            int64_t remSec = static_cast<int64_t>(budgetSec) - elapsed;
                            if (remSec < 0) remSec = 0;
                            if (!est.empty()) {
                                endStr = L"~" + Utf8ToWide(TimeOfDayText(est)) + L" (" + FormatRemainingTime(remSec) + L")";
                            } else {
                                endStr = FormatRemainingTime(remSec);
                            }
                        }
                    }
                    if (endStr.empty() && !est.empty()) {
                        endStr = L"~" + Utf8ToWide(TimeOfDayText(est));
                    }
                    // The estimated end of the running attempt, with its date.
                    if (startEpochSec > 0) endEpochSec = startEpochSec + budgetSec;
                }
            }
        } else if (state == "pending") {
            int timeoutSec = static_cast<int>(JsonGetInt(j, "timeout_sec", 0));
            int estimateSec = static_cast<int>(JsonGetInt(j, "estimate_sec", 0));
            int budgetSec = estimateSec > 0 ? estimateSec : timeoutSec;
            if (budgetSec > 0) {
                endStr = FormatRemainingTime(budgetSec);
                // A job that has not started yet ends this long after it was queued.
                const int64_t createdEpoch = ParseTimestampStrToEpoch(JsonGetString(j, "created_at", ""));
                if (createdEpoch > 0) endEpochSec = createdEpoch + budgetSec;
            }
        }
        row.startKey = startEpochSec;
        row.endKey = endEpochSec;
        // Follow a task only when its process has just started, so a selection made by hand is kept.
        if (state == "running") {
            const std::string jobHost = JsonGetString(j, "host", "");
            auto attemptsSel = SplitJsonObjectArray(j, "attempts");
            if (!attemptsSel.empty()) {
                int64_t pidSel = JsonGetInt(attemptsSel.back(), "process_id", 0);
                if (!jobHost.empty() && pidSel <= 0) {
                    pidSel = JsonGetInt(attemptsSel.back(), "remote_process_id", 0);
                }
                if (pidSel > 0 && static_cast<int>(pidSel) != st->lastFollowedProcId) {
                    st->selectedProcId = static_cast<int>(pidSel);
                    st->selectedJobKey = jobId;
                    st->lastFollowedProcId = static_cast<int>(pidSel);
                    SyncStreamPane(st);
                }
            }
        }
        std::wstring depsStr = TaskQueueDepsDisplay(j);
        std::string blockedBy = JsonGetString(j, "blocked_by", "");
        if (!blockedBy.empty()) {
            if (!depsStr.empty()) depsStr += L" ";
            depsStr += L"<- " + Utf8ToWide(blockedBy);
        }
        std::wstring queueStr = Utf8ToWide(JsonGetString(j, "taskqueue", ""));
        const std::string jobHost = JsonGetString(j, "host", "");
        if (!jobHost.empty()) queueStr += L" @ " + Utf8ToWide(jobHost);
        std::wstring originTip;
        std::wstring originStr = TaskQueueOriginDisplay(j, originTip);
        row.originTip = std::move(originTip);
        std::wstring resourcesStr = TaskQueueResourcesDisplay(j);

        row.cells[0] = std::move(idStr);
        row.cells[1] = std::move(summaryStr);
        row.cells[2] = std::move(stateStr);
        row.cells[3] = std::move(startStr);
        row.cells[4] = std::move(endStr);
        row.cells[5] = std::move(taskDoingStr);
        row.cells[6] = std::move(depsStr);
        row.cells[7] = std::move(queueStr);
        row.cells[8] = std::move(originStr);
        row.cells[9] = std::move(resourcesStr);
        rows.push_back(std::move(row));
    }

    // The header click picks the column, so the order is applied to the rows here.
    SortTaskRows(rows, st->taskSortColumn, st->taskSortDirection);

    std::vector<std::string> newJobIds;
    std::vector<std::string> newJobStates;
    std::vector<int> newJobProcIds;
    std::vector<std::wstring> newRows;  // kTaskListColumns cells per row, flattened
    std::vector<std::wstring> newOriginTips;  // full origin details per row, for the Origin hover tip
    newJobIds.reserve(rows.size());
    newJobStates.reserve(rows.size());
    newJobProcIds.reserve(rows.size());
    newRows.reserve(rows.size() * kTaskListColumns);
    newOriginTips.reserve(rows.size());
    for (const auto& r : rows) {
        newJobIds.push_back(r.id);
        newJobStates.push_back(r.kind);
        newJobProcIds.push_back(r.procId);
        newOriginTips.push_back(r.originTip);
        for (int col = 0; col < kTaskListColumns; ++col) {
            newRows.push_back(r.cells[col]);
        }
    }

    for (const auto& id : st->runningTaskJobs) {
        if (!nowRunning.count(id)) StartTaskEndBlink(st, id);
    }
    st->runningTaskJobs = std::move(nowRunning);
    st->taskJobStates = std::move(newJobStates);
    st->taskJobProcIds = std::move(newJobProcIds);
    st->taskOriginTips = std::move(newOriginTips);
    std::wstring failureSummary = FailureClassSummary(failureCounts);
    if (!failureSummary.empty()) {
        failureSummary = Tr(L"失敗: ", L"failed: ", L"fallos: ") + failureSummary;
    }
    if (newRows == st->lastTaskRows && newJobIds == st->taskJobIds &&
        failureSummary == st->taskFailureSummary &&
        ListView_GetItemCount(st->hwndTaskList) == static_cast<int>(newJobIds.size())) {
        return;
    }
    st->taskFailureSummary = std::move(failureSummary);

    // Rebuilding the list drops the highlight, so remember which job was selected and select it again.
    std::string keepSelectedJob;
    int prevSel = ListView_GetNextItem(st->hwndTaskList, -1, LVNI_SELECTED);
    if (prevSel >= 0 && prevSel < static_cast<int>(st->taskJobIds.size())) keepSelectedJob = st->taskJobIds[prevSel];
    // It also resets the scroll position, so put that back afterwards too.
    int keepTopIndex = ListView_GetTopIndex(st->hwndTaskList);
    int keepScrollX = GetScrollPos(st->hwndTaskList, SB_HORZ);

    st->taskJobIds = std::move(newJobIds);
    st->lastTaskRows = std::move(newRows);
    SendMessageW(st->hwndTaskList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(st->hwndTaskList);

    for (int row = 0; row < static_cast<int>(st->taskJobIds.size()); ++row) {
        std::wstring* cells = &st->lastTaskRows[static_cast<size_t>(row) * kTaskListColumns];
        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT;
        lvi.iItem = row;
        lvi.iSubItem = 0;
        lvi.pszText = cells[0].data();
        ListView_InsertItem(st->hwndTaskList, &lvi);
        for (int col = 1; col < kTaskListColumns; ++col) {
            ListView_SetItemText(st->hwndTaskList, row, col, cells[col].data());
        }
    }
    if (!keepSelectedJob.empty()) {
        for (int r = 0; r < static_cast<int>(st->taskJobIds.size()); ++r) {
            if (st->taskJobIds[r] != keepSelectedJob) continue;
            ListView_SetItemState(st->hwndTaskList, r, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            break;
        }
    }
    RECT firstRow = {};
    int rowHeight = ListView_GetItemRect(st->hwndTaskList, 0, &firstRow, LVIR_BOUNDS) ? firstRow.bottom - firstRow.top : 0;
    ListView_Scroll(st->hwndTaskList, keepScrollX - GetScrollPos(st->hwndTaskList, SB_HORZ),
                    (keepTopIndex - ListView_GetTopIndex(st->hwndTaskList)) * rowHeight);
    SendMessageW(st->hwndTaskList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(st->hwndTaskList, nullptr, FALSE);
}

// ---- Plain-language agent activity (from the JSON stream, see DescribeLlmActivity) ----

static int64_t NowEpochMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::wstring FormatElapsed(int64_t ms) {
    if (ms < 0) ms = 0;
    int64_t sec = ms / 1000;
    wchar_t buf[32] = {};
    if (sec < 60) swprintf_s(buf, L"%llds", static_cast<long long>(sec));
    else if (sec < 3600) swprintf_s(buf, L"%lldm%02llds", static_cast<long long>(sec / 60), static_cast<long long>(sec % 60));
    else swprintf_s(buf, L"%lldh%02lldm", static_cast<long long>(sec / 3600), static_cast<long long>((sec % 3600) / 60));
    return buf;
}

static std::wstring ActivityIcon(const std::string& kind) {
    if (kind == "read") return L"📖 ";
    if (kind == "edit") return L"✏️ ";
    if (kind == "search") return L"🔍 ";
    if (kind == "command") return L"▶ ";
    if (kind == "web") return L"🌐 ";
    if (kind == "subagent") return L"🤖 ";
    if (kind == "plan") return L"📋 ";
    if (kind == "tool") return L"🔧 ";
    if (kind == "thinking") return L"💭 ";
    if (kind == "replying") return L"💬 ";
    if (kind == "waiting_model") return L"⏳ ";
    if (kind == "done") return L"✅ ";
    if (kind == "error") return L"❌ ";
    return L"";
}

static std::wstring ActivityText(const std::string& kind) {
    if (kind == "read") return Tr(L"ファイルを読んでいる", L"Reading file", L"Leyendo archivo");
    if (kind == "edit") return Tr(L"ファイルを編集している", L"Editing file", L"Editando archivo");
    if (kind == "search") return Tr(L"検索している", L"Searching", L"Buscando");
    if (kind == "command") return Tr(L"コマンドを実行している", L"Running command", L"Ejecutando comando");
    if (kind == "web") return Tr(L"Web を調べている", L"Browsing the web", L"Consultando la web");
    if (kind == "subagent") return Tr(L"サブエージェントに依頼している", L"Running a subagent", L"Ejecutando subagente");
    if (kind == "plan") return Tr(L"計画を更新している", L"Updating the plan", L"Actualizando el plan");
    if (kind == "tool") return Tr(L"ツールを使っている", L"Using a tool", L"Usando herramienta");
    if (kind == "thinking") return Tr(L"考えている", L"Thinking", L"Pensando");
    if (kind == "replying") return Tr(L"回答を書いている", L"Writing a reply", L"Escribiendo respuesta");
    if (kind == "waiting_model") return Tr(L"モデルの応答待ち", L"Waiting for the model", L"Esperando al modelo");
    if (kind == "done") return Tr(L"完了", L"Done", L"Terminado");
    if (kind == "error") return Tr(L"エラー", L"Error", L"Error");
    return Utf8ToWide(kind);
}

static std::wstring ActivityLabel(const std::string& kind) {
    std::wstring text = ActivityText(kind);
    return g_activityStyle ? ActivityIcon(kind) + text : text;
}

// One colour per activity type; brighter variants for the dark themes.
static COLORREF ActivityColor(const std::string& kind) {
    bool dark = g_darkThemeActive;
    if (kind == "read") return dark ? RGB(120, 180, 255) : RGB(30, 90, 200);
    if (kind == "edit") return dark ? RGB(110, 220, 150) : RGB(20, 130, 70);
    if (kind == "search") return dark ? RGB(200, 160, 255) : RGB(130, 60, 190);
    if (kind == "command") return dark ? RGB(255, 190, 90) : RGB(180, 90, 0);
    if (kind == "web") return dark ? RGB(90, 220, 230) : RGB(0, 120, 140);
    if (kind == "subagent") return dark ? RGB(255, 150, 220) : RGB(170, 30, 120);
    if (kind == "plan") return dark ? RGB(210, 220, 110) : RGB(110, 110, 0);
    if (kind == "replying") return dark ? RGB(120, 190, 255) : RGB(20, 100, 200);
    if (kind == "done") return dark ? RGB(110, 220, 150) : RGB(20, 130, 70);
    if (kind == "error") return dark ? RGB(255, 120, 120) : RGB(190, 30, 30);
    return g_theme.muted;  // tool, thinking, waiting_model
}

// Background tint: the running row is strongest, long steps get a lighter tint.
static COLORREF ActivityRowBackground(const std::string& kind, bool ongoing, bool longStep) {
    COLORREF c = ActivityColor(kind);
    if (ongoing) return BlendColor(c, g_theme.panel, 0.78f);
    if (longStep) return BlendColor(c, g_theme.panel, 0.90f);
    return g_theme.panel;
}

// Long paths / commands: keep the end, which carries the file name.
static std::wstring ShortTarget(const std::string& target, size_t maxChars = 80) {
    std::wstring t = Utf8ToWide(target);
    if (t.size() <= maxChars) return t;
    return L"…" + t.substr(t.size() - (maxChars - 1));
}

// "✏️ Editing file  src/a.cpp  (1m20s)"; the elapsed time only while it is still going on.
static std::wstring ActivityLine(const std::string& kind, const std::string& target, int64_t sinceMs, bool ongoing) {
    if (kind.empty()) return L"";
    std::wstring line = ActivityLabel(kind);
    if (!target.empty() && kind != "done" && kind != "waiting_model") line += L"  " + ShortTarget(target);
    if (ongoing && sinceMs > 0) line += L"  (" + FormatElapsed(NowEpochMs() - sinceMs) + L")";
    return line;
}

// Everything the KPI cards show (see DrawDashboardCanvas), so they are repainted only when it changes.
static std::wstring KpiSignature(DashboardState* st) {
    const auto& snaps = st->allSnapshots.empty() ? st->cachedSnapshots : st->allSnapshots;
    int running = 0, idle = 0, ok = 0, failed = 0;
    uint32_t treeProcs = 0;
    uint64_t memBytes = 0;
    int64_t minRemainingSec = -1;
    for (const auto& s : snaps) {
        if (s.state == ProcState::Running) {
            if (s.idleMs >= 10000) idle++;
            else running++;
            treeProcs += s.treeProcessCount;
            memBytes += s.treeMemoryBytes;
            if (s.timeoutSec > 0) {
                int64_t remSec = (static_cast<int64_t>(s.timeoutSec) * 1000 - static_cast<int64_t>(s.elapsedMs) + 999) / 1000;
                if (remSec < 0) remSec = 0;
                if (minRemainingSec < 0 || remSec < minRemainingSec) minRemainingSec = remSec;
            }
        } else if (s.state == ProcState::Completed) {
            ok++;
        } else {
            failed++;
        }
    }
    TaskQueueCounts tq = FetchTaskQueueCountsIpc(st);
    wchar_t buf[256] = {};
    swprintf_s(buf, L"%d|%zu|%d|%d|%d|%d|%u|%lld|%d|%d|%d|%d|%d|%d|%d|%d|%lld|%lld|",
               static_cast<int>(g_uiLanguage), snaps.size(), running, idle, ok, failed, treeProcs,
               static_cast<long long>(minRemainingSec), tq.running, tq.waiting, tq.pending, tq.blocked,
               tq.succeeded, tq.failed, tq.cancelled, tq.total,
               static_cast<long long>(tq.minRemainingSec), static_cast<long long>(tq.sumRemainingSec));
    return buf + Utf8ToWide(FormatBytes(memBytes));
}

static void RefreshDashboardData(DashboardState* st) {
    if (!st) return;
    // Hidden in the tray: no one sees the lists, so only watch for finished processes (completion sound).
    if (!IsWindowVisible(st->hwndMain) || IsIconic(st->hwndMain)) {
        st->allSnapshots = FetchProcessSnapshotsIpc(st);
        bool finished = false;
        bool failed = false;
        std::vector<int> endedProcIds;
        for (const auto& s : st->allSnapshots) {
            auto it = st->lastProcStates.find(s.id);
            if (it != st->lastProcStates.end() && it->second == ProcState::Running && s.state != ProcState::Running) {
                finished = true;
                endedProcIds.push_back(s.id);
                if (s.state != ProcState::Completed) failed = true;
            }
            st->lastProcStates[s.id] = s.state;
        }
        if (finished) PlayProcessEndSound(*st, failed);
        for (int id : endedProcIds) StartProcEndBlink(st, id);

    uint64_t nowTick = GetNowTickMs();
    if (nowTick - st->lastMemoryCheckTickMs >= 10000) {
        st->lastMemoryCheckTickMs = nowTick;
        auto mStats = GetMemoryStorageStatsIpc(st);
        if (mStats.nearLimit || mStats.exceeded) {
            st->memoryNearLimit = true;
            st->memoryWarningText = Utf8ToWide(mStats.warning);
            if (st->trayAdded && !st->memoryTrayWarned) {
                gui_platform::GetGuiPlatform()->TrayShowBalloon(st->nid, L"Hancho Memory", st->memoryWarningText, true);
                st->memoryTrayWarned = true;
            }
        } else {
            st->memoryNearLimit = false;
            st->memoryWarningText.clear();
            st->memoryTrayWarned = false;
        }
    }
        return;
    }
    // Hold every update while a list scrollbar is dragged; the next tick after release catches up.
    if (g_scrollDragWnd && (g_scrollDragWnd == st->hwndList || g_scrollDragWnd == st->hwndTaskList)) return;
    st->allSnapshots = FetchProcessSnapshotsIpc(st);

    bool processFinished = false;
    bool processFailed = false;
    std::vector<int> endedProcIds;
    std::unordered_map<int, ProcState> nextStates;
    nextStates.reserve(st->allSnapshots.size());
    for (const auto& s : st->allSnapshots) {
        auto it = st->lastProcStates.find(s.id);
        if (it != st->lastProcStates.end() && it->second == ProcState::Running &&
            s.state != ProcState::Running) {
            processFinished = true;
            endedProcIds.push_back(s.id);
            if (s.state != ProcState::Completed) processFailed = true;
        }
        nextStates.emplace(s.id, s.state);
    }
    st->lastProcStates.swap(nextStates);
    if (processFinished) {
        PlayProcessEndSound(*st, processFailed);
    }
    for (int id : endedProcIds) StartProcEndBlink(st, id);

    uint64_t nowTick = GetNowTickMs();
    if (nowTick - st->lastMemoryCheckTickMs >= 10000) {
        st->lastMemoryCheckTickMs = nowTick;
        auto mStats = GetMemoryStorageStatsIpc(st);
        if (mStats.nearLimit || mStats.exceeded) {
            st->memoryNearLimit = true;
            st->memoryWarningText = Utf8ToWide(mStats.warning);
            if (st->trayAdded && !st->memoryTrayWarned) {
                gui_platform::GetGuiPlatform()->TrayShowBalloon(st->nid, L"Hancho Memory", st->memoryWarningText, true);
                st->memoryTrayWarned = true;
            }
        } else {
            st->memoryNearLimit = false;
            st->memoryWarningText.clear();
            st->memoryTrayWarned = false;
        }
    }

    st->cachedSnapshots.clear();
    for (const auto& s : st->allSnapshots) {
        if (st->hideEndedProcesses && s.state != ProcState::Running) {
            continue;
        }
        st->cachedSnapshots.push_back(s);
    }

    SortSnapshots(st->cachedSnapshots, st->sortColumn, st->sortDirection);

    auto getNewestId = [&]() -> int {
        int maxId = -1;
        for (const auto& s : st->cachedSnapshots) {
            if (s.id > maxId) maxId = s.id;
        }
        return maxId;
    };

    if (st->selectedProcId <= 0 && !st->cachedSnapshots.empty()) {
        st->selectedProcId = getNewestId();
    } else if (st->selectedProcId > 0) {
        bool exists = false;
        for (const auto& s : st->cachedSnapshots) {
            if (s.id == st->selectedProcId) { exists = true; break; }
        }
        if (!exists) {
            st->selectedProcId = getNewestId();
        }
    }

    LayoutTerminalViews(st);

    int currentCount = ListView_GetItemCount(st->hwndList);
    int targetCount = static_cast<int>(st->cachedSnapshots.size());
    if (static_cast<int>(st->procListCells.size()) != currentCount) {
        st->procListCells.assign(static_cast<size_t>(currentCount), {});
        st->procListRowBg.assign(static_cast<size_t>(currentCount), CLR_INVALID);
    }
    bool rowsAddedOrRemoved = (currentCount != targetCount);
    std::vector<int> changedRows;

    if (rowsAddedOrRemoved) SendMessageW(st->hwndList, WM_SETREDRAW, FALSE, 0);

    while (currentCount > targetCount) {
        ListView_DeleteItem(st->hwndList, currentCount - 1);
        currentCount--;
    }
    st->procListCells.resize(static_cast<size_t>(targetCount));
    st->procListRowBg.resize(static_cast<size_t>(targetCount), CLR_INVALID);

    for (int i = 0; i < targetCount; ++i) {
        const auto& s = st->cachedSnapshots[i];
        std::wstring idStr = L"#" + std::to_wstring(s.id);
        std::wstring nameStr = Utf8ToWide(s.name);
        if (!s.agentType.empty()) {
            nameStr = L"[" + Utf8ToWide(s.agentType) + L"] " + nameStr;
        }
        // "{}" marks a JSON-streaming process; select it to see the stream in the bottom pane.
        bool isJsonStream = s.jsonStream || s.streamEventCount > 0;
        if (isJsonStream) nameStr = L"{} " + nameStr;
        std::wstring statusStr = StateLabelWide(s.state, s.idleMs >= 10000);
        if (!s.agentStatus.empty() && s.agentStatus != "idle" && s.state == ProcState::Running) {
            statusStr += L" (" + Utf8ToWide(s.agentStatus) + L")";
        } else if (s.state != ProcState::Running && s.exitCode != STILL_ACTIVE) {
            statusStr += L" (" + std::to_wstring(s.exitCode) + L")";
        }
        std::wstring startStr = Utf8ToWide(s.startTimeStr);
        std::wstring endStr;
        if (s.state == ProcState::Running) {
            if (s.timeoutSec > 0) {
                int64_t remainingMs = static_cast<int64_t>(s.timeoutSec) * 1000 -
                                      static_cast<int64_t>(s.elapsedMs);
                if (remainingMs < 0) remainingMs = 0;
                long long remainingSec = (remainingMs + 999) / 1000;
                endStr = FormatRemainingTime(remainingSec);
                if (!s.estimatedEndTimeStr.empty()) {
                    endStr = Utf8ToWide(s.estimatedEndTimeStr) + L" (" + endStr + L")";
                }
            } else if (!s.estimatedEndTimeStr.empty()) {
                endStr = L"~" + Utf8ToWide(s.estimatedEndTimeStr);
            }
        }
        if (endStr.empty()) {
            endStr = (s.state == ProcState::Running || s.endTimeStr.empty()) ? L"-" : Utf8ToWide(s.endTimeStr);
        }
        std::wstring uptimeStr = Utf8ToWide(FormatUptimeHMS(s.elapsedMs));
        if (s.timeoutSec > 0) {
            uptimeStr += L" / " + std::to_wstring(s.timeoutSec) + L"s";
        }
        std::wstring winPidStr = std::to_wstring(s.rootPid);
        std::wstring treeProcsStr = std::to_wstring(s.treeProcessCount);
        std::wstring memStr = Utf8ToWide(FormatBytes(s.treeMemoryBytes));
        std::wstring cpuStr = Utf8ToWide(FormatUptimeHMS(s.cpuTimeMs));
        std::wstring idleStr = L"-";
        if (s.state == ProcState::Running) {
            idleStr = Utf8ToWide(FormatDurationSec(s.idleMs));
            if (s.idleTimeoutSec > 0) {
                int64_t idleRemainSec = static_cast<int64_t>(s.idleTimeoutSec) - static_cast<int64_t>(s.idleMs / 1000);
                if (idleRemainSec < 0) idleRemainSec = 0;
                idleStr += L" (残: " + std::to_wstring(idleRemainSec) + L"s)";
            }
        }
        std::wstring previewStr = Utf8ToWide(s.lastLinePreview);
        // A raw JSON line says little; show what the agent is doing instead.
        if (isJsonStream && !s.activity.empty()) {
            previewStr = ActivityLine(s.activity, s.activityTarget, s.activitySinceEpochMs,
                                      s.state == ProcState::Running);
        }
        std::wstring cmdStr = Utf8ToWide(s.commandLine);
        std::wstring jobStr = L"-";
        if (!s.jobKey.empty()) {
            jobStr = Utf8ToWide(s.jobKey) + L" #" + std::to_wstring(s.attemptN);
        }

        std::vector<std::wstring> cells = {
            std::to_wstring(s.id), idStr, winPidStr, nameStr, statusStr, startStr, endStr, uptimeStr,
            treeProcsStr, memStr, cpuStr, idleStr, previewStr, cmdStr, jobStr};
        COLORREF rowBg = GetStateRowBgColor(s);
        std::vector<std::wstring>& prev = st->procListCells[static_cast<size_t>(i)];

        if (i >= currentCount) {
            LVITEMW lvi = {};
            lvi.mask = LVIF_TEXT | LVIF_PARAM;
            lvi.iItem = i;
            lvi.iSubItem = 0;
            lvi.pszText = cells[1].data();
            lvi.lParam = s.id;
            ListView_InsertItem(st->hwndList, &lvi);
            prev.assign(cells.size(), std::wstring());
            prev[0] = cells[0];
            prev[1] = cells[1];
        } else if (prev.size() != cells.size() || prev[0] != cells[0]) {
            LVITEMW lvi = {};
            lvi.mask = LVIF_PARAM;
            lvi.iItem = i;
            lvi.lParam = s.id;
            ListView_SetItem(st->hwndList, &lvi);
            prev.assign(cells.size(), std::wstring(1, L'\x1'));  // force every column below to be written
            prev[0] = cells[0];
        }

        bool rowChanged = (st->procListRowBg[static_cast<size_t>(i)] != rowBg);
        for (int col = 0; col < 14; ++col) {
            std::wstring& cell = cells[static_cast<size_t>(col) + 1];
            if (prev[static_cast<size_t>(col) + 1] == cell) continue;
            ListView_SetItemText(st->hwndList, i, col, cell.data());
            prev[static_cast<size_t>(col) + 1] = cell;
            rowChanged = true;
        }
        st->procListRowBg[static_cast<size_t>(i)] = rowBg;
        if (rowChanged) changedRows.push_back(i);
    }

    int selIdx = -1;
    for (int i = 0; i < targetCount; ++i) {
        if (st->cachedSnapshots[i].id == st->selectedProcId) {
            selIdx = i;
            break;
        }
    }
    if (selIdx >= 0) {
        UINT selState = ListView_GetItemState(st->hwndList, selIdx, LVIS_SELECTED);
        if (!(selState & LVIS_SELECTED)) {
            ListView_SetItemState(st->hwndList, selIdx, LVIS_SELECTED, LVIS_SELECTED);
        }
    }

    if (rowsAddedOrRemoved) {
        SendMessageW(st->hwndList, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(st->hwndList, nullptr, FALSE);
    } else {
        // Repaint only the rows whose text or colour changed (an idle list repaints nothing).
        for (int row : changedRows) ListView_RedrawItems(st->hwndList, row, row);
    }
    RefreshTaskQueueData(st);
    std::wstring kpiSig = KpiSignature(st);
    if (kpiSig != st->lastKpiSignature) {
        st->lastKpiSignature = std::move(kpiSig);
        InvalidateRect(st->hwndMain, &st->rcKpiArea, FALSE);
    }
    // The usage panel repaints itself every second; repaint it here only when the highlighted process changes.
    if (st->showUsagePanel && st->selectedProcId != st->lastUsagePanelProcId) {
        st->lastUsagePanelProcId = st->selectedProcId;
        InvalidateRect(st->hwndUsagePanel, nullptr, FALSE);
    }
    SyncStreamPane(st);
    if (st->showSupervisorHints) {
        EvaluateSupervisorHint(st, false);
    }
}

static void UpdateTaskListColumnWidths(DashboardState* st) {
    if (!st || !st->hwndTaskList) return;
    RECT rc = {};
    GetClientRect(st->hwndTaskList, &rc);
    int total = rc.right - rc.left;
    if (total < 400) return;

    // Summary takes ~half of the visible list width; the other columns share the rest.
    int summaryW = total / 2;
    // Columns other than Summary: ID, State, Start, End, Running, Depends, Queue, Origin, Resources.
    constexpr int kOther = kTaskListColumns - 1;
    const int base[kOther] = {135, 100, 105, 115, 190, 110, 82, 150, 200};
    int baseSum = 0;
    for (int i = 0; i < kOther; ++i) baseSum += base[i];

    // Shares are proportional with no per-column minimum, so the widths always add up to the list width.
    // A minimum here would push the sum past the list and cut off the last column (Resources).
    // The last column takes whatever remains.
    int remaining = total - summaryW;
    // ID is shown at 1.5x its proportional share and Queue at 3/4x. The other columns (State, Start,
    // End, Running, Depends) absorb the difference so the total still fits the list.
    constexpr int kIdIdx = 0, kQueueIdx = 6, kLastIdx = kOther - 1;
    const double idW = base[kIdIdx] * static_cast<double>(remaining) / baseSum * 1.5;
    const double queueW = base[kQueueIdx] * static_cast<double>(remaining) / baseSum * 0.75;
    const double otherBase = baseSum - base[kIdIdx] - base[kQueueIdx] - base[kLastIdx];
    // Resources (the last column) takes the rounding remainder.
    const double freeW = remaining - idW - queueW - (base[kLastIdx] * static_cast<double>(remaining) / baseSum);
    int widths[kOther] = {};
    int assigned = 0;
    for (int i = 0; i < kLastIdx; ++i) {
        double w;
        if (i == kIdIdx) w = idW;
        else if (i == kQueueIdx) w = queueW;
        else w = freeW * base[i] / otherBase;
        widths[i] = static_cast<int>(w);
        assigned += widths[i];
    }
    widths[kLastIdx] = remaining - assigned;

    ListView_SetColumnWidth(st->hwndTaskList, 0, widths[0]);
    ListView_SetColumnWidth(st->hwndTaskList, 1, summaryW);
    for (int i = 1; i < kOther; ++i) ListView_SetColumnWidth(st->hwndTaskList, i + 1, widths[i]);
}

static void LayoutControls(DashboardState* st, int width, int height) {
    if (!st) return;
    int margin = 10;

    st->rcKpiArea = {margin, kHeaderKpiTop, width - margin, kHeaderKpiBottom};

    int usageOffset = 0;
    if (st->showUsagePanel) {
        if (st->usagePanelHeight < 60) st->usagePanelHeight = 60;
        int usageTop = kHeaderKpiBottom + 2;
        st->rcUsagePanel = {margin, usageTop, width - margin, usageTop + st->usagePanelHeight};
        st->rcUsageSplitter = {margin, usageTop + st->usagePanelHeight, width - margin, usageTop + st->usagePanelHeight + 12};
        usageOffset = st->usagePanelHeight + 12;
        if (st->hwndUsagePanel) {
            MoveWindow(st->hwndUsagePanel, margin, usageTop, (std::max)(200, width - margin * 2), st->usagePanelHeight, TRUE);
            ShowWindow(st->hwndUsagePanel, SW_SHOW);
        }
    } else {
        st->rcUsagePanel = {};
        st->rcUsageSplitter = {};
        if (st->hwndUsagePanel) ShowWindow(st->hwndUsagePanel, SW_HIDE);
    }

    // Toolbar buttons flow into multiple rows when the window is too narrow.
    struct ToolbarButton { HWND hwnd; int w; };
    const ToolbarButton buttons[] = {
        {st->hwndBtnViewTasks, 150},
        {st->hwndBtnStart, 160},
        {st->hwndBtnSettings, 90},
        {st->hwndBtnKillAll, 74},
        {st->hwndBtnGridView, 28},
        {st->hwndBtnToggleList, 28},
        {st->hwndBtnToggleTerm, 30},
        {st->hwndBtnStreamPane, 28},
        {st->hwndBtnBtView, 34},
        {st->hwndBtnUsage, 28},
        {st->hwndBtnHideTray, 28},
        {st->hwndBtnRefresh, 28},
        {st->hwndBtnHint, 28},
        {st->hwndBtnSupervisor, 28},
        {st->hwndClock, 80}
    };
    const int gap = 7;
    int xCur = margin;
    int yBar = kToolbarTop + usageOffset;
    for (const auto& b : buttons) {
        if (!b.hwnd) continue;
        if (xCur > margin && xCur + b.w > width - margin) {
            xCur = margin;
            yBar += kToolbarH + 4;
        }
        MoveWindow(b.hwnd, xCur, yBar, b.w, kToolbarH, TRUE);
        xCur += b.w + gap;
    }
    st->processListTop = yBar + kToolbarH + 6;

    int topList = st->processListTop;
    st->rcStreamSplitter = {};
    st->streamPaneBottom = 0;
    st->rcProcCaption = {};
    st->rcTaskCaption = {};
    if (st->hideProcessList) {
        ShowWindow(st->hwndList, SW_HIDE);
        if (st->hwndTaskList) ShowWindow(st->hwndTaskList, SW_HIDE);
        if (st->hwndStreamPane) ShowWindow(st->hwndStreamPane, SW_HIDE);
        st->rcSplitter = {};
        st->rcRowsSplitter = {};
        st->rcProcCaption = {};
        st->rcTaskCaption = {};
    } else {
        int availVert = height - topList - kStatusBarH - margin;
        int maxListH = (std::max)(kMinListH, availVert - kTabH - 2 - kMinTerminalH);
        int areaH = st->processListHeight;
        if (st->hideTerminalGrid) {
            areaH = (std::max)(kMinListH, height - topList - kStatusBarH - 4);
        } else {
            if (areaH < kMinListH) areaH = kMinListH;
            if (areaH > maxListH) areaH = maxListH;
            st->processListHeight = areaH;
        }

        // The JSON stream pane takes the bottom of the list area, below a splitter.
        int listH = areaH;
        int paneH = 0;
        if (st->showStreamPane) {
            int maxPaneH = areaH - kMinListH - kRowSplitterH;
            if (maxPaneH >= kMinStreamPaneH) {
                paneH = (std::clamp)(st->streamPaneHeight, kMinStreamPaneH, maxPaneH);
                listH = areaH - paneH - kRowSplitterH;
            }
        }
        if (paneH > 0) {
            EnsureStreamPane(st);
            int splitTop = topList + listH;
            st->rcStreamSplitter = {margin, splitTop, width - margin, splitTop + kRowSplitterH};
            st->streamPaneBottom = topList + areaH;
            if (st->hwndStreamPane) {
                MoveWindow(st->hwndStreamPane, margin, splitTop + kRowSplitterH, width - margin * 2, paneH, TRUE);
                if (!IsWindowVisible(st->hwndStreamPane)) {
                    ShowWindow(st->hwndStreamPane, SW_SHOW);
                    SyncStreamPane(st);
                }
            }
        } else if (st->hwndStreamPane) {
            ShowWindow(st->hwndStreamPane, SW_HIDE);
        }
        st->lastTotalListHeight = listH;

        if (st->listDisplayMode == ListDisplayMode::TwoRows) {
            int availForRows = listH - kRowSplitterH;
            if (availForRows < kMinRowListH * 2) {
                st->rcRowsSplitter = {};
                st->rcTaskCaption = {};
                if (st->hwndTaskList) ShowWindow(st->hwndTaskList, SW_HIDE);
                int captionH = (std::min)(kListCaptionH, listH);
                st->rcProcCaption = {margin, topList, width - margin, topList + captionH};
                if (st->hwndList) {
                    ShowWindow(st->hwndList, SW_SHOW);
                    MoveWindow(st->hwndList, margin, topList + captionH, width - margin * 2,
                               (std::max)(0, listH - captionH), TRUE);
                }
            } else {
                int procH = static_cast<int>(availForRows * st->procRowRatio);
                if (procH < kMinRowListH) procH = kMinRowListH;
                if (procH > availForRows - kMinRowListH) procH = availForRows - kMinRowListH;
                int taskH = availForRows - procH;
                if (taskH < kMinRowListH) {
                    taskH = kMinRowListH;
                    procH = availForRows - taskH;
                }
                int splitterTop = topList + procH;
                int taskTop = splitterTop + kRowSplitterH;

                st->rcRowsSplitter = {margin, splitterTop, width - margin, splitterTop + kRowSplitterH};
                st->rcProcCaption = {margin, topList, width - margin, topList + kListCaptionH};
                st->rcTaskCaption = {margin, taskTop, width - margin, taskTop + kListCaptionH};

                if (st->hwndList) {
                    ShowWindow(st->hwndList, SW_SHOW);
                    MoveWindow(st->hwndList, margin, topList + kListCaptionH, width - margin * 2,
                               (std::max)(0, procH - kListCaptionH), TRUE);
                }
                if (st->hwndTaskList) {
                    ShowWindow(st->hwndTaskList, SW_SHOW);
                    MoveWindow(st->hwndTaskList, margin, taskTop + kListCaptionH, width - margin * 2,
                               (std::max)(0, taskH - kListCaptionH), TRUE);
                }
            }
        } else if (st->listDisplayMode == ListDisplayMode::ProcessOnly) {
            st->rcRowsSplitter = {};
            st->rcTaskCaption = {};
            st->rcProcCaption = {margin, topList, width - margin, topList + kListCaptionH};
            if (st->hwndTaskList) ShowWindow(st->hwndTaskList, SW_HIDE);
            if (st->hwndList) {
                ShowWindow(st->hwndList, SW_SHOW);
                MoveWindow(st->hwndList, margin, topList + kListCaptionH, width - margin * 2,
                           (std::max)(0, listH - kListCaptionH), TRUE);
            }
        } else { // ListDisplayMode::TaskOnly
            st->rcRowsSplitter = {};
            st->rcProcCaption = {};
            st->rcTaskCaption = {margin, topList, width - margin, topList + kListCaptionH};
            if (st->hwndList) ShowWindow(st->hwndList, SW_HIDE);
            if (st->hwndTaskList) {
                ShowWindow(st->hwndTaskList, SW_SHOW);
                MoveWindow(st->hwndTaskList, margin, topList + kListCaptionH, width - margin * 2,
                           (std::max)(0, listH - kListCaptionH), TRUE);
            }
        }

        st->rcSplitter = {margin, topList + areaH + 1, width - margin, topList + areaH + 1 + kSplitterH};
        if (st->hideTerminalGrid) st->rcSplitter = {};
    }

    int tabTop = st->rcSplitter.bottom > 0 ? st->rcSplitter.bottom + 1 : topList;
    if (st->hwndTab) {
        if (st->hideTerminalGrid) {
            ShowWindow(st->hwndTab, SW_HIDE);
        } else {
            ShowWindow(st->hwndTab, SW_SHOW);
            MoveWindow(st->hwndTab, margin, tabTop, width - margin * 2, kTabH, TRUE);
        }
    }

    if (st->hideTerminalGrid) {
        st->rcTerminalArea = {};
    } else {
        // Middle/Bottom Area: dedicated to embedded Direct2D TerminalView windows
        int termTop = tabTop + kTabH + 2;
        int termBottom = (std::max)(termTop + kMinTerminalH, height - kStatusBarH - 4);
        st->rcTerminalArea = {margin, termTop, width - margin, termBottom};
    }

    // Bottom Status Bar
    st->rcStatusBar = {0, height - kStatusBarH, width, height};

    UpdateTaskListColumnWidths(st);
    LayoutTerminalViews(st);
}

static void CopyTextToClipboard(HWND hwnd, const std::wstring& text) {
    gui_platform::GetGuiPlatform()->CopyToClipboard(hwnd, text);
}

struct TaskDetailsDlgState {
    DashboardState* dash = nullptr;
    int procId = 0;
    ProcessSnapshot snap;
    std::wstring fullOutput;
    std::wstring whyText;  // diagnosis of the task-queue attempt that ran this process
    nlohmann::json failure; // the same diagnosis, attached to the stream JSON tree
    HFONT hFontUi = nullptr;
    HFONT hFontBold = nullptr;
    HFONT hFontMono = nullptr;
    HWND hwndEditCmd = nullptr;
    HWND hwndLblInfo = nullptr;
    HWND hwndTree = nullptr;
    HWND hwndEditInfo = nullptr;
    HWND hwndBtnCopyCmd = nullptr;
    HWND hwndBtnCopyJson = nullptr;
    HWND hwndBtnToggleMode = nullptr;
    HWND hwndBtnOpenLog = nullptr;
    HWND hwndBtnOpenStream = nullptr;
    HWND hwndBtnClose = nullptr;

    bool isJsonMode = false;
    bool showTreeView = true;
    nlohmann::json rootJson;
    std::unordered_map<HTREEITEM, std::string> nodeJsonMap;
    std::unordered_map<HTREEITEM, std::string> nodePathMap;
};

static void PopulateJsonTreeRecursive(
    HWND hTree,
    HTREEITEM hParent,
    const nlohmann::json& j,
    const std::string& keyName,
    const std::string& currentPath,
    std::unordered_map<HTREEITEM, std::string>& nodeJsonMap,
    std::unordered_map<HTREEITEM, std::string>& nodePathMap,
    int depth = 0
) {
    TVINSERTSTRUCTW tvis = {};
    tvis.hParent = hParent;
    tvis.hInsertAfter = TVI_LAST;
    tvis.item.mask = TVIF_TEXT;

    std::wstring label;
    if (j.is_object()) {
        std::wstringstream ss;
        if (!keyName.empty()) {
            ss << Utf8ToWide(keyName) << L": ";
        }
        ss << L"{ ... } (" << j.size() << Tr(L" 個のキー", L" keys", L" claves") << L")";
        label = ss.str();
    } else if (j.is_array()) {
        std::wstringstream ss;
        if (!keyName.empty()) {
            ss << Utf8ToWide(keyName) << L": ";
        }
        ss << L"[ ... ] (" << j.size() << Tr(L" 要素", L" items", L" elementos") << L")";
        label = ss.str();
    } else if (j.is_string()) {
        std::string s = j.get<std::string>();
        if (s.size() > 70) s = s.substr(0, 67) + "...";
        label = (keyName.empty() ? L"" : Utf8ToWide(keyName) + L": ") + L"\"" + Utf8ToWide(s) + L"\"";
    } else if (j.is_number()) {
        label = (keyName.empty() ? L"" : Utf8ToWide(keyName) + L": ") + Utf8ToWide(j.dump());
    } else if (j.is_boolean()) {
        label = (keyName.empty() ? L"" : Utf8ToWide(keyName) + L": ") + (j.get<bool>() ? L"true" : L"false");
    } else if (j.is_null()) {
        label = (keyName.empty() ? L"" : Utf8ToWide(keyName) + L": ") + L"null";
    }

    tvis.item.pszText = const_cast<wchar_t*>(label.c_str());
    tvis.item.cchTextMax = static_cast<int>(label.size());
    HTREEITEM hItem = TreeView_InsertItem(hTree, &tvis);
    if (!hItem) return;

    try {
        nodeJsonMap[hItem] = j.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
    } catch (...) {
        nodeJsonMap[hItem] = "(JSON serialization error)";
    }
    nodePathMap[hItem] = currentPath;

    if (depth < 2) {
        TreeView_Expand(hTree, hItem, TVE_EXPAND);
    }

    if (j.is_object()) {
        for (auto it = j.begin(); it != j.end(); ++it) {
            std::string subPath = currentPath.empty() ? it.key() : (currentPath + "." + it.key());
            PopulateJsonTreeRecursive(hTree, hItem, it.value(), it.key(), subPath, nodeJsonMap, nodePathMap, depth + 1);
        }
    } else if (j.is_array()) {
        size_t idx = 0;
        for (const auto& item : j) {
            std::string itemKey = "[" + std::to_string(idx) + "]";
            std::string subPath = currentPath + itemKey;
            PopulateJsonTreeRecursive(hTree, hItem, item, itemKey, subPath, nodeJsonMap, nodePathMap, depth + 1);
            idx++;
        }
    }
}

static LRESULT CALLBACK TaskDetailsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ds = reinterpret_cast<TaskDetailsDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ds = reinterpret_cast<TaskDetailsDlgState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ds));

            HDC hdc = GetDC(hwnd);
            int logPixelsY = GetDeviceCaps(hdc, LOGPIXELSY);
            ReleaseDC(hwnd, hdc);
            int fontSize = -MulDiv(9, logPixelsY, 72);
            ds->hFontMono = CreateFontW(fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                FIXED_PITCH | FF_MODERN, L"Consolas");
            if (!ds->hFontMono) {
                ds->hFontMono = CreateFontW(fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                    DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                    FIXED_PITCH | FF_MODERN, L"Courier New");
            }

            HWND hLblCmd = CreateWindowExW(0, L"STATIC", Tr(L"コマンドライン:", L"Command Line:", L"Línea de comandos:"),
                WS_CHILD | WS_VISIBLE, 14, 10, 400, 18, hwnd, nullptr, nullptr, nullptr);

            std::wstring cmdW = Utf8ToWide(ds->snap.commandLine);
            ds->hwndEditCmd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", cmdW.c_str(),
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY,
                14, 30, 890, 60, hwnd, (HMENU)2001, nullptr, nullptr);

            ds->hwndLblInfo = CreateWindowExW(0, L"STATIC",
                ds->isJsonMode ? Tr(L"JSON ツリー / エディタ (ノード選択で展開・詳細表示):", L"JSON Tree / Editor (select node to view/edit):", L"Árbol / Editor JSON:")
                               : Tr(L"タスク詳細・出力:", L"Task Details & Output Preview:", L"Detalles de tarea y salida:"),
                WS_CHILD | WS_VISIBLE, 14, 96, 890, 20, hwnd, nullptr, nullptr, nullptr);

            ds->hwndTree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                WS_CHILD | (ds->isJsonMode && ds->showTreeView ? WS_VISIBLE : 0) |
                TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS,
                14, 122, 390, 470, hwnd, (HMENU)2008, nullptr, nullptr);
            TreeView_SetBkColor(ds->hwndTree, g_theme.panel);
            TreeView_SetTextColor(ds->hwndTree, g_theme.listText);
            TreeView_SetLineColor(ds->hwndTree, g_theme.border);

            ds->hwndEditInfo = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                ds->isJsonMode ? 414 : 14, 122, ds->isJsonMode ? 490 : 890, 470, hwnd, (HMENU)2002, nullptr, nullptr);

            if (ds->isJsonMode) {
                SendMessageW(ds->hwndTree, WM_SETREDRAW, FALSE, 0);
                PopulateJsonTreeRecursive(ds->hwndTree, TVI_ROOT, ds->rootJson, "root", "root", ds->nodeJsonMap, ds->nodePathMap, 0);
                SendMessageW(ds->hwndTree, WM_SETREDRAW, TRUE, 0);
                InvalidateRect(ds->hwndTree, nullptr, TRUE);

                std::wstring dumpW = NormalizeCrLfWide(ds->rootJson.dump(2, ' ', false, nlohmann::json::error_handler_t::replace));
                SetWindowTextW(ds->hwndEditInfo, dumpW.c_str());
            } else {
                std::wstringstream ss;
                ss << Tr(L"ID              : ", L"ID              : ", L"ID              : ") << L"#" << ds->snap.id << L"\r\n"
                   << Tr(L"名前            : ", L"Name            : ", L"Nombre          : ") << Utf8ToWide(ds->snap.name) << L"\r\n"
                   << Tr(L"状態            : ", L"Status          : ", L"Estado          : ") << StateLabelWide(ds->snap.state, ds->snap.idleMs >= 10000);
                if (ds->snap.state != ProcState::Running && ds->snap.exitCode != STILL_ACTIVE) {
                    ss << Tr(L" (終了コード: ", L" (Exit Code: ", L" (Código de salida: ") << ds->snap.exitCode << L")";
                }
                ss << L"\r\n"
                   << Tr(L"コマンドライン  : ", L"Command Line    : ", L"Línea de comandos: ") << Utf8ToWide(ds->snap.commandLine) << L"\r\n"
                   << Tr(L"作業ディレクトリ: ", L"Working Dir     : ", L"Directorio      : ")
                   << (ds->snap.workingDir.empty() ? Tr(L"(既定)", L"(default)", L"(predeterminado)") : Utf8ToWide(ds->snap.workingDir)) << L"\r\n"
                   << Tr(L"ルート PID      : ", L"Root PID        : ", L"PID raíz        : ") << ds->snap.rootPid
                   << L" (" << ds->snap.treeProcessCount << Tr(L" プロセス (Job Object)", L" processes in Job Object", L" procesos en Job Object") << L")\r\n"
                   << Tr(L"メモリ使用量    : ", L"Memory Usage    : ", L"Uso de memoria  : ") << Utf8ToWide(FormatBytes(ds->snap.treeMemoryBytes)) << L"\r\n"
                   << Tr(L"開始時刻        : ", L"Start Time      : ", L"Hora de inicio  : ") << Utf8ToWide(ds->snap.startTimeStr) << L"\r\n"
                   << Tr(L"終了時刻        : ", L"End Time        : ", L"Hora de fin     : ")
                   << (ds->snap.endTimeStr.empty() ? Tr(L"(実行中)", L"(running)", L"(en ejecución)") : Utf8ToWide(ds->snap.endTimeStr)) << L"\r\n";
                if (ds->snap.state == ProcState::Running && ds->snap.timeoutSec > 0) {
                    int64_t remainingMs = static_cast<int64_t>(ds->snap.timeoutSec) * 1000 - static_cast<int64_t>(ds->snap.elapsedMs);
                    int64_t remSec = (remainingMs + 999) / 1000;
                    if (remSec < 0) remSec = 0;
                    ss << Tr(L"残り時間        : ", L"Time Remaining  : ", L"Tiempo restante : ") << FormatRemainingTime(remSec)
                       << L" (" << Tr(L"タイムアウト: ", L"Timeout: ", L"Tiempo límite: ") << ds->snap.timeoutSec << L"s)\r\n";
                }
                ss << Tr(L"稼働時間        : ", L"Uptime          : ", L"Tiempo activo   : ") << Utf8ToWide(FormatUptimeHMS(ds->snap.elapsedMs))
                   << L" (" << Utf8ToWide(FormatDurationSec(ds->snap.elapsedMs)) << L")\r\n"
                   << Tr(L"CPU時間         : ", L"CPU Time        : ", L"Tiempo de CPU   : ") << Utf8ToWide(FormatUptimeHMS(ds->snap.cpuTimeMs)) << L"\r\n"
                   << Tr(L"ログファイル    : ", L"Log File        : ", L"Archivo de log  : ") << Utf8ToWide(ds->snap.logFilePath) << L"\r\n"
                    << Tr(L"完了ファイル    : ", L"Done File       : ", L"Archivo .done   : ") << Utf8ToWide(ds->snap.doneFilePath) << L"\r\n";
                 if (!ds->whyText.empty()) {
                     ss << L"--------------------------------------------------------------------------------\r\n"
                        << Tr(L"なぜ失敗したか:", L"Why it failed:", L"Por qué falló:") << L"\r\n"
                        << ds->whyText;
                 }
                 ss << L"--------------------------------------------------------------------------------\r\n"
                    << ds->fullOutput;
                 SetWindowTextW(ds->hwndEditInfo, ss.str().c_str());
            }

            ds->hwndBtnCopyCmd = CreateWindowExW(0, L"BUTTON", Tr(L"コマンドラインをコピー", L"Copy Command Line", L"Copiar línea de comandos"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 14, 600, 180, 30, hwnd, (HMENU)2003, nullptr, nullptr);
            ds->hwndBtnCopyJson = CreateWindowExW(0, L"BUTTON", Tr(L"JSONをコピー", L"Copy JSON", L"Copiar JSON"),
                WS_CHILD | (ds->isJsonMode ? WS_VISIBLE : 0) | BS_OWNERDRAW, 202, 600, 130, 30, hwnd, (HMENU)2006, nullptr, nullptr);
            ds->hwndBtnToggleMode = CreateWindowExW(0, L"BUTTON",
                ds->showTreeView ? Tr(L"生テキスト表示に切替", L"Switch to Raw Text", L"Cambiar a texto")
                                 : Tr(L"JSONツリー表示に切替", L"Switch to JSON Tree", L"Cambiar a árbol JSON"),
                WS_CHILD | (ds->isJsonMode ? WS_VISIBLE : 0) | BS_OWNERDRAW, 340, 600, 175, 30, hwnd, (HMENU)2005, nullptr, nullptr);
            ds->hwndBtnOpenLog = CreateWindowExW(0, L"BUTTON", Tr(L"ログファイルを開く", L"Open Log File", L"Abrir archivo de registro"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 523, 600, 160, 30, hwnd, (HMENU)2004, nullptr, nullptr);
            ds->hwndBtnOpenStream = CreateWindowExW(0, L"BUTTON", Tr(L"ストリーム (jsonl) を開く", L"Open Stream File", L"Abrir flujo"),
                WS_CHILD | (!ds->snap.streamFilePath.empty() ? WS_VISIBLE : 0) | BS_OWNERDRAW, 691, 600, 160, 30, hwnd, (HMENU)2007, nullptr, nullptr);
            ds->hwndBtnClose = CreateWindowExW(0, L"BUTTON", Tr(L"閉じる", L"Close", L"Cerrar"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 860, 600, 95, 30, hwnd, (HMENU)IDOK, nullptr, nullptr);

            SendMessageW(hLblCmd, WM_SETFONT, (WPARAM)ds->hFontBold, TRUE);
            SendMessageW(ds->hwndLblInfo, WM_SETFONT, (WPARAM)ds->hFontBold, TRUE);
            HWND uiCtrls[] = {ds->hwndEditCmd, ds->hwndTree, ds->hwndBtnCopyCmd, ds->hwndBtnCopyJson, ds->hwndBtnToggleMode, ds->hwndBtnOpenLog, ds->hwndBtnOpenStream, ds->hwndBtnClose};
            for (HWND h : uiCtrls) {
                if (h) SendMessageW(h, WM_SETFONT, (WPARAM)ds->hFontUi, TRUE);
            }
            if (ds->hFontMono) {
                SendMessageW(ds->hwndEditInfo, WM_SETFONT, (WPARAM)ds->hFontMono, TRUE);
            }

            ApplyNativeThemeToWindow(hwnd);
            return 0;
        }
        case WM_NOTIFY: {
            auto* pnmh = reinterpret_cast<NMHDR*>(lParam);
            if (ds && pnmh && ds->hwndTree && pnmh->hwndFrom == ds->hwndTree) {
                if (pnmh->code == TVN_SELCHANGEDW) {
                    auto* pnmtv = reinterpret_cast<NMTREEVIEWW*>(lParam);
                    HTREEITEM hSel = pnmtv->itemNew.hItem;
                    if (hSel) {
                        auto it = ds->nodeJsonMap.find(hSel);
                        if (it != ds->nodeJsonMap.end()) {
                            std::wstring jsonW = NormalizeCrLfWide(it->second);
                            SetWindowTextW(ds->hwndEditInfo, jsonW.c_str());
                        }
                        auto itP = ds->nodePathMap.find(hSel);
                        if (itP != ds->nodePathMap.end() && ds->hwndLblInfo) {
                            std::wstring pathLbl = std::wstring(Tr(L"JSON パス: ", L"JSON Path: ", L"Ruta JSON: ")) + Utf8ToWide(itP->second);
                            SetWindowTextW(ds->hwndLblInfo, pathLbl.c_str());
                        }
                    }
                }
            }
            break;
        }
        case WM_SIZE: {
            if (!ds) return 0;
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            int pad = 14;
            int editW = (std::max)(200, w - pad * 2);
            MoveWindow(ds->hwndEditCmd, pad, 30, editW, 60, TRUE);
            if (ds->hwndLblInfo) {
                MoveWindow(ds->hwndLblInfo, pad, 96, editW, 20, TRUE);
            }
            int btnY = (std::max)(220, h - 42);
            int mainY = 122;
            int mainH = (std::max)(100, btnY - mainY - 8);

            if (ds->isJsonMode && ds->showTreeView && ds->hwndTree) {
                int treeW = (editW - 12) * 44 / 100;
                int jsonW = editW - treeW - 12;
                ShowWindow(ds->hwndTree, SW_SHOW);
                MoveWindow(ds->hwndTree, pad, mainY, treeW, mainH, TRUE);
                MoveWindow(ds->hwndEditInfo, pad + treeW + 12, mainY, jsonW, mainH, TRUE);
            } else {
                if (ds->hwndTree) ShowWindow(ds->hwndTree, SW_HIDE);
                MoveWindow(ds->hwndEditInfo, pad, mainY, editW, mainH, TRUE);
            }

            int curX = pad;
            if (ds->hwndBtnCopyCmd) {
                MoveWindow(ds->hwndBtnCopyCmd, curX, btnY, 170, 30, TRUE);
                curX += 176;
            }
            if (ds->hwndBtnCopyJson) {
                if (ds->isJsonMode) {
                    ShowWindow(ds->hwndBtnCopyJson, SW_SHOW);
                    MoveWindow(ds->hwndBtnCopyJson, curX, btnY, 120, 30, TRUE);
                    curX += 126;
                } else {
                    ShowWindow(ds->hwndBtnCopyJson, SW_HIDE);
                }
            }
            if (ds->hwndBtnToggleMode) {
                if (ds->isJsonMode) {
                    ShowWindow(ds->hwndBtnToggleMode, SW_SHOW);
                    MoveWindow(ds->hwndBtnToggleMode, curX, btnY, 175, 30, TRUE);
                    curX += 181;
                } else {
                    ShowWindow(ds->hwndBtnToggleMode, SW_HIDE);
                }
            }
            if (ds->hwndBtnOpenLog) {
                MoveWindow(ds->hwndBtnOpenLog, curX, btnY, 150, 30, TRUE);
                curX += 156;
            }
            if (ds->hwndBtnOpenStream) {
                if (!ds->snap.streamFilePath.empty()) {
                    ShowWindow(ds->hwndBtnOpenStream, SW_SHOW);
                    MoveWindow(ds->hwndBtnOpenStream, curX, btnY, 150, 30, TRUE);
                    curX += 156;
                } else {
                    ShowWindow(ds->hwndBtnOpenStream, SW_HIDE);
                }
            }
            if (ds->hwndBtnClose) {
                MoveWindow(ds->hwndBtnClose, (std::max)(curX + 10, w - pad - 100), btnY, 100, 30, TRUE);
            }
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == 2003 && ds) {
                CopyTextToClipboard(hwnd, Utf8ToWide(ds->snap.commandLine));
            } else if (id == 2004 && ds) {
                if (!ds->snap.logFilePath.empty()) {
                    std::wstring logW = Utf8ToWide(ds->snap.logFilePath);
                    gui_platform::GetGuiPlatform()->OpenFileInEditor(hwnd, logW);
                }
            } else if (id == 2005 && ds) {
                if (ds->isJsonMode) {
                    ds->showTreeView = !ds->showTreeView;
                    if (ds->showTreeView) {
                        SetWindowTextW(ds->hwndBtnToggleMode, Tr(L"生テキスト表示に切替", L"Switch to Raw Text", L"Cambiar a texto"));
                        HTREEITEM hSel = ds->hwndTree ? TreeView_GetSelection(ds->hwndTree) : nullptr;
                        if (hSel && ds->nodeJsonMap.count(hSel)) {
                            SetWindowTextW(ds->hwndEditInfo, NormalizeCrLfWide(ds->nodeJsonMap[hSel]).c_str());
                        } else {
                            SetWindowTextW(ds->hwndEditInfo, NormalizeCrLfWide(ds->rootJson.dump(2, ' ', false, nlohmann::json::error_handler_t::replace)).c_str());
                        }
                    } else {
                        SetWindowTextW(ds->hwndBtnToggleMode, Tr(L"JSONツリー表示に切替", L"Switch to JSON Tree", L"Cambiar a árbol JSON"));
                        SetWindowTextW(ds->hwndEditInfo, ds->fullOutput.c_str());
                    }
                    RECT rc;
                    GetClientRect(hwnd, &rc);
                    SendMessageW(hwnd, WM_SIZE, 0, MAKELPARAM(rc.right - rc.left, rc.bottom - rc.top));
                }
            } else if (id == 2006 && ds) {
                HTREEITEM hSel = (ds->hwndTree && ds->showTreeView) ? TreeView_GetSelection(ds->hwndTree) : nullptr;
                if (hSel && ds->nodeJsonMap.count(hSel)) {
                    CopyTextToClipboard(hwnd, Utf8ToWide(ds->nodeJsonMap[hSel]));
                } else if (!ds->rootJson.is_null()) {
                    CopyTextToClipboard(hwnd, Utf8ToWide(ds->rootJson.dump(2, ' ', false, nlohmann::json::error_handler_t::replace)));
                }
            } else if (id == 2007 && ds) {
                if (!ds->snap.streamFilePath.empty()) {
                    std::wstring streamW = Utf8ToWide(ds->snap.streamFilePath);
                    gui_platform::GetGuiPlatform()->OpenFileInEditor(hwnd, streamW);
                }
            } else if (id == IDOK || id == IDCANCEL) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis || !ds) break;
            bool pressed = (dis->itemState & ODS_SELECTED) != 0;
            COLORREF bg = pressed ? g_theme.border : g_theme.panel;
            HBRUSH bgBrush = CreateSolidBrush(bg);
            FillRect(dis->hDC, &dis->rcItem, bgBrush);
            DeleteObject(bgBrush);
            HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
            HGDIOBJ oldPen = SelectObject(dis->hDC, borderPen);
            HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
            Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
            SelectObject(dis->hDC, oldBrush);
            SelectObject(dis->hDC, oldPen);
            DeleteObject(borderPen);

            if (dis->itemState & ODS_FOCUS) {
                RECT rcFocus = dis->rcItem;
                InflateRect(&rcFocus, -2, -2);
                DrawFocusRect(dis->hDC, &rcFocus);
            }

            wchar_t text[256] = {};
            GetWindowTextW(dis->hwndItem, text, 256);
            RECT rc = dis->rcItem;
            InflateRect(&rc, -4, 0);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, g_theme.text);
            HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC, ds->hFontUi));
            DrawTextW(dis->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dis->hDC, oldFont);
            return TRUE;
        }
        case WM_ERASEBKGND: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, g_dialogBgBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, g_theme.text);
            SetBkColor(hdc, g_theme.panel);
            return reinterpret_cast<LRESULT>(g_dialogBgBrush);
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            if (ds) {
                if (ds->hFontMono) DeleteObject(ds->hFontMono);
                delete ds;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// A process started by a task-queue attempt inherits that attempt's diagnosis, so the
// process detail window can answer "why did it stop" without opening the job.
static nlohmann::json FailureForProcess(DashboardState* st, int procId) {
    if (!st || procId <= 0) return nlohmann::json();
    nlohmann::json req = {{"action", "taskqueue_diagnose"}, {"process_id", procId}};
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp) && resp.value("ok", false) && resp.contains("failure") && resp["failure"].is_object()) {
        return resp["failure"];
    }
    if (st->tqm) {
        std::string err;
        std::string listResp = st->tqm->TaskQueueList("", "", 0, err);
        nlohmann::json root = nlohmann::json::parse(listResp, nullptr, false);
        if (!root.is_discarded() && root.is_object()) {
            auto jobs = root.find("jobs");
            if (jobs != root.end() && jobs->is_array()) {
                nlohmann::json failure;
                for (const auto& job : *jobs) {
                    if (!job.is_object()) continue;
                    auto attempts = job.find("attempts");
                    if (attempts == job.end() || !attempts->is_array()) continue;
                    for (const auto& attempt : *attempts) {
                        if (!attempt.is_object()) continue;
                        if (attempt.value("process_id", 0) != procId) continue;
                        auto f = attempt.find("failure");
                        if (f != attempt.end() && f->is_object() && !f->empty()) failure = *f;
                    }
                }
                return failure;
            }
        }
    }
    return nlohmann::json();
}

static void ShowTaskDetailsDialog(DashboardState* st, int procId) {
    if (!st || procId <= 0) return;
    ProcessSnapshot snap;
    if (!FetchProcessSnapshotIpc(st, procId, snap)) return;

    static bool regDone = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!regDone) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = TaskDetailsWndProc;
        wc.hInstance = hInst;
        wc.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"HanchoTaskDetailsWnd";
        RegisterClassExW(&wc);
        regDone = true;
    }

    auto* ds = new TaskDetailsDlgState();
    ds->dash = st;
    ds->procId = procId;
    ds->snap = snap;
    ds->hFontUi = st->hFontUi;
    ds->hFontBold = st->hFontUiBold;
    ds->failure = FailureForProcess(st, procId);
    ds->whyText = ds->failure.empty() ? L"" : WhyFailureText(ds->failure);

    std::string outUtf8;
    GetProcessOutputIpc(st, procId, 200, outUtf8);
    ds->fullOutput = NormalizeCrLfWide(outUtf8);

    bool isStream = snap.jsonStream || !snap.agentType.empty() || snap.streamEventCount > 0;
    if (isStream) {
        nlohmann::json root = nlohmann::json::object();

        nlohmann::json proc = nlohmann::json::object();
        proc["id"] = snap.id;
        proc["name"] = snap.name;
        proc["agentType"] = snap.agentType;
        proc["state"] = ProcStateToString(snap.state);
        proc["exitCode"] = snap.exitCode;
        proc["commandLine"] = snap.commandLine;
        proc["workingDir"] = snap.workingDir;
        proc["rootPid"] = snap.rootPid;
        proc["treeProcessCount"] = snap.treeProcessCount;
        proc["memoryBytes"] = snap.treeMemoryBytes;
        proc["startTime"] = snap.startTimeStr;
        proc["endTime"] = snap.endTimeStr;
        proc["elapsedMs"] = snap.elapsedMs;
        proc["cpuTimeMs"] = snap.cpuTimeMs;
        proc["logFilePath"] = snap.logFilePath;
        proc["streamFilePath"] = snap.streamFilePath;
        proc["jsonStream"] = snap.jsonStream;
        if (snap.state == ProcState::Running && snap.timeoutSec > 0) {
            int64_t remainingMs = static_cast<int64_t>(snap.timeoutSec) * 1000 - static_cast<int64_t>(snap.elapsedMs);
            int64_t remSec = (remainingMs + 999) / 1000;
            if (remSec < 0) remSec = 0;
            proc["remainingSec"] = remSec;
            proc["remainingTime"] = WideToUtf8(FormatRemainingTime(remSec));
            proc["timeoutSec"] = snap.timeoutSec;
        }
        root["process"] = proc;

        LlmAgentState agStateObj;
        GetAgentStateIpc(st, procId, agStateObj);

        auto v1Events = ReadHanchoEventsFile(
            HanchoEventsFilePathForStream(snap.streamFilePath), 0, "", 0);
        std::string modelForState = agStateObj.model;
        if (modelForState.empty()) {
            for (const auto& ev : v1Events) {
                if (!ev.contains("agent") || !ev["agent"].is_object()) continue;
                std::string model = ev["agent"].value("model", std::string());
                if (!model.empty()) { modelForState = model; break; }
            }
        }
        nlohmann::json agState = nlohmann::json::object();
        agState["agentType"] = snap.agentType.empty() ? agStateObj.agentType : snap.agentType;
        agState["model"] = modelForState;
        agState["status"] = agStateObj.status.empty() ? snap.agentStatus : agStateObj.status;
        agState["currentTool"] = agStateObj.currentTool.empty() ? snap.currentTool : agStateObj.currentTool;
        agState["currentToolAction"] = agStateObj.currentToolAction.empty() ? snap.currentToolAction : agStateObj.currentToolAction;
        agState["supervisorGoal"] = agStateObj.supervisorGoal.empty() ? snap.supervisorGoal : agStateObj.supervisorGoal;
        agState["supervisorNotes"] = agStateObj.supervisorNotes.empty() ? snap.supervisorNotes : agStateObj.supervisorNotes;
        agState["totalTokens"] = agStateObj.totalTokens;
        agState["costUsd"] = agStateObj.costUsd;
        agState["stepCount"] = agStateObj.stepCount;
        agState["lastThought"] = agStateObj.lastThought.empty() ? snap.lastThought : agStateObj.lastThought;
        agState["lastMessage"] = agStateObj.lastMessage.empty() ? snap.lastMessage : agStateObj.lastMessage;
        root["agentState"] = agState;

        nlohmann::json evArray = nlohmann::json::array();
        for (const auto& ev : v1Events) evArray.push_back(ev);
        root["streamEvents"] = evArray;
        if (!ds->failure.empty()) root["failure"] = ds->failure;

        ds->rootJson = std::move(root);
        ds->isJsonMode = true;
        ds->showTreeView = true;
    } else {
        std::string trimmed = outUtf8;
        size_t first = trimmed.find_first_not_of(" \t\r\n");
        if (first != std::string::npos && (trimmed[first] == '{' || trimmed[first] == '[')) {
            try {
                auto j = nlohmann::json::parse(trimmed, nullptr, false);
                if (!j.is_discarded() && (j.is_object() || j.is_array())) {
                    ds->rootJson = std::move(j);
                    ds->isJsonMode = true;
                    ds->showTreeView = true;
                }
            } catch (...) {}
        }
    }

    std::wstring title = std::wstring(Tr(L"タスク詳細", L"Task Details", L"Detalles de tarea")) + L" — #" + std::to_wstring(snap.id) + L" [" + Utf8ToWide(snap.name) + L"]";
    if (ds->isJsonMode) {
        title += L" [JSON Tree Editor]";
    }
    RECT rcParent = {};
    GetWindowRect(st->hwndMain, &rcParent);
    int dlgW = ds->isJsonMode ? 960 : 775;
    int dlgH = ds->isJsonMode ? 680 : 550;
    int x = rcParent.left + ((rcParent.right - rcParent.left) - dlgW) / 2;
    int y = rcParent.top + ((rcParent.bottom - rcParent.top) - dlgH) / 2;

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"HanchoTaskDetailsWnd",
        title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        x, y, dlgW, dlgH,
        st->hwndMain, nullptr, hInst, ds
    );
    if (hDlg) {
        ShowWindow(hDlg, SW_SHOW);
        SetForegroundWindow(hDlg);
    } else {
        delete ds;
    }
}

// ---------------------------------------------------------------------------
// Behavior tree debug view (doc/behavior-tree-plan.md 8.1). Developer-only: it opens with
// Ctrl+Shift+B and is never listed in menus or help, because users are not told about BTs.
// Reads <logs>/behaviortrees/*.json through bt_debug_view and shows each tree in a TreeView.
// ---------------------------------------------------------------------------

enum BtDebugControlId {
    IDC_BTDBG_TREE = 2101,
    IDC_BTDBG_DETAIL = 2102,
    IDC_BTDBG_STATUS = 2103,
    IDC_BTDBG_REFRESH = 2104,
    IDC_BTDBG_COPY = 2105,
    IDC_BTDBG_FOLDER = 2106,
    IDC_BTDBG_SHOWALL = 2107,
};
constexpr UINT_PTR kBtDebugTimerId = 1;
constexpr UINT kBtDebugRefreshMs = 2000;
constexpr int64_t kBtDebugRecentSec = 24 * 3600;  // finished trees older than this are hidden unless "show all"

struct BtDebugDlgState {
    DashboardState* dash = nullptr;
    HFONT hFontUi = nullptr;
    HFONT hFontMono = nullptr;
    HWND hwndLabel = nullptr;
    HWND hwndTree = nullptr;
    HWND hwndDetail = nullptr;
    HWND hwndStatus = nullptr;
    HWND hwndBtnRefresh = nullptr;
    HWND hwndBtnCopy = nullptr;
    HWND hwndBtnFolder = nullptr;
    HWND hwndBtnClose = nullptr;
    HWND hwndBtnShowAll = nullptr;
    bool showAll = false;                    // also list finished trees older than kBtDebugRecentSec
    std::unordered_set<std::string> finishedTrees;  // ids of finished trees (drawn muted)
    std::wstring dir;
    std::string signature;                  // what the tree showed last time; rebuild only on change
    std::vector<BtDebugNode> trees;
    std::vector<const BtDebugNode*> items;  // TreeView lParam -> node (points into trees)
    std::unordered_map<std::string, HTREEITEM> itemByKey;
    std::unordered_set<std::string> knownKeys;  // keys seen before: new ones start expanded
    std::string selectedKey;
    std::string pendingSelectKey;           // node to select on the next rebuild
    std::string pendingSelectJob;           // job selected in the task list when opened
    bool rebuilding = false;
};

static HWND g_btDebugWnd = nullptr;

static const BtDebugNode* BtDebugItemNode(BtDebugDlgState* ds, HTREEITEM h) {
    if (!ds || !h) return nullptr;
    TVITEMW it = {};
    it.mask = TVIF_PARAM;
    it.hItem = h;
    if (!TreeView_GetItem(ds->hwndTree, &it)) return nullptr;
    size_t idx = static_cast<size_t>(it.lParam);
    return idx < ds->items.size() ? ds->items[idx] : nullptr;
}

static std::string BtDebugJobState(BtDebugDlgState* ds, const std::string& jobId) {
    if (!ds || !ds->dash) return "";
    std::string err;
    std::string resp = TaskQueueGetIpc(ds->dash, jobId, false, 0, err);
    if (resp.find("\"ok\":true") == std::string::npos) return "missing";
    return JsonGetString(resp, "state", "");
}

// The node's own label plus the current task-queue state of its last job.
static std::wstring BtDebugLabel(BtDebugDlgState* ds, const BtDebugNode& n) {
    std::wstring label = Utf8ToWide(n.label);
    if (!n.jobIds.empty()) {
        std::string js = BtDebugJobState(ds, n.jobIds.back());
        if (!js.empty()) label += L" (" + Utf8ToWide(js) + L")";
    }
    return label;
}

static void BtDebugCollectLabels(BtDebugDlgState* ds, const BtDebugNode& n,
                                 std::unordered_map<std::string, std::wstring>& labels, std::string& sig) {
    std::wstring label = BtDebugLabel(ds, n);
    sig += n.key;
    sig += '\x1f';
    sig += WideToUtf8(label);
    sig += '\x1f';
    sig += n.detailJson;
    sig += '\x1e';
    labels[n.key] = std::move(label);
    for (const auto& c : n.children) BtDebugCollectLabels(ds, c, labels, sig);
}

static void BtDebugShowDetail(BtDebugDlgState* ds) {
    HTREEITEM h = TreeView_GetSelection(ds->hwndTree);
    const BtDebugNode* n = BtDebugItemNode(ds, h);
    if (!n) {
        SetWindowTextW(ds->hwndDetail, L"");
        return;
    }
    std::string text = n->key + "\n" + n->detailJson;
    for (const auto& jobId : n->jobIds) {
        text += "\n\n--- job " + jobId + " ---\n";
        if (!ds || !ds->dash) continue;
        std::string err;
        std::string resp = TaskQueueGetIpc(ds->dash, jobId, false, 0, err);
        auto parsed = nlohmann::json::parse(resp, nullptr, false);
        text += parsed.is_discarded() ? resp : parsed.dump(2, ' ', false, nlohmann::json::error_handler_t::replace);
    }
    SetWindowTextW(ds->hwndDetail, NormalizeCrLfWide(text).c_str());
}

static void BtDebugInsert(BtDebugDlgState* ds, HTREEITEM parent, const BtDebugNode& n,
                          const std::unordered_map<std::string, std::wstring>& labels,
                          const std::unordered_map<std::string, bool>& expanded) {
    auto lit = labels.find(n.key);
    std::wstring label = lit != labels.end() ? lit->second : Utf8ToWide(n.label);
    TVINSERTSTRUCTW tvis = {};
    tvis.hParent = parent;
    tvis.hInsertAfter = TVI_LAST;
    tvis.item.mask = TVIF_TEXT | TVIF_PARAM;
    tvis.item.pszText = const_cast<wchar_t*>(label.c_str());
    tvis.item.lParam = static_cast<LPARAM>(ds->items.size());
    ds->items.push_back(&n);
    HTREEITEM h = TreeView_InsertItem(ds->hwndTree, &tvis);
    if (!h) return;
    ds->itemByKey[n.key] = h;
    for (const auto& c : n.children) BtDebugInsert(ds, h, c, labels, expanded);
    if (n.children.empty()) return;
    auto eit = expanded.find(n.key);
    // New rows open, except a finished tree's own row, which starts closed so active trees stay in view.
    bool startClosed = n.key == n.btId && ds->finishedTrees.count(n.btId);
    bool expand = eit != expanded.end() ? eit->second : (!ds->knownKeys.count(n.key) && !startClosed);
    if (expand) TreeView_Expand(ds->hwndTree, h, TVE_EXPAND);
}

static void BtDebugSaveExpanded(BtDebugDlgState* ds, HTREEITEM h, std::unordered_map<std::string, bool>& out) {
    for (; h; h = TreeView_GetNextSibling(ds->hwndTree, h)) {
        if (const BtDebugNode* n = BtDebugItemNode(ds, h)) {
            out[n->key] = (TreeView_GetItemState(ds->hwndTree, h, TVIS_EXPANDED) & TVIS_EXPANDED) != 0;
        }
        BtDebugSaveExpanded(ds, TreeView_GetChild(ds->hwndTree, h), out);
    }
}

static bool BtDebugIsFinished(const std::string& state) {
    return state == "succeeded" || state == "failed" || state == "cancelled";
}

// UTC "YYYY-MM-DDTHH:MM:SS" (the format of the state files' updatedAt) for now - ageSec.
static std::string BtDebugUtcStamp(int64_t ageSec) {
    std::time_t t = std::time(nullptr) - static_cast<std::time_t>(ageSec);
    std::tm tmUtc = {};
    gmtime_s(&tmUtc, &t);
    char buf[32] = {};
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tmUtc);
    return buf;
}

static void BtDebugReload(BtDebugDlgState* ds, bool force) {
    std::vector<std::string> errors;
    std::vector<BtDebugNode> loaded = LoadBtDebugTrees(ds->dir, errors);  // newest first
    std::string keepTree;  // a tree the caller asked to see is shown even when it would be hidden
    if (!ds->pendingSelectJob.empty()) {
        std::string key = FindBtNodeKeyByJob(loaded, ds->pendingSelectJob);
        if (!key.empty()) {
            ds->pendingSelectKey = key;
            keepTree = key.substr(0, key.find('/'));
        }
        ds->pendingSelectJob.clear();
        force = force || !ds->pendingSelectKey.empty();
    }

    // Active trees (running, waiting for a decision, paused) first, then finished ones. Finished
    // trees not updated for kBtDebugRecentSec are hidden unless "show all" is on.
    const std::string cutoff = BtDebugUtcStamp(kBtDebugRecentSec);
    std::vector<BtDebugNode> trees, finished;
    std::unordered_set<std::string> finishedIds;
    int hidden = 0;
    for (auto& t : loaded) {
        if (!BtDebugIsFinished(t.state)) {
            trees.push_back(std::move(t));
            continue;
        }
        auto j = nlohmann::json::parse(t.detailJson, nullptr, false);
        std::string updated = (j.is_object() && j.contains("updatedAt") && j["updatedAt"].is_string())
                                  ? j["updatedAt"].get<std::string>() : "";
        if (!ds->showAll && t.btId != keepTree && updated < cutoff) {
            hidden++;
            continue;
        }
        finishedIds.insert(t.btId);
        finished.push_back(std::move(t));
    }
    size_t activeCount = trees.size();
    for (auto& t : finished) trees.push_back(std::move(t));

    std::unordered_map<std::string, std::wstring> labels;
    std::string sig = ds->showAll ? "all\x1e" : "recent\x1e";
    for (const auto& t : trees) BtDebugCollectLabels(ds, t, labels, sig);

    std::wstringstream status;
    status << Tr(L"実行中 ", L"Active ", L"Activos ") << activeCount
           << Tr(L"  終了 ", L"  Finished ", L"  Finalizados ") << (trees.size() - activeCount);
    if (hidden > 0) {
        status << Tr(L"  (24 時間より前の終了分 ", L"  (finished over 24h ago hidden: ",
                     L"  (finalizados hace más de 24 h ocultos: ")
               << hidden << Tr(L" 件は非表示)", L")", L")");
    }
    status << L"   " << ds->dir;
    for (const auto& e : errors) status << L"   ! " << Utf8ToWide(e);
    SetWindowTextW(ds->hwndStatus, status.str().c_str());

    if (!force && sig == ds->signature) return;
    ds->signature = std::move(sig);

    std::unordered_map<std::string, bool> expanded;
    BtDebugSaveExpanded(ds, TreeView_GetRoot(ds->hwndTree), expanded);
    std::string selectKey = !ds->pendingSelectKey.empty() ? ds->pendingSelectKey : ds->selectedKey;
    bool revealSelection = !ds->pendingSelectKey.empty();
    ds->pendingSelectKey.clear();

    int scrollPos = GetScrollPos(ds->hwndTree, SB_VERT);
    ds->rebuilding = true;
    SendMessageW(ds->hwndTree, WM_SETREDRAW, FALSE, 0);
    TreeView_DeleteAllItems(ds->hwndTree);
    ds->items.clear();
    ds->itemByKey.clear();
    ds->trees = std::move(trees);
    ds->finishedTrees = std::move(finishedIds);
    for (const auto& t : ds->trees) BtDebugInsert(ds, TVI_ROOT, t, labels, expanded);
    for (const auto& kv : ds->itemByKey) ds->knownKeys.insert(kv.first);

    auto sit = ds->itemByKey.find(selectKey);
    if (sit == ds->itemByKey.end() && selectKey.empty() && !ds->trees.empty()) {
        sit = ds->itemByKey.find(ds->trees.front().key);  // nothing chosen yet: show the newest tree
    }
    if (sit != ds->itemByKey.end()) {
        selectKey = sit->first;
        TreeView_SelectItem(ds->hwndTree, sit->second);
        if (revealSelection) TreeView_EnsureVisible(ds->hwndTree, sit->second);
        ds->selectedKey = selectKey;
    } else {
        ds->selectedKey.clear();
    }
    if (!revealSelection) {
        // Keep the user's scroll position: put the same line back at the top.
        HTREEITEM first = TreeView_GetRoot(ds->hwndTree);
        for (int i = 0; i < scrollPos && first; ++i) first = TreeView_GetNextVisible(ds->hwndTree, first);
        if (first) TreeView_SelectSetFirstVisible(ds->hwndTree, first);
    }
    SendMessageW(ds->hwndTree, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(ds->hwndTree, nullptr, TRUE);
    ds->rebuilding = false;
    BtDebugShowDetail(ds);
}

static COLORREF BtDebugStateColor(const std::string& state, bool& has) {
    has = true;
    if (state == "running") return g_theme.rowRunning;
    if (state == "failed" || state == "cancelled") return g_theme.rowFailed;
    if (state == "succeeded") return g_theme.rowCompleted;
    if (state == "waitingForLlm" || state == "paused") return g_theme.rowIdle;
    has = false;
    return g_theme.panel;
}

static void BtDebugLayout(HWND hwnd, BtDebugDlgState* ds) {
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    const int pad = 12;
    int innerW = (std::max)(200, w - pad * 2);
    int btnY = (std::max)(160, h - pad - 30);
    int statusY = btnY - 26;
    int mainY = pad + 22;
    int mainH = (std::max)(80, statusY - mainY - 6);
    int treeW = innerW * 58 / 100;
    MoveWindow(ds->hwndLabel, pad, pad, innerW, 18, TRUE);
    MoveWindow(ds->hwndTree, pad, mainY, treeW, mainH, TRUE);
    MoveWindow(ds->hwndDetail, pad + treeW + 8, mainY, innerW - treeW - 8, mainH, TRUE);
    MoveWindow(ds->hwndStatus, pad, statusY, innerW, 20, TRUE);
    int x = pad;
    MoveWindow(ds->hwndBtnRefresh, x, btnY, 120, 30, TRUE);
    x += 126;
    MoveWindow(ds->hwndBtnCopy, x, btnY, 140, 30, TRUE);
    x += 146;
    MoveWindow(ds->hwndBtnFolder, x, btnY, 140, 30, TRUE);
    x += 146;
    MoveWindow(ds->hwndBtnShowAll, x, btnY, 150, 30, TRUE);
    x += 6;
    MoveWindow(ds->hwndBtnClose, (std::max)(x + 150, w - pad - 100), btnY, 100, 30, TRUE);
}

static LRESULT CALLBACK BtDebugWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ds = reinterpret_cast<BtDebugDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ds = reinterpret_cast<BtDebugDlgState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ds));

            HDC hdc = GetDC(hwnd);
            int fontSize = -MulDiv(9, GetDeviceCaps(hdc, LOGPIXELSY), 72);
            ReleaseDC(hwnd, hdc);
            ds->hFontMono = CreateFontW(fontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                FIXED_PITCH | FF_MODERN, L"Consolas");

            ds->hwndLabel = CreateWindowExW(0, L"STATIC",
                Tr(L"ビヘイビアツリー — 各タスクを起動したノードとツリーの進み具合",
                   L"Behavior trees - the nodes that started each task and how far each tree has run",
                   L"Árboles de comportamiento - nodos que iniciaron cada tarea y progreso"),
                WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, hwnd, nullptr, nullptr, nullptr);
            ds->hwndTree = CreateWindowExW(WS_EX_CLIENTEDGE, WC_TREEVIEWW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS |
                TVS_SHOWSELALWAYS | TVS_FULLROWSELECT,
                0, 0, 10, 10, hwnd, (HMENU)IDC_BTDBG_TREE, nullptr, nullptr);
            TreeView_SetBkColor(ds->hwndTree, g_theme.panel);
            TreeView_SetTextColor(ds->hwndTree, g_theme.listText);
            TreeView_SetLineColor(ds->hwndTree, g_theme.border);
            ds->hwndDetail = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_AUTOVSCROLL |
                ES_AUTOHSCROLL | ES_READONLY,
                0, 0, 10, 10, hwnd, (HMENU)IDC_BTDBG_DETAIL, nullptr, nullptr);
            ds->hwndStatus = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS | SS_NOPREFIX,
                0, 0, 10, 10, hwnd, (HMENU)IDC_BTDBG_STATUS, nullptr, nullptr);
            ds->hwndBtnRefresh = CreateWindowExW(0, L"BUTTON", Tr(L"再読み込み", L"Reload", L"Recargar"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd, (HMENU)IDC_BTDBG_REFRESH, nullptr, nullptr);
            ds->hwndBtnCopy = CreateWindowExW(0, L"BUTTON", Tr(L"詳細をコピー", L"Copy Details", L"Copiar detalles"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd, (HMENU)IDC_BTDBG_COPY, nullptr, nullptr);
            ds->hwndBtnFolder = CreateWindowExW(0, L"BUTTON", Tr(L"フォルダーを開く", L"Open Folder", L"Abrir carpeta"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd, (HMENU)IDC_BTDBG_FOLDER, nullptr, nullptr);
            ds->hwndBtnShowAll = CreateWindowExW(0, L"BUTTON", Tr(L"☐ すべて表示", L"☐ Show all", L"☐ Mostrar todo"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd, (HMENU)IDC_BTDBG_SHOWALL, nullptr, nullptr);
            ds->hwndBtnClose = CreateWindowExW(0, L"BUTTON", Tr(L"閉じる", L"Close", L"Cerrar"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 10, 10, hwnd, (HMENU)IDOK, nullptr, nullptr);

            SendMessageW(ds->hwndLabel, WM_SETFONT, (WPARAM)(ds->dash ? ds->dash->hFontUiBold : nullptr), TRUE);
            HWND uiCtrls[] = {ds->hwndTree, ds->hwndStatus, ds->hwndBtnRefresh, ds->hwndBtnCopy, ds->hwndBtnFolder,
                              ds->hwndBtnShowAll, ds->hwndBtnClose};
            for (HWND h : uiCtrls) SendMessageW(h, WM_SETFONT, (WPARAM)ds->hFontUi, TRUE);
            SendMessageW(ds->hwndDetail, WM_SETFONT, (WPARAM)(ds->hFontMono ? ds->hFontMono : ds->hFontUi), TRUE);

            BtDebugLayout(hwnd, ds);
            BtDebugReload(ds, true);
            SetTimer(hwnd, kBtDebugTimerId, kBtDebugRefreshMs, nullptr);
            ApplyNativeThemeToWindow(hwnd);
            return 0;
        }
        case WM_TIMER:
            if (ds && wParam == kBtDebugTimerId) BtDebugReload(ds, false);
            return 0;
        case WM_NOTIFY: {
            auto* pnmh = reinterpret_cast<NMHDR*>(lParam);
            if (!ds || !pnmh || pnmh->hwndFrom != ds->hwndTree) break;
            if (pnmh->code == TVN_SELCHANGEDW && !ds->rebuilding) {
                const BtDebugNode* n = BtDebugItemNode(ds, TreeView_GetSelection(ds->hwndTree));
                ds->selectedKey = n ? n->key : "";
                BtDebugShowDetail(ds);
                return 0;
            }
            if (pnmh->code == NM_CUSTOMDRAW) {
                auto* cd = reinterpret_cast<NMTVCUSTOMDRAW*>(lParam);
                if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
                if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                    size_t idx = static_cast<size_t>(cd->nmcd.lItemlParam);
                    if (idx < ds->items.size() && !(cd->nmcd.uItemState & CDIS_SELECTED) &&
                        ds->finishedTrees.count(ds->items[idx]->btId)) {
                        cd->clrText = g_theme.muted;  // finished trees are shown faded
                        cd->clrTextBk = g_theme.panel;
                    } else if (idx < ds->items.size() && !(cd->nmcd.uItemState & CDIS_SELECTED)) {
                        bool has = false;
                        COLORREF bg = BtDebugStateColor(ds->items[idx]->state, has);
                        if (has) {
                            cd->clrTextBk = bg;
                            cd->clrText = g_theme.listText;
                        }
                    }
                    return CDRF_DODEFAULT;
                }
            }
            break;
        }
        case WM_SIZE:
            if (ds) BtDebugLayout(hwnd, ds);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 640;
            mmi->ptMinTrackSize.y = 360;
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (!ds) break;
            if (id == IDC_BTDBG_REFRESH) {
                BtDebugReload(ds, true);
            } else if (id == IDC_BTDBG_COPY) {
                int len = GetWindowTextLengthW(ds->hwndDetail);
                std::wstring text(static_cast<size_t>(len) + 1, L'\0');
                GetWindowTextW(ds->hwndDetail, &text[0], len + 1);
                text.resize(static_cast<size_t>(len));
                if (!text.empty()) CopyTextToClipboard(hwnd, text);
            } else if (id == IDC_BTDBG_SHOWALL) {
                ds->showAll = !ds->showAll;
                SetWindowTextW(ds->hwndBtnShowAll, ds->showAll ? Tr(L"☑ すべて表示", L"☑ Show all", L"☑ Mostrar todo")
                                                               : Tr(L"☐ すべて表示", L"☐ Show all", L"☐ Mostrar todo"));
                InvalidateRect(ds->hwndBtnShowAll, nullptr, TRUE);
                BtDebugReload(ds, true);
            } else if (id == IDC_BTDBG_FOLDER) {
                CreateDirectoryW(ds->dir.c_str(), nullptr);
                gui_platform::GetGuiPlatform()->OpenFolder(hwnd, ds->dir);
            } else if (id == IDOK || id == IDCANCEL) {
                DestroyWindow(hwnd);
            }
            return 0;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis || !ds) break;
            bool pressed = (dis->itemState & ODS_SELECTED) != 0;
            HBRUSH bgBrush = CreateSolidBrush(pressed ? g_theme.border : g_theme.panel);
            FillRect(dis->hDC, &dis->rcItem, bgBrush);
            DeleteObject(bgBrush);
            HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
            HGDIOBJ oldPen = SelectObject(dis->hDC, borderPen);
            HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
            Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
            SelectObject(dis->hDC, oldBrush);
            SelectObject(dis->hDC, oldPen);
            DeleteObject(borderPen);
            if (dis->itemState & ODS_FOCUS) {
                RECT rcFocus = dis->rcItem;
                InflateRect(&rcFocus, -2, -2);
                DrawFocusRect(dis->hDC, &rcFocus);
            }
            wchar_t text[256] = {};
            GetWindowTextW(dis->hwndItem, text, 256);
            RECT rc = dis->rcItem;
            InflateRect(&rc, -4, 0);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, g_theme.text);
            HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC, ds->hFontUi));
            DrawTextW(dis->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dis->hDC, oldFont);
            return TRUE;
        }
        case WM_ERASEBKGND: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, g_dialogBgBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, g_theme.text);
            SetBkColor(hdc, g_theme.panel);
            return reinterpret_cast<LRESULT>(g_dialogBgBrush);
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_DESTROY:
            KillTimer(hwnd, kBtDebugTimerId);
            if (g_btDebugWnd == hwnd) g_btDebugWnd = nullptr;
            return 0;
        case WM_NCDESTROY:
            if (ds) {
                if (ds->hFontMono) DeleteObject(ds->hFontMono);
                delete ds;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// Opens the debug view (or brings it forward). When a task-list row is selected, its job's node
// is selected in the tree.
static void ShowBtDebugDialog(DashboardState* st) {
    if (!st) return;
    std::string jobId;
    if (st->hwndTaskList) {
        int idx = ListView_GetNextItem(st->hwndTaskList, -1, LVNI_SELECTED);
        if (idx >= 0 && idx < static_cast<int>(st->taskJobIds.size())) jobId = st->taskJobIds[idx];
    }

    if (g_btDebugWnd && IsWindow(g_btDebugWnd)) {
        auto* ds = reinterpret_cast<BtDebugDlgState*>(GetWindowLongPtrW(g_btDebugWnd, GWLP_USERDATA));
        if (ds && !jobId.empty()) {
            ds->pendingSelectJob = jobId;
            BtDebugReload(ds, false);
        }
        if (IsIconic(g_btDebugWnd)) ShowWindow(g_btDebugWnd, SW_RESTORE);
        SetForegroundWindow(g_btDebugWnd);
        return;
    }

    static bool regDone = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!regDone) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = BtDebugWndProc;
        wc.hInstance = hInst;
        wc.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"HanchoBtDebugWnd";
        RegisterClassExW(&wc);
        regDone = true;
    }

    auto* ds = new BtDebugDlgState();
    ds->dash = st;
    ds->hFontUi = st->hFontUi;
    ds->dir = GetBehaviorTreesDirWide();
    ds->pendingSelectJob = jobId;

    RECT rcParent = {};
    GetWindowRect(st->hwndMain, &rcParent);
    int dlgW = 1100;
    int dlgH = 700;
    int x = rcParent.left + ((rcParent.right - rcParent.left) - dlgW) / 2;
    int y = rcParent.top + ((rcParent.bottom - rcParent.top) - dlgH) / 2;
    HWND hDlg = CreateWindowExW(0, L"HanchoBtDebugWnd",
        Tr(L"ビヘイビアツリー", L"Behavior Trees", L"Árboles de comportamiento"),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, x, y, dlgW, dlgH, st->hwndMain, nullptr, hInst, ds);
    if (hDlg) {
        g_btDebugWnd = hDlg;
        SetForegroundWindow(hDlg);
    } else {
        delete ds;
    }
}

// One plain-language step of the agent, merged from consecutive stream events.
struct ActivityRow {
    std::string kind;
    std::string target;
    std::string description;  // the tool call's own "description", shown in its own column
    int64_t startMs = 0;
    int64_t endMs = 0;
    int firstEvent = 0;
    int lastEvent = 0;
    bool ongoing = false;
    uint64_t tokIn = 0;      // input tokens used by this step's events
    uint64_t tokOut = 0;     // output tokens
    uint64_t tokCache = 0;   // cache read + write tokens
    uint64_t tokThink = 0;   // thinking tokens
    uint64_t tokAll = 0;     // sub-agent row: its total, which has no split into the other four
};

// One normalized hancho.event/1 event, shaped for the monitor pane. The pane reads only these,
// from <log>.events.jsonl; the internal LlmStreamEvent type stays in the stream layer.
struct MonitorEvent {
    uint64_t seq = 0;
    int64_t recvEpochMs = 0;
    std::string timestamp;
    std::string kind;
    std::string agentType;
    std::string model;
    std::string agentId;
    std::string toolUseId;
    std::string toolName;
    std::string activity;
    std::string target;
    std::string note;
    std::string text;        // thought / message / error / raw text
    std::string toolInput;   // data.input, serialized
    std::string planTarget;  // kind == "plan": "2/5 done: ..."
    std::string rawJson;
    nlohmann::json data = nlohmann::json::object();
    uint64_t inputTokens = 0;
    uint64_t outputTokens = 0;
    uint64_t thinkingTokens = 0;
    uint64_t cacheReadTokens = 0;
    uint64_t cacheWriteTokens = 0;
    uint64_t totalTokens = 0;
    double costUsd = 0.0;
};

struct LlmStreamDlgState {
    DashboardState* dash = nullptr;
    int procId = 0;
    ProcessSnapshot snap;
    std::vector<MonitorEvent> events;
    std::vector<ActivityRow> rows;     // flat steps, oldest first: the time chart reads these
    std::vector<ActivityTreeRow> treeRows;  // what the list shows, in display order (see BuildMonitorTreeRows)
    ActivityTreeOverrides treeOverrides;    // branches the user opened (true) or closed (false), by sub-agent id
    int overridesProcId = 0;                // process the overrides belong to
    std::string listSig;                    // signature of the rows the list shows; a change rebuilds it
    bool treeMode = false;                  // sub-agents present: time-ordered tree instead of newest-first
    bool rebuilding = false;                // true while the list is being refilled (ignore its notifications)
    std::string goalJobKey;            // cache of the task queue job used for the goal / end condition
    std::string goalJobSummary;
    std::wstring goalJobSuccess;       // "exit 0, output /re/ ..."
    std::wstring goalJobResources;     // "write: ... / read: ..." or "(none)"
    std::wstring goalJobMemoryRefs;    // ids of the memory records injected into this attempt
    int goalJobMaxAttempts = 0;
    int goalJobAttempt = -1;
    LlmAgentState agentState;
    HFONT hFontUi = nullptr;
    HFONT hFontBold = nullptr;
    HFONT hFontMono = nullptr;
    HWND hwndHeader = nullptr;
    HWND hwndList = nullptr;
    HWND hwndEditPreview = nullptr;
    HWND hwndAgentIcon = nullptr;
    HWND hwndTooltip = nullptr;
    UINT_PTR timerId = 0;
    size_t lastEventCount = 0;
    uint64_t lastEventSeq = 0;
    RECT rcUsageGraph = {};
    RECT rcKindChart = {};
    bool embedded = false;   // bottom pane of the main window (follows the selected process)
    std::shared_ptr<pc::remote::WebSocketClient> wsClient;
    std::string subscribedJobId;
    int subscribedAttempt = 0;

    ~LlmStreamDlgState() {
        if (wsClient) {
            wsClient->Close();
            wsClient.reset();
        }
    }
};

struct UsageBar {
    RECT rc = {};
    int eventIndex = -1;
    uint64_t inputTokens = 0;
    uint64_t outputTokens = 0;
    uint64_t thinkingTokens = 0;
    uint64_t cacheTokens = 0;
};

// The run's time window. The usage graph and the time-by-kind chart both map this window onto
// the same horizontal axis, so their x positions line up.
struct TimeSpan {
    int64_t startMs = 0;
    int64_t lengthMs = 1000;
};

// ISO 8601 UTC ("2026-11-10T09:12:33.120Z") to epoch milliseconds.
static int64_t ParseIsoUtcMs(const std::string& ts) {
    if (ts.size() < 20) return 0;
    std::tm tmv = {};
    int millis = 0;
    if (std::sscanf(ts.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d.%3dZ", &tmv.tm_year, &tmv.tm_mon,
                    &tmv.tm_mday, &tmv.tm_hour, &tmv.tm_min, &tmv.tm_sec, &millis) < 6) {
        return 0;
    }
    tmv.tm_year -= 1900;
    tmv.tm_mon -= 1;
    int64_t sec = static_cast<int64_t>(_mkgmtime64(&tmv));
    return sec * 1000 + millis;
}

static std::string MonitorClipUtf8(const std::string& s, size_t maxBytes) {
    if (s.size() <= maxBytes) return s;
    size_t n = maxBytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return s.substr(0, n);
}

// "2/5 done: fixing tests" from a v1 plan event's data.items.
static std::string PlanTargetFromItems(const nlohmann::json& items) {
    if (!items.is_array()) return "";
    int done = 0;
    std::string current;
    for (const auto& item : items) {
        if (!item.is_object()) continue;
        std::string status = item.value("status", std::string());
        if (status == "completed") {
            ++done;
        } else if (status == "in_progress" && current.empty()) {
            current = item.value("content", std::string());
        }
    }
    std::string out = std::to_string(done) + "/" + std::to_string(items.size()) + " done";
    if (!current.empty()) out += ": " + current;
    return out;
}

static MonitorEvent MonitorEventFromJson(const nlohmann::json& j) {
    MonitorEvent ev;
    ev.rawJson = j.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    ev.seq = j.value("seq", uint64_t{0});
    if (j.contains("ts") && j["ts"].is_string()) {
        ev.timestamp = j["ts"].get<std::string>();
        ev.recvEpochMs = ParseIsoUtcMs(ev.timestamp);
    }
    ev.kind = j.value("kind", std::string());
    if (j.contains("agent") && j["agent"].is_object()) {
        const auto& a = j["agent"];
        ev.agentType = a.value("type", std::string());
        ev.model = a.value("model", std::string());
        ev.agentId = a.value("agentId", std::string());
    }
    if (j.contains("data") && j["data"].is_object()) {
        const auto& d = j["data"];
        ev.data = d;
        if (d.contains("tool") && d["tool"].is_string()) ev.toolName = d["tool"].get<std::string>();
        if (d.contains("activity") && d["activity"].is_string()) ev.activity = d["activity"].get<std::string>();
        if (d.contains("target") && d["target"].is_string()) ev.target = d["target"].get<std::string>();
        if (d.contains("note") && d["note"].is_string()) ev.note = d["note"].get<std::string>();
        if (d.contains("toolUseId") && d["toolUseId"].is_string()) ev.toolUseId = d["toolUseId"].get<std::string>();
        if (d.contains("input")) ev.toolInput = d["input"].dump();
        if (d.contains("text") && d["text"].is_string()) ev.text = d["text"].get<std::string>();
        if (d.contains("message") && d["message"].is_string()) ev.text = d["message"].get<std::string>();
        if (d.contains("summary") && d["summary"].is_string()) ev.text = d["summary"].get<std::string>();
        if (d.contains("error") && d["error"].is_string()) ev.text = d["error"].get<std::string>();
        if (d.contains("items")) ev.planTarget = PlanTargetFromItems(d["items"]);
        ev.inputTokens = d.value("inputTokens", uint64_t{0});
        ev.outputTokens = d.value("outputTokens", uint64_t{0});
        ev.thinkingTokens = d.value("thinkingTokens", uint64_t{0});
        ev.cacheReadTokens = d.value("cacheReadTokens", uint64_t{0});
        ev.cacheWriteTokens = d.value("cacheWriteTokens", uint64_t{0});
        ev.totalTokens = d.value("totalTokens", uint64_t{0});
        ev.costUsd = d.value("costUsd", 0.0);
    }
    return ev;
}

// The monitor pane's only event source: the normalized .events.jsonl for the process.
static void LoadMonitorEvents(const ProcessSnapshot& snap, std::vector<MonitorEvent>& out) {
    out.clear();
    if (snap.streamFilePath.empty()) return;
    std::string eventsPath = HanchoEventsFilePathForStream(snap.streamFilePath);
    if (eventsPath.empty()) {
        eventsPath = snap.streamFilePath;
    }
    auto raw = ReadHanchoEventsFile(eventsPath, 300, "", 0);
    out.reserve(raw.size());
    for (const auto& j : raw) out.push_back(MonitorEventFromJson(j));
}

static void UpdateAgentStateFromMonitorEvents(const std::vector<MonitorEvent>& events,
                                             LlmAgentState& state, bool running) {
    state = LlmAgentState();
    state.status = running ? "running" : "idle";
    std::unordered_map<std::string, LlmSubagentState> subMap;

    for (const auto& ev : events) {
        if (!ev.agentType.empty()) state.agentType = ev.agentType;
        if (!ev.model.empty()) state.model = ev.model;
        if (ev.totalTokens > 0) state.totalTokens += ev.totalTokens;
        else state.totalTokens += (ev.inputTokens + ev.outputTokens + ev.thinkingTokens + ev.cacheReadTokens + ev.cacheWriteTokens);
        if (ev.costUsd > 0.0) state.costUsd += ev.costUsd;

        if (ev.kind == "tool.call") {
            state.toolCalls++;
            state.currentTool = ev.toolName;
            state.currentToolAction = ev.activity;
            state.activity = ev.activity.empty() ? "tool" : ev.activity;
            state.activityTarget = ev.target;
            state.activitySinceEpochMs = ev.recvEpochMs > 0 ? ev.recvEpochMs : NowEpochMs();
            state.status = "calling_tool";
        } else if (ev.kind == "thought") {
            state.lastThought = ev.text;
            state.activity = "thinking";
            state.activityTarget = ev.text;
            state.activitySinceEpochMs = ev.recvEpochMs > 0 ? ev.recvEpochMs : NowEpochMs();
            state.status = "thinking";
        } else if (ev.kind == "message") {
            state.lastMessage = ev.text;
            state.activity = "replying";
            state.activityTarget = ev.text;
            state.activitySinceEpochMs = ev.recvEpochMs > 0 ? ev.recvEpochMs : NowEpochMs();
            state.status = "generating";
        } else if (ev.kind == "error") {
            state.lastError = ev.text;
            state.activity = "error";
            state.activityTarget = ev.text;
            state.status = "error";
        } else if (ev.kind == "run.ended") {
            state.status = "completed";
            state.activity = "done";
        }

        if (!ev.agentId.empty()) {
            auto& sub = subMap[ev.agentId];
            sub.id = ev.agentId;
            if (sub.startedEpochMs == 0) sub.startedEpochMs = ev.recvEpochMs > 0 ? ev.recvEpochMs : NowEpochMs();
            if (ev.totalTokens > 0) sub.totalTokens += ev.totalTokens;
            if (ev.kind == "tool.call") {
                sub.toolUses++;
                sub.activity = ev.activity;
                sub.activityTarget = ev.target;
            }
            if (ev.kind == "run.ended") {
                sub.status = "completed";
                sub.endedEpochMs = ev.recvEpochMs;
            }
        }
    }
    state.stepCount = state.toolCalls;
    state.subagents.clear();
    for (auto& kv : subMap) {
        state.subagents.push_back(kv.second);
    }
}

// Internal bridge: the sub-agent tree builder still consumes LlmStreamEvent. Used only when the
// run has sub-agents, and never to read the .stream.jsonl file.
static LlmStreamEvent ToInternalStreamEvent(const MonitorEvent& m) {
    LlmStreamEvent ev;
    ev.seq = m.seq;
    ev.recvEpochMs = m.recvEpochMs;
    ev.timestamp = m.timestamp;
    ev.agentType = m.agentType;
    ev.model = m.model;
    ev.agentId = m.agentId;
    ev.toolUseId = m.toolUseId;
    ev.inputTokens = m.inputTokens;
    ev.outputTokens = m.outputTokens;
    ev.thinkingTokens = m.thinkingTokens;
    ev.cacheReadTokens = m.cacheReadTokens;
    ev.cacheWriteTokens = m.cacheWriteTokens;
    ev.totalTokens = m.totalTokens;
    ev.costUsd = m.costUsd;
    if (m.kind == "tool.call") {
        ev.type = "tool_call";
        ev.toolName = m.toolName;
        ev.toolInput = m.toolInput;
    } else if (m.kind == "plan") {
        ev.type = "tool_call";
        ev.toolName = "TodoWrite";
        nlohmann::json todos = nlohmann::json::object();
        todos["todos"] = m.data.value("items", nlohmann::json::array());
        ev.toolInput = todos.dump();
    } else if (m.kind == "tool.result") {
        ev.type = "tool_result";
        ev.content = m.text;
    } else if (m.kind == "thought") {
        ev.type = "thinking";
        ev.content = m.text;
    } else if (m.kind == "message") {
        ev.type = "message";
        ev.content = m.text;
    } else if (m.kind == "run.ended") {
        ev.type = "result";
    } else if (m.kind == "error") {
        ev.type = "error";
        ev.content = m.text;
    } else if (m.kind == "usage") {
        ev.type = "step";
    } else {
        ev.type = "other";
    }
    return ev;
}

// Activity kind/target for a v1 event: the same plain-language steps as DescribeLlmActivity.
static bool DescribeMonitorActivity(const MonitorEvent& ev, std::string& kind, std::string& target) {
    kind.clear();
    target.clear();
    if (ev.kind == "tool.call") {
        kind = ev.activity.empty() ? "tool" : ev.activity;
        target = ev.target;
    } else if (ev.kind == "plan") {
        kind = "plan";
        target = ev.planTarget;
    } else if (ev.kind == "tool.result") {
        kind = "waiting_model";
    } else if (ev.kind == "thought") {
        kind = "thinking";
        target = ev.text;
    } else if (ev.kind == "message") {
        if (ev.text.empty()) return false;
        kind = "replying";
        target = ev.text;
    } else if (ev.kind == "error") {
        kind = "error";
        target = ev.text;
    } else if (ev.kind == "run.ended") {
        kind = "done";
    } else {
        return false;
    }
    for (char& c : target) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    }
    target = MonitorClipUtf8(target, 300);
    return true;
}

// The same compact usage line as LlmEventUsageText, over a normalized event.
static std::string MonitorEventUsageText(const MonitorEvent& ev) {
    std::ostringstream o;
    auto sep = [&]() { if (!o.str().empty()) o << " / "; };
    auto addCount = [&](const char* label, uint64_t v) {
        if (v == 0) return;
        sep();
        o << label << " " << v;
    };
    addCount("in", ev.inputTokens);
    addCount("out", ev.outputTokens);
    addCount("think", ev.thinkingTokens);
    if (ev.inputTokens == 0 && ev.outputTokens == 0 && ev.thinkingTokens == 0) addCount("tok", ev.totalTokens);
    addCount("cacheR", ev.cacheReadTokens);
    addCount("cacheW", ev.cacheWriteTokens);
    if (ev.costUsd > 0.0) {
        sep();
        char buf[32];
        snprintf(buf, sizeof(buf), "$%.4f", ev.costUsd);
        o << buf;
    }
    return o.str();
}

static TimeSpan ComputeRunSpan(const LlmStreamDlgState* ds) {
    TimeSpan span;
    int64_t start = ds->snap.startEpochSec > 0 ? ds->snap.startEpochSec * 1000
                  : (ds->events.empty() ? 0 : ds->events.front().recvEpochMs);
    int64_t end = start;
    if (ds->snap.state == ProcState::Running) end = NowEpochMs();
    else if (ds->snap.endEpochSec > 0) end = ds->snap.endEpochSec * 1000;
    else if (!ds->events.empty()) end = ds->events.back().recvEpochMs;
    span.startMs = start;
    span.lengthMs = (std::max<int64_t>)(end - start, 1000);
    return span;
}

// Horizontal extent shared by the usage graph and the time-by-kind chart (labels sit left of it).
static void TimeAxisExtent(const RECT& rc, int& left, int& right) {
    left = rc.left + 56;
    right = rc.right - 8;
}

static std::vector<UsageBar> BuildUsageBars(const RECT& rc, const std::vector<MonitorEvent>& events, const TimeSpan& span) {
    std::vector<UsageBar> bars;
    if (rc.right <= rc.left || rc.bottom <= rc.top) return bars;

    struct Value {
        int index;
        uint64_t in;
        uint64_t out;
        uint64_t think;
        uint64_t cache;
    };
    std::vector<Value> vals;
    uint64_t maxVal = 0;
    for (int i = 0; i < static_cast<int>(events.size()); ++i) {
        const auto& ev = events[i];
        uint64_t in = ev.inputTokens;
        uint64_t out = ev.outputTokens;
        uint64_t think = ev.thinkingTokens;
        uint64_t cache = ev.cacheReadTokens + ev.cacheWriteTokens;
        if (in == 0 && out == 0 && think == 0 && cache == 0 && ev.totalTokens > 0) in = ev.totalTokens;
        uint64_t sum = in + out + think + cache;
        if (sum == 0) continue;
        vals.push_back({i, in, out, think, cache});
        if (sum > maxVal) maxVal = sum;
    }
    if (vals.empty() || maxVal == 0) return bars;

    int left = 0;
    int right = 0;
    TimeAxisExtent(rc, left, right);
    int top = rc.top + 18;
    int bottom = rc.bottom - 4;
    int plotW = right - left;
    int plotH = bottom - top;
    if (plotW <= 0 || plotH <= 0) return bars;

    // Each bar sits at the time its event arrived, so the x axis is the run's elapsed time.
    const int barW = 6;
    for (int k = 0; k < static_cast<int>(vals.size()); ++k) {
        int64_t offset = events[vals[k].index].recvEpochMs - span.startMs;
        int cx = left + static_cast<int>(plotW * offset / span.lengthMs);
        cx = (std::clamp)(cx, left, right);
        int x0 = (std::clamp)(cx - barW / 2, left, right - barW);
        int x1 = x0 + barW;
        UsageBar b;
        b.rc = {x0, top, x1, bottom};
        b.eventIndex = vals[k].index;
        b.inputTokens = vals[k].in;
        b.outputTokens = vals[k].out;
        b.thinkingTokens = vals[k].think;
        b.cacheTokens = vals[k].cache;
        bars.push_back(b);
    }
    return bars;
}

// Mix two colours: percentTo = 0 gives `from`, 100 gives `to`.
static COLORREF BlendColor(COLORREF from, COLORREF to, int percentTo) {
    auto mix = [&](int a, int b) { return (a * (100 - percentTo) + b * percentTo) / 100; };
    return RGB(mix(GetRValue(from), GetRValue(to)), mix(GetGValue(from), GetGValue(to)),
               mix(GetBValue(from), GetBValue(to)));
}

static void DrawUsageGraph(HDC hdc, const RECT& rc, const std::vector<MonitorEvent>& events,
                           const MonitorEvent* selected, int selectedIndex, const TimeSpan& span) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    const COLORREF softLine = BlendColor(g_theme.panel, g_theme.border, 60);

    HBRUSH bg = CreateSolidBrush(g_theme.panel);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    HPEN frame = CreatePen(PS_SOLID, 1, softLine);
    HGDIOBJ oldPen = SelectObject(hdc, frame);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 10, 10);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(frame);

    auto bars = BuildUsageBars(rc, events, span);
    SetBkMode(hdc, TRANSPARENT);
    RECT titleRc = {rc.left + 8, rc.top + 4, rc.right - 8, rc.top + 18};
    SetTextColor(hdc, g_theme.muted);
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, GetStockObject(DEFAULT_GUI_FONT)));
    DrawTextW(hdc, L"Usage", -1, &titleRc, DT_LEFT | DT_SINGLELINE | DT_END_ELLIPSIS);

    if (bars.empty()) {
        RECT emptyRc = rc;
        InflateRect(&emptyRc, -6, -4);
        SetTextColor(hdc, g_theme.muted);
        DrawTextW(hdc, L"No usage data", -1, &emptyRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        return;
    }

    if (selected && !MonitorEventUsageText(*selected).empty()) {
        std::wstring usage = Utf8ToWide(MonitorEventUsageText(*selected));
        RECT valRc = {rc.left + 50, rc.top + 4, rc.right - 8, rc.top + 18};
        SetTextColor(hdc, g_theme.text);
        DrawTextW(hdc, usage.c_str(), -1, &valRc, DT_RIGHT | DT_SINGLELINE | DT_END_ELLIPSIS);
    }

    int plotTop = rc.top + 24;
    int plotBottom = rc.bottom - 6;
    int plotH = plotBottom - plotTop;
    uint64_t maxVal = 1;
    for (const auto& b : bars) {
        uint64_t s = b.inputTokens + b.outputTokens + b.thinkingTokens + b.cacheTokens;
        if (s > maxVal) maxVal = s;
    }

    // Peak value at the top right, unless the selected event's usage already sits there.
    if (!(selected && !MonitorEventUsageText(*selected).empty())) {
        std::wstring peakText = maxVal >= 1000 ? std::to_wstring(maxVal / 1000) + L"k" : std::to_wstring(maxVal);
        RECT peakRc = {rc.left + 50, rc.top + 4, rc.right - 8, rc.top + 18};
        SetTextColor(hdc, g_theme.muted);
        DrawTextW(hdc, peakText.c_str(), -1, &peakRc, DT_RIGHT | DT_SINGLELINE);
    }

    // Faint guide lines at half scale and full scale, then a solid baseline.
    HPEN guide = CreatePen(PS_DOT, 1, softLine);
    HGDIOBJ prevPen = SelectObject(hdc, guide);
    int axisLeft = 0;
    int axisRight = 0;
    TimeAxisExtent(rc, axisLeft, axisRight);
    for (int y : {plotTop, plotTop + plotH / 2}) {
        MoveToEx(hdc, axisLeft, y, nullptr);
        LineTo(hdc, axisRight, y);
    }
    // Time guides at 0, half and full run length, matching the time-by-kind chart's ticks.
    for (int i = 0; i <= 2; ++i) {
        int x = axisLeft + (axisRight - axisLeft) * i / 2;
        MoveToEx(hdc, x, plotTop, nullptr);
        LineTo(hdc, x, plotBottom);
    }
    SelectObject(hdc, prevPen);
    DeleteObject(guide);

    HPEN baseline = CreatePen(PS_SOLID, 1, softLine);
    prevPen = SelectObject(hdc, baseline);
    MoveToEx(hdc, axisLeft, plotBottom, nullptr);
    LineTo(hdc, axisRight, plotBottom);
    SelectObject(hdc, prevPen);
    DeleteObject(baseline);

    for (const auto& b : bars) {
        // A one-pixel gap between neighbouring bars keeps each column readable.
        int colLeft = static_cast<int>(b.rc.left) + 1;
        int colRight = (std::max)(colLeft + 1, static_cast<int>(b.rc.right) - 1);
        int h = static_cast<int>((static_cast<double>(b.inputTokens + b.outputTokens + b.thinkingTokens + b.cacheTokens) / maxVal) * plotH);
        if (h < 1) h = 1;
        int totalH = h;
        auto drawSeg = [&](uint64_t v, COLORREF color) {
            if (v == 0 || totalH <= 0) return;
            int sh = static_cast<int>((static_cast<double>(v) / (b.inputTokens + b.outputTokens + b.thinkingTokens + b.cacheTokens)) * h);
            if (sh < 1) sh = 1;
            if (sh > totalH) sh = totalH;
            RECT seg = {colLeft, plotBottom - totalH, colRight, plotBottom - totalH + sh};
            HBRUSH br = CreateSolidBrush(color);
            FillRect(hdc, &seg, br);
            DeleteObject(br);
            totalH -= sh;
        };
        drawSeg(b.cacheTokens, RGB(0x5C, 0x6C, 0x8C));
        drawSeg(b.thinkingTokens, RGB(0xB4, 0x7A, 0xFF));
        drawSeg(b.outputTokens, RGB(0x3D, 0xD5, 0x98));
        drawSeg(b.inputTokens, RGB(0x4F, 0x8C, 0xFF));

        if (b.eventIndex == selectedIndex) {
            HPEN selPen = CreatePen(PS_SOLID, 1, g_theme.accent);
            HGDIOBJ p = SelectObject(hdc, selPen);
            HGDIOBJ br = SelectObject(hdc, GetStockObject(NULL_BRUSH));
            RoundRect(hdc, colLeft - 2, plotTop - 2, colRight + 2, plotBottom + 2, 6, 6);
            SelectObject(hdc, br);
            SelectObject(hdc, p);
            DeleteObject(selPen);
        }
    }
    SelectObject(hdc, oldFont);
}

// Where the time went, by kind of step: read, write, tool, and everything else.
static void DrawKindChart(HDC hdc, const RECT& rc, const std::vector<ActivityRow>& rows, const TimeSpan& span) {
    if (rc.right <= rc.left || rc.bottom <= rc.top) return;
    const COLORREF softLine = BlendColor(g_theme.panel, g_theme.border, 60);

    HBRUSH bg = CreateSolidBrush(g_theme.panel);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    HPEN frame = CreatePen(PS_SOLID, 1, softLine);
    HGDIOBJ oldPen = SelectObject(hdc, frame);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    RoundRect(hdc, rc.left, rc.top, rc.right, rc.bottom, 10, 10);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(frame);

    // Each event is painted over its own time span on the shared axis, in the colour of its activity.
    struct KindRow {
        const wchar_t* label;
        int64_t ms;
        std::vector<const ActivityRow*> items;
    };
    KindRow kinds[4] = {
        {L"read", 0, {}},
        {L"write", 0, {}},
        {L"tool", 0, {}},
        {L"other", 0, {}},
    };
    for (const auto& r : rows) {
        if (r.startMs <= 0 || r.endMs < r.startMs) continue;  // events restored from an older session
        int idx = 3;
        if (r.kind == "read" || r.kind == "search" || r.kind == "web") idx = 0;
        else if (r.kind == "edit") idx = 1;
        else if (r.kind == "tool" || r.kind == "command" || r.kind == "subagent") idx = 2;
        kinds[idx].ms += r.endMs - r.startMs;
        kinds[idx].items.push_back(&r);
    }
    int64_t maxMs = span.lengthMs;  // the full axis is the run's elapsed time, shared with the usage graph

    SetBkMode(hdc, TRANSPARENT);
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, GetStockObject(DEFAULT_GUI_FONT)));
    RECT titleRc = {rc.left + 8, rc.top + 4, rc.right - 8, rc.top + 18};
    SetTextColor(hdc, g_theme.muted);
    DrawTextW(hdc, L"Time", -1, &titleRc, DT_LEFT | DT_SINGLELINE);

    const int axisH = 16;
    int plotLeft = 0;
    int plotRight = 0;
    TimeAxisExtent(rc, plotLeft, plotRight);
    int plotW = plotRight - plotLeft;
    int plotTop = rc.top + 22;
    int plotBottom = rc.bottom - axisH - 4;

    // Guides at 0, half and full scale, labelled along the bottom as the time axis.
    HPEN guide = CreatePen(PS_DOT, 1, softLine);
    HGDIOBJ prevPen = SelectObject(hdc, guide);
    for (int i = 0; i <= 2; ++i) {
        int x = plotLeft + plotW * i / 2;
        MoveToEx(hdc, x, plotTop, nullptr);
        LineTo(hdc, x, plotBottom);
        std::wstring label = FormatElapsed(maxMs * i / 2);
        RECT tickRc = {x - 40, plotBottom + 2, x + 40, plotBottom + 2 + axisH};
        UINT align = DT_CENTER;
        if (i == 0) { tickRc.left = x; tickRc.right = x + 80; align = DT_LEFT; }
        if (i == 2) { tickRc.left = x - 80; tickRc.right = x; align = DT_RIGHT; }
        SetTextColor(hdc, g_theme.muted);
        DrawTextW(hdc, label.c_str(), -1, &tickRc, align | DT_SINGLELINE);
    }
    SelectObject(hdc, prevPen);
    DeleteObject(guide);

    int rowH = (plotBottom - plotTop) / 4;
    int barPad = (std::max)(2, rowH / 4);
    for (int i = 0; i < 4; ++i) {
        int top = plotTop + i * rowH;
        RECT labelRc = {rc.left + 8, top, plotLeft - 4, top + rowH};
        SetTextColor(hdc, g_theme.muted);
        DrawTextW(hdc, kinds[i].label, -1, &labelRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        if (kinds[i].ms <= 0) continue;
        auto timeToX = [&](int64_t ms) {
            double f = static_cast<double>(ms - span.startMs) / static_cast<double>(maxMs);
            return plotLeft + static_cast<int>(plotW * (std::clamp)(f, 0.0, 1.0));
        };
        int barRight = plotLeft;
        for (const ActivityRow* r : kinds[i].items) {
            int x0 = timeToX(r->startMs);
            int x1 = (std::min)((std::max)(timeToX(r->endMs), x0 + 1), plotRight);
            HBRUSH br = CreateSolidBrush(ActivityColor(r->kind));
            RECT segRc = {x0, top + barPad, x1, top + rowH - barPad};
            FillRect(hdc, &segRc, br);
            DeleteObject(br);
            barRight = (std::max)(barRight, x1);
        }
        std::wstring value = FormatElapsed(kinds[i].ms);
        SetTextColor(hdc, g_theme.text);
        if (barRight + 64 <= rc.right) {
            RECT valRc = {barRight + 4, top, rc.right - 4, top + rowH};
            DrawTextW(hdc, value.c_str(), -1, &valRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        } else {
            // No room after the bar: write the value inside it, at its right end.
            RECT valRc = {plotLeft + 4, top, barRight - 4, top + rowH};
            DrawTextW(hdc, value.c_str(), -1, &valRc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
    }
    SelectObject(hdc, oldFont);
}

static std::wstring TokenCountText(uint64_t n) {
    wchar_t buf[32] = {};
    if (n < 1000) return std::to_wstring(n);
    if (n < 1000000) {
        swprintf_s(buf, L"%.1fk", static_cast<double>(n) / 1000.0);
    } else {
        swprintf_s(buf, L"%.1fM", static_cast<double>(n) / 1000000.0);
    }
    return buf;
}

// One line on what the agent is doing now, taken from the JSON stream.
static std::wstring StreamActivityText(const LlmAgentState& as, const std::vector<MonitorEvent>& events) {
    std::wstring text;
    if (as.status == "calling_tool" && !as.currentTool.empty()) {
        text = Utf8ToWide(as.currentTool);
        if (!as.currentToolAction.empty()) text += L" — " + Utf8ToWide(as.currentToolAction);
        return text;
    }
    std::string detail;
    if (as.status == "thinking") detail = as.lastThought;
    else if (as.status == "generating") detail = as.lastMessage;
    else if (as.status == "error") detail = as.lastError;
    if (detail.empty() && !events.empty()) {
        const auto& ev = events.back();
        detail = !ev.activity.empty() ? ev.activity : ev.text;
    }
    for (char& c : detail) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    }
    std::wstring detailW = Utf8ToWide(detail);
    if (detailW.size() > 160) detailW = detailW.substr(0, 157) + L"...";
    text = Utf8ToWide(as.status);
    if (!detailW.empty()) text += L" — " + detailW;
    return text;
}

// "-" for a step with no usage of this kind, otherwise a compact count.
static std::wstring TokenCellText(uint64_t n) {
    return n == 0 ? std::wstring(L"-") : TokenCountText(n);
}

static void AddEventTokens(ActivityRow& row, const MonitorEvent& ev) {
    uint64_t in = ev.inputTokens;
    uint64_t out = ev.outputTokens;
    uint64_t think = ev.thinkingTokens;
    uint64_t cache = ev.cacheReadTokens + ev.cacheWriteTokens;
    if (in == 0 && out == 0 && think == 0 && cache == 0 && ev.totalTokens > 0) in = ev.totalTokens;
    row.tokIn += in;
    row.tokOut += out;
    row.tokThink += think;
    row.tokCache += cache;
}

static std::vector<ActivityRow> BuildActivityRows(const std::vector<MonitorEvent>& events, bool running) {
    std::vector<ActivityRow> rows;
    for (int i = 0; i < static_cast<int>(events.size()); ++i) {
        std::string kind, target;
        if (!DescribeMonitorActivity(events[i], kind, target)) continue;
        if (!rows.empty() && rows.back().kind == kind && (kind == "thinking" || kind == "replying")) {
            rows.back().lastEvent = i;
            if (rows.back().target.empty()) rows.back().target = target;
            continue;
        }
        if (!rows.empty()) rows.back().endMs = events[i].recvEpochMs;
        ActivityRow r;
        r.kind = kind;
        r.target = target;
        r.description = events[i].note;
        r.startMs = events[i].recvEpochMs;
        r.endMs = r.startMs;
        r.firstEvent = r.lastEvent = i;
        rows.push_back(std::move(r));
    }
    if (!rows.empty()) {
        auto& last = rows.back();
        last.ongoing = running && last.kind != "done" && last.kind != "error";
        if (last.ongoing) last.endMs = NowEpochMs();
        else if (last.lastEvent < static_cast<int>(events.size())) last.endMs = events[last.lastEvent].recvEpochMs;
    }
    for (auto& r : rows) {
        for (int i = r.firstEvent; i <= r.lastEvent && i < static_cast<int>(events.size()); ++i) AddEventTokens(r, events[i]);
    }
    return rows;
}

static std::wstring ActivityRowDuration(const ActivityRow& r) {
    if (r.startMs <= 0 || r.endMs < r.startMs) return L"-";  // events restored from an older session
    return FormatElapsed(r.endMs - r.startMs);
}

static std::wstring ActivityRowStartTime(const ActivityRow& r) {
    if (r.startMs <= 0) return L"-";
    time_t t = static_cast<time_t>(r.startMs / 1000);
    struct tm lt = {};
    localtime_s(&lt, &t);
    wchar_t buf[16] = {};
    swprintf_s(buf, L"%02d:%02d:%02d", lt.tm_hour, lt.tm_min, lt.tm_sec);
    return buf;
}

static uint64_t ActivityRowTotalTokens(const ActivityRow& r) {
    return r.tokAll > 0 ? r.tokAll : r.tokIn + r.tokOut + r.tokCache + r.tokThink;
}

// One step as tab-separated values for the clipboard, with the full target rather than the cell's shortened text.
static std::wstring ActivityRowCopyText(const ActivityRow& r) {
    return ActivityRowStartTime(r) + L"\t" + ActivityRowDuration(r) + L"\t" + ActivityLabel(r.kind) + L"\t" +
           Utf8ToWide(r.description) + L"\t" + Utf8ToWide(r.target) + L"\t" + TokenCellText(r.tokIn) + L"\t" + TokenCellText(r.tokOut) + L"\t" +
           TokenCellText(r.tokCache) + L"\t" + TokenCellText(r.tokThink) + L"\t" +
           TokenCellText(ActivityRowTotalTokens(r));
}

// Session facts from the stream: session, turns, API time, tool results, denials, plan limit.
// Empty when the agent reports none of them.
static std::wstring AgentStatsText(const LlmAgentState& as) {
    std::wstringstream o;
    bool any = false;
    auto sep = [&]() { if (any) o << L"  |  "; any = true; };
    if (!as.sessionId.empty()) {
        sep();
        o << L"Session " << Utf8ToWide(as.sessionId.substr(0, 8));
    }
    if (as.numTurns > 0) {
        sep();
        o << Tr(L"ターン ", L"Turns ", L"Turnos ") << as.numTurns;
    }
    if (as.apiDurationMs > 0) {
        sep();
        o << L"API " << FormatElapsed(static_cast<int64_t>(as.apiDurationMs));
    }
    if (as.toolCalls > 0) {
        sep();
        o << Tr(L"ツール ", L"Tools ", L"Herramientas ") << as.toolCalls;
        if (as.toolErrors > 0) o << Tr(L" (失敗 ", L" (failed ", L" (fallidas ") << as.toolErrors << L")";
    }
    if (as.permissionDenials > 0) {
        sep();
        o << Tr(L"権限拒否 ", L"Denied ", L"Denegadas ") << as.permissionDenials;
    }
    if (!as.rateLimitType.empty()) {
        sep();
        o << Tr(L"上限 ", L"Limit ", L"Límite ") << Utf8ToWide(as.rateLimitType);
        if (as.rateLimitUtilization >= 0.0) {
            wchar_t pct[16] = {};
            swprintf_s(pct, L" %.0f%%", as.rateLimitUtilization * 100.0);
            o << pct;
        }
        if (as.rateLimitStatus == "rejected") o << Tr(L" (拒否中)", L" (rejected)", L" (rechazado)");
        if (as.rateLimitResetsAtSec > 0) {
            time_t t = static_cast<time_t>(as.rateLimitResetsAtSec);
            struct tm lt = {};
            localtime_s(&lt, &t);
            wchar_t reset[16] = {};
            swprintf_s(reset, L" → %02d:%02d", lt.tm_hour, lt.tm_min);
            o << reset;
        }
    }
    return o.str();
}

// The execution monitor pane: goal, where things stand now, and the activity history.
// Sub-agent status in the user's language, then its tool calls.
static std::wstring SubagentRowDescription(const ActivityTreeRow& t) {
    std::wstring status;
    if (t.kind == "running") status = Tr(L"実行中", L"Running", L"En curso");
    else if (t.kind == "completed") status = Tr(L"完了", L"Completed", L"Completado");
    else if (t.kind == "failed") status = Tr(L"失敗", L"Failed", L"Fallido");
    else if (t.kind == "killed") status = Tr(L"強制終了", L"Killed", L"Forzado");
    else if (t.kind == "stopped") status = Tr(L"停止", L"Stopped", L"Detenido");
    else status = Utf8ToWide(t.kind);
    return status + L"  |  " + Tr(L"ツール ", L"Tools ", L"Herramientas ") + std::to_wstring(t.toolUses);
}

// A tree row shown through the step cells: the same fields ActivityRow has.
static ActivityRow ToActivityRow(const ActivityTreeRow& t) {
    ActivityRow r;
    r.startMs = t.sinceEpochMs;
    r.endMs = t.ongoing ? NowEpochMs() : t.endEpochMs;
    r.ongoing = t.ongoing;
    r.firstEvent = t.firstEvent;
    r.lastEvent = t.lastEvent;
    r.tokIn = t.tokIn;
    r.tokOut = t.tokOut;
    r.tokCache = t.tokCache;
    r.tokThink = t.tokThink;
    if (t.isBranch) {
        // A sub-agent: its status goes in the description column, its total in the last token column.
        // One that did not complete keeps the error colour.
        r.kind = (t.kind == "running" || t.kind == "completed") ? "subagent" : "error";
        r.target = t.target;
        r.description = WideToUtf8(SubagentRowDescription(t));
        r.tokAll = t.tokens;
    } else {
        r.kind = t.kind;
        r.target = t.target;
        r.description = t.description;
    }
    return r;
}

// Without sub-agents the tree rows mirror the flat rows, newest first.
// With sub-agents the rows are the tree in time order (top = oldest, running work at the bottom).
// ds->rows (flat, oldest first) is left alone: the time chart reads it.
static void BuildMonitorTreeRows(LlmStreamDlgState* ds, bool running) {
    if (ds->overridesProcId != ds->procId) {
        ds->treeOverrides.clear();
        ds->overridesProcId = ds->procId;
    }
    ds->treeRows.clear();
    ds->treeMode = !ds->agentState.subagents.empty();
    const int eventCount = static_cast<int>(ds->events.size());
    if (!ds->treeMode) {
        for (int ri = static_cast<int>(ds->rows.size()) - 1; ri >= 0; --ri) {
            const auto& r = ds->rows[ri];
            ActivityTreeRow t;
            t.kind = r.kind;
            t.target = r.target;
            t.description = r.description;
            t.sinceEpochMs = r.startMs;
            t.endEpochMs = r.endMs;
            t.firstEvent = r.firstEvent;
            t.lastEvent = r.lastEvent;
            if (r.firstEvent >= 0 && r.firstEvent < eventCount) t.firstSeq = ds->events[r.firstEvent].seq;
            t.ongoing = r.ongoing;
            t.tokIn = r.tokIn;
            t.tokOut = r.tokOut;
            t.tokCache = r.tokCache;
            t.tokThink = r.tokThink;
            ds->treeRows.push_back(t);
        }
        return;
    }
    std::vector<LlmStreamEvent> internalEvents;
    internalEvents.reserve(ds->events.size());
    for (const auto& ev : ds->events) internalEvents.push_back(ToInternalStreamEvent(ev));
    ds->treeRows = BuildActivityTree(internalEvents, ds->agentState.subagents, NowEpochMs(), running, ds->treeOverrides);
    // The last activity of each agent (root = "") is the one still running, if its agent is.
    std::map<std::string, int> lastOfOwner;
    for (int i = 0; i < static_cast<int>(ds->treeRows.size()); ++i) {
        auto& t = ds->treeRows[i];
        if (t.isBranch) {
            t.ongoing = t.kind == "running";
            continue;
        }
        ActivityRow tok;
        // Other agents' events can lie between this row's first and last event.
        for (int k = t.firstEvent; k <= t.lastEvent && k < eventCount; ++k) {
            if (ds->events[k].agentId == t.agentId) AddEventTokens(tok, ds->events[k]);
        }
        t.tokIn = tok.tokIn;
        t.tokOut = tok.tokOut;
        t.tokCache = tok.tokCache;
        t.tokThink = tok.tokThink;
        lastOfOwner[t.agentId] = i;
    }
    for (const auto& entry : lastOfOwner) {
        auto& t = ds->treeRows[entry.second];
        bool ownerRunning = running;
        if (!entry.first.empty()) {
            ownerRunning = false;
            for (const auto& s : ds->agentState.subagents) {
                if (s.id == entry.first && s.status == "running") ownerRunning = running;
            }
        }
        t.ongoing = ownerRunning && t.kind != "done" && t.kind != "error";
    }
}

// Identity of a row across rebuilds, so the selection can be put back on the same row.
static std::string MonitorRowKey(const ActivityTreeRow& t) {
    if (t.isBranch) return "B" + t.agentId;
    return "A" + t.agentId + "#" + std::to_string(t.firstSeq);
}

// What the list shows. A change means the list must be refilled.
static std::string MonitorTreeSignature(const std::vector<ActivityTreeRow>& rows) {
    std::string s;
    for (const auto& t : rows) {
        s += std::to_string(t.depth);
        s += t.isBranch ? 'B' : 'A';
        s += t.agentId;
        s += '|';
        s += t.open ? '1' : '0';
        s += t.kind;
        s += '|';
        s += std::to_string(t.count);
        s += ':';
        s += std::to_string(t.lastEvent);
        s += ';';
    }
    return s;
}

// Index into treeRows of a list item (its lParam), or -1.
static int MonitorRowAt(const LlmStreamDlgState* ds, int listItem) {
    if (!ds->hwndList || listItem < 0) return -1;
    LVITEMW item = {};
    item.mask = LVIF_PARAM;
    item.iItem = listItem;
    if (!ListView_GetItem(ds->hwndList, &item)) return -1;
    const int ti = static_cast<int>(item.lParam);
    return (ti >= 0 && ti < static_cast<int>(ds->treeRows.size())) ? ti : -1;
}

// Key of the selected row, read before the tree is rebuilt (the list still refers to the old rows).
static std::string MonitorSelectedKey(const LlmStreamDlgState* ds) {
    if (!ds->hwndList) return std::string();
    const int ti = MonitorRowAt(ds, ListView_GetNextItem(ds->hwndList, -1, LVNI_SELECTED));
    return ti >= 0 ? MonitorRowKey(ds->treeRows[ti]) : std::string();
}

// Refills the list from treeRows. lParam = index into treeRows. Keeps the selection,
// and the scroll position unless the view was at the bottom (then it follows the running row).
static void RebuildMonitorList(LlmStreamDlgState* ds, const std::string& selKey) {
    HWND list = ds->hwndList;
    const int oldCount = ListView_GetItemCount(list);
    const int oldTop = oldCount > 0 ? ListView_GetTopIndex(list) : 0;
    const bool atTail = oldCount == 0 || oldTop + ListView_GetCountPerPage(list) >= oldCount;
    ds->rebuilding = true;
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list);
    const int n = static_cast<int>(ds->treeRows.size());
    int lastOngoing = -1;
    for (int i = 0; i < n; ++i) {
        const auto& t = ds->treeRows[i];
        const ActivityRow r = ToActivityRow(t);
        if (t.ongoing) lastOngoing = i;
        std::wstring startStr = ActivityRowStartTime(r);
        std::wstring durStr = ActivityRowDuration(r);
        // Sub-agent rows show a triangle (open or closed) and their type; nested rows are indented.
        std::wstring whatStr(static_cast<size_t>(t.depth) * 3, L' ');
        if (t.isBranch) {
            whatStr += t.open ? L"▼ " : L"▶ ";
            if (g_activityStyle) whatStr += ActivityIcon("subagent");
            whatStr += t.subagentType.empty() ? ActivityText("subagent") : Utf8ToWide(t.subagentType);
        } else {
            whatStr += ActivityLabel(r.kind);
            if (t.count > 1) whatStr += L" ×" + std::to_wstring(t.count);
        }
        std::wstring targetStr = ShortTarget(r.target, 200);
        std::wstring descStr = ShortTarget(r.description, 200);
        LVITEMW lvi = {};
        lvi.mask = LVIF_TEXT | LVIF_PARAM;
        lvi.iItem = i;
        lvi.pszText = startStr.data();
        lvi.lParam = i;
        ListView_InsertItem(list, &lvi);
        ListView_SetItemText(list, i, 1, durStr.data());
        ListView_SetItemText(list, i, 2, whatStr.data());
        ListView_SetItemText(list, i, 3, descStr.data());
        ListView_SetItemText(list, i, 4, targetStr.data());
        ListView_SetItemText(list, i, 5, TokenCellText(r.tokIn).data());
        ListView_SetItemText(list, i, 6, TokenCellText(r.tokOut).data());
        ListView_SetItemText(list, i, 7, TokenCellText(r.tokCache).data());
        ListView_SetItemText(list, i, 8, TokenCellText(r.tokThink).data());
        ListView_SetItemText(list, i, 9, TokenCellText(ActivityRowTotalTokens(r)).data());
    }
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, FALSE);
    ds->rebuilding = false;
    if (!selKey.empty()) {
        for (int i = 0; i < n; ++i) {
            if (MonitorRowKey(ds->treeRows[i]) != selKey) continue;
            ListView_SetItemState(list, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            break;
        }
    }
    if (n == 0) return;
    if (!ds->treeMode) {
        ListView_EnsureVisible(list, 0, FALSE);  // newest step on top, as before
    } else if (atTail) {
        ListView_EnsureVisible(list, lastOngoing >= 0 ? lastOngoing : n - 1, FALSE);
    } else {
        // The user scrolled away from the bottom: keep the view where it was. The refilled list is at
        // the top, and EnsureVisible stops at the page's last row, so ask for that row.
        const int bottom = oldTop + ListView_GetCountPerPage(list) - 1;
        ListView_EnsureVisible(list, bottom < n ? bottom : n - 1, FALSE);
    }
}

static void ReloadMonitorPane(HWND hwnd, LlmStreamDlgState* ds) {
    const ProcessSnapshot& s = ds->snap;
    bool running = s.state == ProcState::Running;

    if (!s.jobKey.empty() && ds->dash &&
        (ds->goalJobKey != s.jobKey || ds->goalJobAttempt != s.attemptN)) {
        std::string err;
        std::string jobResp = TaskQueueGetIpc(ds->dash, s.jobKey, false, 0, err);
        auto job = nlohmann::json::parse(jobResp, nullptr, false);
        ds->goalJobKey = s.jobKey;
        ds->goalJobAttempt = s.attemptN;
        ds->goalJobSummary.clear();
        ds->goalJobSuccess.clear();
        ds->goalJobMemoryRefs.clear();
        ds->goalJobMaxAttempts = 0;
        {
            std::wstring declared = TaskQueueResourcesDisplay(jobResp);
            ds->goalJobResources = declared.empty() ? std::wstring(Tr(L"(なし)", L"(none)", L"(ninguno)")) : declared;
        }
        if (job.is_object()) {
            if (job.contains("job") && job["job"].is_object()) job = job["job"];
            if (job.contains("summary") && job["summary"].is_string()) ds->goalJobSummary = job["summary"].get<std::string>();
            if (job.contains("max_attempts") && job["max_attempts"].is_number_integer()) ds->goalJobMaxAttempts = job["max_attempts"].get<int>();
            auto succ = job.find("success");
            if (succ != job.end() && succ->is_object()) {
                std::wstringstream ss;
                ss << L"exit " << succ->value("exit_code", 0);
                auto fn = succ->find("files_newer");
                if (fn != succ->end() && fn->is_array()) {
                    for (const auto& f : *fn) if (f.is_string()) ss << Tr(L" + 更新: ", L" + updated: ", L" + actualizado: ") << Utf8ToWide(f.get<std::string>());
                }
                auto re = succ->find("output_regex");
                if (re != succ->end() && re->is_string()) ss << Tr(L" + 出力に /", L" + output /", L" + salida /") << Utf8ToWide(re->get<std::string>()) << L"/";
                auto tr = succ->find("test_report");
                if (tr != succ->end() && tr->is_string()) ss << Tr(L" + テスト結果: ", L" + test report: ", L" + informe de pruebas: ") << Utf8ToWide(tr->get<std::string>());
                ds->goalJobSuccess = ss.str();
            }
            auto attempts = job.find("attempts");
            if (attempts != job.end() && attempts->is_array()) {
                const nlohmann::json* chosen = nullptr;
                for (const auto& a : *attempts) {
                    if (!a.is_object()) continue;
                    if (a.value("n", 0) == s.attemptN) { chosen = &a; break; }
                }
                if (!chosen && !attempts->empty() && attempts->back().is_object()) chosen = &attempts->back();
                if (chosen) {
                    auto refs = chosen->find("memory_refs");
                    if (refs != chosen->end() && refs->is_array()) {
                        std::wstringstream rs;
                        bool first = true;
                        for (const auto& id : *refs) {
                            if (!id.is_string()) continue;
                            if (!first) rs << L", ";
                            first = false;
                            rs << Utf8ToWide(id.get<std::string>());
                        }
                        ds->goalJobMemoryRefs = rs.str();
                    }
                }
            }
        }
    }
    if (s.jobKey.empty()) {
        ds->goalJobKey.clear();
        ds->goalJobAttempt = -1;
        ds->goalJobMemoryRefs.clear();
    }
    std::string goal = s.supervisorGoal;
    if (goal.empty() && !s.jobKey.empty()) goal = ds->goalJobSummary;

    std::wstringstream hdr;

    // First line: the task ID (the task queue job key when there is one, else the process).
    std::wstring processLabel = L"#" + std::to_wstring(s.id) + L" " + Utf8ToWide(s.name);
    if (s.jobKey.empty()) {
        hdr << Tr(L"タスクID: ", L"Task ID: ", L"ID de tarea: ") << processLabel << L"\r\n";
    } else {
        hdr << Tr(L"タスクID: ", L"Task ID: ", L"ID de tarea: ") << Utf8ToWide(s.jobKey) << L"  (" << processLabel << L")\r\n";
    }

    // End condition: the agent's own TODO list, the job's success check, attempts and time limits.
    std::vector<std::wstring> ends;
    for (auto it = ds->events.rbegin(); it != ds->events.rend(); ++it) {
        if (it->kind == "plan" && it->planTarget.find(" done") != std::string::npos) {
            ends.push_back(L"📋 TODO " + Utf8ToWide(it->planTarget));
            break;
        }
    }
    if (!s.jobKey.empty()) {
        std::wstring succ = Tr(L"🎯 成功条件 ", L"🎯 success when ", L"🎯 éxito si ")
            + (ds->goalJobSuccess.empty() ? std::wstring(L"exit 0") : ds->goalJobSuccess);
        ends.push_back(succ);
        if (ds->goalJobMaxAttempts > 1) {
            ends.push_back(Tr(L"🔁 試行 ", L"🔁 attempt ", L"🔁 intento ") + std::to_wstring(s.attemptN)
                           + L"/" + std::to_wstring(ds->goalJobMaxAttempts));
        }
    }
    if (s.timeoutSec > 0) {
        std::wstring t = Tr(L"⏱ 制限 ", L"⏱ limit ", L"⏱ límite ") + FormatElapsed(static_cast<int64_t>(s.timeoutSec) * 1000);
        if (running) {
            int64_t remain = static_cast<int64_t>(s.timeoutSec) * 1000 - static_cast<int64_t>(s.elapsedMs);
            t += Tr(L" (残り ", L" (", L" (quedan ") + FormatElapsed(remain) + Tr(L")", L" left)", L")");
        }
        ends.push_back(t);
    }
    if (s.idleTimeoutSec > 0) {
        ends.push_back(Tr(L"💤 無出力 ", L"💤 no output for ", L"💤 sin salida ") + FormatElapsed(static_cast<int64_t>(s.idleTimeoutSec) * 1000)
                       + Tr(L" で打ち切り", L" stops it", L" lo detiene"));
    }
    hdr << Tr(L"終了条件: ", L"End condition: ", L"Condición de fin: ");
    if (ends.empty()) {
        hdr << Tr(L"プロセスが終了したとき (制限なし)", L"when the process exits (no limit)", L"cuando el proceso termine (sin límite)");
    } else {
        for (size_t i = 0; i < ends.size(); ++i) hdr << (i ? L"  ·  " : L"") << ends[i];
    }
    hdr << L"\r\n";

    hdr << Tr(L"目標: ", L"Goal: ", L"Objetivo: ");
    if (!goal.empty()) hdr << Utf8ToWide(goal);
    else hdr << Tr(L"(未設定: hancho-cli worker info <id> --goal で設定、タスクキューのジョブは summary)",
                   L"(not set: hancho-cli worker info <id> --goal, or a task queue job's summary)",
                   L"(sin definir: hancho-cli worker info <id> --goal)");
    hdr << L"\r\n";
    if (!ds->goalJobMemoryRefs.empty()) {
        hdr << Tr(L"注入メモリ: ", L"Injected memory: ", L"Memoria inyectada: ")
            << ds->goalJobMemoryRefs << L"\r\n";
    }
    if (!s.jobKey.empty()) {
        hdr << Tr(L"リソース: ", L"Resources: ", L"Recursos: ") << ds->goalJobResources << L"\r\n";
    }

    hdr << Tr(L"現状: ", L"Now: ", L"Ahora: ");
    std::wstring now = ActivityLine(ds->agentState.activity, ds->agentState.activityTarget,
                                    ds->agentState.activitySinceEpochMs, running);
    if (!running) {
        hdr << StateLabelWide(s.state, false);
        if (s.exitCode != STILL_ACTIVE) hdr << L" (exit " << s.exitCode << L")";
        if (!now.empty()) hdr << Tr(L"  — 最後: ", L"  — last: ", L"  — último: ") << now;
    } else if (!now.empty()) {
        hdr << now;
    } else if (ds->events.empty() && !s.jsonStream) {
        hdr << StateLabelWide(s.state, s.idleMs >= 10000)
            << Tr(L"  (JSON ストリームなし: 最終行) ", L"  (no JSON stream: last line) ", L"  (sin flujo JSON) ")
            << Utf8ToWide(s.lastLinePreview);
    } else {
        hdr << Tr(L"開始待ち", L"Starting", L"Iniciando");
    }
    hdr << L"\r\n";

    std::wstring agentLabel = Utf8ToWide(s.agentType.empty() ? ds->agentState.agentType : s.agentType);
    if (!agentLabel.empty()) hdr << L"[" << agentLabel << L"]";
    if (!ds->agentState.model.empty()) hdr << (agentLabel.empty() ? L"" : L" ") << Utf8ToWide(ds->agentState.model);
    if (!agentLabel.empty() || !ds->agentState.model.empty()) hdr << L"  |  ";
    hdr << Tr(L"経過 ", L"Elapsed ", L"Transcurrido ") << FormatElapsed(static_cast<int64_t>(s.elapsedMs));
    if (ds->agentState.stepCount > 0) hdr << L"  |  Steps " << ds->agentState.stepCount;
    if (ds->agentState.totalTokens > 0) hdr << L"  |  Tokens " << ds->agentState.totalTokens;
    if (ds->agentState.costUsd > 0.0) {
        wchar_t cost[32] = {};
        swprintf_s(cost, L"  |  $%.2f", ds->agentState.costUsd);
        hdr << cost;
    }
    hdr << L"\r\n";
    std::wstring stats = AgentStatsText(ds->agentState);
    if (!stats.empty()) hdr << stats << L"\r\n";
    if (!s.supervisorNotes.empty()) {
        hdr << Tr(L"メモ: ", L"Notes: ", L"Notas: ") << Utf8ToWide(s.supervisorNotes) << L"\r\n";
    }
    if (ds->hwndHeader) {
        std::wstring text = hdr.str();
        wchar_t cur[4096] = {};
        GetWindowTextW(ds->hwndHeader, cur, 4096);
        if (text != cur) SetWindowTextW(ds->hwndHeader, text.c_str());  // avoid flicker / scroll reset
    }

    const std::string selKey = MonitorSelectedKey(ds);  // read before the rows are replaced
    ds->rows = BuildActivityRows(ds->events, running);
    BuildMonitorTreeRows(ds, running);
    uint64_t lastSeq = ds->events.empty() ? 0 : ds->events.back().seq;
    const std::string sig = MonitorTreeSignature(ds->treeRows);
    const bool eventsChanged = ds->lastEventCount != ds->events.size() || ds->lastEventSeq != lastSeq;
    const bool holdList = g_scrollDragWnd == ds->hwndList;
    if (ds->hwndList && !holdList && (eventsChanged || sig != ds->listSig)) {
        ds->lastEventCount = ds->events.size();
        ds->lastEventSeq = lastSeq;
        ds->listSig = sig;
        RebuildMonitorList(ds, selKey);
    } else if (ds->hwndList && !holdList) {
        // Only the running steps' durations change between events. Their rows are updated in place.
        for (int i = 0; i < static_cast<int>(ds->treeRows.size()); ++i) {
            if (!ds->treeRows[i].ongoing) continue;
            std::wstring durStr = ActivityRowDuration(ToActivityRow(ds->treeRows[i]));
            ListView_SetItemText(ds->hwndList, i, 1, durStr.data());
        }
    }
    if (ds->rcUsageGraph.right > ds->rcUsageGraph.left) InvalidateRect(hwnd, &ds->rcUsageGraph, FALSE);
    if (ds->rcKindChart.right > ds->rcKindChart.left) InvalidateRect(hwnd, &ds->rcKindChart, FALSE);
}

static void ReloadStreamDialogData(HWND hwnd, LlmStreamDlgState* ds) {
    if (!ds || !ds->dash) return;
    bool hasLocalSnap = (ds->procId > 0 && FetchProcessSnapshotIpc(ds->dash, ds->procId, ds->snap));
    if (hasLocalSnap) {
        if (ds->wsClient) {
            ds->wsClient->Close();
            ds->wsClient.reset();
            ds->subscribedJobId.clear();
        }
        ds->events.clear();
        GetAgentStateIpc(ds->dash, ds->procId, ds->agentState);
        LoadMonitorEvents(ds->snap, ds->events);
    } else {
        std::string selectedJob = (!ds->dash->selectedJobKey.empty()) ? ds->dash->selectedJobKey : ds->goalJobKey;
        bool foundJob = false;
        if (!selectedJob.empty() && ds->dash) {
            std::string tqErr;
            std::string jobResp = TaskQueueGetIpc(ds->dash, selectedJob, true, 0, tqErr);
            nlohmann::json rootJob = nlohmann::json::parse(jobResp, nullptr, false);
            nlohmann::json job = (rootJob.is_object() && rootJob.contains("job") && rootJob["job"].is_object()) ? rootJob["job"] : rootJob;
            if (job.is_object() && job.contains("id")) {
                foundJob = true;
                std::string jobId = job.value("id", selectedJob);
                std::string jobName = job.value("name", jobId);
                std::string host = job.value("host", "");
                std::string jobState = job.value("state", "");
                std::string summary = job.value("summary", "");
                int timeoutSec = static_cast<int>(job.value("timeout_sec", 0));
                int idleTimeoutSec = static_cast<int>(job.value("idle_timeout_sec", 0));

                int attemptN = 1;
                std::string remoteHost = host;
                int remotePid = 0;
                std::string startStr;
                std::string endStr;
                int exitCode = 0;
                auto attempts = job.value("attempts", nlohmann::json::array());
                if (!attempts.empty() && attempts.back().is_object()) {
                    const auto& a = attempts.back();
                    attemptN = a.value("n", 1);
                    if (a.contains("remote_host") && a["remote_host"].is_string()) remoteHost = a["remote_host"].get<std::string>();
                    if (a.contains("remote_process_id") && a["remote_process_id"].is_number()) remotePid = a["remote_process_id"].get<int>();
                    startStr = a.value("start", "");
                    endStr = a.value("end", "");
                    exitCode = a.value("exit_code", 0);
                }

                ds->snap = ProcessSnapshot();
                ds->snap.id = remotePid > 0 ? remotePid : 0;
                ds->snap.name = jobName;
                ds->snap.jobKey = jobId;
                ds->snap.attemptN = attemptN;
                ds->snap.state = (jobState == "running") ? ProcState::Running : (exitCode != 0 ? ProcState::Failed : ProcState::Completed);
                ds->snap.exitCode = (jobState == "running") ? STILL_ACTIVE : static_cast<DWORD>(exitCode);
                ds->snap.supervisorGoal = summary;
                ds->snap.startTimeStr = startStr;
                ds->snap.endTimeStr = endStr;
                ds->snap.timeoutSec = timeoutSec;
                ds->snap.idleTimeoutSec = idleTimeoutSec;
                if (!startStr.empty()) {
                    int64_t ep = ParseTimestampStrToEpoch(startStr);
                    if (ep > 0) {
                        ds->snap.startEpochSec = ep;
                        int64_t nowSec = time(nullptr);
                        if (jobState == "running") {
                            ds->snap.elapsedMs = static_cast<uint64_t>((std::max)(int64_t{0}, nowSec - ep) * 1000);
                        } else if (!endStr.empty()) {
                            int64_t endEp = ParseTimestampStrToEpoch(endStr);
                            if (endEp >= ep) {
                                ds->snap.endEpochSec = endEp;
                                ds->snap.elapsedMs = static_cast<uint64_t>((endEp - ep) * 1000);
                            }
                        }
                    }
                }

                std::wstring tqDir = PathJoinWide(GetLogsDirectoryWide(), L"taskqueue");
                std::wstring eventsPathW = PathJoinWide(tqDir, Utf8ToWide(SanitizeFileNameComponent(jobId)) + L"." + std::to_wstring(attemptN) + L".events.jsonl");
                ds->snap.streamFilePath = WideToUtf8(eventsPathW);

                // Set up or maintain live streaming subscription when job is running on a remote host
                if (jobState == "running" && !remoteHost.empty()) {
                    if (ds->subscribedJobId != jobId || ds->subscribedAttempt != attemptN ||
                        !ds->wsClient || !ds->wsClient->IsConnected()) {
                        if (ds->wsClient) {
                            ds->wsClient->Close();
                            ds->wsClient.reset();
                        }
                        HostRegistry registry;
                        std::string regErr;
                        if (registry.Load(regErr)) {
                            RemoteHost hostEntry;
                            if (registry.Get(remoteHost, hostEntry)) {
                                std::string token = ResolveHostToken(hostEntry);
                                auto client = std::make_shared<pc::remote::WebSocketClient>();
                                std::string connErr;
                                if (client->Connect(hostEntry.address, token, 5000, &connErr)) {
                                    ds->wsClient = client;
                                    ds->subscribedJobId = jobId;
                                    ds->subscribedAttempt = attemptN;
                                    uint64_t sinceSeq = ds->events.empty() ? 0 : ds->events.back().seq;
                                    HWND targetHwnd = hwnd;
                                    std::wstring targetEventsW = eventsPathW;
                                    client->SetStreamCallback([targetHwnd, targetEventsW](
                                        const std::string& subId, uint64_t seq, const std::string& line, const nlohmann::json& doc) {
                                        if (!line.empty()) {
                                            AppendToLogFileWide(targetEventsW, line + "\n");
                                        }
                                        if (IsWindow(targetHwnd)) {
                                            PostMessageW(targetHwnd, WM_USER + 101, 0, 0);
                                        }
                                    });
                                    std::string subId;
                                    std::string subErr;
                                    if (remotePid > 0) {
                                        client->Subscribe("events", remotePid, sinceSeq, subId, subErr, 5000);
                                    } else {
                                        client->SubscribeFile(WideToUtf8(eventsPathW), sinceSeq, subId, subErr, 5000);
                                    }
                                }
                            }
                        }
                    }
                } else {
                    if (ds->wsClient) {
                        ds->wsClient->Close();
                        ds->wsClient.reset();
                        ds->subscribedJobId.clear();
                    }
                }

                ds->events.clear();
                LoadMonitorEvents(ds->snap, ds->events);
                UpdateAgentStateFromMonitorEvents(ds->events, ds->agentState, jobState == "running");
            }
        }

        if (!foundJob) {
            if (ds->wsClient) {
                ds->wsClient->Close();
                ds->wsClient.reset();
                ds->subscribedJobId.clear();
            }
            ds->snap = ProcessSnapshot();
            ds->events.clear();
            ds->agentState = LlmAgentState();
            if (ds->hwndHeader) {
                SetWindowTextW(ds->hwndHeader, Tr(L"プロセスリストでプロセスを選択してください。",
                                                  L"Select a process in the process list.",
                                                  L"Seleccione un proceso en la lista."));
            }
            if (ds->hwndList && ds->lastEventCount != 0) {
                ListView_DeleteAllItems(ds->hwndList);
                if (ds->hwndEditPreview) SetWindowTextW(ds->hwndEditPreview, L"");
            }
            ds->lastEventCount = 0;
            ds->lastEventSeq = 0;
            if (ds->rcUsageGraph.right > ds->rcUsageGraph.left) InvalidateRect(hwnd, &ds->rcUsageGraph, FALSE);
            return;
        }
    }

    std::string modelStr = ds->agentState.model;
    if (modelStr.empty()) {
        for (const auto& ev : ds->events) {
            if (!ev.model.empty()) { modelStr = ev.model; break; }
        }
    }
    if (!modelStr.empty()) ds->agentState.model = modelStr;
    if (ds->embedded) {
        ReloadMonitorPane(hwnd, ds);
        return;
    }

    std::wstring agentLabel =Utf8ToWide(ds->snap.agentType.empty() ? (ds->agentState.agentType.empty() ? "generic" : ds->agentState.agentType) : ds->snap.agentType);
    std::wstringstream hdr;
    hdr << L"ID: #" << ds->snap.id << L"  |  "
        << Utf8ToWide(ds->snap.name) << L"  |  Agent: ["
        << agentLabel << L"] "
        << StateLabelWide(ds->snap.state, ds->snap.idleMs >= 10000)
        << L" (" << Utf8ToWide(ds->agentState.status) << L")";
    if (!modelStr.empty()) {
        hdr << L"  |  Model: " << Utf8ToWide(modelStr);
    }
    hdr << L"\r\n";
    std::wstring doing = StreamActivityText(ds->agentState, ds->events);
    if (!doing.empty()) {
        hdr << Tr(L"現在: ", L"Now: ", L"Ahora: ") << doing << L"\r\n";
    }
    if (!ds->agentState.currentTool.empty()) {
        hdr << L"Tool: " << Utf8ToWide(ds->agentState.currentTool);
        if (!ds->agentState.currentToolAction.empty()) {
            hdr << L" (" << Utf8ToWide(ds->agentState.currentToolAction) << L")";
        }
        hdr << L"  |  ";
    }
    if (ds->events.empty() && !ds->snap.jsonStream && ds->snap.agentType.empty()) {
        hdr << Tr(L"(JSON ストリームなし)  |  ", L"(no JSON stream)  |  ", L"(sin flujo JSON)  |  ");
    }
    hdr << L"Steps: " << ds->agentState.stepCount
        << L"  |  Events: " << ds->events.size();
    if (ds->agentState.totalTokens > 0) {
        hdr << L"  |  Tokens: " << ds->agentState.totalTokens;
    }
    if (ds->agentState.costUsd > 0.0) {
        hdr << L"  |  Cost: $" << ds->agentState.costUsd;
    }
    hdr << L"\r\n";
    std::wstring stats = AgentStatsText(ds->agentState);
    if (!stats.empty()) hdr << stats << L"\r\n";
    if (!ds->snap.supervisorGoal.empty()) {
        hdr << Tr(L"目標: ", L"Goal: ", L"Objetivo: ") << Utf8ToWide(ds->snap.supervisorGoal) << L"\r\n";
    }
    if (!ds->snap.supervisorNotes.empty()) {
        hdr << Tr(L"メモ: ", L"Notes: ", L"Notas: ") << Utf8ToWide(ds->snap.supervisorNotes) << L"\r\n";
    }
    if (ds->hwndHeader) {
        SetWindowTextW(ds->hwndHeader, hdr.str().c_str());
    }

    // The tail is capped, so a new event may keep the count unchanged: compare the last seq too.
    uint64_t lastSeq = ds->events.empty() ? 0 : ds->events.back().seq;
    if (ds->hwndList && g_scrollDragWnd != ds->hwndList &&
        (ds->lastEventCount != ds->events.size() || ds->lastEventSeq != lastSeq)) {
        ds->lastEventCount = ds->events.size();
        ds->lastEventSeq = lastSeq;
        SendMessageW(ds->hwndList, WM_SETREDRAW, FALSE, 0);
        ListView_DeleteAllItems(ds->hwndList);
        for (int i = 0; i < static_cast<int>(ds->events.size()); ++i) {
            const auto& ev = ds->events[i];
            std::wstring seqStr = std::to_wstring(ev.seq);
            std::wstring timeStr = Utf8ToWide(ev.timestamp);
            std::wstring typeStr = Utf8ToWide(ev.kind);
            std::wstring toolStr = Utf8ToWide(ev.toolName);
            std::wstring actionStr = Utf8ToWide(ev.activity);
            std::string content = ev.text;
            if (content.empty() && !ev.toolInput.empty()) content = ev.toolInput;
            if (content.empty() && !ev.rawJson.empty()) content = ev.rawJson;
            std::wstring contentStr = Utf8ToWide(content);
            if (contentStr.size() > 100) contentStr = contentStr.substr(0, 97) + L"...";
            std::wstring usageStr = Utf8ToWide(MonitorEventUsageText(ev));

            LVITEMW lvi = {};
            lvi.mask = LVIF_TEXT | LVIF_PARAM;
            lvi.iItem = i;
            lvi.iSubItem = 0;
            lvi.pszText = seqStr.data();
            lvi.lParam = i;
            ListView_InsertItem(ds->hwndList, &lvi);
            ListView_SetItemText(ds->hwndList, i, 1, timeStr.data());
            ListView_SetItemText(ds->hwndList, i, 2, typeStr.data());
            ListView_SetItemText(ds->hwndList, i, 3, toolStr.data());
            ListView_SetItemText(ds->hwndList, i, 4, actionStr.data());
            ListView_SetItemText(ds->hwndList, i, 5, contentStr.data());
            ListView_SetItemText(ds->hwndList, i, 6, usageStr.data());
        }
        SendMessageW(ds->hwndList, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(ds->hwndList, nullptr, FALSE);
        if (!ds->events.empty()) {
            int lastIdx = static_cast<int>(ds->events.size()) - 1;
            ListView_EnsureVisible(ds->hwndList, lastIdx, FALSE);
        }
    }
    if (ds->rcUsageGraph.right > ds->rcUsageGraph.left) {
        InvalidateRect(hwnd, &ds->rcUsageGraph, FALSE);
    }
}

static LRESULT CALLBACK LlmStreamWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ds = reinterpret_cast<LlmStreamDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ds = reinterpret_cast<LlmStreamDlgState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ds));

            ds->hFontMono = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                        FIXED_PITCH | FF_MODERN, L"Consolas");

            ds->hwndHeader = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                40, 12, 792, 75, hwnd, (HMENU)2100, nullptr, nullptr);
            SetWindowSubclass(ds->hwndHeader, ListViewSubclassProc, 1, 0);
            ds->hwndAgentIcon = CreateWindowExW(0, L"STATIC", L"🤖",
                WS_CHILD | WS_VISIBLE | SS_CENTER,
                12, 12, 24, 24, hwnd, (HMENU)2105, nullptr, nullptr);

            ds->hwndList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                12, 95, 820, 260, hwnd, (HMENU)2101, nullptr, nullptr);
            ListView_SetExtendedListViewStyle(ds->hwndList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
            ListView_SetBkColor(ds->hwndList, g_theme.panel);
            ListView_SetTextBkColor(ds->hwndList, g_theme.panel);
            ListView_SetTextColor(ds->hwndList, g_theme.listText);

            const wchar_t* colTitles[] = {
                Tr(L"#", L"#", L"#"),
                Tr(L"時刻", L"Time", L"Hora"),
                Tr(L"種別", L"Type", L"Tipo"),
                Tr(L"ツール", L"Tool", L"Herramienta"),
                Tr(L"アクション", L"Action", L"Acción"),
                Tr(L"内容 / サマリ", L"Content / Summary", L"Contenido"),
                Tr(L"Usage", L"Usage", L"Uso")
            };
            const int colWidths[] = {50, 80, 80, 110, 140, 235, 150};
            const wchar_t* monTitles[] = {
                Tr(L"開始", L"Start", L"Inicio"),
                Tr(L"所要", L"Took", L"Duración"),
                Tr(L"実行中", L"Running", L"En curso"),
                Tr(L"説明", L"Description", L"Descripción"),
                Tr(L"対象", L"Target", L"Objetivo"),
                L"i",
                L"o",
                L"c",
                L"t",
                L"a"
            };
            const int monWidths[] = {60, 60, 210, 150, 170, 44, 44, 44, 44, 52};
            int colCount = ds->embedded ? 10 : 7;
            for (int i = 0; i < colCount; ++i) {
                if (ds->embedded) {
                    LVCOLUMNW lvc = {};
                    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                    lvc.pszText = const_cast<wchar_t*>(monTitles[i]);
                    lvc.cx = monWidths[i];
                    lvc.iSubItem = i;
                    ListView_InsertColumn(ds->hwndList, i, &lvc);
                    continue;
                }
                LVCOLUMNW lvc = {};
                lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                lvc.pszText = const_cast<wchar_t*>(colTitles[i]);
                lvc.cx = colWidths[i];
                lvc.iSubItem = i;
                ListView_InsertColumn(ds->hwndList, i, &lvc);
            }
            HWND hMonHeader = ListView_GetHeader(ds->hwndList);
            if (hMonHeader) {
                for (int i = 0; i < colCount; ++i) {
                    HDITEMW hdi = {};
                    hdi.mask = HDI_FORMAT;
                    if (Header_GetItem(hMonHeader, i, &hdi)) {
                        hdi.fmt |= HDF_OWNERDRAW;
                        Header_SetItem(hMonHeader, i, &hdi);
                    }
                }
                SetWindowSubclass(hMonHeader, HeaderSubclassProc, 1, 0);
            }
            SetWindowSubclass(ds->hwndList, ListViewSubclassProc, 1, 0);

            ds->hwndEditPreview = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
                12, 365, 820, 180, hwnd, (HMENU)2102, nullptr, nullptr);
            SetWindowSubclass(ds->hwndEditPreview, ListViewSubclassProc, 1, 0);

            if (ds->hFontUi) {
                SendMessageW(ds->hwndHeader, WM_SETFONT, (WPARAM)ds->hFontUi, TRUE);
                if (ds->hwndAgentIcon) SendMessageW(ds->hwndAgentIcon, WM_SETFONT, (WPARAM)ds->hFontUi, TRUE);
                SendMessageW(ds->hwndList, WM_SETFONT, (WPARAM)ds->hFontUi, TRUE);
            }
            if (ds->hFontMono) {
                SendMessageW(ds->hwndEditPreview, WM_SETFONT, (WPARAM)ds->hFontMono, TRUE);
            }

            ApplyNativeThemeToWindow(hwnd);
            ReloadStreamDialogData(hwnd, ds);
            if (ds->hwndTooltip && ds->hwndAgentIcon) {
                std::string modelTooltip = ds->agentState.model;
                if (modelTooltip.empty()) {
                    for (const auto& ev : ds->events) {
                        if (!ev.model.empty()) { modelTooltip = ev.model; break; }
                    }
                }
                if (!modelTooltip.empty()) {
                    static wchar_t tipBuf2[512];
                    std::wstring tip = L"Model: " + Utf8ToWide(modelTooltip);
                    wcsncpy_s(tipBuf2, tip.c_str(), _TRUNCATE);
                    TOOLINFOW ti = {};
                    ti.cbSize = sizeof(ti);
                    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
                    ti.hwnd = hwnd;
                    ti.uId = (UINT_PTR)ds->hwndAgentIcon;
                    ti.hinst = GetModuleHandleW(nullptr);
                    ti.lpszText = tipBuf2;
                    SendMessageW(ds->hwndTooltip, TTM_ADDTOOL, 0, (LPARAM)&ti);
                }
            }
            ds->timerId = SetTimer(hwnd, 1, 1000, nullptr);
            return 0;
        }
        case WM_TIMER: {
            if (ds && !(ds->embedded && !IsWindowVisible(hwnd))) {
                ReloadStreamDialogData(hwnd, ds);
                if (ds->hwndTooltip && ds->hwndAgentIcon) {
                    std::string modelTooltip = ds->agentState.model;
                    if (modelTooltip.empty()) {
                        for (const auto& ev : ds->events) {
                            if (!ev.model.empty()) { modelTooltip = ev.model; break; }
                        }
                    }
                    if (!modelTooltip.empty()) {
                        static wchar_t tipBuf3[512];
                        std::wstring tip = L"Model: " + Utf8ToWide(modelTooltip);
                        wcsncpy_s(tipBuf3, tip.c_str(), _TRUNCATE);
                        TOOLINFOW ti = {};
                        ti.cbSize = sizeof(ti);
                        ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
                        ti.hwnd = hwnd;
                        ti.uId = (UINT_PTR)ds->hwndAgentIcon;
                        ti.hinst = GetModuleHandleW(nullptr);
                        ti.lpszText = tipBuf3;
                        SendMessageW(ds->hwndTooltip, TTM_UPDATETIPTEXT, 0, (LPARAM)&ti);
                    }
                }
            }
            return 0;
        }
        case WM_NOTIFY: {
            auto* pnmh = reinterpret_cast<NMHDR*>(lParam);
            if (ds && pnmh && pnmh->hwndFrom == ds->hwndList) {
                if (pnmh->code == LVN_ITEMCHANGED) {
                    auto* pnmlv = reinterpret_cast<NMLISTVIEW*>(lParam);
                    int evIdx = pnmlv->iItem;
                    if (ds->embedded) {
                        // lParam is the row's index in ds->treeRows. A sub-agent row has no event of its own.
                        int rowIdx = static_cast<int>(pnmlv->lParam);
                        bool valid = !ds->rebuilding && rowIdx >= 0 && rowIdx < static_cast<int>(ds->treeRows.size());
                        evIdx = (valid && !ds->treeRows[rowIdx].isBranch) ? ds->treeRows[rowIdx].firstEvent : -1;
                        if (valid && ds->treeRows[rowIdx].isBranch && (pnmlv->uNewState & LVIS_SELECTED)) {
                            const auto& t = ds->treeRows[rowIdx];
                            std::wstringstream sub;
                            sub << L"=== Sub-agent ===\r\n"
                                << L"Description : " << Utf8ToWide(t.target) << L"\r\n"
                                << L"Status      : " << Utf8ToWide(t.kind) << L"\r\n"
                                << L"Tokens      : " << t.tokens << L"\r\n"
                                << L"Tool calls  : " << t.toolUses << L"\r\n";
                            SetWindowTextW(ds->hwndEditPreview, sub.str().c_str());
                        }
                    }
                    if ((pnmlv->uNewState & LVIS_SELECTED) && evIdx >= 0 &&
                        evIdx < static_cast<int>(ds->events.size())) {
                        const auto& ev = ds->events[evIdx];
                        std::wstringstream detail;
                        detail << L"=== Event #" << ev.seq << L" [" << Utf8ToWide(ev.kind) << L"] ===\r\n"
                               << L"Timestamp : " << Utf8ToWide(ev.timestamp) << L"\r\n"
                               << L"Agent     : " << Utf8ToWide(ev.agentType) << L"\r\n";
                        if (!ev.toolName.empty()) {
                            detail << L"Tool      : " << Utf8ToWide(ev.toolName) << L"\r\n";
                        }
                        if (!ev.activity.empty()) {
                            detail << L"Action    : " << Utf8ToWide(ev.activity) << L"\r\n";
                        }
                        if (!ev.toolInput.empty()) {
                            detail << L"Input     : " << Utf8ToWide(ev.toolInput) << L"\r\n";
                        }
                        if (!ev.text.empty()) {
                            detail << L"Content   :\r\n" << Utf8ToWide(ev.text) << L"\r\n";
                        }
                        if (ev.totalTokens > 0) {
                            detail << L"Tokens    : " << ev.totalTokens << L"\r\n";
                        }
                        std::string usageText = MonitorEventUsageText(ev);
                        if (!usageText.empty()) {
                            detail << L"Usage     : " << Utf8ToWide(usageText) << L"\r\n";
                        }
                        detail << L"\r\n=== Raw JSON ===\r\n"
                               << Utf8ToWide(ev.rawJson) << L"\r\n";
                        SetWindowTextW(ds->hwndEditPreview, detail.str().c_str());
                    }
                } else if (pnmh->code == NM_CLICK && ds->embedded) {
                    // Click on the triangle column of a sub-agent row: open or close it.
                    auto* act = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                    if (act->iItem >= 0 && act->iSubItem == 2) {
                        const int ti = MonitorRowAt(ds, act->iItem);
                        if (ti >= 0 && ds->treeRows[ti].isBranch) {
                            ds->treeOverrides[ds->treeRows[ti].agentId] = !ds->treeRows[ti].open;
                            ReloadStreamDialogData(hwnd, ds);
                        }
                    }
                    return 0;
                } else if (pnmh->code == LVN_KEYDOWN && ds->embedded) {
                    // Left / right on a sub-agent row: close / open it.
                    auto* kd = reinterpret_cast<NMLVKEYDOWN*>(lParam);
                    if (kd->wVKey == VK_LEFT || kd->wVKey == VK_RIGHT) {
                        const int ti = MonitorRowAt(ds, ListView_GetNextItem(ds->hwndList, -1, LVNI_SELECTED));
                        const bool wantOpen = kd->wVKey == VK_RIGHT;
                        if (ti >= 0 && ds->treeRows[ti].isBranch && ds->treeRows[ti].open != wantOpen) {
                            ds->treeOverrides[ds->treeRows[ti].agentId] = wantOpen;
                            ReloadStreamDialogData(hwnd, ds);
                        }
                    }
                    return 0;
                } else if (pnmh->code == NM_RCLICK && ds->embedded) {
                    // Right-click on a step: copy the whole row, or only its full target.
                    auto* act = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                    if (act->iItem < 0) return 0;
                    const int rowIdx = MonitorRowAt(ds, act->iItem);
                    if (rowIdx < 0) return 0;
                    const ActivityRow r = ToActivityRow(ds->treeRows[rowIdx]);
                    POINT pt = {};
                    GetCursorPos(&pt);
                    HMENU menu = CreatePopupMenu();
                    AppendMenuW(menu, MF_STRING, 1, Tr(L"行をコピー", L"Copy row", L"Copiar fila"));
                    AppendMenuW(menu, MF_STRING, 2, Tr(L"対象をコピー（全文）", L"Copy target (full text)", L"Copiar objetivo (texto completo)"));
                    int cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
                    DestroyMenu(menu);
                    if (cmd == 1) CopyTextToClipboard(hwnd, ActivityRowCopyText(r));
                    else if (cmd == 2) CopyTextToClipboard(hwnd, Utf8ToWide(r.target));
                    return 0;
                } else if (pnmh->code == LVN_GETINFOTIPW && ds->embedded) {
                    // Hover: the full text of the step, not the shortened cell.
                    auto* tip = reinterpret_cast<NMLVGETINFOTIPW*>(lParam);
                    int rowIdx = static_cast<int>(tip->lParam);
                    if (rowIdx >= 0 && rowIdx < static_cast<int>(ds->treeRows.size()) && tip->pszText && tip->cchTextMax > 0) {
                        const ActivityRow r = ToActivityRow(ds->treeRows[rowIdx]);
                        std::wstring text = ActivityLabel(r.kind);
                        if (!r.target.empty()) text += L"\r\n" + Utf8ToWide(r.target);
                        wcsncpy_s(tip->pszText, tip->cchTextMax, text.c_str(), _TRUNCATE);
                    }
                } else if (pnmh->code == NM_CUSTOMDRAW && ds->embedded) {
                    auto* lvcd = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
                    if (lvcd->nmcd.dwDrawStage == CDDS_PREPAINT) {
                        return CDRF_NOTIFYITEMDRAW;
                    }
                    if (lvcd->nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT) {
                        DrawListRowSeparator(lvcd);
                        return CDRF_DODEFAULT;
                    }
                    if (lvcd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                        bool selected = (lvcd->nmcd.uItemState & CDIS_SELECTED) != 0;
                        lvcd->clrText = g_theme.listText;
                        lvcd->clrTextBk = selected ? g_theme.rowSelected : g_theme.panel;
                        int idx = static_cast<int>(lvcd->nmcd.lItemlParam);  // treeRows index, see RebuildMonitorList
                        if (idx >= 0 && idx < static_cast<int>(ds->treeRows.size())) {
                            const ActivityRow r = ToActivityRow(ds->treeRows[idx]);
                            if (g_activityStyle) {
                                bool longStep = r.startMs > 0 && r.endMs >= r.startMs &&
                                                (r.endMs - r.startMs) >= kLongStepMs;
                                lvcd->clrText = ActivityColor(r.kind);
                                if (!selected) lvcd->clrTextBk = ActivityRowBackground(r.kind, r.ongoing, longStep);
                            }
                        }
                        return CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
                    }
                    return CDRF_DODEFAULT;
                }
            }
            break;
        }
        case WM_SIZE: {
            if (!ds) return 0;
            int w = LOWORD(lParam);
            int h = HIWORD(lParam);
            if (ds->embedded) {
                // Left 2/5: goal text on top, usage graph below. Right 3/5: the subtask list, full height.
                int pad = 4;
                // Goal / Now / Ends / process summary: half the pane height.
                int topH = h / 2;
                int availW = (std::max)(300, w - pad * 2);
                int leftW = availW * 2 / 5 - 4;
                int contentX = pad + 28;
                int textW = (std::max)(160, leftW - 28);
                if (ds->hwndAgentIcon) MoveWindow(ds->hwndAgentIcon, pad, pad, 24, 24, TRUE);
                MoveWindow(ds->hwndHeader, contentX, pad, textW, topH, TRUE);
                int bodyTop = pad + topH + 4;
                int bodyH = (std::max)(30, h - bodyTop - pad);
                // Left column: usage graph above, the time-by-kind chart below it.
                int graphH = bodyH * 55 / 100;
                ds->rcUsageGraph = { pad, bodyTop, pad + leftW, bodyTop + graphH };
                ds->rcKindChart = { pad, bodyTop + graphH + 6, pad + leftW, bodyTop + bodyH };
                int subX = pad + leftW + 8;
                int subW = (std::max)(200, w - pad - subX);
                MoveWindow(ds->hwndList, subX, pad, subW, (std::max)(60, h - pad * 2), TRUE);
                if (ds->hwndEditPreview) ShowWindow(ds->hwndEditPreview, SW_HIDE);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            int pad = 12;
            int availW = (std::max)(200, w - pad * 2);
            int hdrH = 75;
            int contentX = pad + 28;
            int contentW = availW - 28;
            int graphW = (std::max)(180, contentW * 45 / 100);
            int textW = (std::max)(160, contentW - graphW - 8);
            MoveWindow(ds->hwndHeader, contentX, pad, textW, hdrH, TRUE);
            ds->rcUsageGraph = { contentX + textW + 8, pad, contentX + contentW, pad + hdrH };
            if (ds->hwndAgentIcon) MoveWindow(ds->hwndAgentIcon, pad, pad, 24, 24, TRUE);
            int btnY = (std::max)(250, h - 38);
            int middleH = btnY - (pad + hdrH + pad) - pad;
            int listH = (std::max)(80, middleH * 55 / 100);
            int editH = (std::max)(60, middleH - listH - 6);
            MoveWindow(ds->hwndList, pad, pad + hdrH + 6, availW, listH, TRUE);
            MoveWindow(ds->hwndEditPreview, pad, pad + hdrH + 6 + listH + 6, availW, editH, TRUE);
            InvalidateRect(hwnd, &ds->rcUsageGraph, FALSE);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            HDC hdc = BeginPaint(hwnd, &ps);
            if (ds) {
                int selIdx = ds->hwndList ? ListView_GetNextItem(ds->hwndList, -1, LVNI_SELECTED) : -1;
                if (ds->embedded) {
                    // The selected list row (tree index) -> its first event; a sub-agent row selects none.
                    const int ti = MonitorRowAt(ds, selIdx);
                    selIdx = (ti >= 0 && !ds->treeRows[ti].isBranch) ? ds->treeRows[ti].firstEvent : -1;
                }
                const MonitorEvent* sel = (selIdx >= 0 && selIdx < static_cast<int>(ds->events.size()))
                    ? &ds->events[selIdx] : nullptr;
                TimeSpan span = ComputeRunSpan(ds);
                DrawUsageGraph(hdc, ds->rcUsageGraph, ds->events, sel, selIdx, span);
                DrawKindChart(hdc, ds->rcKindChart, ds->rows, span);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!ds) break;
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (PtInRect(&ds->rcUsageGraph, pt)) {
                auto bars = BuildUsageBars(ds->rcUsageGraph, ds->events, ComputeRunSpan(ds));
                for (const auto& b : bars) {
                    if (!PtInRect(&b.rc, pt)) continue;
                    int idx = b.eventIndex;
                    if (idx >= 0 && idx < static_cast<int>(ds->events.size())) {
                        ListView_SetItemState(ds->hwndList, idx,
                                              LVIS_SELECTED | LVIS_FOCUSED,
                                              LVIS_SELECTED | LVIS_FOCUSED);
                        ListView_EnsureVisible(ds->hwndList, idx, FALSE);
                        SetFocus(ds->hwndList);
                        InvalidateRect(hwnd, &ds->rcUsageGraph, FALSE);
                    }
                    break;
                }
                return 0;
            }
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == 2103 && ds) {
                ReloadStreamDialogData(hwnd, ds);
            } else if (id == 2104 && ds) {
                if (!ds->snap.streamFilePath.empty()) {
                    std::wstring streamW = Utf8ToWide(ds->snap.streamFilePath);
                    gui_platform::GetGuiPlatform()->OpenFileInEditor(hwnd, streamW);
                }
            } else if (id == IDOK || id == IDCANCEL) {
                if (ds && ds->embedded) {
                    // The pane belongs to the main window: "Close" hides it.
                    PostMessageW(GetParent(hwnd), WM_COMMAND, MAKEWPARAM(IDC_BTN_STREAM_PANE, 0), 0);
                } else {
                    DestroyWindow(hwnd);
                }
            }
            return 0;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis || !ds) break;
            bool pressed = (dis->itemState & ODS_SELECTED) != 0;
            COLORREF bg = pressed ? g_theme.border : g_theme.panel;
            HBRUSH bgBrush = CreateSolidBrush(bg);
            FillRect(dis->hDC, &dis->rcItem, bgBrush);
            DeleteObject(bgBrush);
            HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
            HGDIOBJ oldPen = SelectObject(dis->hDC, borderPen);
            HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
            Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
            SelectObject(dis->hDC, oldBrush);
            SelectObject(dis->hDC, oldPen);
            DeleteObject(borderPen);
            wchar_t text[256] = {};
            GetWindowTextW(dis->hwndItem, text, 256);
            RECT rc = dis->rcItem;
            InflateRect(&rc, -4, 0);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, g_theme.text);
            HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC, ds->hFontUi));
            DrawTextW(dis->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dis->hDC, oldFont);
            return TRUE;
        }
        case WM_ERASEBKGND: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, g_dialogBgBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, g_theme.text);
            SetBkColor(hdc, g_theme.panel);
            return reinterpret_cast<LRESULT>(g_dialogBgBrush);
        }
        case WM_USER + 101:
            if (ds) ReloadStreamDialogData(hwnd, ds);
            return 0;
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            if (ds) {
                if (ds->embedded && ds->dash) ds->dash->streamPane = nullptr;
                if (ds->timerId) KillTimer(hwnd, ds->timerId);
                if (ds->hFontMono) DeleteObject(ds->hFontMono);
                if (ds->wsClient) {
                    ds->wsClient->Close();
                    ds->wsClient.reset();
                }
                delete ds;
            }
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void RegisterLlmStreamClass() {
    static bool regDone = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!regDone) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = LlmStreamWndProc;
        wc.hInstance = hInst;
        wc.hIcon = LoadIconW(nullptr, IDI_INFORMATION);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"HanchoLlmStreamWnd";
        RegisterClassExW(&wc);
        regDone = true;
    }
}

static void ShowLlmStreamDialog(DashboardState* st, int procId) {
    if (!st || procId <= 0) return;
    ProcessSnapshot snap;
    if (!FetchProcessSnapshotIpc(st, procId, snap)) return;

    RegisterLlmStreamClass();
    HINSTANCE hInst = GetModuleHandleW(nullptr);

    auto* ds = new LlmStreamDlgState();
    ds->dash = st;
    ds->procId = procId;
    ds->snap = snap;
    ds->hFontUi = st->hFontUi;
    ds->hFontBold = st->hFontUiBold;

    std::wstring title = std::wstring(Tr(L"LLM JSON ストリーム・イベント表示", L"LLM JSON Stream & Events", L"Flujo de eventos JSON de LLM"))
        + L" — #" + std::to_wstring(snap.id) + L" [" + Utf8ToWide(snap.name) + L"]";
    if (!snap.agentType.empty()) {
        title += L" (" + Utf8ToWide(snap.agentType) + L")";
    }

    RECT rcParent = {};
    GetWindowRect(st->hwndMain, &rcParent);
    int dlgW = 860;
    int dlgH = 620;
    int x = rcParent.left + ((rcParent.right - rcParent.left) - dlgW) / 2;
    int y = rcParent.top + ((rcParent.bottom - rcParent.top) - dlgH) / 2;

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"HanchoLlmStreamWnd",
        title.c_str(),
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        x, y, dlgW, dlgH,
        st->hwndMain, nullptr, hInst, ds
    );
    if (hDlg) {
        ShowWindow(hDlg, SW_SHOW);
        SetForegroundWindow(hDlg);
    } else {
        delete ds;
    }
}

// Bottom pane under the process / task lists: the stream view of the selected process.
static void EnsureStreamPane(DashboardState* st) {
    if (!st || st->hwndStreamPane) return;
    RegisterLlmStreamClass();
    auto* ds = new LlmStreamDlgState();
    ds->dash = st;
    ds->embedded = true;
    ds->procId = st->selectedProcId;
    ds->hFontUi = st->hFontUi;
    ds->hFontBold = st->hFontUiBold;
    st->streamPane = ds;
    st->hwndStreamPane = CreateWindowExW(0, L"HanchoLlmStreamWnd", L"",
        WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 600, 200,
        st->hwndMain, nullptr, GetModuleHandleW(nullptr), ds);
    if (!st->hwndStreamPane) {
        st->streamPane = nullptr;
        delete ds;
    }
}

static void SyncStreamPane(DashboardState* st) {
    if (!st || !st->streamPane || !st->hwndStreamPane || !IsWindowVisible(st->hwndStreamPane)) return;
    LlmStreamDlgState* ds = st->streamPane;
    if (ds->procId == st->selectedProcId && ds->goalJobKey == st->selectedJobKey) return;
    ds->procId = st->selectedProcId;
    ds->lastEventCount = static_cast<size_t>(-1);  // force the list to rebuild
    if (ds->hwndEditPreview) SetWindowTextW(ds->hwndEditPreview, L"");
    ReloadStreamDialogData(st->hwndStreamPane, ds);
}

static void ApplyStreamPaneTheme(DashboardState* st) {
    if (!st || !st->streamPane || !st->streamPane->hwndList) return;
    HWND hList = st->streamPane->hwndList;
    ListView_SetBkColor(hList, g_theme.panel);
    ListView_SetTextBkColor(hList, g_theme.panel);
    ListView_SetTextColor(hList, g_theme.listText);
    RedrawWindow(hList, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
    ApplyNativeThemeToWindow(st->hwndStreamPane);
    InvalidateRect(st->hwndStreamPane, nullptr, TRUE);
}

static void ShowTaskContextMenu(DashboardState* st, int procId, POINT ptScreen) {
    if (!st || procId <= 0) return;
    ProcessSnapshot snap;
    if (!FetchProcessSnapshotIpc(st, procId, snap)) return;

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, 3001, Tr(L"詳細を表示 (タスク詳細・コマンドライン)...", L"Task Details & Command Line...", L"Detalles de tarea y línea de comandos..."));
    AppendMenuW(hMenu, MF_STRING, 3009, Tr(L"JSON ストリームをボトムペインに表示", L"Show JSON Stream in Bottom Pane", L"Mostrar flujo JSON en el panel inferior"));
    AppendMenuW(hMenu, MF_STRING, 3007, Tr(L"LLM JSON ストリームを別ウィンドウで開く...", L"Open LLM JSON Stream in a Window...", L"Abrir flujo JSON de LLM en una ventana..."));
    AppendMenuW(hMenu, MF_STRING, 3002, Tr(L"コマンドラインをコピー", L"Copy Command Line", L"Copiar línea de comandos"));
    if (!snap.logFilePath.empty()) {
        AppendMenuW(hMenu, MF_STRING, 3003, Tr(L"ログファイルを開く", L"Open Log File", L"Abrir archivo de registro"));
    }
    if (!snap.streamFilePath.empty()) {
        AppendMenuW(hMenu, MF_STRING, 3008, Tr(L"ストリームファイル (jsonl) を開く", L"Open Stream File (jsonl)", L"Abrir archivo de flujo (jsonl)"));
    }
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    if (snap.state == ProcState::Running) {
        AppendMenuW(hMenu, MF_STRING, 3004, Tr(L"ツリー終了", L"Kill Tree", L"Terminar árbol"));
    } else {
        AppendMenuW(hMenu, MF_STRING, 3005, Tr(L"この履歴を削除", L"Remove This Entry", L"Eliminar esta entrada"));
        AppendMenuW(hMenu, MF_STRING, 3006, Tr(L"終了履歴をすべて消去", L"Clear All Ended History", L"Borrar todo el historial finalizado"));
    }

    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, ptScreen.x, ptScreen.y, 0, st->hwndMain, nullptr);
    DestroyMenu(hMenu);

    if (cmd == 3001) {
        ShowTaskDetailsDialog(st, procId);
    } else if (cmd == 3007) {
        ShowLlmStreamDialog(st, procId);
    } else if (cmd == 3009) {
        st->selectedProcId = procId;
        if (!st->showStreamPane || st->hideProcessList) {
            st->showStreamPane = true;
            st->hideProcessList = false;
            UpdatePanelToggleLabels(st);
            SaveShellConfig(st);
            RECT rcClient = {};
            GetClientRect(st->hwndMain, &rcClient);
            LayoutControls(st, rcClient.right, rcClient.bottom);
            InvalidateRect(st->hwndMain, nullptr, FALSE);
        }
        SyncStreamPane(st);
    } else if (cmd == 3008) {
        std::wstring streamW = Utf8ToWide(snap.streamFilePath);
        gui_platform::GetGuiPlatform()->OpenFileInEditor(st->hwndMain, streamW);
    } else if (cmd == 3002) {
        CopyTextToClipboard(st->hwndMain, Utf8ToWide(snap.commandLine));
    } else if (cmd == 3003) {
        std::wstring logW = Utf8ToWide(snap.logFilePath);
        gui_platform::GetGuiPlatform()->OpenFileInEditor(st->hwndMain, logW);
    } else if (cmd == 3004) {
        if (gui_platform::GetGuiPlatform()->ShowMessageBox(
                st->hwndMain,
                Tr(L"ツリー終了の確認", L"Confirm Kill Tree", L"Confirmar cierre"),
                Tr(L"このプロセスツリーを強制終了しますか？",
                   L"Are you sure you want to kill this process tree?",
                   L"¿Seguro que desea terminar este árbol de procesos?"),
                gui_platform::MessageBoxType::QuestionYesNo,
                true) == gui_platform::MessageBoxResult::Yes) {
            std::string err;
            KillProcessIpc(st, procId, err);
            RefreshDashboardData(st);
        }
    } else if (cmd == 3005) {
        RemoveProcessIpc(st, procId);
        RefreshDashboardData(st);
    } else if (cmd == 3006) {
        ClearFinishedIpc(st);
        RefreshDashboardData(st);
    }
}

static std::string GetDefaultBashCommand() {
    const wchar_t* gitBash = L"C:\\Program Files\\Git\\bin\\bash.exe";
    DWORD attr = GetFileAttributesW(gitBash);
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return "\"C:\\Program Files\\Git\\bin\\bash.exe\" --login -i";
    }
    return "bash.exe";
}

static std::wstring GetConfigFilePathWide() {
    return GetLogsDirectoryWide() + L"\\config.json";
}

// Sets TIMER_REFRESH_ID to the configured interval, doubled while nothing is running.
// Only calls SetTimer when the period actually changes.
static void ApplyDashboardRefreshTimer(DashboardState* st) {
    if (!st || !st->hwndMain) return;
    const int ms = st->refreshIntervalMs * (st->runningTotalCount > 0 ? 1 : 2);
    if (ms == st->appliedRefreshMs) return;
    st->appliedRefreshMs = ms;
    SetTimer(st->hwndMain, TIMER_REFRESH_ID, static_cast<UINT>(ms), nullptr);
}

// Recreates the process / task list fonts at the configured sizes and applies them to the lists.
static void ApplyListFonts(DashboardState* st) {
    if (!st) return;
    auto makeFont = [](int px) {
        return CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    HFONT oldProc = st->hFontProcList;
    HFONT oldTask = st->hFontTaskList;
    st->hFontProcList = makeFont(st->processListFontPx);
    st->hFontTaskList = makeFont(st->taskListFontPx);
    if (st->hwndList) {
        SendMessageW(st->hwndList, WM_SETFONT, reinterpret_cast<WPARAM>(st->hFontProcList), TRUE);
        InvalidateRect(st->hwndList, nullptr, TRUE);
    }
    if (st->hwndTaskList) {
        SendMessageW(st->hwndTaskList, WM_SETFONT, reinterpret_cast<WPARAM>(st->hFontTaskList), TRUE);
        InvalidateRect(st->hwndTaskList, nullptr, TRUE);
    }
    // The lists no longer reference the old fonts, so they can be released.
    if (oldProc) DeleteObject(oldProc);
    if (oldTask) DeleteObject(oldTask);
}

static void LoadShellConfig(DashboardState* st) {
    if (!st) return;
    st->defaultShell = ShellType::PowerShell;
    st->cmdCommand = "cmd.exe";
    st->powershellCommand = "powershell.exe";
    st->bashCommand = GetDefaultBashCommand();

    std::string content;
    nlohmann::json req = {{"action", "config_get"}};
    nlohmann::json resp;
    if (GuiIpcCall(st, req, resp) && resp.value("ok", false) && resp.contains("config")) {
        if (resp["config"].is_string()) {
            content = resp["config"].get<std::string>();
        } else if (resp["config"].is_object()) {
            content = resp["config"].dump();
        }
    }
    if (content.empty()) {
        content = ReadFileUtf8Wide(GetConfigFilePathWide());
    }
    if (content.empty()) return;

    std::string defShell = JsonGetString(content, "default_shell", "powershell");
    if (defShell == "cmd") st->defaultShell = ShellType::Cmd;
    else if (defShell == "bash") st->defaultShell = ShellType::Bash;
    else st->defaultShell = ShellType::PowerShell;

    st->cmdCommand = JsonGetString(content, "cmd_command", st->cmdCommand);
    st->powershellCommand = JsonGetString(content, "powershell_command", st->powershellCommand);
    st->bashCommand = JsonGetString(content, "bash_command", st->bashCommand);
    st->hideEndedProcesses = JsonGetBool(content, "hide_ended", false);
    st->hideProcessList = JsonGetBool(content, "hide_process_list", false);
    st->hideTerminalGrid = JsonGetBool(content, "hide_terminal_grid", true);
    st->completionSoundEnabled = JsonGetBool(content, "completion_sound_enabled", true);
    st->completionSoundPath = JsonGetString(content, "completion_sound_path", "");
    st->endBlinkEnabled = JsonGetBool(content, "end_blink_enabled", true);
    st->endBlinkSeconds = ClampEndBlinkSeconds(
        static_cast<int>(JsonGetInt(content, "end_blink_seconds", kDefaultEndBlinkSec)));
    int listH = static_cast<int>(JsonGetInt(content, "process_list_height", st->processListHeight));
    if (listH >= kMinListH && listH <= 4000) st->processListHeight = listH;
    int modeVal = static_cast<int>(JsonGetInt(content, "list_display_mode", static_cast<int64_t>(ListDisplayMode::TwoRows)));
    if (modeVal >= 0 && modeVal <= 2) {
        st->listDisplayMode = static_cast<ListDisplayMode>(modeVal);
    }
    st->showStreamPane = JsonGetBool(content, "show_stream_pane", true);
    int paneH = static_cast<int>(JsonGetInt(content, "stream_pane_height", st->streamPaneHeight));
    if (paneH >= kMinStreamPaneH && paneH <= 4000) st->streamPaneHeight = paneH;
    g_activityStyle = JsonGetBool(content, "activity_style", true);
    int ratioPct = static_cast<int>(JsonGetInt(content, "proc_row_ratio", 50));
    if (ratioPct >= 15 && ratioPct <= 85) {
        st->procRowRatio = ratioPct / 100.0f;
    }
    std::string langCode = JsonGetString(content, "language", "");
    if (!langCode.empty()) g_uiLanguage = ParseLanguage(langCode);
    std::string themeCode = JsonGetString(content, "theme", "");
    if (!themeCode.empty()) g_uiTheme = ParseTheme(themeCode);
    int satPct = static_cast<int>(JsonGetInt(content, "kpi_saturation", 100));
    if (satPct < 10) satPct = 10;
    if (satPct > 100) satPct = 100;
    g_kpiSaturation = satPct / 100.0f;
    int graphMode = static_cast<int>(JsonGetInt(content, "usage_graph_mode", 0));
    if (graphMode < 0 || graphMode > 1) graphMode = 0;
    st->usageGraphMode = graphMode;
    st->restoreUsageGraph = JsonGetBool(content, "restore_usage_graph", false);
    SetUsageHistoryPersistence(st->restoreUsageGraph);
    st->llmMaxEvents = ClampLlmMaxEvents(JsonGetInt(content, "llm_max_events", kDefaultLlmMaxEvents));
    SetLlmMaxEvents(st->llmMaxEvents);
    st->resourceLocks = JsonGetBool(content, "resource_locks", true);
    SetResourceLocksEnabledIpc(st, st->resourceLocks);
    st->memoryMaxBytes = JsonGetInt(content, "memory_max_bytes", 0);
    if (st->memoryMaxBytes == 0) {
        st->memoryMaxBytes = JsonGetInt(content, "memory_limit_bytes", 0);
    }
    if (st->memoryMaxBytes == 0) {
        int64_t mb = JsonGetInt(content, "memory_max_mb", 0);
        if (mb > 0) st->memoryMaxBytes = mb * 1024LL * 1024LL;
    }
    st->processListFontPx = ClampListFontPx(static_cast<int>(
        JsonGetInt(content, "process_list_font_px", kDefaultListFontPx)));
    st->taskListFontPx = ClampListFontPx(static_cast<int>(
        JsonGetInt(content, "task_list_font_px", kDefaultListFontPx)));
    st->refreshIntervalMs = ClampRefreshIntervalMs(
        static_cast<int>(JsonGetInt(content, "refresh_interval_ms", kDefaultRefreshIntervalMs)));
    SettingsManager::Instance().SetTabGridRefreshIntervalMs(st->refreshIntervalMs);
    st->showSupervisorHints = JsonGetBool(content, "show_supervisor_hints", true);
    st->hintAutoDismissSec = static_cast<int>(JsonGetInt(content, "hint_auto_dismiss_sec", 6));
    st->supervisorCommand = JsonGetString(content, "supervisor_command", "claude");
    for (int m = 0; m < kListRowModeCount; ++m) {
        for (int k = 0; k < kListRowKeyCount; ++k) {
            COLORREF c = 0;
            g_listRowBgSet[m][k] = ParseHexColor(JsonGetString(content, ListRowConfigKey(m, k), ""), c);
            if (g_listRowBgSet[m][k]) g_listRowBg[m][k] = c;
        }
    }
    ApplyTheme();
}

static void SaveShellConfig(const DashboardState* st) {
    if (!st) return;
    std::string defShell = "powershell";
    if (st->defaultShell == ShellType::Cmd) defShell = "cmd";
    else if (st->defaultShell == ShellType::Bash) defShell = "bash";

    std::ostringstream listBg;
    for (int m = 0; m < kListRowModeCount; ++m) {
        for (int k = 0; k < kListRowKeyCount; ++k) {
            listBg << "  \"" << ListRowConfigKey(m, k) << "\": \""
                   << (g_listRowBgSet[m][k] ? FormatHexColor(g_listRowBg[m][k]) : std::string()) << "\",\n";
        }
    }

    std::ostringstream ss;
    ss << "{\n"
       << "  \"default_shell\": \"" << JsonEscape(defShell) << "\",\n"
       << "  \"cmd_command\": \"" << JsonEscape(st->cmdCommand) << "\",\n"
       << "  \"powershell_command\": \"" << JsonEscape(st->powershellCommand) << "\",\n"
       << "  \"bash_command\": \"" << JsonEscape(st->bashCommand) << "\",\n"
       << "  \"hide_ended\": " << (st->hideEndedProcesses ? "true" : "false") << ",\n"
       << "  \"hide_process_list\": " << (st->hideProcessList ? "true" : "false") << ",\n"
       << "  \"hide_terminal_grid\": " << (st->hideTerminalGrid ? "true" : "false") << ",\n"
       << "  \"completion_sound_enabled\": " << (st->completionSoundEnabled ? "true" : "false") << ",\n"
       << "  \"completion_sound_path\": \"" << JsonEscape(st->completionSoundPath) << "\",\n"
       << "  \"end_blink_enabled\": " << (st->endBlinkEnabled ? "true" : "false") << ",\n"
       << "  \"end_blink_seconds\": " << st->endBlinkSeconds << ",\n"
       << "  \"process_list_height\": " << st->processListHeight << ",\n"
       << "  \"list_display_mode\": " << static_cast<int>(st->listDisplayMode) << ",\n"
       << "  \"proc_row_ratio\": " << static_cast<int>(st->procRowRatio * 100.0f + 0.5f) << ",\n"
       << "  \"show_stream_pane\": " << (st->showStreamPane ? "true" : "false") << ",\n"
       << "  \"stream_pane_height\": " << st->streamPaneHeight << ",\n"
       << "  \"activity_style\": " << (g_activityStyle ? "true" : "false") << ",\n"
       << "  \"language\": \"" << LanguageCode(g_uiLanguage) << "\",\n"
        << "  \"theme\": \"" << ThemeCode(g_uiTheme) << "\",\n"
        << "  \"kpi_saturation\": " << static_cast<int>(g_kpiSaturation * 100 + 0.5f) << ",\n"
        << "  \"usage_graph_mode\": " << st->usageGraphMode << ",\n"
        << "  \"restore_usage_graph\": " << (st->restoreUsageGraph ? "true" : "false") << ",\n"
        << "  \"llm_max_events\": " << st->llmMaxEvents << ",\n"
        << "  \"resource_locks\": " << (st->resourceLocks ? "true" : "false") << ",\n"
        << "  \"memory_max_bytes\": " << st->memoryMaxBytes << ",\n"
        << "  \"process_list_font_px\": " << st->processListFontPx << ",\n"
        << "  \"task_list_font_px\": " << st->taskListFontPx << ",\n"
        << "  \"show_supervisor_hints\": " << (st->showSupervisorHints ? "true" : "false") << ",\n"
        << "  \"hint_auto_dismiss_sec\": " << st->hintAutoDismissSec << ",\n"
        << "  \"supervisor_command\": \"" << JsonEscape(st->supervisorCommand) << "\",\n"
        << listBg.str()
        << "  \"refresh_interval_ms\": " << st->refreshIntervalMs << "\n"
        << "}\n";

    std::string serialized = ss.str();
    nlohmann::json cfgJson = nlohmann::json::parse(serialized, nullptr, false);
    if (!cfgJson.is_discarded()) {
        // Preserve settings managed outside the dashboard (webui, memory, ...).
        // The old config is read before writing so this save does not drop them.
        std::string oldContent = ReadFileUtf8Wide(GetConfigFilePathWide());
        nlohmann::json oldJson = nlohmann::json::parse(oldContent, nullptr, false);
        if (oldJson.is_object()) {
            for (auto it = oldJson.begin(); it != oldJson.end(); ++it) {
                if (!cfgJson.contains(it.key())) cfgJson[it.key()] = it.value();
            }
        }
        serialized = cfgJson.dump(2);
        WriteFileAtomicWide(GetConfigFilePathWide(), serialized);

        // The file already carries "webui"; do not send it back as a patch or an
        // unrelated settings save would restart the WebUI listener.
        nlohmann::json ipcJson = cfgJson;
        ipcJson.erase("webui");
        nlohmann::json req = {{"action", "config_set"}, {"config", ipcJson}};
        nlohmann::json resp;
        GuiIpcCall(const_cast<DashboardState*>(st), req, resp);
    } else {
        WriteFileAtomicWide(GetConfigFilePathWide(), serialized);
    }
}

static void UpdateLaunchButtonLabel(DashboardState* st) {
    if (!st || !st->hwndBtnStart) return;
    std::wstring label = Tr(L"+ 起動 (", L"+ Launch (", L"+ Iniciar (");
    if (st->defaultShell == ShellType::Cmd) label += L"cmd";
    else if (st->defaultShell == ShellType::Bash) label += L"bash";
    else label += L"powershell";
    label += L")";
    SetWindowTextW(st->hwndBtnStart, label.c_str());
}

static void ApplyListTheme(DashboardState* st) {
    if (!st || !st->hwndList) return;
    ListView_SetBkColor(st->hwndList, g_theme.panel);
    ListView_SetTextBkColor(st->hwndList, g_theme.panel);
    ListView_SetTextColor(st->hwndList, g_theme.listText);
    InvalidateRect(st->hwndList, nullptr, TRUE);
    RedrawWindow(st->hwndList, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
    HWND hHeader = ListView_GetHeader(st->hwndList);
    if (hHeader) InvalidateRect(hHeader, nullptr, TRUE);
    if (st->hwndTaskList) {
        ListView_SetBkColor(st->hwndTaskList, g_theme.panel);
        ListView_SetTextBkColor(st->hwndTaskList, g_theme.panel);
        ListView_SetTextColor(st->hwndTaskList, g_theme.listText);
        InvalidateRect(st->hwndTaskList, nullptr, TRUE);
        RedrawWindow(st->hwndTaskList, nullptr, nullptr, RDW_FRAME | RDW_INVALIDATE);
        HWND hTaskHeader = ListView_GetHeader(st->hwndTaskList);
        if (hTaskHeader) InvalidateRect(hTaskHeader, nullptr, TRUE);
    }
    if (st->hwndTab) InvalidateRect(st->hwndTab, nullptr, TRUE);
    ApplyStreamPaneTheme(st);
}

static void UpdateClock(DashboardState* st) {
    if (!st || !st->hwndClock) return;
    SYSTEMTIME t = {};
    GetLocalTime(&t);
    wchar_t buf[16] = {};
    swprintf_s(buf, L"%02d:%02d:%02d", t.wHour, t.wMinute, t.wSecond);
    wchar_t shown[16] = {};
    GetWindowTextW(st->hwndClock, shown, 16);
    if (wcscmp(shown, buf) == 0) return;  // called 4x a second; repaint only when the second changes
    SetWindowTextW(st->hwndClock, buf);
    InvalidateRect(st->hwndClock, nullptr, TRUE);
}

static void UpdateToolbarLabels(DashboardState* st) {
    if (!st) return;
    UpdateLaunchButtonLabel(st);
    if (st->hwndBtnSettings)
        SetWindowTextW(st->hwndBtnSettings, Tr(L"設定", L"Settings", L"Ajustes"));
    if (st->hwndBtnKillAll)
        SetWindowTextW(st->hwndBtnKillAll, Tr(L"全終了", L"Kill All", L"Terminar todo"));
    if (st->hwndBtnGridView)
        SetWindowTextW(st->hwndBtnGridView, L"▦");
    if (st->hwndBtnToggleList)
        SetWindowTextW(st->hwndBtnToggleList, L"☰");
    if (st->hwndBtnToggleTerm)
        SetWindowTextW(st->hwndBtnToggleTerm, L">_");
    if (st->hwndBtnHideTray)
        SetWindowTextW(st->hwndBtnHideTray, L"⤓");
    if (st->hwndBtnHint)
        SetWindowTextW(st->hwndBtnHint, L"💡");
    if (st->hwndBtnSupervisor)
        SetWindowTextW(st->hwndBtnSupervisor, L"🤖");

    if (st->hwndToolbarTip) {
        if (st->hwndBtnGridView)
            UpdateControlTooltip(st->hwndToolbarTip, st->hwndMain, st->hwndBtnGridView,
                Tr(L"グリッド表示", L"Grid View", L"Vista de cuadrícula"));
        if (st->hwndBtnHideTray)
            UpdateControlTooltip(st->hwndToolbarTip, st->hwndMain, st->hwndBtnHideTray,
                Tr(L"トレイに格納", L"Tray", L"Bandeja"));
        if (st->hwndBtnHint)
            UpdateControlTooltip(st->hwndToolbarTip, st->hwndMain, st->hwndBtnHint,
                Tr(L"Supervisor ガイダンス・ヒント", L"Supervisor Guidance / Hints", L"Guía de Supervisor"));
        if (st->hwndBtnSupervisor)
            UpdateControlTooltip(st->hwndToolbarTip, st->hwndMain, st->hwndBtnSupervisor,
                Tr(L"Supervisor LLM を起動", L"Launch Supervisor LLM", L"Iniciar Supervisor LLM"));
    }
    UpdatePanelToggleLabels(st);
    if (st->hwndBtnViewTasks) {
        const wchar_t* btnLabel = Tr(L"表示(上下)", L"View: 2 Rows", L"Vista: 2 Filas");
        if (st->listDisplayMode == ListDisplayMode::ProcessOnly) {
            btnLabel = Tr(L"表示: プロセス", L"View: Process Only", L"Vista: Solo Proc");
        } else if (st->listDisplayMode == ListDisplayMode::TaskOnly) {
            btnLabel = Tr(L"表示: タスク", L"View: Task Only", L"Vista: Solo Tareas");
        }
        SetWindowTextW(st->hwndBtnViewTasks, btnLabel);
    }
}

static const wchar_t* const kMainWindowTitle = L"Hancho";

// Adds the --data-dir override to the title so that several dashboards can be told apart.
static std::wstring MainWindowTitle() {
    std::wstring title = kMainWindowTitle;
    const std::wstring dataDir = GetDataDirectoryOverride();
    if (!dataDir.empty()) {
        title += L" - ";
        title += dataDir;
    }
    return title;
}

static void ApplyLanguageToUi(DashboardState* st) {
    if (!st) return;
    SetWindowTextW(st->hwndMain, MainWindowTitle().c_str());
    UpdateToolbarLabels(st);
    UpdateListHeaderSortIndicator(st);
    UpdateTaskListHeaders(st);
    RefreshDashboardData(st);
    InvalidateRect(st->hwndMain, nullptr, FALSE);
}

static void LaunchShellByType(DashboardState* st, ShellType type) {
    if (!st) return;
    StartProcessRequest req;
    req.mode = ExecMode::ConPTY;
    if (type == ShellType::Cmd) {
        req.name = "cmd";
        req.commandLine = st->cmdCommand.empty() ? "cmd.exe" : st->cmdCommand;
    } else if (type == ShellType::Bash) {
        req.name = "bash";
        req.commandLine = st->bashCommand.empty() ? GetDefaultBashCommand() : st->bashCommand;
    } else {
        req.name = "powershell";
        req.commandLine = st->powershellCommand.empty() ? "powershell.exe" : st->powershellCommand;
    }

    std::string err;
    int id = -1;
    if (st->pm) {
        id = st->pm->StartProcess(req, err);
    } else {
        nlohmann::json startReq = {
            {"action", "start"},
            {"cmd", req.commandLine},
            {"name", req.name},
            {"mode", ExecModeToString(req.mode)},
            {"wait", false}
        };
        nlohmann::json startResp;
        if (GuiIpcCall(st, startReq, startResp, &err) && startResp.value("ok", false)) {
            id = startResp.value("id", -1);
        }
    }
    if (id < 0) {
        std::wstring werr = Utf8ToWide(err);
        gui_platform::GetGuiPlatform()->ShowMessageBox(st->hwndMain, L"Failed to Start Terminal", werr, gui_platform::MessageBoxType::Error);
    } else {
        st->selectedProcId = id;
        RefreshDashboardData(st);
        TerminalView* tv = st->pm ? st->pm->GetTerminalView(id) : nullptr;
        if (tv && tv->Hwnd()) {
            SetFocus(tv->Hwnd());
        }
    }
}

static void LaunchFromToolbar(DashboardState* st) {
    if (!st) return;
    LaunchShellByType(st, st->defaultShell);
}

struct ShellConfigDlgState {
    DashboardState* dash = nullptr;
    HWND hwndRadCmd = nullptr;
    HWND hwndRadPs = nullptr;
    HWND hwndRadBash = nullptr;
    HWND hwndEditCmd = nullptr;
    HWND hwndEditPs = nullptr;
    HWND hwndEditBash = nullptr;
    ShellType selectedShell = ShellType::PowerShell;
};

static void ApplyShellConfigFromDialog(HWND hwnd, ShellConfigDlgState* cs) {
    if (!cs || !cs->dash) return;
    cs->dash->defaultShell = cs->selectedShell;

    wchar_t buf[1024] = {};
    GetWindowTextW(cs->hwndEditCmd, buf, 1023);
    cs->dash->cmdCommand = WideToUtf8(buf);
    GetWindowTextW(cs->hwndEditPs, buf, 1023);
    cs->dash->powershellCommand = WideToUtf8(buf);
    GetWindowTextW(cs->hwndEditBash, buf, 1023);
    cs->dash->bashCommand = WideToUtf8(buf);

    SaveShellConfig(cs->dash);
    UpdateLaunchButtonLabel(cs->dash);
}

static LRESULT CALLBACK ShellConfigWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* cs = reinterpret_cast<ShellConfigDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* createStruct = reinterpret_cast<CREATESTRUCTW*>(lParam);
            cs = reinterpret_cast<ShellConfigDlgState*>(createStruct->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs));
            DashboardState* st = cs->dash;

            HWND hTitle = CreateWindowExW(0, L"STATIC",
                Tr(L"Launch Terminal のデフォルトシェルと起動コマンド:",
                   L"Default shell and startup command for Launch Terminal:",
                   L"Shell predeterminado y comando de inicio para Launch Terminal:"),
                WS_CHILD | WS_VISIBLE, 16, 14, 540, 20, hwnd, nullptr, nullptr, nullptr);

            cs->hwndRadCmd = CreateWindowExW(0, L"BUTTON", L"cmd (Command Prompt)",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                16, 44, 190, 24, hwnd, (HMENU)5001, nullptr, nullptr);
            cs->hwndRadPs = CreateWindowExW(0, L"BUTTON", L"powershell (PowerShell)",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                16, 84, 190, 24, hwnd, (HMENU)5002, nullptr, nullptr);
            cs->hwndRadBash = CreateWindowExW(0, L"BUTTON", L"bash (Git Bash / WSL)",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                16, 124, 190, 24, hwnd, (HMENU)5003, nullptr, nullptr);

            cs->hwndEditCmd = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", Utf8ToWide(st->cmdCommand).c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 212, 44, 260, 24, hwnd, (HMENU)5011, nullptr, nullptr);
            cs->hwndEditPs = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", Utf8ToWide(st->powershellCommand).c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 212, 84, 260, 24, hwnd, (HMENU)5012, nullptr, nullptr);
            cs->hwndEditBash = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", Utf8ToWide(st->bashCommand).c_str(),
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 212, 124, 260, 24, hwnd, (HMENU)5013, nullptr, nullptr);

            HWND hBtnRunCmd = CreateWindowExW(0, L"BUTTON", Tr(L"cmd を起動", L"Launch cmd", L"Iniciar cmd"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 480, 44, 140, 24, hwnd, (HMENU)5021, nullptr, nullptr);
            HWND hBtnRunPs = CreateWindowExW(0, L"BUTTON", Tr(L"powershell を起動", L"Launch powershell", L"Iniciar powershell"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 480, 84, 140, 24, hwnd, (HMENU)5022, nullptr, nullptr);
            HWND hBtnRunBash = CreateWindowExW(0, L"BUTTON", Tr(L"bash を起動", L"Launch bash", L"Iniciar bash"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 480, 124, 140, 24, hwnd, (HMENU)5023, nullptr, nullptr);

            cs->selectedShell = st->defaultShell;
            SendMessageW(cs->hwndRadCmd, BM_SETCHECK, st->defaultShell == ShellType::Cmd ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(cs->hwndRadPs, BM_SETCHECK, st->defaultShell == ShellType::PowerShell ? BST_CHECKED : BST_UNCHECKED, 0);
            SendMessageW(cs->hwndRadBash, BM_SETCHECK, st->defaultShell == ShellType::Bash ? BST_CHECKED : BST_UNCHECKED, 0);

            HWND hBtnSave = CreateWindowExW(0, L"BUTTON", Tr(L"保存して閉じる", L"Save and Close", L"Guardar y cerrar"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 330, 170, 140, 30, hwnd, (HMENU)IDOK, nullptr, nullptr);
            HWND hBtnCancel = CreateWindowExW(0, L"BUTTON", Tr(L"キャンセル", L"Cancel", L"Cancelar"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 480, 170, 140, 30, hwnd, (HMENU)IDCANCEL, nullptr, nullptr);

            SendMessageW(hTitle, WM_SETFONT, (WPARAM)st->hFontUiBold, TRUE);
            HWND ctrls[] = {
                cs->hwndRadCmd, cs->hwndRadPs, cs->hwndRadBash,
                cs->hwndEditCmd, cs->hwndEditPs, cs->hwndEditBash,
                hBtnRunCmd, hBtnRunPs, hBtnRunBash, hBtnSave, hBtnCancel
            };
            for (HWND h : ctrls) {
                SendMessageW(h, WM_SETFONT, (WPARAM)st->hFontUi, TRUE);
            }
            ApplyNativeThemeToWindow(hwnd);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDOK && cs) {
                ApplyShellConfigFromDialog(hwnd, cs);
                DestroyWindow(hwnd);
            } else if (id == IDCANCEL) {
                DestroyWindow(hwnd);
            } else if ((id == 5001 || id == 5002 || id == 5003) && cs) {
                cs->selectedShell = (id == 5001) ? ShellType::Cmd :
                                    (id == 5003) ? ShellType::Bash : ShellType::PowerShell;
                SendMessageW(cs->hwndRadCmd, BM_SETCHECK, id == 5001 ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(cs->hwndRadPs, BM_SETCHECK, id == 5002 ? BST_CHECKED : BST_UNCHECKED, 0);
                SendMessageW(cs->hwndRadBash, BM_SETCHECK, id == 5003 ? BST_CHECKED : BST_UNCHECKED, 0);
                InvalidateRect(hwnd, nullptr, TRUE);
            } else if ((id == 5021 || id == 5022 || id == 5023) && cs && cs->dash) {
                if (id == 5021) {
                    cs->selectedShell = ShellType::Cmd;
                    SendMessageW(cs->hwndRadCmd, BM_SETCHECK, BST_CHECKED, 0);
                    SendMessageW(cs->hwndRadPs, BM_SETCHECK, BST_UNCHECKED, 0);
                    SendMessageW(cs->hwndRadBash, BM_SETCHECK, BST_UNCHECKED, 0);
                } else if (id == 5022) {
                    cs->selectedShell = ShellType::PowerShell;
                    SendMessageW(cs->hwndRadCmd, BM_SETCHECK, BST_UNCHECKED, 0);
                    SendMessageW(cs->hwndRadPs, BM_SETCHECK, BST_CHECKED, 0);
                    SendMessageW(cs->hwndRadBash, BM_SETCHECK, BST_UNCHECKED, 0);
                } else if (id == 5023) {
                    cs->selectedShell = ShellType::Bash;
                    SendMessageW(cs->hwndRadCmd, BM_SETCHECK, BST_UNCHECKED, 0);
                    SendMessageW(cs->hwndRadPs, BM_SETCHECK, BST_UNCHECKED, 0);
                    SendMessageW(cs->hwndRadBash, BM_SETCHECK, BST_CHECKED, 0);
                }
                ApplyShellConfigFromDialog(hwnd, cs);
                DashboardState* dash = cs->dash;
                ShellType target = dash->defaultShell;
                DestroyWindow(hwnd);
                LaunchShellByType(dash, target);
            }
            return 0;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis || !cs) break;
            int id = static_cast<int>(dis->CtlID);
            bool radio = (id == 5001 || id == 5002 || id == 5003);
            bool checked = (id == 5001 && cs->selectedShell == ShellType::Cmd) ||
                           (id == 5002 && cs->selectedShell == ShellType::PowerShell) ||
                           (id == 5003 && cs->selectedShell == ShellType::Bash);
            bool pressed = (dis->itemState & ODS_SELECTED) != 0;
            COLORREF bg = pressed ? g_theme.border : g_theme.panel;
            HBRUSH bgBrush = CreateSolidBrush(bg);
            FillRect(dis->hDC, &dis->rcItem, bgBrush);
            DeleteObject(bgBrush);
            HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
            HGDIOBJ oldPen = SelectObject(dis->hDC, borderPen);
            HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
            Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
            SelectObject(dis->hDC, oldBrush);
            SelectObject(dis->hDC, oldPen);
            DeleteObject(borderPen);

            if (dis->itemState & ODS_FOCUS) {
                RECT rcFocus = dis->rcItem;
                InflateRect(&rcFocus, -2, -2);
                DrawFocusRect(dis->hDC, &rcFocus);
            }

            RECT textRc = dis->rcItem;
            if (radio) {
                int cx = dis->rcItem.left + 14;
                int cy = (dis->rcItem.top + dis->rcItem.bottom) / 2;
                HPEN circlePen = CreatePen(PS_SOLID, 1, g_theme.muted);
                HGDIOBJ op = SelectObject(dis->hDC, circlePen);
                HGDIOBJ ob = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
                Ellipse(dis->hDC, cx - 6, cy - 6, cx + 6, cy + 6);
                SelectObject(dis->hDC, ob);
                SelectObject(dis->hDC, op);
                DeleteObject(circlePen);
                if (checked) {
                    HBRUSH dot = CreateSolidBrush(g_theme.accent);
                    HGDIOBJ ob2 = SelectObject(dis->hDC, dot);
                    Ellipse(dis->hDC, cx - 3, cy - 3, cx + 3, cy + 3);
                    SelectObject(dis->hDC, ob2);
                    DeleteObject(dot);
                }
                textRc.left += 28;
            }
            InflateRect(&textRc, -4, 0);
            wchar_t text[256] = {};
            GetWindowTextW(dis->hwndItem, text, 256);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, g_theme.text);
            HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC, cs->dash->hFontUi));
            UINT fmt = (radio ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
            DrawTextW(dis->hDC, text, -1, &textRc, fmt);
            SelectObject(dis->hDC, oldFont);
            return TRUE;
        }
        case WM_ERASEBKGND: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, g_dialogBgBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, g_theme.text);
            SetBkColor(hdc, g_theme.panel);
            return reinterpret_cast<LRESULT>(g_dialogBgBrush);
        }
        case WM_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        case WM_NCDESTROY:
            delete cs;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowShellConfigDialog(DashboardState* st) {
    if (!st || !st->hwndMain) return;
    static bool regDone = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!regDone) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = ShellConfigWndProc;
        wc.hInstance = hInst;
        wc.hIcon = LoadAppIcon();
        wc.hIconSm = LoadAppSmallIcon();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"HanchoShellConfigWnd";
        RegisterClassExW(&wc);
        regDone = true;
    }

    auto* cs = new ShellConfigDlgState();
    cs->dash = st;

    RECT rcParent = {};
    GetWindowRect(st->hwndMain, &rcParent);
    int dlgW = 660;
    int dlgH = 250;
    int x = rcParent.left + ((rcParent.right - rcParent.left) - dlgW) / 2;
    int y = rcParent.top + ((rcParent.bottom - rcParent.top) - dlgH) / 2;

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"HanchoShellConfigWnd",
        Tr(L"シェル設定 (cmd / powershell / bash)", L"Shell Config (cmd / powershell / bash)", L"Configuración de shell (cmd / powershell / bash)"),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, dlgW, dlgH,
        st->hwndMain, nullptr, hInst, cs
    );
    if (hDlg) {
        ShowWindow(hDlg, SW_SHOW);
        SetForegroundWindow(hDlg);
    } else {
        delete cs;
    }
}

static void InitTrayIcon(DashboardState* st) {
    if (!st || !st->hwndMain || st->trayAdded) return;
    ZeroMemory(&st->nid, sizeof(st->nid));
    st->nid.cbSize = sizeof(NOTIFYICONDATAW);
    st->nid.hWnd = st->hwndMain;
    st->nid.uID = 1;
    st->nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    st->nid.uCallbackMessage = WM_PC_TRAYICON;
    st->nid.hIcon = LoadAppSmallIcon();
    wcscpy_s(st->nid.szTip, L"Hancho — Multi-Terminal ConPTY Dashboard");
    st->trayAdded = gui_platform::GetGuiPlatform()->TrayAdd(st->nid);
}

static void RemoveTrayIcon(DashboardState* st) {
    if (!st || !st->trayAdded) return;
    gui_platform::GetGuiPlatform()->TrayDelete(st->nid);
    st->trayAdded = false;
}

static void HideToTray(DashboardState* st) {
    if (!st || !st->hwndMain) return;
    InitTrayIcon(st);
    // Without a tray icon, hiding the window would leave no way back to it: fall back to a normal minimize.
    if (!st->trayAdded) {
        ShowWindow(st->hwndMain, SW_MINIMIZE);
        return;
    }
    if (st->gridView && st->gridView->IsVisible()) {
        st->gridView->Hide();
    }
    ShowWindow(st->hwndMain, SW_HIDE);
}

static void RestoreFromTray(DashboardState* st) {
    if (!st || !st->hwndMain) return;
    ShowWindow(st->hwndMain, SW_SHOW);
    if (IsIconic(st->hwndMain)) {
        ShowWindow(st->hwndMain, SW_RESTORE);
    }
    SetForegroundWindow(st->hwndMain);
    RefreshDashboardData(st);
    if (st->selectedProcId > 0 && st->pm) {
        TerminalView* tv = st->pm->GetTerminalView(st->selectedProcId);
        if (tv && tv->Hwnd()) {
            SetFocus(tv->Hwnd());
        }
    }
}

static void ShowTrayContextMenu(DashboardState* st) {
    if (!st || !st->hwndMain) return;
    POINT pt = {};
    GetCursorPos(&pt);

    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING | MF_DEFAULT, IDM_TRAY_RESTORE, Tr(L"ダッシュボードを復元", L"Restore Dashboard", L"Restaurar panel"));
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_GRID_VIEW, Tr(L"グリッド表示を開く", L"Open Grid View", L"Abrir vista de cuadrícula"));
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_USAGE, Tr(L"トークン使用量グラフ", L"Token Usage Graph", L"Gráfico de uso de tokens"));
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_MEMORY, Tr(L"メモリ一覧...", L"Memory List...", L"Lista de memoria..."));
    if (IsWindowVisible(st->hwndMain)) {
        AppendMenuW(hMenu, MF_STRING, IDM_TRAY_HIDE, Tr(L"タスクトレイに格納", L"Hide to Tray", L"Ocultar en bandeja"));
    }
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, IDM_TRAY_EXIT, Tr(L"Hancho を終了", L"Exit Hancho", L"Salir de Hancho"));

    SetForegroundWindow(st->hwndMain);
    int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, st->hwndMain, nullptr);
    PostMessageW(st->hwndMain, WM_NULL, 0, 0);
    DestroyMenu(hMenu);

    if (cmd == IDM_TRAY_RESTORE) {
        RestoreFromTray(st);
    } else if (cmd == IDM_TRAY_GRID_VIEW) {
        RestoreFromTray(st);
        if (st->gridView) {
            LayoutTerminalViews(st);
            st->gridView->Show();
        }
    } else if (cmd == IDM_TRAY_USAGE) {
        RestoreFromTray(st);
        st->showUsagePanel = true;
        RECT rcClient = {};
        GetClientRect(st->hwndMain, &rcClient);
        LayoutControls(st, rcClient.right, rcClient.bottom);
        InvalidateRect(st->hwndMain, nullptr, FALSE);
    } else if (cmd == IDM_TRAY_MEMORY) {
        RestoreFromTray(st);
        ShowMemoryListDialog(st);
    } else if (cmd == IDM_TRAY_HIDE) {
        HideToTray(st);
    } else if (cmd == IDM_TRAY_EXIT) {
        // When a dashboard is attached to a --headless core (it runs in the same
        // process as the core host window), "Exit" only closes the dashboard.
        // The core and its jobs keep running and can be reattached later with
        // `Hancho.exe gui`. `Hancho.exe quit` stops the core itself.
        bool attachedHeadless = false;
        for (HWND w = FindWindowW(L"HanchoCoreHostWnd", nullptr); w;
             w = FindWindowExW(nullptr, w, L"HanchoCoreHostWnd", nullptr)) {
            DWORD ownerPid = 0;
            GetWindowThreadProcessId(w, &ownerPid);
            if (ownerPid == GetCurrentProcessId()) {
                attachedHeadless = true;
                break;
            }
        }
        if (attachedHeadless) {
            HideToTray(st);
            return;
        }
        st->allowExit = true;
        DestroyWindow(st->hwndMain);
    }
}

static void ResetProcessIdsWithConfirm(DashboardState* st) {
    if (!st || !st->hwndMain) return;
    std::wstring title = Tr(L"プロセスIDをリセット", L"Reset Process IDs", L"Restablecer IDs");
    std::wstring msg = Tr(
        L"プロセスIDをリセットしますか？ 終了済みプロセスは #1 から振り直されます。\n"
        L"実行中のプロセスはすべて停止している必要があります。",
        L"Reset process IDs? Finished processes will be renumbered from #1.\n"
        L"All running processes must be stopped first.",
        L"¿Restablecer los IDs? Los procesos finalizados se renumerarán desde #1.\n"
        L"Primero debe detener todos los procesos en ejecución.");
    if (gui_platform::GetGuiPlatform()->ShowMessageBox(
            st->hwndMain, title, msg, gui_platform::MessageBoxType::QuestionYesNo, true) != gui_platform::MessageBoxResult::Yes) {
        return;
    }
    std::string err;
    int count = ResetProcessIdsIpc(st, err);
    if (count < 0) {
        gui_platform::GetGuiPlatform()->ShowMessageBox(
            st->hwndMain, title, Utf8ToWide(err), gui_platform::MessageBoxType::Warning);
    } else {
        st->selectedProcId = -1;
        RefreshDashboardData(st);
    }
}

static UiTheme ThemeForButton(int id) {
    switch (id) {
        case IDC_SET_THEME_DARK:     return UiTheme::Dark;
        case IDC_SET_THEME_LIGHT:    return UiTheme::Light;
        case IDC_SET_THEME_PASTEL:   return UiTheme::Pastel;
        case IDC_SET_THEME_HC:       return UiTheme::HighContrast;
        case IDC_SET_THEME_MIDNIGHT: return UiTheme::Midnight;
        case IDC_SET_THEME_EMBER:    return UiTheme::Ember;
        case IDC_SET_THEME_BERRY:    return UiTheme::Berry;
        case IDC_SET_THEME_CRIMSON:  return UiTheme::Crimson;
        case IDC_SET_THEME_FOREST:   return UiTheme::Forest;
        case IDC_SET_THEME_MIST:     return UiTheme::Mist;
        case IDC_SET_THEME_PASTEL_BLUE: return UiTheme::PastelBlue;
        case IDC_SET_THEME_PASTEL_ORANGE: return UiTheme::PastelOrange;
        case IDC_SET_THEME_PASTEL_PINK: return UiTheme::PastelPink;
        case IDC_SET_THEME_PASTEL_RED: return UiTheme::PastelRed;
        case IDC_SET_THEME_PASTEL_GREEN: return UiTheme::PastelGreen;
        case IDC_SET_THEME_DEEP_PURPLE: return UiTheme::DeepPurple;
        case IDC_SET_THEME_PASTEL_PURPLE: return UiTheme::PastelPurple;
        case IDC_SET_THEME_DEEP_YELLOW: return UiTheme::DeepYellow;
        case IDC_SET_THEME_PASTEL_YELLOW: return UiTheme::PastelYellow;
        default:                     return UiTheme::System;
    }
}

static int ThemeButtonId(UiTheme theme) {
    switch (theme) {
        case UiTheme::Dark:          return IDC_SET_THEME_DARK;
        case UiTheme::Light:         return IDC_SET_THEME_LIGHT;
        case UiTheme::Pastel:        return IDC_SET_THEME_PASTEL;
        case UiTheme::HighContrast:  return IDC_SET_THEME_HC;
        case UiTheme::Midnight:      return IDC_SET_THEME_MIDNIGHT;
        case UiTheme::Ember:         return IDC_SET_THEME_EMBER;
        case UiTheme::Berry:         return IDC_SET_THEME_BERRY;
        case UiTheme::Crimson:       return IDC_SET_THEME_CRIMSON;
        case UiTheme::Forest:        return IDC_SET_THEME_FOREST;
        case UiTheme::Mist:          return IDC_SET_THEME_MIST;
        case UiTheme::PastelBlue: return IDC_SET_THEME_PASTEL_BLUE;
        case UiTheme::PastelOrange: return IDC_SET_THEME_PASTEL_ORANGE;
        case UiTheme::PastelPink: return IDC_SET_THEME_PASTEL_PINK;
        case UiTheme::PastelRed: return IDC_SET_THEME_PASTEL_RED;
        case UiTheme::PastelGreen: return IDC_SET_THEME_PASTEL_GREEN;
        case UiTheme::DeepPurple: return IDC_SET_THEME_DEEP_PURPLE;
        case UiTheme::PastelPurple: return IDC_SET_THEME_PASTEL_PURPLE;
        case UiTheme::DeepYellow: return IDC_SET_THEME_DEEP_YELLOW;
        case UiTheme::PastelYellow: return IDC_SET_THEME_PASTEL_YELLOW;
        default:                     return IDC_SET_THEME_SYSTEM;
    }
}

static constexpr int kSettingsTabCount = 5;

struct SettingsDlgState {
    DashboardState* dash = nullptr;
    HWND hwndTitle = nullptr;
    HWND hwndLangLabel = nullptr;
    HWND hwndThemeLabel = nullptr;
    HWND btnJa = nullptr;
    HWND btnEn = nullptr;
    HWND btnEs = nullptr;
    HWND btnSystem = nullptr;
    HWND btnDark = nullptr;
    HWND btnLight = nullptr;
    HWND btnPastel = nullptr;
    HWND btnHc = nullptr;
    HWND btnMidnight = nullptr;
    HWND btnEmber = nullptr;
    HWND btnBerry = nullptr;
    HWND btnCrimson = nullptr;
    HWND btnForest = nullptr;
    HWND btnMist = nullptr;
    HWND btnPastelBlue = nullptr;
    HWND btnPastelOrange = nullptr;
    HWND btnPastelPink = nullptr;
    HWND btnPastelRed = nullptr;
    HWND btnPastelGreen = nullptr;
    HWND btnDeepPurple = nullptr;
    HWND btnPastelPurple = nullptr;
    HWND btnDeepYellow = nullptr;
    HWND btnPastelYellow = nullptr;
    HWND btnShell = nullptr;
    HWND btnReset = nullptr;
    HWND btnClear = nullptr;
    HWND btnClose = nullptr;
    HWND hwndSoundLabel = nullptr;
    HWND chkSound = nullptr;
    HWND editSound = nullptr;
    HWND btnBrowseSound = nullptr;
    HWND btnTestSound = nullptr;
    HWND hwndSoundHint = nullptr;
    HWND hwndSatLabel = nullptr;
    HWND hwndSatCombo = nullptr;
     HWND chkHideEnded = nullptr;
     HWND chkUsageGraph = nullptr;
     HWND chkRestoreUsageGraph = nullptr;
      HWND chkResourceLocks = nullptr;
      HWND chkActivityStyle = nullptr;
      HWND lblEventRetention = nullptr;
      HWND editEventRetention = nullptr;
      HWND lblRefreshInterval = nullptr;
      HWND editRefreshInterval = nullptr;
      HWND lblListBg = nullptr;
      HWND btnListBg[kListRowKeyCount] = {};
      HWND btnListBgReset = nullptr;
      HWND chkEndBlink = nullptr;
      HWND lblEndBlinkSec = nullptr;
      HWND editEndBlinkSec = nullptr;
      HWND chkSupervisorHints = nullptr;
      HWND lblHintAutoDismissSec = nullptr;
      HWND editHintAutoDismissSec = nullptr;
      HWND lblSupervisorCommand = nullptr;
      HWND editSupervisorCommand = nullptr;
      HWND lblWebUiTitle = nullptr;
      HWND chkWebUi = nullptr;
      HWND lblWebUiScope = nullptr;
      HWND cmbWebUiScope = nullptr;
      HWND lblWebUiAllowedHosts = nullptr;
      HWND editWebUiAllowedHosts = nullptr;
      nlohmann::json webuiConfig;
      HWND lblProcFont = nullptr;
      HWND cmbProcFont = nullptr;
       HWND lblTaskFont = nullptr;
       HWND cmbTaskFont = nullptr;
       HWND hwndAboutTitle = nullptr;
       HWND hwndAboutVersion = nullptr;
       HWND hwndAboutDesc = nullptr;
       HWND tabCtrl = nullptr;
       // controls shown under each tab: 0 appearance, 1 behavior, 2 maintenance, 3 colors, 4 about
       std::vector<HWND> tabPages[kSettingsTabCount];
};

static LRESULT CALLBACK SettingsTabSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                                UINT_PTR, DWORD_PTR) {
    if (msg == WM_ERASEBKGND) {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        RECT rc = {};
        GetClientRect(hwnd, &rc);
        FillRect(hdc, &rc, g_dialogBgBrush);
        return 1;
    }
    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

static void UpdateSettingsDialogTexts(HWND hwnd);

static void ApplyListRowBgChange(SettingsDlgState* ds, HWND hwnd) {
    SaveShellConfig(ds->dash);
    ApplyListTheme(ds->dash);
    UpdateSettingsDialogTexts(hwnd);
    InvalidateRect(hwnd, nullptr, TRUE);
}

// Reads the event retention box, applies it to every stream tracker and saves it. Out-of-range
// values are clamped (see ClampLlmMaxEvents), and the box shows the value that was applied.
static void CommitEventRetention(SettingsDlgState* ds) {
    if (!ds || !ds->dash || !ds->editEventRetention) return;
    wchar_t text[64] = {};
    GetWindowTextW(ds->editEventRetention, text, 64);
    const int64_t applied = ClampLlmMaxEvents(_wtoi64(text));
    SetLlmMaxEvents(applied);
    ds->dash->llmMaxEvents = applied;
    std::wstring shown = std::to_wstring(applied);
    SetWindowTextW(ds->editEventRetention, shown.c_str());
    SaveShellConfig(ds->dash);
}

// Reads the refresh interval box (ms), clamps it, restarts the dashboard refresh timer,
// pushes the value to the terminal grid, and saves it.
static void CommitRefreshInterval(SettingsDlgState* ds) {
    if (!ds || !ds->dash || !ds->editRefreshInterval) return;
    wchar_t text[64] = {};
    GetWindowTextW(ds->editRefreshInterval, text, 64);
    const int applied = ClampRefreshIntervalMs(_wtoi(text));
    ds->dash->refreshIntervalMs = applied;
    SettingsManager::Instance().SetTabGridRefreshIntervalMs(applied);
    ApplyDashboardRefreshTimer(ds->dash);
    SetWindowTextW(ds->editRefreshInterval, std::to_wstring(applied).c_str());
    SaveShellConfig(ds->dash);
}

static void CommitSoundSettings(SettingsDlgState* ds) {
    if (!ds || !ds->dash) return;
    wchar_t path[MAX_PATH * 4] = {};
    GetWindowTextW(ds->editSound, path, MAX_PATH * 4);
    ds->dash->completionSoundPath = WideToUtf8(path);
    ds->dash->completionSoundEnabled =
        (SendMessageW(ds->chkSound, BM_GETCHECK, 0, 0) == BST_CHECKED);
    SaveShellConfig(ds->dash);
}

// Reads the end-blink controls, clamps the seconds, and saves them.
static void CommitEndBlinkSettings(SettingsDlgState* ds) {
    if (!ds || !ds->dash || !ds->editEndBlinkSec) return;
    ds->dash->endBlinkEnabled = (SendMessageW(ds->chkEndBlink, BM_GETCHECK, 0, 0) == BST_CHECKED);
    wchar_t text[64] = {};
    GetWindowTextW(ds->editEndBlinkSec, text, 64);
    const int applied = ClampEndBlinkSeconds(_wtoi(text));
    ds->dash->endBlinkSeconds = applied;
    SetWindowTextW(ds->editEndBlinkSec, std::to_wstring(applied).c_str());
    SaveShellConfig(ds->dash);
}

static void CommitSupervisorSettings(SettingsDlgState* ds) {
    if (!ds || !ds->dash) return;
    if (ds->chkSupervisorHints) {
        ds->dash->showSupervisorHints = (SendMessageW(ds->chkSupervisorHints, BM_GETCHECK, 0, 0) == BST_CHECKED);
    }
    if (ds->editHintAutoDismissSec) {
        wchar_t text[64] = {};
        GetWindowTextW(ds->editHintAutoDismissSec, text, 64);
        int val = _wtoi(text);
        if (val < 0) val = 0;
        if (val > 60) val = 60;
        ds->dash->hintAutoDismissSec = val;
        SetWindowTextW(ds->editHintAutoDismissSec, std::to_wstring(val).c_str());
    }
    if (ds->editSupervisorCommand) {
        wchar_t text[512] = {};
        GetWindowTextW(ds->editSupervisorCommand, text, 512);
        std::string cmd = WideToUtf8(text);
        if (!cmd.empty()) ds->dash->supervisorCommand = cmd;
    }
    SaveShellConfig(ds->dash);
}

// Shows the controls of one tab page and hides the rest.
static void SelectSettingsTab(SettingsDlgState* ds, int tab) {
    if (!ds) return;
    for (int p = 0; p < kSettingsTabCount; ++p) {
        for (HWND h : ds->tabPages[p]) ShowWindow(h, p == tab ? SW_SHOW : SW_HIDE);
    }
}

static std::string TrimWebUiItem(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static void LoadWebUiDialogConfig(SettingsDlgState* ds) {
    if (!ds) return;
    ds->webuiConfig = nlohmann::json::object();
    std::string content = ReadFileUtf8Wide(GetConfigFilePathWide());
    nlohmann::json cfg = nlohmann::json::parse(content, nullptr, false);
    if (cfg.is_object() && cfg.contains("webui") && cfg["webui"].is_object()) {
        ds->webuiConfig = cfg["webui"];
    }
    const bool enabled = ds->webuiConfig.value("enabled", true);
    const std::string scope = ds->webuiConfig.value("scope", "loopback");
    if (ds->chkWebUi) {
        SendMessageW(ds->chkWebUi, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    }
    if (ds->cmbWebUiScope) {
        const bool lan = (scope == "lan" || scope == "custom");
        SendMessageW(ds->cmbWebUiScope, CB_SETCURSEL, lan ? 1 : 0, 0);
    }
    std::string hosts;
    if (ds->webuiConfig.contains("allowed_hosts") && ds->webuiConfig["allowed_hosts"].is_array()) {
        for (const auto& item : ds->webuiConfig["allowed_hosts"]) {
            if (!item.is_string()) continue;
            if (!hosts.empty()) hosts += ", ";
            hosts += item.get<std::string>();
        }
    }
    if (ds->editWebUiAllowedHosts) {
        SetWindowTextW(ds->editWebUiAllowedHosts, Utf8ToWide(hosts).c_str());
    }
}

static void ApplyWebUiDialogConfig(SettingsDlgState* ds) {
    if (!ds || !ds->dash) return;
    nlohmann::json webui = ds->webuiConfig.is_object() ? ds->webuiConfig : nlohmann::json::object();
    const bool enabled = !ds->chkWebUi ||
        (SendMessageW(ds->chkWebUi, BM_GETCHECK, 0, 0) == BST_CHECKED);
    const int scopeSel = ds->cmbWebUiScope
        ? static_cast<int>(SendMessageW(ds->cmbWebUiScope, CB_GETCURSEL, 0, 0))
        : 0;
    const std::string scope = (scopeSel == 1) ? "lan" : "loopback";
    webui["enabled"] = enabled;
    webui["scope"] = scope;
    if (scope == "lan") {
        const std::string host = webui.value("host", "0.0.0.0");
        if (host.empty() || host == "127.0.0.1" || host == "localhost") {
            webui["host"] = "0.0.0.0";
        }
    } else {
        webui["host"] = "127.0.0.1";
    }

    nlohmann::json hosts = nlohmann::json::array();
    if (ds->editWebUiAllowedHosts) {
        wchar_t buf[1024] = {};
        GetWindowTextW(ds->editWebUiAllowedHosts, buf, 1024);
        std::string text = WideToUtf8(buf);
        size_t start = 0;
        while (start <= text.size()) {
            size_t comma = text.find(',', start);
            std::string item = TrimWebUiItem(text.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start));
            if (!item.empty()) hosts.push_back(item);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
    }
    webui["allowed_hosts"] = hosts;
    ds->webuiConfig = webui;

    nlohmann::json req = {{"action", "config_set"}, {"patch", {{"webui", webui}}}};
    nlohmann::json resp;
    GuiIpcCall(ds->dash, req, resp);
}

static void UpdateSettingsDialogTexts(HWND hwnd) {
    auto* ds = reinterpret_cast<SettingsDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!ds) return;
    SetWindowTextW(hwnd, Tr(L"設定", L"Settings", L"Ajustes"));
    SetWindowTextW(ds->hwndTitle, Tr(L"表示と言語の設定", L"Appearance and language", L"Apariencia e idioma"));
    SetWindowTextW(ds->hwndLangLabel, Tr(L"言語", L"Language", L"Idioma"));
    SetWindowTextW(ds->hwndThemeLabel, Tr(L"テーマ (色)", L"Theme (colors)", L"Tema (colores)"));
    SetWindowTextW(ds->btnSystem, L"Windows");
    SetWindowTextW(ds->btnDark, Tr(L"ダーク", L"Dark", L"Oscuro"));
    SetWindowTextW(ds->btnLight, Tr(L"ライト", L"Light", L"Claro"));
    SetWindowTextW(ds->btnPastel, Tr(L"パステル", L"Pastel", L"Pastel"));
    SetWindowTextW(ds->btnHc, Tr(L"ハイコントラスト", L"High Contrast", L"Alto contraste"));
    SetWindowTextW(ds->btnMidnight, Tr(L"深夜", L"Midnight", L"Medianoche"));
    SetWindowTextW(ds->btnEmber, Tr(L"残り火", L"Ember", L"Brasa"));
    SetWindowTextW(ds->btnBerry, Tr(L"ベリー", L"Berry", L"Baya"));
    SetWindowTextW(ds->btnCrimson, Tr(L"クリムゾン", L"Crimson", L"Carmesí"));
    SetWindowTextW(ds->btnForest, Tr(L"深緑", L"Forest", L"Bosque"));
    SetWindowTextW(ds->btnMist, Tr(L"ミスト", L"Mist", L"Niebla"));
    SetWindowTextW(ds->btnPastelBlue, Tr(L"パステル青", L"Pastel Blue", L"Pastel azul"));
    SetWindowTextW(ds->btnPastelOrange, Tr(L"パステル橙", L"Pastel Orange", L"Pastel naranja"));
    SetWindowTextW(ds->btnPastelPink, Tr(L"パステル桃", L"Pastel Pink", L"Pastel rosa"));
    SetWindowTextW(ds->btnPastelRed, Tr(L"パステル赤", L"Pastel Red", L"Pastel rojo"));
    SetWindowTextW(ds->btnPastelGreen, Tr(L"パステル緑", L"Pastel Green", L"Pastel verde"));
    SetWindowTextW(ds->btnDeepPurple, Tr(L"濃い紫", L"Deep Purple", L"Púrpura oscuro"));
    SetWindowTextW(ds->btnPastelPurple, Tr(L"薄い紫", L"Pale Purple", L"Púrpura claro"));
    SetWindowTextW(ds->btnDeepYellow, Tr(L"濃い黄", L"Deep Yellow", L"Amarillo oscuro"));
    SetWindowTextW(ds->btnPastelYellow, Tr(L"薄い黄", L"Pale Yellow", L"Amarillo claro"));
    SetWindowTextW(ds->btnShell, Tr(L"シェル構成...", L"Shell Config...", L"Configurar shell..."));
    SetWindowTextW(ds->btnReset, Tr(L"プロセスIDをリセット", L"Reset Process IDs", L"Restablecer IDs"));
    SetWindowTextW(ds->btnClear, Tr(L"終了履歴を消去", L"Clear Ended", L"Limpiar finalizados"));
    SetWindowTextW(ds->chkHideEnded, Tr(L"終了履歴を非表示", L"Hide ended processes", L"Ocultar finalizados"));
    SetWindowTextW(ds->chkUsageGraph, Tr(L"Usageグラフを provider/model 積み上げ表示", L"Usage graph: stack by provider/model", L"Gráfico de uso: apilar por proveedor/modelo"));
    SetWindowTextW(ds->chkRestoreUsageGraph, Tr(L"終了後もUsageグラフを復元する", L"Restore usage graph after exit", L"Restaurar gráfico de uso tras salir"));
    SetWindowTextW(ds->chkResourceLocks, Tr(L"タスクキュー: リソースが競合するジョブを同時に実行しない", L"Task queue: never run jobs with conflicting resources together", L"Cola de tareas: no ejecutar a la vez trabajos con recursos en conflicto"));
    SetWindowTextW(ds->chkActivityStyle, Tr(L"実行状態監視ペイン: 行を色分けし、アイコンを表示", L"Execution monitor pane: colour rows and show icons", L"Panel de ejecución: colorear filas y mostrar iconos"));
    SetWindowTextW(ds->btnClose, Tr(L"閉じる", L"Close", L"Cerrar"));
    {
        const wchar_t* const tabNames[kSettingsTabCount] = {
            Tr(L"表示", L"Appearance", L"Apariencia"),
            Tr(L"動作・通知", L"Behavior & alerts", L"Comportamiento y avisos"),
            Tr(L"管理", L"Maintenance", L"Mantenimiento"),
            Tr(L"色", L"Colors", L"Colores"),
            Tr(L"バージョン情報", L"About", L"Acerca de"),
        };
        for (int t = 0; t < kSettingsTabCount; ++t) {
            std::wstring name = tabNames[t];
            TCITEMW ti = {};
            ti.mask = TCIF_TEXT;
            ti.pszText = name.data();
            TabCtrl_SetItem(ds->tabCtrl, t, &ti);
        }
    }
    if (ds->hwndAboutTitle) SetWindowTextW(ds->hwndAboutTitle, L"Hancho");
    if (ds->hwndAboutVersion) {
        std::wstring version = Tr(L"バージョン ", L"Version ", L"Versión ") + Utf8ToWide(PC_VERSION);
        SetWindowTextW(ds->hwndAboutVersion, version.c_str());
    }
    if (ds->hwndAboutDesc) {
        SetWindowTextW(ds->hwndAboutDesc, Tr(
            L"Native Win32 マルチ LLM プロセス・ConPTY マネージャー",
            L"Native Win32 multi-LLM process & ConPTY manager",
            L"Gestor nativo Win32 de procesos multi-LLM y ConPTY"));
    }
    SetWindowTextW(ds->chkEndBlink, Tr(L"プロセス・タスク終了時に一覧の行を点滅させる", L"Blink list rows when a process or task ends", L"Hacer parpadear las filas al terminar un proceso o tarea"));
    SetWindowTextW(ds->lblEndBlinkSec, Tr(L"点滅秒数 (1〜60, 既定 5):", L"Blink duration (seconds, 1-60, default 5):", L"Duración del parpadeo (segundos, 1-60, predeterminado 5):"));
    if (ds->chkSupervisorHints) {
        SetWindowTextW(ds->chkSupervisorHints, Tr(
            L"Supervisor LLM ガイダンス・ヒントを表示 (状態に応じたアドバイス・見積もり乖離検知)",
            L"Show Supervisor LLM guidance hints (state-dependent tips & timing divergence alerts)",
            L"Mostrar consejos del Supervisor LLM (avisos de estado y desviación de tiempo)"));
    }
    if (ds->lblHintAutoDismissSec) {
        SetWindowTextW(ds->lblHintAutoDismissSec, Tr(
            L"ヒント自動消去秒数 (0〜60, 既定 6, 0 = 手動消去のみ):",
            L"Hint auto-dismiss seconds (0-60, default 6, 0 = manual close only):",
            L"Segundos para auto-ocultar consejos (0-60, predeterminado 6, 0 = solo manual):"));
    }
    if (ds->lblSupervisorCommand) {
        SetWindowTextW(ds->lblSupervisorCommand, Tr(
            L"Supervisor 起動コマンド (既定 claude):",
            L"Supervisor launch command (default claude):",
            L"Comando de lanzamiento de Supervisor (predeterminado claude):"));
    }
    if (ds->lblWebUiTitle) {
        SetWindowTextW(ds->lblWebUiTitle, Tr(L"ブラウザ UI (WebUI)", L"Browser UI (WebUI)", L"Interfaz web (WebUI)"));
    }
    if (ds->chkWebUi) {
        SetWindowTextW(ds->chkWebUi, Tr(L"ブラウザ UI を有効にする", L"Enable the browser UI", L"Activar la interfaz web"));
    }
    if (ds->lblWebUiScope) {
        SetWindowTextW(ds->lblWebUiScope, Tr(L"公開範囲:", L"Exposure:", L"Exposición:"));
    }
    if (ds->cmbWebUiScope) {
        int sel = static_cast<int>(SendMessageW(ds->cmbWebUiScope, CB_GETCURSEL, 0, 0));
        if (sel < 0) sel = 0;
        SendMessageW(ds->cmbWebUiScope, CB_RESETCONTENT, 0, 0);
        SendMessageW(ds->cmbWebUiScope, CB_ADDSTRING, 0,
            (LPARAM)Tr(L"ローカルのみ (127.0.0.1)", L"Loopback only (127.0.0.1)", L"Solo local (127.0.0.1)"));
        SendMessageW(ds->cmbWebUiScope, CB_ADDSTRING, 0,
            (LPARAM)Tr(L"LAN (0.0.0.0)", L"LAN (0.0.0.0)", L"LAN (0.0.0.0)"));
        SendMessageW(ds->cmbWebUiScope, CB_SETCURSEL, sel, 0);
    }
    if (ds->lblWebUiAllowedHosts) {
        SetWindowTextW(ds->lblWebUiAllowedHosts, Tr(
            L"追加で許可するホスト名 (カンマ区切り、LAN 時):",
            L"Extra allowed hostnames (comma separated, LAN only):",
            L"Nombres de host adicionales (separados por comas, solo LAN):"));
    }
    SetWindowTextW(ds->lblEventRetention, Tr(
        L"イベント保持件数 (100〜100000, -1 = 無制限: メモリを使い切る恐れあり):",
        L"Events kept per process (100-100000, -1 = unlimited: may use up memory):",
        L"Eventos guardados por proceso (100-100000, -1 = sin límite: puede agotar la memoria):"));
    SetWindowTextW(ds->lblRefreshInterval, Tr(
        L"更新間隔 (ms, 100〜60000, 既定 2000):",
        L"Update interval (ms, 100-60000, default 2000):",
        L"Intervalo de actualización (ms, 100-60000, predeterminado 2000):"));
    {
        const bool darkMode = ListRowMode() == 0;
        std::wstring listBgLabel = Tr(
            L"リスト行の背景色 (状態別): ",
            L"List row background (by state): ",
            L"Fondo de fila (por estado): ");
        listBgLabel += darkMode
            ? Tr(L"ダークテーマ用を編集中", L"editing dark theme colors", L"editando colores del tema oscuro")
            : Tr(L"ライトテーマ用を編集中", L"editing light theme colors", L"editando colores del tema claro");
        SetWindowTextW(ds->lblListBg, listBgLabel.c_str());
    }
    static const wchar_t* const kListBgNames[kListRowKeyCount][3] = {
        { L"実行中", L"Running", L"En ejecución" },
        { L"アイドル", L"Idle", L"Inactivo" },
        { L"完了", L"Completed", L"Completado" },
        { L"タイムアウト", L"Timeout", L"Tiempo agotado" },
        { L"失敗", L"Failed", L"Fallido" },
    };
    for (int k = 0; k < kListRowKeyCount; ++k) {
        std::wstring label = Tr(kListBgNames[k][0], kListBgNames[k][1], kListBgNames[k][2]);
        label += L": ";
        const int m = ListRowMode();
        label += g_listRowBgSet[m][k] ? Utf8ToWide(FormatHexColor(g_listRowBg[m][k]))
                                   : std::wstring(Tr(L"既定", L"default", L"predet."));
        SetWindowTextW(ds->btnListBg[k], label.c_str());
    }
    SetWindowTextW(ds->btnListBgReset, Tr(L"背景色を既定に戻す", L"Reset row colors", L"Restablecer colores"));
    SetWindowTextW(ds->hwndSoundLabel, Tr(L"終了時の通知音", L"Completion sound", L"Sonido al finalizar"));
    SetWindowTextW(ds->chkSound, Tr(L"プロセス終了時に音を鳴らす", L"Play a sound when a process finishes", L"Reproducir un sonido al finalizar un proceso"));
    SetWindowTextW(ds->btnBrowseSound, Tr(L"参照...", L"Browse...", L"Examinar..."));
    SetWindowTextW(ds->btnTestSound, Tr(L"試聴", L"Test", L"Probar"));
    SetWindowTextW(ds->hwndSoundHint, Tr(L"(空欄 = Windows の既定の通知音)", L"(Empty = Windows default notification sound)", L"(Vacío = sonido de notificación predeterminado de Windows)"));
    if (ds->hwndSatLabel) {
        SetWindowTextW(ds->hwndSatLabel, Tr(L"KPIバーの彩度", L"KPI bar saturation", L"Saturación de barras KPI"));
    }
    if (ds->hwndSatCombo) {
        int sel = 3;
        float sat = g_kpiSaturation;
        if (sat <= 0.35f) sel = 0;
        else if (sat <= 0.55f) sel = 1;
        else if (sat <= 0.75f) sel = 2;
        else sel = 3;
        SendMessageW(ds->hwndSatCombo, CB_SETCURSEL, sel, 0);
    }
    if (ds->lblProcFont) {
        SetWindowTextW(ds->lblProcFont, Tr(L"プロセス一覧の文字サイズ (px):", L"Process list font size (px):", L"Tamaño de letra de la lista de procesos (px):"));
    }
    if (ds->lblTaskFont) {
        SetWindowTextW(ds->lblTaskFont, Tr(L"タスクキューの文字サイズ (px):", L"Task queue font size (px):", L"Tamaño de letra de la cola de tareas (px):"));
    }
    if (ds->cmbProcFont) {
        SendMessageW(ds->cmbProcFont, CB_SETCURSEL, ListFontSizeIndex(ds->dash->processListFontPx), 0);
    }
    if (ds->cmbTaskFont) {
        SendMessageW(ds->cmbTaskFont, CB_SETCURSEL, ListFontSizeIndex(ds->dash->taskListFontPx), 0);
    }
    CheckRadioButton(hwnd, IDC_SET_LANG_JA, IDC_SET_LANG_ES,
        g_uiLanguage == UiLanguage::Japanese ? IDC_SET_LANG_JA :
        g_uiLanguage == UiLanguage::Spanish ? IDC_SET_LANG_ES : IDC_SET_LANG_EN);
    CheckRadioButton(hwnd, IDC_SET_THEME_SYSTEM, IDC_SET_THEME_PASTEL_YELLOW, ThemeButtonId(g_uiTheme));
}

static LRESULT CALLBACK SettingsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* ds = reinterpret_cast<SettingsDlgState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            ds = reinterpret_cast<SettingsDlgState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ds));
            HINSTANCE hInst = GetModuleHandleW(nullptr);

            // The title is no longer shown: the tab names say what each page is.
            ds->hwndTitle = CreateWindowExW(0, L"STATIC", L"", WS_CHILD,
                18, 14, 440, 22, hwnd, nullptr, hInst, nullptr);
            SendMessageW(ds->hwndTitle, WM_SETFONT, (WPARAM)ds->dash->hFontUiBold, TRUE);

            ds->tabCtrl = CreateWindowExW(0, WC_TABCONTROLW, L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_CLIPSIBLINGS | TCS_TABS | TCS_SINGLELINE | TCS_OWNERDRAWFIXED,
                8, 8, 684, 760, hwnd, (HMENU)IDC_SET_TAB, hInst, nullptr);
            SendMessageW(ds->tabCtrl, WM_SETFONT, (WPARAM)ds->dash->hFontUi, TRUE);
            SetWindowSubclass(ds->tabCtrl, SettingsTabSubclassProc, 1, 0);
            for (int t = 0; t < kSettingsTabCount; ++t) {
                TCITEMW ti = {};
                ti.mask = TCIF_TEXT;
                ti.pszText = const_cast<LPWSTR>(L"");
                TabCtrl_InsertItem(ds->tabCtrl, t, &ti);
            }

            ds->hwndLangLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                18, 48, 200, 20, hwnd, nullptr, hInst, nullptr);
            SendMessageW(ds->hwndLangLabel, WM_SETFONT, (WPARAM)ds->dash->hFontUiBold, TRUE);

            ds->btnJa = CreateWindowExW(0, L"BUTTON", L"日本語", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                18, 72, 120, 30, hwnd, (HMENU)IDC_SET_LANG_JA, hInst, nullptr);
            ds->btnEn = CreateWindowExW(0, L"BUTTON", L"English", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                146, 72, 120, 30, hwnd, (HMENU)IDC_SET_LANG_EN, hInst, nullptr);
            ds->btnEs = CreateWindowExW(0, L"BUTTON", L"Español", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                274, 72, 120, 30, hwnd, (HMENU)IDC_SET_LANG_ES, hInst, nullptr);

            ds->hwndThemeLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                18, 118, 200, 20, hwnd, nullptr, hInst, nullptr);
            SendMessageW(ds->hwndThemeLabel, WM_SETFONT, (WPARAM)ds->dash->hFontUiBold, TRUE);

            ds->btnSystem = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                18, 142, 130, 30, hwnd, (HMENU)IDC_SET_THEME_SYSTEM, hInst, nullptr);
            ds->btnDark = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                156, 142, 90, 30, hwnd, (HMENU)IDC_SET_THEME_DARK, hInst, nullptr);
            ds->btnLight = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                254, 142, 90, 30, hwnd, (HMENU)IDC_SET_THEME_LIGHT, hInst, nullptr);
            ds->btnPastel = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                352, 142, 100, 30, hwnd, (HMENU)IDC_SET_THEME_PASTEL, hInst, nullptr);
            ds->btnHc = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                460, 142, 168, 30, hwnd, (HMENU)IDC_SET_THEME_HC, hInst, nullptr);
            ds->btnMidnight = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                18, 178, 100, 30, hwnd, (HMENU)IDC_SET_THEME_MIDNIGHT, hInst, nullptr);
            ds->btnEmber = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                126, 178, 100, 30, hwnd, (HMENU)IDC_SET_THEME_EMBER, hInst, nullptr);
            ds->btnBerry = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                234, 178, 100, 30, hwnd, (HMENU)IDC_SET_THEME_BERRY, hInst, nullptr);
            ds->btnCrimson = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                342, 178, 100, 30, hwnd, (HMENU)IDC_SET_THEME_CRIMSON, hInst, nullptr);
            ds->btnForest = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                450, 178, 100, 30, hwnd, (HMENU)IDC_SET_THEME_FOREST, hInst, nullptr);
            ds->btnMist = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                558, 178, 100, 30, hwnd, (HMENU)IDC_SET_THEME_MIST, hInst, nullptr);

            ds->btnPastelBlue = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                18, 214, 120, 30, hwnd, (HMENU)IDC_SET_THEME_PASTEL_BLUE, hInst, nullptr);
            ds->btnPastelOrange = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                146, 214, 120, 30, hwnd, (HMENU)IDC_SET_THEME_PASTEL_ORANGE, hInst, nullptr);
            ds->btnPastelPink = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                274, 214, 120, 30, hwnd, (HMENU)IDC_SET_THEME_PASTEL_PINK, hInst, nullptr);
            ds->btnPastelRed = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                402, 214, 120, 30, hwnd, (HMENU)IDC_SET_THEME_PASTEL_RED, hInst, nullptr);
            ds->btnPastelGreen = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                530, 214, 120, 30, hwnd, (HMENU)IDC_SET_THEME_PASTEL_GREEN, hInst, nullptr);

            ds->btnDeepPurple = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                18, 250, 120, 30, hwnd, (HMENU)IDC_SET_THEME_DEEP_PURPLE, hInst, nullptr);
            ds->btnPastelPurple = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                146, 250, 120, 30, hwnd, (HMENU)IDC_SET_THEME_PASTEL_PURPLE, hInst, nullptr);
            ds->btnDeepYellow = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                274, 250, 120, 30, hwnd, (HMENU)IDC_SET_THEME_DEEP_YELLOW, hInst, nullptr);
            ds->btnPastelYellow = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                402, 250, 120, 30, hwnd, (HMENU)IDC_SET_THEME_PASTEL_YELLOW, hInst, nullptr);

            ds->hwndSatLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                18, 288, 220, 20, hwnd, nullptr, hInst, nullptr);
            SendMessageW(ds->hwndSatLabel, WM_SETFONT, (WPARAM)ds->dash->hFontUiBold, TRUE);

            ds->hwndSatCombo = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                18, 310, 220, 200, hwnd, (HMENU)IDC_SET_KPI_SAT, hInst, nullptr);
            SendMessageW(ds->hwndSatCombo, CB_ADDSTRING, 0, (LPARAM)L"30%");
            SendMessageW(ds->hwndSatCombo, CB_ADDSTRING, 0, (LPARAM)L"50%");
            SendMessageW(ds->hwndSatCombo, CB_ADDSTRING, 0, (LPARAM)L"70%");
            SendMessageW(ds->hwndSatCombo, CB_ADDSTRING, 0, (LPARAM)L"100%");

            ds->btnShell = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                18, 48, 180, 32, hwnd, (HMENU)IDC_SET_SHELL_CONFIG, hInst, nullptr);
            ds->btnReset = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                210, 48, 180, 32, hwnd, (HMENU)IDC_SET_RESET_IDS, hInst, nullptr);
            ds->btnClear = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                402, 48, 180, 32, hwnd, (HMENU)IDC_SET_CLEAR_ENDED, hInst, nullptr);
            ds->btnClose = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                560, 784, 110, 32, hwnd, (HMENU)IDCANCEL, hInst, nullptr);

            ds->hwndSoundLabel = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                18, 48, 300, 20, hwnd, nullptr, hInst, nullptr);
            SendMessageW(ds->hwndSoundLabel, WM_SETFONT, (WPARAM)ds->dash->hFontUiBold, TRUE);

            ds->chkSound = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                18, 72, 340, 24, hwnd, (HMENU)IDC_SET_SOUND_ENABLED, hInst, nullptr);
            ds->editSound = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
                18, 102, 470, 26, hwnd, (HMENU)IDC_SET_SOUND_PATH, hInst, nullptr);
            ds->btnBrowseSound = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                496, 102, 90, 26, hwnd, (HMENU)IDC_SET_SOUND_BROWSE, hInst, nullptr);
            ds->btnTestSound = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                594, 102, 80, 26, hwnd, (HMENU)IDC_SET_SOUND_TEST, hInst, nullptr);
            ds->hwndSoundHint = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                18, 132, 470, 18, hwnd, nullptr, hInst, nullptr);
            SendMessageW(ds->hwndSoundHint, WM_SETFONT, (WPARAM)ds->dash->hFontUi, TRUE);

            SendMessageW(ds->chkSound, BM_SETCHECK,
                ds->dash->completionSoundEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
            SetWindowTextW(ds->editSound, Utf8ToWide(ds->dash->completionSoundPath).c_str());

            ds->chkHideEnded = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                18, 170, 380, 24, hwnd, (HMENU)IDC_SET_HIDE_ENDED, hInst, nullptr);
            SendMessageW(ds->chkHideEnded, BM_SETCHECK,
                ds->dash->hideEndedProcesses ? BST_CHECKED : BST_UNCHECKED, 0);

            ds->chkUsageGraph = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                18, 194, 380, 24, hwnd, (HMENU)IDC_SET_USAGE_GRAPH_MODE, hInst, nullptr);
            SendMessageW(ds->chkUsageGraph, BM_SETCHECK,
                ds->dash->usageGraphMode == 1 ? BST_CHECKED : BST_UNCHECKED, 0);

            ds->chkRestoreUsageGraph = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                18, 218, 380, 24, hwnd, (HMENU)IDC_SET_RESTORE_USAGE_GRAPH, hInst, nullptr);
            SendMessageW(ds->chkRestoreUsageGraph, BM_SETCHECK,
                ds->dash->restoreUsageGraph ? BST_CHECKED : BST_UNCHECKED, 0);

            ds->chkResourceLocks = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                18, 242, 520, 24, hwnd, (HMENU)IDC_SET_RESOURCE_LOCKS, hInst, nullptr);
            SendMessageW(ds->chkResourceLocks, BM_SETCHECK,
                ds->dash->resourceLocks ? BST_CHECKED : BST_UNCHECKED, 0);

            ds->chkActivityStyle = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                18, 266, 520, 24, hwnd, (HMENU)IDC_SET_ACTIVITY_STYLE, hInst, nullptr);
            SendMessageW(ds->chkActivityStyle, BM_SETCHECK,
                g_activityStyle ? BST_CHECKED : BST_UNCHECKED, 0);

            ds->lblEventRetention = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                18, 296, 530, 20, hwnd, nullptr, hInst, nullptr);
            ds->editEventRetention = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                560, 293, 110, 24, hwnd, (HMENU)IDC_SET_LLM_MAX_EVENTS, hInst, nullptr);
            SetWindowTextW(ds->editEventRetention, std::to_wstring(ds->dash->llmMaxEvents).c_str());

            ds->lblRefreshInterval = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                18, 332, 530, 20, hwnd, nullptr, hInst, nullptr);
            ds->editRefreshInterval = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
                560, 329, 110, 24, hwnd, (HMENU)IDC_SET_REFRESH_INTERVAL, hInst, nullptr);
            SetWindowTextW(ds->editRefreshInterval, std::to_wstring(ds->dash->refreshIntervalMs).c_str());

            // Row colors live on their own 色 tab, so these start at the top of the page.
            ds->lblListBg = CreateWindowExW(0, L"STATIC", L"", WS_CHILD,
                18, 48, 640, 20, hwnd, nullptr, hInst, nullptr);
            SendMessageW(ds->lblListBg, WM_SETFONT, (WPARAM)ds->dash->hFontUiBold, TRUE);
            SendMessageW(ds->lblWebUiTitle, WM_SETFONT, (WPARAM)ds->dash->hFontUiBold, TRUE);
            for (int k = 0; k < kListRowKeyCount; ++k) {
                ds->btnListBg[k] = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                    18 + k * 126, 72, 120, 30, hwnd,
                    (HMENU)(INT_PTR)(IDC_SET_LIST_BG_RUNNING + k), hInst, nullptr);
            }
            ds->btnListBgReset = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW,
                18, 112, 220, 30, hwnd, (HMENU)IDC_SET_LIST_BG_RESET, hInst, nullptr);

            ds->chkEndBlink = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                18, 368, 520, 24, hwnd, (HMENU)IDC_SET_END_BLINK_ENABLED, hInst, nullptr);
            SendMessageW(ds->chkEndBlink, BM_SETCHECK,
                ds->dash->endBlinkEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
            ds->lblEndBlinkSec = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                18, 402, 530, 20, hwnd, nullptr, hInst, nullptr);
            ds->editEndBlinkSec = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
                560, 399, 110, 24, hwnd, (HMENU)IDC_SET_END_BLINK_SECONDS, hInst, nullptr);
            SetWindowTextW(ds->editEndBlinkSec, std::to_wstring(ds->dash->endBlinkSeconds).c_str());

            ds->chkSupervisorHints = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                18, 436, 640, 24, hwnd, (HMENU)IDC_SET_SHOW_SUPERVISOR_HINTS, hInst, nullptr);
            SendMessageW(ds->chkSupervisorHints, BM_SETCHECK,
                ds->dash->showSupervisorHints ? BST_CHECKED : BST_UNCHECKED, 0);
            ds->lblHintAutoDismissSec = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                18, 468, 530, 20, hwnd, nullptr, hInst, nullptr);
            ds->editHintAutoDismissSec = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | ES_NUMBER,
                560, 465, 110, 24, hwnd, (HMENU)IDC_SET_HINT_AUTO_DISMISS_SEC, hInst, nullptr);
            SetWindowTextW(ds->editHintAutoDismissSec, std::to_wstring(ds->dash->hintAutoDismissSec).c_str());
            ds->lblSupervisorCommand = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                18, 502, 330, 20, hwnd, nullptr, hInst, nullptr);
            ds->editSupervisorCommand = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                360, 499, 310, 24, hwnd, (HMENU)IDC_SET_SUPERVISOR_COMMAND, hInst, nullptr);
            SetWindowTextW(ds->editSupervisorCommand, Utf8ToWide(ds->dash->supervisorCommand).c_str());

            ds->lblWebUiTitle = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                18, 100, 640, 20, hwnd, nullptr, hInst, nullptr);
            ds->chkWebUi = CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                18, 126, 520, 24, hwnd, (HMENU)IDC_SET_WEBUI_ENABLED, hInst, nullptr);
            ds->lblWebUiScope = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                18, 160, 260, 20, hwnd, nullptr, hInst, nullptr);
            ds->cmbWebUiScope = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                18, 184, 260, 200, hwnd, (HMENU)IDC_SET_WEBUI_SCOPE, hInst, nullptr);
            SendMessageW(ds->cmbWebUiScope, CB_ADDSTRING, 0, (LPARAM)L"Loopback only (127.0.0.1)");
            SendMessageW(ds->cmbWebUiScope, CB_ADDSTRING, 0, (LPARAM)L"LAN (0.0.0.0)");
            ds->lblWebUiAllowedHosts = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                18, 218, 640, 20, hwnd, nullptr, hInst, nullptr);
            ds->editWebUiAllowedHosts = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                18, 242, 520, 26, hwnd, (HMENU)IDC_SET_WEBUI_ALLOWED_HOSTS, hInst, nullptr);

            ds->lblProcFont = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                18, 458, 530, 20, hwnd, nullptr, hInst, nullptr);
            ds->cmbProcFont = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                18, 480, 220, 300, hwnd, (HMENU)IDC_SET_PROC_FONT, hInst, nullptr);
            ds->lblTaskFont = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                18, 514, 530, 20, hwnd, nullptr, hInst, nullptr);
            ds->cmbTaskFont = CreateWindowExW(WS_EX_CLIENTEDGE, L"COMBOBOX", L"",
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                18, 536, 220, 300, hwnd, (HMENU)IDC_SET_TASK_FONT, hInst, nullptr);
            for (int i = 0; i < kListFontSizeCount; ++i) {
                std::wstring item = std::to_wstring(kListFontSizes[i]) + L" px";
                SendMessageW(ds->cmbProcFont, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
                SendMessageW(ds->cmbTaskFont, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(item.c_str()));
            }

            // About page: product name, version and a one-line description.
            ds->hwndAboutTitle = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT,
                18, 48, 500, 34, hwnd, nullptr, hInst, nullptr);
            ds->hwndAboutVersion = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT,
                18, 92, 500, 24, hwnd, nullptr, hInst, nullptr);
            ds->hwndAboutDesc = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | SS_LEFT,
                18, 124, 620, 24, hwnd, nullptr, hInst, nullptr);

            HWND ctrls[] = { ds->btnJa, ds->btnEn, ds->btnEs, ds->btnSystem, ds->btnDark, ds->btnLight, ds->btnPastel, ds->btnHc,
                             ds->btnMidnight, ds->btnEmber, ds->btnBerry, ds->btnCrimson, ds->btnForest, ds->btnMist,
                             ds->btnPastelBlue, ds->btnPastelOrange, ds->btnPastelPink, ds->btnPastelRed, ds->btnPastelGreen,
                             ds->btnDeepPurple, ds->btnPastelPurple, ds->btnDeepYellow, ds->btnPastelYellow,
                             ds->btnShell, ds->btnReset, ds->btnClear, ds->btnClose, ds->chkSound, ds->editSound,
                             ds->btnBrowseSound, ds->btnTestSound, ds->chkHideEnded, ds->chkUsageGraph, ds->chkRestoreUsageGraph,
                             ds->chkResourceLocks, ds->chkActivityStyle, ds->lblEventRetention, ds->editEventRetention,
                             ds->lblRefreshInterval, ds->editRefreshInterval, ds->btnListBgReset,
                             ds->chkEndBlink, ds->lblEndBlinkSec, ds->editEndBlinkSec,
                             ds->chkSupervisorHints, ds->lblHintAutoDismissSec, ds->editHintAutoDismissSec,
                             ds->lblSupervisorCommand, ds->editSupervisorCommand,
                             ds->lblWebUiTitle, ds->chkWebUi, ds->lblWebUiScope, ds->cmbWebUiScope,
                             ds->lblWebUiAllowedHosts, ds->editWebUiAllowedHosts,
                             ds->lblProcFont, ds->cmbProcFont, ds->lblTaskFont, ds->cmbTaskFont };
            for (HWND h : ctrls) SendMessageW(h, WM_SETFONT, (WPARAM)ds->dash->hFontUi, TRUE);
            for (HWND h : ds->btnListBg) SendMessageW(h, WM_SETFONT, (WPARAM)ds->dash->hFontUi, TRUE);
            SendMessageW(ds->lblListBg, WM_SETFONT, (WPARAM)ds->dash->hFontUiBold, TRUE);
            SendMessageW(ds->hwndAboutTitle, WM_SETFONT, (WPARAM)ds->dash->hFontUiBold, TRUE);
            SendMessageW(ds->hwndAboutVersion, WM_SETFONT, (WPARAM)ds->dash->hFontUi, TRUE);
            SendMessageW(ds->hwndAboutDesc, WM_SETFONT, (WPARAM)ds->dash->hFontUi, TRUE);

            // Sort the controls into their tab pages.
            {
                auto& appearance = ds->tabPages[0];
                auto& behavior = ds->tabPages[1];
                auto& maintenance = ds->tabPages[2];
                appearance.insert(appearance.end(), {
                    ds->hwndLangLabel, ds->btnJa, ds->btnEn, ds->btnEs,
                    ds->hwndThemeLabel, ds->btnSystem, ds->btnDark, ds->btnLight, ds->btnPastel, ds->btnHc,
                    ds->btnMidnight, ds->btnEmber, ds->btnBerry, ds->btnCrimson, ds->btnForest, ds->btnMist,
                    ds->btnPastelBlue, ds->btnPastelOrange, ds->btnPastelPink, ds->btnPastelRed, ds->btnPastelGreen,
                    ds->btnDeepPurple, ds->btnPastelPurple, ds->btnDeepYellow, ds->btnPastelYellow,
                    ds->hwndSatLabel, ds->hwndSatCombo,
                    ds->lblProcFont, ds->cmbProcFont, ds->lblTaskFont, ds->cmbTaskFont });
                auto& colors = ds->tabPages[3];
                colors.insert(colors.end(), { ds->lblListBg, ds->btnListBgReset });
                colors.insert(colors.end(), ds->btnListBg, ds->btnListBg + kListRowKeyCount);
                behavior.insert(behavior.end(), {
                    ds->hwndSoundLabel, ds->chkSound, ds->editSound, ds->btnBrowseSound, ds->btnTestSound,
                    ds->hwndSoundHint, ds->chkHideEnded, ds->chkUsageGraph, ds->chkRestoreUsageGraph,
                    ds->chkResourceLocks, ds->chkActivityStyle,
                    ds->lblEventRetention, ds->editEventRetention,
                    ds->lblRefreshInterval, ds->editRefreshInterval,
                    ds->chkEndBlink, ds->lblEndBlinkSec, ds->editEndBlinkSec,
                    ds->chkSupervisorHints, ds->lblHintAutoDismissSec, ds->editHintAutoDismissSec,
                    ds->lblSupervisorCommand, ds->editSupervisorCommand });
                maintenance.insert(maintenance.end(), { ds->btnShell, ds->btnReset, ds->btnClear });
                maintenance.insert(maintenance.end(), {
                    ds->lblWebUiTitle, ds->chkWebUi, ds->lblWebUiScope, ds->cmbWebUiScope,
                    ds->lblWebUiAllowedHosts, ds->editWebUiAllowedHosts });
                auto& about = ds->tabPages[4];
                about.insert(about.end(), { ds->hwndAboutTitle, ds->hwndAboutVersion, ds->hwndAboutDesc });
            }
            SelectSettingsTab(ds, 0);
            UpdateSettingsDialogTexts(hwnd);
            LoadWebUiDialogConfig(ds);
            ApplyNativeThemeToWindow(hwnd);
            // The tab pages are fully owner-painted with the dialog colours; theming the tab
            // control would repaint its body with the system (white) background.
            SetWindowTheme(ds->tabCtrl, L"", L"");
            return 0;
        }
        case WM_NOTIFY: {
            auto* nm = reinterpret_cast<NMHDR*>(lParam);
            if (ds && nm->idFrom == IDC_SET_TAB && nm->code == TCN_SELCHANGE) {
                SelectSettingsTab(ds, TabCtrl_GetCurSel(ds->tabCtrl));
                return 0;
            }
            break;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDCANCEL || id == IDOK) {
                if (ds) CommitSoundSettings(ds);
                if (ds) CommitEventRetention(ds);
                if (ds) CommitRefreshInterval(ds);
                if (ds) CommitEndBlinkSettings(ds);
                if (ds) CommitSupervisorSettings(ds);
                DestroyWindow(hwnd);
                return 0;
            }
            if (!ds) return 0;
            if (id == IDC_SET_SHOW_SUPERVISOR_HINTS) {
                CommitSupervisorSettings(ds);
                return 0;
            }
            if (id == IDC_SET_HINT_AUTO_DISMISS_SEC || id == IDC_SET_SUPERVISOR_COMMAND) {
                if (HIWORD(wParam) == EN_KILLFOCUS) CommitSupervisorSettings(ds);
                return 0;
            }
            if (id == IDC_SET_END_BLINK_ENABLED) {
                CommitEndBlinkSettings(ds);
                return 0;
            }
            if (id == IDC_SET_END_BLINK_SECONDS) {
                if (HIWORD(wParam) == EN_KILLFOCUS) CommitEndBlinkSettings(ds);
                return 0;
            }
            if (id == IDC_SET_REFRESH_INTERVAL) {
                if (HIWORD(wParam) == EN_KILLFOCUS) CommitRefreshInterval(ds);
                return 0;
            }
            if (id == IDC_SET_HIDE_ENDED) {
                ds->dash->hideEndedProcesses =
                    (SendMessageW(ds->chkHideEnded, BM_GETCHECK, 0, 0) == BST_CHECKED);
                SaveShellConfig(ds->dash);
                RefreshDashboardData(ds->dash);
                return 0;
            }
            if (id == IDC_SET_USAGE_GRAPH_MODE) {
                ds->dash->usageGraphMode =
                    (SendMessageW(ds->chkUsageGraph, BM_GETCHECK, 0, 0) == BST_CHECKED) ? 1 : 0;
                SaveShellConfig(ds->dash);
                InvalidateRect(ds->dash->hwndUsagePanel, nullptr, FALSE);
                return 0;
            }
            if (id == IDC_SET_LLM_MAX_EVENTS) {
                if (HIWORD(wParam) == EN_KILLFOCUS) CommitEventRetention(ds);
                return 0;
            }
            if (id == IDC_SET_RESTORE_USAGE_GRAPH) {
                ds->dash->restoreUsageGraph =
                    (SendMessageW(ds->chkRestoreUsageGraph, BM_GETCHECK, 0, 0) == BST_CHECKED);
                SetUsageHistoryPersistence(ds->dash->restoreUsageGraph);
                SaveShellConfig(ds->dash);
                return 0;
            }
            if (id == IDC_SET_WEBUI_ENABLED) {
                ApplyWebUiDialogConfig(ds);
                return 0;
            }
            if (id == IDC_SET_WEBUI_SCOPE) {
                if (HIWORD(wParam) == CBN_SELCHANGE) ApplyWebUiDialogConfig(ds);
                return 0;
            }
            if (id == IDC_SET_WEBUI_ALLOWED_HOSTS) {
                if (HIWORD(wParam) == EN_KILLFOCUS) ApplyWebUiDialogConfig(ds);
                return 0;
            }
            if (id == IDC_SET_RESOURCE_LOCKS) {
                ds->dash->resourceLocks =
                    (SendMessageW(ds->chkResourceLocks, BM_GETCHECK, 0, 0) == BST_CHECKED);
                SetResourceLocksEnabledIpc(ds->dash, ds->dash->resourceLocks);
                SaveShellConfig(ds->dash);
                return 0;
            }
            if (id == IDC_SET_ACTIVITY_STYLE) {
                g_activityStyle =
                    (SendMessageW(ds->chkActivityStyle, BM_GETCHECK, 0, 0) == BST_CHECKED);
                SaveShellConfig(ds->dash);
                if (ds->dash->hwndStreamPane) {
                    ApplyStreamPaneTheme(ds->dash);
                    if (ds->dash->streamPane) {
                        ds->dash->streamPane->lastEventCount = static_cast<size_t>(-1);
                        ReloadStreamDialogData(ds->dash->hwndStreamPane, ds->dash->streamPane);
                    }
                    InvalidateRect(ds->dash->hwndStreamPane, nullptr, TRUE);
                }
                RefreshDashboardData(ds->dash);
                return 0;
            }
            if (id == IDC_SET_SOUND_ENABLED) {
                CommitSoundSettings(ds);
                return 0;
            }
            if (id == IDC_SET_SOUND_BROWSE) {
                CommitSoundSettings(ds);
                std::wstring picked;
                if (gui_platform::GetGuiPlatform()->PickOpenFileName(
                        hwnd, L"WAV (*.wav)\0*.wav\0All Files (*.*)\0*.*\0", picked)) {
                    SetWindowTextW(ds->editSound, picked.c_str());
                    CommitSoundSettings(ds);
                }
                return 0;
            }
            if (id == IDC_SET_SOUND_TEST) {
                CommitSoundSettings(ds);
                PlayCompletionSound(*ds->dash);
                return 0;
            }
            if (id == IDC_SET_LANG_JA || id == IDC_SET_LANG_EN || id == IDC_SET_LANG_ES) {
                g_uiLanguage = (id == IDC_SET_LANG_JA) ? UiLanguage::Japanese :
                               (id == IDC_SET_LANG_ES) ? UiLanguage::Spanish : UiLanguage::English;
                SaveShellConfig(ds->dash);
                ApplyLanguageToUi(ds->dash);
                UpdateSettingsDialogTexts(hwnd);
                InvalidateRect(hwnd, nullptr, TRUE);
                InvalidateRect(ds->tabCtrl, nullptr, TRUE);
                return 0;
            }
            if (id >= IDC_SET_THEME_SYSTEM && id <= IDC_SET_THEME_PASTEL_YELLOW) {
                g_uiTheme = ThemeForButton(id);
                ApplyTheme();
                SaveShellConfig(ds->dash);
                ApplyListTheme(ds->dash);
                ApplyNativeThemeToWindow(ds->dash->hwndMain);
                ApplyNativeThemeToWindow(hwnd);
                SetWindowTheme(ds->tabCtrl, L"", L"");
                HWND shellDlg = FindWindowW(L"HanchoShellConfigWnd", nullptr);
                if (shellDlg) ApplyNativeThemeToWindow(shellDlg);
                HWND taskDlg = FindWindowW(L"HanchoTaskDetailsWnd", nullptr);
                if (taskDlg) ApplyNativeThemeToWindow(taskDlg);
                InvalidateRect(ds->dash->hwndMain, nullptr, TRUE);
                UpdateSettingsDialogTexts(hwnd);
                InvalidateRect(hwnd, nullptr, TRUE);
                InvalidateRect(ds->tabCtrl, nullptr, TRUE);
                return 0;
            }
            if (id >= IDC_SET_LIST_BG_RUNNING && id < IDC_SET_LIST_BG_RUNNING + kListRowKeyCount) {
                if (HIWORD(wParam) != BN_CLICKED) return 0;
                const int k = id - IDC_SET_LIST_BG_RUNNING;
                static COLORREF customColors[16] = {};
                CHOOSECOLORW cc = {};
                cc.lStructSize = sizeof(cc);
                cc.hwndOwner = hwnd;
                cc.rgbResult = ListRowBgOrDefault(static_cast<ListRowKey>(k), g_theme.panel);
                cc.lpCustColors = customColors;
                cc.Flags = CC_RGBINIT | CC_FULLOPEN;
                if (ChooseColorW(&cc)) {
                    g_listRowBg[ListRowMode()][k] = cc.rgbResult;
                    g_listRowBgSet[ListRowMode()][k] = true;
                    ApplyListRowBgChange(ds, hwnd);
                }
                return 0;
            }
            if (id == IDC_SET_LIST_BG_RESET) {
                for (int k = 0; k < kListRowKeyCount; ++k) g_listRowBgSet[ListRowMode()][k] = false;
                ApplyListRowBgChange(ds, hwnd);
                return 0;
            }
            if ((id == IDC_SET_PROC_FONT || id == IDC_SET_TASK_FONT) && HIWORD(wParam) == CBN_SELCHANGE) {
                HWND combo = (id == IDC_SET_PROC_FONT) ? ds->cmbProcFont : ds->cmbTaskFont;
                const int sel = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0));
                if (sel >= 0 && sel < kListFontSizeCount) {
                    if (id == IDC_SET_PROC_FONT) ds->dash->processListFontPx = kListFontSizes[sel];
                    else ds->dash->taskListFontPx = kListFontSizes[sel];
                    ApplyListFonts(ds->dash);
                    SaveShellConfig(ds->dash);
                }
                return 0;
            }
            if (id == IDC_SET_SHELL_CONFIG) {
                ShowShellConfigDialog(ds->dash);
                return 0;
            }
            if (id == IDC_SET_RESET_IDS) {
                ResetProcessIdsWithConfirm(ds->dash);
                return 0;
            }
            if (id == IDC_SET_CLEAR_ENDED) {
                if (ds && ds->dash) {
                    ClearFinishedIpc(ds->dash);
                    RefreshDashboardData(ds->dash);
                }
                return 0;
            } else if (id == IDC_SET_KPI_SAT && HIWORD(wParam) == CBN_SELCHANGE) {
                if (ds && ds->dash) {
                    int sel = (int)SendMessageW(ds->hwndSatCombo, CB_GETCURSEL, 0, 0);
                    float sat = 1.0f;
                    if (sel == 0) sat = 0.3f;
                    else if (sel == 1) sat = 0.5f;
                    else if (sel == 2) sat = 0.7f;
                    else sat = 1.0f;
                    g_kpiSaturation = sat;
                    SaveShellConfig(ds->dash);
                    InvalidateRect(ds->dash->hwndMain, &ds->dash->rcKpiArea, FALSE);
                }
                return 0;
            }
            return 0;
        }
        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis || !ds) break;
            if (dis->CtlType == ODT_TAB) {
                wchar_t text[256] = {};
                TCITEMW tie = {};
                tie.mask = TCIF_TEXT;
                tie.pszText = text;
                tie.cchTextMax = 256;
                TabCtrl_GetItem(dis->hwndItem, dis->itemID, &tie);
                const bool selected = (dis->itemState & ODS_SELECTED) != 0 ||
                                      TabCtrl_GetCurSel(dis->hwndItem) == static_cast<int>(dis->itemID);
                HBRUSH bgBrush = CreateSolidBrush(selected ? g_theme.panel : g_theme.windowBg);
                FillRect(dis->hDC, &dis->rcItem, bgBrush);
                DeleteObject(bgBrush);
                HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
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
                    HBRUSH accentBrush = CreateSolidBrush(g_theme.accent);
                    FillRect(dis->hDC, &bar, accentBrush);
                    DeleteObject(accentBrush);
                }
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, selected ? g_theme.accent : g_theme.text);
                HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC, ds->dash->hFontUi));
                RECT rcText = dis->rcItem;
                InflateRect(&rcText, -6, 0);
                DrawTextW(dis->hDC, text, -1, &rcText,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(dis->hDC, oldFont);
                return TRUE;
            }
            int id = static_cast<int>(dis->CtlID);
            bool radio = (id >= IDC_SET_LANG_JA && id <= IDC_SET_LANG_ES) ||
                         (id >= IDC_SET_THEME_SYSTEM && id <= IDC_SET_THEME_PASTEL_YELLOW);
            bool checked = false;
            if (id == IDC_SET_LANG_JA) checked = (g_uiLanguage == UiLanguage::Japanese);
            else if (id == IDC_SET_LANG_EN) checked = (g_uiLanguage == UiLanguage::English);
            else if (id == IDC_SET_LANG_ES) checked = (g_uiLanguage == UiLanguage::Spanish);
            else if (radio) checked = (ThemeButtonId(g_uiTheme) == id);

            bool pressed = (dis->itemState & ODS_SELECTED) != 0;
            // List row color buttons show their own color as a swatch.
            const bool listBgBtn = id >= IDC_SET_LIST_BG_RUNNING && id < IDC_SET_LIST_BG_RUNNING + kListRowKeyCount;
            const int listBgIdx = listBgBtn ? id - IDC_SET_LIST_BG_RUNNING : 0;
            const bool listBgSwatch = listBgBtn && g_listRowBgSet[ListRowMode()][listBgIdx] && !pressed;
            COLORREF bg = pressed ? g_theme.border : g_theme.panel;
            if (listBgSwatch) bg = g_listRowBg[ListRowMode()][listBgIdx];
            HBRUSH bgBrush = CreateSolidBrush(bg);
            FillRect(dis->hDC, &dis->rcItem, bgBrush);
            DeleteObject(bgBrush);
            HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
            HGDIOBJ oldPen = SelectObject(dis->hDC, borderPen);
            HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
            Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
            SelectObject(dis->hDC, oldBrush);
            SelectObject(dis->hDC, oldPen);
            DeleteObject(borderPen);

            if (dis->itemState & ODS_FOCUS) {
                RECT rcFocus = dis->rcItem;
                InflateRect(&rcFocus, -2, -2);
                DrawFocusRect(dis->hDC, &rcFocus);
            }

            RECT textRc = dis->rcItem;
            if (radio) {
                int cx = dis->rcItem.left + 16;
                int cy = (dis->rcItem.top + dis->rcItem.bottom) / 2;
                HPEN circlePen = CreatePen(PS_SOLID, 1, g_theme.muted);
                HGDIOBJ op = SelectObject(dis->hDC, circlePen);
                HGDIOBJ ob = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
                Ellipse(dis->hDC, cx - 7, cy - 7, cx + 7, cy + 7);
                SelectObject(dis->hDC, ob);
                SelectObject(dis->hDC, op);
                DeleteObject(circlePen);
                if (checked) {
                    HBRUSH dot = CreateSolidBrush(g_theme.accent);
                    HGDIOBJ ob2 = SelectObject(dis->hDC, dot);
                    Ellipse(dis->hDC, cx - 4, cy - 4, cx + 4, cy + 4);
                    SelectObject(dis->hDC, ob2);
                    DeleteObject(dot);
                }
                textRc.left += 30;
            }
            InflateRect(&textRc, -4, 0);
            wchar_t text[256] = {};
            GetWindowTextW(dis->hwndItem, text, 256);
            SetBkMode(dis->hDC, TRANSPARENT);
            COLORREF textCol = g_theme.text;
            if (listBgSwatch) textCol = ListRowTextFor(bg);
            SetTextColor(dis->hDC, textCol);
            HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC, ds->dash->hFontUi));
            UINT fmt = (radio ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
            DrawTextW(dis->hDC, text, -1, &textRc, fmt);
            SelectObject(dis->hDC, oldFont);
            return TRUE;
        }

        case WM_ERASEBKGND: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            FillRect(hdc, &rc, g_dialogBgBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetTextColor(hdc, g_theme.text);
            SetBkColor(hdc, g_theme.panel);
            return reinterpret_cast<LRESULT>(g_dialogBgBrush);
        }
        case WM_CLOSE:
            if (ds) CommitSoundSettings(ds);
            if (ds) CommitRefreshInterval(ds);
            DestroyWindow(hwnd);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void ShowSettingsDialog(DashboardState* st) {
    if (!st || !st->hwndMain) return;
    static bool regDone = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!regDone) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = SettingsWndProc;
        wc.hInstance = hInst;
        wc.hIcon = LoadAppIcon();
        wc.hIconSm = LoadAppSmallIcon();
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"HanchoSettingsWnd";
        RegisterClassExW(&wc);
        regDone = true;
    }

    HWND existing = FindWindowW(L"HanchoSettingsWnd", nullptr);
    if (existing) {
        ShowWindow(existing, SW_SHOW);
        SetForegroundWindow(existing);
        return;
    }

    auto* ds = new SettingsDlgState();
    ds->dash = st;
    RECT rcParent = {};
    GetWindowRect(st->hwndMain, &rcParent);
    int dlgW = 700;
    int dlgH = 870;
    int x = rcParent.left + ((rcParent.right - rcParent.left) - dlgW) / 2;
    int y = rcParent.top + ((rcParent.bottom - rcParent.top) - dlgH) / 2;

    HWND hDlg = CreateWindowExW(
        WS_EX_DLGMODALFRAME,
        L"HanchoSettingsWnd",
        Tr(L"設定", L"Settings", L"Ajustes"),
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_VISIBLE,
        x, y, dlgW, dlgH,
        st->hwndMain, nullptr, hInst, ds
    );
    if (hDlg) {
        ShowWindow(hDlg, SW_SHOW);
        SetForegroundWindow(hDlg);
    } else {
        delete ds;
    }
}

// ---------------------------------------------------------------------------
// Token usage panel (docked below the KPI cards)
// ---------------------------------------------------------------------------
static const int kUsageRangeCount = 4;
static const int64_t kUsageRangeSec[kUsageRangeCount] = {86400, 7 * 86400, 30 * 86400, 365 * 86400};
static const wchar_t* const kUsageRangeLabels[kUsageRangeCount] = {L"24h", L"7d", L"30d", L"12m"};
static const COLORREF kUsagePalette[] = {
    RGB(0x4F, 0x8C, 0xFF), RGB(0x3D, 0xD5, 0x98), RGB(0xFF, 0xB4, 0x54),
    RGB(0xFF, 0x6B, 0x6B), RGB(0xB4, 0x7A, 0xFF), RGB(0x2E, 0xC4, 0xE0),
    RGB(0xF7, 0x8F, 0xB4), RGB(0x9C, 0xD6, 0x5B)
};
static const int kUsagePaletteCount = static_cast<int>(sizeof(kUsagePalette) / sizeof(kUsagePalette[0]));

static std::wstring FormatCompactTokens(uint64_t v) {
    wchar_t buf[32] = {};
    if (v >= 1000000ULL) swprintf_s(buf, L"%.1fM", v / 1000000.0);
    else if (v >= 1000ULL) swprintf_s(buf, L"%.1fk", v / 1000.0);
    else swprintf_s(buf, L"%llu", static_cast<unsigned long long>(v));
    return buf;
}

static void GetUsagePillRects(int width, RECT out[kUsageRangeCount], RECT& closeRc) {
    int x = 136;
    for (int i = 0; i < kUsageRangeCount; ++i) {
        out[i] = {x, 7, x + 40, 24};
        x += 44;
    }
    closeRc = {width - 28, 5, width - 8, 25};
}

static COLORREF ColorForUsageKey(const std::string& name) {
    // Deterministic, well-spaced color per (provider/model) key using golden-ratio hue.
    unsigned int h = 2166137821u;
    for (unsigned char c : name) { h ^= c; h *= 16777619u; }
    double hue = (h % 360) / 360.0;
    // golden-ratio scatter for better separation
    hue = fmod(hue + (h % 97) / 97.0 * 0.6180339887, 1.0);
    double sat = 0.62 + (h % 20) / 100.0;
    double val = 0.78 + (h % 15) / 100.0;
    // HSV -> RGB
    int hi = static_cast<int>(hue * 6.0);
    double f = hue * 6.0 - hi;
    double p = val * (1.0 - sat);
    double q = val * (1.0 - f * sat);
    double t = val * (1.0 - (1.0 - f) * sat);
    double r = 0, g = 0, b = 0;
    switch (hi % 6) {
        case 0: r = val; g = t; b = p; break;
        case 1: r = q; g = val; b = p; break;
        case 2: r = p; g = val; b = t; break;
        case 3: r = p; g = q; b = val; break;
        case 4: r = t; g = p; b = val; break;
        default: r = val; g = p; b = q; break;
    }
    return RGB((int)(r * 255), (int)(g * 255), (int)(b * 255));
}

static COLORREF DesaturateColor(COLORREF c, float amount) {
    int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
    int gray = (r * 299 + g * 587 + b * 114) / 1000;
    int nr = (int)(r + (gray - r) * amount);
    int ng = (int)(g + (gray - g) * amount);
    int nb = (int)(b + (gray - b) * amount);
    if (nr < 0) nr = 0; if (nr > 255) nr = 255;
    if (ng < 0) ng = 0; if (ng > 255) ng = 255;
    if (nb < 0) nb = 0; if (nb > 255) nb = 255;
    return RGB(nr, ng, nb);
}

static void DrawUsagePanel(HWND hwnd, HDC hdc, DashboardState* st) {
    RECT rc = {};
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    int64_t selStart = 0, selEnd = 0;
    const auto& snaps = st->allSnapshots.empty() ? st->cachedSnapshots : st->allSnapshots;
    if (st->selectedProcId > 0) {
        for (const auto& s : snaps) {
            if (s.id == st->selectedProcId) { selStart = s.startEpochSec; selEnd = s.endEpochSec; break; }
        }
    } else if (st->hwndTaskList) {
        int idx = ListView_GetNextItem(st->hwndTaskList, -1, LVNI_SELECTED);
        if (idx >= 0 && idx < static_cast<int>(st->taskJobIds.size())) {
            int jid = 0;
            try { jid = std::stoi(st->taskJobIds[idx]); } catch (...) { jid = 0; }
            for (const auto& s : snaps) {
                if (jid > 0 && s.jobId == jid) { selStart = s.startEpochSec; selEnd = s.endEpochSec; break; }
            }
        }
    }
    if (selStart > 0 && selEnd == 0) selEnd = static_cast<int64_t>(time(nullptr));

    HBRUSH bg = CreateSolidBrush(g_theme.panel);
    FillRect(hdc, &rc, bg);
    DeleteObject(bg);
    HPEN border = CreatePen(PS_SOLID, 1, g_theme.border);
    HGDIOBJ oldPen = SelectObject(hdc, border);
    HGDIOBJ oldBrush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
    Rectangle(hdc, 0, 0, w, h);
    SelectObject(hdc, oldBrush);
    SelectObject(hdc, oldPen);
    DeleteObject(border);

    SetBkMode(hdc, TRANSPARENT);
    HFONT oldFont = static_cast<HFONT>(SelectObject(hdc, st->hFontUiBold ? st->hFontUiBold : GetStockObject(DEFAULT_GUI_FONT)));
    HBRUSH accent = CreateSolidBrush(DesaturateColor(g_theme.accent, 0.45f));
    RECT accentRc = {10, 8, 13, 22};
    FillRect(hdc, &accentRc, accent);
    DeleteObject(accent);
    SetTextColor(hdc, g_theme.text);
    RECT titleRc = {18, 6, 130, 24};
    DrawTextW(hdc, Tr(L"トークン使用量", L"TOKEN USAGE", L"USO DE TOKENS"), -1, &titleRc,
              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    SelectObject(hdc, st->hFontUi ? st->hFontUi : GetStockObject(DEFAULT_GUI_FONT));
    int rangeIdx = st->usageRangeIndex;
    if (rangeIdx < 0) rangeIdx = 0;
    if (rangeIdx >= kUsageRangeCount) rangeIdx = kUsageRangeCount - 1;

    RECT pills[kUsageRangeCount] = {};
    RECT closeRc = {};
    GetUsagePillRects(w, pills, closeRc);
    for (int i = 0; i < kUsageRangeCount; ++i) {
        bool active = (i == rangeIdx);
        HBRUSH pb = CreateSolidBrush(active ? DesaturateColor(g_theme.accent, 0.45f) : g_theme.panel);
        HPEN pp = CreatePen(PS_SOLID, 1, active ? g_theme.accent : g_theme.border);
        HGDIOBJ ob = SelectObject(hdc, pb);
        HGDIOBJ op = SelectObject(hdc, pp);
        RoundRect(hdc, pills[i].left, pills[i].top, pills[i].right, pills[i].bottom, 10, 10);
        SelectObject(hdc, op);
        SelectObject(hdc, ob);
        DeleteObject(pp);
        DeleteObject(pb);
        SetTextColor(hdc, active ? g_theme.windowBg : g_theme.muted);
        DrawTextW(hdc, kUsageRangeLabels[i], -1, &pills[i],
                  DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SetTextColor(hdc, g_theme.muted);
     DrawTextW(hdc, L"\x2715", -1, &closeRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);

    // View toggle button: token kinds (T) vs by provider/model (M).
    {
        RECT viewRc = {360, 7, 392, 24};
        HBRUSH vb = CreateSolidBrush(g_theme.panel);
        HPEN vp = CreatePen(PS_SOLID, 1, g_theme.border);
        HGDIOBJ ob = SelectObject(hdc, vb);
        HGDIOBJ op = SelectObject(hdc, vp);
        RoundRect(hdc, viewRc.left, viewRc.top, viewRc.right, viewRc.bottom, 8, 8);
        SelectObject(hdc, op);
        SelectObject(hdc, ob);
        DeleteObject(vp);
        DeleteObject(vb);
        SetTextColor(hdc, g_theme.text);
        const wchar_t* lbl = (st->usageGraphMode == 0) ? L"M" : L"T";
        DrawTextW(hdc, lbl, -1, &viewRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }

    int64_t now = static_cast<int64_t>(time(nullptr));
    int64_t since = now - kUsageRangeSec[rangeIdx];

    struct KeyStat {
        std::string name;
        uint64_t total = 0;
        uint64_t in = 0, out = 0, think = 0, cache = 0;
        COLORREF color = RGB(128, 128, 128);
        int64_t firstEpochSec = 0;
        int64_t lastEpochSec = 0;
    };
    std::vector<KeyStat> keys;
    std::unordered_map<std::string, int> keyIndex;
    uint64_t rangeTotal = 0;
    double rangeCost = 0.0;

    auto getKeyIndex = [&](const std::string& rawName) -> int {
        auto it = keyIndex.find(rawName);
        if (it != keyIndex.end()) return it->second;
        int idx = static_cast<int>(keys.size());
        keyIndex[rawName] = idx;
        KeyStat ks;
        ks.name = rawName;
        ks.color = ColorForUsageKey(ks.name);
        keys.push_back(ks);
        return idx;
    };

    int nBuckets = 12;
    double bucketSec = 1.0;
    int64_t windowStartBucket = 0;
    std::vector<std::vector<uint64_t>> vals;
    std::vector<std::vector<uint64_t>> valIn;
    std::vector<std::vector<uint64_t>> valOut;
    std::vector<std::vector<uint64_t>> valThink;
    std::vector<std::vector<uint64_t>> valCache;
    std::vector<uint64_t> bucketTotal;

    if (rangeIdx == 0) {
        // 24h: live U0 points (fallback to today's daily log if in-memory history empty)
        std::vector<LlmUsagePoint> points = GetLlmUsageHistory(since);
        if (points.empty()) {
            auto daily = ReadDailyUsageSummary("24h", "");
            for (const auto& d : daily) {
                LlmUsagePoint pt;
                pt.epochSec = d.firstEpochSec > 0 ? d.firstEpochSec : now;
                pt.inputTokens = d.inputTokens;
                pt.outputTokens = d.outputTokens;
                pt.thinkingTokens = d.thinkingTokens;
                pt.cacheReadTokens = d.cacheReadTokens;
                pt.cacheWriteTokens = d.cacheWriteTokens;
                pt.totalTokens = d.totalTokens;
                pt.costUsd = d.costUsd;
                pt.agentType = d.engine;
                pt.model = d.model;
                points.push_back(pt);
            }
        }
        for (const auto& p : points) {
            std::string name = p.agentType.empty() ? "unknown" : p.agentType;
            if (!p.model.empty()) name += " / " + p.model;
            else name += " / (default)";
            int idx = getKeyIndex(name);
            keys[idx].total += p.totalTokens;
            keys[idx].in += p.inputTokens;
            keys[idx].out += p.outputTokens;
            keys[idx].think += p.thinkingTokens;
            keys[idx].cache += p.cacheReadTokens + p.cacheWriteTokens;
            rangeTotal += p.totalTokens;
            rangeCost += p.costUsd;
            if (keys[idx].firstEpochSec == 0 || p.epochSec < keys[idx].firstEpochSec) keys[idx].firstEpochSec = p.epochSec;
            if (p.epochSec > keys[idx].lastEpochSec) keys[idx].lastEpochSec = p.epochSec;
        }

        int gx0_tmp = 58;
        int splitX_tmp = gx0_tmp + (w - gx0_tmp - 12) * 33 / 100;
        int graphW_tmp = (splitX_tmp - 6) - gx0_tmp;
        nBuckets = graphW_tmp / 7;
        if (nBuckets < 12) nBuckets = 12;
        if (nBuckets > 96) nBuckets = 96;
        bucketSec = 86400.0 / nBuckets;
        if (bucketSec < 1.0) bucketSec = 1.0;
        int64_t currentBucket = static_cast<int64_t>(now / bucketSec);
        windowStartBucket = currentBucket - nBuckets + 1;

        vals.assign(nBuckets, std::vector<uint64_t>(keys.size(), 0));
        valIn.assign(nBuckets, std::vector<uint64_t>(keys.size(), 0));
        valOut.assign(nBuckets, std::vector<uint64_t>(keys.size(), 0));
        valThink.assign(nBuckets, std::vector<uint64_t>(keys.size(), 0));
        valCache.assign(nBuckets, std::vector<uint64_t>(keys.size(), 0));
        bucketTotal.assign(nBuckets, 0);

        for (const auto& p : points) {
            int64_t pb = static_cast<int64_t>(p.epochSec / bucketSec);
            int bi = static_cast<int>(pb - windowStartBucket);
            if (bi < 0 || bi >= nBuckets) continue;
            std::string name = p.agentType.empty() ? "unknown" : p.agentType;
            if (!p.model.empty()) name += " / " + p.model;
            else name += " / (default)";
            int k = getKeyIndex(name);
            uint64_t pt = p.totalTokens;
            if (pt == 0) pt = p.inputTokens + p.outputTokens + p.thinkingTokens + p.cacheReadTokens + p.cacheWriteTokens;
            vals[bi][k] += pt;
            valIn[bi][k] += p.inputTokens;
            valOut[bi][k] += p.outputTokens;
            valThink[bi][k] += p.thinkingTokens;
            valCache[bi][k] += p.cacheReadTokens + p.cacheWriteTokens;
            bucketTotal[bi] += pt;
        }
    } else if (rangeIdx == 1 || rangeIdx == 2) {
        // 7d or 30d: daily buckets from U1 daily rollups
        int nDays = (rangeIdx == 1) ? 7 : 30;
        nBuckets = nDays;
        std::vector<std::string> dayKeys;
        dayKeys.reserve(nDays);
        for (int i = nDays - 1; i >= 0; --i) {
            time_t t = static_cast<time_t>(now - i * 86400);
            std::tm tm = {};
            LocalTimeFromSec(&t, &tm);
            char buf[32];
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
            dayKeys.push_back(buf);
        }
        std::string sinceStr = dayKeys.front();
        auto daily = ReadDailyUsageSummary(sinceStr, "");

        for (const auto& d : daily) {
            std::string name = d.engine.empty() ? "unknown" : d.engine;
            if (!d.model.empty()) name += " / " + d.model;
            else name += " / (default)";
            int idx = getKeyIndex(name);
            keys[idx].total += d.totalTokens;
            keys[idx].in += d.inputTokens;
            keys[idx].out += d.outputTokens;
            keys[idx].think += d.thinkingTokens;
            keys[idx].cache += d.cacheReadTokens + d.cacheWriteTokens;
            rangeTotal += d.totalTokens;
            rangeCost += d.costUsd;
            if (d.firstEpochSec > 0 && (keys[idx].firstEpochSec == 0 || d.firstEpochSec < keys[idx].firstEpochSec)) keys[idx].firstEpochSec = d.firstEpochSec;
            if (d.lastEpochSec > keys[idx].lastEpochSec) keys[idx].lastEpochSec = d.lastEpochSec;
        }

        vals.assign(nBuckets, std::vector<uint64_t>(keys.size(), 0));
        valIn.assign(nBuckets, std::vector<uint64_t>(keys.size(), 0));
        valOut.assign(nBuckets, std::vector<uint64_t>(keys.size(), 0));
        valThink.assign(nBuckets, std::vector<uint64_t>(keys.size(), 0));
        valCache.assign(nBuckets, std::vector<uint64_t>(keys.size(), 0));
        bucketTotal.assign(nBuckets, 0);

        for (const auto& d : daily) {
            int bi = -1;
            for (int i = 0; i < nDays; ++i) {
                if (dayKeys[i] == d.date) { bi = i; break; }
            }
            if (bi < 0 || bi >= nBuckets) continue;
            std::string name = d.engine.empty() ? "unknown" : d.engine;
            if (!d.model.empty()) name += " / " + d.model;
            else name += " / (default)";
            int k = getKeyIndex(name);
            uint64_t pt = d.totalTokens;
            if (pt == 0) pt = d.inputTokens + d.outputTokens + d.thinkingTokens + d.cacheReadTokens + d.cacheWriteTokens;
            vals[bi][k] += pt;
            valIn[bi][k] += d.inputTokens;
            valOut[bi][k] += d.outputTokens;
            valThink[bi][k] += d.thinkingTokens;
            valCache[bi][k] += d.cacheReadTokens + d.cacheWriteTokens;
            bucketTotal[bi] += pt;
        }
    } else {
        // 12m: monthly buckets from U1 rollups summed by month
        nBuckets = 12;
        std::vector<std::string> monthKeys;
        monthKeys.reserve(12);
        time_t nowT = static_cast<time_t>(now);
        std::tm curTm = {};
        LocalTimeFromSec(&nowT, &curTm);
        int curY = curTm.tm_year + 1900;
        int curM = curTm.tm_mon + 1;
        for (int i = 11; i >= 0; --i) {
            int m = curM - i;
            int y = curY;
            while (m <= 0) { m += 12; y--; }
            char buf[16];
            snprintf(buf, sizeof(buf), "%04d-%02d", y, m);
            monthKeys.push_back(buf);
        }
        std::string sinceStr = monthKeys.front() + "-01";
        auto daily = ReadDailyUsageSummary(sinceStr, "");

        for (const auto& d : daily) {
            std::string name = d.engine.empty() ? "unknown" : d.engine;
            if (!d.model.empty()) name += " / " + d.model;
            else name += " / (default)";
            int idx = getKeyIndex(name);
            keys[idx].total += d.totalTokens;
            keys[idx].in += d.inputTokens;
            keys[idx].out += d.outputTokens;
            keys[idx].think += d.thinkingTokens;
            keys[idx].cache += d.cacheReadTokens + d.cacheWriteTokens;
            rangeTotal += d.totalTokens;
            rangeCost += d.costUsd;
            if (d.firstEpochSec > 0 && (keys[idx].firstEpochSec == 0 || d.firstEpochSec < keys[idx].firstEpochSec)) keys[idx].firstEpochSec = d.firstEpochSec;
            if (d.lastEpochSec > keys[idx].lastEpochSec) keys[idx].lastEpochSec = d.lastEpochSec;
        }

        vals.assign(nBuckets, std::vector<uint64_t>(keys.size(), 0));
        valIn.assign(nBuckets, std::vector<uint64_t>(keys.size(), 0));
        valOut.assign(nBuckets, std::vector<uint64_t>(keys.size(), 0));
        valThink.assign(nBuckets, std::vector<uint64_t>(keys.size(), 0));
        valCache.assign(nBuckets, std::vector<uint64_t>(keys.size(), 0));
        bucketTotal.assign(nBuckets, 0);

        for (const auto& d : daily) {
            std::string ym = d.date.size() >= 7 ? d.date.substr(0, 7) : d.date;
            int bi = -1;
            for (int i = 0; i < 12; ++i) {
                if (monthKeys[i] == ym) { bi = i; break; }
            }
            if (bi < 0 || bi >= nBuckets) continue;
            std::string name = d.engine.empty() ? "unknown" : d.engine;
            if (!d.model.empty()) name += " / " + d.model;
            else name += " / (default)";
            int k = getKeyIndex(name);
            uint64_t pt = d.totalTokens;
            if (pt == 0) pt = d.inputTokens + d.outputTokens + d.thinkingTokens + d.cacheReadTokens + d.cacheWriteTokens;
            vals[bi][k] += pt;
            valIn[bi][k] += d.inputTokens;
            valOut[bi][k] += d.outputTokens;
            valThink[bi][k] += d.thinkingTokens;
            valCache[bi][k] += d.cacheReadTokens + d.cacheWriteTokens;
            bucketTotal[bi] += pt;
        }
    }

    SelectObject(hdc, st->hFontUiBold ? st->hFontUiBold : GetStockObject(DEFAULT_GUI_FONT));
    std::wstring summary = FormatCompactTokens(rangeTotal) + L" tok";
    if (rangeCost > 0.0) {
        wchar_t cbuf[32] = {};
        swprintf_s(cbuf, L"  \x00B7  $%.4f", rangeCost);
        summary += cbuf;
    }
    RECT sumRc = {closeRc.left - 270, 6, closeRc.left - 116, 24};
    SetTextColor(hdc, g_theme.accent);
    DrawTextW(hdc, summary.c_str(), -1, &sumRc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    int gx0 = 58;
    int splitX = gx0 + (w - gx0 - 12) * 33 / 100;
    int gx1 = splitX - 6;
    int mx0 = splitX + 6;
    int mx1 = w - 12;
    int gy0 = 30;
    int gy1 = h - 26;
    int graphW = gx1 - gx0;
    int graphH = gy1 - gy0;
    if (graphW < 40 || graphH < 20) {
        SelectObject(hdc, oldFont);
        return;
    }

    if (keys.empty()) {
        SetTextColor(hdc, g_theme.muted);
        RECT emptyRc = {gx0, gy0, gx1, gy1};
        const auto& snaps = st->allSnapshots.empty() ? st->cachedSnapshots : st->allSnapshots;
        bool fetching = false;
        for (const auto& sp : snaps) {
            if (sp.state == ProcState::Running || sp.streamEventCount > 0) { fetching = true; break; }
        }
        const wchar_t* msg = fetching
            ? Tr(L"データを取得中です...", L"Fetching data...", L"Obteniendo datos...")
            : Tr(L"まだ使用量データがありません", L"No token usage recorded yet", L"Aún no hay datos de uso");
        DrawTextW(hdc, msg, -1, &emptyRc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        SelectObject(hdc, oldFont);
        return;
    }

    uint64_t maxBucket = 1;
    for (uint64_t v : bucketTotal) if (v > maxBucket) maxBucket = v;

    // Highlight selected process/job time range in usage graph background.
    if (selStart > 0 && !keys.empty()) {
        int64_t ws = since;
        int64_t we = now;
        if (rangeIdx == 0) {
            ws = static_cast<int64_t>(windowStartBucket * bucketSec);
            we = ws + static_cast<int64_t>(nBuckets * bucketSec);
        }
        int64_t s = std::max(selStart, ws);
        int64_t e = std::min(selEnd > 0 ? selEnd : now, we);
        if (e > s && we > ws) {
            int x0 = gx0 + static_cast<int>((static_cast<double>(s - ws) / (we - ws)) * graphW);
            int x1 = gx0 + static_cast<int>((static_cast<double>(e - ws) / (we - ws)) * graphW);
            if (x1 < x0 + 1) x1 = x0 + 1;
            if (x0 < gx0) x0 = gx0;
            if (x1 > gx1) x1 = gx1;
            HBRUSH gb = CreateSolidBrush(g_theme.border);
            RECT grc = {x0, gy0, x1, gy1};
            FillRect(hdc, &grc, gb);
            DeleteObject(gb);
        }
    }

    // Grid + Y-axis labels
    {
        HPEN grid = CreatePen(PS_DOT, 1, g_theme.border);
        HGDIOBJ op = SelectObject(hdc, grid);
        SelectObject(hdc, st->hFontUi ? st->hFontUi : GetStockObject(DEFAULT_GUI_FONT));
        SetTextColor(hdc, g_theme.muted);
        for (int g = 1; g <= 4; ++g) {
            int y = gy1 - graphH * g / 4;
            if (g < 4) {
                MoveToEx(hdc, gx0, y, nullptr);
                LineTo(hdc, gx1, y);
            }
            uint64_t val = (uint64_t)((double)maxBucket * g / 4.0);
            std::wstring lbl = FormatCompactTokens(val);
            RECT lr = {gx0 - 46, y - 8, gx0 - 2, y + 8};
            DrawTextW(hdc, lbl.c_str(), -1, &lr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        }
        SelectObject(hdc, op);
        DeleteObject(grid);
    }

    int slot = graphW / nBuckets;
    if (slot < 1) slot = 1;
    for (int bi = 0; bi < nBuckets; ++bi) {
        if (bucketTotal[bi] == 0) continue;
        int x0 = gx0 + bi * slot;
        int x1 = x0 + slot - 1;
        if (x1 <= x0) x1 = x0 + 1;
        int barH = static_cast<int>((static_cast<double>(bucketTotal[bi]) / maxBucket) * graphH);
        if (barH < 1) barH = 1;
        int used = 0;
        for (size_t k = 0; k < keys.size(); ++k) {
            uint64_t v = vals[bi][k];
            if (v == 0) continue;
            int sh = static_cast<int>((static_cast<double>(v) / bucketTotal[bi]) * barH);
            if (sh < 1) sh = 1;
            int remain = barH - used;
            if (sh > remain) sh = remain;
            if (sh <= 0) break;
            if (st->usageGraphMode == 1) {
                RECT segRc = {x0, gy1 - used - sh, x1, gy1 - used};
                HBRUSH sb = CreateSolidBrush(keys[k].color);
                FillRect(hdc, &segRc, sb);
                DeleteObject(sb);
            } else {
                // Draw sub-segments by token kind (input / output / thinking / cache).
                struct Seg { uint64_t v; COLORREF c; };
                Seg segs[4] = {
                    {valIn[bi][k], RGB(0x4F, 0x8C, 0xFF)},
                    {valOut[bi][k], RGB(0x3D, 0xD5, 0x98)},
                    {valThink[bi][k], RGB(0xB4, 0x7A, 0xFF)},
                    {valCache[bi][k], RGB(0x5C, 0x6C, 0x8C)}
                };
                uint64_t segSum = valIn[bi][k] + valOut[bi][k] + valThink[bi][k] + valCache[bi][k];
                if (segSum == 0) segSum = v;
                int subUsed = 0;
                for (int s = 0; s < 4; ++s) {
                    if (segs[s].v == 0) continue;
                    int ssh = static_cast<int>((static_cast<double>(segs[s].v) / segSum) * sh);
                    if (ssh < 1) ssh = 1;
                    int subRemain = sh - subUsed;
                    if (ssh > subRemain) ssh = subRemain;
                    if (ssh <= 0) break;
                    RECT segRc = {x0, gy1 - used - subUsed - ssh, x1, gy1 - used - subUsed};
                    HBRUSH sb = CreateSolidBrush(segs[s].c);
                    FillRect(hdc, &segRc, sb);
                    DeleteObject(sb);
                    subUsed += ssh;
                    if (subUsed >= sh) break;
                }
            }
            used += sh;
            if (used >= barH) break;
        }
    }
    // Per-provider/model horizontal bar chart on the right.
    {
        RECT mtitle = {mx0, gy0 - 14, mx1, gy0};
        SetTextColor(hdc, g_theme.muted);
        SelectObject(hdc, st->hFontUiBold ? st->hFontUiBold : GetStockObject(DEFAULT_GUI_FONT));
        DrawTextW(hdc, Tr(L"モデル別使用量", L"USAGE BY MODEL", L"USO POR MODELO"), -1, &mtitle,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

        uint64_t maxModel = 1;
        for (const auto& k : keys) if (k.total > maxModel) maxModel = k.total;
        int n = static_cast<int>(keys.size());
        int availH = gy1 - gy0;
        int rowH = (n > 0) ? (availH / n) : availH;
        if (rowH < 30) rowH = 30;
        if (rowH > 44) rowH = 44;
        int labelW = (std::min)(300, mx1 - mx0 - 30);
        int barX0 = mx0 + labelW + 4;
        int y = gy0 + 2;
        SelectObject(hdc, st->hFontUi ? st->hFontUi : GetStockObject(DEFAULT_GUI_FONT));
        for (int i = 0; i < n && y + rowH <= gy1 + 4; ++i) {
            std::wstring lbl = Utf8ToWide(keys[i].name);
            RECT labRc = {mx0, y, mx0 + labelW, y + rowH};
            SetTextColor(hdc, g_theme.text);
            DrawTextW(hdc, lbl.c_str(), -1, &labRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            int barW = (int)((double)keys[i].total / maxModel * (mx1 - barX0));
            if (barW < 2) barW = 2;
            if (barX0 + barW > mx1) barW = mx1 - barX0;
            int barH = 14;
            int barTop = y + (rowH - barH) / 2;
            if (barTop < y) barTop = y;
            RECT barRc = {barX0, barTop, barX0 + barW, barTop + barH};
            uint64_t segSumR = keys[i].in + keys[i].out + keys[i].think + keys[i].cache;
            if (segSumR == 0) segSumR = keys[i].total;
            COLORREF baseR = keys[i].color;
            struct RSeg { uint64_t v; COLORREF c; };
            RSeg rsegs[4] = {
                {keys[i].in, baseR},
                {keys[i].out, DesaturateColor(baseR, 0.75f)},
                {keys[i].think, DesaturateColor(baseR, 0.25f)},
                {keys[i].cache, DesaturateColor(baseR, 0.85f)}
            };
            int rx = barX0;
            for (int s = 0; s < 4; ++s) {
                if (rsegs[s].v == 0) continue;
                int sw = static_cast<int>((static_cast<double>(rsegs[s].v) / segSumR) * barW);
                if (sw < 1) sw = 1;
                if (rx + sw > barX0 + barW) sw = barX0 + barW - rx;
                if (sw <= 0) break;
                RECT segRc = {rx, barTop, rx + sw, barTop + barH};
                HBRUSH sb = CreateSolidBrush(rsegs[s].c);
                FillRect(hdc, &segRc, sb);
                DeleteObject(sb);
                rx += sw;
            }
            if (rx < barX0 + barW) {
                RECT rem = {rx, barTop, barX0 + barW, barTop + barH};
                HBRUSH rb = CreateSolidBrush(keys[i].color);
                FillRect(hdc, &rem, rb);
                DeleteObject(rb);
            }

            // Build overlay string: "total remaining [i.. o.. t.. c.. dur]"
            int64_t nowEpoch = static_cast<int64_t>(time(nullptr));
            int64_t elapsed = (keys[i].lastEpochSec > 0) ? (nowEpoch - keys[i].lastEpochSec) : 0;
            int64_t e2 = elapsed;
            if (e2 < 0) e2 = 0;
            int rh = (int)(e2 / 3600), rm = (int)((e2 % 3600) / 60), rs = (int)(e2 % 60);
            wchar_t rbuf[16] = {};
            swprintf_s(rbuf, L"%d:%02d:%02d", rh, rm, rs);
            std::wstring val = FormatCompactTokens(keys[i].total) + L" " + std::wstring(rbuf);

            int64_t dur = 0;
            if (keys[i].firstEpochSec > 0 && keys[i].lastEpochSec >= keys[i].firstEpochSec) {
                dur = keys[i].lastEpochSec - keys[i].firstEpochSec;
            }
            int dh = (int)(dur / 3600), dm = (int)((dur % 3600) / 60), ds2 = (int)(dur % 60);
            wchar_t dbuf[16] = {};
            swprintf_s(dbuf, L"%d:%02d:%02d", dh, dm, ds2);
            std::wstring b2 = L"[i" + FormatCompactTokens(keys[i].in) +
                              L" o" + FormatCompactTokens(keys[i].out) +
                              L" t" + FormatCompactTokens(keys[i].think) +
                              L" c" + FormatCompactTokens(keys[i].cache) +
                              L" " + std::wstring(dbuf) + L"]";
            std::wstring full = val + L" " + b2;

            RECT valRc = {barX0 + 2, barTop, (barW < 80) ? mx1 : barX0 + barW - 2, barTop + barH};
            if (barW < 80) valRc.left = barX0 + 2;
            SetTextColor(hdc, (keys[i].lastEpochSec > 0 && elapsed >= 5 * 3600) ? RGB(0xFF, 0xC0, 0xC0) : RGB(0xFF, 0xFF, 0xFF));
            SelectObject(hdc, st->hFontUi ? st->hFontUi : GetStockObject(DEFAULT_GUI_FONT));
            DrawTextW(hdc, full.c_str(), -1, &valRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            y += 16;
        }
    }

    SetTextColor(hdc, g_theme.muted);
    RECT axisL = {gx0, gy1 + 2, gx0 + 80, gy1 + 16};
    RECT axisR = {gx1 - 80, gy1 + 2, gx1, gy1 + 16};
    wchar_t tbuf[32] = {};
    wchar_t nowBuf[32] = {};
    struct tm tmv = {};
    time_t stv = static_cast<time_t>(since);
    localtime_s(&tmv, &stv);
    time_t nowT = time(nullptr);
    struct tm nowTm = {};
    localtime_s(&nowTm, &nowT);

    if (rangeIdx == 0) {
        swprintf_s(tbuf, L"%02d:%02d", tmv.tm_hour, tmv.tm_min);
        swprintf_s(nowBuf, L"%02d:%02d", nowTm.tm_hour, nowTm.tm_min);
    } else if (rangeIdx == 1 || rangeIdx == 2) {
        swprintf_s(tbuf, L"%02d/%02d", tmv.tm_mon + 1, tmv.tm_mday);
        swprintf_s(nowBuf, L"%02d/%02d", nowTm.tm_mon + 1, nowTm.tm_mday);
    } else {
        swprintf_s(tbuf, L"%04d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1);
        swprintf_s(nowBuf, L"%04d-%02d", nowTm.tm_year + 1900, nowTm.tm_mon + 1);
    }
    DrawTextW(hdc, tbuf, -1, &axisL, DT_LEFT | DT_SINGLELINE);
    DrawTextW(hdc, nowBuf, -1, &axisR, DT_RIGHT | DT_SINGLELINE);

    SelectObject(hdc, oldFont);
}

static LRESULT CALLBACK UsagePanelWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* st = reinterpret_cast<DashboardState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
            SetTimer(hwnd, 1, 1000, nullptr);
            return 0;
        }
        case WM_TIMER:
            if (wParam == 1) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps = {};
            HDC hdc = BeginPaint(hwnd, &ps);
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            int w = rc.right - rc.left;
            int h = rc.bottom - rc.top;
            if (w > 0 && h > 0 && st) {
                HDC memDc = CreateCompatibleDC(hdc);
                HBITMAP memBm = CreateCompatibleBitmap(hdc, w, h);
                HGDIOBJ oldBm = SelectObject(memDc, memBm);
                DrawUsagePanel(hwnd, memDc, st);
                BitBlt(hdc, 0, 0, w, h, memDc, 0, 0, SRCCOPY);
                SelectObject(memDc, oldBm);
                DeleteObject(memBm);
                DeleteDC(memDc);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN: {
            if (!st) break;
            POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            RECT pills[kUsageRangeCount] = {};
            RECT closeRc = {};
            GetUsagePillRects(rc.right, pills, closeRc);
            RECT viewRc = {360, 7, 392, 24};
            if (PtInRect(&viewRc, pt)) {
                st->usageGraphMode = (st->usageGraphMode == 0) ? 1 : 0;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (PtInRect(&closeRc, pt)) {
                st->showUsagePanel = false;
                RECT mainRc = {};
                GetClientRect(st->hwndMain, &mainRc);
                LayoutControls(st, mainRc.right - mainRc.left, mainRc.bottom - mainRc.top);
                InvalidateRect(st->hwndMain, nullptr, TRUE);
                return 0;
            }
            for (int i = 0; i < kUsageRangeCount; ++i) {
                if (PtInRect(&pills[i], pt)) {
                    st->usageRangeIndex = i;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
            return 0;
        }
        case WM_NCDESTROY:
            KillTimer(hwnd, 1);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Supervisor Guidance & Audit Hint Window (Card 070)
// ---------------------------------------------------------------------------

static void LaunchSupervisorTerminal(DashboardState* st) {
    if (!st) return;
    std::string cmd = st->supervisorCommand;
    if (cmd.empty()) cmd = "claude";
    StartProcessRequest req;
    req.mode = ExecMode::ConPTY;
    req.name = "supervisor";
    req.commandLine = cmd;

    std::string err;
    int id = -1;
    if (st->pm) {
        id = st->pm->StartProcess(req, err);
    } else {
        nlohmann::json startReq = {
            {"action", "start"},
            {"cmd", req.commandLine},
            {"name", req.name},
            {"mode", ExecModeToString(req.mode)},
            {"wait", false}
        };
        nlohmann::json startResp;
        if (GuiIpcCall(st, startReq, startResp, &err) && startResp.value("ok", false)) {
            id = startResp.value("id", -1);
        }
    }
    if (id < 0) {
        std::wstring werr = Utf8ToWide(err);
        gui_platform::GetGuiPlatform()->ShowMessageBox(st->hwndMain, L"Failed to Launch Supervisor", werr, gui_platform::MessageBoxType::Error);
    } else {
        st->selectedProcId = id;
        RefreshDashboardData(st);
        TerminalView* tv = st->pm ? st->pm->GetTerminalView(id) : nullptr;
        if (tv && tv->Hwnd()) {
            SetFocus(tv->Hwnd());
        }
    }
}

static void RepositionHintWindow(DashboardState* st) {
    if (!st || !st->hwndHintWnd || !IsWindow(st->hwndHintWnd)) return;
    RECT rcMain = {};
    GetWindowRect(st->hwndMain, &rcMain);
    int hintW = 460;
    int hintH = 210;
    int x = rcMain.right - hintW - 20;
    int y = rcMain.bottom - hintH - 45;
    if (x < rcMain.left + 10) x = rcMain.left + 10;
    if (y < rcMain.top + 10) y = rcMain.top + 10;
    SetWindowPos(st->hwndHintWnd, HWND_TOP, x, y, hintW, hintH, SWP_NOACTIVATE);
}

struct SupervisorHintInfo {
    std::string signature;
    std::wstring category;
    std::wstring summary;
    std::wstring prompt;
};

static SupervisorHintInfo AnalyzeStateForHint(DashboardState* st) {
    SupervisorHintInfo info;
    if (!st) return info;

    auto makePrompt = [](const std::wstring& ja, const std::wstring& en, const std::wstring& es) -> std::wstring {
        if (g_uiLanguage == UiLanguage::Japanese) return ja;
        if (g_uiLanguage == UiLanguage::Spanish) return es;
        return en;
    };

    // 1. Check for Timing Divergence in lastTaskListJson
    if (!st->lastTaskListJson.empty()) {
        auto jobs = SplitJsonObjectArray(st->lastTaskListJson, "jobs");
        for (const auto& j : jobs) {
            std::string tfKey = "\"timing_feedback\"";
            size_t tfPos = j.find(tfKey);
            if (tfPos != std::string::npos) {
                std::string acc = JsonGetString(j, "accuracy", "");
                if (acc == "diverged_over" || acc == "diverged_under") {
                    std::string jid = JsonGetString(j, "id", "");
                    std::string jname = JsonGetString(j, "name", jid);
                    int64_t dur = JsonGetInt(j, "duration_sec", 0);
                    int64_t est = JsonGetInt(j, "estimate_sec", 0);
                    std::string advice = JsonGetString(j, "advice", "");

                    info.signature = "timing:" + jid + ":" + acc;
                    info.category = Tr(L"⏱ 見積もり乖離検知 (Audit LLM)",
                                       L"⏱ Timing Divergence (Audit LLM)",
                                       L"⏱ Desviación de estimación (Audit LLM)");
                    if (!advice.empty()) {
                        info.summary = Utf8ToWide(advice);
                    } else if (dur > est) {
                        info.summary = Tr(L"ジョブ '", L"Job '", L"Trabajo '") + Utf8ToWide(jname) +
                                       Tr(L"' の実所要時間が事前見積もりを大幅に超過しました。",
                                          L"' actual duration greatly exceeded estimate.",
                                          L"' la duración real superó ampliamente la estimación.");
                    } else {
                        info.summary = Tr(L"ジョブ '", L"Job '", L"Trabajo '") + Utf8ToWide(jname) +
                                       Tr(L"' の実所要時間は事前見積もりより大幅に短時間でした。",
                                          L"' actual duration was far below estimate.",
                                          L"' la duración real fue muy inferior a la estimación.");
                    }
                    std::wstring jidW = Utf8ToWide(jid);
                    info.prompt = makePrompt(
                        L"hancho-cli taskqueue list を確認してください。ジョブ '" + jidW +
                        L"' の実所要時間 (" + std::to_wstring(dur) + L"s / 見積もり " + std::to_wstring(est) +
                        L"s) に基づき、後続ジョブの estimate_sec を再調整し、必要に応じてタスクを分割してください。",
                        L"Check hancho-cli taskqueue list. Based on job '" + jidW +
                        L"' actual duration (" + std::to_wstring(dur) + L"s vs estimate " + std::to_wstring(est) +
                        L"s), recalibrate subsequent estimate_sec and consider task partitioning.",
                        L"Revise hancho-cli taskqueue list. Según la duración de '" + jidW +
                        L"' (" + std::to_wstring(dur) + L"s vs " + std::to_wstring(est) +
                        L"s), recalibre estimate_sec para tareas posteriores.");
                    return info;
                }
            }
        }
    }

    // 2. Check for Behavior Tree Escalation (waiting_for_llm)
    try {
        std::vector<std::string> btErrors;
        std::vector<BtDebugNode> trees = LoadBtDebugTrees(GetBehaviorTreesDirWide(), btErrors);
        std::function<const BtDebugNode*(const BtDebugNode&)> findWaiting = [&](const BtDebugNode& n) -> const BtDebugNode* {
            if (n.state == "waiting_for_llm" || n.state == "waiting_for_decision" || n.state == "escalated") {
                return &n;
            }
            for (const auto& ch : n.children) {
                const BtDebugNode* f = findWaiting(ch);
                if (f) return f;
            }
            return nullptr;
        };
        for (const auto& root : trees) {
            const BtDebugNode* waiting = findWaiting(root);
            if (waiting) {
                info.signature = "bt:" + waiting->btId + ":" + waiting->nodeId;
                info.category = Tr(L"🧠 LLM判断要請 (BT Escalation)",
                                   L"🧠 LLM Decision Required (BT)",
                                   L"🧠 Decisión LLM Requerida (BT)");
                std::wstring nodeName = waiting->label.empty() ? Utf8ToWide(waiting->nodeId) : Utf8ToWide(waiting->label);
                info.summary = Tr(L"ビヘイビアツリー '", L"Behavior tree '", L"Árbol '") +
                               Utf8ToWide(waiting->btId) + Tr(L"' のノード '", L"' node '", L"' nodo '") +
                               nodeName +
                               Tr(L"' が Supervisor LLM の判断を求めて待機しています。",
                                  L"' is waiting for Supervisor LLM decision.",
                                  L"' espera decisión del Supervisor LLM.");
                std::wstring btIdW = Utf8ToWide(waiting->btId);
                std::wstring nodeW = Utf8ToWide(waiting->nodeId);
                info.prompt = makePrompt(
                    L"hancho-cli bt get " + btIdW +
                    L" でエスカレーションパケットを確認し、結果を hancho-cli bt answer " + btIdW +
                    L" --node " + nodeW + L" --result <success/failure> で応答してください。",
                    L"Check escalation details with hancho-cli bt get " + btIdW +
                    L" and answer with hancho-cli bt answer " + btIdW +
                    L" --node " + nodeW + L" --result <success/failure>",
                    L"Revise la escalación con hancho-cli bt get " + btIdW +
                    L" y responda con hancho-cli bt answer " + btIdW +
                    L" --node " + nodeW + L" --result <success/failure>");
                return info;
            }
        }
    } catch (...) {}

    // 3. Check for Failed jobs
    if (!st->lastTaskListJson.empty()) {
        auto jobs = SplitJsonObjectArray(st->lastTaskListJson, "jobs");
        for (const auto& j : jobs) {
            std::string state = JsonGetString(j, "state", "");
            if (state == "failed") {
                std::string jid = JsonGetString(j, "id", "");
                std::string jname = JsonGetString(j, "name", jid);
                info.signature = "failed:" + jid;
                info.category = Tr(L"⚠️ ジョブ失敗検知", L"⚠️ Job Failure Detected", L"⚠️ Fallo de trabajo detectado");
                info.summary = Tr(L"ジョブ '", L"Job '", L"Trabajo '") + Utf8ToWide(jname) +
                               Tr(L"' が失敗状態で停止しています。", L"' is in failed state.", L"' está en estado fallido.");
                std::wstring jidW = Utf8ToWide(jid);
                info.prompt = makePrompt(
                    L"hancho-cli taskqueue inspect " + jidW +
                    L" で失敗診断ログを確認し、修復後に hancho-cli taskqueue requeue " + jidW +
                    L" を実行してください。",
                    L"Inspect failure diagnostics via hancho-cli taskqueue inspect " + jidW +
                    L", resolve the issue, and rerun via hancho-cli taskqueue requeue " + jidW,
                    L"Inspeccione el fallo con hancho-cli taskqueue inspect " + jidW +
                    L", solucione el problema y reintente con hancho-cli taskqueue requeue " + jidW);
                return info;
            }
        }
    }

    // 4. Check for Resource Contention
    if (!st->lastTaskListJson.empty()) {
        auto jobs = SplitJsonObjectArray(st->lastTaskListJson, "jobs");
        for (const auto& j : jobs) {
            std::string resWait = JsonGetString(j, "resource_wait", "");
            if (!resWait.empty() && resWait != "null") {
                std::string jid = JsonGetString(j, "id", "");
                info.signature = "res:" + resWait + ":" + jid;
                info.category = Tr(L"🔒 リソース排他待機", L"🔒 Resource Contention", L"🔒 Conflicto de recursos");
                info.summary = Tr(L"ジョブ '", L"Job '", L"Trabajo '") + Utf8ToWide(jid) +
                               Tr(L"' がリソース '", L"' is waiting for resource '", L"' espera el recurso '") +
                               Utf8ToWide(resWait) + Tr(L"' のロック解放を待機しています。", L"'.", L"'.");
                std::wstring resW = Utf8ToWide(resWait);
                info.prompt = makePrompt(
                    L"リソース '" + resW +
                    L"' の排他競合が発生しています。タスクキュー定義のロック種別 (read/write) または依存順序を見直してください。",
                    L"Resource lock contention on '" + resW +
                    L"'. Review lock mode (read/write) or job dependencies in taskqueue definitions.",
                    L"Conflicto de bloqueo en '" + resW +
                    L"'. Revise modo de bloqueo (read/write) o dependencias.");
                return info;
            }
        }
    }

    // 5. Check if all tasks succeeded
    if (!st->lastTaskListJson.empty()) {
        auto jobs = SplitJsonObjectArray(st->lastTaskListJson, "jobs");
        if (!jobs.empty()) {
            bool allSucceeded = true;
            for (const auto& j : jobs) {
                std::string s = JsonGetString(j, "state", "");
                if (s != "succeeded") { allSucceeded = false; break; }
            }
            if (allSucceeded) {
                info.signature = "all_succeeded:" + std::to_string(jobs.size());
                info.category = Tr(L"✅ 全タスク完了", L"✅ All Tasks Completed", L"✅ Tareas completadas");
                info.summary = makePrompt(L"タスクキュー内のすべてのジョブ (" + std::to_wstring(jobs.size()) + L"件) が正常完了しました。",
                                          L"All " + std::to_wstring(jobs.size()) + L" jobs in task queue succeeded.",
                                          L"Todos los " + std::to_wstring(jobs.size()) + L" trabajos se completaron con éxito.");
                info.prompt = makePrompt(
                    L"全タスクが完了しました。git diff やテスト結果を検証し、コミットまたは次のフェーズ指示を投入してください。",
                    L"All tasks completed. Verify git diff and test results, then commit or proceed to next phase instructions.",
                    L"Tareas completadas. Verifique git diff y pruebas, luego confirme o proceda a la siguiente fase.");
                return info;
            }
        }
    }

    // 6. Idle
    info.signature = "idle";
    info.category = Tr(L"💡 Supervisor ガイダンス", L"💡 Supervisor Guidance", L"💡 Guía de Supervisor");
    info.summary = Tr(L"Hancho は待機状態です。新しい自律タスクまたはコマンドを投入できます。",
                      L"Hancho is idle. Ready for new autonomous tasks or commands.",
                      L"Hancho está inactivo. Listo para nuevas tareas o comandos autónomos.");
    info.prompt = makePrompt(
        L"hancho-cli taskqueue add または hancho-cli bt run を実行して、自律実行フローを開始してください。",
        L"Run hancho-cli taskqueue add or hancho-cli bt run to initiate autonomous execution flow.",
        L"Ejecute hancho-cli taskqueue add o hancho-cli bt run para iniciar el flujo de ejecución.");
    return info;
}

struct HintWndState {
    DashboardState* dash = nullptr;
    HWND hwndCategory = nullptr;
    HWND hwndCountdown = nullptr;
    HWND hwndSummary = nullptr;
    HWND hwndPromptEdit = nullptr;
    HWND btnCopyPrompt = nullptr;
    HWND btnLaunchSupervisor = nullptr;
    HWND btnClose = nullptr;
    int copySuccessTicks = 0;
};

static LRESULT CALLBACK HintWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* hs = reinterpret_cast<HintWndState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            hs = reinterpret_cast<HintWndState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(hs));
            HINSTANCE hInst = cs->hInstance;

            hs->hwndCategory = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                12, 10, 300, 20, hwnd, nullptr, hInst, nullptr);
            hs->hwndCountdown = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_RIGHT,
                315, 10, 130, 20, hwnd, nullptr, hInst, nullptr);
            hs->hwndSummary = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT,
                12, 34, 434, 36, hwnd, nullptr, hInst, nullptr);
            hs->hwndPromptEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                12, 74, 434, 88, hwnd, nullptr, hInst, nullptr);

            hs->btnCopyPrompt = CreateWindowExW(0, L"BUTTON", Tr(L"📋 プロンプトをコピー", L"📋 Copy Prompt", L"📋 Copiar Prompt"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                12, 170, 160, 28, hwnd, (HMENU)IDC_HINT_COPY, hInst, nullptr);
            hs->btnLaunchSupervisor = CreateWindowExW(0, L"BUTTON", Tr(L"🤖 Supervisor 起動", L"🤖 Launch Supervisor", L"🤖 Iniciar Supervisor"),
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                180, 170, 190, 28, hwnd, (HMENU)IDC_HINT_LAUNCH, hInst, nullptr);
            hs->btnClose = CreateWindowExW(0, L"BUTTON", L"✕",
                WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                378, 170, 68, 28, hwnd, (HMENU)IDC_HINT_CLOSE, hInst, nullptr);

            if (hs && hs->dash) {
                SendMessageW(hs->hwndCategory, WM_SETFONT, (WPARAM)hs->dash->hFontUiBold, TRUE);
                SendMessageW(hs->hwndCountdown, WM_SETFONT, (WPARAM)hs->dash->hFontUi, TRUE);
                SendMessageW(hs->hwndSummary, WM_SETFONT, (WPARAM)hs->dash->hFontUi, TRUE);
                SendMessageW(hs->hwndPromptEdit, WM_SETFONT, (WPARAM)hs->dash->hFontUi, TRUE);
                SendMessageW(hs->btnCopyPrompt, WM_SETFONT, (WPARAM)hs->dash->hFontUi, TRUE);
                SendMessageW(hs->btnLaunchSupervisor, WM_SETFONT, (WPARAM)hs->dash->hFontUi, TRUE);
                SendMessageW(hs->btnClose, WM_SETFONT, (WPARAM)hs->dash->hFontUi, TRUE);
            }
            ApplyNativeThemeToWindow(hwnd);
            return 0;
        }
        case WM_COMMAND: {
            int id = LOWORD(wParam);
            if (id == IDC_HINT_COPY) {
                if (hs && hs->dash) {
                    CopyTextToClipboard(hwnd, hs->dash->currentHintPrompt);
                    SetWindowTextW(hs->btnCopyPrompt, Tr(L"✓ コピー完了!", L"✓ Copied!", L"✓ ¡Copiado!"));
                    hs->copySuccessTicks = 2;
                }
                return 0;
            }
            if (id == IDC_HINT_LAUNCH) {
                if (hs && hs->dash) {
                    LaunchSupervisorTerminal(hs->dash);
                }
                return 0;
            }
            if (id == IDC_HINT_CLOSE) {
                ShowWindow(hwnd, SW_HIDE);
                KillTimer(hwnd, TIMER_HINT_COUNTDOWN_ID);
                return 0;
            }
            break;
        }
        case WM_TIMER: {
            if (wParam == TIMER_HINT_COUNTDOWN_ID && hs && hs->dash) {
                if (hs->copySuccessTicks > 0) {
                    hs->copySuccessTicks--;
                    if (hs->copySuccessTicks == 0) {
                        SetWindowTextW(hs->btnCopyPrompt, Tr(L"📋 プロンプトをコピー", L"📋 Copy Prompt", L"📋 Copiar Prompt"));
                    }
                }
                POINT pt = {};
                GetCursorPos(&pt);
                RECT rc = {};
                GetWindowRect(hwnd, &rc);
                bool hovered = PtInRect(&rc, pt);
                if (hovered) {
                    SetWindowTextW(hs->hwndCountdown, Tr(L"(一時停止中)", L"(Paused)", L"(En pausa)"));
                } else if (hs->dash->hintAutoDismissSec > 0) {
                    hs->dash->hintDismissCountdown--;
                    if (hs->dash->hintDismissCountdown <= 0) {
                        ShowWindow(hwnd, SW_HIDE);
                        KillTimer(hwnd, TIMER_HINT_COUNTDOWN_ID);
                    } else {
                        std::wstring s = Tr(L"自動消去まで ", L"Auto-close in ", L"Cerrar en ") +
                                         std::to_wstring(hs->dash->hintDismissCountdown) + L"s";
                        SetWindowTextW(hs->hwndCountdown, s.c_str());
                    }
                }
                return 0;
            }
            break;
        }
        case WM_ERASEBKGND: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            RECT rc = {};
            GetClientRect(hwnd, &rc);
            HBRUSH bgBrush = CreateSolidBrush(g_theme.panel);
            FillRect(hdc, &rc, bgBrush);
            DeleteObject(bgBrush);
            HBRUSH borderBrush = CreateSolidBrush(g_theme.accent ? g_theme.accent : g_theme.border);
            FrameRect(hdc, &rc, borderBrush);
            DeleteObject(borderBrush);
            return 1;
        }
        case WM_CTLCOLORSTATIC: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkMode(hdc, TRANSPARENT);
            SetTextColor(hdc, g_theme.text);
            return reinterpret_cast<INT_PTR>(g_dialogBgBrush);
        }
        case WM_CTLCOLOREDIT: {
            HDC hdc = reinterpret_cast<HDC>(wParam);
            SetBkColor(hdc, g_theme.windowBg);
            SetTextColor(hdc, g_theme.text);
            static HBRUSH hEditBg = nullptr;
            if (hEditBg) DeleteObject(hEditBg);
            hEditBg = CreateSolidBrush(g_theme.windowBg);
            return reinterpret_cast<INT_PTR>(hEditBg);
        }
        case WM_NCDESTROY: {
            KillTimer(hwnd, TIMER_HINT_COUNTDOWN_ID);
            delete hs;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

static void EnsureHintWindow(DashboardState* st) {
    if (!st || !st->hwndMain) return;
    if (st->hwndHintWnd && IsWindow(st->hwndHintWnd)) return;

    static bool regDone = false;
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    if (!regDone) {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = HintWndProc;
        wc.hInstance = hInst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = g_dialogBgBrush;
        wc.lpszClassName = L"HanchoHintWnd";
        RegisterClassExW(&wc);
        regDone = true;
    }

    auto* hs = new HintWndState();
    hs->dash = st;

    RECT rcMain = {};
    GetWindowRect(st->hwndMain, &rcMain);
    int hintW = 460;
    int hintH = 210;
    int x = rcMain.right - hintW - 20;
    int y = rcMain.bottom - hintH - 45;

    st->hwndHintWnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        L"HanchoHintWnd",
        L"Hancho Supervisor Hint",
        WS_POPUP | WS_CLIPCHILDREN | WS_BORDER,
        x, y, hintW, hintH,
        st->hwndMain,
        nullptr,
        hInst,
        hs
    );
}

static void EvaluateSupervisorHint(DashboardState* st, bool forceShow = false) {
    if (!st || !st->hwndMain) return;
    if (!forceShow && !st->showSupervisorHints) return;

    SupervisorHintInfo info = AnalyzeStateForHint(st);
    if (!forceShow) {
        if (info.signature == st->lastHintSignature) return;
        if (info.signature == "idle") return; // Don't pop up spontaneously when idle
    }
    st->lastHintSignature = info.signature;
    st->currentHintCategory = info.category;
    st->currentHintSummary = info.summary;
    st->currentHintPrompt = info.prompt;

    EnsureHintWindow(st);
    RepositionHintWindow(st);

    auto* hs = reinterpret_cast<HintWndState*>(GetWindowLongPtrW(st->hwndHintWnd, GWLP_USERDATA));
    if (hs) {
        SetWindowTextW(hs->hwndCategory, info.category.c_str());
        SetWindowTextW(hs->hwndSummary, info.summary.c_str());
        SetWindowTextW(hs->hwndPromptEdit, info.prompt.c_str());
        if (st->hintAutoDismissSec > 0) {
            st->hintDismissCountdown = st->hintAutoDismissSec;
            std::wstring cdStr = Tr(L"自動消去まで ", L"Auto-close in ", L"Cerrar en ") +
                                 std::to_wstring(st->hintDismissCountdown) + L"s";
            SetWindowTextW(hs->hwndCountdown, cdStr.c_str());
            SetTimer(st->hwndHintWnd, TIMER_HINT_COUNTDOWN_ID, 1000, nullptr);
        } else {
            SetWindowTextW(hs->hwndCountdown, L"");
            KillTimer(st->hwndHintWnd, TIMER_HINT_COUNTDOWN_ID);
        }
    }

    ShowWindow(st->hwndHintWnd, SW_SHOWNOACTIVATE);
    InvalidateRect(st->hwndHintWnd, nullptr, TRUE);
}

static LRESULT CALLBACK DashboardWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    DashboardState* st = GetState(hwnd);

    // "Hancho.exe quit" from another process: exit the way the tray menu's "Exit" does,
    // so the config is saved and the tray icon is removed on WM_DESTROY.
    static const UINT quitMessage = RegisterWindowMessageW(L"HanchoQuit");
    if (msg == quitMessage) {
        if (st) st->allowExit = true;
        PostMessageW(hwnd, WM_CLOSE, 0, 0);
        return 0;
    }

    // `Hancho.exe gui` (or an IPC gui_show request) from another process while the
    // dashboard is hidden in the tray or minimized: bring it back without touching
    // the running jobs.
    static const UINT showMessage = RegisterWindowMessageW(L"HanchoShowGui");
    if (msg == showMessage) {
        if (st) {
            RestoreFromTray(st);
        } else {
            ShowWindow(hwnd, SW_SHOW);
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
        }
        return 0;
    }

    // Explorer restarted (or was not ready when we added the icon): the notification area has dropped our
    // icon, so register it again. Only needed when the icon is or should be in the tray.
    static const UINT taskbarCreatedMsg = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == taskbarCreatedMsg && st) {
        if (st->trayAdded || !IsWindowVisible(hwnd)) {
            st->trayAdded = false;
            InitTrayIcon(st);
        }
        return 0;
    }

    switch (msg) {
        case WM_CREATE: {
            auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
            st = reinterpret_cast<DashboardState*>(cs->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(st));
            st->hwndMain = hwnd;
            g_mainHwnd = hwnd;

            st->hFontUi = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                      OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            st->hFontUiBold = CreateFontW(-13, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
            st->hFontKpiVal = CreateFontW(-14, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                                          OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

            st->hwndBtnStart = CreateWindowExW(0, L"BUTTON", L"+ Launch (powershell)",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 160, 24, hwnd, (HMENU)IDC_BTN_START, nullptr, nullptr);
            st->hwndBtnSettings = CreateWindowExW(0, L"BUTTON", Tr(L"設定", L"Settings", L"Ajustes"),
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 90, 24, hwnd, (HMENU)IDC_BTN_SETTINGS, nullptr, nullptr);
            st->hwndBtnKillAll = CreateWindowExW(0, L"BUTTON", L"Kill All",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 74, 24, hwnd, (HMENU)IDC_BTN_KILL_ALL, nullptr, nullptr);
            st->hwndBtnGridView = CreateWindowExW(0, L"BUTTON", L"▦",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 28, 24, hwnd, (HMENU)IDC_BTN_GRID_VIEW, nullptr, nullptr);
            st->hwndBtnToggleList = CreateWindowExW(0, L"BUTTON", L"☰",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 28, 24, hwnd, (HMENU)IDC_BTN_TOGGLE_LIST, nullptr, nullptr);
            st->hwndBtnToggleTerm = CreateWindowExW(0, L"BUTTON", L">_",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 30, 24, hwnd, (HMENU)IDC_BTN_TOGGLE_TERM, nullptr, nullptr);
            st->hwndBtnHideTray = CreateWindowExW(0, L"BUTTON", L"⤓",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 28, 24, hwnd, (HMENU)IDC_BTN_HIDE_TRAY, nullptr, nullptr);
            st->hwndBtnViewTasks = CreateWindowExW(0, L"BUTTON", L"Task Queue",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 150, 24, hwnd, (HMENU)IDC_BTN_VIEW_TASKS, nullptr, nullptr);
            st->hwndBtnRefresh = CreateWindowExW(0, L"BUTTON", L"↻",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 28, 24, hwnd, (HMENU)IDC_BTN_REFRESH, nullptr, nullptr);
            st->hwndBtnHint = CreateWindowExW(0, L"BUTTON", L"💡",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 28, 24, hwnd, (HMENU)IDC_BTN_HINT, nullptr, nullptr);
            st->hwndBtnSupervisor = CreateWindowExW(0, L"BUTTON", L"🤖",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 28, 24, hwnd, (HMENU)IDC_BTN_SUPERVISOR, nullptr, nullptr);
            st->hwndClock = CreateWindowExW(0, L"BUTTON", L"",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 80, 24, hwnd, (HMENU)IDC_CLOCK, nullptr, nullptr);
            st->hwndBtnUsage = CreateWindowExW(0, L"BUTTON", L"📊",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 28, 24, hwnd, (HMENU)IDC_BTN_USAGE, nullptr, nullptr);
            st->hwndBtnStreamPane = CreateWindowExW(0, L"BUTTON", L"{}",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 28, 24, hwnd, (HMENU)IDC_BTN_STREAM_PANE, nullptr, nullptr);
            st->hwndBtnBtView = CreateWindowExW(0, L"BUTTON", L"BT",
                WS_CHILD | WS_VISIBLE | BS_OWNERDRAW, 0, 0, 34, 24, hwnd, (HMENU)IDC_BT_DEBUG, nullptr, nullptr);

            static bool usageClassRegistered = false;
            if (!usageClassRegistered) {
                WNDCLASSEXW wcUsage = {};
                wcUsage.cbSize = sizeof(wcUsage);
                wcUsage.style = CS_HREDRAW | CS_VREDRAW;
                wcUsage.lpfnWndProc = UsagePanelWndProc;
                wcUsage.hInstance = GetModuleHandleW(nullptr);
                wcUsage.hCursor = LoadCursorW(nullptr, IDC_ARROW);
                wcUsage.lpszClassName = L"HanchoUsagePanelWnd";
                RegisterClassExW(&wcUsage);
                usageClassRegistered = true;
            }
            st->hwndUsagePanel = CreateWindowExW(0, L"HanchoUsagePanelWnd", L"",
                WS_CHILD, 0, 0, 10, 10, hwnd, nullptr, GetModuleHandleW(nullptr), st);

            st->hwndToolbarTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (st->hwndToolbarTip) {
                SetWindowPos(st->hwndToolbarTip, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                SendMessageW(st->hwndToolbarTip, TTM_SETMAXTIPWIDTH, 0, 320);
                auto addTool = [&](HWND hCtrl) {
                    if (!hCtrl) return;
                    TOOLINFOW ti = {};
                    ti.cbSize = sizeof(ti);
                    ti.hwnd = hwnd;
                    ti.uId = reinterpret_cast<UINT_PTR>(hCtrl);
                    ti.uFlags = TTF_SUBCLASS | TTF_IDISHWND;
                    ti.lpszText = const_cast<wchar_t*>(L"");
                    SendMessageW(st->hwndToolbarTip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
                };
                addTool(st->hwndBtnGridView);
                addTool(st->hwndBtnToggleList);
                addTool(st->hwndBtnToggleTerm);
                addTool(st->hwndBtnStreamPane);
                addTool(st->hwndBtnBtView);
                addTool(st->hwndBtnHideTray);
                addTool(st->hwndBtnRefresh);
                addTool(st->hwndBtnHint);
                addTool(st->hwndBtnSupervisor);
            }

            LoadShellConfig(st);
            UpdateToolbarLabels(st);
            SetWindowTextW(hwnd, MainWindowTitle().c_str());

            st->hwndList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                0, 0, 600, 160, hwnd, (HMENU)IDC_LIST_PROCS, nullptr, nullptr);
            ListView_SetExtendedListViewStyle(st->hwndList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
            ApplyListTheme(st);

            const int colWidths[14] = {52, 78, 100, 100, 120, 140, 85, 65, 72, 78, 70, 185, 175, 80};
            for (int i = 0; i < 14; ++i) {
                LVCOLUMNW lvc = {};
                lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                lvc.pszText = const_cast<wchar_t*>(ColumnTitle(i));
                lvc.cx = colWidths[i];
                lvc.iSubItem = i;
                ListView_InsertColumn(st->hwndList, i, &lvc);
            }
            UpdateListHeaderSortIndicator(st);

            HWND hHeader = ListView_GetHeader(st->hwndList);
            if (hHeader) {
                for (int i = 0; i < 14; ++i) {
                    HDITEMW hdi = {};
                    hdi.mask = HDI_FORMAT;
                    if (Header_GetItem(hHeader, i, &hdi)) {
                        hdi.fmt |= HDF_OWNERDRAW;
                        Header_SetItem(hHeader, i, &hdi);
                    }
                }
                SetWindowSubclass(hHeader, HeaderSubclassProc, 1, 0);
            }
            SetWindowSubclass(st->hwndList, ListViewSubclassProc, 1, 0);

            st->hwndTaskList = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                WS_CHILD | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                0, 0, 600, 160, hwnd, (HMENU)IDC_LIST_TASKS, nullptr, nullptr);
            ListView_SetExtendedListViewStyle(st->hwndTaskList, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_INFOTIP);
            ListView_SetBkColor(st->hwndTaskList, g_theme.panel);
            ListView_SetTextBkColor(st->hwndTaskList, g_theme.panel);
            ListView_SetTextColor(st->hwndTaskList, g_theme.listText);
            {
                const int taskWidths[kTaskListColumns] = {90, 350, 100, 105, 115, 190, 110, 110, 150, 200};
                for (int i = 0; i < kTaskListColumns; ++i) {
                    LVCOLUMNW lvc = {};
                    lvc.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
                    lvc.pszText = const_cast<wchar_t*>(TaskColumnTitle(i));
                    lvc.cx = taskWidths[i];
                    lvc.iSubItem = i;
                    ListView_InsertColumn(st->hwndTaskList, i, &lvc);
                }
            }
            HWND hTaskHeader = ListView_GetHeader(st->hwndTaskList);
            if (hTaskHeader) {
                for (int i = 0; i < kTaskListColumns; ++i) {
                    HDITEMW hdi = {};
                    hdi.mask = HDI_FORMAT;
                    if (Header_GetItem(hTaskHeader, i, &hdi)) {
                        hdi.fmt |= HDF_OWNERDRAW;
                        Header_SetItem(hTaskHeader, i, &hdi);
                    }
                }
                SetWindowSubclass(hTaskHeader, HeaderSubclassProc, 1, 0);
            }
            SetWindowSubclass(st->hwndTaskList, ListViewSubclassProc, 1, 0);

            st->hwndTab = CreateWindowExW(0, WC_TABCONTROLW, L"",
                WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | TCS_TABS | TCS_SINGLELINE | TCS_OWNERDRAWFIXED,
                0, 0, 600, 26, hwnd, (HMENU)IDC_TAB_TERMINALS, nullptr, nullptr);
            g_tabHwnd = st->hwndTab;
            SetWindowSubclass(st->hwndTab, TabSubclassProc, 1, 0);

            HWND uiCtrls[] = {
                st->hwndBtnStart, st->hwndBtnSettings, st->hwndBtnKillAll,
                st->hwndBtnGridView,
                st->hwndBtnToggleList, st->hwndBtnToggleTerm, st->hwndBtnStreamPane, st->hwndBtnBtView, st->hwndBtnHideTray, st->hwndBtnRefresh,
                st->hwndBtnHint, st->hwndBtnSupervisor, st->hwndClock,
                st->hwndBtnViewTasks, st->hwndList, st->hwndTaskList, st->hwndTab
            };
            for (HWND h : uiCtrls) {
                if (h) SendMessageW(h, WM_SETFONT, (WPARAM)st->hFontUi, TRUE);
            }
            ApplyListFonts(st);

            st->gridView = std::make_unique<TerminalGridView>();
            st->gridView->Create(hwnd);

            st->hwndKpiTip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                hwnd, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (st->hwndKpiTip) {
                SetWindowPos(st->hwndKpiTip, HWND_TOPMOST, 0, 0, 0, 0,
                             SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
                SendMessageW(st->hwndKpiTip, TTM_SETMAXTIPWIDTH, 0, 420);
                for (int i = 0; i < kNumKpiCards; ++i) {
                    TOOLINFOW ti = {};
                    ti.cbSize = sizeof(ti);
                    ti.uFlags = TTF_SUBCLASS;
                    ti.hwnd = hwnd;
                    ti.uId = static_cast<UINT_PTR>(i + 1);
                    ti.rect = {0, 0, 0, 0};
                    ti.lpszText = const_cast<wchar_t*>(L"");
                    SendMessageW(st->hwndKpiTip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
                }
            }

            InitTrayIcon(st);
            UpdateClock(st);
            ApplyNativeThemeToWindow(hwnd);

            SetTimer(hwnd, TIMER_KPI_ANIM_ID, 60, nullptr);
            RefreshDashboardData(st);
            ApplyDashboardRefreshTimer(st);
            return 0;
        }

        case WM_PC_CREATE_TERMVIEW: {
            auto* params = reinterpret_cast<CreateTermViewParams*>(lParam);
            if (!params || !st) return 0;

            auto* tv = new TerminalView();
            HWND hTv = tv->Create(hwnd);
            if (!hTv) {
                params->outError = L"TerminalView::Create failed";
                delete tv;
                return 0;
            }
            int paneW = (std::max)(200L, st->rcTerminalArea.right - st->rcTerminalArea.left);
            int paneH = (std::max)(120L, st->rcTerminalArea.bottom - st->rcTerminalArea.top - 28);
            tv->MoveAndResize(st->rcTerminalArea.left, st->rcTerminalArea.top + 26, paneW, paneH);
            tv->SetWorkingDirectory(params->cwd);
            tv->SetOutputObserver(params->onOutput);

            if (!tv->StartSession(params->resolvedCmd, CP_UTF8)) {
                params->outError = tv->LastError();
                DestroyWindow(hTv);
                delete tv;
                return 0;
            }
            params->outView = tv;
            return 1;
        }

        case WM_PC_DESTROY_TERMVIEW: {
            auto* tv = reinterpret_cast<TerminalView*>(wParam);
            if (tv) {
                if (tv->Hwnd() && IsWindow(tv->Hwnd())) {
                    DestroyWindow(tv->Hwnd());
                }
                delete tv;
            }
            return 0;
        }

        case WM_DEFERRED_FOCUS: {
            int procId = static_cast<int>(lParam);
            if (st && procId > 0) {
                st->selectedProcId = procId;
                RefreshDashboardData(st);
            }
            HWND hTarget = reinterpret_cast<HWND>(wParam);
            if (hTarget && IsWindow(hTarget)) {
                SetFocus(hTarget);
            }
            return 0;
        }

        case WM_GETMINMAXINFO: {
            auto* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
            mmi->ptMinTrackSize.x = 1000;
            mmi->ptMinTrackSize.y = 220;
            return 0;
        }

        case WM_MOVE: {
            if (st && st->hwndHintWnd && IsWindowVisible(st->hwndHintWnd)) {
                RepositionHintWindow(st);
            }
            break;
        }

        case WM_SIZE: {
            if (st) {
                LayoutControls(st, LOWORD(lParam), HIWORD(lParam));
                if (st->hwndHintWnd && IsWindowVisible(st->hwndHintWnd)) {
                    RepositionHintWindow(st);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        case WM_TIMER: {
            if (wParam == TIMER_REFRESH_ID && st) {
                UpdateClock(st);
                RefreshDashboardData(st);
                ApplyDashboardRefreshTimer(st);
            } else if (wParam == TIMER_END_BLINK_ID && st) {
                if (!TickEndBlink(st)) KillTimer(hwnd, TIMER_END_BLINK_ID);
            } else if (wParam == TIMER_KPI_ANIM_ID && st) {
                if (st->activeRunningCount > 0) {
                    g_kpiBarPhase += 0.05f;
                    if (g_kpiBarPhase >= 1.0f) g_kpiBarPhase -= 1.0f;
                    InvalidateRect(hwnd, &st->kpiCardRects[1], FALSE);
                    InvalidateRect(hwnd, &st->kpiCardRects[8], FALSE);
                }
            }
            return 0;
        }

        case WM_PC_IPC_EVENT: {
            if (st) {
                int newId = static_cast<int>(wParam);
                if (newId > 0) {
                    st->selectedProcId = newId;
                }
                RefreshDashboardData(st);
            }
            return 0;
        }

        case WM_SETCURSOR: {
            if (st && LOWORD(lParam) == HTCLIENT) {
                POINT pt = {};
                GetCursorPos(&pt);
                ScreenToClient(hwnd, &pt);
                if ((st->rcRowsSplitter.bottom > st->rcRowsSplitter.top && PtInRect(&st->rcRowsSplitter, pt)) ||
                    (st->rcStreamSplitter.bottom > st->rcStreamSplitter.top && PtInRect(&st->rcStreamSplitter, pt)) ||
                    (st->rcSplitter.bottom > st->rcSplitter.top && PtInRect(&st->rcSplitter, pt)) ||
                    (st->showUsagePanel && st->rcUsageSplitter.bottom > st->rcUsageSplitter.top && PtInRect(&st->rcUsageSplitter, pt))) {
                    SetCursor(LoadCursorW(nullptr, IDC_SIZENS));
                    return TRUE;
                }
            }
            break;
        }

        case WM_LBUTTONDOWN: {
            if (st) {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (st->rcRowsSplitter.bottom > st->rcRowsSplitter.top && PtInRect(&st->rcRowsSplitter, pt)) {
                    st->rowsSplitterDragging = true;
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, &st->rcRowsSplitter, FALSE);
                    return 0;
                }
                if (st->rcStreamSplitter.bottom > st->rcStreamSplitter.top && PtInRect(&st->rcStreamSplitter, pt)) {
                    st->streamSplitterDragging = true;
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, &st->rcStreamSplitter, FALSE);
                    return 0;
                }
                if (st->rcSplitter.bottom > st->rcSplitter.top && PtInRect(&st->rcSplitter, pt)) {
                    st->splitterDragging = true;
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, &st->rcSplitter, FALSE);
                    return 0;
                }
                if (st->showUsagePanel && st->rcUsageSplitter.bottom > st->rcUsageSplitter.top && PtInRect(&st->rcUsageSplitter, pt)) {
                    st->usageSplitterDragging = true;
                    SetCapture(hwnd);
                    InvalidateRect(hwnd, &st->rcUsageSplitter, FALSE);
                    return 0;
                }
                for (const auto& tb : st->termBanners) {
                    if (PtInRect(&tb.rcBorder, pt)) {
                        st->selectedProcId = tb.procId;
                        RefreshDashboardData(st);
                        TerminalView* tv = st->pm ? st->pm->GetTerminalView(tb.procId) : nullptr;
                        if (tv && tv->Hwnd()) {
                            SetFocus(tv->Hwnd());
                        }
                        break;
                    }
                }
            }
            return 0;
        }

        case WM_MOUSEMOVE: {
            if (st && st->rowsSplitterDragging) {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (st->lastTotalListHeight >= kMinRowListH * 2) {
                    int relY = pt.y - st->processListTop;
                    float ratio = static_cast<float>(relY) / static_cast<float>(st->lastTotalListHeight);
                    ratio = (std::clamp)(ratio, static_cast<float>(kMinRowListH) / static_cast<float>(st->lastTotalListHeight),
                                         1.0f - static_cast<float>(kMinRowListH) / static_cast<float>(st->lastTotalListHeight));
                    st->procRowRatio = ratio;
                    RECT rcClient = {};
                    GetClientRect(hwnd, &rcClient);
                    LayoutControls(st, rcClient.right, rcClient.bottom);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (st && st->streamSplitterDragging) {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                if (st->streamPaneBottom > 0) {
                    int newH = st->streamPaneBottom - pt.y - kRowSplitterH / 2;
                    if (newH < kMinStreamPaneH) newH = kMinStreamPaneH;
                    st->streamPaneHeight = newH;
                    RECT rcClient = {};
                    GetClientRect(hwnd, &rcClient);
                    LayoutControls(st, rcClient.right, rcClient.bottom);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
            if (st && st->splitterDragging) {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                st->processListHeight = pt.y - st->processListTop;
                if (st->processListHeight < kMinListH) st->processListHeight = kMinListH;
                RECT rcClient = {};
                GetClientRect(hwnd, &rcClient);
                LayoutControls(st, rcClient.right, rcClient.bottom);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            if (st && st->usageSplitterDragging) {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                int usageTop = kHeaderKpiBottom + 2;
                int newH = pt.y - usageTop;
                if (newH < 60) newH = 60;
                if (newH > 400) newH = 400;
                st->usagePanelHeight = newH;
                RECT rcClient = {};
                GetClientRect(hwnd, &rcClient);
                LayoutControls(st, rcClient.right, rcClient.bottom);
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }
            break;
        }

        case WM_LBUTTONUP: {
            if (st && st->rowsSplitterDragging) {
                st->rowsSplitterDragging = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                SaveShellConfig(st);
                InvalidateRect(hwnd, &st->rcRowsSplitter, FALSE);
                return 0;
            }
            if (st && st->streamSplitterDragging) {
                st->streamSplitterDragging = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                SaveShellConfig(st);
                InvalidateRect(hwnd, &st->rcStreamSplitter, FALSE);
                return 0;
            }
            if (st && st->splitterDragging) {
                st->splitterDragging = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                SaveShellConfig(st);
                InvalidateRect(hwnd, &st->rcSplitter, FALSE);
                return 0;
            }
            if (st && st->usageSplitterDragging) {
                st->usageSplitterDragging = false;
                if (GetCapture() == hwnd) ReleaseCapture();
                InvalidateRect(hwnd, &st->rcUsageSplitter, FALSE);
                return 0;
            }
            break;
        }

        case WM_CAPTURECHANGED: {
            if (st) {
                st->splitterDragging = false;
                st->rowsSplitterDragging = false;
                st->usageSplitterDragging = false;
                st->streamSplitterDragging = false;
            }
            break;
        }

        case WM_RBUTTONDOWN: {
            if (st) {
                POINT pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
                for (const auto& tb : st->termBanners) {
                    if (PtInRect(&tb.rcBorder, pt)) {
                        st->selectedProcId = tb.procId;
                        RefreshDashboardData(st);
                        POINT ptScreen = pt;
                        ClientToScreen(hwnd, &ptScreen);
                        ShowTaskContextMenu(st, tb.procId, ptScreen);
                        break;
                    }
                }
            }
            return 0;
        }

        case WM_DRAWITEM: {
            auto* dis = reinterpret_cast<DRAWITEMSTRUCT*>(lParam);
            if (!dis) break;
            if (dis->CtlType == ODT_HEADER) {
                DrawHeaderItem(dis);
                return TRUE;
            }
            if (dis->CtlType == ODT_TAB) {
                wchar_t text[256] = {};
                TCITEMW tie = {};
                tie.mask = TCIF_TEXT;
                tie.pszText = text;
                tie.cchTextMax = 256;
                TabCtrl_GetItem(dis->hwndItem, dis->itemID, &tie);
                bool selected = (dis->itemState & ODS_SELECTED) != 0 ||
                                TabCtrl_GetCurSel(dis->hwndItem) == static_cast<int>(dis->itemID);
                COLORREF bg = selected ? g_theme.panel : g_theme.windowBg;
                HBRUSH bgBrush = CreateSolidBrush(bg);
                FillRect(dis->hDC, &dis->rcItem, bgBrush);
                DeleteObject(bgBrush);
                HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
                HGDIOBJ oldPen = SelectObject(dis->hDC, borderPen);
                HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
                Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
                SelectObject(dis->hDC, oldBrush);
                SelectObject(dis->hDC, oldPen);
                DeleteObject(borderPen);
                if (selected) {
                    RECT bar = dis->rcItem;
                    bar.top = bar.bottom - 2;
                    HBRUSH accentBrush = CreateSolidBrush(g_theme.accent);
                    FillRect(dis->hDC, &bar, accentBrush);
                    DeleteObject(accentBrush);
                }
                SetBkMode(dis->hDC, TRANSPARENT);
                SetTextColor(dis->hDC, selected ? g_theme.accent : g_theme.text);
                HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC,
                    st ? st->hFontUi : GetStockObject(DEFAULT_GUI_FONT)));
                RECT rcText = dis->rcItem;
                InflateRect(&rcText, -6, 0);
                DrawTextW(dis->hDC, text, -1, &rcText,
                          DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                SelectObject(dis->hDC, oldFont);
                return TRUE;
            }
            if (dis->CtlType != ODT_BUTTON) break;
            // Toggle buttons stay pressed while the pane or list they control is shown.
            bool latched = false;
            if (st) {
                if (dis->hwndItem == st->hwndBtnStreamPane) latched = st->showStreamPane;
                else if (dis->hwndItem == st->hwndBtnToggleList) latched = !st->hideProcessList;
                else if (dis->hwndItem == st->hwndBtnToggleTerm) latched = !st->hideTerminalGrid;
            }
            bool pressed = (dis->itemState & ODS_SELECTED) != 0 || latched;
            COLORREF bg = pressed ? g_theme.border : g_theme.panel;
            HBRUSH bgBrush = CreateSolidBrush(bg);
            FillRect(dis->hDC, &dis->rcItem, bgBrush);
            DeleteObject(bgBrush);
            HPEN borderPen = CreatePen(PS_SOLID, 1, g_theme.border);
            HGDIOBJ oldPen = SelectObject(dis->hDC, borderPen);
            HGDIOBJ oldBrush = SelectObject(dis->hDC, GetStockObject(NULL_BRUSH));
            Rectangle(dis->hDC, dis->rcItem.left, dis->rcItem.top, dis->rcItem.right, dis->rcItem.bottom);
            SelectObject(dis->hDC, oldBrush);
            SelectObject(dis->hDC, oldPen);
            DeleteObject(borderPen);

            if (dis->itemState & ODS_FOCUS) {
                RECT rcFocus = dis->rcItem;
                InflateRect(&rcFocus, -2, -2);
                DrawFocusRect(dis->hDC, &rcFocus);
            }

            wchar_t text[256] = {};
            GetWindowTextW(dis->hwndItem, text, 256);
            SetBkMode(dis->hDC, TRANSPARENT);
            SetTextColor(dis->hDC, (dis->itemState & ODS_DISABLED) ? g_theme.muted : g_theme.text);
            HFONT oldFont = static_cast<HFONT>(SelectObject(dis->hDC, st ? st->hFontUi : GetStockObject(DEFAULT_GUI_FONT)));
            RECT rc = dis->rcItem;
            InflateRect(&rc, -4, 0);
            DrawTextW(dis->hDC, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(dis->hDC, oldFont);
            return TRUE;
        }

        case WM_COMMAND: {
            if (!st) return 0;
            int id = LOWORD(wParam);
            if (id == IDC_BTN_START) {
                LaunchFromToolbar(st);
            } else if (id == IDC_BTN_SETTINGS) {
                ShowSettingsDialog(st);
            } else if (id == IDC_BTN_KILL_SEL) {
                if (st->selectedProcId > 0) {
                    if (gui_platform::GetGuiPlatform()->ShowMessageBox(
                            hwnd,
                            Tr(L"終了の確認", L"Confirm Kill", L"Confirmar cierre"),
                            Tr(L"このプロセスツリーを強制終了しますか？", L"Are you sure you want to kill this process tree?", L"¿Seguro que desea terminar este árbol de procesos?"),
                            gui_platform::MessageBoxType::QuestionYesNo,
                            true) == gui_platform::MessageBoxResult::Yes) {
                        std::string err;
                        KillProcessIpc(st, st->selectedProcId, err);
                        RefreshDashboardData(st);
                    }
                }
            } else if (id == IDC_BTN_KILL_ALL) {
                if (gui_platform::GetGuiPlatform()->ShowMessageBox(
                        hwnd,
                        Tr(L"全終了の確認", L"Confirm Kill All", L"Confirmar cierre total"),
                        Tr(L"実行中のすべてのプロセスを強制終了しますか？", L"Are you sure you want to kill ALL running processes?", L"¿Seguro que desea terminar TODOS los procesos en ejecución?"),
                        gui_platform::MessageBoxType::QuestionYesNo,
                        true) == gui_platform::MessageBoxResult::Yes) {
                    KillAllRunningIpc(st);
                    RefreshDashboardData(st);
                }
            } else if (id == IDC_BTN_CLEAR) {
                if (gui_platform::GetGuiPlatform()->ShowMessageBox(
                        hwnd,
                        Tr(L"履歴消去の確認", L"Confirm Clear", L"Confirmar borrado"),
                        Tr(L"終了済みプロセスの履歴を消去しますか？", L"Are you sure you want to clear finished process history?", L"¿Seguro que desea borrar el historial de procesos finalizados?"),
                        gui_platform::MessageBoxType::QuestionYesNo,
                        true) == gui_platform::MessageBoxResult::Yes) {
                    ClearFinishedIpc(st);
                    RefreshDashboardData(st);
                }
            } else if (id == IDC_BT_DEBUG) {
                ShowBtDebugDialog(st);
            } else if (id == IDC_BTN_VIEW_TASKS) {
                if (st->listDisplayMode == ListDisplayMode::TwoRows) {
                    st->listDisplayMode = ListDisplayMode::ProcessOnly;
                } else if (st->listDisplayMode == ListDisplayMode::ProcessOnly) {
                    st->listDisplayMode = ListDisplayMode::TaskOnly;
                } else {
                    st->listDisplayMode = ListDisplayMode::TwoRows;
                }
                if (st->hideProcessList) {
                    st->hideProcessList = false;
                    UpdatePanelToggleLabels(st);
                }
                UpdateToolbarLabels(st);
                SaveShellConfig(st);
                RECT rcClient = {};
                GetClientRect(hwnd, &rcClient);
                LayoutControls(st, rcClient.right, rcClient.bottom);
                RefreshDashboardData(st);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (id == IDC_BTN_GRID_VIEW) {
                if (st->gridView) {
                    LayoutTerminalViews(st);
                    if (st->gridView->IsVisible()) st->gridView->Hide();
                    else st->gridView->Show();
                }
            } else if (id == IDC_BTN_TOGGLE_LIST) {
                st->hideProcessList = !st->hideProcessList;
                UpdatePanelToggleLabels(st);
                SaveShellConfig(st);
                RECT rcClient = {};
                GetClientRect(hwnd, &rcClient);
                LayoutControls(st, rcClient.right, rcClient.bottom);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (id == IDC_BTN_TOGGLE_TERM) {
                st->hideTerminalGrid = !st->hideTerminalGrid;
                UpdatePanelToggleLabels(st);
                SaveShellConfig(st);
                RECT rcClient = {};
                GetClientRect(hwnd, &rcClient);
                LayoutControls(st, rcClient.right, rcClient.bottom);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (id == IDC_BTN_REFRESH) {
                RefreshDashboardData(st);
                if (st->streamPane && st->hwndStreamPane && IsWindowVisible(st->hwndStreamPane)) {
                    ReloadStreamDialogData(st->hwndStreamPane, st->streamPane);
                }
            } else if (id == IDC_BTN_STREAM_PANE) {
                st->showStreamPane = !st->showStreamPane;
                if (st->showStreamPane && st->hideProcessList) st->hideProcessList = false;
                UpdatePanelToggleLabels(st);
                SaveShellConfig(st);
                RECT rcClient = {};
                GetClientRect(hwnd, &rcClient);
                LayoutControls(st, rcClient.right, rcClient.bottom);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (id == IDC_BTN_USAGE) {
                st->showUsagePanel = !st->showUsagePanel;
                RECT rcClient = {};
                GetClientRect(hwnd, &rcClient);
                LayoutControls(st, rcClient.right, rcClient.bottom);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (id == IDC_BTN_HIDE_TRAY) {
                HideToTray(st);
            } else if (id == IDC_BTN_HINT) {
                if (st->hwndHintWnd && IsWindowVisible(st->hwndHintWnd)) {
                    ShowWindow(st->hwndHintWnd, SW_HIDE);
                    KillTimer(st->hwndHintWnd, TIMER_HINT_COUNTDOWN_ID);
                } else {
                    EvaluateSupervisorHint(st, true);
                }
            } else if (id == IDC_BTN_SUPERVISOR) {
                LaunchSupervisorTerminal(st);
            }
            return 0;
        }

        case WM_NOTIFY: {
            if (!st) return 0;
            auto* nmhdr = reinterpret_cast<NMHDR*>(lParam);
            if (nmhdr->idFrom == IDC_TAB_TERMINALS) {
                if (nmhdr->code == TCN_SELCHANGE && !g_suppressTabChange) {
                    int sel = TabCtrl_GetCurSel(st->hwndTab);
                    if (sel >= 0 && sel < static_cast<int>(g_appTabs.size())) {
                        int procId = static_cast<int>(reinterpret_cast<intptr_t>(g_appTabs[sel].data));
                        HWND hTv = g_appTabs[sel].hwnd;
                        if (procId > 0) {
                            st->selectedProcId = procId;
                            RefreshDashboardData(st);
                            if (hTv && IsWindow(hTv)) {
                                SetFocus(hTv);
                            }
                        }
                    }
                } else if (nmhdr->code == NM_RCLICK) {
                    POINT ptScreen = {};
                    GetCursorPos(&ptScreen);
                    TCHITTESTINFO hti = {};
                    hti.pt = ptScreen;
                    ScreenToClient(st->hwndTab, &hti.pt);
                    int idx = TabCtrl_HitTest(st->hwndTab, &hti);
                    if (idx >= 0 && idx < static_cast<int>(g_appTabs.size())) {
                        int procId = static_cast<int>(reinterpret_cast<intptr_t>(g_appTabs[idx].data));
                        if (procId > 0) {
                            st->selectedProcId = procId;
                            RefreshDashboardData(st);
                            ShowTaskContextMenu(st, procId, ptScreen);
                        }
                    }
                }
            } else if (nmhdr->idFrom == IDC_LIST_TASKS) {
                if (nmhdr->code == LVN_COLUMNCLICK) {
                    // Clicking a header sorts by that column; the same column cycles
                    // ascending -> descending -> unsorted.
                    auto* nmlv = reinterpret_cast<NMLISTVIEW*>(lParam);
                    int clickedCol = nmlv->iSubItem;
                    if (clickedCol >= 0 && clickedCol < kTaskListColumns) {
                        if (st->taskSortColumn == clickedCol) {
                            st->taskSortDirection = NextSortDirection(st->taskSortDirection);
                        } else {
                            st->taskSortColumn = clickedCol;
                            st->taskSortDirection = SortDirection::Ascending;
                        }
                        UpdateTaskListHeaders(st);
                        RefreshTaskQueueData(st, true);
                    }
                } else if (nmhdr->code == LVN_ITEMCHANGED) {
                    // Selecting a task shows its process in the monitor pane below.
                    auto* nmlv = reinterpret_cast<NMLISTVIEW*>(lParam);
                    bool newlySelected = (nmlv->uNewState & LVIS_SELECTED) && !(nmlv->uOldState & LVIS_SELECTED);
                    int idx = nmlv->iItem;
                    if (newlySelected && idx >= 0 && idx < static_cast<int>(st->taskJobIds.size())) {
                        std::string jobId = st->taskJobIds[idx];
                        st->selectedJobKey = jobId;
                        int pid = (idx < static_cast<int>(st->taskJobProcIds.size())) ? st->taskJobProcIds[idx] : 0;
                        st->selectedProcId = pid;
                        st->lastFollowedProcId = pid;
                        SyncStreamPane(st);
                    }
                } else if (nmhdr->code == NM_RCLICK) {
                    auto* nmia = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                    int idx = nmia->iItem;
                    if (idx >= 0 && idx < static_cast<int>(st->taskJobIds.size())) {
                        POINT ptScreen = {};
                        GetCursorPos(&ptScreen);
                        std::wstring cell = TaskListCellText(st->hwndTaskList, idx, nmia->iSubItem);
                        ShowTaskQueueContextMenu(st, st->taskJobIds[idx], ptScreen, cell);
                    }
                } else if (nmhdr->code == NM_DBLCLK) {
                    auto* nmia = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                    int idx = nmia->iItem;
                    if (idx >= 0 && idx < static_cast<int>(st->taskJobIds.size())) {
                        ShowTaskQueueJobDetails(st, st->taskJobIds[idx]);
                    }
                } else if (nmhdr->code == LVN_GETINFOTIPW) {
                    // Hover over the Resources cell: show all of it, not only what fits the column.
                    // Hover over the Origin cell: show who added the job (session, folder, client).
                    auto* tip = reinterpret_cast<NMLVGETINFOTIPW*>(lParam);
                    if (tip->iSubItem == kTaskListResourcesCol && tip->pszText && tip->cchTextMax > 0) {
                        std::wstring full = TaskListCellText(st->hwndTaskList, tip->iItem, tip->iSubItem);
                        wcsncpy_s(tip->pszText, tip->cchTextMax, full.c_str(), _TRUNCATE);
                    } else if (tip->iSubItem == kTaskListOriginCol && tip->pszText && tip->cchTextMax > 0 &&
                               tip->iItem >= 0 && tip->iItem < static_cast<int>(st->taskOriginTips.size())) {
                        wcsncpy_s(tip->pszText, tip->cchTextMax, st->taskOriginTips[tip->iItem].c_str(), _TRUNCATE);
                    }
                } else if (nmhdr->code == NM_CUSTOMDRAW) {
                    auto* lvcd = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
                    if (lvcd->nmcd.dwDrawStage == CDDS_PREPAINT) {
                        return CDRF_NOTIFYITEMDRAW;
                    }
                    if (lvcd->nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT) {
                        DrawListRowSeparator(lvcd);
                        return CDRF_DODEFAULT;
                    }
                    if (lvcd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                        int idx = static_cast<int>(lvcd->nmcd.dwItemSpec);
                        std::string kind;
                        if (idx >= 0 && idx < static_cast<int>(st->taskJobStates.size())) {
                            kind = st->taskJobStates[idx];
                        }
                        lvcd->clrText = g_theme.listText;
                        ListRowKey taskKey = ListRowKey::Running;
                        COLORREF custom = 0;
                        bool hasKey = true;
                        if (kind == "running") taskKey = ListRowKey::Running;
                        else if (kind == "timeout") taskKey = ListRowKey::Timeout;
                        else if (kind == "failed") taskKey = ListRowKey::Failed;
                        else hasKey = false;
                        if (hasKey && GetListRowBgOverride(taskKey, custom)) {
                            lvcd->clrTextBk = custom;
                            lvcd->clrText = ListRowTextFor(custom);
                        } else if (kind == "running") {
                            lvcd->clrTextBk = MutedRowColor(g_theme.rowSelected);
                        } else if (kind == "timeout") {
                            lvcd->clrTextBk = MutedRowColor(g_theme.rowIdle);
                        } else if (kind == "failed") {
                            lvcd->clrTextBk = MutedRowColor(g_theme.rowFailed);
                        } else {
                            lvcd->clrTextBk = g_theme.panel;
                        }
                        if (idx >= 0 && idx < static_cast<int>(st->taskJobIds.size()) &&
                            IsEndBlinkOn(st, st->taskEndBlinkUntil, st->taskJobIds[idx])) {
                            const COLORREF blink = BlendColor(lvcd->clrTextBk, g_theme.accent, 0.6f);
                            lvcd->clrTextBk = blink;
                            lvcd->clrText = ListRowTextFor(blink);
                        }
                        return CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
                    }
                }
            } else if (nmhdr->idFrom == IDC_LIST_PROCS) {
                if (nmhdr->code == LVN_COLUMNCLICK) {
                    // Clicking a header sorts by that column; the same column cycles
                    // ascending -> descending -> unsorted.
                    auto* nmlv = reinterpret_cast<NMLISTVIEW*>(lParam);
                    int clickedCol = nmlv->iSubItem;
                    if (clickedCol >= 0 && clickedCol < 14) {
                        if (st->sortColumn == clickedCol) {
                            st->sortDirection = NextSortDirection(st->sortDirection);
                        } else {
                            st->sortColumn = clickedCol;
                            st->sortDirection = SortDirection::Ascending;
                        }
                        UpdateListHeaderSortIndicator(st);
                        RefreshDashboardData(st);
                    }
                } else if (nmhdr->code == LVN_ITEMCHANGED) {
                    auto* nmlv = reinterpret_cast<NMLISTVIEW*>(lParam);
                    if ((nmlv->uChanged & LVIF_STATE) && (nmlv->uNewState & LVIS_SELECTED)) {
                        int procId = static_cast<int>(nmlv->lParam);
                        if (procId > 0 && procId != st->selectedProcId) {
                            st->selectedProcId = procId;
                            st->selectedJobKey.clear();
                            RefreshDashboardData(st);
                            TerminalView* tv = st->pm ? st->pm->GetTerminalView(procId) : nullptr;
                            if (tv && tv->Hwnd()) {
                                SetFocus(tv->Hwnd());
                            }
                        }
                    }
                } else if (nmhdr->code == NM_RCLICK) {
                    auto* nmia = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                    int procId = st->selectedProcId;
                    if (nmia->iItem >= 0 && nmia->iItem < static_cast<int>(st->cachedSnapshots.size())) {
                        procId = st->cachedSnapshots[nmia->iItem].id;
                        st->selectedProcId = procId;
                        RefreshDashboardData(st);
                    }
                    if (procId > 0) {
                        POINT ptScreen = {};
                        GetCursorPos(&ptScreen);
                        ShowTaskContextMenu(st, procId, ptScreen);
                    }
                } else if (nmhdr->code == NM_DBLCLK) {
                    auto* nmia = reinterpret_cast<NMITEMACTIVATE*>(lParam);
                    int procId = st->selectedProcId;
                    if (nmia->iItem >= 0 && nmia->iItem < static_cast<int>(st->cachedSnapshots.size())) {
                        procId = st->cachedSnapshots[nmia->iItem].id;
                        st->selectedProcId = procId;
                    }
                    if (procId > 0) {
                        ShowTaskDetailsDialog(st, procId);
                    }
                } else if (nmhdr->code == NM_CUSTOMDRAW) {
                    auto* lvcd = reinterpret_cast<NMLVCUSTOMDRAW*>(lParam);
                    if (lvcd->nmcd.dwDrawStage == CDDS_PREPAINT) {
                        return CDRF_NOTIFYITEMDRAW;
                    }
                    if (lvcd->nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT) {
                        DrawListRowSeparator(lvcd);
                        return CDRF_DODEFAULT;
                    }
                    if (lvcd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
                        int itemIdx = static_cast<int>(lvcd->nmcd.dwItemSpec);
                        if (itemIdx >= 0 && itemIdx < static_cast<int>(st->cachedSnapshots.size())) {
                            const auto& snap = st->cachedSnapshots[itemIdx];
                            bool selected = (lvcd->nmcd.uItemState & CDIS_SELECTED) != 0;
                            COLORREF custom = 0;
                            const bool hasCustom = !selected && ProcessRowCustomBg(snap, custom);
                            lvcd->clrTextBk = selected ? g_theme.rowSelected : GetStateRowBgColor(snap);
                            lvcd->clrText = hasCustom ? ListRowTextFor(custom) : g_theme.listText;
                            if (!selected && IsEndBlinkOn(st, st->procEndBlinkUntil, snap.id)) {
                                const COLORREF blink = BlendColor(GetStateRowBgColor(snap), g_theme.accent, 0.6f);
                                lvcd->clrTextBk = blink;
                                lvcd->clrText = ListRowTextFor(blink);
                            }
                        }
                        return CDRF_NOTIFYSUBITEMDRAW | CDRF_NEWFONT | CDRF_NOTIFYPOSTPAINT;
                    }
                    if (lvcd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
                        int itemIdx = static_cast<int>(lvcd->nmcd.dwItemSpec);
                        if (itemIdx >= 0 && itemIdx < static_cast<int>(st->cachedSnapshots.size())) {
                            const auto& snap = st->cachedSnapshots[itemIdx];
                            COLORREF custom = 0;
                            const bool selected = (lvcd->nmcd.uItemState & CDIS_SELECTED) != 0;
                            if (!selected && ProcessRowCustomBg(snap, custom)) {
                                lvcd->clrText = ListRowTextFor(custom);
                            } else {
                                lvcd->clrText = (lvcd->iSubItem == 5 && snap.state == ProcState::Running)
                                                    ? g_theme.muted : g_theme.listText;
                            }
                            if (!selected && IsEndBlinkOn(st, st->procEndBlinkUntil, snap.id)) {
                                lvcd->clrText = ListRowTextFor(BlendColor(GetStateRowBgColor(snap), g_theme.accent, 0.6f));
                            }
                        }
                        return CDRF_NEWFONT;
                    }
                }
            }
            return 0;
        }

        case WM_PC_TRAYICON: {
            if (!st) return 0;
            UINT ev = LOWORD(lParam);
            if (ev == WM_LBUTTONUP || ev == WM_LBUTTONDBLCLK) {
                if (IsWindowVisible(hwnd) && !IsIconic(hwnd)) {
                    SetForegroundWindow(hwnd);
                } else {
                    RestoreFromTray(st);
                }
            } else if (ev == WM_RBUTTONUP || ev == WM_CONTEXTMENU) {
                ShowTrayContextMenu(st);
            }
            return 0;
        }

        case WM_SYSCOMMAND: {
            if ((wParam & 0xFFF0) == SC_MINIMIZE && st) {
                HideToTray(st);
                return 0;
            }
            break;
        }

        case WM_SETTINGCHANGE:
        case 0x0320: { // WM_DWMCOLORIZATIONCOLORCHANGED
            if (st && g_uiTheme == UiTheme::System) {
                ApplyTheme();
                ApplyListTheme(st);
                ApplyNativeThemeToWindow(st->hwndMain);
                InvalidateRect(hwnd, nullptr, TRUE);
            }
            return 0;
        }

        case WM_CLOSE: {
            if (st && !st->allowExit && wParam != 1) {
                HideToTray(st);
                return 0;
            }
            if (st) SaveShellConfig(st);
            DestroyWindow(hwnd);
            return 0;
        }

        case WM_TERMINAL_PROGRESS: {
            if (st) {
                st->termProgressState = (lParam >= 0) ? 1 : 0;
                st->termProgressValue = (lParam >= 0) ? static_cast<int>(lParam / 100) : 0;
                InvalidateRect(hwnd, &st->rcStatusBar, FALSE);
            }
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_PRINTCLIENT: {
            if (st && wParam) {
                DrawDashboardCanvas(st, reinterpret_cast<HDC>(wParam));
            }
            return 0;
        }

        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            if (st) {
                DrawDashboardCanvas(st, hdc);
            }
            EndPaint(hwnd, &ps);
            return 0;
        }

        case WM_DESTROY: {
            KillTimer(hwnd, TIMER_REFRESH_ID);
            KillTimer(hwnd, TIMER_KPI_ANIM_ID);
            if (st) {
                if (st->hwndHintWnd) {
                    DestroyWindow(st->hwndHintWnd);
                    st->hwndHintWnd = nullptr;
                }
                RemoveTrayIcon(st);
                st->gridView.reset();
                if (st->hFontUi) DeleteObject(st->hFontUi);
                if (st->hFontUiBold) DeleteObject(st->hFontUiBold);
                if (st->hFontKpiVal) DeleteObject(st->hFontKpiVal);
                if (st->hFontProcList) DeleteObject(st->hFontProcList);
                if (st->hFontTaskList) DeleteObject(st->hFontTaskList);
            }
            PostQuitMessage(0);
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

int RunGuiDashboard(ProcessManager* pm, IpcServer* ipc, TaskQueueManager* tqm) {
    SetProcessDPIAware();

    INITCOMMONCONTROLSEX icex = {};
    icex.dwSize = sizeof(icex);
    icex.dwICC = ICC_LISTVIEW_CLASSES | ICC_TAB_CLASSES | ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icex);

    HINSTANCE hInst = GetModuleHandleW(nullptr);
    TerminalView::RegisterWindowClass(hInst);
    TerminalGridView::RegisterWindowClass(hInst);

    g_uiLanguage = DetectDefaultLanguage();
    ApplyTheme();

    DashboardState state;
    state.pm = pm;
    state.ipc = ipc;
    state.tqm = tqm;
    state.guiThreadId = GetCurrentThreadId();
    state.dashboardStartTimeStr = GetCurrentTimestampStr();
    state.dashboardStartTickMs = GetNowTickMs();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = DashboardWndProc;
    wc.hInstance = hInst;
    wc.hIcon = LoadAppIcon();
    wc.hIconSm = LoadAppSmallIcon();
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"HanchoDashboardWnd";
    RegisterClassExW(&wc);

    HWND hwnd = CreateWindowExW(
        0,
        wc.lpszClassName,
        MainWindowTitle().c_str(),
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, 1460, 940,
        nullptr, nullptr, hInst, &state
    );
    if (!hwnd) return 1;

    if (pm) {
        pm->SetTerminalCallbacks(
            [hwnd](const std::wstring& resolvedCmd,
                   const std::wstring& cwd,
                   std::function<void(const char*, size_t)> onOutput,
                   std::wstring& outError) -> TerminalView* {
                CreateTermViewParams params;
                params.resolvedCmd = resolvedCmd;
                params.cwd = cwd;
                params.onOutput = std::move(onOutput);
                SendMessageW(hwnd, WM_PC_CREATE_TERMVIEW, 0, reinterpret_cast<LPARAM>(&params));
                outError = params.outError;
                return params.outView;
            },
            [hwnd](TerminalView* tv) {
                if (tv && IsWindow(hwnd)) {
                    PostMessageW(hwnd, WM_PC_DESTROY_TERMVIEW, reinterpret_cast<WPARAM>(tv), 0);
                }
            }
        );
    }

    if (ipc) {
        ipc->SetNotifyHwnd(hwnd);
    }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);
    ApplyNativeThemeToWindow(hwnd);

    ACCEL accels[2] = {};
    accels[0].fVirt = FVIRTKEY | FCONTROL;
    accels[0].key = 'T';
    accels[0].cmd = IDC_BTN_VIEW_TASKS;
    accels[1].fVirt = FVIRTKEY | FCONTROL | FSHIFT;
    accels[1].key = 'B';
    accels[1].cmd = IDC_BT_DEBUG;
    HACCEL hAccel = CreateAcceleratorTableW(accels, 2);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (hAccel && TranslateAcceleratorW(hwnd, hAccel, &msg)) {
            continue;
        }
        TranslateMessage(&msg);
        // An exception escaping a window procedure would end the process without a trace: log it and keep running.
        try {
            DispatchMessageW(&msg);
        } catch (const std::exception& e) {
            WriteAuditLog("CRASH_GUARD", std::string("GUI message dispatch exception: ") + e.what());
        } catch (...) {
            WriteAuditLog("CRASH_GUARD", "GUI message dispatch exception: unknown");
        }
    }
    if (hAccel) DestroyAcceleratorTable(hAccel);

    if (pm) pm->SetTerminalCallbacks(nullptr, nullptr);
    if (ipc) ipc->SetNotifyHwnd(nullptr);
    return static_cast<int>(msg.wParam);
}

int RunGuiDashboard(ProcessManager& pm, IpcServer& ipc, TaskQueueManager& tqm) {
    return RunGuiDashboard(&pm, &ipc, &tqm);
}

int RunStandaloneGuiDashboard() {
    return RunGuiDashboard(nullptr, nullptr, nullptr);
}

} // namespace pc
