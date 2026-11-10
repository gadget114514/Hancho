// Headless smoke test for HANCHO_EVENT_PIPE: a pipe-mode worker receives the pipe name in its
// environment and its emitted hancho.event/1 lines land in <log>.events.jsonl.

#include "../src/common.h"
#include "../src/event_adapter.h"
#include "../src/event_schema.h"
#include "../src/process_manager.h"
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace pc;

static int g_failures = 0;

static void Check(bool cond, const std::string& label) {
    std::printf("%s %s\n", cond ? "[PASS]" : "[FAIL]", label.c_str());
    std::fflush(stdout);
    if (!cond) g_failures++;
}

static std::string ExeDir() {
    wchar_t path[MAX_PATH] = {};
    DWORD len = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring full(path, len);
    size_t slash = full.find_last_of(L"\\/");
    return WideToUtf8(slash == std::wstring::npos ? full : full.substr(0, slash));
}

int main() {
    SetConsoleOutputCP(CP_UTF8);

    std::filesystem::path temp = std::filesystem::temp_directory_path() /
                                 ("hancho_event_pipe_" + std::to_string(GetCurrentProcessId()));
    std::error_code ec;
    std::filesystem::remove_all(temp, ec);
    std::filesystem::create_directories(temp, ec);
    SetDataDirectoryOverride(temp.wstring());

    ProcessManager pm;
    StartProcessRequest req;
    req.name = "pipe-smoke";
    req.agentType = "mycli";
    req.jsonStream = true;
    req.timeoutSec = 20;
    req.commandLine = "\"" + ExeDir() + "\\hancho-cli.exe\" event emit --kind message --text hello";
    std::string error;
    int id = pm.StartProcess(req, error);
    Check(id > 0, "pipe-mode worker starts: " + error);
    if (id <= 0) {
        SetDataDirectoryOverride(L"");
        std::filesystem::remove_all(temp, ec);
        return 1;
    }

    Check(pm.WaitForExit(id, 15000), "event emitter exits");

    ProcessSnapshot snap;
    std::string eventsPath;
    for (int i = 0; i < 100 && eventsPath.empty(); ++i) {
        if (pm.GetSnapshot(id, snap)) eventsPath = HanchoEventsFilePathForStream(snap.streamFilePath);
        if (eventsPath.empty()) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    Check(!eventsPath.empty(), "events file path is derived from the worker");

    std::vector<nlohmann::json> events;
    for (int attempt = 0; attempt < 60; ++attempt) {
        events.clear();
        std::ifstream in(Utf8ToWide(eventsPath), std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (line.empty() || line == "\r") continue;
            auto parsed = nlohmann::json::parse(line, nullptr, false);
            if (parsed.is_object()) events.push_back(parsed);
        }
        if (!events.empty()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    Check(!events.empty(), "the emitted event reaches the events file");
    if (!events.empty()) {
        Check(events[0]["schema"] == "hancho.event/1" && events[0]["kind"] == "message" &&
                  events[0]["data"]["text"] == "hello",
              "the event keeps its kind and data");
        Check(events[0]["source"]["processId"] == id, "source.processId is stamped by Hancho");
        Check(events[0]["agent"]["type"] == "mycli", "agent.type is the worker's agent type");
    }

    // Tier 1 adapter path: a CLI's plain JSON lines are normalized through <data-dir>/adapters/<name>.json.
    {
        std::filesystem::create_directories(temp / "adapters", ec);
        std::ofstream adapterFile(temp / "adapters" / "mycli.json", std::ios::binary);
        adapterFile << R"({
  "adapter": "mycli",
  "match": { "field": "/type" },
  "rules": [
    { "when": { "/type": "tool_start" }, "kind": "tool.call",
      "map": { "tool": "/name", "target": "/args/path", "toolUseId": "/id" },
      "activity": { "from": "/name", "table": { "read_file": "read" }, "default": "tool" } },
    { "when": { "/type": "done" }, "kind": "run.ended", "map": { "status": "/status" } }
  ]
})";
        adapterFile.close();

        std::filesystem::path script = temp / "emit.cmd";
        std::ofstream scriptFile(script, std::ios::binary);
        scriptFile << "@echo {\"type\":\"tool_start\",\"id\":\"t1\",\"name\":\"read_file\",\"args\":{\"path\":\"src/a.cpp\"}}\r\n";
        scriptFile << "@echo {\"type\":\"done\",\"status\":\"completed\"}\r\n";
        scriptFile.close();

        StartProcessRequest areq;
        areq.name = "adapter-smoke";
        areq.agentType = "mycli";
        areq.jsonStream = true;
        areq.timeoutSec = 20;
        areq.commandLine = "cmd /c \"" + WideToUtf8(script.wstring()) + "\"";
        std::string aerr;
        int aid = pm.StartProcess(areq, aerr);
        Check(aid > 0, "adapter worker starts: " + aerr);
        if (aid > 0) {
            Check(pm.WaitForExit(aid, 15000), "adapter worker exits");
            ProcessSnapshot asnap;
            std::string apath;
            if (pm.GetSnapshot(aid, asnap)) apath = HanchoEventsFilePathForStream(asnap.streamFilePath);
            std::vector<nlohmann::json> aevents;
            for (int attempt = 0; attempt < 60; ++attempt) {
                aevents.clear();
                std::ifstream in(Utf8ToWide(apath), std::ios::binary);
                std::string aline;
                while (std::getline(in, aline)) {
                    if (aline.empty() || aline == "\r") continue;
                    auto parsed = nlohmann::json::parse(aline, nullptr, false);
                    if (parsed.is_object()) aevents.push_back(parsed);
                }
                if (aevents.size() >= 2) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            Check(aevents.size() == 2, "adapter lines produce two normalized events");
            if (aevents.size() == 2) {
                Check(aevents[0]["kind"] == "tool.call" && aevents[0]["data"]["tool"] == "read_file" &&
                          aevents[0]["data"]["activity"] == "read" &&
                          aevents[0]["data"]["target"] == "src/a.cpp",
                      "adapter tool_start becomes tool.call with activity and target");
                Check(aevents[1]["kind"] == "run.ended" && aevents[1]["data"]["status"] == "completed",
                      "adapter done becomes run.ended");
            }
            std::string akill;
            pm.KillProcess(aid, akill);
        }
    }

    // Tier 3 adapter path: an external converter reads raw lines on stdin and writes hancho.event
    // lines on stdout.
    {
        std::filesystem::create_directories(temp / "adapters", ec);
        std::filesystem::path ps1 = temp / "tier3.ps1";
        std::ofstream ps1File(ps1, std::ios::binary);
        ps1File << "$reader = [Console]::In\r\n"
                   "while ($true) {\r\n"
                   "  $line = $reader.ReadLine()\r\n"
                   "  if ($line -eq $null) { break }\r\n"
                   "  $obj = @{ kind = 'raw'; data = @{ text = $line } }\r\n"
                   "  $obj | ConvertTo-Json -Compress -Depth 5\r\n"
                   "  [Console]::Out.Flush()\r\n"
                   "}\r\n";
        ps1File.close();

        nlohmann::json adapterJson;
        adapterJson["adapter"] = "tier3";
        adapterJson["exec"] = "powershell -NoProfile -ExecutionPolicy Bypass -File \"" +
                               WideToUtf8(ps1.wstring()) + "\"";
        std::ofstream adapterFile(temp / "adapters" / "tier3.json", std::ios::binary);
        adapterFile << adapterJson.dump(2);
        adapterFile.close();

        std::filesystem::path script = temp / "tier3.cmd";
        std::ofstream scriptFile(script, std::ios::binary);
        scriptFile << "@echo hello1\r\n@echo hello2\r\n";
        scriptFile.close();

        StartProcessRequest treq;
        treq.name = "tier3-smoke";
        treq.agentType = "tier3";
        treq.jsonStream = true;
        treq.timeoutSec = 30;
        treq.commandLine = "cmd /c \"" + WideToUtf8(script.wstring()) + "\"";
        std::string terr;
        int tid = pm.StartProcess(treq, terr);
        Check(tid > 0, "tier 3 worker starts: " + terr);
        if (tid > 0) {
            Check(pm.WaitForExit(tid, 20000), "tier 3 worker exits");
            ProcessSnapshot tsnap;
            std::string tpath;
            if (pm.GetSnapshot(tid, tsnap)) tpath = HanchoEventsFilePathForStream(tsnap.streamFilePath);
            std::vector<nlohmann::json> tevents;
            for (int attempt = 0; attempt < 100; ++attempt) {
                tevents.clear();
                std::ifstream in(Utf8ToWide(tpath), std::ios::binary);
                std::string tline;
                while (std::getline(in, tline)) {
                    if (tline.empty() || tline == "\r") continue;
                    auto parsed = nlohmann::json::parse(tline, nullptr, false);
                    if (parsed.is_object()) tevents.push_back(parsed);
                }
                if (tevents.size() >= 2) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            Check(tevents.size() == 2, "tier 3 converter produces two events");
            if (tevents.size() == 2) {
                Check(tevents[0]["kind"] == "raw" && tevents[0]["data"]["text"] == "hello1" &&
                          tevents[1]["data"]["text"] == "hello2",
                      "tier 3 converter events keep their kind and data");
            }
            std::string tkill;
            pm.KillProcess(tid, tkill);
        }
    }

    std::string killError;
    pm.KillProcess(id, killError);
    SetDataDirectoryOverride(L"");
    std::filesystem::remove_all(temp, ec);

    if (g_failures == 0) std::printf("ALL PASS\n");
    else std::printf("%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
