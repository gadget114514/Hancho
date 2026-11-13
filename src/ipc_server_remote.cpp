#include "ipc_server.h"

#include "bt_manager.h"
#include "common.h"
#include "event_schema.h"
#include "ipmsg.h"
#include "ipmsg_handler.h"
#include "remote_hosts.h"
#include "webui_server.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <sstream>
#include <unordered_map>

namespace pc {

using RemoteFilesCheckCallback = std::function<bool(
    const std::string& host,
    const std::string& cwd,
    int remoteProcessId,
    const std::vector<std::string>& files,
    uint64_t startFiletime,
    std::string& outDetail)>;

void SetRemoteFilesCheckCallback(RemoteFilesCheckCallback cb);

namespace {

constexpr uint64_t kWindowsToUnixEpochSec = 11644473600ULL;
static std::mutex g_remoteStartFiletimesMutex;
static std::unordered_map<int, uint64_t> g_remoteStartFiletimes;

std::string ModeText(ExecMode mode) {
    switch (mode) {
    case ExecMode::ConPTY: return "conpty";
    case ExecMode::Console: return "console";
    case ExecMode::Pipe:
    default: return "pipe";
    }
}

std::string EndKindFromState(const std::string& state) {
    if (state == "failed") return "failed";
    if (state == "timed_out") return "timed_out";
    if (state == "idle_timed_out") return "idle_timed_out";
    if (state == "killed") return "killed";
    return "completed";
}

int RemoteStart(const std::string& host, const StartProcessRequest& req, std::string& outError) {
    nlohmann::json j;
    j["action"] = "start";
    j["name"] = req.name;
    j["cmd"] = req.commandLine;
    j["cwd"] = req.workingDir;
    j["mode"] = ModeText(req.mode);
    j["open_stdin"] = false;
    j["timeout_sec"] = req.timeoutSec;
    j["idle_timeout_sec"] = req.idleTimeoutSec;
    if (!req.estimatedEndTimeStr.empty()) j["estimate_end"] = req.estimatedEndTimeStr;
    j["agent_type"] = req.agentType;
    j["json_stream"] = req.jsonStream;
    j["supervisor_notes"] = req.supervisorNotes;
    j["supervisor_goal"] = req.supervisorGoal;
    j["job_key"] = req.jobKey;
    j["job_id"] = req.jobId;
    j["attempt"] = req.attemptN;
    if (!req.instructionsText.empty()) {
        j["instructions_text"] = req.instructionsText;
        j["instructions_rev"] = req.instructionsRev;
    }
    // The job already passed the local chain guard when it was added.
    j["allow_chain"] = true;
    if (!req.env.empty()) {
        nlohmann::json env = nlohmann::json::object();
        for (const auto& kv : req.env) env[kv.first] = kv.second;
        j["env"] = std::move(env);
    }

    std::string response;
    if (!remote::CallHost(host, j.dump(), response, outError)) return 0;

    nlohmann::json r = nlohmann::json::parse(response, nullptr, false);
    if (r.is_discarded() || !r.is_object()) {
        outError = "remote host returned invalid JSON";
        return 0;
    }
    if (!r.value("ok", false)) {
        outError = r.value("error", std::string("remote start failed"));
        return 0;
    }
    int id = r.value("id", 0);
    if (id <= 0) {
        outError = "remote start returned no process id";
        return 0;
    }
    return id;
}

bool RemotePoll(const std::string& host, int remoteId, RemotePollResult& out, std::string& outError) {
    nlohmann::json waitReq;
    waitReq["action"] = "wait";
    waitReq["id"] = remoteId;
    waitReq["wait_ms"] = 200;

    std::string response;
    if (!remote::CallHost(host, waitReq.dump(), response, outError)) return false;
    nlohmann::json r = nlohmann::json::parse(response, nullptr, false);
    if (r.is_discarded() || !r.is_object()) {
        outError = "remote wait returned invalid JSON";
        return false;
    }
    if (!r.value("ok", false)) {
        outError = r.value("error", std::string("remote wait failed"));
        return false;
    }

    out.finished = r.value("finished", false);
    if (r.contains("process") && r["process"].is_object()) {
        const auto& proc = r["process"];
        out.exitCode = proc.value("exit_code", -1);
        out.endKind = EndKindFromState(proc.value("state", std::string("completed")));
    }
    out.output = r.value("output", std::string());
    if (!out.finished) return true;

    // Fetch full output log so the complete attempt log is restored locally.
    nlohmann::json logReq;
    logReq["action"] = "output";
    logReq["id"] = remoteId;
    logReq["tail"] = 0;
    std::string logResponse;
    std::string logError;
    if (remote::CallHost(host, logReq.dump(), logResponse, logError)) {
        nlohmann::json logDoc = nlohmann::json::parse(logResponse, nullptr, false);
        if (logDoc.is_object() && logDoc.value("ok", false) && logDoc.contains("output")) {
            out.output = logDoc.value("output", std::string());
        }
    }

    // Mirror the normalized event stream so the local GUI can show the same
    // worker events without knowing that the job ran elsewhere.
    nlohmann::json eventsReq;
    eventsReq["action"] = "get_agent_stream";
    eventsReq["id"] = remoteId;
    eventsReq["tail"] = 0;
    eventsReq["schema"] = "hancho.event/1";
    std::string eventsResponse;
    std::string eventsError;
    if (remote::CallHost(host, eventsReq.dump(), eventsResponse, eventsError)) {
        nlohmann::json ev = nlohmann::json::parse(eventsResponse, nullptr, false);
        if (ev.is_object() && ev.contains("events") && ev["events"].is_array()) {
            std::ostringstream lines;
            for (const auto& item : ev["events"]) {
                lines << item.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) << "\n";
            }
            out.eventsJsonl = lines.str();
        }
    }

    nlohmann::json doneReq;
    doneReq["action"] = "get_done";
    doneReq["id"] = remoteId;
    std::string doneResponse;
    std::string doneError;
    if (remote::CallHost(host, doneReq.dump(), doneResponse, doneError)) {
        nlohmann::json done = nlohmann::json::parse(doneResponse, nullptr, false);
        if (done.is_object() && done.value("ok", false)) {
            out.doneContent = done.value("content", std::string());
        }
    }
    return true;
}

bool RemoteKill(const std::string& host, int remoteId, std::string& outError) {
    nlohmann::json j;
    j["action"] = "kill";
    j["id"] = remoteId;
    std::string response;
    if (!remote::CallHost(host, j.dump(), response, outError)) return false;
    nlohmann::json r = nlohmann::json::parse(response, nullptr, false);
    if (r.is_discarded() || !r.is_object() || !r.value("ok", false)) {
        outError = r.is_object() ? r.value("error", std::string("remote kill failed"))
                                 : std::string("remote kill returned invalid JSON");
        return false;
    }
    return true;
}

bool ParsePortValue(const std::string& text, uint16_t& outPort) {
    if (text.empty()) return false;
    for (char c : text) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    int value = 0;
    try {
        value = std::stoi(text);
    } catch (...) {
        return false;
    }
    if (value <= 0 || value > 65535) return false;
    outPort = static_cast<uint16_t>(value);
    return true;
}

std::string HandleCheckFilesNewer(ProcessManager& pm, const std::string& requestJson) {
    nlohmann::json req = nlohmann::json::parse(requestJson, nullptr, false);
    if (!req.is_object()) {
        return "{\"ok\":false,\"error\":\"invalid JSON request\"}";
    }
    int id = req.value("id", 0);
    std::string cwd = req.value("cwd", std::string());
    uint64_t reqStartFiletime = req.value("start_filetime", 0ULL);

    std::vector<std::string> files;
    if (req.contains("files") && req["files"].is_array()) {
        for (const auto& item : req["files"]) {
            if (item.is_string()) files.push_back(item.get<std::string>());
        }
    }
    if (files.empty()) {
        return "{\"ok\":true,\"success\":true,\"detail\":\"\"}";
    }

    std::string workingDir = cwd;
    if (workingDir.empty() && id > 0) {
        ProcessSnapshot snap;
        if (pm.GetSnapshot(id, snap)) {
            workingDir = snap.workingDir;
        }
    }
    if (workingDir.empty()) {
        workingDir = WideToUtf8(GetCurrentDirectoryWide());
    }

    uint64_t refFiletime = reqStartFiletime;
    if (id > 0) {
        std::lock_guard<std::mutex> lock(g_remoteStartFiletimesMutex);
        auto it = g_remoteStartFiletimes.find(id);
        if (it != g_remoteStartFiletimes.end()) {
            refFiletime = it->second;
        }
    }
    if (refFiletime == 0 && id > 0) {
        ProcessSnapshot snap;
        if (pm.GetSnapshot(id, snap)) {
            if (snap.startEpochSec > 0) {
                refFiletime = (static_cast<uint64_t>(snap.startEpochSec) + kWindowsToUnixEpochSec) * 10000000ULL;
            } else if (!snap.startTimeStr.empty()) {
                int64_t ep = ParseTimestampStrToEpoch(snap.startTimeStr);
                if (ep > 0) {
                    refFiletime = (static_cast<uint64_t>(ep) + kWindowsToUnixEpochSec) * 10000000ULL;
                }
            }
        }
    }

    for (const auto& rel : files) {
        std::wstring relW = Utf8ToWide(rel);
        std::filesystem::path p(relW);
        std::wstring pathW;
        if (p.is_absolute()) {
            pathW = relW;
        } else {
            pathW = PathJoinWide(Utf8ToWide(workingDir), relW);
        }

        if (!PathExistsWide(pathW)) {
            nlohmann::json resp;
            resp["ok"] = true;
            resp["success"] = false;
            resp["detail"] = "missing file: " + rel;
            return resp.dump();
        }
        uint64_t fileTime = 0;
        if (!GetFileMtimeWide(pathW, fileTime)) {
            nlohmann::json resp;
            resp["ok"] = true;
            resp["success"] = false;
            resp["detail"] = "cannot read mtime: " + rel;
            return resp.dump();
        }
        if (refFiletime > 0 && fileTime <= refFiletime) {
            nlohmann::json resp;
            resp["ok"] = true;
            resp["success"] = false;
            resp["detail"] = "file not newer than attempt start: " + rel;
            return resp.dump();
        }
    }

    nlohmann::json resp;
    resp["ok"] = true;
    resp["success"] = true;
    resp["detail"] = "";
    return resp.dump();
}

bool RemoteCheckFilesNewer(const std::string& host, const std::string& cwd, int remoteProcessId,
                           const std::vector<std::string>& files, uint64_t startFiletime,
                           std::string& outDetail) {
    nlohmann::json j;
    j["action"] = "check_files_newer";
    j["id"] = remoteProcessId;
    if (!cwd.empty()) j["cwd"] = cwd;
    j["files"] = files;
    if (startFiletime > 0) j["start_filetime"] = startFiletime;

    std::string response;
    std::string callError;
    if (!remote::CallHost(host, j.dump(), response, callError)) {
        outDetail = "remote check_files_newer failed: " + callError;
        return false;
    }
    nlohmann::json r = nlohmann::json::parse(response, nullptr, false);
    if (r.is_discarded() || !r.is_object() || !r.value("ok", false)) {
        outDetail = r.is_object() ? r.value("error", std::string("remote check_files_newer failed"))
                                  : std::string("remote check_files_newer returned invalid JSON");
        return false;
    }
    bool success = r.value("success", false);
    if (!success) {
        outDetail = r.value("detail", std::string("success condition not met"));
        return false;
    }
    return true;
}

std::string HandleRemoteSubscribe(ProcessManager& pm, const std::string& requestJson) {
    nlohmann::json req = nlohmann::json::parse(requestJson, nullptr, false);
    if (!req.is_object()) {
        return "{\"ok\":false,\"error\":\"invalid JSON request\"}";
    }

    std::string streamType = req.value("stream", "events");
    if (streamType == "events.jsonl") streamType = "events";
    else if (streamType == "stream.jsonl") streamType = "stream";
    else if (streamType == "tail.txt") streamType = "tail";

    std::string filePath;
    if (req.contains("file") && req["file"].is_string() && !req["file"].get<std::string>().empty()) {
        filePath = ResolveAbsolutePathUtf8(req["file"].get<std::string>());
    } else if (req.contains("path") && req["path"].is_string() && !req["path"].get<std::string>().empty()) {
        filePath = ResolveAbsolutePathUtf8(req["path"].get<std::string>());
    }

    int id = 0;
    if (req.contains("id") && req["id"].is_number()) {
        id = req["id"].get<int>();
    }

    std::string jobId = req.value("job_id", "");
    if (jobId.empty() && req.contains("id") && req["id"].is_string()) {
        jobId = req["id"].get<std::string>();
    }

    int attempt = req.value("attempt", 0);

    if (filePath.empty()) {
        std::wstring logsDir = GetLogsDirectoryWide();
        std::wstring tqDir = PathJoinWide(logsDir, L"taskqueue");

        if (id > 0) {
            ProcessSnapshot snap;
            bool hasSnap = pm.GetSnapshot(id, snap);
            std::string safeName = hasSnap ? SanitizeFileNameComponent(snap.name) : "";

            if (streamType == "events") {
                if (hasSnap && !snap.streamFilePath.empty()) {
                    filePath = HanchoEventsFilePathForStream(snap.streamFilePath);
                }
                if (filePath.empty() || !PathExistsWide(Utf8ToWide(filePath))) {
                    auto matches = ListDirectoryWide(logsDir, L"proc_" + std::to_wstring(id) + L"_*.events.jsonl");
                    if (!matches.empty()) {
                        filePath = WideToUtf8(PathJoinWide(logsDir, matches[0]));
                    }
                }
                if (filePath.empty() && hasSnap) {
                    filePath = WideToUtf8(PathJoinWide(logsDir, L"proc_" + std::to_wstring(id) + L"_" + Utf8ToWide(safeName) + L".events.jsonl"));
                }
            } else if (streamType == "stream") {
                if (hasSnap && !snap.streamFilePath.empty()) {
                    filePath = snap.streamFilePath;
                }
                if (filePath.empty() || !PathExistsWide(Utf8ToWide(filePath))) {
                    auto matches = ListDirectoryWide(logsDir, L"proc_" + std::to_wstring(id) + L"_*_stream.jsonl");
                    if (!matches.empty()) {
                        filePath = WideToUtf8(PathJoinWide(logsDir, matches[0]));
                    } else {
                        auto matches2 = ListDirectoryWide(logsDir, L"proc_" + std::to_wstring(id) + L"_*.stream.jsonl");
                        if (!matches2.empty()) {
                            filePath = WideToUtf8(PathJoinWide(logsDir, matches2[0]));
                        }
                    }
                }
                if (filePath.empty() && hasSnap) {
                    filePath = WideToUtf8(PathJoinWide(logsDir, L"proc_" + std::to_wstring(id) + L"_" + Utf8ToWide(safeName) + L"_stream.jsonl"));
                }
            } else if (streamType == "tail") {
                auto matches = ListDirectoryWide(logsDir, L"proc_" + std::to_wstring(id) + L"_*.tail.txt");
                if (!matches.empty()) {
                    filePath = WideToUtf8(PathJoinWide(logsDir, matches[0]));
                } else if (hasSnap && !snap.logFilePath.empty()) {
                    filePath = snap.logFilePath;
                }
            }
        } else if (!jobId.empty()) {
            std::string safeJob = SanitizeFileNameComponent(jobId);
            std::wstring attStr = (attempt > 0) ? (L"." + std::to_wstring(attempt)) : L".*";
            if (streamType == "events") {
                auto matches = ListDirectoryWide(tqDir, Utf8ToWide(safeJob) + attStr + L".events.jsonl");
                if (!matches.empty()) {
                    filePath = WideToUtf8(PathJoinWide(tqDir, matches.back()));
                } else {
                    filePath = WideToUtf8(PathJoinWide(tqDir, Utf8ToWide(safeJob) + L"." + std::to_wstring(attempt > 0 ? attempt : 1) + L".events.jsonl"));
                }
            } else if (streamType == "tail") {
                auto matches = ListDirectoryWide(tqDir, Utf8ToWide(safeJob) + attStr + L".tail.txt");
                if (!matches.empty()) {
                    filePath = WideToUtf8(PathJoinWide(tqDir, matches.back()));
                } else {
                    filePath = WideToUtf8(PathJoinWide(tqDir, Utf8ToWide(safeJob) + L"." + std::to_wstring(attempt > 0 ? attempt : 1) + L".tail.txt"));
                }
            } else if (streamType == "stream") {
                auto matches = ListDirectoryWide(tqDir, Utf8ToWide(safeJob) + attStr + L".stream.jsonl");
                if (!matches.empty()) {
                    filePath = WideToUtf8(PathJoinWide(tqDir, matches.back()));
                } else {
                    filePath = WideToUtf8(PathJoinWide(tqDir, Utf8ToWide(safeJob) + L"." + std::to_wstring(attempt > 0 ? attempt : 1) + L".stream.jsonl"));
                }
            }
        }
    }

    if (filePath.empty()) {
        return "{\"ok\":false,\"error\":\"no matching target file for subscribe\"}";
    }

    nlohmann::json resp;
    resp["ok"] = true;
    resp["action"] = "subscribe";
    if (id > 0) resp["id"] = id;
    if (!jobId.empty()) resp["job_id"] = jobId;
    resp["stream"] = streamType;
    resp["file"] = filePath;
    resp["seq"] = req.value("seq", uint64_t{0});
    return resp.dump();
}

} // namespace

void IpcServer::InstallRemoteCallbacks() {
    if (remoteCallbacksInstalled_) return;
    remoteCallbacksInstalled_ = true;
    tqm_.SetRemoteCallbacks(
        [](const std::string& host, const StartProcessRequest& req, std::string& err) {
            return RemoteStart(host, req, err);
        },
        [](const std::string& host, int remoteId, RemotePollResult& result, std::string& err) {
            return RemotePoll(host, remoteId, result, err);
        },
        [](const std::string& host, int remoteId, std::string& err) {
            return RemoteKill(host, remoteId, err);
        });
    SetRemoteFilesCheckCallback(
        [](const std::string& host, const std::string& cwd, int remoteProcessId,
           const std::vector<std::string>& files, uint64_t startFiletime,
           std::string& outDetail) -> bool {
            return RemoteCheckFilesNewer(host, cwd, remoteProcessId, files, startFiletime, outDetail);
        });
}

void IpcServer::StartRemoteListener() {
    InstallRemoteCallbacks();

    const char* listen = std::getenv("HANCHO_REMOTE_LISTEN");
    if (!listen || !*listen) return;

    std::string bindHost = "127.0.0.1";
    uint16_t port = 9137;
    std::string value = listen;
    if (ParsePortValue(value, port)) {
        // HANCHO_REMOTE_LISTEN=9137 means loopback on that port.
    } else {
        std::string parseError;
        if (!ParseRemoteAddress(value, bindHost, port, parseError)) {
            WriteAuditLog("IPC_REMOTE_START", std::string("invalid HANCHO_REMOTE_LISTEN: ") + parseError);
            return;
        }
    }

    const std::string token = ResolveServerToken();
    auto listener = std::make_unique<remote::RemoteListener>();
    listener->SetHandler([this](const std::string& requestJson, const std::string& peer) {
        const std::string action = JsonGetString(requestJson, "action", "");
        WriteAuditLog("IPC_REMOTE_REQ", peer + " action=" + action);
        if (action == "check_files_newer") {
            return HandleCheckFilesNewer(pm_, requestJson);
        }
        if (action == "subscribe") {
            return HandleRemoteSubscribe(pm_, requestJson);
        }
        if (action.rfind("power_", 0) == 0) {
            return std::string("{\"ok\":false,\"error\":\"power actions are only available ")
                   + "on the local host\"}";
        }
        std::string resp = HandleRequestJson(requestJson);
        if (action == "start") {
            nlohmann::json r = nlohmann::json::parse(resp, nullptr, false);
            if (r.is_object() && r.value("ok", false)) {
                int id = r.value("id", 0);
                if (id > 0) {
                    std::lock_guard<std::mutex> lock(g_remoteStartFiletimesMutex);
                    if (g_remoteStartFiletimes.size() > 2000) g_remoteStartFiletimes.clear();
                    g_remoteStartFiletimes[id] = GetSystemTimeAsFiletime100ns();
                }
            }
        }
        return resp;
    });

    std::string err;
    if (!listener->Start(bindHost, port, token, err)) {
        WriteAuditLog("IPC_REMOTE_START", err.empty() ? "cannot start remote listener" : err);
        return;
    }
    WriteAuditLog("IPC_REMOTE_LISTEN",
                  listener->Endpoint() + (token.empty() ? " (loopback only)" : " (token required)"));
    remoteListener_ = std::move(listener);
}

void IpcServer::StopRemoteListener() {
    if (remoteListener_) {
        remoteListener_->Stop();
        remoteListener_.reset();
    }
}

void IpcServer::StartWebUiServer(const std::string& token) {
    auto webui = std::make_unique<WebUiServer>([this](const std::string& reqJson) {
        return HandleRequestJson(reqJson);
    });
    std::string err;
    if (!webui->Start(0, token, "", &err)) {
        WriteAuditLog("WEBUI_START_FAILED", err.empty() ? "cannot start WebUI" : err);
        return;
    }
    webUiServer_ = std::move(webui);
}

void IpcServer::StopWebUiServer() {
    if (webUiServer_) {
        webUiServer_->Stop();
        webUiServer_.reset();
    }
}

void IpcServer::ScheduleWebUiRestart() {
    if (webuiRestartThread_.joinable()) {
        webuiRestartThread_.join();
    }
    webuiRestartThread_ = std::thread([this]() {
        // Let the config_set reply reach the caller before closing its connection.
        SleepMs(250);
        if (stop_.load()) return;
        // Keep the session token so an open browser tab keeps working after the
        // listener moves to a new port or scope.
        std::string token = webUiServer_ ? webUiServer_->GetToken() : std::string();
        StopWebUiServer();
        if (stop_.load()) return;
        StartWebUiServer(token);
    });
}

void IpcServer::StartIpmsg() {
    if (!ipmsg_) ipmsg_ = std::make_unique<IpmsgNode>();
    nlohmann::json block = nlohmann::json::object();
    std::string content =
        ReadFileUtf8Wide(PathJoinWide(GetEffectiveDataDirectoryWide(), L"config.json"));
    if (!content.empty()) {
        nlohmann::json cfg = nlohmann::json::parse(content, nullptr, false);
        if (cfg.is_object() && cfg.contains("ipmsg") && cfg["ipmsg"].is_object()) {
            block = cfg["ipmsg"];
        }
    }
    IpmsgConfig cfg = IpmsgConfig::FromJson(block);
    // The handler keeps its state across node restarts and re-reads the config here.
    if (ipmsgHandler_) {
        nlohmann::json handlerBlock =
            block.is_object() && block.contains("handler") && block["handler"].is_object()
                ? block["handler"]
                : nlohmann::json::object();
        IpmsgHandlerConfig handlerCfg = IpmsgHandlerConfig::FromJson(handlerBlock);
        ipmsgHandler_->Configure(handlerCfg, WideToUtf8(GetEffectiveDataDirectoryWide()),
                                 btm_ ? &btm_->Engines() : nullptr);
        ipmsg_->SetOnMessage([this](const nlohmann::json& message) {
            if (ipmsgHandler_) ipmsgHandler_->OnMessage(message);
        });
    }
    // Start() stops an existing node itself, so a disabled config also frees the port.
    if (!cfg.enabled) {
        StopIpmsg();
        return;
    }
    std::string err;
    if (!ipmsg_->Start(cfg, &err)) {
        WriteAuditLog("IPMSG_START_FAILED", err.empty() ? "cannot start IPMsg" : err);
    }
}

void IpcServer::StopIpmsg() {
    if (ipmsg_) ipmsg_->Stop();
}

void IpcServer::ScheduleIpmsgRestart() {
    if (ipmsgRestartThread_.joinable()) {
        ipmsgRestartThread_.join();
    }
    ipmsgRestartThread_ = std::thread([this]() {
        // Let the config_set reply reach the caller before closing its connection.
        SleepMs(250);
        if (stop_.load()) return;
        StopIpmsg();
        if (stop_.load()) return;
        StartIpmsg();
    });
}

} // namespace pc
