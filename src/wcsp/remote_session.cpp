#include "remote_session.h"

#include <algorithm>
#include <chrono>
#include <cstring>

namespace {

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    int need = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                                   nullptr, 0, nullptr, nullptr);
    if (need <= 0) return {};
    std::string out(need, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(),
                        out.data(), need, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    int need = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    if (need <= 0) return {};
    std::wstring out(need, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), out.data(), need);
    return out;
}

uint64_t NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

} // namespace

RemoteSession::RemoteSession(RemoteConfig config) : config_(std::move(config)) {}

RemoteSession::~RemoteSession() { Close(); }

void RemoteSession::SetOutputSink(OutputFn fn) {
    std::lock_guard<std::mutex> lock(sink_mutex_);
    sink_ = std::move(fn);
}

bool RemoteSession::Start(const TerminalStartOptions& opt, std::wstring* error) {
    if (running_) return true;
    stopping_ = false;

    config_.shell = WideToUtf8(opt.shell);
    if (config_.shell.empty()) config_.shell = "cmd";

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        if (error) *error = L"WSAStartup failed";
        return false;
    }
    wsa_started_ = true;

    if (!Connect(error)) {
        ShutdownSocket();
        if (socket_ != INVALID_SOCKET) { closesocket(socket_); socket_ = INVALID_SOCKET; }
        if (wsa_started_) { WSACleanup(); wsa_started_ = false; }
        return false;
    }

    if (!Handshake(opt, error)) {
        ShutdownSocket();
        if (socket_ != INVALID_SOCKET) { closesocket(socket_); socket_ = INVALID_SOCKET; }
        if (wsa_started_) { WSACleanup(); wsa_started_ = false; }
        return false;
    }

    running_ = true;
    next_ping_ms_ = NowMs() + 15000;
    receiver_ = std::thread(&RemoteSession::ReceiverLoop, this);
    sender_ = std::thread(&RemoteSession::SenderLoop, this);
    return true;
}

bool RemoteSession::Connect(std::wstring* error) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    const std::string port = std::to_string(config_.port);
    addrinfo* result = nullptr;
    int rc = getaddrinfo(config_.host.c_str(), port.c_str(), &hints, &result);
    if (rc != 0) {
        if (error) *error = L"getaddrinfo failed: " + Utf8ToWide(std::to_string(rc));
        return false;
    }

    bool connected = false;
    for (addrinfo* ai = result; ai && !connected; ai = ai->ai_next) {
        SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        u_long nonblocking = 1;
        ioctlsocket(s, FIONBIO, &nonblocking);

        int cr = connect(s, ai->ai_addr, (int)ai->ai_addrlen);
        if (cr == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
            closesocket(s);
            continue;
        }

        if (cr == SOCKET_ERROR) {
            fd_set writeSet;
            FD_ZERO(&writeSet);
            FD_SET(s, &writeSet);
            timeval tv{};
            tv.tv_sec = config_.connect_timeout_ms / 1000;
            tv.tv_usec = (config_.connect_timeout_ms % 1000) * 1000;
            int sel = select(0, nullptr, &writeSet, nullptr, &tv);
            if (sel <= 0) {
                closesocket(s);
                continue;
            }
            int soError = 0;
            int soLen = sizeof(soError);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, (char*)&soError, &soLen) != 0 ||
                soError != 0) {
                closesocket(s);
                continue;
            }
        }

        nonblocking = 0;
        ioctlsocket(s, FIONBIO, &nonblocking);

        int one = 1;
        setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char*)&one, sizeof(one));
        setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char*)&one, sizeof(one));

        socket_ = s;
        connected = true;
    }

    freeaddrinfo(result);
    if (!connected) {
        if (error) *error = L"connect failed: " + Utf8ToWide(config_.host) + L":" +
                            std::to_wstring(config_.port);
        return false;
    }
    return true;
}

bool RemoteSession::Handshake(const TerminalStartOptions& opt, std::wstring* error) {
    wcs::Hello hello;
    hello.major = wcs::kProtocolMajor;
    hello.minor = wcs::kProtocolMinor;
    hello.features = config_.features;
    hello.cols = (uint16_t)std::clamp(opt.cols > 0 ? opt.cols : 80,
                                      (int)wcs::kMinColumns, (int)wcs::kMaxDimension);
    hello.rows = (uint16_t)std::clamp(opt.rows > 0 ? opt.rows : 24,
                                      (int)wcs::kMinRows, (int)wcs::kMaxDimension);
#ifdef PC_VERSION
    hello.name = "Hancho/" PC_VERSION;
#else
    hello.name = "Hancho";
#endif
    hello.shell = config_.shell;
    hello.token = config_.token;

    wcs::Frame frame;
    frame.type = wcs::FrameType::Hello;
    frame.payload = wcs::EncodeHello(hello);
    if (!SendFrame(frame)) {
        if (error) *error = L"failed to send HELLO";
        return false;
    }

    DWORD timeout = (DWORD)config_.connect_timeout_ms;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));

    reader_.Clear();
    std::vector<uint8_t> recvBuf(8192);

    for (;;) {
        wcs::Frame incoming;
        std::string parseError;
        wcs::ReadResult rr = reader_.Next(incoming, &parseError);
        if (rr == wcs::ReadResult::Error) {
            if (error) *error = L"protocol error: " + Utf8ToWide(parseError);
            return false;
        }
        if (rr == wcs::ReadResult::NeedMore) {
            int n = recv(socket_, (char*)recvBuf.data(), (int)recvBuf.size(), 0);
            if (n <= 0) {
                if (error) *error = L"connection closed during handshake";
                return false;
            }
            reader_.Append(recvBuf.data(), (size_t)n);
            continue;
        }

        if (incoming.type == wcs::FrameType::HelloAck) {
            wcs::HelloAck ack;
            std::string decodeError;
            if (!wcs::DecodeHelloAck(incoming.payload.data(), incoming.payload.size(),
                                     ack, &decodeError)) {
                if (error) *error = L"bad HELLO_ACK: " + Utf8ToWide(decodeError);
                return false;
            }
            if (ack.major != wcs::kProtocolMajor) {
                if (error) *error = L"protocol major version mismatch";
                return false;
            }
            if (onResized && (ack.cols != hello.cols || ack.rows != hello.rows)) {
                wcs::ResizedInfo info;
                info.cols = ack.cols;
                info.rows = ack.rows;
                info.reason = wcs::ResizeReason::Clamped;
                onResized(info);
            }
            timeout = 0;
            setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof(timeout));
            return true;
        }

        if (incoming.type == wcs::FrameType::Error) {
            wcs::ErrorInfo info;
            if (wcs::DecodeError(incoming.payload.data(), incoming.payload.size(), info)) {
                if (error) *error = L"server error: " + Utf8ToWide(info.message);
            } else {
                if (error) *error = L"server returned an error";
            }
            return false;
        }
    }
}

void RemoteSession::Write(const void* data, size_t len) {
    if (!running_ || !data || len == 0) return;
    wcs::Frame frame;
    frame.type = wcs::FrameType::Input;
    frame.flags = wcs::FlagNone;
    frame.payload.assign((const uint8_t*)data, (const uint8_t*)data + len);
    EnqueueFrame(std::move(frame), false);
}

void RemoteSession::WriteFront(const void* data, size_t len) {
    if (!running_ || !data || len == 0) return;
    wcs::Frame frame;
    frame.type = wcs::FrameType::Input;
    frame.flags = wcs::FlagPriority;
    frame.payload.assign((const uint8_t*)data, (const uint8_t*)data + len);
    EnqueueFrame(std::move(frame), true);
}

void RemoteSession::Resize(int cols, int rows) {
    if (!running_) return;
    wcs::ResizeInfo info;
    info.cols = (uint16_t)std::clamp(cols, (int)wcs::kMinColumns, (int)wcs::kMaxDimension);
    info.rows = (uint16_t)std::clamp(rows, (int)wcs::kMinRows, (int)wcs::kMaxDimension);
    wcs::Frame frame;
    frame.type = wcs::FrameType::Resize;
    frame.payload = wcs::EncodeResize(info);
    EnqueueFrame(std::move(frame), false);
}

bool RemoteSession::IsRunning() const { return running_; }

void RemoteSession::Close() {
    std::lock_guard<std::mutex> closeLock(close_mutex_);
    stopping_ = true;
    ShutdownSocket();
    queue_cv_.notify_all();

    if (receiver_.joinable()) receiver_.join();
    if (sender_.joinable()) sender_.join();

    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    if (wsa_started_) {
        WSACleanup();
        wsa_started_ = false;
    }
    running_ = false;
}

std::wstring RemoteSession::LastError() const {
    std::lock_guard<std::mutex> lock(error_mutex_);
    return last_error_;
}

void RemoteSession::ReceiverLoop() {
    std::vector<uint8_t> buf(32768);
    std::string parseError;

    while (!stopping_) {
        wcs::Frame frame;
        wcs::ReadResult rr = reader_.Next(frame, &parseError);
        if (rr == wcs::ReadResult::Error) {
            SetError(L"protocol error: " + Utf8ToWide(parseError));
            break;
        }
        if (rr == wcs::ReadResult::NeedMore) {
            int n = recv(socket_, (char*)buf.data(), (int)buf.size(), 0);
            if (n <= 0) {
                if (!stopping_) SetError(L"connection closed");
                break;
            }
            reader_.Append(buf.data(), (size_t)n);
            continue;
        }

        switch (frame.type) {
        case wcs::FrameType::Output: {
            OutputFn sink;
            {
                std::lock_guard<std::mutex> lock(sink_mutex_);
                sink = sink_;
            }
            if (sink && !frame.payload.empty()) {
                sink((const char*)frame.payload.data(), frame.payload.size());
            }
            break;
        }
        case wcs::FrameType::Resized: {
            wcs::ResizedInfo info;
            if (wcs::DecodeResized(frame.payload.data(), frame.payload.size(), info) &&
                onResized) {
                onResized(info);
            }
            break;
        }
        case wcs::FrameType::Exit: {
            wcs::ExitInfo info;
            if (wcs::DecodeExit(frame.payload.data(), frame.payload.size(), info) &&
                onExit) {
                onExit(info);
            }
            stopping_ = true;
            break;
        }
        case wcs::FrameType::Error: {
            wcs::ErrorInfo info;
            if (wcs::DecodeError(frame.payload.data(), frame.payload.size(), info)) {
                SetError(Utf8ToWide(info.message));
                if (onError) onError(info);
            } else {
                SetError(L"server error");
            }
            stopping_ = true;
            break;
        }
        case wcs::FrameType::Pong:
            break;
        default:
            break;
        }
    }

    running_ = false;
    ShutdownSocket();
    queue_cv_.notify_all();
}

void RemoteSession::SenderLoop() {
    while (!stopping_) {
        wcs::Frame frame;
        bool haveFrame = false;
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            queue_cv_.wait_for(lock, std::chrono::milliseconds(100), [this] {
                return stopping_ || !priority_queue_.empty() || !normal_queue_.empty();
            });
            if (stopping_) break;

            if (!priority_queue_.empty()) {
                frame = std::move(priority_queue_.front());
                priority_queue_.pop_front();
                haveFrame = true;
            } else if (!normal_queue_.empty()) {
                frame = std::move(normal_queue_.front());
                normal_queue_.pop_front();
                haveFrame = true;
            }
        }

        if (haveFrame) {
            if (!SendFrame(frame)) {
                SetError(L"send failed");
                stopping_ = true;
                ShutdownSocket();
                break;
            }
            continue;
        }

        const uint64_t now = NowMs();
        if (now >= next_ping_ms_.load()) {
            wcs::Frame ping;
            ping.type = wcs::FrameType::Ping;
            ping.payload = wcs::EncodePing(now);
            if (!SendFrame(ping)) {
                SetError(L"send failed");
                stopping_ = true;
                ShutdownSocket();
                break;
            }
            next_ping_ms_ = now + 15000;
        }
    }
    queue_cv_.notify_all();
}

bool RemoteSession::SendAll(const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send(socket_, (const char*)data + sent, (int)(len - sent), 0);
        if (n == SOCKET_ERROR) return false;
        if (n == 0) return false;
        sent += (size_t)n;
    }
    return true;
}

bool RemoteSession::SendFrame(const wcs::Frame& frame) {
    std::vector<uint8_t> bytes = wcs::EncodeFrame(frame);
    return SendAll(bytes.data(), bytes.size());
}

void RemoteSession::EnqueueFrame(wcs::Frame frame, bool priority) {
    {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        if (priority) priority_queue_.push_back(std::move(frame));
        else normal_queue_.push_back(std::move(frame));
    }
    queue_cv_.notify_one();
}

void RemoteSession::SetError(const std::wstring& error) {
    std::lock_guard<std::mutex> lock(error_mutex_);
    last_error_ = error;
}

void RemoteSession::ShutdownSocket() {
    if (socket_ != INVALID_SOCKET) {
        shutdown(socket_, SD_BOTH);
    }
}
