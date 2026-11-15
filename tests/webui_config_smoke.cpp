// Headless regression test for the dashboard freeze when the WebUI is switched
// off (src/ipc_server.cpp config_set + src/ipc_server_remote.cpp restart worker).
//
// The test keeps a WebSocket connection to the WebUI open and runs a request
// that blocks inside the core, then turns the WebUI off with config_set. The
// answer of config_set must come back immediately: the caller (the dashboard
// settings dialog) waits for it and used to freeze for up to 30 seconds while
// the core joined the previous restart thread.
//
// Build: cmake --build build --config Release --target webui_config_smoke
// Run:   build\Release\webui_config_smoke.exe

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "common.h"
#include "ipc_server.h"
#include "nlohmann/json.hpp"
#include "process_manager.h"
#include "remote_client.h"
#include "taskqueue.h"
#include "remote_ws_test.h"
#include "webui_server.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

using namespace pc;
using json = nlohmann::json;

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

std::string ReadFirstLine(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    std::string line;
    if (in) std::getline(in, line);
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    return line;
}

// True when a TCP connection to the port can be established.
bool CanConnect(uint16_t port) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return false;
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
    const bool ok = ::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != SOCKET_ERROR;
    closesocket(s);
    return ok;
}

int64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

} // namespace

int main() {
    wchar_t tempBase[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempBase);
    std::wstring tempDirW = std::wstring(tempBase) + L"pc_webui_config_smoke_" +
                            std::to_wstring(GetCurrentProcessId());
    std::error_code ec;
    std::filesystem::remove_all(tempDirW, ec);
    std::filesystem::create_directories(tempDirW, ec);
    SetDataDirectoryOverride(tempDirW);

    // A port that is unlikely to clash with a running core.
    const uint16_t webuiPort =
        static_cast<uint16_t>(20000 + (GetCurrentProcessId() % 20000));

    std::wstring configPath = PathJoinWide(tempDirW, L"config.json");
    {
        std::string cfg = "{\"webui\":{\"enabled\":true,\"scope\":\"loopback\",\"host\":\"127.0.0.1\",\"port\":" +
                          std::to_string(webuiPort) + "}}";
        WriteFileAtomicWide(configPath, cfg);
    }

    std::wstring tokenFile = PathJoinWide(tempDirW, L".webui_token");
    std::wstring portFile = PathJoinWide(tempDirW, L".webui_port");

    ProcessManager pm;
    TaskQueueManager tqm;
    tqm.SetProcessManager(&pm);
    pm.SetProcessFinishedCallback([&tqm](int pid, int exitCode, const std::string& kind) {
        tqm.OnProcessFinished(pid, exitCode, kind);
    });

    IpcServer ipc(pm, tqm);

    std::printf("Running WebUI config smoke tests...\n");

    ipc.StartWebUiServer();
    Check(ipc.GetWebUiServer() != nullptr && ipc.GetWebUiServer()->IsRunning(),
          "WebUI server is listening after StartWebUiServer");
    Check(PathExistsWide(portFile), ".webui_port exists while the WebUI is enabled");
    Check(PathExistsWide(tokenFile), ".webui_token exists while the WebUI is enabled");
    const std::string token = ReadFirstLine(tokenFile);
    Check(!token.empty(), "WebUI session token was written");

    // Keep a WebSocket connection open, like an open browser tab.
    remote::WebSocketClient client;
    std::string connectErr;
    const std::string endpoint = "ws://127.0.0.1:" + std::to_string(webuiPort);
    const bool connected = client.Connect(endpoint, token, 5000, &connectErr);
    Check(connected, "WebSocket connected to the WebUI: " + connectErr);

    // A request that blocks inside the core for a few seconds.
    json startResp = json::parse(
        ipc.HandleRequestJson(
            "{\"action\":\"start\",\"name\":\"slow\",\"cmd\":\"cmd /c ping -n 5 127.0.0.1 > nul\",\"open_stdin\":false}"),
        nullptr, false);
    Check(startResp.is_object() && startResp.value("ok", false), "slow process started");
    const int procId = startResp.is_object() ? startResp.value("id", 0) : 0;
    Check(procId > 0, "slow process has an id");

    // Fire the blocking request in the background; its answer is not needed.
    std::thread waiter([&client, procId]() {
        std::string resp;
        std::string err;
        client.SendRequest("{\"action\":\"wait\",\"id\":" + std::to_string(procId) +
                               ",\"wait_ms\":10000}",
                           resp, err, 12000);
    });
    waiter.detach();
    SleepMs(400); // let the request reach the core and block there

    // 1. Turning the WebUI off must answer right away.
    const int64_t start1 = NowMs();
    json offResp = json::parse(
        ipc.HandleRequestJson("{\"action\":\"config_set\",\"patch\":{\"webui\":{\"enabled\":false,\"scope\":\"loopback\",\"host\":\"127.0.0.1\",\"port\":" +
                              std::to_string(webuiPort) + "}}}"),
        nullptr, false);
    const int64_t elapsed1 = NowMs() - start1;
    Check(offResp.is_object() && offResp.value("ok", false), "config_set (webui off) answered");
    Check(elapsed1 < 2000,
          "config_set (webui off) answered in " + std::to_string(elapsed1) + " ms (limit 2000 ms)");

    // 2. A second change while the first restart is still running must not block
    //    either: this is what froze the settings dialog.
    const int64_t start2 = NowMs();
    json offResp2 = json::parse(
        ipc.HandleRequestJson("{\"action\":\"config_set\",\"patch\":{\"webui\":{\"enabled\":false,\"scope\":\"loopback\",\"host\":\"127.0.0.1\",\"port\":" +
                              std::to_string(webuiPort + 1) + "}}}"),
        nullptr, false);
    const int64_t elapsed2 = NowMs() - start2;
    Check(offResp2.is_object() && offResp2.value("ok", false), "second config_set (webui off) answered");
    Check(elapsed2 < 2000,
          "second config_set (webui off) answered in " + std::to_string(elapsed2) + " ms (limit 2000 ms)");

    // 3. The listener is closed and the discovery files are cleaned up.
    for (int i = 0; i < 100 && PathExistsWide(portFile); ++i) SleepMs(50);
    Check(!PathExistsWide(portFile), ".webui_port removed after the WebUI was switched off");
    Check(!PathExistsWide(tokenFile), ".webui_token removed after the WebUI was switched off");
    SleepMs(300);
    Check(!CanConnect(webuiPort), "the WebUI port is closed after the WebUI was switched off");

    // 4. Switching it back on brings the listener up again.
    const int64_t start3 = NowMs();
    json onResp = json::parse(
        ipc.HandleRequestJson("{\"action\":\"config_set\",\"patch\":{\"webui\":{\"enabled\":true,\"scope\":\"loopback\",\"host\":\"127.0.0.1\",\"port\":" +
                              std::to_string(webuiPort + 1) + "}}}"),
        nullptr, false);
    const int64_t elapsed3 = NowMs() - start3;
    Check(onResp.is_object() && onResp.value("ok", false), "config_set (webui on) answered");
    Check(elapsed3 < 2000,
          "config_set (webui on) answered in " + std::to_string(elapsed3) + " ms (limit 2000 ms)");

    bool listening = false;
    for (int i = 0; i < 100; ++i) {
        if (PathExistsWide(portFile) && CanConnect(webuiPort + 1)) {
            listening = true;
            break;
        }
        SleepMs(50);
    }
    Check(listening, "the WebUI listens again after it was switched back on");
    Check(PathExistsWide(portFile), ".webui_port exists again after the WebUI was switched on");

    client.Close();

    // 5. The same change sent over the WebSocket itself (a browser tab that
    //    turns the WebUI off) must be answered before the listener is closed.
    {
        const std::string token2 = ReadFirstLine(tokenFile);
        const std::string endpoint2 = "ws://127.0.0.1:" + std::to_string(webuiPort + 1);
        remote::WebSocketClient ws;
        std::string err;
        const bool conn2 = ws.Connect(endpoint2, token2, 5000, &err);
        Check(conn2, "WebSocket reconnected after the WebUI was switched on: " + err);
        if (conn2) {
            std::string resp;
            const int64_t start = NowMs();
            const bool ok = ws.SendRequest(
                "{\"action\":\"config_set\",\"patch\":{\"webui\":{\"enabled\":false,\"scope\":\"loopback\",\"host\":\"127.0.0.1\",\"port\":" +
                    std::to_string(webuiPort + 1) + "}}}",
                resp, err, 5000);
            const int64_t elapsed = NowMs() - start;
            Check(ok && resp.find("\"ok\":true") != std::string::npos,
                  "config_set over the WebSocket is answered: " + resp);
            Check(elapsed < 2000,
                  "config_set over the WebSocket answered in " + std::to_string(elapsed) + " ms (limit 2000 ms)");
        }
        ws.Close();
    }

    // The core waits for the detached request before it goes away.
    ipc.Stop();
    pm.SetProcessFinishedCallback(nullptr);
    tqm.ShutdownTaskQueueManager();

    std::filesystem::remove_all(tempDirW, ec);
    std::printf("\nWebUI config smoke tests finished: %d passed, %d failed\n", g_passes, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
