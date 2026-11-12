// WebUI server and skeleton smoke test (Card 061b).
// Tests HTTP static asset delivery, token authorization, Host/Origin verification,
// and WebSocket multiplexed IPC with reconnect restoration.

#include "webui_server.h"
#include "webui_assets.h"
#include "common.h"
#include "nlohmann/json.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#define closesocket close
using SOCKET = int;
constexpr int INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <chrono>
#include <fstream>
#include <filesystem>
#include <iostream>

using namespace pc;

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

void InitSockets() {
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

std::string HttpExchange(uint16_t port, const std::string& rawRequest) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return "";

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(s);
        return "";
    }

    ::send(s, rawRequest.data(), static_cast<int>(rawRequest.size()), 0);

    std::string response;
    char buf[4096];
    while (true) {
        int n = ::recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        response.append(buf, n);
    }
    closesocket(s);
    return response;
}

std::string WebSocketHandshake(uint16_t port, const std::string& pathWithQuery,
                               const std::string& hostHeader, const std::string& originHeader,
                               SOCKET& outSocket) {
    outSocket = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (outSocket == INVALID_SOCKET) return "";

    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    if (::connect(outSocket, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        closesocket(outSocket);
        outSocket = INVALID_SOCKET;
        return "";
    }

    std::string req = "GET " + pathWithQuery + " HTTP/1.1\r\n";
    req += "Host: " + hostHeader + "\r\n";
    if (!originHeader.empty()) {
        req += "Origin: " + originHeader + "\r\n";
    }
    req += "Upgrade: websocket\r\n";
    req += "Connection: Upgrade\r\n";
    req += "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n";
    req += "Sec-WebSocket-Version: 13\r\n\r\n";

    ::send(outSocket, req.data(), static_cast<int>(req.size()), 0);

    std::string response;
    char buf[1024];
    while (response.find("\r\n\r\n") == std::string::npos) {
        int n = ::recv(outSocket, buf, sizeof(buf), 0);
        if (n <= 0) break;
        response.append(buf, n);
    }
    return response;
}

std::string SendWsFrame(SOCKET s, const std::string& payload) {
    // Encode simple unmasked text frame (client must mask in real WS, but server sends unmasked;
    // remote_client ReadWebSocketMessage accepts masked or unmasked).
    // Let's send masked frame: 0x81 (text, fin), mask bit 0x80 | len
    std::string frame;
    frame.push_back(static_cast<char>(0x81));
    uint8_t maskKey[4] = { 0x12, 0x34, 0x56, 0x78 };
    if (payload.size() < 126) {
        frame.push_back(static_cast<char>(0x80 | payload.size()));
    } else {
        frame.push_back(static_cast<char>(0x80 | 126));
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xff));
        frame.push_back(static_cast<char>(payload.size() & 0xff));
    }
    for (int i = 0; i < 4; ++i) frame.push_back(static_cast<char>(maskKey[i]));
    for (size_t i = 0; i < payload.size(); ++i) {
        frame.push_back(static_cast<char>(payload[i] ^ maskKey[i % 4]));
    }

    ::send(s, frame.data(), static_cast<int>(frame.size()), 0);

    // Read response frame
    std::string raw;
    char buf[4096];
    while (raw.size() < 2) {
        int n = ::recv(s, buf, sizeof(buf), 0);
        if (n <= 0) return "";
        raw.append(buf, n);
    }

    uint8_t b1 = static_cast<uint8_t>(raw[1]);
    size_t payloadLen = b1 & 0x7f;
    size_t headerBytes = 2;
    if (payloadLen == 126) {
        while (raw.size() < 4) {
            int n = ::recv(s, buf, sizeof(buf), 0);
            if (n <= 0) return "";
            raw.append(buf, n);
        }
        payloadLen = (static_cast<uint8_t>(raw[2]) << 8) | static_cast<uint8_t>(raw[3]);
        headerBytes = 4;
    }
    while (raw.size() < headerBytes + payloadLen) {
        int n = ::recv(s, buf, sizeof(buf), 0);
        if (n <= 0) break;
        raw.append(buf, n);
    }
    if (raw.size() >= headerBytes + payloadLen) {
        return raw.substr(headerBytes, payloadLen);
    }
    return "";
}

} // namespace

int main() {
    InitSockets();

    wchar_t tempBase[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempBase);
    std::wstring tempDirW = std::wstring(tempBase) + L"pc_webui_smoke_" +
                            std::to_wstring(GetCurrentProcessId());
    std::error_code ec;
    std::filesystem::remove_all(tempDirW, ec);
    std::filesystem::create_directories(tempDirW, ec);
    SetDataDirectoryOverride(tempDirW);

    std::printf("Running WebUI smoke tests...\n");

    // Mock IPC handler simulating core
    auto mockIpcHandler = [](const std::string& reqJson) -> std::string {
        nlohmann::json req = nlohmann::json::parse(reqJson, nullptr, false);
        std::string action = req.value("action", "");
        if (action == "taskqueue_list") {
            nlohmann::json resp;
            resp["ok"] = true;
            resp["jobs"] = nlohmann::json::array({
                {
                    {"key", "job_001"},
                    {"name", "build_job"},
                    {"commandLine", "cmake --build build"},
                    {"state", "completed"},
                    {"exitCode", 0},
                    {"durationSec", 4.2},
                    {"createdAt", "2026-11-11T12:00:00Z"},
                    {"updatedAt", "2026-11-11T12:00:04Z"}
                },
                {
                    {"key", "job_002"},
                    {"name", "test_runner"},
                    {"commandLine", "ctest"},
                    {"state", "running"},
                    {"durationSec", 1.5},
                    {"createdAt", "2026-11-11T12:00:05Z"},
                    {"updatedAt", "2026-11-11T12:00:06Z"}
                }
            });
            return resp.dump();
        }
        if (action == "list") {
            nlohmann::json resp;
            resp["ok"] = true;
            resp["processes"] = nlohmann::json::array({
                {
                    {"id", 1},
                    {"pid", 12345},
                    {"name", "worker_proc"},
                    {"command", "python worker.py"},
                    {"state", "running"},
                    {"cpu_percent", 1.5},
                    {"memory_bytes", 10485760},
                    {"exit_code", 0}
                }
            });
            return resp.dump();
        }
        if (action == "taskqueue_counts") {
            nlohmann::json resp;
            resp["ok"] = true;
            resp["counts"] = {
                {"total", 2}, {"running", 1}, {"waiting", 0}, {"pending", 0},
                {"blocked", 0}, {"succeeded", 1}, {"failed", 0}, {"cancelled", 0}
            };
            return resp.dump();
        }
        if (action == "get_agent_stream") {
            nlohmann::json resp;
            resp["ok"] = true;
            resp["schema"] = "hancho.event/1";
            resp["events"] = nlohmann::json::array({
                {
                    {"ts", 1700000000},
                    {"type", "tool_call"},
                    {"payload", {{"tool", "execute_task"}}}
                }
            });
            return resp.dump();
        }
        if (action == "get_agent_state") {
            nlohmann::json resp;
            resp["ok"] = true;
            resp["id"] = 1;
            resp["agent_type"] = "worker";
            resp["status"] = "running";
            resp["supervisor_goal"] = "complete task";
            return resp.dump();
        }
        if (action == "get_usage_history") {
            nlohmann::json resp;
            resp["ok"] = true;
            resp["points"] = nlohmann::json::array({
                {
                    {"epoch", 1700000000},
                    {"input", 120},
                    {"output", 80},
                    {"total", 200},
                    {"cost", 0.0015}
                }
            });
            return resp.dump();
        }
        if (action == "taskqueue_diagnose") {
            nlohmann::json resp;
            resp["ok"] = true;
            resp["classes"] = {
                {"exit_nonzero", 1},
                {"timeout", 1}
            };
            resp["summary"] = "exit_nonzero x1, timeout x1";
            return resp.dump();
        }
        if (action == "memory_storage_stats") {
            nlohmann::json resp;
            resp["ok"] = true;
            resp["total_bytes"] = 10240;
            resp["limit_bytes"] = 104857600;
            resp["l1_bytes"] = 8192;
            resp["l2_bytes"] = 1024;
            resp["l3_bytes"] = 1024;
            return resp.dump();
        }
        if (action == "memory_search") {
            nlohmann::json resp;
            resp["ok"] = true;
            resp["results"] = nlohmann::json::array({
                {
                    {"id", "note_001"},
                    {"kind", "l2"},
                    {"status", "active"},
                    {"summary", "Test note for webui"}
                }
            });
            return resp.dump();
        }
        return "{\"ok\":true}";
    };

    WebUiServer server(mockIpcHandler);
    uint16_t testPort = 19138;
    std::string testToken = "test_token_1234567890abcdef123456";
    std::string startError;

    Check(server.Start(testPort, testToken, "", &startError), "Server starts on port " + std::to_string(testPort));
    Check(server.IsRunning(), "Server reports running");
    Check(server.GetPort() == testPort, "Server port matches");
    Check(server.GetToken() == testToken, "Server token matches");

    std::wstring dataDir = GetEffectiveDataDirectoryWide();
    std::wstring tokenFile = PathJoinWide(dataDir, L".webui_token");
    std::wstring portFile = PathJoinWide(dataDir, L".webui_port");

    {
        std::ifstream tf(tokenFile);
        std::string tok;
        if (tf) std::getline(tf, tok);
        while (!tok.empty() && (tok.back() == '\r' || tok.back() == '\n')) tok.pop_back();
        Check(tok == testToken, "Token file written to data directory");
    }

    // 1. HTTP GET / without token -> 401 Unauthorized
    {
        std::string req = "GET / HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) + "\r\n\r\n";
        std::string resp = HttpExchange(testPort, req);
        Check(resp.find("401 Unauthorized") != std::string::npos, "GET / without token gives 401 Unauthorized");
    }

    // 2. HTTP GET /?token=<valid> -> 200 OK with HTML and Set-Cookie
    {
        std::string req = "GET /?token=" + testToken + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) + "\r\n\r\n";
        std::string resp = HttpExchange(testPort, req);
        Check(resp.find("200 OK") != std::string::npos, "GET / with valid token gives 200 OK");
        Check(resp.find("Hancho Dashboard") != std::string::npos, "Response contains Hancho Dashboard title");
        Check(resp.find("Set-Cookie: hancho_token=") != std::string::npos, "Response sets authentication cookie");
    }

    // 3. Static asset delivery: GET /style.css -> 200 OK
    {
        std::string req = "GET /style.css HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) + "\r\n\r\n";
        std::string resp = HttpExchange(testPort, req);
        Check(resp.find("200 OK") != std::string::npos, "GET /style.css gives 200 OK");
        Check(resp.find("text/css") != std::string::npos, "GET /style.css has text/css content type");
    }

    // 4. Static asset delivery: GET /app.js -> 200 OK
    {
        std::string req = "GET /app.js HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) + "\r\n\r\n";
        std::string resp = HttpExchange(testPort, req);
        Check(resp.find("200 OK") != std::string::npos, "GET /app.js gives 200 OK");
        Check(resp.find("application/javascript") != std::string::npos, "GET /app.js has javascript content type");
    }

    // 5. Host Header Verification: invalid host -> 403 Forbidden
    {
        std::string req = "GET /?token=" + testToken + " HTTP/1.1\r\nHost: evil.attacker.com\r\n\r\n";
        std::string resp = HttpExchange(testPort, req);
        Check(resp.find("403 Forbidden") != std::string::npos, "Invalid Host header gives 403 Forbidden");
    }

    // 6. Origin Header Verification: invalid foreign origin -> 403 Forbidden
    {
        std::string req = "GET /?token=" + testToken + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(testPort) + "\r\nOrigin: http://evil.attacker.com\r\n\r\n";
        std::string resp = HttpExchange(testPort, req);
        Check(resp.find("403 Forbidden") != std::string::npos, "Foreign Origin gives 403 Forbidden");
    }

    // 7. WebSocket Handshake & multiplexed query (1 screen job list)
    {
        SOCKET wsSock = INVALID_SOCKET;
        std::string hs = WebSocketHandshake(testPort, "/ws?token=" + testToken,
                                            "127.0.0.1:" + std::to_string(testPort),
                                            "http://127.0.0.1:" + std::to_string(testPort),
                                            wsSock);
        Check(hs.find("101 Switching Protocols") != std::string::npos, "WebSocket handshake succeeded");
        Check(wsSock != INVALID_SOCKET, "WebSocket connection active");

        if (wsSock != INVALID_SOCKET) {
            // Ping
            std::string pong = SendWsFrame(wsSock, "{\"action\":\"ping\"}");
            Check(pong.find("\"pong\":true") != std::string::npos, "WebSocket ping returns pong");

            // Query taskqueue_list
            std::string jobsResp = SendWsFrame(wsSock, "{\"action\":\"taskqueue_list\"}");
            Check(jobsResp.find("\"ok\":true") != std::string::npos, "taskqueue_list ok: true");
            Check(jobsResp.find("job_001") != std::string::npos, "taskqueue_list contains job_001");
            Check(jobsResp.find("build_job") != std::string::npos, "taskqueue_list contains build_job");

            // Close connection (simulating closing the browser tab)
            closesocket(wsSock);
        }
    }

    // 8. Durability & Reconnect verification:
    // Core is still running after tab was closed. Reopening the tab reconnects and fetches state!
    Check(server.IsRunning(), "Server is still running after browser tab was closed");
    {
        SOCKET wsSock2 = INVALID_SOCKET;
        std::string hs = WebSocketHandshake(testPort, "/ws?token=" + testToken,
                                            "127.0.0.1:" + std::to_string(testPort),
                                            "http://127.0.0.1:" + std::to_string(testPort),
                                            wsSock2);
        Check(hs.find("101 Switching Protocols") != std::string::npos, "Reconnected after tab closure");
        if (wsSock2 != INVALID_SOCKET) {
            std::string jobsResp = SendWsFrame(wsSock2, "{\"action\":\"taskqueue_list\"}");
            Check(jobsResp.find("job_002") != std::string::npos, "Restored state after reconnect: contains job_002");
            // 9. Verify 5 functional panes queries over WebSocket (Card 061c)
            // Pane 1: Processes list
            std::string procResp = SendWsFrame(wsSock2, "{\"action\":\"list\"}");
            Check(procResp.find("\"processes\"") != std::string::npos, "Pane 1: list returns processes array");
            Check(procResp.find("worker_proc") != std::string::npos, "Pane 1: contains worker_proc");

            // Pane 2: Task Queue counts
            std::string countsResp = SendWsFrame(wsSock2, "{\"action\":\"taskqueue_counts\"}");
            Check(countsResp.find("\"counts\"") != std::string::npos, "Pane 2: taskqueue_counts returns counts object");
            Check(countsResp.find("\"total\":2") != std::string::npos, "Pane 2: total jobs is 2");

            // Pane 3: Monitor pane (Agent state, stream, and usage history)
            std::string stateResp = SendWsFrame(wsSock2, "{\"action\":\"get_agent_state\",\"id\":1}");
            Check(stateResp.find("\"agent_type\":\"worker\"") != std::string::npos, "Pane 3: get_agent_state ok");
            std::string streamResp = SendWsFrame(wsSock2, "{\"action\":\"get_agent_stream\",\"id\":1,\"schema\":\"v1\"}");
            Check(streamResp.find("\"events\"") != std::string::npos, "Pane 3: get_agent_stream ok");
            std::string usageResp = SendWsFrame(wsSock2, "{\"action\":\"get_usage_history\",\"since_sec\":86400}");
            Check(usageResp.find("\"points\"") != std::string::npos, "Pane 3: get_usage_history ok");

            // Pane 4: Failure diagnosis breakdown
            std::string diagResp = SendWsFrame(wsSock2, "{\"action\":\"taskqueue_diagnose\",\"breakdown\":true}");
            Check(diagResp.find("\"classes\"") != std::string::npos, "Pane 4: taskqueue_diagnose breakdown ok");
            Check(diagResp.find("exit_nonzero") != std::string::npos, "Pane 4: breakdown contains exit_nonzero");

            // Pane 5: Memory storage stats & search
            std::string memStatsResp = SendWsFrame(wsSock2, "{\"action\":\"memory_storage_stats\"}");
            Check(memStatsResp.find("\"total_bytes\"") != std::string::npos, "Pane 5: memory_storage_stats ok");
            std::string memSearchResp = SendWsFrame(wsSock2, "{\"action\":\"memory_search\",\"text\":\"test\"}");
            Check(memSearchResp.find("\"results\"") != std::string::npos, "Pane 5: memory_search ok");
            Check(memSearchResp.find("note_001") != std::string::npos, "Pane 5: results contains note_001");

            closesocket(wsSock2);
        }
    }

    // Stop server and check cleanup
    server.Stop();
    Check(!server.IsRunning(), "Server stopped");
    Check(!PathExistsWide(tokenFile), ".webui_token file cleaned up on shutdown");
    Check(!PathExistsWide(portFile), ".webui_port file cleaned up on shutdown");

    // Config-driven exposure: enabled=false, LAN scope, and allowed hosts/origins.
    std::wstring configPath = PathJoinWide(tempDirW, L"config.json");
    auto writeConfig = [&](const std::string& text) {
        WriteFileAtomicWide(configPath, text);
    };

    {
        writeConfig(R"({"webui":{"enabled":false}})");
        WebUiServer disabledServer(mockIpcHandler);
        std::string err;
        Check(disabledServer.Start(testPort + 1, testToken, "", &err),
              "Start returns true when webui.enabled=false: " + err);
        Check(!disabledServer.IsRunning(), "webui.enabled=false does not listen");
        Check(!PathExistsWide(PathJoinWide(tempDirW, L".webui_token")),
              "disabled WebUI leaves no .webui_token");
    }

    {
        writeConfig(R"({"webui":{"enabled":true,"scope":"lan","host":"0.0.0.0","allowed_hosts":["hancho.example"],"allowed_origins":["http://hancho.example:19140"]}})");
        WebUiServer lanServer(mockIpcHandler);
        std::string err;
        Check(lanServer.Start(testPort + 2, testToken, "", &err),
              "LAN-scope server starts: " + err);
        Check(lanServer.IsRunning(), "LAN-scope server is running");

        auto get = [&](const std::string& host, const std::string& origin) {
            std::string req = "GET /?token=" + testToken + " HTTP/1.1\r\nHost: " + host + "\r\n";
            if (!origin.empty()) req += "Origin: " + origin + "\r\n";
            req += "\r\n";
            return HttpExchange(static_cast<uint16_t>(testPort + 2), req);
        };

        Check(get("hancho.example:19140", "").find("200 OK") != std::string::npos,
              "LAN scope accepts an allowed hostname");
        Check(get("evil.example:19140", "").find("403 Forbidden") != std::string::npos,
              "LAN scope rejects an unknown hostname");
        Check(get("192.168.1.50:19140", "").find("200 OK") != std::string::npos,
              "LAN scope accepts an IP literal Host");
        Check(get("hancho.example:19140", "http://hancho.example:19140").find("200 OK") != std::string::npos,
              "LAN scope accepts an allowed Origin");
        Check(get("hancho.example:19140", "http://evil.example").find("403 Forbidden") != std::string::npos,
              "LAN scope rejects a foreign Origin");
        lanServer.Stop();
    }

    {
        writeConfig(R"({"webui":{"enabled":true,"scope":"loopback"}})");
        WebUiServer loopServer(mockIpcHandler);
        std::string err;
        Check(loopServer.Start(testPort + 3, testToken, "", &err),
              "loopback-scope server starts: " + err);
        std::string req = "GET /?token=" + testToken +
                          " HTTP/1.1\r\nHost: evil.example:19141\r\n\r\n";
        std::string resp = HttpExchange(static_cast<uint16_t>(testPort + 3), req);
        Check(resp.find("403 Forbidden") != std::string::npos,
              "loopback scope rejects a non-loopback Host");
        loopServer.Stop();
    }

    std::filesystem::remove_all(tempDirW, ec);
    std::printf("\nWebUI smoke tests finished: %d passed, %d failed\n", g_passes, g_failures);
    return (g_failures == 0) ? 0 : 1;
}
