#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "wct_client.h"

#include "common.h"
#include "ipc_client.h"
#include "nlohmann/json.hpp"
#include "wcsp/remote_session.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <windows.h>

namespace pc {
namespace {

// Ctrl+] detaches: the socket closes without CLOSE, so the remote ConPTY job
// stays alive and can be reattached with `wct <job-id>`. Ctrl+\ ends the
// session: wct sends WCSP CLOSE, which terminates the remote job.
constexpr uint8_t kDetachKey = 0x1D; // Ctrl+]
constexpr uint8_t kCloseKey = 0x1C;  // Ctrl+\

enum class ExitRequest {
    None = 0,
    Close,   // Ctrl+\: terminate the remote session
    Detach,  // Ctrl+]: keep the remote session
};
constexpr int kDefaultRemotePort = 9139;

struct Endpoint {
    std::string host;
    int port = 0;
    std::string token;
};

std::mutex g_writeMutex;
HANDLE g_in = INVALID_HANDLE_VALUE;
HANDLE g_out = INVALID_HANDLE_VALUE;
DWORD g_origInMode = 0;
DWORD g_origOutMode = 0;
std::atomic<bool> g_stop{false};
std::atomic<ExitRequest> g_exitRequest{ExitRequest::None};

bool IsValidHandle(HANDLE h) {
    return h != nullptr && h != INVALID_HANDLE_VALUE;
}

void WriteBytes(const void* data, size_t len) {
    if (!data || len == 0 || !IsValidHandle(g_out)) return;
    const char* p = static_cast<const char*>(data);
    std::lock_guard<std::mutex> lock(g_writeMutex);
    while (len > 0) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(len, 64 * 1024));
        DWORD written = 0;
        if (!WriteFile(g_out, p, chunk, &written, nullptr) || written == 0) return;
        p += written;
        len -= written;
    }
}

void WriteText(const std::string& text) {
    WriteBytes(text.data(), text.size());
}

std::string EnvToken() {
    DWORD len = GetEnvironmentVariableA("HANCHO_TERM_TOKEN", nullptr, 0);
    if (len == 0) return {};
    std::string value(len, '\0');
    DWORD got = GetEnvironmentVariableA("HANCHO_TERM_TOKEN", value.data(), len);
    value.resize(got);
    return value;
}

// Puts the console into raw VT input/output mode and restores it on exit.
struct ConsoleGuard {
    bool active = false;
    HANDLE openedIn = INVALID_HANDLE_VALUE;
    HANDLE openedOut = INVALID_HANDLE_VALUE;

    bool Setup(std::string& error) {
        g_in = GetStdHandle(STD_INPUT_HANDLE);
        g_out = GetStdHandle(STD_OUTPUT_HANDLE);
        // A process started inside a ConPTY may inherit pipe handles from its
        // parent instead of the console handles. Open CONIN$/CONOUT$ in that
        // case so wct still talks to its attached console.
        if (!IsValidHandle(g_in) || !GetConsoleMode(g_in, &g_origInMode)) {
            openedIn = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
                                   FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, 0, nullptr);
            if (IsValidHandle(openedIn)) g_in = openedIn;
        }
        if (!IsValidHandle(g_out) || !GetConsoleMode(g_out, &g_origOutMode)) {
            openedOut = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                    OPEN_EXISTING, 0, nullptr);
            if (IsValidHandle(openedOut)) g_out = openedOut;
        }
        if (!IsValidHandle(g_in) || !IsValidHandle(g_out)) {
            CloseOpenedHandles();
            error = "wct must run inside a console (stdin/stdout are not available)";
            return false;
        }
        // GetConsoleMode is the reliable console check: ConPTY handles may not
        // report FILE_TYPE_CHAR even though they are real console handles.
        if (!GetConsoleMode(g_in, &g_origInMode) || !GetConsoleMode(g_out, &g_origOutMode)) {
            CloseOpenedHandles();
            error = "wct must run inside a console (stdin/stdout are redirected)";
            return false;
        }

        DWORD outMode = g_origOutMode | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
        if (!SetConsoleMode(g_out, outMode)) {
            CloseOpenedHandles();
            error = "this console does not support VT output";
            return false;
        }

        // Let conhost translate keys to VT sequences so they can be forwarded to
        // the remote ConPTY unchanged, and stop the console from turning Ctrl+C
        // into a signal (it belongs to the remote shell).
        DWORD inMode = g_origInMode;
        inMode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT |
                    ENABLE_MOUSE_INPUT | ENABLE_QUICK_EDIT_MODE | ENABLE_INSERT_MODE);
        inMode |= ENABLE_EXTENDED_FLAGS | ENABLE_VIRTUAL_TERMINAL_INPUT;
        if (!SetConsoleMode(g_in, inMode)) {
            SetConsoleMode(g_out, g_origOutMode);
            CloseOpenedHandles();
            error = "this console does not support VT input (Windows 10 1809 or later is required)";
            return false;
        }

        active = true;
        return true;
    }

    void CloseOpenedHandles() {
        if (openedIn != INVALID_HANDLE_VALUE) {
            HANDLE h = openedIn;
            openedIn = INVALID_HANDLE_VALUE;
            if (g_in == h) g_in = INVALID_HANDLE_VALUE;
            CloseHandle(h);
        }
        if (openedOut != INVALID_HANDLE_VALUE) {
            HANDLE h = openedOut;
            openedOut = INVALID_HANDLE_VALUE;
            if (g_out == h) g_out = INVALID_HANDLE_VALUE;
            CloseHandle(h);
        }
    }

    void Restore() {
        if (!active) return;
        WriteText("\x1b[0m\x1b[?25h\x1b[?1049l");
        SetConsoleMode(g_in, g_origInMode);
        SetConsoleMode(g_out, g_origOutMode);
        active = false;
        CloseOpenedHandles();
    }
};

BOOL WINAPI CtrlHandler(DWORD type) {
    switch (type) {
    case CTRL_C_EVENT:
        // Raw VT input forwards Ctrl+C to the remote shell; do not terminate.
        return TRUE;
    case CTRL_BREAK_EVENT:
        g_stop = true;
        return TRUE;
    case CTRL_CLOSE_EVENT:
    case CTRL_LOGOFF_EVENT:
    case CTRL_SHUTDOWN_EVENT:
        g_stop = true;
        // Best effort: leave the console usable if Windows is about to end us.
        WriteText("\x1b[0m\x1b[?25h\x1b[?1049l");
        if (g_in != INVALID_HANDLE_VALUE) SetConsoleMode(g_in, g_origInMode);
        if (g_out != INVALID_HANDLE_VALUE) SetConsoleMode(g_out, g_origOutMode);
        return TRUE;
    default:
        return FALSE;
    }
}

bool ResolveEndpoint(const WctOptions& options, Endpoint& endpoint, std::string& error) {
    const bool remote = !options.local && !options.host.empty();
    if (remote) {
        endpoint.host = options.host;
        endpoint.port = options.port > 0 ? options.port : kDefaultRemotePort;
        endpoint.token = options.token.empty() ? EnvToken() : options.token;
        if (endpoint.token.empty()) {
            error = "a WCSP token is required for --host (use --token or HANCHO_TERM_TOKEN)";
            return false;
        }
        return true;
    }

    std::string response;
    std::string ipcError;
    if (!SendIpcRequest("{\"action\":\"term_endpoint\"}", response, ipcError, false)) {
        error = ipcError.empty() ? "the Hancho core is not running" : ipcError;
        error += " (start it with `Hancho.exe --headless` or `Hancho.exe gui`)";
        return false;
    }

    nlohmann::json json = nlohmann::json::parse(response, nullptr, false);
    if (json.is_discarded() || !json.is_object() || !json.value("ok", false)) {
        if (json.is_object() && json.contains("error") && json["error"].is_string()) {
            error = json["error"].get<std::string>();
        } else {
            error = "the terminal server is not running";
        }
        return false;
    }

    endpoint.host = json.value("host", std::string("127.0.0.1"));
    endpoint.port = json.value("port", 0);
    endpoint.token = json.value("token", std::string());
    if (endpoint.port <= 0 || endpoint.port > 65535) {
        error = "the terminal server returned an invalid port";
        return false;
    }
    return true;
}

bool GetConsoleSize(int& cols, int& rows) {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!IsValidHandle(g_out) || !GetConsoleScreenBufferInfo(g_out, &info)) return false;
    cols = info.srWindow.Right - info.srWindow.Left + 1;
    rows = info.srWindow.Bottom - info.srWindow.Top + 1;
    if (cols < 2) cols = 2;
    if (rows < 1) rows = 1;
    return true;
}

// Wakes a ReadFile() blocked in the input thread so it can observe g_stop.
void WakeInputThread() {
    if (!IsValidHandle(g_in)) return;
    INPUT_RECORD record{};
    record.EventType = KEY_EVENT;
    record.Event.KeyEvent.bKeyDown = TRUE;
    record.Event.KeyEvent.wRepeatCount = 1;
    record.Event.KeyEvent.wVirtualKeyCode = VK_SPACE;
    record.Event.KeyEvent.uChar.UnicodeChar = L' ';
    DWORD written = 0;
    WriteConsoleInputW(g_in, &record, 1, &written);
}

std::unique_ptr<RemoteSession> MakeSession(const Endpoint& endpoint) {
    RemoteConfig config;
    config.host = endpoint.host;
    config.port = static_cast<uint16_t>(std::clamp(endpoint.port, 1, 65535));
    config.token = endpoint.token;
    config.features = wcs::kDefaultFeatures;
    config.connect_timeout_ms = 5000;
    return std::make_unique<RemoteSession>(std::move(config));
}

} // namespace

int RunWct(const WctOptions& options) {
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    Endpoint endpoint;
    std::string error;
    if (!ResolveEndpoint(options, endpoint, error)) {
        std::cerr << "wct: " << error << std::endl;
        return 1;
    }

    ConsoleGuard console;
    if (!console.Setup(error)) {
        std::cerr << "wct: " << error << std::endl;
        return 1;
    }
    struct RestoreOnExit {
        ConsoleGuard& guard;
        ~RestoreOnExit() { guard.Restore(); }
    } restoreOnExit{console};

    int cols = 80;
    int rows = 24;
    GetConsoleSize(cols, rows);

    g_stop = false;
    g_exitRequest = ExitRequest::None;
    std::mutex messageMutex;
    std::string connectionError;
    bool exited = false;
    int exitCode = 0;

    std::unique_ptr<RemoteSession> session = MakeSession(endpoint);
    RemoteSession* remote = session.get();

    remote->SetOutputSink([](const char* data, size_t len) {
        WriteBytes(data, len);
    });
    remote->onExit = [&](const wcs::ExitInfo& info) {
        std::lock_guard<std::mutex> lock(messageMutex);
        exited = true;
        exitCode = static_cast<int>(info.exit_code);
        g_stop = true;
    };
    remote->onError = [&](const wcs::ErrorInfo& info) {
        std::lock_guard<std::mutex> lock(messageMutex);
        connectionError = info.message;
        g_stop = true;
    };

    TerminalStartOptions start;
    start.shell = Utf8ToWide(options.target);
    start.cols = cols;
    start.rows = rows;
    start.codePage = CP_UTF8;

    std::wstring startError;
    if (!remote->Start(start, &startError)) {
        console.Restore();
        WriteText("wct: cannot connect to " + endpoint.host + ":" +
                  std::to_string(endpoint.port) + ": " + WideToUtf8(startError) + "\r\n");
        return 1;
    }

    // The remote replay arrives right after HELLO_ACK; print the hint first so it
    // stays in the local scrollback but never overlays a full-screen remote app.
    WriteText("\x1b[0m[wct] " + endpoint.host + ":" + std::to_string(endpoint.port) +
              " (" + options.target + ") - Ctrl+] detaches, Ctrl+\\ closes\r\n");

    std::thread input([&] {
        std::vector<char> buffer(4096);
        while (!g_stop) {
            DWORD read = 0;
            if (!ReadFile(g_in, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) break;
            if (read == 0) break;
            if (g_stop) break;

            const char* begin = buffer.data();
            size_t stopAt = static_cast<size_t>(read);
            for (size_t i = 0; i < read; ++i) {
                const uint8_t byte = static_cast<uint8_t>(begin[i]);
                if (byte == kCloseKey) {
                    g_exitRequest = ExitRequest::Close;
                    stopAt = i;
                    break;
                }
                if (byte == kDetachKey) {
                    g_exitRequest = ExitRequest::Detach;
                    stopAt = i;
                    break;
                }
            }
            if (stopAt > 0) remote->Write(begin, stopAt);
            if (stopAt < static_cast<size_t>(read)) {
                g_stop = true;
                break;
            }
        }
    });

    int lastCols = cols;
    int lastRows = rows;
    uint64_t lastSizeCheck = GetNowTickMs();

    while (!g_stop) {
        const uint64_t now = GetNowTickMs();
        if (now - lastSizeCheck >= 100) {
            lastSizeCheck = now;
            int newCols = 0;
            int newRows = 0;
            if (GetConsoleSize(newCols, newRows) &&
                (newCols != lastCols || newRows != lastRows)) {
                lastCols = newCols;
                lastRows = newRows;
                remote->Resize(newCols, newRows);
            }
        }
        if (!remote->IsRunning()) {
            // The receiver has stopped (EXIT/ERROR); the callback should have run,
            // but do not spin if it did not.
            g_stop = true;
        }
        SleepMs(20);
    }

    WakeInputThread();
    if (input.joinable()) input.join();
    if (IsValidHandle(g_in)) FlushConsoleInputBuffer(g_in);

    const ExitRequest request = g_exitRequest.load();
    if (request == ExitRequest::Detach) {
        // Plain disconnect: the server keeps the ConPTY job for a later attach.
        remote->Close();
    } else {
        // Ctrl+\ (or an abrupt wct exit) ends the session. TerminateSession
        // closes directly when the socket is already gone (EXIT/ERROR).
        remote->TerminateSession();
    }
    session.reset();

    std::string report;
    {
        std::lock_guard<std::mutex> lock(messageMutex);
        if (!connectionError.empty()) {
            report = "connection error: " + connectionError;
        } else if (exited) {
            report = "session exited (code " + std::to_string(exitCode) + ")";
        } else if (request == ExitRequest::Detach) {
            report = "detached (session kept running; reattach with wct <job-id>)";
        } else {
            report = "session closed";
        }
    }

    console.Restore();
    WriteText("\r\n[wct] " + report + "\r\n");
    return 0;
}

} // namespace pc
