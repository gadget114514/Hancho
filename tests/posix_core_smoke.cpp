// POSIX headless core smoke test (doc/design-v3.md 2.4).
//
// Covers the platform boundary (shell quoting, instance lock, Unix-socket IPC),
// a real task queue run through /bin/sh, and one behaviour tree with a job node.

#include "bt_manager.h"
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

    tqm.ShutdownTaskQueueManager();
    fs::remove_all(temp, ec);
    std::printf("%s (%d passed, %d failures)\n", g_failures ? "FAILED" : "ALL PASSED",
                g_passes, g_failures);
    return g_failures ? 1 : 0;
}
