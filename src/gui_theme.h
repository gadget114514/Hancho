#pragma once

// Colours and native theming shared by the dashboard, the standalone Chat
// window and the standalone terminal client (HanchoTerm.exe). The palette is
// read from <data-dir>/config.json ("theme") so separate processes follow the
// same theme as the dashboard.

#include <windows.h>

#include <string>

namespace pc {

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

bool IsSystemDarkMode();
ThemeColors ThemeFor(UiTheme theme);
bool IsPastelTheme(UiTheme theme);
bool IsDarkUiTheme(UiTheme theme);
const char* ThemeCode(UiTheme theme);
UiTheme ParseTheme(const std::string& code);

// Dark-mode opt-in and native (title bar / controls) theming helpers.
void ApplyDarkModeApi(HWND hwnd, bool dark);
void ApplyNativeControlTheme(HWND hwnd, bool dark);
void ApplyNativeThemeToWindow(HWND hwnd, bool dark);

struct DashboardPalette {
    COLORREF windowBg = RGB(10, 10, 12);
    COLORREF panel = RGB(18, 18, 20);
    COLORREF border = RGB(50, 50, 54);
    COLORREF text = RGB(202, 202, 208);
    COLORREF muted = RGB(145, 145, 150);
    COLORREF accent = RGB(95, 125, 145);
    COLORREF listText = RGB(202, 202, 208);
    COLORREF rowSelected = RGB(55, 55, 60);
    COLORREF rowRunning = RGB(18, 30, 24);
    COLORREF rowIdle = RGB(34, 30, 20);
    COLORREF rowCompleted = RGB(20, 28, 36);
    COLORREF rowFailed = RGB(36, 22, 24);
    bool dark = true;

    bool operator==(const DashboardPalette& o) const {
        return windowBg == o.windowBg && panel == o.panel && border == o.border &&
               text == o.text && muted == o.muted && accent == o.accent &&
               listText == o.listText && rowSelected == o.rowSelected &&
               rowRunning == o.rowRunning && rowIdle == o.rowIdle &&
               rowCompleted == o.rowCompleted && rowFailed == o.rowFailed &&
               dark == o.dark;
    }
    bool operator!=(const DashboardPalette& o) const { return !(*this == o); }
};

// Reads the configured theme every time (cheap; used at start and on refresh).
DashboardPalette GetDashboardPalette();
// Opt the process into dark mode before creating windows (call once at startup).
void InitDashboardDarkMode(bool dark);
// Dark title bar + DarkMode_Explorer on the window and its controls.
void ApplyDashboardNativeTheme(HWND hwnd);

} // namespace pc
