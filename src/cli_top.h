#pragma once

#include "common.h"
#include "ipc_client.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <conio.h>
#else
#include <termios.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <signal.h>
#endif

namespace pc::top {

// Terminal keys handled by hancho-cli top
enum class TopKey {
    None = 0,
    Quit,
    Refresh,
    NextTab,
    PrevTab,
    TabProcesses,
    TabQueue,
    TabMonitor,
    TabDiagnostics,
    TabMemory,
    Up,
    Down,
    Left,
    Right,
    PageUp,
    PageDown,
    Home,
    End,
    Enter,
    Escape,
    Help,
    JobStop,
    JobRetry,
    JobCancel,
    Unknown
};

// 5 primary dashboard views (cards 062-1, 062-2, 062-3)
enum class TopTab {
    Processes = 0,
    Queue = 1,
    Monitor = 2,
    Diagnostics = 3,
    Memory = 4,
    Count = 5
};

inline const char* TabName(TopTab tab) {
    switch (tab) {
        case TopTab::Processes:   return "1: Processes";
        case TopTab::Queue:       return "2: Queue";
        case TopTab::Monitor:     return "3: Monitor";
        case TopTab::Diagnostics: return "4: Why";
        case TopTab::Memory:      return "5: Memory";
        default:                  return "Unknown";
    }
}

// Formatting helpers
inline std::string FormatBytes(int64_t bytes) {
    if (bytes < 0) bytes = 0;
    std::ostringstream ss;
    if (bytes < 1024) {
        ss << bytes << " B";
    } else if (bytes < 1024 * 1024) {
        ss << std::fixed << std::setprecision(1) << (static_cast<double>(bytes) / 1024.0) << " KB";
    } else if (bytes < 1024 * 1024 * 1024) {
        ss << std::fixed << std::setprecision(1) << (static_cast<double>(bytes) / (1024.0 * 1024.0)) << " MB";
    } else {
        ss << std::fixed << std::setprecision(2) << (static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0)) << " GB";
    }
    return ss.str();
}

inline std::string FormatTimeStr(const std::string& timeStr) {
    if (timeStr.empty()) return "-";
    auto tPos = timeStr.find('T');
    if (tPos == std::string::npos) tPos = timeStr.find(' ');
    if (tPos != std::string::npos && tPos + 8 < timeStr.size()) {
        return timeStr.substr(tPos + 1, 8);
    }
    return timeStr;
}

inline std::string PadOrTruncate(const std::string& str, size_t width) {
    if (str.size() > width) {
        if (width <= 3) return str.substr(0, width);
        return str.substr(0, width - 3) + "...";
    }
    return str + std::string(width - str.size(), ' ');
}

// Current state of the top TUI
struct TopState {
    TopTab activeTab = TopTab::Processes;
    int selectedRow = 0;
    int monitorProcIndex = 0;
    bool showHelp = false;
    int intervalMs = 1000;
    bool coreConnected = false;
    std::string timestamp;
    std::string statusMessage;

    // View data models
    nlohmann::json processData;
    nlohmann::json countsData;
    nlohmann::json queueData;
    nlohmann::json monitorStream;
    nlohmann::json monitorState;
    nlohmann::json diagBreakdown;
    nlohmann::json failedJobsData;
    nlohmann::json diagJobDetail;
    nlohmann::json memoryStats;
    nlohmann::json memoryRecords;

    void NextTab() {
        int next = (static_cast<int>(activeTab) + 1) % static_cast<int>(TopTab::Count);
        activeTab = static_cast<TopTab>(next);
        selectedRow = 0;
    }

    void PrevTab() {
        int prev = (static_cast<int>(activeTab) - 1 + static_cast<int>(TopTab::Count)) % static_cast<int>(TopTab::Count);
        activeTab = static_cast<TopTab>(prev);
        selectedRow = 0;
    }

    void SetTab(TopTab tab) {
        activeTab = tab;
        selectedRow = 0;
    }
};

// Check if stdin/stdout are interactive terminals
inline bool IsTerminalInteractive() {
#ifdef _WIN32
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hIn == INVALID_HANDLE_VALUE || hOut == INVALID_HANDLE_VALUE) return false;
    DWORD inMode = 0, outMode = 0;
    if (!GetConsoleMode(hIn, &inMode) || !GetConsoleMode(hOut, &outMode)) return false;
    return true;
#else
    return isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
#endif
}

// Query terminal width and height in columns/rows
inline void GetTerminalSize(int& cols, int& rows) {
    cols = 80;
    rows = 24;
#ifdef _WIN32
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE) {
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(hOut, &csbi)) {
            cols = csbi.srWindow.Right - csbi.srWindow.Left + 1;
            rows = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        }
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        cols = ws.ws_col;
        rows = ws.ws_row;
    }
#endif
    if (cols < 40) cols = 80;
    if (rows < 10) rows = 24;
}

// Parse an input byte or escape sequence into a TopKey (testable pure function)
inline TopKey ParseKeySequence(const std::string& seq) {
    if (seq.empty()) return TopKey::None;
    if (seq == "\x1b") return TopKey::Escape;
    if (seq == "\x1b[A" || seq == "\x1bOA") return TopKey::Up;
    if (seq == "\x1b[B" || seq == "\x1bOB") return TopKey::Down;
    if (seq == "\x1b[C" || seq == "\x1bOC") return TopKey::Right;
    if (seq == "\x1b[D" || seq == "\x1bOD") return TopKey::Left;
    if (seq == "\x1b[H") return TopKey::Home;
    if (seq == "\x1b[F") return TopKey::End;
    if (seq == "\x1b[5~") return TopKey::PageUp;
    if (seq == "\x1b[6~") return TopKey::PageDown;
    if (seq == "\x1b[Z") return TopKey::PrevTab; // Shift+Tab
    if (seq == " ") return TopKey::Refresh;
    if (seq == "\x12") return TopKey::Refresh; // Ctrl+R

    if (seq.size() == 1) {
        char c = seq[0];
        if (c == 3) return TopKey::Quit; // Ctrl+C
        if (c == 9) return TopKey::NextTab; // Tab
        if (c == '\r' || c == '\n') return TopKey::Enter;
        if (c == ' ') return TopKey::Refresh;
        if (c == 'q' || c == 'Q') return TopKey::Quit;
        if (c == 's' || c == 'S') return TopKey::JobStop;
        if (c == 'r' || c == 'R') return TopKey::JobRetry;
        if (c == 'e' || c == 'E') return TopKey::JobRetry;
        if (c == 'c' || c == 'C') return TopKey::JobCancel;
        if (c == 'x' || c == 'X') return TopKey::JobCancel;
        if (c == 'u' || c == 'U') return TopKey::Refresh;
        if (c == '1') return TopKey::TabProcesses;
        if (c == '2') return TopKey::TabQueue;
        if (c == '3') return TopKey::TabMonitor;
        if (c == '4') return TopKey::TabDiagnostics;
        if (c == '5') return TopKey::TabMemory;
        if (c == 'k' || c == 'K') return TopKey::Up;
        if (c == 'j' || c == 'J') return TopKey::Down;
        if (c == 'h' || c == 'H') return TopKey::Left;
        if (c == 'l' || c == 'L') return TopKey::Right;
        if (c == '?' || c == '/') return TopKey::Help;
    }
    return TopKey::Unknown;
}

// RAII Guard for terminal raw mode and alternate screen buffer
class TerminalSession {
public:
    explicit TerminalSession(bool interactive) : interactive_(interactive && IsTerminalInteractive()) {
        if (!interactive_) return;
#ifdef _WIN32
        hIn_ = GetStdHandle(STD_INPUT_HANDLE);
        hOut_ = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hIn_ != INVALID_HANDLE_VALUE && hOut_ != INVALID_HANDLE_VALUE) {
            GetConsoleMode(hIn_, &origInMode_);
            GetConsoleMode(hOut_, &origOutMode_);
            DWORD newOutMode = origOutMode_ | 0x0004 /* ENABLE_VIRTUAL_TERMINAL_PROCESSING */;
            SetConsoleMode(hOut_, newOutMode);
            DWORD newInMode = origInMode_ & ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);
            SetConsoleMode(hIn_, newInMode);
        }
#else
        if (tcgetattr(STDIN_FILENO, &origTermios_) == 0) {
            struct termios raw = origTermios_;
            raw.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
            raw.c_iflag &= ~(IXON | ICRNL);
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 0;
            tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
        }
#endif
        // Switch to alternate screen and hide cursor
        std::cout << "\x1b[?1049h\x1b[?25l" << std::flush;
        active_ = true;
    }

    ~TerminalSession() {
        Restore();
    }

    void Restore() {
        if (!active_) return;
        // Show cursor and switch back to main screen buffer
        std::cout << "\x1b[?25h\x1b[?1049l" << std::flush;
#ifdef _WIN32
        if (hIn_ != INVALID_HANDLE_VALUE) SetConsoleMode(hIn_, origInMode_);
        if (hOut_ != INVALID_HANDLE_VALUE) SetConsoleMode(hOut_, origOutMode_);
#else
        tcsetattr(STDIN_FILENO, TCSAFLUSH, &origTermios_);
#endif
        active_ = false;
    }

    TopKey PollKey(int timeoutMs) {
        if (!active_) return TopKey::None;
#ifdef _WIN32
        int elapsed = 0;
        while (elapsed < timeoutMs) {
            if (_kbhit()) {
                int ch = _getch();
                if (ch == 0 || ch == 0xE0) {
                    int ext = _getch();
                    switch (ext) {
                        case 72: return TopKey::Up;
                        case 80: return TopKey::Down;
                        case 75: return TopKey::Left;
                        case 77: return TopKey::Right;
                        case 15: return TopKey::PrevTab; // Shift+Tab
                        case 73: return TopKey::PageUp;
                        case 81: return TopKey::PageDown;
                        case 71: return TopKey::Home;
                        case 79: return TopKey::End;
                        default: return TopKey::Unknown;
                    }
                }
                std::string s(1, static_cast<char>(ch));
                return ParseKeySequence(s);
            }
            int step = std::min(25, timeoutMs - elapsed);
            SleepMs(static_cast<uint32_t>(step));
            elapsed += step;
        }
        return TopKey::None;
#else
        struct pollfd pfd;
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        pfd.revents = 0;
        int ret = poll(&pfd, 1, timeoutMs);
        if (ret <= 0 || !(pfd.revents & POLLIN)) return TopKey::None;

        unsigned char c = 0;
        if (read(STDIN_FILENO, &c, 1) != 1) return TopKey::None;

        if (c == 27) { // ESC sequence
            struct pollfd pfd2;
            pfd2.fd = STDIN_FILENO;
            pfd2.events = POLLIN;
            pfd2.revents = 0;
            if (poll(&pfd2, 1, 30) > 0 && (pfd2.revents & POLLIN)) {
                std::string seq;
                seq.push_back('\x1b');
                unsigned char buf[16] = {0};
                ssize_t n = read(STDIN_FILENO, buf, sizeof(buf) - 1);
                if (n > 0) {
                    seq.append(reinterpret_cast<const char*>(buf), static_cast<size_t>(n));
                    return ParseKeySequence(seq);
                }
            }
            return TopKey::Escape;
        }

        std::string s(1, static_cast<char>(c));
        return ParseKeySequence(s);
#endif
    }

    bool IsActive() const { return active_; }

private:
    bool interactive_ = false;
    bool active_ = false;
#ifdef _WIN32
    HANDLE hIn_ = INVALID_HANDLE_VALUE;
    HANDLE hOut_ = INVALID_HANDLE_VALUE;
    DWORD origInMode_ = 0;
    DWORD origOutMode_ = 0;
#else
    struct termios origTermios_{};
#endif
};

// Remote host and job attribution helpers (card 062-3, doc/remote-hosts.md)
inline std::string GetJobHost(const nlohmann::json& job) {
    if (!job.is_object()) return "";
    if (job.contains("host") && job["host"].is_string()) {
        std::string h = job["host"].get<std::string>();
        if (!h.empty()) return h;
    }
    if (job.contains("remote_host") && job["remote_host"].is_string()) {
        std::string h = job["remote_host"].get<std::string>();
        if (!h.empty()) return h;
    }
    if (job.contains("attempts") && job["attempts"].is_array() && !job["attempts"].empty()) {
        const auto& lastA = job["attempts"].back();
        if (lastA.is_object() && lastA.contains("remote_host") && lastA["remote_host"].is_string()) {
            std::string h = lastA["remote_host"].get<std::string>();
            if (!h.empty()) return h;
        }
    }
    return "";
}

inline std::string FormatQueueColumn(const std::string& queue, const std::string& host) {
    std::string q = queue.empty() ? "default" : queue;
    if (!host.empty()) {
        return q + " @ " + host;
    }
    return q;
}

inline void MergeRemoteJobs(nlohmann::json& localJobs, const nlohmann::json& remoteData) {
    if (!localJobs.is_array()) localJobs = nlohmann::json::array();
    if (!remoteData.is_object() || !remoteData.value("ok", false)) return;
    if (!remoteData.contains("hosts") || !remoteData["hosts"].is_array()) return;

    for (const auto& hItem : remoteData["hosts"]) {
        if (!hItem.is_object()) continue;
        std::string hostName = hItem.value("host", "");
        if (!hItem.contains("response") || !hItem["response"].is_object()) continue;
        const auto& resp = hItem["response"];
        if (!resp.contains("jobs") || !resp["jobs"].is_array()) continue;

        for (const auto& rJob : resp["jobs"]) {
            if (!rJob.is_object()) continue;
            std::string rId = rJob.value("id", rJob.value("key", ""));
            if (rId.empty()) continue;

            bool exists = false;
            for (auto& lJob : localJobs) {
                if (lJob.is_object() && lJob.value("id", lJob.value("key", "")) == rId) {
                    exists = true;
                    if (GetJobHost(lJob).empty() && !hostName.empty()) {
                        lJob["host"] = hostName;
                    }
                    break;
                }
            }
            if (!exists) {
                nlohmann::json enriched = rJob;
                if (!enriched.contains("host") || enriched["host"].empty()) {
                    enriched["host"] = hostName;
                }
                localJobs.push_back(std::move(enriched));
            }
        }
    }
}

inline void MergeRemoteProcesses(nlohmann::json& localProcs, const nlohmann::json& remoteData) {
    if (!localProcs.is_array()) localProcs = nlohmann::json::array();
    if (!remoteData.is_object() || !remoteData.value("ok", false)) return;
    if (!remoteData.contains("hosts") || !remoteData["hosts"].is_array()) return;

    for (const auto& hItem : remoteData["hosts"]) {
        if (!hItem.is_object()) continue;
        std::string hostName = hItem.value("host", "");
        if (!hItem.contains("response") || !hItem["response"].is_object()) continue;
        const auto& resp = hItem["response"];
        if (!resp.contains("processes") || !resp["processes"].is_array()) continue;

        for (const auto& rProc : resp["processes"]) {
            if (!rProc.is_object()) continue;
            int rId = rProc.value("id", 0);

            bool exists = false;
            for (const auto& lProc : localProcs) {
                if (lProc.is_object() &&
                    lProc.value("host", "") == hostName &&
                    lProc.value("id", 0) == rId) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                nlohmann::json enriched = rProc;
                enriched["host"] = hostName;
                localProcs.push_back(std::move(enriched));
            }
        }
    }
}

// Request builders for interactive operations (cards 062-3, doc/tui-design-ja.md)
inline bool BuildJobStopRequest(const TopState& state, std::string& outReq, std::string& outDesc) {
    if (state.activeTab == TopTab::Processes) {
        if (state.processData.contains("processes") && state.processData["processes"].is_array() &&
            state.selectedRow >= 0 && state.selectedRow < static_cast<int>(state.processData["processes"].size())) {
            const auto& p = state.processData["processes"][static_cast<size_t>(state.selectedRow)];
            int id = p.value("id", 0);
            if (id > 0) {
                nlohmann::json req;
                req["action"] = "kill";
                req["id"] = id;
                outReq = req.dump();
                outDesc = "process #" + std::to_string(id);
                return true;
            }
        }
    } else if (state.activeTab == TopTab::Queue) {
        if (state.queueData.contains("jobs") && state.queueData["jobs"].is_array() &&
            state.selectedRow >= 0 && state.selectedRow < static_cast<int>(state.queueData["jobs"].size())) {
            const auto& j = state.queueData["jobs"][static_cast<size_t>(state.selectedRow)];
            std::string jid = j.value("id", j.value("key", ""));
            std::string host = GetJobHost(j);
            std::string st = j.value("state", "");
            int procId = 0;
            if (j.contains("attempts") && j["attempts"].is_array() && !j["attempts"].empty()) {
                const auto& lastA = j["attempts"].back();
                if (lastA.is_object()) {
                    procId = lastA.value("process_id", 0);
                    if (procId <= 0) procId = lastA.value("remote_process_id", 0);
                }
            }
            if (st == "running") {
                if (!host.empty()) {
                    nlohmann::json req;
                    req["action"] = "taskqueue_cancel";
                    req["id"] = jid;
                    outReq = req.dump();
                    outDesc = "remote job " + jid + " on " + host;
                    return true;
                } else if (procId > 0) {
                    nlohmann::json req;
                    req["action"] = "kill";
                    req["id"] = procId;
                    outReq = req.dump();
                    outDesc = "running job " + jid + " (PID " + std::to_string(procId) + ")";
                    return true;
                } else {
                    nlohmann::json req;
                    req["action"] = "taskqueue_cancel";
                    req["id"] = jid;
                    outReq = req.dump();
                    outDesc = "job " + jid;
                    return true;
                }
            } else if (!jid.empty()) {
                nlohmann::json req;
                req["action"] = "taskqueue_cancel";
                req["id"] = jid;
                outReq = req.dump();
                outDesc = host.empty() ? ("job " + jid) : ("remote job " + jid + " on " + host);
                return true;
            }
        }
    } else if (state.activeTab == TopTab::Monitor) {
        if (state.processData.contains("processes") && state.processData["processes"].is_array() &&
            state.monitorProcIndex >= 0 && state.monitorProcIndex < static_cast<int>(state.processData["processes"].size())) {
            const auto& p = state.processData["processes"][static_cast<size_t>(state.monitorProcIndex)];
            int id = p.value("id", 0);
            if (id > 0) {
                nlohmann::json req;
                req["action"] = "kill";
                req["id"] = id;
                outReq = req.dump();
                outDesc = "monitored process #" + std::to_string(id);
                return true;
            }
        }
    } else if (state.activeTab == TopTab::Diagnostics) {
        if (state.failedJobsData.contains("jobs") && state.failedJobsData["jobs"].is_array() &&
            state.selectedRow >= 0 && state.selectedRow < static_cast<int>(state.failedJobsData["jobs"].size())) {
            const auto& j = state.failedJobsData["jobs"][static_cast<size_t>(state.selectedRow)];
            std::string jid = j.value("id", j.value("key", ""));
            std::string host = GetJobHost(j);
            if (!jid.empty()) {
                nlohmann::json req;
                req["action"] = "taskqueue_cancel";
                req["id"] = jid;
                outReq = req.dump();
                outDesc = host.empty() ? ("failed job " + jid) : ("remote failed job " + jid + " on " + host);
                return true;
            }
        }
    }
    return false;
}

inline bool BuildJobRetryRequest(const TopState& state, std::string& outReq, std::string& outDesc) {
    if (state.activeTab == TopTab::Queue) {
        if (state.queueData.contains("jobs") && state.queueData["jobs"].is_array() &&
            state.selectedRow >= 0 && state.selectedRow < static_cast<int>(state.queueData["jobs"].size())) {
            const auto& j = state.queueData["jobs"][static_cast<size_t>(state.selectedRow)];
            std::string jid = j.value("id", j.value("key", ""));
            std::string host = GetJobHost(j);
            if (!jid.empty()) {
                nlohmann::json req;
                req["action"] = "taskqueue_retry";
                req["id"] = jid;
                req["reset_attempts"] = true;
                req["refresh_instructions"] = true;
                outReq = req.dump();
                outDesc = host.empty() ? ("job " + jid) : ("remote job " + jid + " on " + host);
                return true;
            }
        }
    } else if (state.activeTab == TopTab::Diagnostics) {
        if (state.failedJobsData.contains("jobs") && state.failedJobsData["jobs"].is_array() &&
            state.selectedRow >= 0 && state.selectedRow < static_cast<int>(state.failedJobsData["jobs"].size())) {
            const auto& j = state.failedJobsData["jobs"][static_cast<size_t>(state.selectedRow)];
            std::string jid = j.value("id", j.value("key", ""));
            std::string host = GetJobHost(j);
            if (!jid.empty()) {
                nlohmann::json req;
                req["action"] = "taskqueue_retry";
                req["id"] = jid;
                req["reset_attempts"] = true;
                req["refresh_instructions"] = true;
                outReq = req.dump();
                outDesc = host.empty() ? ("failed job " + jid) : ("remote failed job " + jid + " on " + host);
                return true;
            }
        }
    }
    return false;
}

inline bool BuildJobCancelRequest(const TopState& state, std::string& outReq, std::string& outDesc) {
    if (state.activeTab == TopTab::Queue) {
        if (state.queueData.contains("jobs") && state.queueData["jobs"].is_array() &&
            state.selectedRow >= 0 && state.selectedRow < static_cast<int>(state.queueData["jobs"].size())) {
            const auto& j = state.queueData["jobs"][static_cast<size_t>(state.selectedRow)];
            std::string jid = j.value("id", j.value("key", ""));
            std::string host = GetJobHost(j);
            if (!jid.empty()) {
                nlohmann::json req;
                req["action"] = "taskqueue_cancel";
                req["id"] = jid;
                outReq = req.dump();
                outDesc = host.empty() ? ("job " + jid) : ("remote job " + jid + " on " + host);
                return true;
            }
        }
    } else if (state.activeTab == TopTab::Diagnostics) {
        if (state.failedJobsData.contains("jobs") && state.failedJobsData["jobs"].is_array() &&
            state.selectedRow >= 0 && state.selectedRow < static_cast<int>(state.failedJobsData["jobs"].size())) {
            const auto& j = state.failedJobsData["jobs"][static_cast<size_t>(state.selectedRow)];
            std::string jid = j.value("id", j.value("key", ""));
            std::string host = GetJobHost(j);
            if (!jid.empty()) {
                nlohmann::json req;
                req["action"] = "taskqueue_cancel";
                req["id"] = jid;
                outReq = req.dump();
                outDesc = host.empty() ? ("failed job " + jid) : ("remote failed job " + jid + " on " + host);
                return true;
            }
        }
    }
    return false;
}

// Strip ANSI escape sequences so no-color output still contains the text.
inline std::string StripAnsiSequences(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size();) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            i += 2;
            while (i < s.size() && !(s[i] >= '@' && s[i] <= '~')) ++i;
            if (i < s.size()) ++i;
        } else {
            out.push_back(s[i++]);
        }
    }
    return out;
}

// Render screen content as pure string (decoupled from I/O for unit testability)
inline std::string RenderScreen(const TopState& state, int width, int height, bool useColor = true) {
    std::ostringstream out;

    // ANSI sequence shortcuts
    auto code = [useColor](const std::string& ansi) -> std::string {
        return useColor ? ansi : StripAnsiSequences(ansi);
    };
    auto reset = [useColor]() -> std::string {
        return useColor ? "\x1b[0m" : "";
    };
    auto bold = [useColor](const std::string& s) -> std::string {
        return useColor ? ("\x1b[1m" + s + "\x1b[0m") : s;
    };
    auto invert = [useColor](const std::string& s) -> std::string {
        return useColor ? ("\x1b[7m" + s + "\x1b[0m") : ("[" + s + "]");
    };
    auto colorState = [useColor](const std::string& st) -> std::string {
        if (!useColor) return st;
        std::string lower = st;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
        if (lower == "running") return "\x1b[32;1m" + st + "\x1b[0m";
        if (lower == "completed" || lower == "succeeded") return "\x1b[36m" + st + "\x1b[0m";
        if (lower == "failed" || lower == "timed_out" || lower == "idle_timed_out" || lower == "killed") return "\x1b[31;1m" + st + "\x1b[0m";
        if (lower == "waiting" || lower == "pending" || lower == "queued") return "\x1b[33m" + st + "\x1b[0m";
        if (lower == "blocked") return "\x1b[35m" + st + "\x1b[0m";
        return st;
    };

    // 1. Header Bar
    out << bold("Hancho Top") << "  "
        << (state.timestamp.empty() ? GetCurrentTimestampStr() : state.timestamp)
        << "  |  Core: "
        << (state.coreConnected ? code("\x1b[32;1mONLINE\x1b[0m") : code("\x1b[31;1mOFFLINE\x1b[0m"))
        << "  |  Interval: " << state.intervalMs << "ms\n";

    // 2. Tab Navigation Bar
    out << " ";
    for (int i = 0; i < static_cast<int>(TopTab::Count); ++i) {
        TopTab t = static_cast<TopTab>(i);
        std::string label = std::string(" ") + TabName(t) + " ";
        if (t == state.activeTab) {
            out << invert(label);
        } else {
            out << code("\x1b[90m") << label << reset();
        }
        out << " ";
    }
    out << "\n";

    // Divider
    std::string divLine(static_cast<size_t>(std::max(40, width)), '-');
    out << code("\x1b[90m") << divLine << reset() << "\n";

    int linesRendered = 3;

    // 3. View Content (5 dashboard equivalent panes)
    if (state.activeTab == TopTab::Processes) {
        // Tab 1: Processes View
        int runningCount = 0;
        int completedCount = 0;
        int failedCount = 0;
        int totalCount = 0;

        if (state.processData.contains("processes") && state.processData["processes"].is_array()) {
            const auto& procs = state.processData["processes"];
            totalCount = static_cast<int>(procs.size());
            for (const auto& p : procs) {
                std::string st = p.value("state", "");
                if (st == "running") runningCount++;
                else if (st == "completed" || st == "succeeded") completedCount++;
                else if (st == "failed" || st == "timed_out" || st == "killed") failedCount++;
            }
        }

        out << bold(" Process List") << "   "
            << "Total: " << totalCount
            << "  |  Running: " << code("\x1b[32;1m") << runningCount << reset()
            << "  |  Completed: " << code("\x1b[36m") << completedCount << reset()
            << "  |  Failed: " << code("\x1b[31;1m") << failedCount << reset() << "\n";
        out << " " << bold("  ID   PID   STATE             ELAPSED     CPU       COMMAND / LAST LINE") << "\n";
        out << code("\x1b[90m") << " ---- ----- ----------------- ----------- --------- ----------------------------------------" << reset() << "\n";
        linesRendered += 3;

        if (state.processData.contains("processes") && state.processData["processes"].is_array() &&
            !state.processData["processes"].empty()) {
            const auto& procs = state.processData["processes"];
            int maxTableRows = std::max(2, height - linesRendered - 8);
            int displayCount = std::min(static_cast<int>(procs.size()), maxTableRows);

            for (int i = 0; i < displayCount; ++i) {
                const auto& p = procs[static_cast<size_t>(i)];
                std::string id = std::to_string(p.value("id", 0));
                std::string pid = std::to_string(p.value("pid", 0));
                std::string host = p.value("host", "");
                std::string st = p.value("state", "");
                std::string elapsed = p.value("elapsed_str", p.value("uptime_str", ""));
                std::string cpu = p.value("cpu_time_str", "");
                std::string last = p.value("last_line", p.value("command", ""));
                if (!host.empty()) {
                    last = "[" + host + "] " + last;
                }

                bool isSel = (i == state.selectedRow);
                std::ostringstream row;
                row << (isSel ? ">" : " ") << " "
                    << std::setw(4) << id << " "
                    << std::setw(5) << pid << " "
                    << std::left << PadOrTruncate(st, 17) << " "
                    << PadOrTruncate(elapsed, 11) << " "
                    << PadOrTruncate(cpu, 9) << " "
                    << PadOrTruncate(last, static_cast<size_t>(std::max(10, width - 56)));

                if (isSel && useColor) {
                    out << "\x1b[1;36m" << row.str() << "\x1b[0m\n";
                } else {
                    out << row.str() << "\n";
                }
                linesRendered++;
            }

            // Detail subpane for selected process
            if (state.selectedRow >= 0 && state.selectedRow < static_cast<int>(procs.size())) {
                const auto& sel = procs[static_cast<size_t>(state.selectedRow)];
                std::string id = std::to_string(sel.value("id", 0));
                std::string pid = std::to_string(sel.value("pid", 0));
                std::string host = sel.value("host", "");
                std::string st = sel.value("state", "");
                std::string elapsed = sel.value("elapsed_str", sel.value("uptime_str", ""));
                int exitCode = sel.value("exit_code", 0);
                std::string cmd = sel.value("command", sel.value("commandLine", ""));
                std::string cwd = sel.value("cwd", "");
                std::string last = sel.value("last_line", "");
                std::string agentType = sel.value("agent_type", "");
                std::string agentStatus = sel.value("agent_status", "");

                out << code("\x1b[90m") << divLine << reset() << "\n";
                linesRendered++;
                out << bold("[Process Detail: #" + id + " (PID " + pid + ")]")
                    << "  State: " << colorState(st)
                    << "  Exit: " << exitCode
                    << "  Uptime: " << elapsed;
                if (!host.empty()) out << "  Host: " << host;
                out << "\n";
                linesRendered++;
                if (!host.empty()) {
                    out << "   Host: " << host << "\n";
                    linesRendered++;
                }
                if (!agentType.empty() || !agentStatus.empty()) {
                    out << "   Agent: " << (agentType.empty() ? "-" : agentType)
                        << "  |  Status: " << (agentStatus.empty() ? "-" : agentStatus) << "\n";
                    linesRendered++;
                }
                if (!cmd.empty()) {
                    out << "   Command: " << PadOrTruncate(cmd, static_cast<size_t>(std::max(10, width - 14))) << "\n";
                    linesRendered++;
                }
                if (!cwd.empty()) {
                    out << "   Cwd: " << PadOrTruncate(cwd, static_cast<size_t>(std::max(10, width - 10))) << "\n";
                    linesRendered++;
                }
                if (!last.empty()) {
                    out << "   Output: " << PadOrTruncate(last, static_cast<size_t>(std::max(10, width - 13))) << "\n";
                    linesRendered++;
                }
            }
        } else {
            out << "   (No running or recent processes)\n";
            linesRendered++;
        }
    } else if (state.activeTab == TopTab::Queue) {
        // Tab 2: Task Queue Overview & Jobs View
        out << bold(" Task Queue Overview") << "   ";
        if (state.countsData.is_object() && state.countsData.value("ok", false)) {
            out << "Running: " << code("\x1b[32;1m") << state.countsData.value("running", 0) << reset()
                << "  |  Waiting: " << code("\x1b[33m") << state.countsData.value("waiting", 0) << reset()
                << "  |  Pending: " << code("\x1b[33m") << state.countsData.value("pending", 0) << reset()
                << "  |  Succeeded: " << code("\x1b[36m") << state.countsData.value("succeeded", 0) << reset()
                << "  |  Failed: " << code("\x1b[31;1m") << state.countsData.value("failed", 0) << reset()
                << "  |  Total: " << state.countsData.value("total", 0) << "\n";
        } else {
            out << "Running: 0  |  Waiting: 0  |  Pending: 0  |  Succeeded: 0  |  Failed: 0\n";
        }
        linesRendered++;

        out << " " << bold("  JOB ID         QUEUE             STATE        DURATION    SUMMARY") << "\n";
        out << code("\x1b[90m") << " -------------- ----------------- ------------ ----------- ----------------------------------------" << reset() << "\n";
        linesRendered += 2;

        if (state.queueData.contains("jobs") && state.queueData["jobs"].is_array() && !state.queueData["jobs"].empty()) {
            const auto& jobs = state.queueData["jobs"];
            int maxTableRows = std::max(2, height - linesRendered - 8);
            int displayCount = std::min(static_cast<int>(jobs.size()), maxTableRows);

            for (int i = 0; i < displayCount; ++i) {
                const auto& j = jobs[static_cast<size_t>(i)];
                std::string jid = j.value("id", j.value("key", "-"));
                std::string rawQueue = j.value("taskqueue", j.value("queue", "default"));
                std::string host = GetJobHost(j);
                std::string queueCol = FormatQueueColumn(rawQueue, host);
                std::string st = j.value("state", "pending");
                std::string dur = j.value("duration_str", "");
                if (dur.empty() && j.contains("durationSec")) {
                    dur = std::to_string(j["durationSec"].get<int64_t>()) + "s";
                }
                std::string summary = j.value("summary", j.value("commandLine", ""));

                bool isSel = (i == state.selectedRow);
                std::ostringstream row;
                row << (isSel ? ">" : " ") << " "
                    << PadOrTruncate(jid, 14) << " "
                    << PadOrTruncate(queueCol, 17) << " "
                    << PadOrTruncate(st, 12) << " "
                    << PadOrTruncate(dur, 11) << " "
                    << PadOrTruncate(summary, static_cast<size_t>(std::max(10, width - 62)));

                if (isSel && useColor) {
                    out << "\x1b[1;36m" << row.str() << "\x1b[0m\n";
                } else {
                    out << row.str() << "\n";
                }
                linesRendered++;
            }

            // Detail subpane for selected job
            if (state.selectedRow >= 0 && state.selectedRow < static_cast<int>(jobs.size())) {
                const auto& sel = jobs[static_cast<size_t>(state.selectedRow)];
                std::string jid = sel.value("id", sel.value("key", "-"));
                std::string name = sel.value("name", "");
                std::string rawQueue = sel.value("taskqueue", sel.value("queue", "default"));
                std::string host = GetJobHost(sel);
                std::string queueCol = FormatQueueColumn(rawQueue, host);
                std::string st = sel.value("state", "pending");
                std::string summary = sel.value("summary", "");
                std::string cmd = sel.value("commandLine", sel.value("cmd", ""));
                std::string blockedBy = sel.value("blockedBy", "");
                int attempts = sel.value("attemptsCount", 0);
                int maxAttempts = sel.value("maxAttempts", 1);
                int timeoutSec = sel.value("timeoutSec", 0);
                int remotePid = 0;
                if (sel.contains("attempts") && sel["attempts"].is_array() && !sel["attempts"].empty()) {
                    const auto& lastAtt = sel["attempts"].back();
                    if (lastAtt.is_object() && lastAtt.contains("remote_process_id")) {
                        remotePid = lastAtt.value("remote_process_id", 0);
                    }
                }

                out << code("\x1b[90m") << divLine << reset() << "\n";
                linesRendered++;
                out << bold("[Job Detail: " + jid + "]")
                    << "  Queue: " << queueCol
                    << "  State: " << colorState(st)
                    << "  Attempts: " << attempts << "/" << maxAttempts;
                if (!host.empty()) out << "  Host: " << host;
                if (timeoutSec > 0) out << "  Timeout: " << timeoutSec << "s";
                out << "\n";
                linesRendered++;
                if (remotePid > 0) {
                    out << "   Remote PID: " << remotePid << "\n";
                    linesRendered++;
                }
                if (!name.empty() && name != jid) {
                    out << "   Name: " << name << "\n";
                    linesRendered++;
                }
                if (!summary.empty()) {
                    out << "   Summary: " << PadOrTruncate(summary, static_cast<size_t>(std::max(10, width - 14))) << "\n";
                    linesRendered++;
                }
                if (!cmd.empty()) {
                    out << "   Command: " << PadOrTruncate(cmd, static_cast<size_t>(std::max(10, width - 14))) << "\n";
                    linesRendered++;
                }
                if (!blockedBy.empty()) {
                    out << "   Blocked By: " << code("\x1b[35m") << blockedBy << reset() << "\n";
                    linesRendered++;
                }
            }
        } else {
            out << "   (Queue is empty)\n";
            linesRendered++;
        }
    } else if (state.activeTab == TopTab::Monitor) {
        // Tab 3: Execution Monitor Pane (LLM Agent Stream / Process Monitor)
        out << bold(" Execution Monitor Stream") << "\n";
        linesRendered++;

        int monId = 0;
        int pid = 0;
        std::string procName, procState;
        if (state.processData.contains("processes") && state.processData["processes"].is_array() &&
            !state.processData["processes"].empty()) {
            const auto& procs = state.processData["processes"];
            int idx = state.monitorProcIndex;
            if (idx < 0) idx = 0;
            if (idx >= static_cast<int>(procs.size())) idx = static_cast<int>(procs.size()) - 1;
            const auto& p = procs[static_cast<size_t>(idx)];
            monId = p.value("id", 0);
            pid = p.value("pid", 0);
            procState = p.value("state", "");
            procName = p.value("name", p.value("command", "Process #" + std::to_string(monId)));

            out << " Process: " << bold("[#" + std::to_string(monId) + "]") << " " << procName
                << " (PID: " << pid << ", State: " << colorState(procState) << ")  "
                << code("\x1b[90m") << "[< / > Switch Process (" << (idx + 1) << "/" << procs.size() << ")]" << reset() << "\n";
            linesRendered++;

            if (state.monitorState.is_object() && state.monitorState.value("ok", false)) {
                std::string agType = state.monitorState.value("agent_type", "");
                std::string agStatus = state.monitorState.value("status", "");
                std::string model = state.monitorState.value("model", "");
                std::string goal = state.monitorState.value("goal", "");
                std::string notes = state.monitorState.value("notes", "");
                if (!agType.empty() || !agStatus.empty() || !model.empty()) {
                    out << " Agent: " << (agType.empty() ? "-" : agType)
                        << "  |  Status: " << (agStatus.empty() ? "-" : agStatus);
                    if (!model.empty()) out << "  |  Model: " << model;
                    out << "\n";
                    linesRendered++;
                }
                if (!goal.empty()) {
                    out << " Goal: " << PadOrTruncate(goal, static_cast<size_t>(std::max(10, width - 9))) << "\n";
                    linesRendered++;
                }
                if (!notes.empty()) {
                    out << " Notes: " << PadOrTruncate(notes, static_cast<size_t>(std::max(10, width - 10))) << "\n";
                    linesRendered++;
                }
            }
        } else {
            out << "   (no active processes available for monitoring)\n";
            linesRendered++;
        }

        out << code("\x1b[90m") << divLine << reset() << "\n";
        linesRendered++;

        // Event stream table or output log tail
        if (state.monitorStream.is_object() && state.monitorStream.contains("events") &&
            state.monitorStream["events"].is_array() && !state.monitorStream["events"].empty()) {
            out << " " << bold("  TIME     TYPE         DETAILS") << "\n";
            out << code("\x1b[90m") << " -------- ------------ -----------------------------------------------------------------" << reset() << "\n";
            linesRendered += 2;

            const auto& evs = state.monitorStream["events"];
            int maxEvRows = std::max(2, height - linesRendered - 3);
            int startIdx = std::max(0, static_cast<int>(evs.size()) - maxEvRows);
            for (size_t i = static_cast<size_t>(startIdx); i < evs.size(); ++i) {
                const auto& ev = evs[i];
                std::string ts = FormatTimeStr(ev.value("timestamp", ev.value("ts", "")));
                std::string type = ev.value("type", "EVENT");
                std::string details;
                if (ev.contains("payload")) {
                    const auto& pl = ev["payload"];
                    if (pl.is_string()) details = pl.get<std::string>();
                    else if (pl.is_object()) {
                        if (pl.contains("tool")) details = pl.value("tool", "") + " " + pl.value("args", "");
                        else if (pl.contains("thought")) details = pl.value("thought", "");
                        else if (pl.contains("output")) details = pl.value("output", "");
                        else details = pl.dump();
                    } else details = pl.dump();
                } else if (ev.contains("data")) {
                    details = ev.value("data", "");
                } else if (ev.contains("summary")) {
                    details = ev.value("summary", "");
                }

                std::string typeColor;
                if (useColor) {
                    std::string lt = type;
                    std::transform(lt.begin(), lt.end(), lt.begin(), [](unsigned char c){ return static_cast<char>(std::tolower(c)); });
                    if (lt.find("tool") != std::string::npos) typeColor = "\x1b[36m";
                    else if (lt.find("thought") != std::string::npos || lt.find("think") != std::string::npos) typeColor = "\x1b[35m";
                    else if (lt.find("progress") != std::string::npos) typeColor = "\x1b[32m";
                    else if (lt.find("error") != std::string::npos || lt.find("fail") != std::string::npos) typeColor = "\x1b[31;1m";
                    else typeColor = "\x1b[37m";
                }

                out << " " << PadOrTruncate(ts, 8) << " "
                    << (useColor ? typeColor : "") << PadOrTruncate(type, 12) << reset() << " "
                    << PadOrTruncate(details, static_cast<size_t>(std::max(10, width - 26))) << "\n";
                linesRendered++;
            }
        } else if (state.monitorStream.is_object() && (state.monitorStream.contains("output") || state.monitorStream.contains("lines"))) {
            out << bold(" Recent Output Log:") << "\n";
            linesRendered++;
            std::string outText = state.monitorStream.value("output", "");
            std::vector<std::string> lines;
            if (!outText.empty()) {
                std::istringstream iss(outText);
                std::string l;
                while (std::getline(iss, l)) {
                    if (!l.empty() && l.back() == '\r') l.pop_back();
                    lines.push_back(l);
                }
            } else if (state.monitorStream.contains("lines") && state.monitorStream["lines"].is_array()) {
                for (const auto& l : state.monitorStream["lines"]) {
                    lines.push_back(l.is_string() ? l.get<std::string>() : l.dump());
                }
            }
            int maxLogRows = std::max(2, height - linesRendered - 3);
            int startIdx = std::max(0, static_cast<int>(lines.size()) - maxLogRows);
            for (size_t i = static_cast<size_t>(startIdx); i < lines.size(); ++i) {
                out << "   " << PadOrTruncate(lines[i], static_cast<size_t>(std::max(10, width - 6))) << "\n";
                linesRendered++;
            }
        } else {
            out << "   (No agent events recorded yet)\n";
            linesRendered++;
        }
    } else if (state.activeTab == TopTab::Diagnostics) {
        // Tab 4: Failure Diagnostics (Why)
        out << bold(" Failure Diagnostics (Why)") << "   ";
        std::string diagSummary;
        if (state.diagBreakdown.is_object() && state.diagBreakdown.contains("summary")) {
            diagSummary = state.diagBreakdown.value("summary", "");
        }
        if (!diagSummary.empty()) {
            out << "Categories: " << code("\x1b[33m") << diagSummary << reset() << "\n";
        } else {
            out << "Categories: none\n";
        }
        linesRendered++;

        out << " " << bold("  JOB ID         QUEUE             FAIL CLASS     ACTION / SUMMARY") << "\n";
        out << code("\x1b[90m") << " -------------- ----------------- -------------- -------------------------------------------------" << reset() << "\n";
        linesRendered += 2;

        if (state.failedJobsData.contains("jobs") && state.failedJobsData["jobs"].is_array() &&
            !state.failedJobsData["jobs"].empty()) {
            const auto& fjobs = state.failedJobsData["jobs"];
            int maxRows = std::max(2, height - linesRendered - 10);
            int count = std::min(static_cast<int>(fjobs.size()), maxRows);

            for (int i = 0; i < count; ++i) {
                const auto& fj = fjobs[static_cast<size_t>(i)];
                std::string jid = fj.value("id", fj.value("key", "-"));
                std::string rawQueue = fj.value("taskqueue", fj.value("queue", "default"));
                std::string host = GetJobHost(fj);
                std::string queueCol = FormatQueueColumn(rawQueue, host);
                std::string failClass = "unknown";
                std::string action = fj.value("summary", "");
                if (fj.contains("attempts") && fj["attempts"].is_array() && !fj["attempts"].empty()) {
                    const auto& lastAtt = fj["attempts"].back();
                    if (lastAtt.contains("failure") && lastAtt["failure"].is_object()) {
                        failClass = lastAtt["failure"].value("class", failClass);
                        if (lastAtt["failure"].contains("suggestedAction")) {
                            action = lastAtt["failure"].value("suggestedAction", action);
                        }
                    }
                }

                bool isSel = (i == state.selectedRow);
                std::ostringstream row;
                row << (isSel ? ">" : " ") << " "
                    << PadOrTruncate(jid, 14) << " "
                    << PadOrTruncate(queueCol, 17) << " "
                    << PadOrTruncate(failClass, 14) << " "
                    << PadOrTruncate(action, static_cast<size_t>(std::max(10, width - 52)));

                if (isSel && useColor) {
                    out << "\x1b[1;31m" << row.str() << "\x1b[0m\n";
                } else {
                    out << row.str() << "\n";
                }
                linesRendered++;
            }

            // Full taskqueue why diagnosis detail box
            nlohmann::json jobObj;
            if (state.diagJobDetail.is_object() && state.diagJobDetail.contains("job") && state.diagJobDetail["job"].is_object()) {
                jobObj = state.diagJobDetail["job"];
            } else if (state.diagJobDetail.is_object() && state.diagJobDetail.contains("id")) {
                jobObj = state.diagJobDetail;
            } else if (state.selectedRow >= 0 && state.selectedRow < static_cast<int>(fjobs.size())) {
                jobObj = fjobs[static_cast<size_t>(state.selectedRow)];
            }

            if (jobObj.is_object() && !jobObj.empty()) {
                std::string selJid = jobObj.value("id", jobObj.value("key", "-"));
                std::string name = jobObj.value("name", "");
                std::string rawQueue = jobObj.value("taskqueue", jobObj.value("queue", "default"));
                std::string host = GetJobHost(jobObj);
                std::string queueCol = FormatQueueColumn(rawQueue, host);
                std::string st = jobObj.value("state", "failed");

                nlohmann::json lastFailure;
                nlohmann::json history = nlohmann::json::array();
                if (jobObj.contains("attempts") && jobObj["attempts"].is_array()) {
                    for (size_t ai = 0; ai < jobObj["attempts"].size(); ++ai) {
                        const auto& a = jobObj["attempts"][ai];
                        if (!a.is_object()) continue;
                        if (a.contains("failure") && a["failure"].is_object() && !a["failure"].empty()) {
                            nlohmann::json h;
                            h["attempt"] = a.value("attempt", a.value("n", static_cast<int>(ai) + 1));
                            h["class"] = a["failure"].value("class", "");
                            history.push_back(h);
                            lastFailure = a["failure"];
                        }
                    }
                } else if (state.diagJobDetail.contains("failure") && state.diagJobDetail["failure"].is_object()) {
                    lastFailure = state.diagJobDetail["failure"];
                }

                out << code("\x1b[90m") << divLine << reset() << "\n";
                linesRendered++;
                out << bold("[Failure Diagnosis: " + selJid + "]")
                    << " Job " << selJid;
                if (!name.empty() && name != selJid) out << " (" << name << ")";
                out << " [" << colorState(st) << "] queue=" << queueCol;
                if (!host.empty()) out << " host=" << host;
                out << "\n";
                linesRendered++;

                if (lastFailure.is_object() && !lastFailure.empty()) {
                    std::string fc = lastFailure.value("class", "unknown");
                    std::string conf = lastFailure.value("confidence", "");
                    std::string summ = lastFailure.value("summary", "");
                    std::string act = lastFailure.value("suggestedAction", "");
                    std::string retryAt = lastFailure.value("retryAt", "");

                    out << "   " << bold("Why: ") << code("\x1b[31;1m") << fc << reset();
                    if (!conf.empty()) out << " (" << conf << ")";
                    out << "\n";
                    linesRendered++;
                    if (!summ.empty()) {
                        out << "     " << PadOrTruncate(summ, static_cast<size_t>(std::max(10, width - 8))) << "\n";
                        linesRendered++;
                    }
                    if (!act.empty()) {
                        out << "   " << bold("Next: ") << act;
                        if (!retryAt.empty()) out << " (retry at " << retryAt << ")";
                        out << "\n";
                        linesRendered++;
                    }

                    if (lastFailure.contains("lastActivity") && lastFailure["lastActivity"].is_object()) {
                        const auto& lact = lastFailure["lastActivity"];
                        std::string actDesc = lact.value("activity", "");
                        std::string target = lact.value("target", "");
                        int64_t sinceSec = lact.value("sinceSec", 0);
                        out << "   " << bold("Last activity: ") << actDesc;
                        if (!target.empty()) out << " " << target;
                        if (sinceSec > 0) out << " (" << sinceSec << "s ago)";
                        out << "\n";
                        linesRendered++;
                    }

                    if (lastFailure.contains("llmGuess") && lastFailure["llmGuess"].is_object()) {
                        const auto& lg = lastFailure["llmGuess"];
                        std::string gClass = lg.value("class", "");
                        std::string gSumm = lg.value("summary", "");
                        std::string gAct = lg.value("suggestedAction", "");
                        if (!gClass.empty()) {
                            out << "   " << bold("LLM guess: ") << gClass;
                            if (!gSumm.empty()) out << " - " << gSumm;
                            if (!gAct.empty()) out << " -> " << gAct;
                            out << "\n";
                            linesRendered++;
                        }
                    }

                    if (!history.empty()) {
                        out << "   " << bold("Attempt classes:");
                        for (const auto& h : history) {
                            out << " " << h.value("attempt", 0) << "=" << h.value("class", "");
                        }
                        out << "\n";
                        linesRendered++;
                    }
                } else {
                    out << "   No detailed failure record found for this job.\n";
                    linesRendered++;
                }
            }
        } else {
            out << "   (No failed jobs recorded)\n";
            linesRendered++;
        }
    } else if (state.activeTab == TopTab::Memory) {
        // Tab 5: Memory & Knowledge Records View
        out << bold(" Memory & Knowledge Records") << "\n";
        linesRendered++;

        // Storage statistics summary bar
        if (state.memoryStats.is_object() && state.memoryStats.value("ok", false)) {
            int64_t totalBytes = state.memoryStats.value("total_bytes", 0LL);
            int64_t limitBytes = state.memoryStats.value("limit_bytes", 0LL);
            int64_t l1Bytes = state.memoryStats.value("l1_bytes", 0LL);
            int64_t l2Bytes = state.memoryStats.value("l2_bytes", 0LL);
            int64_t l3Bytes = state.memoryStats.value("l3_bytes", 0LL);
            std::string warning = state.memoryStats.value("warning", "");

            int pct = limitBytes > 0 ? static_cast<int>((totalBytes * 100) / limitBytes) : 0;
            out << " Storage: " << FormatBytes(totalBytes) << " / " << (limitBytes > 0 ? FormatBytes(limitBytes) : "Unlimited")
                << " (" << pct << "%)"
                << "  |  L1 (Runs): " << FormatBytes(l1Bytes)
                << "  |  L2 (Notes): " << FormatBytes(l2Bytes)
                << "  |  L3 (Knowledge): " << FormatBytes(l3Bytes)
                << "  |  Status: " << (warning.empty() ? code("\x1b[32mHealthy\x1b[0m") : code("\x1b[31;1m" + warning + "\x1b[0m")) << "\n";
        } else {
            out << " Storage: 0 B  |  L1: 0 B  |  L2: 0 B  |  L3: 0 B  |  Status: Healthy\n";
        }
        linesRendered++;

        out << " " << bold("  LAYER  ID                  JOB / QUEUE    STATUS     SUMMARY / TITLE") << "\n";
        out << code("\x1b[90m") << " ------- ------------------- -------------- ---------- ------------------------------------" << reset() << "\n";
        linesRendered += 2;

        nlohmann::json recordsArray;
        if (state.memoryRecords.is_object()) {
            if (state.memoryRecords.contains("results") && state.memoryRecords["results"].is_array()) {
                recordsArray = state.memoryRecords["results"];
            } else if (state.memoryRecords.contains("records") && state.memoryRecords["records"].is_array()) {
                recordsArray = state.memoryRecords["records"];
            }
        } else if (state.memoryRecords.is_array()) {
            recordsArray = state.memoryRecords;
        }

        if (!recordsArray.empty()) {
            int maxRows = std::max(2, height - linesRendered - 6);
            int count = std::min(static_cast<int>(recordsArray.size()), maxRows);

            for (int i = 0; i < count; ++i) {
                const auto& rec = recordsArray[static_cast<size_t>(i)];
                std::string kind = rec.value("kind", "l1");
                std::transform(kind.begin(), kind.end(), kind.begin(), [](unsigned char c){ return static_cast<char>(std::toupper(c)); });
                std::string kindTag = "[" + kind.substr(0, 2) + "]";
                std::string rid = rec.value("id", "-");
                std::string job = rec.value("jobName", rec.value("taskqueue", "-"));
                std::string status = rec.value("status", rec.value("outcome", "-"));
                std::string summary = rec.value("summary", rec.value("title", rec.value("text", "")));

                bool isSel = (i == state.selectedRow);
                std::ostringstream row;
                row << (isSel ? ">" : " ") << " "
                    << PadOrTruncate(kindTag, 7) << " "
                    << PadOrTruncate(rid, 19) << " "
                    << PadOrTruncate(job, 14) << " "
                    << PadOrTruncate(status, 10) << " "
                    << PadOrTruncate(summary, static_cast<size_t>(std::max(10, width - 60)));

                if (isSel && useColor) {
                    out << "\x1b[1;36m" << row.str() << "\x1b[0m\n";
                } else {
                    out << row.str() << "\n";
                }
                linesRendered++;
            }

            // Detail preview for selected memory record
            if (state.selectedRow >= 0 && state.selectedRow < static_cast<int>(recordsArray.size())) {
                const auto& sel = recordsArray[static_cast<size_t>(state.selectedRow)];
                std::string rid = sel.value("id", "-");
                std::string kind = sel.value("kind", "l1");
                std::string job = sel.value("jobName", sel.value("taskqueue", "-"));
                std::string status = sel.value("status", sel.value("outcome", "-"));
                std::string created = FormatTimeStr(sel.value("createdAt", sel.value("created", "")));
                std::string summary = sel.value("summary", sel.value("title", ""));
                std::string text = sel.value("text", sel.value("content", ""));

                out << code("\x1b[90m") << divLine << reset() << "\n";
                linesRendered++;
                out << bold("[Memory Record: " + rid + " (" + kind + ")]")
                    << "  Job: " << job
                    << "  Status: " << status
                    << "  Created: " << created << "\n";
                linesRendered++;
                if (!summary.empty()) {
                    out << "   Summary: " << PadOrTruncate(summary, static_cast<size_t>(std::max(10, width - 14))) << "\n";
                    linesRendered++;
                }
                if (!text.empty() && text != summary) {
                    out << "   Content: " << PadOrTruncate(text, static_cast<size_t>(std::max(10, width - 14))) << "\n";
                    linesRendered++;
                }
            }
        } else {
            out << "   (No memory records found)\n";
            linesRendered++;
        }
    }

    // Pad remaining height so footer stays pinned if needed
    while (linesRendered < height - 3) {
        out << "\n";
        linesRendered++;
    }

    // 4. Help Overlay (if requested)
    if (state.showHelp) {
        out << code("\x1b[33m")
            << "+----------------- Hancho Top Keyboard Shortcuts -----------------+\n"
            << "|  Tab / Shift+Tab   Switch to next / previous view               |\n"
            << "|  1 .. 5            Switch directly to view (1-Proc .. 5-Mem)    |\n"
            << "|  Up / Down (k / j) Navigate / select item in active view        |\n"
            << "|  Left / Right      Switch monitored process (Monitor tab)       |\n"
            << "|  s / S             Stop selected process or job                 |\n"
            << "|  r / R / e         Retry / rerun selected job                   |\n"
            << "|  c / C / x         Cancel selected job                          |\n"
            << "|  Space / u         Force immediate refresh                      |\n"
            << "|  ? / h             Toggle this help overlay                     |\n"
            << "|  Esc               Close help overlay                           |\n"
            << "|  q / Q / Ctrl+C    Quit hancho-cli top                          |\n"
            << "+-----------------------------------------------------------------+"
            << reset() << "\n";
    }

    // 5. Footer Line
    out << code("\x1b[90m") << divLine << reset() << "\n";
    if (!state.statusMessage.empty()) {
        out << code("\x1b[33m") << state.statusMessage << reset() << "\n";
    } else {
        if (state.activeTab == TopTab::Queue || state.activeTab == TopTab::Diagnostics) {
            out << "[Tab/1-5] View  [Up/Down] Select  [s] Stop  [r] Retry  [c] Cancel  [Space] Refresh  [?] Help  [q] Quit\n";
        } else if (state.activeTab == TopTab::Processes) {
            out << "[Tab/1-5] View  [Up/Down] Select  [s] Stop  [Space/r] Refresh  [?] Help  [q] Quit\n";
        } else if (state.activeTab == TopTab::Monitor) {
            out << "[Tab/1-5] View  [Left/Right] Switch Proc  [s] Stop  [Space/r] Refresh  [?] Help  [q] Quit\n";
        } else {
            out << "[Tab/1-5] View  [Up/Down] Select  [Space/r] Refresh  [?] Help  [q] Quit\n";
        }
    }

    return out.str();
}

// Controller function running the TUI loop
inline int RunTopLoop(const std::vector<std::string>& args, bool jsonOutput) {
    int intervalMs = 1000;
    bool once = false;

    for (size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--once") {
            once = true;
        } else if ((args[i] == "--interval" || args[i] == "--interval-ms") && i + 1 < args.size()) {
            try { intervalMs = std::max(100, std::stoi(args[++i])); } catch (...) {}
        } else if (!args[i].empty() && args[i][0] != '-') {
            // Accept an ignored queue name so `top main` works like the other commands.
        } else {
            std::cerr << "Error: unknown option for top: " << args[i]
                      << " (use: hancho-cli top [--once] [--interval <ms>])\n";
            return 1;
        }
    }

    TopState state;
    state.intervalMs = intervalMs;

    auto fetchState = [&]() {
        state.timestamp = GetCurrentTimestampStr();

        // 1. Process list (always fetch)
        std::string respList, errList;
        if (SendIpcRequest("{\"action\":\"list\"}", respList, errList, false)) {
            state.coreConnected = true;
            state.processData = nlohmann::json::parse(respList, nullptr, false);
            if (state.processData.is_discarded() || !state.processData.value("ok", false)) {
                state.coreConnected = false;
            }
        } else {
            state.coreConnected = false;
            state.processData = nlohmann::json::object();
        }

        // Merge remote processes from registered hosts
        if (state.coreConnected) {
            std::string respRemP, errRemP;
            if (SendIpcRequest("{\"action\":\"remote_processes\"}", respRemP, errRemP, false)) {
                nlohmann::json remData = nlohmann::json::parse(respRemP, nullptr, false);
                MergeRemoteProcesses(state.processData["processes"], remData);
            }
        }

        // 2. Taskqueue counts (fast summary)
        std::string respCounts, errCounts;
        if (SendIpcRequest("{\"action\":\"taskqueue_counts\"}", respCounts, errCounts, false)) {
            state.countsData = nlohmann::json::parse(respCounts, nullptr, false);
        }

        // Tab-specific fetches:
        if (state.activeTab == TopTab::Queue) {
            std::string respQ, errQ;
            if (SendIpcRequest("{\"action\":\"taskqueue_list\"}", respQ, errQ, false)) {
                state.queueData = nlohmann::json::parse(respQ, nullptr, false);
            }
            std::string respRemQ, errRemQ;
            if (SendIpcRequest("{\"action\":\"remote_taskqueues\"}", respRemQ, errRemQ, false)) {
                nlohmann::json remData = nlohmann::json::parse(respRemQ, nullptr, false);
                MergeRemoteJobs(state.queueData["jobs"], remData);
            }
        } else if (state.activeTab == TopTab::Monitor) {
            int monId = 0;
            if (state.processData.contains("processes") && state.processData["processes"].is_array() &&
                !state.processData["processes"].empty()) {
                const auto& procs = state.processData["processes"];
                if (state.monitorProcIndex < 0) state.monitorProcIndex = 0;
                if (state.monitorProcIndex >= static_cast<int>(procs.size())) {
                    state.monitorProcIndex = static_cast<int>(procs.size()) - 1;
                }
                monId = procs[static_cast<size_t>(state.monitorProcIndex)].value("id", 0);
            }
            if (monId > 0) {
                std::string respState, errState;
                std::string stReq = "{\"action\":\"get_agent_state\",\"id\":" + std::to_string(monId) + "}";
                if (SendIpcRequest(stReq, respState, errState, false)) {
                    state.monitorState = nlohmann::json::parse(respState, nullptr, false);
                }
                std::string respStream, errStream;
                std::string streamReq = "{\"action\":\"get_agent_stream\",\"id\":" + std::to_string(monId) + ",\"schema\":\"v1\",\"tail\":25}";
                if (SendIpcRequest(streamReq, respStream, errStream, false)) {
                    state.monitorStream = nlohmann::json::parse(respStream, nullptr, false);
                }
                if (!state.monitorStream.is_object() || !state.monitorStream.value("ok", false) ||
                    !state.monitorStream.contains("events") || state.monitorStream["events"].empty()) {
                    std::string respLogs, errLogs;
                    std::string logReq = "{\"action\":\"logs\",\"id\":" + std::to_string(monId) + ",\"tail\":25}";
                    if (SendIpcRequest(logReq, respLogs, errLogs, false)) {
                        state.monitorStream = nlohmann::json::parse(respLogs, nullptr, false);
                    }
                }
            }
        } else if (state.activeTab == TopTab::Diagnostics) {
            std::string respBreak, errBreak;
            if (SendIpcRequest("{\"action\":\"taskqueue_diagnose\",\"breakdown\":true}", respBreak, errBreak, false)) {
                state.diagBreakdown = nlohmann::json::parse(respBreak, nullptr, false);
            }
            std::string respFailed, errFailed;
            if (SendIpcRequest("{\"action\":\"taskqueue_list\",\"state\":\"failed\"}", respFailed, errFailed, false)) {
                state.failedJobsData = nlohmann::json::parse(respFailed, nullptr, false);
            }
            std::string respRemF, errRemF;
            if (SendIpcRequest("{\"action\":\"remote_taskqueues\",\"state\":\"failed\"}", respRemF, errRemF, false)) {
                nlohmann::json remData = nlohmann::json::parse(respRemF, nullptr, false);
                MergeRemoteJobs(state.failedJobsData["jobs"], remData);
            }
            std::string selJobId;
            if (state.failedJobsData.contains("jobs") && state.failedJobsData["jobs"].is_array() &&
                !state.failedJobsData["jobs"].empty()) {
                const auto& jobs = state.failedJobsData["jobs"];
                if (state.selectedRow >= 0 && state.selectedRow < static_cast<int>(jobs.size())) {
                    selJobId = jobs[static_cast<size_t>(state.selectedRow)].value("id", "");
                }
            }
            if (!selJobId.empty()) {
                nlohmann::json req;
                req["action"] = "taskqueue_get";
                req["id"] = selJobId;
                req["include_instructions"] = false;
                std::string respWhy, errWhy;
                if (SendIpcRequest(req.dump(), respWhy, errWhy, false)) {
                    state.diagJobDetail = nlohmann::json::parse(respWhy, nullptr, false);
                }
            }
        } else if (state.activeTab == TopTab::Memory) {
            std::string respStats, errStats;
            if (SendIpcRequest("{\"action\":\"memory_storage_stats\"}", respStats, errStats, false)) {
                state.memoryStats = nlohmann::json::parse(respStats, nullptr, false);
            }
            std::string respSearch, errSearch;
            if (SendIpcRequest("{\"action\":\"memory_search\",\"text\":\"\",\"kinds\":[\"l1\",\"l2\",\"l3\"],\"limit\":30}", respSearch, errSearch, false)) {
                state.memoryRecords = nlohmann::json::parse(respSearch, nullptr, false);
            }
        }
    };

    // In JSON mode, output raw JSON and exit / poll
    if (jsonOutput) {
        while (true) {
            std::string resp, err;
            if (!SendIpcRequest("{\"action\":\"list\"}", resp, err, true)) {
                std::cerr << "Error: " << err << "\n";
                return 1;
            }
            std::cout << resp << "\n";
            if (once) return 0;
            SleepMs(static_cast<uint32_t>(intervalMs));
        }
    }

    // In --once non-interactive mode, fetch once, render without raw mode or alternate buffer
    if (once) {
        fetchState();
        int cols = 80, rows = 24;
        GetTerminalSize(cols, rows);
        std::cout << RenderScreen(state, cols, rows, false);
        return 0;
    }

    // Interactive TUI session
    TerminalSession session(true);
    fetchState();

    bool running = true;
    while (running) {
        int cols = 80, rows = 24;
        GetTerminalSize(cols, rows);

        // Move cursor to top-left and redraw
        std::cout << "\x1b[H" << RenderScreen(state, cols, rows, true) << std::flush;

        TopKey key = session.PollKey(intervalMs);
        switch (key) {
            case TopKey::Quit:
                running = false;
                break;
            case TopKey::Refresh:
                fetchState();
                state.statusMessage = "Refreshed.";
                break;
            case TopKey::JobStop: {
                std::string req, desc;
                if (BuildJobStopRequest(state, req, desc)) {
                    std::string resp, err;
                    if (SendIpcRequest(req, resp, err, false)) {
                        state.statusMessage = "Stopped " + desc + ".";
                    } else {
                        state.statusMessage = "Failed to stop " + desc + ": " + err;
                    }
                    fetchState();
                } else {
                    state.statusMessage = "No stoppable process or job selected.";
                }
                break;
            }
            case TopKey::JobRetry: {
                std::string req, desc;
                if (BuildJobRetryRequest(state, req, desc)) {
                    std::string resp, err;
                    if (SendIpcRequest(req, resp, err, false)) {
                        state.statusMessage = "Retried " + desc + ".";
                    } else {
                        state.statusMessage = "Failed to retry " + desc + ": " + err;
                    }
                    fetchState();
                } else {
                    fetchState();
                    state.statusMessage = "Refreshed.";
                }
                break;
            }
            case TopKey::JobCancel: {
                std::string req, desc;
                if (BuildJobCancelRequest(state, req, desc)) {
                    std::string resp, err;
                    if (SendIpcRequest(req, resp, err, false)) {
                        state.statusMessage = "Cancelled " + desc + ".";
                    } else {
                        state.statusMessage = "Failed to cancel " + desc + ": " + err;
                    }
                    fetchState();
                } else {
                    state.statusMessage = "No cancellable job selected (select a job in Queue or Why tab).";
                }
                break;
            }
            case TopKey::NextTab:
                state.NextTab();
                state.statusMessage.clear();
                fetchState();
                break;
            case TopKey::PrevTab:
                state.PrevTab();
                state.statusMessage.clear();
                fetchState();
                break;
            case TopKey::TabProcesses:
                state.SetTab(TopTab::Processes);
                state.statusMessage.clear();
                fetchState();
                break;
            case TopKey::TabQueue:
                state.SetTab(TopTab::Queue);
                state.statusMessage.clear();
                fetchState();
                break;
            case TopKey::TabMonitor:
                state.SetTab(TopTab::Monitor);
                state.statusMessage.clear();
                fetchState();
                break;
            case TopKey::TabDiagnostics:
                state.SetTab(TopTab::Diagnostics);
                state.statusMessage.clear();
                fetchState();
                break;
            case TopKey::TabMemory:
                state.SetTab(TopTab::Memory);
                state.statusMessage.clear();
                fetchState();
                break;
            case TopKey::Up:
                if (state.selectedRow > 0) {
                    state.selectedRow--;
                    if (state.activeTab == TopTab::Diagnostics) {
                        fetchState();
                    }
                }
                break;
            case TopKey::Down: {
                int maxRow = 0;
                if (state.activeTab == TopTab::Processes && state.processData.contains("processes") && state.processData["processes"].is_array()) {
                    maxRow = static_cast<int>(state.processData["processes"].size()) - 1;
                } else if (state.activeTab == TopTab::Queue && state.queueData.contains("jobs") && state.queueData["jobs"].is_array()) {
                    maxRow = static_cast<int>(state.queueData["jobs"].size()) - 1;
                } else if (state.activeTab == TopTab::Diagnostics && state.failedJobsData.contains("jobs") && state.failedJobsData["jobs"].is_array()) {
                    maxRow = static_cast<int>(state.failedJobsData["jobs"].size()) - 1;
                } else if (state.activeTab == TopTab::Memory) {
                    if (state.memoryRecords.contains("results") && state.memoryRecords["results"].is_array()) {
                        maxRow = static_cast<int>(state.memoryRecords["results"].size()) - 1;
                    } else if (state.memoryRecords.contains("records") && state.memoryRecords["records"].is_array()) {
                        maxRow = static_cast<int>(state.memoryRecords["records"].size()) - 1;
                    }
                }
                if (state.selectedRow < maxRow) {
                    state.selectedRow++;
                    if (state.activeTab == TopTab::Diagnostics) {
                        fetchState();
                    }
                }
                break;
            }
            case TopKey::Left:
                if (state.activeTab == TopTab::Monitor && state.monitorProcIndex > 0) {
                    state.monitorProcIndex--;
                    fetchState();
                }
                break;
            case TopKey::Right: {
                if (state.activeTab == TopTab::Monitor) {
                    int maxProc = 0;
                    if (state.processData.contains("processes") && state.processData["processes"].is_array()) {
                        maxProc = static_cast<int>(state.processData["processes"].size()) - 1;
                    }
                    if (state.monitorProcIndex < maxProc) {
                        state.monitorProcIndex++;
                        fetchState();
                    }
                }
                break;
            }
            case TopKey::Help:
                state.showHelp = !state.showHelp;
                break;
            case TopKey::Escape:
                if (state.showHelp) state.showHelp = false;
                break;
            case TopKey::None:
                // Interval elapsed: periodic background refresh
                fetchState();
                state.statusMessage.clear();
                break;
            default:
                break;
        }
    }

    session.Restore();
    return 0;
}

} // namespace pc::top
