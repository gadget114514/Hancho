#include "ipc_server.h"
#include "bt_manager.h"
#include "chain_guard.h"
#include "common.h"
#include "event_schema.h"
#include "ipmsg.h"
#include "ipmsg_handler.h"
#include "llm_stream.h"
#include "memory_record.h"
#include "memory_search.h"
#include "remote_hosts.h"
#include "triage_manager.h"
#include "webui_server.h"
#include "term_server.h"
#include "nlohmann/json.hpp"
#include <ctime>
#include <fstream>
#include <sstream>
#include <cctype>

namespace pc {

constexpr UINT WM_PC_IPC_EVENT = WM_APP + 101;

std::string SnapshotToJson(const ProcessSnapshot& s) {
    std::ostringstream oss;
    oss << "{"
        << "\"id\":" << s.id << ","
        << "\"name\":\"" << JsonEscape(s.name) << "\","
        << "\"command\":\"" << JsonEscape(s.commandLine) << "\","
        << "\"cwd\":\"" << JsonEscape(s.workingDir) << "\","
        << "\"mode\":\"" << ExecModeToString(s.mode) << "\","
        << "\"state\":\"" << ProcStateToString(s.state) << "\","
        << "\"pid\":" << s.rootPid << ","
        << "\"exit_code\":" << s.exitCode << ","
        << "\"start_time\":\"" << JsonEscape(s.startTimeStr) << "\","
        << "\"end_time\":\"" << JsonEscape(s.endTimeStr) << "\","
        << "\"estimated_end_time\":\"" << JsonEscape(s.estimatedEndTimeStr) << "\","
        << "\"elapsed_ms\":" << s.elapsedMs << ","
        << "\"elapsed_str\":\"" << JsonEscape(FormatDurationSec(s.elapsedMs)) << "\","
        << "\"cpu_time_ms\":" << s.cpuTimeMs << ","
        << "\"cpu_time_str\":\"" << JsonEscape(FormatUptimeHMS(s.cpuTimeMs)) << "\","
        << "\"uptime_str\":\"" << JsonEscape(FormatUptimeHMS(s.elapsedMs)) << "\","
        << "\"idle_ms\":" << s.idleMs << ","
        << "\"timeout_sec\":" << s.timeoutSec << ","
        << "\"idle_timeout_sec\":" << s.idleTimeoutSec << ","
        << "\"tree_procs\":" << s.treeProcessCount << ","
        << "\"memory_bytes\":" << s.treeMemoryBytes << ","
        << "\"memory_str\":\"" << JsonEscape(FormatBytes(s.treeMemoryBytes)) << "\","
        << "\"total_bytes_read\":" << s.totalBytesRead << ","
        << "\"log_file\":\"" << JsonEscape(s.logFilePath) << "\","
        << "\"done_file\":\"" << JsonEscape(s.doneFilePath) << "\","
        << "\"last_line\":\"" << JsonEscape(s.lastLinePreview) << "\","
        << "\"job_key\":\"" << JsonEscape(s.jobKey) << "\","
        << "\"job_id\":" << s.jobId << ","
        << "\"attempt\":" << s.attemptN << ","
        << "\"agent_type\":\"" << JsonEscape(s.agentType) << "\","
        << "\"agent_status\":\"" << JsonEscape(s.agentStatus) << "\","
        << "\"current_tool\":\"" << JsonEscape(s.currentTool) << "\","
        << "\"current_tool_action\":\"" << JsonEscape(s.currentToolAction) << "\","
        << "\"last_thought\":\"" << JsonEscape(s.lastThought) << "\","
        << "\"last_message\":\"" << JsonEscape(s.lastMessage) << "\","
        << "\"step_count\":" << s.stepCount << ","
        << "\"stream_event_count\":" << s.streamEventCount << ","
        << "\"stream_file\":\"" << JsonEscape(s.streamFilePath) << "\","
        << "\"supervisor_notes\":\"" << JsonEscape(s.supervisorNotes) << "\","
        << "\"supervisor_goal\":\"" << JsonEscape(s.supervisorGoal) << "\""
        << "}";
    return oss.str();
}


static std::string ExtractRawJsonValue(const std::string& json, const std::string& key) {
    std::string pattern = "\"" + key + "\"";
    size_t keyPos = json.find(pattern);
    if (keyPos == std::string::npos) return "";
    size_t colon = json.find(':', keyPos + pattern.size());
    if (colon == std::string::npos) return "";
    size_t p = colon + 1;
    while (p < json.size() && std::isspace(static_cast<unsigned char>(json[p]))) p++;
    if (p >= json.size()) return "";
    char open = json[p];
    if (open == '[' || open == '{') {
        char close = (open == '[') ? ']' : '}';
        int depth = 0;
        bool inStr = false;
        bool escaped = false;
        for (size_t i = p; i < json.size(); ++i) {
            char c = json[i];
            if (inStr) {
                if (escaped) escaped = false;
                else if (c == '\\') escaped = true;
                else if (c == '"') inStr = false;
                continue;
            }
            if (c == '"') { inStr = true; continue; }
            if (c == open) depth++;
            else if (c == close) { depth--; if (depth == 0) return json.substr(p, i - p + 1); }
        }
        return "";
    }
    size_t end = p;
    if (open == '"') {
        end = p + 1;
        while (end < json.size()) {
            if (json[end] == '"' && json[end - 1] != '\\') { end++; break; }
            end++;
        }
        return json.substr(p, end - p);
    }
    while (end < json.size() && json[end] != ',' && json[end] != '}' && json[end] != ']' &&
           !std::isspace(static_cast<unsigned char>(json[end]))) end++;
    return json.substr(p, end - p);
}

IpcServer::IpcServer(ProcessManager& pm, TaskQueueManager& tqm, HWND notifyHwnd)
    : pm_(pm), tqm_(tqm), btm_(nullptr), notifyHwnd_(notifyHwnd) {
    triageManager_ = std::make_unique<TriageManager>(pm_, tqm_, btm_);
    ipmsg_ = std::make_unique<IpmsgNode>();
    ipmsgHandler_ = std::make_unique<IpmsgHandler>(tqm_, pm_, ipmsg_.get());
    InstallRemoteCallbacks();
}

IpcServer::IpcServer(ProcessManager& pm, TaskQueueManager& tqm, BtManager* btm, HWND notifyHwnd)
    : pm_(pm), tqm_(tqm), btm_(btm), notifyHwnd_(notifyHwnd) {
    triageManager_ = std::make_unique<TriageManager>(pm_, tqm_, btm_);
    ipmsg_ = std::make_unique<IpmsgNode>();
    ipmsgHandler_ = std::make_unique<IpmsgHandler>(tqm_, pm_, ipmsg_.get());
    InstallRemoteCallbacks();
}

IpcServer::~IpcServer() {
    Stop();
}

#ifdef _WIN32
bool IpcServer::Start() {
    stop_ = false;
    if (triageManager_) triageManager_->Start();
    acceptThread_ = std::thread(&IpcServer::AcceptLoop, this);
    StartRemoteListener();
    StartWebUiServer();
    StartTermServer();
    StartIpmsg();
    return true;
}

void IpcServer::Stop() {
    if (stop_.exchange(true)) return;
    if (triageManager_) triageManager_->Stop();
    if (webuiRestartThread_.joinable()) webuiRestartThread_.join();
    if (termRestartThread_.joinable()) termRestartThread_.join();
    {
        std::lock_guard<std::mutex> lk(ipmsgRestartMutex_);
        if (ipmsgRestartThread_.joinable()) ipmsgRestartThread_.join();
    }
    StopWebUiServer();
    StopTermServer();
    StopIpmsg();
    if (ipmsgHandler_) ipmsgHandler_->Stop();
    StopRemoteListener();
    // Wake up blocking ConnectNamedPipe by opening a dummy connection
    HANDLE hDummy = CreateFileW(GetPipeNameWide().c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hDummy != INVALID_HANDLE_VALUE) {
        CloseHandle(hDummy);
    }
    if (acceptThread_.joinable()) {
        acceptThread_.join();
    }
}

void IpcServer::AcceptLoop() {
    while (!stop_) {
        HANDLE hPipe = CreateNamedPipeW(
            GetPipeNameWide().c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES,
            256 * 1024,
            64 * 1024,
            0,
            nullptr
        );
        if (hPipe == INVALID_HANDLE_VALUE) {
            SleepMs(100);
            continue;
        }

        BOOL connected = ConnectNamedPipe(hPipe, nullptr) ? TRUE : (GetLastError() == ERROR_PIPE_CONNECTED);
        if (stop_) {
            CloseHandle(hPipe);
            break;
        }
        if (connected) {
            std::thread([this, hPipe] {
                try {
                    HandleClient(hPipe);
                } catch (const std::exception& e) {
                    WriteAuditLog("CRASH_GUARD", std::string("IPC client exception: ") + e.what());
                    DisconnectNamedPipe(hPipe);
                    CloseHandle(hPipe);
                } catch (...) {
                    WriteAuditLog("CRASH_GUARD", "IPC client exception: unknown");
                    DisconnectNamedPipe(hPipe);
                    CloseHandle(hPipe);
                }
            }).detach();
        } else {
            CloseHandle(hPipe);
        }
    }
}

void IpcServer::HandleClient(HANDLE hPipe) {
    std::string req;
    char buf[16384];
    DWORD bytesRead = 0;

    while (true) {
        BOOL ok = ReadFile(hPipe, buf, sizeof(buf), &bytesRead, nullptr);
        if (bytesRead > 0) {
            req.append(buf, bytesRead);
        }
        if (ok) {
            break; // Message complete in PIPE_READMODE_MESSAGE
        }
        if (GetLastError() != ERROR_MORE_DATA) {
            break;
        }
    }

    if (!req.empty()) {
        std::string resp = HandleRequestJson(req);
        DWORD written = 0;
        WriteFile(hPipe, resp.data(), static_cast<DWORD>(resp.size()), &written, nullptr);
        FlushFileBuffers(hPipe);
    }

    DisconnectNamedPipe(hPipe);
    CloseHandle(hPipe);
}

#endif

// The worker's final report for run_worker / wait_worker: only this reaches the supervisor's context,
// the raw stream stays in stream_file. Size-capped so one run cannot flood that context.
static std::string AgentResultToJson(ProcessManager& pm, int id, bool finished) {
    AgentExtractedResult ex = ExtractAgentResult(pm, id, finished);
    if (!ex.found) {
        return "{\"ok\":false,\"error\":\"Process ID not found.\"}";
    }

    std::ostringstream oss;
    oss << "{\"ok\":true,\"id\":" << id
        << ",\"finished\":" << (finished ? "true" : "false")
        << ",\"success\":" << ((finished && ex.snap.state == ProcState::Completed && !ex.isError) ? "true" : "false")
        << ",\"state\":\"" << ProcStateToString(ex.snap.state) << "\""
        << ",\"exit_code\":" << ex.snap.exitCode
        << ",\"elapsed_ms\":" << ex.snap.elapsedMs
        << ",\"agent_type\":\"" << JsonEscape(ex.agentState.agentType) << "\""
        << ",\"model\":\"" << JsonEscape(ex.agentState.model) << "\""
        << ",\"result\":\"" << JsonEscape(ex.result) << "\""
        << ",\"result_source\":\"" << ex.source << "\""
        << ",\"result_truncated\":" << (ex.truncated ? "true" : "false")
        << ",\"tool_calls\":" << ex.agentState.toolCalls
        << ",\"tool_errors\":" << ex.agentState.toolErrors
        << ",\"num_turns\":" << ex.agentState.numTurns
        << ",\"permission_denials\":" << ex.agentState.permissionDenials
        << ",\"total_tokens\":" << ex.agentState.totalTokens
        << ",\"cost_usd\":" << ex.agentState.costUsd;
    if (!finished) {
        oss << ",\"activity\":\"" << JsonEscape(ex.agentState.activity) << "\""
            << ",\"activity_target\":\"" << JsonEscape(ex.agentState.activityTarget) << "\"";
    }
    oss << ",\"log_file\":\"" << JsonEscape(ex.snap.logFilePath) << "\""
        << ",\"stream_file\":\"" << JsonEscape(ex.agentState.streamFilePath) << "\"}";
    return oss.str();
}

static std::string RedactIpcToken(const std::string& json) {
    nlohmann::json doc = nlohmann::json::parse(json, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) return json;
    auto it = doc.find("token");
    if (it == doc.end()) return json;
    doc["token"] = "<redacted>";
    return doc.dump();
}

static std::string IpmsgReqString(const nlohmann::json& req, const char* key,
                                  const std::string& def = std::string()) {
    auto it = req.find(key);
    if (it != req.end() && it->is_string()) return it->get<std::string>();
    return def;
}

static bool IpmsgReqBool(const nlohmann::json& req, const char* key, bool def) {
    auto it = req.find(key);
    if (it != req.end() && it->is_boolean()) return it->get<bool>();
    return def;
}

static int64_t IpmsgReqInt(const nlohmann::json& req, const char* key, int64_t def) {
    auto it = req.find(key);
    if (it == req.end()) return def;
    if (it->is_number_unsigned()) return static_cast<int64_t>(it->get<uint64_t>());
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_number_float()) return static_cast<int64_t>(it->get<double>());
    if (it->is_string()) {
        try {
            return std::stoll(it->get<std::string>());
        } catch (...) {
            return def;
        }
    }
    return def;
}

static uint64_t IpmsgReqUint(const nlohmann::json& req, const char* key, uint64_t def) {
    auto it = req.find(key);
    if (it == req.end()) return def;
    if (it->is_number_unsigned()) return it->get<uint64_t>();
    if (it->is_number_integer()) {
        int64_t v = it->get<int64_t>();
        return v < 0 ? def : static_cast<uint64_t>(v);
    }
    if (it->is_number_float()) {
        double d = it->get<double>();
        return d < 0 ? def : static_cast<uint64_t>(d);
    }
    if (it->is_string()) {
        try {
            return std::stoull(it->get<std::string>());
        } catch (...) {
            return def;
        }
    }
    return def;
}

static nlohmann::json& EnsureChatBlock(nlohmann::json& cfg) {
    if (!cfg.contains("chat") || !cfg["chat"].is_object()) {
        if (cfg.contains("ipmsg") && cfg["ipmsg"].is_object()) {
            // Migrate the pre-Chat key transparently.
            cfg["chat"] = cfg["ipmsg"];
        } else {
            cfg["chat"] = nlohmann::json::object();
        }
    }
    cfg.erase("ipmsg");
    return cfg["chat"];
}

static void PatchChatHandlerConfig(nlohmann::json& handler, const nlohmann::json& req) {
    auto str = [&](const char* snake, const char* camel) {
        auto it = req.find(snake);
        if (it == req.end() && camel) it = req.find(camel);
        if (it != req.end() && it->is_string()) handler[snake] = it->get<std::string>();
    };
    auto boolean = [&](const char* snake, const char* camel) {
        auto it = req.find(snake);
        if (it == req.end() && camel) it = req.find(camel);
        if (it != req.end() && it->is_boolean()) handler[snake] = it->get<bool>();
    };
    auto integer = [&](const char* snake, const char* camel) {
        auto it = req.find(snake);
        if (it == req.end() && camel) it = req.find(camel);
        if (it == req.end()) return;
        if (it->is_number_integer()) handler[snake] = it->get<int64_t>();
        else if (it->is_number_unsigned()) handler[snake] = static_cast<int64_t>(it->get<uint64_t>());
        else if (it->is_string()) {
            try { handler[snake] = std::stoll(it->get<std::string>()); } catch (...) {}
        }
    };
    boolean("enabled", nullptr);
    str("engine", nullptr);
    str("model", nullptr);
    str("agent", nullptr);
    str("cwd", nullptr);
    str("instructions", nullptr);
    str("taskqueue", "taskQueue");
    boolean("auto_reply", "autoReply");
    boolean("skip_permissions", "skipPermissions");
    integer("timeout_sec", "timeoutSec");
    integer("max_hops", "maxHops");
    integer("concurrency", nullptr);
}

std::string IpcServer::HandleChatAction(const std::string& action, const std::string& reqJson) {
    nlohmann::json req = nlohmann::json::parse(reqJson, nullptr, false);
    if (req.is_discarded() || !req.is_object()) {
        return "{\"ok\":false,\"error\":\"invalid JSON request\"}";
    }
    // Old ipmsg_* calls keep working without advertising the old name.
    std::string act = action;
    if (act.rfind("ipmsg_", 0) == 0) act = "chat_" + act.substr(6);
    if (!ipmsg_) ipmsg_ = std::make_unique<IpmsgNode>();

    if (act == "chat_status") {
        nlohmann::json status = ipmsg_->Status();
        if (ipmsgHandler_) status["handler"] = ipmsgHandler_->StatusJson();
        return status.dump();
    }
    if (act == "chat_enable") {
        auto it = req.find("enabled");
        if (it == req.end() || !it->is_boolean()) {
            return "{\"ok\":false,\"error\":\"enabled (bool) is required for chat_enable\"}";
        }
        const bool enabled = it->get<bool>();
        std::wstring configPath = PathJoinWide(GetEffectiveDataDirectoryWide(), L"config.json");
        nlohmann::json cfg = nlohmann::json::object();
        std::string content = ReadFileUtf8Wide(configPath);
        if (!content.empty()) {
            nlohmann::json parsed = nlohmann::json::parse(content, nullptr, false);
            if (parsed.is_object()) cfg = parsed;
        }
        nlohmann::json& chat = EnsureChatBlock(cfg);
        chat["enabled"] = enabled;
        WriteFileAtomicWide(configPath, cfg.dump(2));
        WriteAuditLog("CHAT_ENABLE", enabled ? "enabled" : "disabled");
        // Synchronous so the reply reflects the new state (and a freed port).
        StartIpmsg();
        nlohmann::json status = ipmsg_->Status();
        if (ipmsgHandler_) status["handler"] = ipmsgHandler_->StatusJson();
        return status.dump();
    }
    if (act == "chat_handler") {
        std::wstring configPath = PathJoinWide(GetEffectiveDataDirectoryWide(), L"config.json");
        nlohmann::json cfg = nlohmann::json::object();
        std::string content = ReadFileUtf8Wide(configPath);
        if (!content.empty()) {
            nlohmann::json parsed = nlohmann::json::parse(content, nullptr, false);
            if (parsed.is_object()) cfg = parsed;
        }
        nlohmann::json& chat = EnsureChatBlock(cfg);
        if (!chat.contains("handler") || !chat["handler"].is_object()) {
            chat["handler"] = nlohmann::json::object();
        }
        nlohmann::json& handler = chat["handler"];
        PatchChatHandlerConfig(handler, req);
        // Return (and persist) the effective values, not only the patched keys.
        nlohmann::json handlerDefaults = IpmsgHandlerConfig().ToJson();
        for (auto it = handlerDefaults.begin(); it != handlerDefaults.end(); ++it) {
            if (!handler.contains(it.key())) handler[it.key()] = it.value();
        }
        WriteFileAtomicWide(configPath, cfg.dump(2));
        ScheduleIpmsgRestart();
        nlohmann::json out;
        out["ok"] = true;
        out["handler"] = handler;
        out["restarting"] = true;
        return out.dump();
    }
    if (act == "chat_peers") {
        return ipmsg_->Peers().dump();
    }
    if (act == "chat_refresh") {
        return ipmsg_->Refresh().dump();
    }
    if (act == "chat_send") {
        std::vector<std::string> files;
        if (req.contains("files") && req["files"].is_array()) {
            for (const auto& f : req["files"]) {
                if (f.is_string()) files.push_back(f.get<std::string>());
            }
        }
        return ipmsg_->Send(IpmsgReqString(req, "to"), IpmsgReqString(req, "text"),
                            IpmsgReqString(req, "from_agent"), IpmsgReqString(req, "to_agent"),
                            IpmsgReqUint(req, "reply_to", 0), files,
                            IpmsgReqString(req, "audience"),
                            static_cast<int>(IpmsgReqInt(req, "hops", 0)))
            .dump();
    }
    if (act == "chat_get") {
        return ipmsg_->Get(IpmsgReqUint(req, "id", 0)).dump();
    }
    if (act == "chat_inbox") {
        return ipmsg_->Inbox(IpmsgReqUint(req, "since_id", 0), IpmsgReqString(req, "agent"),
                             IpmsgReqBool(req, "include_unaddressed", true),
                             IpmsgReqBool(req, "unread_only", false),
                             IpmsgReqBool(req, "include_outgoing", false),
                             static_cast<size_t>(IpmsgReqUint(req, "limit", 50)),
                             IpmsgReqBool(req, "mark_read", false))
            .dump();
    }
    if (act == "chat_wait") {
        int timeoutMs = static_cast<int>(IpmsgReqInt(req, "timeout_ms", 60000));
        if (timeoutMs < 0) timeoutMs = 0;
        if (timeoutMs > 600000) timeoutMs = 600000;
        return ipmsg_->Wait(IpmsgReqUint(req, "since_id", 0), IpmsgReqString(req, "agent"),
                            IpmsgReqBool(req, "include_unaddressed", true), timeoutMs,
                            IpmsgReqBool(req, "mark_read", true))
            .dump();
    }
    if (act == "chat_read") {
        std::vector<uint64_t> ids;
        if (req.contains("ids") && req["ids"].is_array()) {
            for (const auto& id : req["ids"]) {
                if (id.is_number_unsigned()) ids.push_back(id.get<uint64_t>());
                else if (id.is_number_integer()) {
                    int64_t v = id.get<int64_t>();
                    if (v >= 0) ids.push_back(static_cast<uint64_t>(v));
                } else if (id.is_string()) {
                    try {
                        ids.push_back(std::stoull(id.get<std::string>()));
                    } catch (...) {
                    }
                }
            }
        }
        return ipmsg_->MarkRead(ids, IpmsgReqBool(req, "all", false)).dump();
    }
    if (act == "chat_download") {
        int timeoutMs = static_cast<int>(IpmsgReqInt(req, "timeout_ms", 300000));
        return ipmsg_->Download(IpmsgReqUint(req, "message_id", 0),
                                IpmsgReqInt(req, "file_id", -1), IpmsgReqString(req, "dir"),
                                IpmsgReqBool(req, "wait", false), timeoutMs)
            .dump();
    }
    if (act == "chat_log") {
        return ipmsg_->Log(IpmsgReqString(req, "since", "7d"), IpmsgReqString(req, "until"),
                           IpmsgReqString(req, "query"), IpmsgReqString(req, "peer"),
                           IpmsgReqString(req, "agent"), IpmsgReqString(req, "event"),
                           static_cast<size_t>(IpmsgReqUint(req, "limit", 200)))
            .dump();
    }
    if (act == "chat_clear") {
        return ipmsg_->Clear().dump();
    }
    return "{\"ok\":false,\"error\":\"Unknown chat action: " + JsonEscape(action) + "\"}";
}

std::string IpcServer::HandleRequestJson(const std::string& reqJson) {
    std::string action = JsonGetString(reqJson, "action", "");
    if (action != "ping") {
        WriteAuditLog("IPC_REQ", RedactIpcToken(reqJson));
    }
    std::ostringstream oss;

    if (action == "ping") {
        return "{\"ok\":true,\"status\":\"running\",\"headless\":"
               + std::string(headless_ ? "true" : "false") + "}";
    }
    else if (action == "term_endpoint") {
        if (!termServer_ || !termServer_->IsRunning() || termServer_->Port() == 0) {
            return "{\"ok\":false,\"error\":\"terminal server is not running\"}";
        }
        nlohmann::json out;
        out["ok"] = true;
        out["host"] = "127.0.0.1";
        out["port"] = termServer_->Port();
        out["token"] = termServer_->Token();
        return out.dump();
    }
    else if (action == "triage_list") {
        return triageManager_ ? triageManager_->List(JsonGetString(reqJson, "state", "pending"))
                              : "{\"ok\":false,\"error\":\"triage manager is unavailable\"}";
    }
    else if (action == "triage_get") {
        return triageManager_ ? triageManager_->Get(JsonGetString(reqJson, "id", ""))
                              : "{\"ok\":false,\"error\":\"triage manager is unavailable\"}";
    }
    else if (action == "triage_advise") {
        return triageManager_ ? triageManager_->RequestAdvice(JsonGetString(reqJson, "id", ""))
                              : "{\"ok\":false,\"error\":\"triage manager is unavailable\"}";
    }
    else if (action == "triage_decide") {
        if (!triageManager_) return "{\"ok\":false,\"error\":\"triage manager is unavailable\"}";
        nlohmann::json request = nlohmann::json::parse(reqJson, nullptr, false);
        if (!request.is_object()) return "{\"ok\":false,\"error\":\"invalid JSON request\"}";
        nlohmann::json params = request.value("params", nlohmann::json::object());
        if (!params.is_object()) return "{\"ok\":false,\"error\":\"params must be an object\"}";
        return triageManager_->Decide(JsonGetString(reqJson, "id", ""),
                                      JsonGetString(reqJson, "candidateId", JsonGetString(reqJson, "candidate_id", "")),
                                      JsonGetString(reqJson, "reason", ""), params,
                                      JsonGetString(reqJson, "by", "human"));
    }
    else if (action == "triage_handoff") {
        if (!triageManager_) return "{\"ok\":false,\"error\":\"triage manager is unavailable\"}";
        nlohmann::json request = nlohmann::json::parse(reqJson, nullptr, false);
        if (!request.is_object()) return "{\"ok\":false,\"error\":\"invalid JSON request\"}";
        std::vector<std::string> ids;
        auto idsIt = request.find("ids");
        if (idsIt != request.end() && idsIt->is_array()) {
            for (const auto& v : *idsIt) if (v.is_string()) ids.push_back(v.get<std::string>());
        }
        const std::string single = request.value("id", std::string());
        if (!single.empty()) ids.push_back(single);
        const bool all = request.value("all", false);
        if (!all && ids.empty()) return "{\"ok\":false,\"error\":\"triage_handoff needs ids or all\"}";
        return triageManager_->Handoff(ids, all);
    }
    else if (action == "triage_manual") {
        return triageManager_ ? triageManager_->MarkManual(JsonGetString(reqJson, "id", ""))
                              : "{\"ok\":false,\"error\":\"triage manager is unavailable\"}";
    }
    else if (action == "triage_ack") {
        return triageManager_ ? triageManager_->Acknowledge(JsonGetString(reqJson, "id", ""),
                                  JsonGetBool(reqJson, "all_auto", JsonGetBool(reqJson, "allAuto", false)))
                              : "{\"ok\":false,\"error\":\"triage manager is unavailable\"}";
    }
    else if (action == "gui_status") {
        // A GUI client can ask whether a dashboard is available without starting one.
        bool canShow = false;
        {
            std::lock_guard<std::mutex> lock(cbMutex_);
            canShow = !!guiShowCb_;
        }
        return "{\"ok\":true,\"headless\":" + std::string(headless_ ? "true" : "false")
               + ",\"canShowGui\":" + std::string(canShow ? "true" : "false") + "}";
    }
    else if (action == "gui_show") {
        // Ask the core host to expose the dashboard. In a --headless host this
        // starts the dashboard window in the host process; the calling GUI
        // process exits afterwards.
        std::function<void()> showCb;
        {
            std::lock_guard<std::mutex> lock(cbMutex_);
            showCb = guiShowCb_;
        }
        if (!showCb) {
            return "{\"ok\":false,\"error\":\"no dashboard host is available\"}";
        }
        showCb();
        return "{\"ok\":true,\"shown\":true}";
    }
    else if (action == "quit") {
        // Stop the core host. The response is sent before the host begins its
        // shutdown sequence, so the client always gets an answer.
        std::function<void()> stopCb;
        {
            std::lock_guard<std::mutex> lock(cbMutex_);
            stopCb = shutdownCb_;
        }
        if (!stopCb) {
            return "{\"ok\":false,\"error\":\"shutdown is not available\"}";
        }
        // Answer the client first, then let the host tear the pipe down.
        std::thread([stopCb] {
            SleepMs(200);
            stopCb();
        }).detach();
        return "{\"ok\":true,\"stopping\":true}";
    }
    else if (action == "power_suspend") {
        const std::string jobId = JsonGetString(reqJson, "job_id", JsonGetString(reqJson, "jobId", ""));
        const bool dryRun = JsonGetBool(reqJson, "dry_run", JsonGetBool(reqJson, "dryRun", false));
        const bool hibernate = JsonGetBool(reqJson, "hibernate", false);
        int delaySec = static_cast<int>(JsonGetInt(reqJson, "delay_sec", JsonGetInt(reqJson, "delaySec", 3)));
        if (delaySec < 0) delaySec = 0;
        if (delaySec > 86400) delaySec = 86400;

        if (!jobId.empty()) {
            if (dryRun) {
                TaskQueueJob job;
                if (!tqm_.GetJobSnapshot(jobId, job)) {
                    return "{\"ok\":false,\"error\":\"job not found: " + JsonEscape(jobId) + "\"}";
                }
                nlohmann::json out;
                out["ok"] = true;
                out["dry_run"] = true;
                out["armed"] = false;
                out["would_arm"] = true;
                out["id"] = job.id;
                out["state"] = job.state;
                out["terminal"] = (job.state == "succeeded" || job.state == "failed" ||
                                   job.state == "cancelled" || job.state == "blocked");
                out["delay_sec"] = delaySec;
                out["hibernate"] = hibernate;
                return out.dump();
            }
            std::string err;
            return tqm_.SetJobSuspendWhenDone(jobId, hibernate, delaySec, err);
        }

        if (dryRun) {
            nlohmann::json out;
            out["ok"] = true;
            out["dry_run"] = true;
            out["suspending"] = false;
            out["delay_sec"] = delaySec;
            out["hibernate"] = hibernate;
            return out.dump();
        }

        if (delaySec < 1) delaySec = 1;
        PowerCallback cb;
        {
            std::lock_guard<std::mutex> lock(cbMutex_);
            cb = powerCb_;
        }
        if (!cb) {
            return "{\"ok\":false,\"error\":\"suspend is not available\"}";
        }
        WriteAuditLog("POWER_SUSPEND", std::string("immediate delay=") + std::to_string(delaySec) +
                                           (hibernate ? " hibernate" : " sleep"));
        std::thread([cb, delaySec, hibernate] {
            if (delaySec > 0) SleepMs(static_cast<uint32_t>(delaySec) * 1000);
            std::string err;
            if (!cb(hibernate, err)) WriteAuditLog("POWER_FAILED", err);
        }).detach();
        nlohmann::json out;
        out["ok"] = true;
        out["suspending"] = true;
        out["delay_sec"] = delaySec;
        out["hibernate"] = hibernate;
        return out.dump();
    }
    else if (action == "power_cancel") {
        std::string err;
        return tqm_.ClearSuspendWhenDone(err);
    }
    else if (action == "power_status") {
        return tqm_.SuspendStatusJson();
    }
    else if (action == "start") {
        StartProcessRequest req;
        req.name = JsonGetString(reqJson, "name", "");
        req.commandLine = JsonGetString(reqJson, "cmd", "");
        req.workingDir = JsonGetString(reqJson, "cwd", "");
        req.logFile = JsonGetString(reqJson, "log_file", "");
        req.doneFile = JsonGetString(reqJson, "done_file", "");
        req.mode = StringToExecMode(JsonGetString(reqJson, "mode", "pipe"));
        req.openStdin = JsonGetBool(reqJson, "open_stdin", true);
        req.timeoutSec = static_cast<int>(JsonGetInt(reqJson, "timeout_sec", 0));
        req.idleTimeoutSec = static_cast<int>(JsonGetInt(reqJson, "idle_timeout_sec", 0));
        req.estimatedEndTimeStr = JsonGetString(reqJson, "estimate_end", "");
        req.agentType = JsonGetString(reqJson, "agent_type", "");
        req.jsonStream = JsonGetBool(reqJson, "json_stream", false);
        req.supervisorNotes = JsonGetString(reqJson, "supervisor_notes", "");
        req.supervisorGoal = JsonGetString(reqJson, "supervisor_goal", "");

        // A remote job sends its instruction text instead of the sender's local path.
        const std::string instructionsText = JsonGetString(reqJson, "instructions_text", "");
        if (!instructionsText.empty()) {
            std::string jobKey = req.jobKey.empty() ? req.name : req.jobKey;
            if (jobKey.empty()) jobKey = "remote";
            int rev = static_cast<int>(JsonGetInt(reqJson, "instructions_rev", 1));
            if (rev <= 0) rev = 1;
            std::wstring instrW = TaskQueueManager::GetInstructionsFileWide(jobKey, jobKey, rev);
            WriteFileAtomicWide(instrW, instructionsText);
            const std::string instrPathUtf8 = WideToUtf8(instrW);
            size_t instrPos = 0;
            while ((instrPos = req.commandLine.find("{instructions}", instrPos)) != std::string::npos) {
                req.commandLine.replace(instrPos, 14, instrPathUtf8);
                instrPos += instrPathUtf8.size();
            }
            req.env["PC_JOB_INSTRUCTIONS"] = instrPathUtf8;
            req.env["PC_JOB_INSTRUCTIONS_REV"] = std::to_string(rev);
        }
        bool waitFinish = JsonGetBool(reqJson, "wait", false);
        int waitMs = static_cast<int>(JsonGetInt(reqJson, "wait_ms", 0));

        if (!JsonGetBool(reqJson, "allow_chain", false)) {
            std::string chainErr;
            if (!CheckChainGuard(req.commandLine, chainErr)) {
                oss << "{\"ok\":false,\"error\":\"" << JsonEscape(chainErr) << "\"}";
                return oss.str();
            }
        }
        std::string guardJobId;
        if (tqm_.FindUnfinishedJobByCommand(req.commandLine, guardJobId)) {
            std::string msg = "This command belongs to unfinished taskqueue job '" + guardJobId +
                              "'. Use taskqueue_retry " + guardJobId + " instead of start_process.";
            oss << "{\"ok\":false,\"error\":\"" << JsonEscape(msg) << "\"}";
            return oss.str();
        }

        std::string err;
        int id = pm_.StartProcess(req, err);
        if (id < 0) {
            oss << "{\"ok\":false,\"error\":\"" << JsonEscape(err) << "\"}";
            return oss.str();
        }

        HWND hwnd = notifyHwnd_.load();
        if (hwnd) PostMessageW(hwnd, WM_PC_IPC_EVENT, static_cast<WPARAM>(id), 0);

        bool finished = false;
        if (waitFinish) {
            uint32_t maxWait = (waitMs > 0) ? static_cast<uint32_t>(waitMs) : 0;
            finished = pm_.WaitForExit(id, maxWait);
        } else if (waitMs > 0) {
            finished = pm_.WaitForExit(id, static_cast<uint32_t>(waitMs));
        }

        if (JsonGetBool(reqJson, "result_only", false)) {
            return AgentResultToJson(pm_, id, finished);
        }

        ProcessSnapshot snap;
        pm_.GetSnapshot(id, snap);
        std::string outLog;
        pm_.GetOutput(id, true, 200, outLog);

        oss << "{\"ok\":true,\"id\":" << id
            << ",\"log_file\":\"" << JsonEscape(snap.logFilePath) << "\""
            << ",\"done_file\":\"" << JsonEscape(snap.doneFilePath) << "\""
            << ",\"process\":" << SnapshotToJson(snap)
            << ",\"output\":\"" << JsonEscape(outLog) << "\"}";
        return oss.str();
    }
    else if (action == "list") {
        int filterId = static_cast<int>(JsonGetInt(reqJson, "id", 0));
        oss << "{\"ok\":true,\"processes\":[";
        if (filterId > 0) {
            ProcessSnapshot snap;
            if (pm_.GetSnapshot(filterId, snap)) {
                oss << SnapshotToJson(snap);
            }
        } else {
            auto snaps = pm_.GetAllSnapshots();
            for (size_t i = 0; i < snaps.size(); ++i) {
                if (i > 0) oss << ",";
                oss << SnapshotToJson(snaps[i]);
            }
        }
        oss << "]}";
        return oss.str();
    }
    else if (action == "get_done") {
        int id = static_cast<int>(JsonGetInt(reqJson, "id", 0));
        ProcessSnapshot snap;
        if (!pm_.GetSnapshot(id, snap)) {
            return "{\"ok\":false,\"error\":\"Process ID not found.\"}";
        }
        std::string content;
        if (!snap.doneFilePath.empty()) content = ReadFileUtf8Wide(Utf8ToWide(snap.doneFilePath));
        oss << "{\"ok\":true,\"id\":" << id
            << ",\"done_file\":\"" << JsonEscape(snap.doneFilePath) << "\""
            << ",\"exists\":" << (content.empty() ? "false" : "true")
            << ",\"content\":\"" << JsonEscape(content) << "\"}";
        return oss.str();
    }
    else if (action == "logs" || action == "output") {
        int id = static_cast<int>(JsonGetInt(reqJson, "id", 0));
        int tailLines = static_cast<int>(JsonGetInt(reqJson, "tail", 0));
        bool raw = JsonGetBool(reqJson, "raw", false);
        int waitMs = static_cast<int>(JsonGetInt(reqJson, "wait_ms", 0));

        if (waitMs > 0) {
            pm_.WaitForExit(id, static_cast<uint32_t>(waitMs));
        }

        ProcessSnapshot snap;
        if (!pm_.GetSnapshot(id, snap)) {
            return "{\"ok\":false,\"error\":\"Process ID not found.\"}";
        }
        std::string text;
        pm_.GetOutput(id, !raw, tailLines, text);
        oss << "{\"ok\":true,\"id\":" << id
            << ",\"process\":" << SnapshotToJson(snap)
            << ",\"output\":\"" << JsonEscape(text) << "\"}";
        return oss.str();
    }
    else if (action == "send") {
        int id = static_cast<int>(JsonGetInt(reqJson, "id", 0));
        std::string input = JsonGetString(reqJson, "input", "");
        bool newline = JsonGetBool(reqJson, "newline", true);
        int waitMs = static_cast<int>(JsonGetInt(reqJson, "wait_ms", 300));

        std::string err;
        if (!pm_.SendInput(id, input, newline, err)) {
            oss << "{\"ok\":false,\"error\":\"" << JsonEscape(err) << "\"}";
            return oss.str();
        }
        if (waitMs > 0) {
            SleepMs(static_cast<DWORD>(waitMs));
        }
        ProcessSnapshot snap;
        pm_.GetSnapshot(id, snap);
        std::string outLog;
        pm_.GetOutput(id, true, 50, outLog);
        oss << "{\"ok\":true,\"id\":" << id
            << ",\"process\":" << SnapshotToJson(snap)
            << ",\"recent_output\":\"" << JsonEscape(outLog) << "\"}";
        return oss.str();
    }
    else if (action == "kill") {
        int id = static_cast<int>(JsonGetInt(reqJson, "id", 0));
        std::string err;
        if (!pm_.KillProcess(id, err)) {
            oss << "{\"ok\":false,\"error\":\"" << JsonEscape(err) << "\"}";
            return oss.str();
        }
        HWND hwnd = notifyHwnd_.load();
        if (hwnd) PostMessageW(hwnd, WM_PC_IPC_EVENT, static_cast<WPARAM>(id), 0);
        return "{\"ok\":true,\"killed_id\":" + std::to_string(id) + "}";
    }
    else if (action == "kill_all") {
        int count = pm_.KillAllRunning();
        HWND hwnd = notifyHwnd_.load();
        if (hwnd) PostMessageW(hwnd, WM_PC_IPC_EVENT, 0, 0);
        return "{\"ok\":true,\"killed_count\":" + std::to_string(count) + "}";
    }
    else if (action == "clear") {
        int count = pm_.ClearFinished();
        HWND hwnd = notifyHwnd_.load();
        if (hwnd) PostMessageW(hwnd, WM_PC_IPC_EVENT, 0, 0);
        return "{\"ok\":true,\"cleared_count\":" + std::to_string(count) + "}";
    }
    else if (action == "remove" || action == "process_remove") {
        int id = static_cast<int>(JsonGetInt(reqJson, "id", 0));
        if (id <= 0) {
            return "{\"ok\":false,\"error\":\"id is required for process removal\"}";
        }
        pm_.RemoveProcess(id);
        HWND hwnd = notifyHwnd_.load();
        if (hwnd) PostMessageW(hwnd, WM_PC_IPC_EVENT, static_cast<WPARAM>(id), 0);
        return "{\"ok\":true,\"removed_id\":" + std::to_string(id) + "}";
    }
    else if (action == "reset_ids" || action == "process_reset_ids") {
        std::string err;
        int count = pm_.ResetProcessIds(err);
        if (count < 0) {
            return "{\"ok\":false,\"error\":\"" + JsonEscape(err) + "\"}";
        }
        HWND hwnd = notifyHwnd_.load();
        if (hwnd) PostMessageW(hwnd, WM_PC_IPC_EVENT, 0, 0);
        return "{\"ok\":true,\"count\":" + std::to_string(count) + "}";
    }
    else if (action == "wait") {
        int id = static_cast<int>(JsonGetInt(reqJson, "id", 0));
        int waitMs = static_cast<int>(JsonGetInt(reqJson, "wait_ms", 30000));
        ProcessSnapshot snap;
        if (!pm_.GetSnapshot(id, snap)) {
            return "{\"ok\":false,\"error\":\"Process ID not found.\"}";
        }
        bool finished = pm_.WaitForExit(id, static_cast<uint32_t>(waitMs > 0 ? waitMs : 0));
        if (JsonGetBool(reqJson, "result_only", false)) {
            return AgentResultToJson(pm_, id, finished);
        }
        pm_.GetSnapshot(id, snap);
        std::string outLog;
        pm_.GetOutput(id, true, 200, outLog);
        oss << "{\"ok\":true,\"finished\":" << (finished ? "true" : "false")
            << ",\"process\":" << SnapshotToJson(snap)
            << ",\"output\":\"" << JsonEscape(outLog) << "\"}";
        return oss.str();
    }
    else if (action == "get_agent_stream") {
        int id = static_cast<int>(JsonGetInt(reqJson, "id", 0));
        int tail = static_cast<int>(JsonGetInt(reqJson, "tail", 50));
        std::string typeFilter = JsonGetString(reqJson, "type", "");
        if (typeFilter.empty()) typeFilter = JsonGetString(reqJson, "filter", "");
        uint64_t sinceSeq = static_cast<uint64_t>(JsonGetInt(reqJson, "since_seq", 0));
        std::string format = JsonGetString(reqJson, "format", "json");
        std::string schema = JsonGetString(reqJson, "schema", "");

        if (schema == "v1" || schema == "hancho.event/1") {
            ProcessSnapshot snap;
            if (!pm_.GetSnapshot(id, snap)) {
                return "{\"ok\":false,\"error\":\"Agent stream not found for process ID " + std::to_string(id) + "\"}";
            }
            std::string eventsPath = HanchoEventsFilePathForStream(snap.streamFilePath);
            auto normalized = ReadHanchoEventsFile(eventsPath, tail > 0 ? static_cast<size_t>(tail) : 0,
                                                   typeFilter, sinceSeq);
            oss << "{\"ok\":true,\"schema\":\"hancho.event/1\",\"id\":" << id
                << ",\"agent_type\":\"" << JsonEscape(snap.agentType) << "\""
                << ",\"status\":\"" << JsonEscape(snap.agentStatus) << "\""
                << ",\"events_file\":\"" << JsonEscape(eventsPath) << "\""
                << ",\"events\":[";
            for (size_t i = 0; i < normalized.size(); ++i) {
                if (i) oss << ",";
                oss << normalized[i].dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
            }
            oss << "]}";
            return oss.str();
        }

        std::vector<LlmStreamEvent> events;
        LlmAgentState state;
        if (!pm_.GetAgentStream(id, tail, typeFilter, sinceSeq, events, state)) {
            return "{\"ok\":false,\"error\":\"Agent stream not found for process ID " + std::to_string(id) + "\"}";
        }

        if (format == "summary") {
            std::string summary = LlmEventsToSummary(events, state);
            oss << "{\"ok\":true,\"id\":" << id << ",\"agent_type\":\"" << JsonEscape(state.agentType)
                << "\",\"status\":\"" << JsonEscape(state.status)
                << "\",\"summary\":\"" << JsonEscape(summary) << "\"}";
            return oss.str();
        }

        std::string jsonEvents = LlmEventsToJson(events, state);
        return jsonEvents;
    }
    else if (action == "get_usage_history") {
        int sinceSec = static_cast<int>(JsonGetInt(reqJson, "since_sec", 86400));
        int64_t since = static_cast<int64_t>(time(nullptr)) - sinceSec;
        std::vector<LlmUsagePoint> pts = GetLlmUsageHistory(since);
        oss << "{\"ok\":true,\"record_calls\":" << GetLlmUsageRecordCalls()
            << ",\"count\":" << pts.size() << ",\"points\":[";
        for (size_t i = 0; i < pts.size(); ++i) {
            if (i) oss << ",";
            oss << "{\"epoch\":" << pts[i].epochSec
                << ",\"input\":" << pts[i].inputTokens
                << ",\"output\":" << pts[i].outputTokens
                << ",\"thinking\":" << pts[i].thinkingTokens
                << ",\"cache\":" << (pts[i].cacheReadTokens + pts[i].cacheWriteTokens)
                << ",\"total\":" << pts[i].totalTokens
                << ",\"cost\":" << pts[i].costUsd
                << ",\"agent\":\"" << JsonEscape(pts[i].agentType) << "\""
                << ",\"model\":\"" << JsonEscape(pts[i].model) << "\"}";
        }
        oss << "]}";
        return oss.str();
    }
    else if (action == "get_usage_daily") {
        LlmUsageStatsOptions opts;
        std::string date = JsonGetString(reqJson, "date", "");
        if (!date.empty()) {
            opts.since = date;
            opts.until = date;
        }
        if (opts.since.empty()) {
            opts.since = JsonGetString(reqJson, "since", "");
        }
        if (opts.until.empty()) {
            opts.until = JsonGetString(reqJson, "until", "");
        }
        if (opts.since.empty()) {
            int64_t sinceSec = JsonGetInt(reqJson, "since_sec", 0);
            if (sinceSec > 0) {
                opts.since = std::to_string(sinceSec) + "s";
            }
        }
        opts.groupBy = JsonGetString(reqJson, "groupBy", "");
        if (opts.groupBy.empty()) {
            opts.groupBy = JsonGetString(reqJson, "group_by", "");
        }
        LlmUsageStatsResult res = QueryUsageStats(opts);
        nlohmann::json jres = UsageStatsResultToJson(res);
        return jres.dump();
    }
    else if (action == "get_agent_state") {
        int id = static_cast<int>(JsonGetInt(reqJson, "id", 0));
        std::vector<LlmStreamEvent> events;
        LlmAgentState st;
        if (!pm_.GetAgentStream(id, 1, "", 0, events, st)) {
            ProcessSnapshot snap;
            if (pm_.GetSnapshot(id, snap)) {
                st.agentType = snap.agentType;
                st.status = snap.agentStatus;
                st.currentTool = snap.currentTool;
                st.currentToolAction = snap.currentToolAction;
                st.lastThought = snap.lastThought;
                st.lastMessage = snap.lastMessage;
                st.stepCount = snap.stepCount;
                st.eventCount = snap.streamEventCount;
                st.streamFilePath = snap.streamFilePath;
                st.supervisorNotes = snap.supervisorNotes;
                st.supervisorGoal = snap.supervisorGoal;
            } else {
                return "{\"ok\":false,\"error\":\"Process ID " + std::to_string(id) + " not found\"}";
            }
        }
        oss << "{\"ok\":true,\"id\":" << id
            << ",\"agent_type\":\"" << JsonEscape(st.agentType) << "\""
            << ",\"status\":\"" << JsonEscape(st.status) << "\""
            << ",\"current_tool\":\"" << JsonEscape(st.currentTool) << "\""
            << ",\"current_tool_action\":\"" << JsonEscape(st.currentToolAction) << "\""
            << ",\"last_thought\":\"" << JsonEscape(st.lastThought) << "\""
            << ",\"last_message\":\"" << JsonEscape(st.lastMessage) << "\""
            << ",\"step_count\":" << st.stepCount
            << ",\"event_count\":" << st.eventCount
            << ",\"total_tokens\":" << st.totalTokens
            << ",\"cost_usd\":" << st.costUsd
            << ",\"stream_file\":\"" << JsonEscape(st.streamFilePath) << "\""
            << ",\"supervisor_notes\":\"" << JsonEscape(st.supervisorNotes) << "\""
            << ",\"supervisor_goal\":\"" << JsonEscape(st.supervisorGoal) << "\"";
        if (!st.subagents.empty()) oss << ",\"subagents\":" << LlmSubagentsToJson(st.subagents);
        oss << ",\"updated_at\":\"" << JsonEscape(st.updatedAt) << "\"}";
        return oss.str();
    }
    else if (action == "update_agent_info") {
        int id = static_cast<int>(JsonGetInt(reqJson, "id", 0));
        std::string notes = JsonGetString(reqJson, "supervisor_notes", "");
        if (notes.empty()) notes = JsonGetString(reqJson, "notes", "");
        std::string goal = JsonGetString(reqJson, "supervisor_goal", "");
        if (goal.empty()) goal = JsonGetString(reqJson, "goal", "");
        if (!pm_.UpdateAgentInfo(id, notes, goal)) {
            return "{\"ok\":false,\"error\":\"Failed to update agent info for process ID " + std::to_string(id) + "\"}";
        }
        return "{\"ok\":true,\"id\":" + std::to_string(id) + "}";
    }

    else if (action == "hosts_list" || action == "hosts_reload") {
        HostRegistry registry;
        std::string err;
        if (!registry.Load(err)) {
            return "{\"ok\":false,\"error\":\"" + JsonEscape(err) + "\"}";
        }
        return registry.ToJson(JsonGetBool(reqJson, "token_presence", true));
    }
    else if (action == "hosts_add") {
        RemoteHost host;
        host.name = JsonGetString(reqJson, "name", "");
        host.address = JsonGetString(reqJson, "address", "");
        host.tokenEnv = JsonGetString(reqJson, "token_env", "");
        host.tokenCredential = JsonGetString(reqJson, "token_credential", "");
        host.enabled = JsonGetBool(reqJson, "enabled", true);
        HostRegistry registry;
        std::string err;
        if (!registry.Load(err)) {
            return "{\"ok\":false,\"error\":\"" + JsonEscape(err) + "\"}";
        }
        if (!registry.Add(host, err)) {
            return "{\"ok\":false,\"error\":\"" + JsonEscape(err) + "\"}";
        }
        return "{\"ok\":true,\"host\":\"" + JsonEscape(host.name) + "\"}";
    }
    else if (action == "hosts_remove") {
        HostRegistry registry;
        std::string err;
        if (!registry.Load(err)) {
            return "{\"ok\":false,\"error\":\"" + JsonEscape(err) + "\"}";
        }
        if (!registry.Remove(JsonGetString(reqJson, "name", ""), err)) {
            return "{\"ok\":false,\"error\":\"" + JsonEscape(err) + "\"}";
        }
        return "{\"ok\":true}";
    }
    else if (action == "hosts_ping") {
        const std::string name = JsonGetString(reqJson, "name", "");
        HostRegistry registry;
        std::string err;
        if (!registry.Load(err)) {
            return "{\"ok\":false,\"error\":\"" + JsonEscape(err) + "\"}";
        }
        RemoteHost host;
        if (!registry.Get(name, host)) {
            return "{\"ok\":false,\"error\":\"" + JsonEscape("host not registered: " + name) + "\"}";
        }
        uint64_t started = GetNowTickMs();
        std::string response;
        if (!remote::CallHost(host, "{\"action\":\"ping\"}", response, err)) {
            return "{\"ok\":false,\"host\":\"" + JsonEscape(name) +
                   "\",\"error\":\"" + JsonEscape(err) + "\"}";
        }
        nlohmann::json reply = nlohmann::json::parse(response, nullptr, false);
        nlohmann::json out;
        out["ok"] = true;
        out["host"] = name;
        out["latency_ms"] = GetNowTickMs() - started;
        out["response"] = reply.is_discarded() ? nlohmann::json(response) : reply;
        return out.dump();
    }
    else if (action == "remote_taskqueues" || action == "remote_processes") {
        HostRegistry registry;
        std::string err;
        if (!registry.Load(err)) {
            return "{\"ok\":false,\"error\":\"" + JsonEscape(err) + "\"}";
        }
        nlohmann::json out;
        out["ok"] = true;
        out["hosts"] = nlohmann::json::array();
        for (const auto& host : registry.List()) {
            nlohmann::json item;
            item["host"] = host.name;
            item["address"] = host.address;
            if (!host.enabled) {
                item["error"] = "host is disabled";
                out["hosts"].push_back(std::move(item));
                continue;
            }
            nlohmann::json request;
            std::string response;
            if (action == "remote_taskqueues") {
                request["action"] = "taskqueue_list";
                request["taskqueue"] = JsonGetString(reqJson, "taskqueue", "");
                request["state"] = JsonGetString(reqJson, "state", "");
                request["since"] = JsonGetString(reqJson, "since", "");
            } else {
                request["action"] = "list";
            }
            if (!remote::CallHost(host, request.dump(), response, err)) {
                item["error"] = err;
            } else {
                nlohmann::json reply = nlohmann::json::parse(response, nullptr, false);
                if (reply.is_discarded()) item["error"] = "invalid JSON from remote host";
                else item["response"] = reply;
            }
            out["hosts"].push_back(std::move(item));
        }
        return out.dump();
    }
    else if (action == "failure_diagnose" || action.rfind("taskqueue_", 0) == 0) {
        std::string err;
        std::string resp;
        if (action == "taskqueue_add") {
            const std::string jobsRaw = ExtractRawJsonValue(reqJson, "jobs");
            // A job that names a host must reference a registered, enabled host.
            nlohmann::json jobsDoc = nlohmann::json::parse(jobsRaw, nullptr, false);
            if (jobsDoc.is_array()) {
                std::vector<std::string> wantedHosts;
                for (const auto& job : jobsDoc) {
                    if (!job.is_object()) continue;
                    auto it = job.find("host");
                    if (it != job.end() && it->is_string() && !it->get<std::string>().empty()) {
                        wantedHosts.push_back(it->get<std::string>());
                    }
                }
                if (!wantedHosts.empty()) {
                    HostRegistry registry;
                    std::string hostErr;
                    if (!registry.Load(hostErr)) {
                        err = hostErr;
                        return "{\"ok\":false,\"error\":\"" + JsonEscape(err) + "\"}";
                    }
                    for (const auto& name : wantedHosts) {
                        RemoteHost host;
                        if (!registry.Get(name, host)) {
                            err = "host not registered: " + name + " (add it to hosts.json)";
                            return "{\"ok\":false,\"error\":\"" + JsonEscape(err) + "\"}";
                        }
                        if (!host.enabled) {
                            err = "host is disabled: " + name;
                            return "{\"ok\":false,\"error\":\"" + JsonEscape(err) + "\"}";
                        }
                    }
                }
            }
            resp = tqm_.TaskQueueAdd(JsonGetString(reqJson, "taskqueue", ""),
                                     jobsRaw,
                                     JsonGetInt(reqJson, "concurrency", 0),
                                     JsonGetString(reqJson, "notify_file", ""), err,
                                     ExtractRawJsonValue(reqJson, "origin"));
        } else if (action == "taskqueue_list") {
            int64_t sinceSec = 0;
            const std::string sinceStr = JsonGetString(reqJson, "since", "");
            if (!sinceStr.empty() && !ParseDurationToSec(sinceStr, sinceSec)) {
                err = "invalid since '" + sinceStr + "' (use a number followed by m, h or d, e.g. 6h)";
            } else {
                const std::string tqFilter = JsonGetString(reqJson, "taskqueue", "");
                const std::string stateFilter = JsonGetString(reqJson, "state", "");
                const std::string btFilter = JsonGetString(reqJson, "bt", "");
                resp = tqm_.TaskQueueList(tqFilter, stateFilter, sinceSec, err,
                                          JsonGetString(reqJson, "origin", ""), btFilter,
                                          JsonGetBool(reqJson, "include_hidden", false));
                // Provisional BT rows: idle job/worker leaves of the current branch. They are
                // not stored anywhere, so a branch change on the next query drops them.
                if (err.empty() && btm_ && JsonGetBool(reqJson, "include_planned", false) &&
                    (stateFilter.empty() || stateFilter == "pending" || stateFilter == "planned")) {
                    nlohmann::json doc = nlohmann::json::parse(resp, nullptr, false);
                    if (doc.is_object() && doc.contains("jobs") && doc["jobs"].is_array()) {
                        BtRemainingInfo info = btm_->GetRemainingInfo();
                        for (const auto& t : info.planned) {
                            if (!tqFilter.empty() && t.queueName != tqFilter) continue;
                            if (!btFilter.empty() && t.btId != btFilter) continue;
                            nlohmann::json job;
                            job["id"] = "btplan:" + t.btId + ":" + t.nodeKey;
                            job["summary"] = t.summary.empty() ? t.nodeKey : t.summary;
                            job["state"] = "planned";
                            job["virtual"] = true;
                            job["taskqueue"] = t.queueName;
                            job["timeout_sec"] = t.estimateSec;
                            job["estimate_sec"] = t.estimateSec;
                            job["created_at"] = "";
                            job["bt"] = {{"id", t.btId}, {"node", t.nodeKey}, {"planned", true}};
                            doc["jobs"].push_back(std::move(job));
                        }
                        resp = doc.dump();
                    }
                }
            }
        } else if (action == "taskqueue_get") {
            resp = tqm_.TaskQueueGet(JsonGetString(reqJson, "id", ""),
                                     JsonGetBool(reqJson, "include_instructions", true),
                                     static_cast<int>(JsonGetInt(reqJson, "rev", 0)), err);
        } else if (action == "taskqueue_retry") {
            const std::string retryId = JsonGetString(reqJson, "id", "");
            const bool resetAttempts = JsonGetBool(reqJson, "reset_attempts", true);
            const bool refreshInstr = JsonGetBool(reqJson, "refresh_instructions", true);
            int64_t sinceSec = 0;
            const std::string sinceStr = JsonGetString(reqJson, "since", "");
            if (!sinceStr.empty() && !ParseDurationToSec(sinceStr, sinceSec)) {
                err = "invalid since '" + sinceStr + "' (use a number followed by m, h or d, e.g. 6h)";
            } else if (!retryId.empty()) {
                resp = tqm_.TaskQueueRetry(retryId, resetAttempts, refreshInstr, TaskQueueCaller::External, err);
            } else if (JsonGetBool(reqJson, "failed", false)) {
                // Bulk retry: every failed job, optionally limited to one queue and a time window.
                resp = tqm_.TaskQueueRetryFailed(JsonGetString(reqJson, "taskqueue", ""),
                                                 sinceSec, resetAttempts, refreshInstr, TaskQueueCaller::External, err);
            } else {
                err = "taskqueue_retry requires an id, or failed=true for bulk retry";
            }
        } else if (action == "taskqueue_update") {
            resp = tqm_.TaskQueueUpdate(JsonGetString(reqJson, "id", ""),
                                        ExtractRawJsonValue(reqJson, "patch"), TaskQueueCaller::External, err);
        } else if (action == "taskqueue_cancel") {
            resp = tqm_.TaskQueueCancel(JsonGetString(reqJson, "id", ""), err);
        } else if (action == "taskqueue_pause") {
            resp = tqm_.TaskQueuePause(JsonGetString(reqJson, "taskqueue", ""), true, err);
        } else if (action == "taskqueue_resume") {
            resp = tqm_.TaskQueuePause(JsonGetString(reqJson, "taskqueue", ""), false, err);
        } else if (action == "taskqueue_wait") {
            resp = tqm_.TaskQueueWait(JsonGetString(reqJson, "taskqueue", ""),
                                      static_cast<int>(JsonGetInt(reqJson, "since_version", 0)),
                                      static_cast<int>(JsonGetInt(reqJson, "timeout_ms", 30000)), err);
        } else if (action == "taskqueue_purge") {
            if (reqJson.find("\"older_than_sec\"") != std::string::npos) {
                resp = tqm_.TaskQueuePurgeOlderThan(JsonGetString(reqJson, "taskqueue", ""),
                                                    JsonGetInt(reqJson, "older_than_sec", 0), err);
            } else {
                resp = tqm_.TaskQueuePurge(JsonGetString(reqJson, "taskqueue", ""),
                                           ExtractRawJsonValue(reqJson, "states"), err);
            }
        } else if (action == "taskqueue_link") {
            resp = tqm_.TaskQueueLink(JsonGetString(reqJson, "job", ""),
                                      JsonGetString(reqJson, "depends_on", ""),
                                      JsonGetString(reqJson, "on", ""), err);
        } else if (action == "taskqueue_unlink") {
            resp = tqm_.TaskQueueUnlink(JsonGetString(reqJson, "job", ""),
                                        JsonGetString(reqJson, "depends_on", ""), err);
        } else if (action == "taskqueue_graph") {
            resp = tqm_.TaskQueueGraph(JsonGetString(reqJson, "taskqueue", ""),
                                       JsonGetString(reqJson, "root", ""), err);
        } else if (action == "taskqueue_counts") {
            TaskQueueCounts c = tqm_.GetJobCounts();
            int64_t btPendingSec = 0;
            int btPlanned = 0;
            if (btm_) {
                BtRemainingInfo info = btm_->GetRemainingInfo();
                btPendingSec = info.pendingSec;
                btPlanned = static_cast<int>(info.planned.size());
            }
            int64_t sumRemainingSec = c.sumRemainingSec;
            if (btPendingSec > 0) {
                sumRemainingSec = (sumRemainingSec < 0) ? btPendingSec : sumRemainingSec + btPendingSec;
            }
            const int pendingWithPlanned = c.pending + btPlanned;
            const int totalWithPlanned = c.total + btPlanned;
            nlohmann::json counts;
            counts["running"] = c.running;
            counts["waiting"] = c.waiting;
            counts["pending"] = pendingWithPlanned;
            counts["blocked"] = c.blocked;
            counts["succeeded"] = c.succeeded;
            counts["failed"] = c.failed;
            counts["cancelled"] = c.cancelled;
            counts["total"] = totalWithPlanned;
            counts["bt_planned"] = btPlanned;
            counts["bt_remaining_sec"] = btPendingSec;
            counts["min_remaining_sec"] = c.minRemainingSec;
            counts["sum_remaining_sec"] = sumRemainingSec;
            nlohmann::json out;
            out["ok"] = true;
            out["counts"] = counts;
            out["running"] = c.running;
            out["waiting"] = c.waiting;
            out["pending"] = pendingWithPlanned;
            out["blocked"] = c.blocked;
            out["succeeded"] = c.succeeded;
            out["failed"] = c.failed;
            out["cancelled"] = c.cancelled;
            out["total"] = totalWithPlanned;
            out["bt_planned"] = btPlanned;
            out["bt_remaining_sec"] = btPendingSec;
            out["min_remaining_sec"] = c.minRemainingSec;
            out["sum_remaining_sec"] = sumRemainingSec;
            return out.dump();
        } else if (action == "taskqueue_resource_locks") {
            if (reqJson.find("\"enabled\"") != std::string::npos) {
                bool enabled = JsonGetBool(reqJson, "enabled", true);
                tqm_.SetResourceLocksEnabled(enabled);
            }
            nlohmann::json out;
            out["ok"] = true;
            out["enabled"] = tqm_.GetResourceLocksEnabled();
            return out.dump();
        } else if (action == "taskqueue_diagnose" || action == "failure_diagnose") {
            int procId = static_cast<int>(JsonGetInt(reqJson, "process_id", 0));
            if (procId <= 0) procId = static_cast<int>(JsonGetInt(reqJson, "proc_id", 0));
            std::string jobId = JsonGetString(reqJson, "job_id", "");
            if (jobId.empty()) jobId = JsonGetString(reqJson, "id", "");
            std::string tqName = JsonGetString(reqJson, "taskqueue", "");
            bool breakdown = JsonGetBool(reqJson, "breakdown", false);

            if (breakdown || (!tqName.empty() && procId <= 0 && jobId.empty())) {
                std::string listErr;
                std::string listResp = tqm_.TaskQueueList(tqName, "failed", 0, listErr);
                nlohmann::json root = nlohmann::json::parse(listResp, nullptr, false);
                std::map<std::string, int> classCounts;
                if (root.is_object() && root.contains("jobs") && root["jobs"].is_array()) {
                    for (const auto& job : root["jobs"]) {
                        if (!job.is_object()) continue;
                        auto attempts = job.find("attempts");
                        if (attempts != job.end() && attempts->is_array()) {
                            for (const auto& att : *attempts) {
                                if (!att.is_object()) continue;
                                auto f = att.find("failure");
                                if (f != att.end() && f->is_object() && !f->empty()) {
                                    std::string c = f->value("class", "unknown");
                                    classCounts[c]++;
                                }
                            }
                        }
                    }
                }
                nlohmann::json classesObj = nlohmann::json::object();
                std::ostringstream summary;
                bool first = true;
                for (const auto& kv : classCounts) {
                    classesObj[kv.first] = kv.second;
                    if (!first) summary << ", ";
                    summary << kv.first << " x" << kv.second;
                    first = false;
                }
                nlohmann::json out;
                out["ok"] = true;
                out["taskqueue"] = tqName;
                out["classes"] = classesObj;
                out["summary"] = summary.str();
                return out.dump();
            }

            if (procId > 0) {
                std::string listErr;
                std::string listResp = tqm_.TaskQueueList("", "", 0, listErr);
                nlohmann::json root = nlohmann::json::parse(listResp, nullptr, false);
                if (root.is_object() && root.contains("jobs") && root["jobs"].is_array()) {
                    for (const auto& job : root["jobs"]) {
                        if (!job.is_object()) continue;
                        auto attempts = job.find("attempts");
                        if (attempts == job.end() || !attempts->is_array()) continue;
                        for (const auto& attempt : *attempts) {
                            if (!attempt.is_object()) continue;
                            if (attempt.value("process_id", 0) == procId) {
                                auto f = attempt.find("failure");
                                nlohmann::json out;
                                out["ok"] = true;
                                out["found"] = true;
                                out["process_id"] = procId;
                                out["job_id"] = job.value("id", "");
                                out["taskqueue"] = job.value("taskqueue", "");
                                out["attempt"] = attempt.value("attempt", 1);
                                out["failure"] = (f != attempt.end() && f->is_object()) ? *f : nlohmann::json::object();
                                return out.dump();
                            }
                        }
                    }
                }
                ProcessSnapshot snap;
                if (pm_.GetSnapshot(procId, snap)) {
                    nlohmann::json out;
                    out["ok"] = true;
                    out["found"] = true;
                    out["process_id"] = procId;
                    out["state"] = ProcStateToString(snap.state);
                    out["exit_code"] = snap.exitCode;
                    nlohmann::json fail;
                    fail["class"] = (snap.state == ProcState::TimedOut || snap.state == ProcState::IdleTimedOut) ? "timeout" : (snap.exitCode != 0 ? "exit_nonzero" : "none");
                    fail["summary"] = snap.lastLinePreview;
                    out["failure"] = fail;
                    return out.dump();
                }
                return "{\"ok\":false,\"error\":\"process not found: " + std::to_string(procId) + "\"}";
            }

            if (!jobId.empty()) {
                std::string getErr;
                std::string getResp = tqm_.TaskQueueGet(jobId, false, 0, getErr);
                nlohmann::json root = nlohmann::json::parse(getResp, nullptr, false);
                nlohmann::json job = (root.is_object() && root.contains("job") && root["job"].is_object()) ? root["job"] : root;
                if (job.is_object() && job.contains("attempts") && job["attempts"].is_array()) {
                    nlohmann::json lastFailure = nlohmann::json::object();
                    int lastAttempt = 0;
                    for (const auto& att : job["attempts"]) {
                        if (!att.is_object()) continue;
                        lastAttempt = att.value("attempt", lastAttempt);
                        auto f = att.find("failure");
                        if (f != att.end() && f->is_object() && !f->empty()) {
                            lastFailure = *f;
                        }
                    }
                    nlohmann::json out;
                    out["ok"] = true;
                    out["found"] = true;
                    out["job_id"] = jobId;
                    out["attempt"] = lastAttempt;
                    out["failure"] = lastFailure;
                    return out.dump();
                }
                return "{\"ok\":false,\"error\":\"job not found or has no attempts: " + JsonEscape(jobId) + "\"}";
            }
            return "{\"ok\":false,\"error\":\"taskqueue_diagnose requires process_id, job_id, or taskqueue\"}";
        }
        if (!resp.empty()) {
            HWND hwnd = notifyHwnd_.load();
            if (hwnd) PostMessageW(hwnd, WM_PC_IPC_EVENT, 0, 0);
            return resp;
        }
        return "{\"ok\":false,\"error\":\"" + JsonEscape(err.empty() ? "unknown taskqueue action" : err) + "\"}";
    }
    else if (action.rfind("bt_", 0) == 0) {
        if (!btm_) {
            return "{\"ok\":false,\"error\":\"behavior trees are not available\"}";
        }
        using json = nlohmann::json;
        json req = json::parse(reqJson, nullptr, false);
        if (req.is_discarded() || !req.is_object()) {
            return "{\"ok\":false,\"error\":\"invalid JSON request\"}";
        }

        auto extractBtPayload = [&](std::string& outJsonText, std::string& outErr) -> bool {
            for (const char* k : {"bt", "patch", "nodeDef", "def", "json"}) {
                if (req.contains(k) && !req[k].is_null()) {
                    if (req[k].is_object() || req[k].is_array()) {
                        outJsonText = req[k].dump();
                        return true;
                    }
                    if (req[k].is_string()) {
                        outJsonText = req[k].get<std::string>();
                        return !outJsonText.empty();
                    }
                }
            }
            if (req.contains("file") && req["file"].is_string()) {
                std::string fileUtf8 = req["file"].get<std::string>();
                if (fileUtf8.empty()) {
                    outErr = "file path cannot be empty";
                    return false;
                }
                std::wstring absPath = ResolveAbsolutePathWide(Utf8ToWide(fileUtf8));
                std::ifstream ifs(absPath, std::ios::binary);
                if (!ifs) {
                    outErr = "cannot open file: " + fileUtf8;
                    return false;
                }
                std::ostringstream ss;
                ss << ifs.rdbuf();
                outJsonText = ss.str();
                return true;
            }
            if (action == "bt_update") {
                json patch = req;
                patch.erase("action");
                patch.erase("id");
                patch.erase("node");
                if (!patch.empty()) {
                    outJsonText = patch.dump();
                    return true;
                }
            }
            outErr = action + " requires 'bt' (JSON) or 'file'";
            return false;
        };

        std::string resp;
        if (action == "bt_validate") {
            std::string btJson, err;
            if (!extractBtPayload(btJson, err)) {
                return "{\"ok\":false,\"valid\":false,\"error\":\"" + JsonEscape(err) + "\"}";
            }
            resp = btm_->Validate(btJson);
        } else if (action == "bt_start") {
            std::string btJson, err;
            if (!extractBtPayload(btJson, err)) {
                return "{\"ok\":false,\"error\":\"" + JsonEscape(err) + "\"}";
            }
            // Jobs the tree adds carry the origin of whoever started it; set it before
            // Start, which may already add the first jobs.
            if (req.contains("origin") && req["origin"].is_object()) {
                auto btDef = json::parse(btJson, nullptr, false);
                if (btDef.is_object() && btDef.contains("id") && btDef["id"].is_string()) {
                    tqm_.SetBtOrigin(btDef["id"].get<std::string>(), req["origin"].dump());
                }
            }
            resp = btm_->Start(btJson, JsonGetBool(reqJson, "paused", false));
        } else if (action == "bt_get") {
            std::string id = req.value("id", std::string());
            bool compact = req.value("compact", false);
            resp = btm_->Get(id, compact);
        } else if (action == "bt_list") {
            resp = btm_->List();
        } else if (action == "bt_wait") {
            std::string id = req.value("id", std::string());
            uint64_t sinceVer = 0;
            if (req.contains("sinceVersion") && req["sinceVersion"].is_number()) {
                sinceVer = req["sinceVersion"].get<uint64_t>();
            } else if (req.contains("since_version") && req["since_version"].is_number()) {
                sinceVer = req["since_version"].get<uint64_t>();
            }
            int timeoutMs = 60000;
            if (req.contains("timeoutMs") && req["timeoutMs"].is_number_integer()) {
                timeoutMs = req["timeoutMs"].get<int>();
            } else if (req.contains("timeout_ms") && req["timeout_ms"].is_number_integer()) {
                timeoutMs = req["timeout_ms"].get<int>();
            }
            bool compact = req.value("compact", false);
            resp = btm_->Wait(id, sinceVer, timeoutMs, compact);
        } else if (action == "bt_resume") {
            std::string id = req.value("id", std::string());
            std::string node = req.value("node", std::string());
            std::string answer = req.value("answer", std::string());
            std::string rationale = req.value("rationale", std::string());
            std::string varsJson;
            if (req.contains("vars") && !req["vars"].is_null()) {
                if (req["vars"].is_object()) varsJson = req["vars"].dump();
                else if (req["vars"].is_string()) varsJson = req["vars"].get<std::string>();
                else varsJson = req["vars"].dump();
            }
            resp = btm_->Resume(id, node, answer, rationale, varsJson);
        } else if (action == "bt_retry") {
            std::string id = req.value("id", std::string());
            std::string from = req.value("from", req.value("fromNode", req.value("from_node", req.value("node", std::string()))));
            bool resetCounters = false;
            if (req.contains("resetCounters") && req["resetCounters"].is_boolean()) {
                resetCounters = req["resetCounters"].get<bool>();
            } else if (req.contains("reset_counters") && req["reset_counters"].is_boolean()) {
                resetCounters = req["reset_counters"].get<bool>();
            }
            resp = btm_->Retry(id, from, resetCounters);
        } else if (action == "bt_update") {
            std::string id = req.value("id", std::string());
            std::string node = req.value("node", std::string());
            std::string btJson, err;
            if (!extractBtPayload(btJson, err)) {
                return "{\"ok\":false,\"error\":\"" + JsonEscape(err) + "\"}";
            }
            resp = btm_->Update(id, node, btJson);
        } else if (action == "bt_cancel") {
            std::string id = req.value("id", std::string());
            resp = btm_->Cancel(id);
        } else if (action == "bt_pause") {
            std::string id = req.value("id", std::string());
            bool paused = true;
            if (req.contains("paused")) {
                if (req["paused"].is_boolean()) paused = req["paused"].get<bool>();
                else if (req["paused"].is_string()) paused = (req["paused"].get<std::string>() != "false" && req["paused"].get<std::string>() != "0");
            }
            resp = btm_->Pause(id, paused);
        } else {
            return "{\"ok\":false,\"error\":\"Unknown BT IPC action: " + JsonEscape(action) + "\"}";
        }

        HWND hwnd = notifyHwnd_.load();
        if (hwnd) PostMessageW(hwnd, WM_PC_IPC_EVENT, 0, 0);
        return resp;
    }
    else if (action.rfind("memory_", 0) == 0) {
        if (action == "memory_search") {
            memory::Query q;
            std::string qErr;
            memory::ParseQueryJson(reqJson, q, qErr);
            if (q.text.empty()) {
                q.text = JsonGetString(reqJson, "query", "");
            }
            if (q.text.empty()) {
                q.text = JsonGetString(reqJson, "text", "");
            }
            return memory::SearchJson(q);
        } else if (action == "memory_get") {
            std::string id = JsonGetString(reqJson, "id", "");
            std::string kind = JsonGetString(reqJson, "kind", "");
            if (id.empty()) {
                return "{\"ok\":false,\"error\":\"id is required for memory_get\"}";
            }
            return memory::GetJson(id, kind);
        } else if (action == "memory_stats") {
            memory::Query q;
            std::string qErr;
            memory::ParseQueryJson(reqJson, q, qErr);
            return memory::StatsJson(q);
        } else if (action == "memory_storage_stats") {
            auto stats = memory::GetMemoryStorageStats();
            nlohmann::json out;
            out["ok"] = true;
            out["total_bytes"] = stats.totalBytes;
            out["limit_bytes"] = stats.limitBytes;
            out["l1_bytes"] = stats.l1Bytes;
            out["l2_bytes"] = stats.l2Bytes;
            out["l3_bytes"] = stats.l3Bytes;
            out["index_bytes"] = stats.indexBytes;
            out["other_bytes"] = stats.otherBytes;
            out["near_limit"] = stats.nearLimit;
            out["exceeded"] = stats.exceeded;
            out["warning"] = stats.warning;
            return out.dump();
        } else if (action == "memory_retract") {
            std::string id = JsonGetString(reqJson, "id", "");
            if (id.empty()) {
                return "{\"ok\":false,\"error\":\"id is required for memory_retract\"}";
            }
            std::string reason = JsonGetString(reqJson, "reason", "");
            std::string who = JsonGetString(reqJson, "retracted_by", "gui");
            return memory::RetractJson(id, who, reason);
        } else if (action == "memory_promote") {
            std::string id = JsonGetString(reqJson, "id", "");
            if (id.empty()) {
                return "{\"ok\":false,\"error\":\"id is required for memory_promote\"}";
            }
            std::string approvedBy = JsonGetString(reqJson, "approved_by", "gui:human");
            return memory::ApproveJson(id, approvedBy);
        } else if (action == "memory_compare" || action == "memory_inject_compare") {
            std::string job = JsonGetString(reqJson, "job", "");
            if (job.empty()) job = JsonGetString(reqJson, "job_name", "");
            std::string tq = JsonGetString(reqJson, "taskqueue", "");
            std::string since = JsonGetString(reqJson, "since", "");
            return MemoryInjectCompareJson(job, tq, since);
        } else if (action == "memory_prune") {
            memory::PruneOptions opts;
            opts.dryRun = JsonGetBool(reqJson, "dry_run", false);
            opts.retentionDays = static_cast<int>(JsonGetInt(reqJson, "retention_days", 90));
            return memory::PruneRunRecordsJson(opts);
        }
        return "{\"ok\":false,\"error\":\"Unknown memory action: " + JsonEscape(action) + "\"}";
    }
    else if (action.rfind("chat_", 0) == 0 || action.rfind("ipmsg_", 0) == 0) {
        return HandleChatAction(action, reqJson);
    }
    else if (action.rfind("config_", 0) == 0) {
        std::wstring configPath = PathJoinWide(GetEffectiveDataDirectoryWide(), L"config.json");
        if (action == "config_get") {
            std::string content = ReadFileUtf8Wide(configPath);
            nlohmann::json cfg = nlohmann::json::object();
            if (!content.empty()) {
                nlohmann::json parsed = nlohmann::json::parse(content, nullptr, false);
                if (parsed.is_object()) cfg = parsed;
            }
            if (!cfg.contains("resource_locks")) {
                cfg["resource_locks"] = tqm_.GetResourceLocksEnabled();
            }
            if (!cfg.contains("llm_max_events")) {
                cfg["llm_max_events"] = GetLlmMaxEvents();
            }
            if (!cfg.contains("restore_usage_graph")) {
                cfg["restore_usage_graph"] = IsUsageHistoryPersistence();
            }
            if (!cfg.contains("webui") || !cfg["webui"].is_object()) {
                cfg["webui"] = nlohmann::json::object();
            }
            {
                nlohmann::json& w = cfg["webui"];
                if (!w.contains("enabled")) w["enabled"] = true;
                if (!w.contains("scope")) w["scope"] = "loopback";
                if (!w.contains("host")) w["host"] = "0.0.0.0";
                if (!w.contains("port")) w["port"] = 9138;
                if (!w.contains("allowed_hosts")) w["allowed_hosts"] = nlohmann::json::array();
                if (!w.contains("allowed_origins")) w["allowed_origins"] = nlohmann::json::array();
            }
            if (!cfg.contains("terminal") || !cfg["terminal"].is_object()) {
                cfg["terminal"] = nlohmann::json::object();
            }
            {
                nlohmann::json& t = cfg["terminal"];
                if (!t.contains("local")) t["local"] = true;
                if (!t.contains("remote")) t["remote"] = false;
                if (!t.contains("host")) t["host"] = "0.0.0.0";
                if (!t.contains("port")) t["port"] = 9139;
                if (!t.contains("token")) t["token"] = "";
                if (!t.contains("history_bytes")) t["history_bytes"] = 32 * 1024 * 1024;
            }
            {
                nlohmann::json& m = EnsureChatBlock(cfg);
                nlohmann::json defaults = IpmsgConfig().ToJson();
                for (auto it = defaults.begin(); it != defaults.end(); ++it) {
                    if (!m.contains(it.key())) m[it.key()] = it.value();
                }
                if (!m.contains("handler") || !m["handler"].is_object()) {
                    m["handler"] = nlohmann::json::object();
                }
                nlohmann::json handlerDefaults = IpmsgHandlerConfig().ToJson();
                for (auto it = handlerDefaults.begin(); it != handlerDefaults.end(); ++it) {
                    if (!m["handler"].contains(it.key())) m["handler"][it.key()] = it.value();
                }
            }
            nlohmann::json out;
            out["ok"] = true;
            out["config"] = cfg;
            return out.dump();
        } else if (action == "config_set") {
            nlohmann::json req = nlohmann::json::parse(reqJson, nullptr, false);
            if (req.is_discarded() || !req.is_object()) {
                return "{\"ok\":false,\"error\":\"invalid JSON request\"}";
            }
            nlohmann::json patch = nlohmann::json::object();
            if (req.contains("config") && req["config"].is_object()) {
                patch = req["config"];
            } else if (req.contains("patch") && req["patch"].is_object()) {
                patch = req["patch"];
            } else {
                patch = req;
                patch.erase("action");
            }
            std::string content = ReadFileUtf8Wide(configPath);
            nlohmann::json cfg = nlohmann::json::object();
            if (!content.empty()) {
                nlohmann::json parsed = nlohmann::json::parse(content, nullptr, false);
                if (parsed.is_object()) cfg = parsed;
            }
            EnsureChatBlock(cfg);
            if (patch.contains("ipmsg") && !patch.contains("chat")) {
                patch["chat"] = patch["ipmsg"];
            }
            patch.erase("ipmsg");
            for (auto it = patch.begin(); it != patch.end(); ++it) {
                if (it.key() == "chat" && it.value().is_object()) {
                    nlohmann::json& chat = EnsureChatBlock(cfg);
                    for (auto cit = it.value().begin(); cit != it.value().end(); ++cit) {
                        if (cit.key() == "handler" && cit.value().is_object() &&
                            chat.contains("handler") && chat["handler"].is_object()) {
                            for (auto hit = cit.value().begin(); hit != cit.value().end(); ++hit) {
                                chat["handler"][hit.key()] = hit.value();
                            }
                        } else {
                            chat[cit.key()] = cit.value();
                        }
                    }
                } else {
                    cfg[it.key()] = it.value();
                }
            }
            if (cfg.contains("resource_locks") && cfg["resource_locks"].is_boolean()) {
                tqm_.SetResourceLocksEnabled(cfg["resource_locks"].get<bool>());
            }
            if (cfg.contains("llm_max_events") && cfg["llm_max_events"].is_number()) {
                SetLlmMaxEvents(cfg["llm_max_events"].get<int64_t>());
            }
            if (cfg.contains("process_metrics_interval_ms") &&
                cfg["process_metrics_interval_ms"].is_number_integer()) {
                pm_.SetMetricsIntervalMs(static_cast<int>(cfg["process_metrics_interval_ms"].get<int64_t>()));
            }
            if (cfg.contains("restore_usage_graph") && cfg["restore_usage_graph"].is_boolean()) {
                SetUsageHistoryPersistence(cfg["restore_usage_graph"].get<bool>());
            }
            const bool webuiChanged = patch.contains("webui");
            const bool chatChanged = patch.contains("chat");
            const bool terminalChanged = patch.contains("terminal");
            WriteFileAtomicWide(configPath, cfg.dump(2));
            if (webuiChanged) ScheduleWebUiRestart();
            if (chatChanged) ScheduleIpmsgRestart();
            if (terminalChanged) ScheduleTermServerRestart();
            nlohmann::json out;
            out["ok"] = true;
            out["config"] = cfg;
            return out.dump();
        }
        return "{\"ok\":false,\"error\":\"Unknown config action: " + JsonEscape(action) + "\"}";
    }
    return "{\"ok\":false,\"error\":\"Unknown IPC action: " + JsonEscape(action) + "\"}";
}

} // namespace pc
