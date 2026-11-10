#include "ipc_server.h"

#ifndef _WIN32

#include "platform/platform.h"

namespace pc {

bool IpcServer::Start() {
    stop_ = false;
    std::string err;
    transport_ = platform::CreateIpcServer(GetIpcEndpointUtf8());
    if (!transport_ || !transport_->Start(err)) {
        WriteAuditLog("IPC_START_FAILED", err.empty() ? "cannot start IPC transport" : err);
        transport_.reset();
        return false;
    }
    WriteAuditLog("IPC_LISTEN", "Listening on " + transport_->Endpoint());
    acceptThread_ = std::thread(&IpcServer::AcceptLoop, this);
    StartRemoteListener();
    return true;
}

void IpcServer::Stop() {
    if (stop_.exchange(true)) return;
    StopRemoteListener();
    if (transport_) transport_->Stop();
    if (acceptThread_.joinable()) acceptThread_.join();
}

void IpcServer::AcceptLoop() {
    while (!stop_) {
        std::string err;
        auto conn = transport_ ? transport_->Accept(err) : nullptr;
        if (!conn) {
            if (stop_) break;
            if (!err.empty()) WriteAuditLog("IPC_ACCEPT", err);
            SleepMs(100);
            continue;
        }
        std::thread([this, c = std::move(conn)]() mutable {
            try {
                HandleClient(std::move(c));
            } catch (const std::exception& e) {
                WriteAuditLog("CRASH_GUARD", std::string("IPC client exception: ") + e.what());
            } catch (...) {
                WriteAuditLog("CRASH_GUARD", "IPC client exception: unknown");
            }
        }).detach();
    }
}

void IpcServer::HandleClient(std::unique_ptr<platform::IIpcConnection> conn) {
    if (!conn) return;
    std::string req;
    char buffer[16384];
    std::string err;
    while (true) {
        int n = conn->Read(buffer, sizeof(buffer), err);
        if (n > 0) {
            req.append(buffer, static_cast<size_t>(n));
            continue;
        }
        break;
    }
    if (!req.empty()) {
        // Accept either a raw JSON body or one newline-terminated request.
        while (!req.empty() && (req.back() == '\n' || req.back() == '\r')) req.pop_back();
        std::string resp = HandleRequestJson(req);
        conn->WriteAll(resp.data(), resp.size(), err);
    }
    conn->Close();
}

} // namespace pc

#endif // !_WIN32
