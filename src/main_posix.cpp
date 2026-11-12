#include "bt_manager.h"
#include "common.h"
#include "ipc_server.h"
#include "mcp_server.h"
#include "process_manager.h"
#include "taskqueue.h"

#ifndef _WIN32

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::atomic<bool> g_stop{false};

void HandleSignal(int) {
    g_stop = true;
}

void PrintUsage() {
    std::printf(
        "Hancho " PC_VERSION " - headless POSIX core\n"
        "\n"
        "Usage: Hancho [--headless] [--data-dir <path>]\n"
        "       Hancho --version\n"
        "       Hancho mcp\n"
        "\n"
        "The POSIX build runs the core (task queue, behaviour trees, IPC) without a GUI.\n"
        "Use `hancho-cli top` from another terminal to watch it.\n");
}

} // namespace

int main(int argc, char** argv) {
    std::string dataDir;
    bool headless = true;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--headless" || arg == "gui") {
            headless = true;
        } else if (arg == "--data-dir" && i + 1 < argc) {
            dataDir = argv[++i];
        } else if (arg.rfind("--data-dir=", 0) == 0) {
            dataDir = arg.substr(11);
        } else if (arg == "--webui-port" && i + 1 < argc) {
            setenv("HANCHO_WEBUI_PORT", argv[++i], 1);
        } else if (arg.rfind("--webui-port=", 0) == 0) {
            setenv("HANCHO_WEBUI_PORT", arg.substr(13).c_str(), 1);
        } else if (arg == "--webui-dir" && i + 1 < argc) {
            setenv("HANCHO_WEBUI_DIR", argv[++i], 1);
        } else if (arg.rfind("--webui-dir=", 0) == 0) {
            setenv("HANCHO_WEBUI_DIR", arg.substr(12).c_str(), 1);
        } else if (arg == "--version" || arg == "-V") {
            std::printf("Hancho %s\n", PC_VERSION);
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            PrintUsage();
            return 0;
        } else if (arg == "mcp") {
            return pc::RunMcpServer();
        } else {
            std::fprintf(stderr, "Unknown option: %s\n", arg.c_str());
            PrintUsage();
            return 1;
        }
    }
    (void)headless;

    if (!dataDir.empty()) pc::SetDataDirectoryOverride(pc::Utf8ToWide(dataDir));

    auto lock = pc::platform::CreateInstanceLock(pc::WideToUtf8(pc::GetGuiMutexNameWide()));
    std::string lockError;
    if (!lock->Acquire(lockError)) {
        std::fprintf(stderr, "Hancho is already running for this data directory: %s\n",
                     lockError.c_str());
        return 1;
    }

    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    pc::ProcessManager pm;
    pc::TaskQueueManager tqm;
    tqm.SetProcessManager(&pm);
    tqm.SetProcessFinishedCallback([&tqm](int pid, int ec, const std::string& kind) {
        tqm.OnProcessFinished(pid, ec, kind);
    });

    pc::BtManager btm(pm, tqm);
    pc::IpcServer server(pm, tqm, &btm);
    server.SetHeadless(true);
    server.SetShutdownCallback([] { g_stop = true; });

    if (!server.Start()) {
        std::fprintf(stderr, "Failed to start IPC server on %s\n",
                     pc::GetIpcEndpointUtf8().c_str());
        return 1;
    }
    std::fprintf(stderr, "Hancho headless core listening on %s\n",
                 pc::GetIpcEndpointUtf8().c_str());

    while (!g_stop.load()) pc::SleepMs(200);

    server.Stop();
    btm.Shutdown();
    tqm.ShutdownTaskQueueManager();
    lock->Release();
    return 0;
}

#endif // !_WIN32
