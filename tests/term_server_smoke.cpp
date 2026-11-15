// Functional test for the WCSP terminal server (src/term_server.cpp).
//
// Usage:
//   term_server_smoke --data-dir <dir> [--id <process-id>]
//
// The isolated core must already be running and a ConPTY job (e.g. named "t1")
// must exist. The test asks the local IPC endpoint for the WCSP listener address,
// attaches to the job with RemoteSession, checks the replay, sends input, then
// disconnects and verifies over IPC that the job is still running.

#include "common.h"
#include "ipc_client.h"
#include "nlohmann/json.hpp"
#include "wcsp/remote_session.h"

#include <windows.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace pc;
using json = nlohmann::json;

namespace {

int g_failures = 0;
int g_passes = 0;

void Check(bool ok, const std::string& msg) {
    if (ok) {
        ++g_passes;
        std::printf("PASS: %s\n", msg.c_str());
    } else {
        ++g_failures;
        std::fprintf(stderr, "FAIL: %s\n", msg.c_str());
    }
}

bool IpcCall(const json& req, json& out, std::string* outError = nullptr) {
    std::string resp;
    std::string err;
    if (!SendIpcRequest(req.dump(), resp, err, false)) {
        if (outError) *outError = err;
        return false;
    }
    out = json::parse(resp, nullptr, false);
    if (out.is_discarded()) {
        if (outError) *outError = "invalid JSON response: " + resp;
        return false;
    }
    return true;
}

bool FindProcess(int id, json& outProcess) {
    json list;
    if (!IpcCall({{"action", "list"}}, list) || !list.value("ok", false)) return false;
    if (!list.contains("processes") || !list["processes"].is_array()) return false;
    for (const auto& p : list["processes"]) {
        if (p.value("id", 0) == id) {
            outProcess = p;
            return true;
        }
    }
    return false;
}

} // namespace

int main(int argc, char** argv) {
    std::wstring dataDir;
    int forcedId = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--data-dir" && i + 1 < argc) {
            dataDir = Utf8ToWide(argv[++i]);
        } else if (arg == "--id" && i + 1 < argc) {
            forcedId = std::atoi(argv[++i]);
        }
    }
    if (dataDir.empty()) {
        std::fprintf(stderr, "usage: term_server_smoke --data-dir <dir> [--id <process-id>]\n");
        return 2;
    }
    SetDataDirectoryOverride(dataDir);

    json endpoint;
    Check(IpcCall({{"action", "term_endpoint"}}, endpoint) && endpoint.value("ok", false),
          "core reports a WCSP endpoint");
    if (!endpoint.value("ok", false)) {
        std::fprintf(stderr, "term_endpoint failed: %s\n", endpoint.dump().c_str());
        return 1;
    }
    const std::string host = endpoint.value("host", "127.0.0.1");
    const int port = endpoint.value("port", 0);
    const std::string token = endpoint.value("token", "");
    Check(port > 0 && port <= 65535, "endpoint port is valid");
    Check(!token.empty(), "endpoint has a token");

    int jobId = forcedId;
    if (jobId <= 0) {
        json list;
        int fallback = 0;
        if (IpcCall({{"action", "list"}}, list) && list.value("ok", false) &&
            list.contains("processes") && list["processes"].is_array()) {
            for (const auto& p : list["processes"]) {
                if (p.value("mode", "") != "pty") continue;
                if (p.value("state", "") != "running") continue;
                int id = p.value("id", 0);
                if (id <= 0) continue;
                if (fallback == 0) fallback = id;
                if (p.value("name", "") == "t1") {
                    jobId = id;
                    break;
                }
            }
        }
        if (jobId <= 0) jobId = fallback;
    }
    Check(jobId > 0, "found a running ConPTY job");
    if (jobId <= 0) return 1;

    RemoteConfig cfg;
    cfg.host = host;
    cfg.port = static_cast<uint16_t>(port);
    cfg.token = token;

    std::mutex outputMutex;
    std::string output;
    RemoteSession session(cfg);
    session.SetOutputSink([&](const char* data, size_t len) {
        std::lock_guard<std::mutex> lock(outputMutex);
        output.append(data, len);
    });

    TerminalStartOptions opt;
    opt.shell = L"attach:" + std::to_wstring(jobId);
    opt.cols = 80;
    opt.rows = 25;

    std::wstring error;
    Check(session.Start(opt, &error), "RemoteSession attaches to attach:" +
                                          std::to_string(jobId));
    Check(session.JobId() == static_cast<uint32_t>(jobId),
          "HELLO_ACK reports the attached job id");

    auto waitFor = [&](const std::string& needle, int timeoutMs) {
        const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeoutMs);
        while (GetTickCount() < deadline) {
            {
                std::lock_guard<std::mutex> lock(outputMutex);
                if (output.find(needle) != std::string::npos) return true;
            }
            Sleep(50);
        }
        return false;
    };

    const bool gotHello = waitFor("hello-wcsp", 10000);
    {
        std::lock_guard<std::mutex> lock(outputMutex);
        std::printf("first replay bytes=%zu\n", output.size());
    }
    Check(gotHello, "replay contains hello-wcsp");

    // The Settings active-session list uses the term_sessions IPC action.
    {
        json active;
        Check(IpcCall({{"action", "term_sessions"}}, active) && active.value("ok", false),
              "term_sessions answers");
        int activeCount = 0;
        if (active.contains("sessions") && active["sessions"].is_array()) {
            activeCount = static_cast<int>(active["sessions"].size());
        }
        Check(activeCount > 0, "term_sessions lists the active session");
    }

    // Multi-session: a second client attaches to the same job and both receive
    // the live output. This is what the tabbed terminal client uses.
    std::mutex output2Mutex;
    std::string output2;
    RemoteSession session2(cfg);
    session2.SetOutputSink([&](const char* data, size_t len) {
        std::lock_guard<std::mutex> lock(output2Mutex);
        output2.append(data, len);
    });
    Check(session2.Start(opt, &error), "a second client attaches to the same job");

    auto waitFor2 = [&](const std::string& needle, int timeoutMs) {
        const DWORD deadline = GetTickCount() + static_cast<DWORD>(timeoutMs);
        while (GetTickCount() < deadline) {
            {
                std::lock_guard<std::mutex> lock(output2Mutex);
                if (output2.find(needle) != std::string::npos) return true;
            }
            Sleep(50);
        }
        return false;
    };

    Check(waitFor2("hello-wcsp", 10000), "second session replay contains hello-wcsp");

    const std::string command2 = "echo second-session\r";
    session2.Write(command2.data(), command2.size());
    Check(waitFor("second-session", 10000), "first session sees input from the second");
    Check(waitFor2("second-session", 10000), "second session sees its own echo");

    session2.Close();
    Sleep(300);

    const std::string command = "echo second\r";
    session.Write(command.data(), command.size());
    Check(waitFor("second", 10000), "first session keeps working after the second closes");

    session.Close();

    Sleep(700);
    json process;
    bool stillThere = FindProcess(jobId, process);
    Check(stillThere, "job is still listed after the client disconnects");
    Check(stillThere && process.value("state", "") == "running",
          "disconnecting an attach session does not kill the job");

    // Retained sessions and the LIST_SESSIONS control session.
    std::mutex listMutex;
    std::condition_variable listCv;
    bool listDone = false;
    std::vector<wcs::SessionInfo> sessions;
    RemoteSession control(cfg);
    control.onSessionList = [&](const std::vector<wcs::SessionInfo>& list) {
        std::lock_guard<std::mutex> lock(listMutex);
        sessions = list;
        listDone = true;
        listCv.notify_all();
    };
    TerminalStartOptions controlOpt;
    controlOpt.shell = L"list";
    controlOpt.cols = 80;
    controlOpt.rows = 25;
    Check(control.Start(controlOpt, &error), "control session starts");
    control.RequestSessionList();
    {
        std::unique_lock<std::mutex> lock(listMutex);
        listDone = listCv.wait_for(lock, std::chrono::seconds(3), [&] { return listDone; });
    }
    Check(listDone, "SESSION_LIST is received");
    bool listedJob = false;
    for (const auto& info : sessions) {
        if (info.id == static_cast<uint32_t>(jobId)) listedJob = true;
    }
    Check(listedJob, "SESSION_LIST contains the attached job");
    control.Close();

    // A server-spawned shell survives a disconnect, is listed, can be
    // reattached, and CLOSE terminates it.
    RemoteSession spawned(cfg);
    TerminalStartOptions spawnOpt;
    spawnOpt.shell = L"cmd";
    spawnOpt.cols = 80;
    spawnOpt.rows = 25;
    Check(spawned.Start(spawnOpt, &error), "spawned cmd session starts");
    Sleep(1000);
    int spawnedId = 0;
    {
        json list;
        if (IpcCall({{"action", "list"}}, list) && list.value("ok", false)) {
            for (const auto& p : list["processes"]) {
                if (p.value("name", "") == "wcsp-cmd" && p.value("state", "") == "running") {
                    int id = p.value("id", 0);
                    if (id > spawnedId) spawnedId = id;
                }
            }
        }
    }
    Check(spawnedId > 0, "spawned session is listed by IPC");
    Check(spawned.JobId() == static_cast<uint32_t>(spawnedId),
          "HELLO_ACK reports the spawned job id");
    spawned.Close();
    Sleep(700);
    json spawnedProcess;
    bool stillRunning = FindProcess(spawnedId, spawnedProcess) &&
                        spawnedProcess.value("state", "") == "running";
    Check(stillRunning, "spawned session is retained after disconnect");

    RemoteSession reattached(cfg);
    TerminalStartOptions reattachOpt;
    reattachOpt.shell = L"attach:" + std::to_wstring(spawnedId);
    reattachOpt.cols = 80;
    reattachOpt.rows = 25;
    Check(reattached.Start(reattachOpt, &error), "retained spawned session can be reattached");
    Check(reattached.JobId() == static_cast<uint32_t>(spawnedId),
          "HELLO_ACK reports the reattached job id");
    reattached.TerminateSession();
    Sleep(700);
    bool gone = !FindProcess(spawnedId, spawnedProcess) ||
                spawnedProcess.value("state", "") != "running";
    Check(gone, "CLOSE terminates a retained spawned session");

    // WCSP 1.2 FeatureSizeSync: a RESIZE from one client is forwarded to the
    // other clients attached to the same job as RESIZED{reason=Peer}. A client
    // that did not advertise the feature receives no Peer frame.
    {
        struct ResizeSink {
            std::mutex mutex;
            std::condition_variable cv;
            std::vector<wcs::ResizedInfo> events;

            void Add(const wcs::ResizedInfo& info) {
                std::lock_guard<std::mutex> lock(mutex);
                events.push_back(info);
                cv.notify_all();
            }
            bool WaitFor(wcs::ResizeReason reason, uint16_t cols, uint16_t rows,
                         int timeoutMs) {
                std::unique_lock<std::mutex> lock(mutex);
                return cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
                    for (const auto& e : events) {
                        if (e.reason == reason && e.cols == cols && e.rows == rows)
                            return true;
                    }
                    return false;
                });
            }
            bool HasReason(wcs::ResizeReason reason) {
                std::lock_guard<std::mutex> lock(mutex);
                for (const auto& e : events) {
                    if (e.reason == reason) return true;
                }
                return false;
            }
        };

        ResizeSink sinkA;
        ResizeSink sinkB;
        ResizeSink sinkLegacy;

        RemoteConfig syncCfg = cfg;
        syncCfg.features = wcs::kDefaultFeatures;  // includes FeatureSizeSync
        RemoteSession syncA(syncCfg);
        RemoteSession syncB(syncCfg);
        syncA.SetResizedHandler([&](const wcs::ResizedInfo& info) { sinkA.Add(info); });
        syncB.SetResizedHandler([&](const wcs::ResizedInfo& info) { sinkB.Add(info); });

        RemoteConfig legacyCfg = cfg;
        legacyCfg.features = wcs::kDefaultFeatures & ~wcs::FeatureSizeSync;
        RemoteSession legacy(legacyCfg);
        legacy.SetResizedHandler([&](const wcs::ResizedInfo& info) { sinkLegacy.Add(info); });

        TerminalStartOptions sizeOpt;
        sizeOpt.shell = L"attach:" + std::to_wstring(jobId);
        sizeOpt.cols = 80;
        sizeOpt.rows = 25;

        Check(syncA.Start(sizeOpt, &error), "SizeSync client A attaches");
        Check(syncB.Start(sizeOpt, &error), "SizeSync client B attaches");
        Check(legacy.Start(sizeOpt, &error), "legacy client attaches without SizeSync");

        // B's handshake also resizes the job; that may already notify A.
        Sleep(200);
        syncB.Resize(100, 30);
        Check(sinkA.WaitFor(wcs::ResizeReason::Peer, 100, 30, 3000),
              "SizeSync peer A receives RESIZED(Peer) from B's resize");

        syncA.Resize(90, 20);
        Check(sinkB.WaitFor(wcs::ResizeReason::Peer, 90, 20, 3000),
              "SizeSync peer B receives RESIZED(Peer) from A's resize");

        Sleep(300);
        Check(!sinkLegacy.HasReason(wcs::ResizeReason::Peer),
              "a client without FeatureSizeSync receives no Peer RESIZED");

        syncA.Close();
        syncB.Close();
        legacy.Close();
    }

    std::printf("\n%d passed, %d failed\n", g_passes, g_failures);
    return g_failures == 0 ? 0 : 1;
}
