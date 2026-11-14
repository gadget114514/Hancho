#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>

#include "iterminal_io.h"
#include "frame_reader.h"
#include "protocol.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct RemoteConfig {
    std::string host = "127.0.0.1";
    uint16_t port = 7000;
    std::string token;
    std::string shell;
    uint32_t features = wcs::kDefaultFeatures;
    int connect_timeout_ms = 5000;
};

class RemoteSession : public ITerminalIo {
public:
    explicit RemoteSession(RemoteConfig config);
    ~RemoteSession() override;

    void SetOutputSink(OutputFn fn) override;
    bool Start(const TerminalStartOptions& opt, std::wstring* error) override;

    void Write(const void* data, size_t len) override;
    void WriteFront(const void* data, size_t len) override;
    void Resize(int cols, int rows) override;

    bool IsRunning() const override;
    void Close() override;
    std::wstring LastError() const override;

    // UI notifications (called from worker threads; post to the UI thread).
    std::function<void(const wcs::ExitInfo&)> onExit;
    std::function<void(const wcs::ErrorInfo&)> onError;
    std::function<void(const wcs::ResizedInfo&)> onResized;

private:
    bool Connect(std::wstring* error);
    bool Handshake(const TerminalStartOptions& opt, std::wstring* error);
    void ReceiverLoop();
    void SenderLoop();

    bool SendAll(const uint8_t* data, size_t len);
    bool SendFrame(const wcs::Frame& frame);
    void EnqueueFrame(wcs::Frame frame, bool priority);
    void SetError(const std::wstring& error);
    void ShutdownSocket();

    RemoteConfig config_;

    SOCKET socket_ = INVALID_SOCKET;
    bool wsa_started_ = false;

    wcs::FrameReader reader_;

    mutable std::mutex sink_mutex_;
    OutputFn sink_;

    mutable std::mutex error_mutex_;
    std::wstring last_error_;

    std::mutex close_mutex_;
    std::mutex queue_mutex_;
    std::condition_variable queue_cv_;
    std::deque<wcs::Frame> normal_queue_;
    std::deque<wcs::Frame> priority_queue_;

    std::thread receiver_;
    std::thread sender_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stopping_{false};
    std::atomic<uint64_t> next_ping_ms_{0};
};
