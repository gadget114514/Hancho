// End-to-end smoke test for wct.exe (src/wct_client.cpp).
//
// wct runs inside a local ConPTY and talks to a small in-process WCSP server.
// The server echoes INPUT back as OUTPUT, so the test checks the real console
// path (raw VT input, passthrough output, Ctrl+] detach, Ctrl+] twice close)
// without needing a Hancho core or a working remote shell.
//
// Usage:
//   wct_smoke --wct <wct.exe>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "wcsp/frame_reader.h"
#include "wcsp/protocol.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
    if (ok) {
        std::printf("PASS: %s\n", what);
    } else {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", what);
    }
}

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                   nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                        out.data(), need, nullptr, nullptr);
    return out;
}

class ConPtyHost {
public:
    ~ConPtyHost() { Close(); }

    bool Start(const std::wstring& command) {
        HANDLE inputRead = INVALID_HANDLE_VALUE;
        HANDLE outputWrite = INVALID_HANDLE_VALUE;
        if (!CreatePipe(&inputRead, &input_, nullptr, 0) ||
            !CreatePipe(&output_, &outputWrite, nullptr, 0)) {
            std::fprintf(stderr, "FAIL: CreatePipe: %lu\n", GetLastError());
            return false;
        }
        if (FAILED(CreatePseudoConsole(COORD{100, 30}, inputRead, outputWrite, 0, &pty_))) {
            std::fprintf(stderr, "FAIL: CreatePseudoConsole: %lu\n", GetLastError());
            return false;
        }
        CloseHandle(inputRead);
        CloseHandle(outputWrite);

        STARTUPINFOEXW siEx{};
        siEx.StartupInfo.cb = sizeof(siEx);
        // Force the child to take the pty's handles instead of copying this
        // process's (possibly redirected) standard handles.
        siEx.StartupInfo.dwFlags |= STARTF_USESTDHANDLES;
        siEx.StartupInfo.hStdInput = nullptr;
        siEx.StartupInfo.hStdOutput = nullptr;
        siEx.StartupInfo.hStdError = nullptr;

        SIZE_T attributeSize = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeSize);
        siEx.lpAttributeList = static_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
            HeapAlloc(GetProcessHeap(), 0, attributeSize));
        if (!siEx.lpAttributeList ||
            !InitializeProcThreadAttributeList(siEx.lpAttributeList, 1, 0, &attributeSize) ||
            !UpdateProcThreadAttribute(siEx.lpAttributeList, 0,
                                       PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
                                       pty_, sizeof(pty_), nullptr, nullptr)) {
            std::fprintf(stderr, "FAIL: attribute list: %lu\n", GetLastError());
            return false;
        }

        std::wstring mutableCommand = command;
        BOOL ok = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr,
                                 FALSE, EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                 &siEx.StartupInfo, &process_);
        DeleteProcThreadAttributeList(siEx.lpAttributeList);
        HeapFree(GetProcessHeap(), 0, siEx.lpAttributeList);
        if (!ok) {
            std::fprintf(stderr, "FAIL: CreateProcessW: %lu\n", GetLastError());
            return false;
        }

        reader_ = std::thread([this] {
            char buffer[4096];
            DWORD read = 0;
            while (ReadFile(output_, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
                std::lock_guard<std::mutex> lock(mutex_);
                text_.append(buffer, read);
                cv_.notify_all();
            }
        });
        return true;
    }

    bool WaitFor(const std::string& needle, int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs);
        std::unique_lock<std::mutex> lock(mutex_);
        while (true) {
            if (text_.find(needle) != std::string::npos) return true;
            if (cv_.wait_until(lock, deadline) == std::cv_status::timeout) {
                return text_.find(needle) != std::string::npos;
            }
        }
    }

    std::string Text() {
        std::lock_guard<std::mutex> lock(mutex_);
        return text_;
    }

    bool Write(const std::string& bytes) {
        DWORD written = 0;
        return WriteFile(input_, bytes.data(), static_cast<DWORD>(bytes.size()),
                         &written, nullptr) && written == bytes.size();
    }

    void Close() {
        if (process_.hProcess) {
            WaitForSingleObject(process_.hProcess, 10000);
            TerminateProcess(process_.hProcess, 0);
            CloseHandle(process_.hThread);
            CloseHandle(process_.hProcess);
            process_ = {};
        }
        if (pty_) {
            ClosePseudoConsole(pty_);
            pty_ = nullptr;
        }
        if (reader_.joinable()) reader_.join();
        if (input_ != INVALID_HANDLE_VALUE) CloseHandle(input_);
        if (output_ != INVALID_HANDLE_VALUE) CloseHandle(output_);
        input_ = INVALID_HANDLE_VALUE;
        output_ = INVALID_HANDLE_VALUE;
    }

private:
    HPCON pty_ = nullptr;
    HANDLE input_ = INVALID_HANDLE_VALUE;
    HANDLE output_ = INVALID_HANDLE_VALUE;
    PROCESS_INFORMATION process_{};
    std::thread reader_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::string text_;
};

class FakeWcspServer {
public:
    ~FakeWcspServer() { Stop(); }

    bool Start() {
        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
        wsa_ = true;
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == INVALID_SOCKET) return false;

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = 0;
        if (bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) return false;
        int length = sizeof(addr);
        if (getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &length) != 0) return false;
        port_ = ntohs(addr.sin_port);
        if (listen(listener_, 1) != 0) return false;

        thread_ = std::thread([this] { Run(); });
        return true;
    }

    bool WaitForHello(int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs);
        while (std::chrono::steady_clock::now() < deadline) {
            if (hello_) return true;
            Sleep(20);
        }
        return hello_;
    }

    void Stop() {
        if (listener_ != INVALID_SOCKET) {
            shutdown(listener_, SD_BOTH);
            closesocket(listener_);
            listener_ = INVALID_SOCKET;
        }
        SOCKET client = client_.exchange(INVALID_SOCKET);
        if (client != INVALID_SOCKET) shutdown(client, SD_BOTH);
        if (thread_.joinable()) thread_.join();
        if (wsa_) {
            WSACleanup();
            wsa_ = false;
        }
    }

    int port() const { return port_; }
    bool close_seen() const { return close_seen_; }
    bool detached() const { return detached_; }

private:
    static bool SendFrame(SOCKET socket, const wcs::Frame& frame) {
        std::vector<uint8_t> bytes = wcs::EncodeFrame(frame);
        size_t sent = 0;
        while (sent < bytes.size()) {
            int n = send(socket, reinterpret_cast<const char*>(bytes.data() + sent),
                         static_cast<int>(bytes.size() - sent), 0);
            if (n <= 0) return false;
            sent += static_cast<size_t>(n);
        }
        return true;
    }

    void Run() {
        SOCKET client = accept(listener_, nullptr, nullptr);
        if (client == INVALID_SOCKET) return;
        client_ = client;

        wcs::FrameReader reader;
        std::vector<uint8_t> buffer(8192);
        bool done = false;

        while (!done) {
            wcs::Frame frame;
            std::string parseError;
            const wcs::ReadResult result = reader.Next(frame, &parseError);
            if (result == wcs::ReadResult::Error) break;
            if (result == wcs::ReadResult::NeedMore) {
                int n = recv(client, reinterpret_cast<char*>(buffer.data()),
                             static_cast<int>(buffer.size()), 0);
                if (n <= 0) {
                    if (!close_seen_) detached_ = true;
                    break;
                }
                reader.Append(buffer.data(), static_cast<size_t>(n));
                continue;
            }

            switch (frame.type) {
            case wcs::FrameType::Hello: {
                wcs::Hello hello;
                if (!wcs::DecodeHello(frame.payload.data(), frame.payload.size(),
                                      hello, nullptr)) {
                    done = true;
                    break;
                }
                wcs::HelloAck ack;
                ack.major = wcs::kProtocolMajor;
                ack.minor = wcs::kProtocolMinor;
                ack.features = hello.features & wcs::kDefaultFeatures;
                ack.cols = hello.cols;
                ack.rows = hello.rows;
                ack.session_id = 1;
                ack.name = "wct-fake";
                ack.shell = hello.shell;
                wcs::Frame out;
                out.type = wcs::FrameType::HelloAck;
                out.payload = wcs::EncodeHelloAck(ack);
                if (!SendFrame(client, out)) done = true;
                else hello_ = true;
                break;
            }
            case wcs::FrameType::Input: {
                if (frame.payload.empty()) break;
                wcs::Frame out;
                out.type = wcs::FrameType::Output;
                out.payload = frame.payload;
                if (!SendFrame(client, out)) done = true;
                break;
            }
            case wcs::FrameType::Resize: {
                wcs::ResizeInfo info;
                if (wcs::DecodeResize(frame.payload.data(), frame.payload.size(), info)) {
                    wcs::ResizedInfo resized;
                    resized.cols = info.cols;
                    resized.rows = info.rows;
                    resized.reason = wcs::ResizeReason::Applied;
                    wcs::Frame out;
                    out.type = wcs::FrameType::Resized;
                    out.payload = wcs::EncodeResized(resized);
                    if (!SendFrame(client, out)) done = true;
                }
                break;
            }
            case wcs::FrameType::Ping: {
                wcs::Frame out;
                out.type = wcs::FrameType::Pong;
                out.payload = frame.payload;
                if (!SendFrame(client, out)) done = true;
                break;
            }
            case wcs::FrameType::Close: {
                close_seen_ = true;
                wcs::ExitInfo info;
                info.reason = wcs::ExitReason::ClientClose;
                info.exit_code = 0;
                wcs::Frame out;
                out.type = wcs::FrameType::Exit;
                out.payload = wcs::EncodeExit(info);
                SendFrame(client, out);
                done = true;
                break;
            }
            default:
                break;
            }
        }

        shutdown(client, SD_BOTH);
        closesocket(client);
        client_ = INVALID_SOCKET;
    }

    SOCKET listener_ = INVALID_SOCKET;
    std::atomic<SOCKET> client_{INVALID_SOCKET};
    std::thread thread_;
    int port_ = 0;
    bool wsa_ = false;
    std::atomic<bool> hello_{false};
    std::atomic<bool> close_seen_{false};
    std::atomic<bool> detached_{false};
};

struct RunResult {
    bool hint = false;
    bool echo = false;
    bool shutdown = false;
    bool closeFrame = false;
    bool detached = false;
};

RunResult RunWctSession(const std::wstring& wctPath, const std::string& keys,
                        const char* expectShutdown) {
    RunResult result;
    FakeWcspServer server;
    if (!server.Start()) {
        std::fprintf(stderr, "FAIL: cannot start the fake WCSP server\n");
        return result;
    }

    ConPtyHost host;
    const std::wstring command = L"\"" + wctPath + L"\" --host 127.0.0.1 --port " +
                                 std::to_wstring(server.port()) +
                                 L" --token test --shell cmd";
    if (!host.Start(command)) return result;

    result.hint = host.WaitFor("Ctrl+] detaches", 15000);
    if (result.hint) {
        server.WaitForHello(5000);
        host.Write("roundtrip-24680\r");
        result.echo = host.WaitFor("roundtrip-24680", 15000);
        host.Write(keys);
        result.shutdown = host.WaitFor(expectShutdown, 15000);
    }

    result.closeFrame = server.close_seen();
    result.detached = server.detached();
    server.Stop();
    host.Close();
    return result;
}

} // namespace

int main(int argc, char** argv) {
    std::wstring wctPath;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--wct" && i + 1 < argc) {
            std::string value = argv[++i];
            int need = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), (int)value.size(),
                                           nullptr, 0);
            wctPath.assign((size_t)need, L'\0');
            MultiByteToWideChar(CP_UTF8, 0, value.c_str(), (int)value.size(),
                                wctPath.data(), need);
        }
    }
    if (wctPath.empty()) {
        std::fprintf(stderr, "usage: wct_smoke --wct <wct.exe>\n");
        return 2;
    }

    // Detach from the harness console so the child is attached only to the
    // pseudo console created by ConPtyHost.
    FreeConsole();

    // ConPTY does not deliver Ctrl+\ as a key (it is filtered before it reaches
    // the client), so the portable close gesture is Ctrl+] twice. Ctrl+\ is
    // still accepted as a close key in terminals that do deliver it.
    RunResult closeRun = RunWctSession(wctPath, std::string("\x1D\x1D", 2),
                                       "[wct] session closed");
    Check(closeRun.hint, "close: wct prints the connect hint");
    Check(closeRun.echo, "close: INPUT is echoed back through wct");
    Check(closeRun.shutdown, "close: Ctrl+] twice reports the session closed");
    Check(closeRun.closeFrame, "close: server received a WCSP CLOSE frame");
    Check(!closeRun.detached, "close: the socket was not just dropped");

    RunResult detachRun = RunWctSession(wctPath, "\x1D", "[wct] detached");
    Check(detachRun.hint, "detach: wct prints the connect hint");
    Check(detachRun.shutdown, "detach: Ctrl+] reports a detach");
    Check(detachRun.detached, "detach: socket closed without CLOSE");
    Check(!detachRun.closeFrame, "detach: no WCSP CLOSE frame was sent");

    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("wct_smoke: all checks passed\n");
    return 0;
}
