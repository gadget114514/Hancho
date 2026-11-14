#include "term_server.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <bcrypt.h>

#include "common.h"
#include "process_manager.h"
#include "wcsp/frame_reader.h"
#include "wcsp/protocol.h"
#include "nlohmann/json.hpp"

#pragma comment(lib, "bcrypt.lib")

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace pc {
namespace {

constexpr size_t kMaxQueueBytes = 8 * 1024 * 1024;
constexpr int kHandshakeTimeoutMs = 5000;
constexpr int kPollTimeoutMs = 200;

std::string GenerateToken() {
    unsigned char buf[16] = {};
    if (BCryptGenRandom(nullptr, buf, static_cast<ULONG>(sizeof(buf)),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        for (size_t i = 0; i < sizeof(buf); ++i) {
            buf[i] = static_cast<unsigned char>(std::rand() & 0xFF);
        }
    }
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(sizeof(buf) * 2);
    for (unsigned char b : buf) {
        out.push_back(hex[b >> 4]);
        out.push_back(hex[b & 0x0F]);
    }
    return out;
}

std::string GetUserProfileUtf8() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) return WideToUtf8(buf);
    return WideToUtf8(GetEffectiveDataDirectoryWide());
}

// Mirrors GetDefaultBashCommand() in gui_dashboard.cpp.
std::string DefaultBashCommand() {
    const wchar_t* gitBash = L"C:\\Program Files\\Git\\bin\\bash.exe";
    DWORD attr = GetFileAttributesW(gitBash);
    if (attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY)) {
        return "\"C:\\Program Files\\Git\\bin\\bash.exe\" --login -i";
    }
    return "bash.exe";
}

std::string ToLower(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

bool ParseAttachId(const std::string& shell, int& id) {
    const std::string prefix = "attach:";
    if (shell.compare(0, prefix.size(), prefix) != 0) return false;
    std::string num = shell.substr(prefix.size());
    if (num.empty()) return false;
    char* end = nullptr;
    long value = std::strtol(num.c_str(), &end, 10);
    if (!end || *end != '\0' || value <= 0) return false;
    id = static_cast<int>(value);
    return true;
}

bool SendAll(SOCKET socket, const uint8_t* data, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        int n = send(socket, reinterpret_cast<const char*>(data) + sent,
                     static_cast<int>(len - sent), 0);
        if (n == SOCKET_ERROR || n == 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

struct TermShared {
    ProcessManager* pm = nullptr;
    std::string token;
    std::mutex mutex;
    std::condition_variable cv;
    int activeSessions = 0;
    bool stopping = false;
};

class TermConnection;

class TermConnection {
public:
    TermConnection(SOCKET socket, std::shared_ptr<TermShared> shared, uint32_t id)
        : socket_(socket), shared_(std::move(shared)), id_(id) {}

    ~TermConnection() {
        JoinWriter();
        if (socket_ != INVALID_SOCKET) {
            shutdown(socket_, SD_BOTH);
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
    }

    TermConnection(const TermConnection&) = delete;
    TermConnection& operator=(const TermConnection&) = delete;

    void Run();
    void RequestStop() {
        stopping_ = true;
        ShutdownSocket();
        queueCv_.notify_all();
    }

private:
    bool ReadFrame(wcs::Frame& frame, int timeoutMs, std::string* error);
    bool SendFrame(const wcs::Frame& frame);
    bool SendError(wcs::ErrorCode code, const std::string& message);
    void EnqueueFrame(wcs::Frame frame, bool priority);
    void QueueReplay(const std::string& replay);
    void WriterLoop();
    void StartWriter();
    void JoinWriter();
    void ShutdownSocket();
    bool HandleFrame(const wcs::Frame& frame, wcs::ExitReason& exitReason);
    void OnJobOutput(const char* data, size_t len);
    void Finish(wcs::ExitReason reason, int exitCode, bool ended);

    SOCKET socket_ = INVALID_SOCKET;
    std::shared_ptr<TermShared> shared_;
    uint32_t id_ = 0;

    wcs::FrameReader reader_;

    int jobId_ = 0;
    int subId_ = 0;
    bool spawned_ = false;

    std::mutex queueMutex_;
    std::condition_variable queueCv_;
    std::deque<wcs::Frame> queue_;
    size_t queueBytes_ = 0;
    std::vector<uint8_t> pendingLive_;
    bool replayPending_ = false;
    bool stopAfterDrain_ = false;

    std::thread writer_;
    bool writerStarted_ = false;

    std::atomic<bool> stopping_{false};
};

void TermConnection::ShutdownSocket() {
    if (socket_ != INVALID_SOCKET) {
        shutdown(socket_, SD_BOTH);
    }
}

bool TermConnection::ReadFrame(wcs::Frame& frame, int timeoutMs, std::string* error) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(timeoutMs);
    std::vector<uint8_t> buf(8192);

    for (;;) {
        std::string parseError;
        wcs::ReadResult rr = reader_.Next(frame, &parseError);
        if (rr == wcs::ReadResult::Frame) return true;
        if (rr == wcs::ReadResult::Error) {
            if (error) *error = parseError;
            return false;
        }

        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) {
            if (error) *error = "timeout";
            return false;
        }

        fd_set readSet;
        FD_ZERO(&readSet);
        FD_SET(socket_, &readSet);
        timeval tv{};
        tv.tv_sec = static_cast<long>(remaining / 1000);
        tv.tv_usec = static_cast<long>((remaining % 1000) * 1000);
        int sel = select(0, &readSet, nullptr, nullptr, &tv);
        if (sel == 0) {
            if (error) *error = "timeout";
            return false;
        }
        if (sel == SOCKET_ERROR) {
            if (error) *error = "select failed";
            return false;
        }

        int n = recv(socket_, reinterpret_cast<char*>(buf.data()),
                     static_cast<int>(buf.size()), 0);
        if (n <= 0) {
            if (error) *error = "connection closed";
            return false;
        }
        reader_.Append(buf.data(), static_cast<size_t>(n));
    }
}

bool TermConnection::SendFrame(const wcs::Frame& frame) {
    std::vector<uint8_t> bytes = wcs::EncodeFrame(frame);
    return SendAll(socket_, bytes.data(), bytes.size());
}

bool TermConnection::SendError(wcs::ErrorCode code, const std::string& message) {
    wcs::ErrorInfo info;
    info.code = code;
    info.message = message;
    wcs::Frame frame;
    frame.type = wcs::FrameType::Error;
    frame.payload = wcs::EncodeError(info);
    return SendFrame(frame);
}

void TermConnection::OnJobOutput(const char* data, size_t len) {
    if (!data || len == 0 || stopping_) return;

    bool overflow = false;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (replayPending_) {
            pendingLive_.insert(pendingLive_.end(),
                                reinterpret_cast<const uint8_t*>(data),
                                reinterpret_cast<const uint8_t*>(data) + len);
        } else {
            wcs::Frame frame;
            frame.type = wcs::FrameType::Output;
            frame.payload.assign(reinterpret_cast<const uint8_t*>(data),
                                 reinterpret_cast<const uint8_t*>(data) + len);
            queueBytes_ += frame.payload.size();
            queue_.push_back(std::move(frame));
        }
        if (queueBytes_ + pendingLive_.size() > kMaxQueueBytes) overflow = true;
    }
    queueCv_.notify_one();
    if (overflow) {
        // A client that cannot keep up is disconnected rather than blocking the
        // ConPTY reader thread (which holds the process output mutex).
        stopping_ = true;
        ShutdownSocket();
    }
}

void TermConnection::QueueReplay(const std::string& replay) {
    std::lock_guard<std::mutex> lock(queueMutex_);
    if (!replay.empty()) {
        wcs::Frame frame;
        frame.type = wcs::FrameType::Output;
        frame.payload.assign(reinterpret_cast<const uint8_t*>(replay.data()),
                             reinterpret_cast<const uint8_t*>(replay.data()) + replay.size());
        queueBytes_ += frame.payload.size();
        queue_.push_back(std::move(frame));
    }
    if (!pendingLive_.empty()) {
        wcs::Frame frame;
        frame.type = wcs::FrameType::Output;
        frame.payload = std::move(pendingLive_);
        queueBytes_ += frame.payload.size();
        pendingLive_.clear();
        queue_.push_back(std::move(frame));
    }
    replayPending_ = false;
}

void TermConnection::EnqueueFrame(wcs::Frame frame, bool priority) {
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (frame.type == wcs::FrameType::Output) {
            queueBytes_ += frame.payload.size();
        }
        if (priority) {
            queue_.push_front(std::move(frame));
        } else {
            queue_.push_back(std::move(frame));
        }
    }
    queueCv_.notify_one();
}

void TermConnection::StartWriter() {
    writerStarted_ = true;
    writer_ = std::thread(&TermConnection::WriterLoop, this);
}

void TermConnection::JoinWriter() {
    if (writerStarted_ && writer_.joinable()) {
        writer_.join();
        writerStarted_ = false;
    }
}

void TermConnection::WriterLoop() {
    for (;;) {
        wcs::Frame frame;
        bool haveFrame = false;
        {
            std::unique_lock<std::mutex> lock(queueMutex_);
            queueCv_.wait(lock, [this] {
                return stopping_ || stopAfterDrain_ || !queue_.empty();
            });
            if (!queue_.empty()) {
                frame = std::move(queue_.front());
                queue_.pop_front();
                haveFrame = true;
            } else if (stopAfterDrain_ || stopping_) {
                break;
            }
        }

        if (!haveFrame) continue;

        if (!SendFrame(frame)) {
            stopping_ = true;
            ShutdownSocket();
            break;
        }

        if (frame.type == wcs::FrameType::Output) {
            std::lock_guard<std::mutex> lock(queueMutex_);
            if (queueBytes_ >= frame.payload.size()) {
                queueBytes_ -= frame.payload.size();
            } else {
                queueBytes_ = 0;
            }
        }
    }
}

bool TermConnection::HandleFrame(const wcs::Frame& frame, wcs::ExitReason& exitReason) {
    ProcessManager& pm = *shared_->pm;
    switch (frame.type) {
    case wcs::FrameType::Input:
        if (!frame.payload.empty()) {
            std::string err;
            pm.WriteTerminalRaw(jobId_, frame.payload.data(), frame.payload.size(),
                                (frame.flags & wcs::FlagPriority) != 0, err);
        }
        return false;

    case wcs::FrameType::Resize: {
        wcs::ResizeInfo info;
        if (!wcs::DecodeResize(frame.payload.data(), frame.payload.size(), info)) {
            return false;
        }
        int cols = std::clamp<int>(info.cols, wcs::kMinColumns, wcs::kMaxDimension);
        int rows = std::clamp<int>(info.rows, wcs::kMinRows, wcs::kMaxDimension);
        std::string err;
        pm.ResizeTerminal(jobId_, cols, rows, err);

        wcs::ResizedInfo resized;
        resized.cols = static_cast<uint16_t>(cols);
        resized.rows = static_cast<uint16_t>(rows);
        resized.reason = (cols != static_cast<int>(info.cols) ||
                          rows != static_cast<int>(info.rows))
                             ? wcs::ResizeReason::Clamped
                             : wcs::ResizeReason::Applied;
        wcs::Frame out;
        out.type = wcs::FrameType::Resized;
        out.payload = wcs::EncodeResized(resized);
        EnqueueFrame(std::move(out), true);
        return false;
    }

    case wcs::FrameType::Ping: {
        wcs::Frame pong;
        pong.type = wcs::FrameType::Pong;
        pong.payload = frame.payload;
        EnqueueFrame(std::move(pong), true);
        return false;
    }

    case wcs::FrameType::Close:
        exitReason = wcs::ExitReason::ClientClose;
        return true;

    default:
        return false;
    }
}

void TermConnection::Finish(wcs::ExitReason reason, int exitCode, bool ended) {
    ProcessManager& pm = *shared_->pm;

    if (spawned_ && !ended && jobId_ > 0) {
        std::string err;
        pm.KillProcess(jobId_, err);
    }
    if (subId_ > 0) {
        pm.RemoveTerminalSubscriber(jobId_, subId_);
        subId_ = 0;
    }

    if (writerStarted_) {
        wcs::ExitInfo info;
        info.reason = reason;
        info.exit_code = static_cast<int32_t>(exitCode);
        wcs::Frame frame;
        frame.type = wcs::FrameType::Exit;
        frame.payload = wcs::EncodeExit(info);
        EnqueueFrame(std::move(frame), false);
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            stopAfterDrain_ = true;
        }
        queueCv_.notify_all();
        JoinWriter();
    }
    ShutdownSocket();
}

void TermConnection::Run() {
    ProcessManager& pm = *shared_->pm;

    wcs::Frame helloFrame;
    std::string readError;
    if (!ReadFrame(helloFrame, kHandshakeTimeoutMs, &readError)) {
        WriteAuditLog("TERM_SESSION_ERR", "session " + std::to_string(id_) +
                      " handshake read failed: " + readError);
        return;
    }
    if (helloFrame.type != wcs::FrameType::Hello) {
        SendError(wcs::ErrorCode::InvalidHello, "expected HELLO");
        return;
    }

    wcs::Hello hello;
    std::string decodeError;
    if (!wcs::DecodeHello(helloFrame.payload.data(), helloFrame.payload.size(),
                          hello, &decodeError)) {
        SendError(wcs::ErrorCode::InvalidHello, "bad HELLO: " + decodeError);
        return;
    }
    if (hello.major != wcs::kProtocolMajor) {
        SendError(wcs::ErrorCode::VersionMismatch, "protocol version mismatch");
        return;
    }
    // The token is always required, even for loopback clients.
    if (hello.token.empty()) {
        SendError(wcs::ErrorCode::AuthRequired, "token required");
        return;
    }
    if (hello.token != shared_->token) {
        SendError(wcs::ErrorCode::AuthFailed, "invalid token");
        return;
    }

    const uint16_t cols = static_cast<uint16_t>(
        std::clamp<int>(hello.cols, wcs::kMinColumns, wcs::kMaxDimension));
    const uint16_t rows = static_cast<uint16_t>(
        std::clamp<int>(hello.rows, wcs::kMinRows, wcs::kMaxDimension));

    std::string shellName = hello.shell;
    int attachId = 0;
    bool attach = ParseAttachId(shellName, attachId);
    std::string displayName;

    ProcessSnapshot snap;
    if (attach) {
        if (!pm.GetSnapshot(attachId, snap)) {
            SendError(wcs::ErrorCode::ShellNotAllowed,
                      "process not found: " + std::to_string(attachId));
            return;
        }
        if (snap.mode != ExecMode::ConPTY) {
            SendError(wcs::ErrorCode::ShellNotAllowed,
                      "process " + std::to_string(attachId) +
                          " is not a ConPTY terminal job");
            return;
        }
        jobId_ = attachId;
        displayName = snap.name;
    } else {
        std::string lower = ToLower(shellName);
        std::string commandLine;
        if (lower == "cmd") {
            commandLine = "cmd.exe";
        } else if (lower == "powershell") {
            commandLine = "powershell.exe";
        } else if (lower == "bash") {
            commandLine = DefaultBashCommand();
        } else {
            SendError(wcs::ErrorCode::ShellNotAllowed,
                      "shell not allowed: " + shellName);
            return;
        }

        StartProcessRequest req;
        req.name = "wcsp-" + lower;
        req.commandLine = commandLine;
        req.workingDir = GetUserProfileUtf8();
        req.mode = ExecMode::ConPTY;
        req.openStdin = true;

        std::string startError;
        int id = pm.StartProcess(req, startError);
        if (id <= 0) {
            SendError(wcs::ErrorCode::ShellStartFailed,
                      "failed to start shell: " + startError);
            return;
        }
        spawned_ = true;
        jobId_ = id;
        displayName = lower;
        pm.GetSnapshot(id, snap);
    }

    // Resize the existing job to the client's size before taking the replay.
    {
        std::string resizeErr;
        pm.ResizeTerminal(jobId_, cols, rows, resizeErr);
    }

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        replayPending_ = true;
    }
    std::string replay;
    std::string subError;
    subId_ = pm.AddTerminalSubscriber(
        jobId_,
        [this](const char* data, size_t len) { OnJobOutput(data, len); },
        replay, subError);
    if (subId_ <= 0) {
        if (spawned_ && jobId_ > 0) {
            std::string killErr;
            pm.KillProcess(jobId_, killErr);
        }
        SendError(wcs::ErrorCode::ShellNotAllowed, subError);
        return;
    }

    bool ended = snap.state != ProcState::Running;

    wcs::HelloAck ack;
    ack.major = wcs::kProtocolMajor;
    ack.minor = wcs::kProtocolMinor;
    ack.features = hello.features & wcs::kDefaultFeatures;
    ack.cols = cols;
    ack.rows = rows;
    ack.session_id = id_;
    ack.shell = displayName;
    wcs::Frame ackFrame;
    ackFrame.type = wcs::FrameType::HelloAck;
    ackFrame.payload = wcs::EncodeHelloAck(ack);
    if (!SendFrame(ackFrame)) {
        std::string err;
        pm.RemoveTerminalSubscriber(jobId_, subId_);
        subId_ = 0;
        if (spawned_ && jobId_ > 0) pm.KillProcess(jobId_, err);
        return;
    }

    QueueReplay(replay);
    StartWriter();

    if (ended) {
        Finish(wcs::ExitReason::ChildExited, static_cast<int>(snap.exitCode), true);
        return;
    }

    DWORD timeout = kPollTimeoutMs;
    setsockopt(socket_, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    DWORD sendTimeout = 5000;
    setsockopt(socket_, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&sendTimeout), sizeof(sendTimeout));

    wcs::ExitReason exitReason = wcs::ExitReason::ChildExited;
    bool clientClosed = false;
    int exitCode = 0;
    bool jobEnded = false;

    while (!stopping_) {
        char buf[16384];
        int n = recv(socket_, buf, static_cast<int>(sizeof(buf)), 0);
        if (n > 0) {
            reader_.Append(reinterpret_cast<const uint8_t*>(buf), static_cast<size_t>(n));
            for (;;) {
                wcs::Frame frame;
                std::string parseError;
                wcs::ReadResult rr = reader_.Next(frame, &parseError);
                if (rr == wcs::ReadResult::NeedMore) break;
                if (rr == wcs::ReadResult::Error) {
                    clientClosed = true;
                    exitReason = wcs::ExitReason::Killed;
                    stopping_ = true;
                    break;
                }
                if (HandleFrame(frame, exitReason)) {
                    clientClosed = true;
                    stopping_ = true;
                    break;
                }
            }
        } else if (n == 0) {
            clientClosed = true;
            exitReason = wcs::ExitReason::ClientClose;
            break;
        } else {
            int err = WSAGetLastError();
            if (err != WSAETIMEDOUT) {
                clientClosed = true;
                exitReason = wcs::ExitReason::ClientClose;
                break;
            }
        }

        ProcessSnapshot current;
        if (!pm.GetSnapshot(jobId_, current) || current.state != ProcState::Running) {
            jobEnded = true;
            exitCode = current.state != ProcState::Running
                           ? static_cast<int>(current.exitCode)
                           : 0;
            break;
        }
    }

    if (!jobEnded) {
        ProcessSnapshot current;
        if (pm.GetSnapshot(jobId_, current) && current.state != ProcState::Running) {
            jobEnded = true;
            exitCode = static_cast<int>(current.exitCode);
        }
    }

    if (clientClosed && !jobEnded) {
        exitReason = wcs::ExitReason::ClientClose;
    } else if (jobEnded) {
        exitReason = wcs::ExitReason::ChildExited;
    }
    Finish(exitReason, exitCode, jobEnded);
}

} // namespace

struct TermServer::Impl {
    explicit Impl(ProcessManager& processManager) : pm(processManager) {}

    ProcessManager& pm;
    std::shared_ptr<TermShared> shared = std::make_shared<TermShared>();
    std::thread acceptThread;
    std::atomic<bool> running{false};
    std::atomic<bool> wsaStarted{false};
    SOCKET listenSocket = INVALID_SOCKET;
    std::atomic<uint16_t> port{0};
    std::mutex sessionsMutex;
    std::unordered_map<uint32_t, std::shared_ptr<TermConnection>> sessions;
    std::atomic<uint32_t> nextId{1};

    void AcceptLoop();
    void StopSessions();
};

void TermServer::Impl::AcceptLoop() {
    while (running) {
        SOCKET client = accept(listenSocket, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            if (!running) break;
            continue;
        }

        int one = 1;
        setsockopt(client, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&one), sizeof(one));
        setsockopt(client, SOL_SOCKET, SO_KEEPALIVE,
                   reinterpret_cast<const char*>(&one), sizeof(one));

        uint32_t id = nextId.fetch_add(1);
        auto conn = std::make_shared<TermConnection>(client, shared, id);
        {
            std::lock_guard<std::mutex> lock(sessionsMutex);
            sessions[id] = conn;
        }
        {
            std::lock_guard<std::mutex> lock(shared->mutex);
            shared->activeSessions++;
        }

        auto sharedState = shared;
        std::thread([conn, sharedState, id]() {
            conn->Run();
            {
                std::lock_guard<std::mutex> lock(sharedState->mutex);
                sharedState->activeSessions--;
            }
            sharedState->cv.notify_all();
            (void)id;
        }).detach();
    }
}

void TermServer::Impl::StopSessions() {
    std::vector<std::shared_ptr<TermConnection>> copy;
    {
        std::lock_guard<std::mutex> lock(sessionsMutex);
        for (auto& kv : sessions) copy.push_back(kv.second);
    }
    for (auto& conn : copy) {
        if (conn) conn->RequestStop();
    }
}

TermServer::TermServer(ProcessManager& pm) : impl_(std::make_unique<Impl>(pm)) {
    impl_->shared->pm = &pm;
}

TermServer::~TermServer() {
    Stop();
}

bool TermServer::Start(std::string& error) {
    Stop();

    impl_->shared->pm = &impl_->pm;

    const std::wstring dataDir = GetEffectiveDataDirectoryWide();
    const std::wstring configPath = PathJoinWide(dataDir, L"config.json");

    nlohmann::json cfg = nlohmann::json::object();
    std::string content = ReadFileUtf8Wide(configPath);
    if (!content.empty()) {
        nlohmann::json parsed = nlohmann::json::parse(content, nullptr, false);
        if (parsed.is_object()) cfg = parsed;
    }

    bool local = true;
    bool remote = false;
    std::string host = "0.0.0.0";
    uint16_t configPort = 9139;
    std::string token;
    if (cfg.contains("terminal") && cfg["terminal"].is_object()) {
        const nlohmann::json& t = cfg["terminal"];
        if (t.contains("local") && t["local"].is_boolean()) local = t["local"].get<bool>();
        if (t.contains("remote") && t["remote"].is_boolean()) remote = t["remote"].get<bool>();
        if (t.contains("host") && t["host"].is_string()) host = t["host"].get<std::string>();
        if (t.contains("port") && t["port"].is_number_integer()) {
            int64_t p = t["port"].get<int64_t>();
            if (p > 0 && p <= 65535) configPort = static_cast<uint16_t>(p);
        }
        if (t.contains("token") && t["token"].is_string()) token = t["token"].get<std::string>();
    }

    bool configChanged = !cfg.contains("terminal") || !cfg["terminal"].is_object();
    if (!cfg.contains("terminal") || !cfg["terminal"].is_object()) {
        cfg["terminal"] = nlohmann::json::object();
    }
    if (!cfg["terminal"].contains("local")) cfg["terminal"]["local"] = true;
    if (!cfg["terminal"].contains("remote")) cfg["terminal"]["remote"] = false;
    if (!cfg["terminal"].contains("host")) cfg["terminal"]["host"] = "0.0.0.0";
    if (!cfg["terminal"].contains("port")) cfg["terminal"]["port"] = 9139;

    if (!local && !remote) {
        if (configChanged && !dataDir.empty()) {
            WriteFileAtomicWide(configPath, cfg.dump(2));
        }
        WriteAuditLog("TERM_SERVER_DISABLED", "local and remote connections are both disabled");
        return true;
    }

    if (token.empty()) {
        token = GenerateToken();
        cfg["terminal"]["token"] = token;
        configChanged = true;
    }
    if (configChanged && !dataDir.empty()) {
        WriteFileAtomicWide(configPath, cfg.dump(2));
    }

    impl_->shared->token = token;

    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        error = "WSAStartup failed";
        return false;
    }
    impl_->wsaStarted = true;

    const std::string bindHost = remote ? host : std::string("127.0.0.1");
    const uint16_t bindPort = remote ? configPort : 0;

    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* result = nullptr;
    int rc = getaddrinfo(bindHost.empty() ? nullptr : bindHost.c_str(),
                         std::to_string(bindPort).c_str(), &hints, &result);
    if (rc != 0) {
        error = "getaddrinfo failed";
        impl_->wsaStarted = false;
        WSACleanup();
        return false;
    }

    SOCKET listenSocket = INVALID_SOCKET;
    for (addrinfo* ai = result; ai; ai = ai->ai_next) {
        SOCKET s = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (s == INVALID_SOCKET) continue;

        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char*>(&one), sizeof(one));

        if (bind(s, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0 &&
            listen(s, SOMAXCONN) == 0) {
            listenSocket = s;
            break;
        }
        closesocket(s);
    }
    freeaddrinfo(result);

    if (listenSocket == INVALID_SOCKET) {
        error = "bind/listen failed on " + bindHost + ":" + std::to_string(bindPort);
        impl_->wsaStarted = false;
        WSACleanup();
        return false;
    }

    sockaddr_in bound{};
    int boundLen = sizeof(bound);
    if (getsockname(listenSocket, reinterpret_cast<sockaddr*>(&bound), &boundLen) == 0) {
        impl_->port = ntohs(bound.sin_port);
    } else {
        impl_->port = bindPort;
    }

    impl_->listenSocket = listenSocket;
    impl_->running = true;
    impl_->acceptThread = std::thread([this]() { impl_->AcceptLoop(); });

    WriteAuditLog("TERM_SERVER_LISTEN",
                  bindHost + ":" + std::to_string(impl_->port.load()) +
                      (token.empty() ? "" : " (token required)"));
    return true;
}

void TermServer::Stop() {
    if (!impl_) return;
    impl_->running = false;
    {
        std::lock_guard<std::mutex> lock(impl_->shared->mutex);
        impl_->shared->stopping = true;
    }

    if (impl_->listenSocket != INVALID_SOCKET) {
        shutdown(impl_->listenSocket, SD_BOTH);
        closesocket(impl_->listenSocket);
        impl_->listenSocket = INVALID_SOCKET;
    }
    if (impl_->acceptThread.joinable()) {
        impl_->acceptThread.join();
    }

    impl_->StopSessions();

    std::unique_lock<std::mutex> lock(impl_->shared->mutex);
    impl_->shared->cv.wait_for(lock, std::chrono::seconds(3),
                               [this]() { return impl_->shared->activeSessions == 0; });

    {
        std::lock_guard<std::mutex> sessionLock(impl_->sessionsMutex);
        impl_->sessions.clear();
    }
    impl_->port = 0;
    if (impl_->wsaStarted.exchange(false)) {
        WSACleanup();
    }
}

bool TermServer::IsRunning() const {
    return impl_ && impl_->running.load();
}

uint16_t TermServer::Port() const {
    return impl_ ? impl_->port.load() : 0;
}

std::string TermServer::Token() const {
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->shared->mutex);
    return impl_->shared->token;
}

} // namespace pc
