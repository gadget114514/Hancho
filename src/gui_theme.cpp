#include "gui_theme.h"

#include "common.h"

#include <dwmapi.h>
#include <uxtheme.h>

namespace pc {
namespace {

COLORREF GetSystemAccentColor() {
    DWORD color = 0;
    BOOL opaque = FALSE;
    if (SUCCEEDED(DwmGetColorizationColor(&color, &opaque))) {
        return RGB((color >> 16) & 0xFF, (color >> 8) & 0xFF, color & 0xFF);
    }
    return GetSysColor(COLOR_HIGHLIGHT);
}

} // namespace

bool IsSystemDarkMode() {
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

ThemeColors ThemeFor(UiTheme theme) {
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

bool IsPastelTheme(UiTheme theme) {
    return theme == UiTheme::Pastel || (theme >= UiTheme::PastelBlue && theme <= UiTheme::PastelYellow);
}

bool IsDarkUiTheme(UiTheme theme) {
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

const char* ThemeCode(UiTheme theme) {
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

UiTheme ParseTheme(const std::string& code) {
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

using SetPreferredAppModeFn = int(WINAPI*)(int);
using AllowDarkModeForWindowFn = BOOL(WINAPI*)(HWND, BOOL);

void ApplyDarkModeApi(HWND hwnd, bool dark) {
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

void ApplyNativeControlTheme(HWND hwnd, bool dark) {
    if (!hwnd) return;
    ApplyDarkModeApi(hwnd, dark);
    SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : L"", dark ? nullptr : L"");
}

// The top-level window frame must not be un-themed in light mode: passing empty
// strings to SetWindowTheme disables visual styles, so the caption falls back to
// the classic 24x24 buttons and the modern DWM frame is lost. nullptr restores
// the default theme, keeping the caption buttons identical in every theme.
static void ApplyWindowFrameTheme(HWND hwnd, bool dark) {
    ApplyDarkModeApi(hwnd, dark);
    SetWindowTheme(hwnd, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
}

void ApplyNativeThemeToWindow(HWND hwnd, bool dark) {
    if (!hwnd) return;
    BOOL darkFlag = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, 20, &darkFlag, sizeof(darkFlag));
    DwmSetWindowAttribute(hwnd, 19, &darkFlag, sizeof(darkFlag));
    ApplyWindowFrameTheme(hwnd, dark);
    for (HWND child = GetWindow(hwnd, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        ApplyNativeControlTheme(child, dark);
        if (GetWindow(child, GW_CHILD)) {
            for (HWND grand = GetWindow(child, GW_CHILD); grand; grand = GetWindow(grand, GW_HWNDNEXT)) {
                ApplyNativeControlTheme(grand, dark);
            }
        }
    }
}

DashboardPalette GetDashboardPalette() {
    UiTheme theme = UiTheme::System;
    const std::string content = ReadFileUtf8Wide(GetLogsDirectoryWide() + L"\\config.json");
    if (!content.empty()) {
        const std::string code = JsonGetString(content, "theme", "");
        if (!code.empty()) theme = ParseTheme(code);
    }
    const ThemeColors c = ThemeFor(theme);
    DashboardPalette p;
    p.windowBg = c.windowBg;
    p.panel = c.panel;
    p.border = c.border;
    p.text = c.text;
    p.muted = c.muted;
    p.accent = c.accent;
    p.listText = c.listText;
    p.rowSelected = c.rowSelected;
    p.rowRunning = c.rowRunning;
    p.rowIdle = c.rowIdle;
    p.rowCompleted = c.rowCompleted;
    p.rowFailed = c.rowFailed;
    p.dark = IsDarkUiTheme(theme);
    return p;
}

void InitDashboardDarkMode(bool dark) {
    ApplyDarkModeApi(nullptr, dark);
}

void ApplyDashboardNativeTheme(HWND hwnd) {
    ApplyNativeThemeToWindow(hwnd, GetDashboardPalette().dark);
}

} // namespace pc
