#include "ipc_server.h"

#include "common.h"
#include "remote_hosts.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <sstream>

namespace pc {

namespace {

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

    // Mirror the normalized event stream so the local GUI can show the same
    // worker events without knowing that the job ran elsewhere.
    nlohmann::json eventsReq;
    eventsReq["action"] = "get_agent_stream";
    eventsReq["id"] = remoteId;
    eventsReq["tail"] = 500;
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
        return HandleRequestJson(requestJson);
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

} // namespace pc
