#pragma once

#include <windows.h>

class SettingsManager {
public:
    static SettingsManager& Instance() {
        static SettingsManager inst;
        return inst;
    }

    int GetTabGridCellW() const { return m_tabGridCellW; }
    void SetTabGridCellW(int w) { m_tabGridCellW = w; }
    int GetTabGridCellH() const { return m_tabGridCellH; }
    void SetTabGridCellH(int h) { m_tabGridCellH = h; }
    bool IsTabGridRefreshEnabled() const { return m_tabGridRefreshEnabled; }
    void SetTabGridRefreshEnabled(bool enabled) { m_tabGridRefreshEnabled = enabled; }
    int GetTabGridRefreshIntervalMs() const { return m_tabGridRefreshIntervalMs; }
    void SetTabGridRefreshIntervalMs(int ms) { m_tabGridRefreshIntervalMs = ms; }

private:
    SettingsManager() = default;
    int m_tabGridCellW = 240;
    int m_tabGridCellH = 170;
    bool m_tabGridRefreshEnabled = true;
    int m_tabGridRefreshIntervalMs = 2000;
};
