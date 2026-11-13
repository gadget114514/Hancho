#pragma once

// Colours and native theming shared by the dashboard and the standalone Chat
// window. The palette is read from <data-dir>/config.json ("theme") so a
// separate Chat process follows the same theme as the dashboard.

#include <windows.h>

namespace pc {

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
