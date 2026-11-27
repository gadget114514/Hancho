#pragma once

#include "process_manager.h"
#include "taskqueue.h"
#include "remote_client.h"
#include <thread>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <string>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace pc {

class BtManager;
class RoutineManager;
class TriageManager;
class PermitDraftManager;
class WebUiServer;
class TermServer;
class IpmsgNode;
class IpmsgHandler;

// Serializes "restart this listener" requests that arrive from an IPC handler.
//
// The caller of an IPC request waits for the reply, so scheduling must never
// block. The stop/start pair therefore runs on a dedicated worker thread and
// requests that arrive while a restart is running are coalesced with a
// generation counter, instead of joining the previous restart inside the IPC
// handler (which used to freeze the dashboard).
class ListenerRestartScheduler {
public:
    using RestartFn = std::function<void()>;

    explicit ListenerRestartScheduler(RestartFn fn) : fn_(std::move(fn)) {}
    ~ListenerRestartScheduler();

    ListenerRestartScheduler(const ListenerRestartScheduler&) = delete;
    ListenerRestartScheduler& operator=(const ListenerRestartScheduler&) = delete;

    // Requests a restart. Returns immediately.
    void Schedule();
    // Stops the worker and waits for a running restart to finish.
    void Stop();

private:
    void Run();

    RestartFn fn_;
    std::mutex mutex_;
    std::condition_variable cv_;
    uint64_t generation_ = 0;
    bool stop_ = false;
    bool running_ = false;
    std::thread thread_;
};

class IpcServer {
public:
    explicit IpcServer(ProcessManager& pm, TaskQueueManager& tqm, HWND notifyHwnd = nullptr);
    IpcServer(ProcessManager& pm, TaskQueueManager& tqm, BtManager* btm, HWND notifyHwnd = nullptr);
    ~IpcServer();

    bool Start();
    void Stop();
    void SetNotifyHwnd(HWND hwnd) { notifyHwnd_ = hwnd; }
    void SetBtManager(BtManager* btm) { btm_ = btm; InstallRoutineValidator(); }

    // Called (from an IPC client thread) when a client asks the dashboard to
    // appear. The host posts a message to its own window instead of touching
    // the UI from the IPC thread.
    void SetGuiShowCallback(std::function<void()> cb) {
        std::lock_guard<std::mutex> lock(cbMutex_);
        guiShowCb_ = std::move(cb);
    }
    // Called when a client asks the core host to stop. Must return immediately.
    void SetShutdownCallback(std::function<void(bool cancelJobs)> cb) {
        std::lock_guard<std::mutex> lock(cbMutex_);
        shutdownCb_ = std::move(cb);
    }
    // Local power implementation used by `power_suspend` / `power_shutdown` (MCP / CLI).
    // action is "sleep", "hibernate" or "shutdown".
    using PowerCallback = std::function<bool(const std::string& action, std::string& outError)>;
    void SetPowerCallback(PowerCallback cb) {
        std::lock_guard<std::mutex> lock(cbMutex_);
        powerCb_ = std::move(cb);
    }
    // True when no dashboard window is owned by this process (started with --headless).
    void SetHeadless(bool headless) { headless_ = headless; }
    bool IsHeadless() const { return headless_; }

    WebUiServer* GetWebUiServer();
    void StartWebUiServer(const std::string& token = std::string());
    // Stops the WebUI server. With waitForHandlers=false the listening socket
    // is closed right away and in-flight handlers are detached instead of
    // waited for, so a stuck request cannot delay the caller. Detached servers
    // are parked until the core shuts down, which is when they are waited for.
    void StopWebUiServer(bool waitForHandlers = true);
    // Stops and restarts the WebUI server after a "webui" config change.
    // The request is handed to a helper thread so the config_set reply is sent
    // first and the caller is never blocked by the restart.
    void ScheduleWebUiRestart();

    // WCSP terminal server (ConPTY jobs exposed to the dashboard and, when
    // "terminal.remote" is enabled, to WinConsoleTerminal on another PC).
    void StartTermServer();
    void StopTermServer();
    // Stops and restarts the terminal server after a "terminal" config change.
    void ScheduleTermServerRestart();

    // IPMsg node. Always constructed so history and logs stay readable while
    // the feature is disabled.
    IpmsgNode* GetIpmsg() { return ipmsg_.get(); }
    // Reads "ipmsg" from <data-dir>/config.json and starts the node when enabled.
    // Failures are logged and the core keeps running.
    void StartIpmsg();
    void StopIpmsg();
    // Stops and restarts the node after an "ipmsg" config change. Runs on a
    // helper thread so the config_set reply is sent first.
    void ScheduleIpmsgRestart();

    // Execute a JSON request directly against ProcessManager / TaskQueueManager / BtManager
    // and return a JSON response.
    std::string HandleRequestJson(const std::string& reqJson);

private:
    void AcceptLoop();
    void InstallRoutineValidator();
    std::string HandleChatAction(const std::string& action, const std::string& reqJson);
    void StartRemoteListener();
    void StopRemoteListener();
    void InstallRemoteCallbacks();
    // Bodies of the restart workers (see ListenerRestartScheduler).
    void RestartWebUi();
    void RestartTermServer();
    void RestartIpmsg();
#ifdef _WIN32
    void HandleClient(HANDLE hPipe);
#else
    void HandleClient(std::unique_ptr<platform::IIpcConnection> conn);
    std::unique_ptr<platform::IIpcServer> transport_;
#endif

    ProcessManager& pm_;
    TaskQueueManager& tqm_;
    BtManager* btm_ = nullptr;
    std::unique_ptr<RoutineManager> routineManager_;
    std::unique_ptr<TriageManager> triageManager_;
    std::unique_ptr<PermitDraftManager> permitDraft_;
    std::atomic<HWND> notifyHwnd_{nullptr};
    std::function<void()> guiShowCb_;
    std::function<void(bool)> shutdownCb_;
    PowerCallback powerCb_;
    std::mutex cbMutex_;
    bool headless_ = false;
    std::atomic<bool> stop_{false};
    std::thread acceptThread_;
    std::unique_ptr<remote::RemoteListener> remoteListener_;
    // Protects webUiServer_ and retiredWebUiServers_: the restart worker and
    // the IPC handlers both touch them.
    std::mutex webuiMutex_;
    std::unique_ptr<WebUiServer> webUiServer_;
    // Servers whose handlers were detached by a config change. They are kept
    // alive so the core can still wait for those handlers when it shuts down.
    std::vector<std::unique_ptr<WebUiServer>> retiredWebUiServers_;
    ListenerRestartScheduler webuiRestart_;
    std::unique_ptr<TermServer> termServer_;
    ListenerRestartScheduler termRestart_;
    std::unique_ptr<IpmsgNode> ipmsg_;
    std::unique_ptr<IpmsgHandler> ipmsgHandler_;
    ListenerRestartScheduler ipmsgRestart_;
    bool remoteCallbacksInstalled_ = false;
};

// Format ProcessSnapshot as JSON object
std::string SnapshotToJson(const ProcessSnapshot& s, bool full = true);

} // namespace pc
