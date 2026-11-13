#pragma once

#include "process_manager.h"
#include "taskqueue.h"
#include "remote_client.h"
#include <thread>
#include <atomic>
#include <string>
#include <functional>
#include <memory>
#include <mutex>

namespace pc {

class BtManager;
class WebUiServer;
class IpmsgNode;
class IpmsgHandler;

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
    // Local suspend implementation used by `power_suspend` (MCP / CLI).
    using PowerCallback = std::function<bool(bool hibernate, std::string& outError)>;
    void SetPowerCallback(PowerCallback cb) {
        std::lock_guard<std::mutex> lock(cbMutex_);
        powerCb_ = std::move(cb);
    }
    // True when no dashboard window is owned by this process (started with --headless).
    void SetHeadless(bool headless) { headless_ = headless; }
    bool IsHeadless() const { return headless_; }

    WebUiServer* GetWebUiServer() { return webUiServer_.get(); }
    void StartWebUiServer(const std::string& token = std::string());
    void StopWebUiServer();
    // Stops and restarts the WebUI server after a "webui" config change.
    // Runs on a short-lived helper thread so the config_set reply is sent first.
    void ScheduleWebUiRestart();

    // IPMsg node. Always constructed so history and logs stay readable while
    // the feature is disabled.
    IpmsgNode* GetIpmsg() { return ipmsg_.get(); }
    // Reads "ipmsg" from <data-dir>/config.json and starts the node when enabled.
    // Failures are logged and the core keeps running.
    void StartIpmsg();
    void StopIpmsg();
    // Stops and restarts the node after an "ipmsg" config change. Runs on a
    // short-lived helper thread so the config_set reply is sent first.
    void ScheduleIpmsgRestart();

    // Execute a JSON request directly against ProcessManager / TaskQueueManager / BtManager
    // and return a JSON response.
    std::string HandleRequestJson(const std::string& reqJson);

private:
    void AcceptLoop();
    std::string HandleChatAction(const std::string& action, const std::string& reqJson);
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
    PowerCallback powerCb_;
    std::mutex cbMutex_;
    bool headless_ = false;
    std::atomic<bool> stop_{false};
    std::thread acceptThread_;
    std::unique_ptr<remote::RemoteListener> remoteListener_;
    std::unique_ptr<WebUiServer> webUiServer_;
    std::thread webuiRestartThread_;
    std::unique_ptr<IpmsgNode> ipmsg_;
    std::unique_ptr<IpmsgHandler> ipmsgHandler_;
    std::thread ipmsgRestartThread_;
    bool remoteCallbacksInstalled_ = false;
};

// Format ProcessSnapshot as JSON object
std::string SnapshotToJson(const ProcessSnapshot& s);

} // namespace pc
