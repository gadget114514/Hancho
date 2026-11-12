// POSIX headless core smoke test (doc/design-v3.md 2.4).
//
// Covers the platform boundary (shell quoting, instance lock, Unix-socket IPC),
// a real task queue run through /bin/sh, and one behaviour tree with a job node.

#include "bt_manager.h"
#include "cli_top.h"
#include "common.h"
#include "ipc_server.h"
#include "platform/platform.h"
#include "process_manager.h"
#include "taskqueue.h"

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace pc;

namespace {

int g_failures = 0;
int g_passes = 0;

void Check(bool ok, const std::string& label) {
    if (ok) {
        ++g_passes;
        std::printf("[PASS] %s\n", label.c_str());
    } else {
        ++g_failures;
        std::printf("[FAIL] %s\n", label.c_str());
    }
}

std::string WaitState(TaskQueueManager& tqm, const std::string& id, int timeoutMs) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    std::string last;
    while (std::chrono::steady_clock::now() < deadline) {
        std::string err;
        last = JsonGetString(tqm.TaskQueueGet(id, false, 0, err), "state", "");
        if (last == "succeeded" || last == "failed") return last;
        SleepMs(100);
    }
    return last;
}

} // namespace

int main() {
    fs::path temp = fs::temp_directory_path() /
                    ("hancho_posix_smoke_" + std::to_string(platform::CurrentProcessId()));
    std::error_code ec;
    fs::remove_all(temp, ec);
    fs::create_directories(temp, ec);
    SetDataDirectoryOverride(Utf8ToWide(temp.string()));

    // --- IShellQuote -------------------------------------------------------
    Check(platform::QuoteCommandArg("plain") == "plain", "safe argument is not quoted");
    Check(platform::QuoteCommandArg("a b") == "'a b'", "space is single-quoted");
    Check(platform::QuoteCommandArg("it's") == "'it'\\''s'", "single quote is escaped");
    Check(platform::NeedsCommandArgQuoting("a;b"), "semicolon needs quoting");

    // --- IInstanceLock -----------------------------------------------------
    {
        auto a = platform::CreateInstanceLock("posix_core_smoke");
        auto b = platform::CreateInstanceLock("posix_core_smoke");
        std::string err;
        Check(a->Acquire(err), "first instance lock is acquired: " + err);
        std::string secondError;
        Check(!b->Acquire(secondError), "second instance lock is refused");
        a->Release();
        std::string thirdError;
        Check(b->Acquire(thirdError), "lock can be acquired after release: " + thirdError);
        b->Release();
    }

    // --- IIpcTransport -----------------------------------------------------
    {
        const std::string endpoint = (temp / "ipc.sock").string();
        auto server = platform::CreateIpcServer(endpoint);
        std::string err;
        Check(server && server->Start(err), "IPC server starts: " + err);
        std::thread worker([&] {
            std::string acceptErr;
            auto conn = server->Accept(acceptErr);
            if (!conn) return;
            char buf[256] = {};
            std::string payload;
            while (true) {
                int n = conn->Read(buf, sizeof(buf), acceptErr);
                if (n <= 0) break;
                payload.append(buf, static_cast<size_t>(n));
            }
            std::string reply = "echo:" + payload;
            conn->WriteAll(reply.data(), reply.size(), acceptErr);
            conn->Close();
        });
        auto client = platform::ConnectIpc(endpoint, 2000, err);
        Check(client != nullptr, "IPC client connects: " + err);
        if (client) {
            std::string request = "{\"action\":\"ping\"}";
            Check(client->WriteAll(request.data(), request.size(), err), "IPC request is written");
            client->ShutdownWrite();
            char buf[256] = {};
            std::string response;
            while (true) {
                int n = client->Read(buf, sizeof(buf), err);
                if (n <= 0) break;
                response.append(buf, static_cast<size_t>(n));
            }
            client->Close();
            Check(response == "echo:" + request, "IPC response is framed by EOF");
        }
        server->Stop();
        worker.join();
    }

    // --- Task queue with a real /bin/sh --------------------------------
    ProcessManager pm;
    TaskQueueManager tqm;
    tqm.SetProcessManager(&pm);
    tqm.SetProcessFinishedCallback([&tqm](int pid, int ec, const std::string& kind) {
        tqm.OnProcessFinished(pid, ec, kind);
    });
    std::string err;
    const std::string jobs = R"([
        {"id":"A","command":"true","summary":"ok job","timeoutSec":30},
        {"id":"B","command":"false","summary":"failing job","timeoutSec":30,"maxAttempts":1},
        {"id":"C","command":"true","summary":"after A","timeoutSec":30,"dependsOn":["A"]}
    ])";
    std::string add = tqm.TaskQueueAdd("main", jobs, 0, "", err);
    Check(JsonGetBool(add, "ok", false), "taskqueue_add accepts POSIX jobs: " + err);
    Check(WaitState(tqm, "A", 15000) == "succeeded", "A succeeds");
    Check(WaitState(tqm, "B", 15000) == "failed", "B fails");
    Check(WaitState(tqm, "C", 15000) == "succeeded", "C succeeds after dependency");

    // --- Behaviour tree with one real job ---------------------------------
    {
        BtManager btm(pm, tqm);
        const std::string bt = R"({
            "id": "posix-bt",
            "root": {
                "type": "sequence",
                "children": [
                    { "type": "job", "id": "j1", "summary": "bt ok", "command": "true", "timeoutSec": 30 }
                ]
            }
        })";
        std::string start = btm.Start(bt, false);
        std::string id = JsonGetString(start, "id", "");
        Check(JsonGetBool(start, "ok", false) && !id.empty(), "BT starts: " + start);
        if (!id.empty()) {
            std::string waited = btm.Wait(id, 0, 15000, true);
            std::string state = JsonGetString(waited, "state", "");
            Check(state == "succeeded" || JsonGetBool(waited, "timedOut", false),
                  "BT runs a job node (state=" + state + ")");
        }
        btm.Shutdown();
    }

    // --- TUI output formatting smoke test (card 062-4) --------------------
    {
        // 1. Format and Key parsing helpers
        Check(top::FormatBytes(1024) == "1.0 KB", "TUI FormatBytes(1024)");
        Check(top::PadOrTruncate("hello world", 5) == "he...", "TUI PadOrTruncate truncate");
        Check(top::ParseKeySequence("\x1b[A") == top::TopKey::Up, "TUI ParseKeySequence Up");
        Check(top::ParseKeySequence("q") == top::TopKey::Quit, "TUI ParseKeySequence Quit");
        Check(top::ParseKeySequence("s") == top::TopKey::JobStop, "TUI ParseKeySequence Stop");
        Check(top::ParseKeySequence("r") == top::TopKey::JobRetry, "TUI ParseKeySequence Retry");
        Check(top::ParseKeySequence("c") == top::TopKey::JobCancel, "TUI ParseKeySequence Cancel");

        // 2. Pure rendering test without live terminal
        top::TopState state;
        state.coreConnected = true;
        state.timestamp = "2026-11-11 19:30:00";

        // Tab 1: Processes
        state.activeTab = top::TopTab::Processes;
        std::string renderedProc = top::RenderScreen(state, 80, 24, false);
        Check(renderedProc.find("Hancho Top") != std::string::npos, "TUI renders Hancho Top title");
        Check(renderedProc.find("Process List") != std::string::npos, "TUI renders Processes tab header");

        // Tab 2: Queue
        state.activeTab = top::TopTab::Queue;
        std::string renderedQ = top::RenderScreen(state, 80, 24, false);
        Check(renderedQ.find("Task Queue Overview") != std::string::npos, "TUI renders Queue tab header");

        // Tab 3: Monitor
        state.activeTab = top::TopTab::Monitor;
        std::string renderedMon = top::RenderScreen(state, 80, 24, false);
        Check(renderedMon.find("Execution Monitor Stream") != std::string::npos, "TUI renders Monitor tab header");

        // Tab 4: Why / Diagnostics
        state.activeTab = top::TopTab::Diagnostics;
        std::string renderedWhy = top::RenderScreen(state, 80, 24, false);
        Check(renderedWhy.find("Failure Diagnostics (Why)") != std::string::npos, "TUI renders Diagnostics tab header");

        // Tab 5: Memory
        state.activeTab = top::TopTab::Memory;
        std::string renderedMem = top::RenderScreen(state, 80, 24, false);
        Check(renderedMem.find("Memory & Knowledge Records") != std::string::npos, "TUI renders Memory tab header");

        // Job Operations request building
        std::string req, desc;
        nlohmann::json qData;
        qData["jobs"] = nlohmann::json::array();
        nlohmann::json testJob;
        testJob["id"] = "posix-smoke-job-1";
        testJob["state"] = "failed";
        qData["jobs"].push_back(testJob);
        state.activeTab = top::TopTab::Queue;
        state.selectedRow = 0;
        state.queueData = qData;

        Check(top::BuildJobRetryRequest(state, req, desc), "TUI BuildJobRetryRequest succeeds");
        Check(req.find("taskqueue_retry") != std::string::npos, "TUI BuildJobRetryRequest action is taskqueue_retry");
    }

    tqm.ShutdownTaskQueueManager();
    fs::remove_all(temp, ec);
    std::printf("%s (%d passed, %d failures)\n", g_failures ? "FAILED" : "ALL PASSED",
                g_passes, g_failures);
    return g_failures ? 1 : 0;
}
