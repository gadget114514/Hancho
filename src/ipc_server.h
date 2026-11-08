#pragma once

#include "process_manager.h"
#include "taskqueue.h"
#include <thread>
#include <atomic>
#include <string>

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

    // Execute a JSON request directly against ProcessManager / TaskQueueManager / BtManager
    // and return a JSON response.
    std::string HandleRequestJson(const std::string& reqJson);

private:
    void AcceptLoop();
    void HandleClient(HANDLE hPipe);

    ProcessManager& pm_;
    TaskQueueManager& tqm_;
    BtManager* btm_ = nullptr;
    std::atomic<HWND> notifyHwnd_{nullptr};
    std::atomic<bool> stop_{false};
    std::thread acceptThread_;
};

// Format ProcessSnapshot as JSON object
std::string SnapshotToJson(const ProcessSnapshot& s);

} // namespace pc
