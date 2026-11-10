#pragma once

#include "process_manager.h"
#include "taskqueue.h"
#include "remote_client.h"
#include <thread>
#include <atomic>
#include <string>
#include <functional>
#include <mutex>

namespace pc {

class BtManager;

class IpcServer {
public:
    explicit IpcServer(ProcessManager& pm, TaskQueueManager& tqm, HWND notifyHwnd = nullptr);
    IpcServer(ProcessManager& pm, TaskQueueManager& tqm, BtManager* btm, HWND notifyHwnd = nullptr);
    ~IpcServer();

    bool Start();
    void Stop();
    void SetNotifyHwnd(HWND hwnd) { notifyHwnd_ = hwnd; }
    void SetBtManager(BtManager* btm) { btm_ = btm; }

    // Called (from an IPC client thread) when a client asks the dashboard to
    // appear. The host posts a message to its own window instead of touching
    // the UI from the IPC thread.
    void SetGuiShowCallback(std::function<void()> cb) {
        std::lock_guard<std::mutex> lock(cbMutex_);
        guiShowCb_ = std::move(cb);
    }
    // Called when a client asks the core host to stop. Must return immediately.
    void SetShutdownCallback(std::function<void()> cb) {
        std::lock_guard<std::mutex> lock(cbMutex_);
        shutdownCb_ = std::move(cb);
    }
    // True when no dashboard window is owned by this process (started with --headless).
    void SetHeadless(bool headless) { headless_ = headless; }
    bool IsHeadless() const { return headless_; }

    // Execute a JSON request directly against ProcessManager / TaskQueueManager / BtManager
    // and return a JSON response.
    std::string HandleRequestJson(const std::string& reqJson);

private:
    void AcceptLoop();
    void StartRemoteListener();
    void StopRemoteListener();
    void InstallRemoteCallbacks();
#ifdef _WIN32
    void HandleClient(HANDLE hPipe);
#else
    void HandleClient(std::unique_ptr<platform::IIpcConnection> conn);
    std::unique_ptr<platform::IIpcServer> transport_;
#endif

    ProcessManager& pm_;
    TaskQueueManager& tqm_;
    BtManager* btm_ = nullptr;
    std::atomic<HWND> notifyHwnd_{nullptr};
    std::function<void()> guiShowCb_;
    std::function<void()> shutdownCb_;
    std::mutex cbMutex_;
    bool headless_ = false;
    std::atomic<bool> stop_{false};
    std::thread acceptThread_;
    std::unique_ptr<remote::RemoteListener> remoteListener_;
    bool remoteCallbacksInstalled_ = false;
};

// Format ProcessSnapshot as JSON object
std::string SnapshotToJson(const ProcessSnapshot& s);

} // namespace pc
