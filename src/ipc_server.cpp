#include "ipc_server.h"
#include "bt_manager.h"
#include "bt_templates.h"
#include "behavior_tree_internal.h"
#include "chain_guard.h"
#include "llm_config.h"
#include "common.h"
#include "event_schema.h"
#include "ipmsg.h"
#include "ipmsg_handler.h"
#include "llm_stream.h"
#include "memory_record.h"
#include "memory_search.h"
#include "power.h"
#include "remote_hosts.h"
#include "routine.h"
#include "triage.h"
#include "triage_manager.h"
#include "translation_store.h"
#include "permit_draft.h"
#include "webui_server.h"
#include "term_server.h"
#include "nlohmann/json.hpp"
#include <atomic>
#include <ctime>
#include <functional>
#include <fstream>
#include <sstream>
#include <cctype>
#include <set>

namespace pc {

constexpr UINT WM_PC_IPC_EVENT = WM_APP + 101;

// 093-5 (defined below): shared by routine_ask and triage's update_routine handler.
static std::string HandleRoutineAsk(RoutineManager& routines, BtManager* btm,
                                    TaskQueueManager* tqm, const std::string& requestText,
                                    const std::string& requestedName, const std::string& cwd,
                                    const std::string& scope, const std::string& by,
                                    int timeoutMs);

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
        << "\"idle_first_event_sec\":" << s.idleFirstEventSec << ","
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
        << "\"engine\":\"" << JsonEscape(s.agentType) << "\","
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


// Task-group / direction rows in the task list have ids "direction:<dirId>" or legacy "group:<treeId>";
// the behavior tree manager keys trees by the bare btId, so strip prefixes or resolve direction->btId.
static std::string StripGroupRowPrefix(const std::string& id) {
    return id.rfind("group:", 0) == 0 ? id.substr(6) : id;
}

static std::string StripDirectionRowPrefix(const std::string& id) {
    return id.rfind("direction:", 0) == 0 ? id.substr(10) : id;
}

static bool IsDirectionOrDirId(const std::string& id) {
    return id.rfind("direction:", 0) == 0 || id.rfind("dir_", 0) == 0;
}

static std::string AsciiLower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// A task group waiting for a decision is reported as "waitingForLlm". Callers often ask
// for "waiting" instead, so treat that as an alias; otherwise the row silently disappears
// from exactly the list a supervisor uses to decide whether to answer an escalation.
static bool TaskGroupStateMatches(const std::string& filter, const std::string& state) {
    if (filter.empty()) return true;
    const std::string f = AsciiLower(filter);
    const std::string s = AsciiLower(state);
    if (f == s) return true;
    if (f == "waiting" || f == "awaiting" || f == "awaiting_decision" || f == "awaitingdecision") {
        return s == "waitingforllm" || s == "paused";
    }
    return false;
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

void IpcServer::InstallRoutineValidator() {
    if (!routineManager_) return;
    if (!btm_) {
        routineManager_->SetValidator(nullptr);
        return;
    }
    routineManager_->SetValidator([this](const std::string& btJson, std::string& outError) -> bool {
        nlohmann::json result = nlohmann::json::parse(btm_->Validate(btJson, false), nullptr, false);
        if (result.is_discarded() || !result.is_object()) {
            outError = "invalid validation response";
            return false;
        }
        if (result.value("ok", false) && result.value("valid", false)) return true;
        outError = result.value("error", std::string("invalid definition"));
        if (outError.empty() && result.contains("errors") && result["errors"].is_array()) {
            outError = result["errors"].dump();
        }
        return false;
    });
}

IpcServer::IpcServer(ProcessManager& pm, TaskQueueManager& tqm, HWND notifyHwnd)
    : pm_(pm), tqm_(tqm), btm_(nullptr), notifyHwnd_(notifyHwnd),
      webuiRestart_([this]() { RestartWebUi(); }),
      termRestart_([this]() { RestartTermServer(); }),
      ipmsgRestart_([this]() { RestartIpmsg(); }) {
    routineManager_ = std::make_unique<RoutineManager>();
    InstallRoutineValidator();
    triageManager_ = std::make_unique<TriageManager>(pm_, tqm_, btm_);
    permitDraft_ = std::make_unique<PermitDraftManager>(pm_, btm_);
    triageManager_->SetRoutineAskHandler([this](const std::string& name, const std::string& cwd,
                                                const std::string& request) {
        return HandleRoutineAsk(*routineManager_, btm_, &tqm_, request, name,
                                cwd.empty() ? WideToUtf8(GetCurrentDirectoryWide()) : cwd,
                                "project", "triage", 600000);
    });
    ipmsg_ = std::make_unique<IpmsgNode>();
    ipmsgHandler_ = std::make_unique<IpmsgHandler>(tqm_, pm_, ipmsg_.get());
    InstallRemoteCallbacks();
}

IpcServer::IpcServer(ProcessManager& pm, TaskQueueManager& tqm, BtManager* btm, HWND notifyHwnd)
    : pm_(pm), tqm_(tqm), btm_(btm), notifyHwnd_(notifyHwnd),
      webuiRestart_([this]() { RestartWebUi(); }),
      termRestart_([this]() { RestartTermServer(); }),
      ipmsgRestart_([this]() { RestartIpmsg(); }) {
    routineManager_ = std::make_unique<RoutineManager>();
    InstallRoutineValidator();
    triageManager_ = std::make_unique<TriageManager>(pm_, tqm_, btm_);
    permitDraft_ = std::make_unique<PermitDraftManager>(pm_, btm_);
    triageManager_->SetRoutineAskHandler([this](const std::string& name, const std::string& cwd,
                                                const std::string& request) {
        return HandleRoutineAsk(*routineManager_, btm_, &tqm_, request, name,
                                cwd.empty() ? WideToUtf8(GetCurrentDirectoryWide()) : cwd,
                                "project", "triage", 600000);
    });
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
    // Stop the restart workers first: they must not touch the servers while
    // they are being closed.
    webuiRestart_.Stop();
    termRestart_.Stop();
    ipmsgRestart_.Stop();
    StopWebUiServer(true);
    StopTermServer();
    StopIpmsg();
    if (ipmsgHandler_) ipmsgHandler_->Stop();
    // Wait for the handlers that were detached by a config change.
    {
        std::lock_guard<std::mutex> lock(webuiMutex_);
        for (auto& retired : retiredWebUiServers_) {
            if (retired) retired->Stop(true);
        }
        retiredWebUiServers_.clear();
    }
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
        << ",\"engine\":\"" << JsonEscape(ex.agentState.agentType) << "\""
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
    if (act == "chat_peers" || act == "chat_hosts") {
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
        std::string fromPeer = IpmsgReqString(req, "from_peer");
        if (fromPeer.empty()) fromPeer = IpmsgReqString(req, "from_agent");
        std::string toPeer = IpmsgReqString(req, "to_peer");
        if (toPeer.empty()) toPeer = IpmsgReqString(req, "to_agent");
        return ipmsg_->Send(IpmsgReqString(req, "to"), IpmsgReqString(req, "text"),
                            fromPeer, toPeer,
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

static void CollectRoutineNodes(const nlohmann::json& node, const std::string& prefix,
                                TaskQueueManager& tqm, nlohmann::json& rows) {
    using json = nlohmann::json;
    if (node.is_array()) {
        int index = 0;
        for (const auto& child : node) {
            CollectRoutineNodes(child, prefix + "/" + std::to_string(index++), tqm, rows);
        }
        return;
    }
    if (!node.is_object()) return;

    std::string key;
    if (node.contains("id") && node["id"].is_string()) {
        key = node["id"].get<std::string>();
    } else if (prefix.empty()) {
        key = node.value("type", std::string("root"));
    } else {
        key = prefix;
    }

    if (node.contains("status") && node["status"].is_object()) {
        const json& status = node["status"];
        json row;
        row["node"] = key;
        row["type"] = node.value("type", std::string());
        row["summary"] = node.value("summary", std::string());
        row["state"] = status.value("state", std::string("idle"));
        row["runs"] = status.value("runs", 0);

        json jobResults = json::array();
        int64_t totalMs = 0;
        if (status.contains("jobs") && status["jobs"].is_array()) {
            for (const auto& jobIdValue : status["jobs"]) {
                if (!jobIdValue.is_string()) continue;
                const std::string jobId = jobIdValue.get<std::string>();
                TaskQueueJob job;
                json jobRow;
                jobRow["id"] = jobId;
                if (!tqm.GetJobSnapshot(jobId, job)) {
                    jobRow["state"] = "missing";
                    jobResults.push_back(std::move(jobRow));
                    continue;
                }
                jobRow["state"] = job.state;
                if (!job.attempts.empty()) {
                    const TaskQueueAttempt& attempt = job.attempts.back();
                    jobRow["attempt"] = attempt.n;
                    jobRow["exit_code"] = attempt.exitCode;
                    jobRow["check_detail"] = attempt.checkDetail;
                    const int64_t started = ParseTimestampStrToEpoch(attempt.start);
                    const int64_t ended = ParseTimestampStrToEpoch(attempt.end);
                    const int64_t durationMs = (started > 0 && ended >= started) ? (ended - started) * 1000 : 0;
                    jobRow["duration_ms"] = durationMs;
                    totalMs += durationMs;
                    if (attempt.testReportSummary.total > 0) {
                        json summary;
                        summary["total"] = attempt.testReportSummary.total;
                        summary["failed"] = attempt.testReportSummary.failed;
                        summary["skipped"] = attempt.testReportSummary.skipped;
                        summary["failed_names"] = attempt.testReportSummary.failedNames;
                        jobRow["test_report_summary"] = std::move(summary);
                    }
                }
                jobResults.push_back(std::move(jobRow));
            }
        }
        row["jobs"] = jobResults;
        row["duration_ms"] = totalMs;
        rows.push_back(std::move(row));
    }

    for (const char* childKey : {"root", "children", "cond", "then", "else", "child", "until", "handlers"}) {
        auto it = node.find(childKey);
        if (it != node.end()) CollectRoutineNodes(*it, key, tqm, rows);
    }
}

static nlohmann::json RoutineNodeResults(BtManager* btm, TaskQueueManager& tqm, const std::string& runId) {
    using json = nlohmann::json;
    json rows = json::array();
    if (!btm) return rows;
    json tree = json::parse(btm->Get(runId, false), nullptr, false);
    if (tree.is_discarded() || !tree.value("ok", false) || !tree.contains("tree")) return rows;
    CollectRoutineNodes(tree["tree"], "", tqm, rows);
    return rows;
}

// 093-3: compare a run's node results with the previous run and report newly failed / fixed nodes.
static void AddRoutineNodeDiff(nlohmann::json& resp, BtManager* btm, TaskQueueManager& tqm,
                               const nlohmann::json& runs, const nlohmann::json& nodes) {
    if (!btm || !runs.is_array() || runs.size() < 2) return;
    const auto failedNodes = [](const nlohmann::json& list) {
        std::set<std::string> out;
        for (const auto& node : list) {
            if (!node.is_object()) continue;
            const std::string state = node.value("state", std::string());
            if (state != "succeeded" && state != "skipped") out.insert(node.value("node", std::string()));
        }
        return out;
    };
    const auto previousRun = runs[runs.size() - 2];
    if (!previousRun.is_object()) return;
    const std::string previousId = previousRun.value("runId", std::string());
    if (previousId.empty()) return;
    const std::set<std::string> before = failedNodes(RoutineNodeResults(btm, tqm, previousId));
    const std::set<std::string> after = failedNodes(nodes.is_array() ? nodes : nlohmann::json::array());
    std::vector<std::string> newlyFailed, fixed;
    for (const auto& name : after) if (!before.count(name)) newlyFailed.push_back(name);
    for (const auto& name : before) if (!after.count(name)) fixed.push_back(name);
    resp["newlyFailed"] = nlohmann::json::array();
    for (const auto& name : newlyFailed) resp["newlyFailed"].push_back(name);
    resp["fixed"] = nlohmann::json::array();
    for (const auto& name : fixed) resp["fixed"].push_back(name);
}

static nlohmann::json RoutineFailedNodes(BtManager* btm, TaskQueueManager& tqm, const std::string& runId) {
    nlohmann::json failed = nlohmann::json::array();
    for (const auto& node : RoutineNodeResults(btm, tqm, runId)) {
        if (!node.is_object()) continue;
        const std::string state = node.value("state", std::string());
        if (state != "succeeded" && state != "skipped" && state != "cancelled") {
            failed.push_back(node.value("node", std::string()));
        }
    }
    return failed;
}

// Captures the final state of a routine run: advances the snapshot on success and opens a
// `routine_stale_failure` triage card on failure when the routine was stale at start.
static void FinalizeRoutineRun(RoutineManager& routines, TriageManager* triage, BtManager* btm,
                               TaskQueueManager& tqm, const std::string& name, const std::string& cwd,
                               const std::string& runId, const std::string& state,
                               bool wasStale, const nlohmann::json& diff) {
    if (state != "succeeded" && state != "failed") return;
    std::string err;
    routines.UpdateLatestRun(name, {{"state", state}, {"finishedAt", GetCurrentTimestampStr()}}, err);
    if (!wasStale) return;
    if (state == "succeeded") {
        routines.RefreshInputsSnapshot(name, cwd, "routine-run", err);
    } else if (triage) {
        triage->AddCard(BuildRoutineStaleFailureCard(
            name, runId, diff, RoutineFailedNodes(btm, tqm, runId), GetCurrentTimestampStr()));
    }
}

// --------------------------------------------------------------------------------------------
// 093-5: routine ask. A worker LLM writes a routine definition; Hancho validates it and stores
// it as a proposal. All of this runs synchronously so `--wait` always returns the final result.
// --------------------------------------------------------------------------------------------

static std::string AskReplaceAll(std::string text, const std::string& from, const std::string& to) {
    if (from.empty()) return text;
    size_t pos = 0;
    while ((pos = text.find(from, pos)) != std::string::npos) {
        text.replace(pos, from.size(), to);
        pos += to.size();
    }
    return text;
}

static std::string AskGlobField(const std::string& rel, const std::string& field) {
    if (field == "stem") return BtPathStem(rel);
    if (field == "name" || field == "filename") return BtPathName(rel);
    if (field == "base") {
        const std::string name = BtPathName(rel);
        const size_t dot = name.find_last_of('.');
        return dot == std::string::npos ? name : name.substr(0, dot);
    }
    return rel;
}

// Expands ${foreach:<glob>:<field>:<separator>} placeholders used by the test-suite template.
static std::string AskExpandForeachTokens(const std::string& text, const std::string& cwd) {
    std::string out = text;
    const std::string prefix = "${foreach:";
    size_t pos = 0;
    while ((pos = out.find(prefix, pos)) != std::string::npos) {
        const size_t end = out.find('}', pos);
        if (end == std::string::npos) break;
        const std::string inner = out.substr(pos + prefix.size(), end - pos - prefix.size());
        const size_t first = inner.find(':');
        const std::string glob = first == std::string::npos ? inner : inner.substr(0, first);
        std::string field = "path";
        std::string separator = " ";
        if (first != std::string::npos) {
            const std::string rest = inner.substr(first + 1);
            const size_t second = rest.find(':');
            field = second == std::string::npos ? rest : rest.substr(0, second);
            if (second != std::string::npos) separator = rest.substr(second + 1);
        }
        std::vector<std::string> matches;
        ExpandBtGlob(cwd, glob, {}, matches);
        std::string joined;
        for (size_t i = 0; i < matches.size(); ++i) {
            if (i) joined += separator;
            joined += AskGlobField(matches[i], field);
        }
        out.replace(pos, end - pos + 1, joined);
        pos += joined.size();
    }
    return out;
}

static std::string AskReplaceVarTokens(std::string text, const std::string& var,
                                       const std::string& rel) {
    const std::string prefix = "${" + var + ".";
    size_t pos = 0;
    while ((pos = text.find(prefix, pos)) != std::string::npos) {
        const size_t end = text.find('}', pos);
        if (end == std::string::npos) break;
        const std::string field = text.substr(pos + prefix.size(), end - pos - prefix.size());
        const std::string value = AskGlobField(rel, field);
        text.replace(pos, end - pos + 1, value);
        pos += value.size();
    }
    return text;
}

static json AskExpandJson(const json& node, const std::string& var, const std::string& rel) {
    if (node.is_array()) {
        json out = json::array();
        for (const auto& child : node) out.push_back(AskExpandJson(child, var, rel));
        return out;
    }
    if (node.is_object()) {
        json out = json::object();
        for (auto it = node.begin(); it != node.end(); ++it) {
            if (it.value().is_string()) {
                out[it.key()] = AskReplaceVarTokens(AskExpandForeachTokens(it.value().get<std::string>(), ""), var, rel);
            } else {
                out[it.key()] = AskExpandJson(it.value(), var, rel);
            }
        }
        return out;
    }
    return node;
}

// Dry-run: expand foreach globs into the concrete list of jobs that would run, and reject
// definitions that contain unattended-hostile nodes or chained commands.
static json AskDryRun(const json& bt, const std::string& cwd, std::string& outError) {
    json jobs = json::array();
    std::function<void(const json&, const std::string&)> walk = [&](const json& node,
                                                                    const std::string& key) {
        if (!outError.empty()) return;
        if (node.is_array()) {
            int index = 0;
            for (const auto& child : node) walk(child, key + "/" + std::to_string(index++));
            return;
        }
        if (!node.is_object()) return;
        const std::string type = node.value("type", std::string());
        if (type == "worker" || type == "escalate") {
            outError = "routine must not contain '" + type +
                       "' nodes (it has to run unattended without a worker or escalation)";
            return;
        }
        if (type == "job") {
            std::string command = AskExpandForeachTokens(node.value("command", std::string()), cwd);
            std::string chainError;
            if (!CheckChainGuard(command, chainError)) {
                outError = "ChainGuard: " + chainError;
                return;
            }
            json row;
            row["node"] = node.value("id", key);
            row["command"] = command;
            row["summary"] = node.value("summary", std::string());
            jobs.push_back(std::move(row));
        } else if (type == "foreach") {
            std::vector<std::string> excludes;
            if (node.contains("exclude") && node["exclude"].is_array()) {
                for (const auto& item : node["exclude"]) {
                    if (item.is_string()) excludes.push_back(item.get<std::string>());
                }
            }
            std::vector<std::string> matches;
            ExpandBtGlob(cwd, node.value("glob", std::string()), excludes, matches);
            const std::string var = node.value("as", std::string("item"));
            for (const auto& rel : matches) {
                walk(AskExpandJson(node.value("do", json::object()), var, rel), key + "[" + rel + "]");
            }
        }
        for (const char* childKey : {"root", "children", "cond", "then", "else", "child", "until", "handlers"}) {
            auto it = node.find(childKey);
            if (it != node.end()) walk(*it, key);
        }
    };
    walk(bt.value("root", json::object()), "");

    json out = json::object();
    out["jobs"] = std::move(jobs);
    out["count"] = out["jobs"].size();
    out["expanded"] = true;
    return out;
}

static json AskSelectWorkers(BtManager* btm, bool& outUsedDefault) {
    json candidates = json::array();
    llm::Config cfg;
    std::string err;
    if (llm::Load(llm::DefaultPath(), cfg, err)) {
        for (const auto& entry : llm::Usable(cfg, llm::kAgent)) {
            if (btm && btm->IsDown(entry.engine, entry.model)) continue;
            json candidate;
            candidate["engine"] = entry.engine;
            candidate["model"] = entry.model;
            candidate["rank"] = entry.rank;
            candidates.push_back(std::move(candidate));
        }
    }
    outUsedDefault = candidates.empty();
    if (outUsedDefault) {
        json candidate;
        candidate["engine"] = btm ? btm->DefaultEngine() : std::string("agy");
        candidate["model"] = "";
        candidate["rank"] = 0;
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

struct AskWorkerResult {
    bool ok = false;
    json value = json::object();
    std::string error;
    std::string engine;
    std::string model;
    bool usedDefault = false;
};

static AskWorkerResult AskRunWorker(BtManager* btm, const std::string& cwd,
                                    const std::string& prompt, const json& candidates,
                                    bool usedDefault, int timeoutMs) {
    AskWorkerResult result;
    if (!btm) {
        result.error = "behavior trees are not available";
        return result;
    }
    result.usedDefault = usedDefault;
    result.engine = candidates.empty() ? std::string() : candidates[0].value("engine", std::string());
    result.model = candidates.empty() ? std::string() : candidates[0].value("model", std::string());

    json schema = {
        {"type", "object"},
        {"required", json::array({"name", "summary", "bt", "inputs", "notes", "rationale"})},
        {"properties", {
            {"name", {{"type", "string"}}},
            {"summary", {{"type", "string"}}},
            {"bt", {{"type", "object"}}},
            {"inputs", {{"type", "array"}, {"items", {{"type", "string"}}}}},
            {"notes", {{"type", "string"}}},
            {"rationale", {{"type", "string"}}}
        }}
    };

    static std::atomic<int> sequence{0};
    const std::string stamp = SanitizeFileNameComponent(GetCurrentTimestampStr());
    const std::string btId = "routine-ask-" + stamp + "-" + std::to_string(++sequence);
    json bt = {
        {"id", btId},
        {"summary", "Generate a Hancho routine proposal"},
        {"cwd", cwd},
        {"taskqueue", "routine-ask"},
        {"vars", {{"dollar", "$"}}},
        {"root", {
            {"type", "worker"},
            {"id", "ask"},
            {"summary", "Ask an LLM for a routine definition"},
            {"prompt", prompt},
            {"timeoutSec", 900},
            {"outputSchema", schema}
        }}
    };
    if (usedDefault) {
        bt["root"]["engine"] = result.engine;
        bt["root"]["model"] = result.model;
    } else {
        bt["root"]["engines"] = candidates;
    }

    json start = json::parse(btm->Start(bt.dump(), false), nullptr, false);
    if (start.is_discarded() || !start.value("ok", false)) {
        result.error = start.is_discarded() ? "failed to start routine ask worker"
                                            : start.value("error", start.dump());
        return result;
    }
    json waited = json::parse(btm->Wait(btId, 0, timeoutMs, false), nullptr, false);
    if (waited.is_discarded()) {
        result.error = "routine ask worker did not return a state";
        return result;
    }
    const std::string state = waited.value("state", std::string());
    if (waited.value("timedOut", false) || (state != "succeeded" && state != "failed" && state != "cancelled")) {
        result.error = "routine ask worker did not finish in time (state=" + state + ")";
        return result;
    }
    if (state != "succeeded") {
        std::string detail = waited.value("detail", std::string());
        json tree = json::parse(btm->Get(btId, false), nullptr, false);
        if (!tree.is_discarded() && tree.contains("tree")) {
            std::function<void(const json&)> findDetail = [&](const json& node) {
                if (node.is_array()) { for (const auto& c : node) findDetail(c); return; }
                if (!node.is_object()) return;
                if (node.contains("status") && node["status"].is_object() &&
                    node.value("id", std::string()) == "ask") {
                    const std::string d = node["status"].value("detail", std::string());
                    if (!d.empty()) detail = d;
                }
                for (auto it = node.begin(); it != node.end(); ++it) findDetail(it.value());
            };
            findDetail(tree["tree"]);
        }
        result.error = "routine ask worker failed: " + (detail.empty() ? state : detail);
        return result;
    }

    const std::wstring resultPath = BtManager::GetBtResultFileWide(btId, btId + "-ask-1");
    if (!PathExistsWide(resultPath)) {
        result.error = "routine ask worker produced no result file";
        return result;
    }
    json value = json::parse(ReadFileUtf8Wide(resultPath), nullptr, false);
    if (value.is_discarded() || !value.is_object()) {
        result.error = "routine ask worker result is not a JSON object";
        return result;
    }
    result.ok = true;
    result.value = std::move(value);
    return result;
}

static std::string AskBuildPrompt(const std::string& requestText, const std::string& cwd,
                                  const std::string& name, bool fix,
                                  const json& existing, const json& diff,
                                  const json& lastRun, const json& nodes,
                                  const std::string& logTail, const std::string& extraError) {
    std::ostringstream oss;
    oss << "You are helping the Hancho supervisor create or update a saved routine.\n\n"
        << "Request from the user:\n" << requestText << "\n\n"
        << "Working directory (cwd): " << cwd << "\n"
        << "Routine name: " << (name.empty() ? std::string("(choose a short kebab-case name)") : name) << "\n"
        << "Mode: " << (fix ? "FIX an existing routine" : "CREATE a new routine") << "\n\n";
    if (!extraError.empty()) {
        oss << "A previous attempt was rejected. Fix these problems:\n" << extraError << "\n\n";
    }
    oss << "A routine is a JSON file with these fields: name, summary, cwd, bt, inputs, notes.\n"
        << "`bt` is the behavior-tree definition: {id, summary, root:{type, children:[...]}}.\n"
        << "Use `job` nodes with a single command and `foreach` nodes with a glob/`do` body.\n"
        << "In commands, ${foreach:<glob>:<field>:<separator>} expands to every matching file;\n"
        << "inside a foreach body, ${<as>.stem} / ${<as>.name} refer to the current match.\n"
        << "Prefer a foreach node with globs over a fixed list of files.\n"
        << "Do not use `worker` or `escalate` nodes: a routine must run unattended.\n"
        << "Do not chain commands (&&, ||, ;, newline); use separate job nodes instead.\n\n";
    oss << "Reference example (`bt template test-suite`):\n"
        << BtTemplateDefinition("test-suite").dump(2) << "\n\n";
    oss << "Existing routines in this project:\n";
    oss << "[routine names and summaries are supplied by Hancho]\n\n";
    if (fix) {
        oss << "Current definition:\n" << existing.dump(2) << "\n\n"
            << "routine check diff:\n" << diff.dump(2) << "\n\n"
            << "Last run:\n" << lastRun.dump(2) << "\n\n"
            << "Per-node results:\n" << nodes.dump(2) << "\n\n"
            << "Failed node log tail:\n" << logTail << "\n\n";
    }
    oss << "Read the repository under cwd to confirm the real build and test commands.\n"
        << "Write ONLY a JSON object with name, summary, bt, inputs, notes and rationale to the\n"
        << "file named by the PC_BT_RESULT_FILE environment variable. `rationale` explains your\n"
        << "choice to the person who will approve the proposal.\n";
    // The BT engine expands ${...} in a worker prompt; keep the literal foreach syntax intact
    // by spelling the dollar sign through a variable that is set to "$" on the ask tree.
    return AskReplaceAll(oss.str(), "${", "${dollar}{");
}

// Main entry point shared by the IPC action and the triage `update_routine` candidate.
static std::string HandleRoutineAsk(RoutineManager& routines, BtManager* btm,
                                    TaskQueueManager* tqm, const std::string& requestText,
                                    const std::string& requestedName, const std::string& cwd,
                                    const std::string& scope, const std::string& by,
                                    int timeoutMs) {
    using json = nlohmann::json;
    const std::string name = requestedName.empty() ? std::string("ask-") + SanitizeFileNameComponent(
                                  requestText.substr(0, 40)) : requestedName;
    if (!RoutineManager::ValidName(name)) {
        return json{{"ok", false}, {"error", "invalid routine name: " + name}}.dump();
    }

    bool fix = false;
    json existing = json::object();
    json diff = json::object();
    json lastRun = json::object();
    json nodes = json::array();
    std::string logTail;
    if (RoutineManager::HasProposal(name, cwd)) {
        json proposal = RoutineManager::ReadProposal(name, cwd);
        if (!proposal.is_null()) {
            fix = true;
            existing = proposal;
            diff = proposal.value("proposal", json::object()).value("diff", json::object());
        }
    } else {
        std::string foundScope;
        bool found = false;
        const std::wstring defPath = RoutineManager::DefinitionPathWide(name, "", cwd, &foundScope, &found);
        if (found) {
            fix = true;
            existing = json::parse(ReadFileUtf8Wide(defPath), nullptr, false);
            json check = json::parse(routines.Check(name, cwd), nullptr, false);
            if (!check.is_discarded() && check.value("ok", false)) diff = check;
        }
    }
    if (fix && existing.is_object()) {
        json shown = json::parse(routines.Show(name, cwd), nullptr, false);
        if (!shown.is_discarded() && shown.value("ok", false)) {
            if (shown.contains("lastRun") && shown["lastRun"].is_object()) lastRun = shown["lastRun"];
            if (shown.contains("nodes") && shown["nodes"].is_array()) nodes = shown["nodes"];
        }
        if (tqm && nodes.is_array()) {
            for (const auto& node : nodes) {
                if (!node.is_object()) continue;
                const std::string state = node.value("state", std::string());
                if (state == "succeeded" || state == "skipped") continue;
                if (!node.contains("jobs") || !node["jobs"].is_array()) continue;
                for (const auto& job : node["jobs"]) {
                    if (!job.is_object()) continue;
                    TaskQueueJob snapshot;
                    if (!tqm->GetJobSnapshot(job.value("id", std::string()), snapshot)) continue;
                    if (snapshot.attempts.empty()) continue;
                    const std::wstring log = Utf8ToWide(snapshot.attempts.back().lastLinesFile);
                    if (!log.empty() && PathExistsWide(log)) {
                        logTail = ReadFileUtf8Wide(log);
                        if (logTail.size() > 4000) logTail = logTail.substr(logTail.size() - 4000);
                        break;
                    }
                }
                if (!logTail.empty()) break;
            }
        }
    }

    bool usedDefault = false;
    const json candidates = AskSelectWorkers(btm, usedDefault);
    std::string extraError;
    AskWorkerResult worker;
    json resultValue;
    json dryRun = json::object();
    std::string validationError;
    int usedAttempts = 0;

    for (int attempt = 0; attempt < 2; ++attempt) {
        usedAttempts = attempt + 1;
        const std::string prompt = AskBuildPrompt(requestText, cwd, name, fix, existing, diff,
                                                  lastRun, nodes, logTail, extraError);
        worker = AskRunWorker(btm, cwd, prompt, candidates, usedDefault, timeoutMs);
        if (!worker.ok) {
            return json{{"ok", false}, {"error", worker.error},
                        {"engine", worker.engine}, {"model", worker.model}}.dump();
        }
        resultValue = worker.value;
        validationError.clear();
        json bt = json::parse(resultValue.value("bt", json::object()).dump(), nullptr, false);
        std::string btJson = bt.is_discarded() ? std::string() : bt.dump();
        if (btJson.empty() || !bt.is_object() || !bt.contains("root")) {
            validationError = "the proposed 'bt' must be an object with a root node";
        } else {
            if (btm) {
                json validated = json::parse(btm->Validate(btJson, true), nullptr, false);
                if (validated.is_discarded() || !validated.value("ok", false) ||
                    !validated.value("valid", false)) {
                    validationError = validated.is_discarded()
                                          ? "strict BT validation failed"
                                          : validated.value("error", validated.dump());
                }
            }
            if (validationError.empty()) {
                dryRun = AskDryRun(bt, cwd, validationError);
            }
        }
        if (validationError.empty()) break;
        if (attempt == 0) {
            extraError = validationError + "\nWrite a corrected definition now.";
            continue;
        }
        return json{{"ok", false},
                    {"error", "proposed routine failed validation after one fix: " + validationError},
                    {"engine", worker.engine}, {"model", worker.model},
                    {"dryRun", dryRun}}.dump();
    }

    const std::string resultName = resultValue.value("name", name);
    if (!RoutineManager::ValidName(resultName)) {
        return json{{"ok", false}, {"error", "worker returned an invalid routine name: " + resultName}}.dump();
    }
    std::vector<std::string> inputs;
    if (resultValue.contains("inputs") && resultValue["inputs"].is_array()) {
        for (const auto& input : resultValue["inputs"]) {
            if (input.is_string()) inputs.push_back(input.get<std::string>());
        }
    }
    const std::string effectiveScope = scope.empty() ? std::string("project") : scope;
    json meta = {
        {"request", requestText},
        {"rationale", resultValue.value("rationale", std::string())},
        {"engine", worker.engine},
        {"model", worker.model},
        {"usedDefaultEngine", usedDefault},
        {"diff", diff},
        {"dryRun", dryRun},
        {"at", GetCurrentTimestampStr()},
        {"by", by}
    };
    json saved = json::parse(routines.SaveProposal(
        resultName, resultValue.value("bt", json::object()).dump(), inputs,
        resultValue.value("summary", std::string()), resultValue.value("notes", std::string()),
        effectiveScope, cwd, by,
        "routine ask" + std::string(fix ? " (fix)" : " (create)") + ": " + requestText, meta),
        nullptr, false);
    if (saved.is_discarded() || !saved.value("ok", false)) {
        return saved.is_discarded() ? json{{"ok", false}, {"error", "failed to save proposal"}}.dump()
                                    : saved.dump();
    }

    json out = json{{"ok", true}};
    out["name"] = resultName;
    out["status"] = "proposed";
    out["scope"] = effectiveScope;
    out["engine"] = worker.engine;
    out["model"] = worker.model;
    out["usedDefaultEngine"] = usedDefault;
    out["rationale"] = resultValue.value("rationale", std::string());
    out["summary"] = resultValue.value("summary", std::string());
    out["dryRun"] = dryRun;
    out["attempts"] = usedAttempts;
    out["proposal"] = saved;
    if (usedDefault) {
        out["warning"] = "agent LLM list is empty; used the default engine";
    }
    return out.dump();
}

std::string IpcServer::HandleRequestJson(const std::string& reqJson) {
    std::string action = JsonGetString(reqJson, "action", "");
    if (action != "ping" && action != "term_sessions") {
        WriteAuditLog("IPC_REQ", RedactIpcToken(reqJson));
    }
    std::ostringstream oss;

    if (action == "ping") {
        return "{\"ok\":true,\"status\":\"running\",\"pid\":" +
               std::to_string(GetCurrentProcessId()) + ",\"headless\":" +
               std::string(headless_ ? "true" : "false") + "}";
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
    else if (action == "term_remote_endpoint") {
        // Used by wct on another PC: the caller already knows the reachable IPC host,
        // so only the WCSP port and token are returned. The terminal server must have
        // been started with terminal.remote enabled.
        if (!termServer_ || !termServer_->IsRunning() || termServer_->Port() == 0) {
            return "{\"ok\":false,\"error\":\"terminal server is not running\"}";
        }
        if (!termServer_->RemoteEnabled()) {
            return "{\"ok\":false,\"error\":\"remote terminal connections are disabled (set terminal.remote to true)\"}";
        }
        nlohmann::json out;
        out["ok"] = true;
        out["port"] = termServer_->Port();
        out["token"] = termServer_->Token();
        return out.dump();
    }
    else if (action == "term_sessions") {
        if (!termServer_ || !termServer_->IsRunning()) {
            return "{\"ok\":true,\"sessions\":[]}";
        }
        auto sessions = termServer_->ListSessions();
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& s : sessions) {
            nlohmann::json item;
            item["id"] = s.id;
            item["job_id"] = s.jobId;
            item["remote"] = s.remoteAddr;
            item["target"] = s.target;
            item["connected_sec"] = s.connectedSec;
            arr.push_back(std::move(item));
        }
        nlohmann::json out;
        out["ok"] = true;
        out["sessions"] = arr;
        return out.dump();
    }
    else if (action == "term_kill_session") {
        if (!termServer_) {
            return "{\"ok\":false,\"error\":\"terminal server is not running\"}";
        }
        uint32_t sid = static_cast<uint32_t>(JsonGetInt(reqJson, "id", 0));
        bool killed = termServer_->KillSession(sid);
        nlohmann::json out;
        out["ok"] = killed;
        if (!killed) out["error"] = "session not found";
        return out.dump();
    }
    else if (action == "term_set_token") {
        if (!termServer_) {
            return "{\"ok\":false,\"error\":\"terminal server is not running\"}";
        }
        std::string token = JsonGetString(reqJson, "token", "");
        termServer_->SetToken(token);
        return "{\"ok\":true}";
    }
    else if (action == "triage_list") {
        return triageManager_ ? triageManager_->List(JsonGetString(reqJson, "state", "pending"))
                              : "{\"ok\":false,\"error\":\"triage manager is unavailable\"}";
    }
    else if (action == "triage_get") {
        return triageManager_ ? triageManager_->Get(JsonGetString(reqJson, "id", ""))
                              : "{\"ok\":false,\"error\":\"triage manager is unavailable\"}";
    }
    else if (action == "permit_draft") {
        return permitDraft_ ? permitDraft_->Start(JsonGetString(reqJson, "request", ""))
                            : "{\"ok\":false,\"error\":\"permit draft is unavailable\"}";
    }
    else if (action == "permit_draft_get") {
        return permitDraft_ ? permitDraft_->Get(JsonGetString(reqJson, "draftId", ""))
                            : "{\"ok\":false,\"error\":\"permit draft is unavailable\"}";
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
    else if (action == "translate_get" || action == "translate_status" || action == "translate_clear") {
        TranslationStore* store = ActiveTranslationStore();
        if (!store) return "{\"ok\":false,\"error\":\"translation store is unavailable\"}";
        if (action == "translate_status") return store->Status();
        if (action == "translate_clear") return store->Clear(JsonGetString(reqJson, "lang", ""));
        nlohmann::json request = nlohmann::json::parse(reqJson, nullptr, false);
        if (!request.is_object()) return "{\"ok\":false,\"error\":\"invalid JSON request\"}";
        std::vector<std::string> texts;
        auto textsIt = request.find("texts");
        if (textsIt != request.end() && textsIt->is_array()) {
            for (const auto& v : *textsIt) if (v.is_string()) texts.push_back(v.get<std::string>());
        }
        if (request.contains("text") && request["text"].is_string()) texts.push_back(request["text"].get<std::string>());
        const std::string lang = request.value("lang", std::string());
        if (lang.empty()) return "{\"ok\":false,\"error\":\"translate_get requires lang (ja or es)\"}";
        return store->Get(texts, lang, request.value("field", std::string()));
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
        const bool cancelJobs = JsonGetBool(reqJson, "cancel_jobs", JsonGetBool(reqJson, "cancelJobs", false));
        std::function<void(bool)> stopCb;
        {
            std::lock_guard<std::mutex> lock(cbMutex_);
            stopCb = shutdownCb_;
        }
        if (!stopCb) {
            return "{\"ok\":false,\"error\":\"shutdown is not available\"}";
        }
        // Answer the client first, then let the host tear the pipe down.
        std::thread([stopCb, cancelJobs] {
            SleepMs(200);
            stopCb(cancelJobs);
        }).detach();
        return "{\"ok\":true,\"stopping\":true}";
    }
    else if (action == "power_suspend" || action == "power_shutdown") {
        const bool shutdown = action == "power_shutdown";
        const std::string powerAction = shutdown ? "shutdown"
            : (JsonGetBool(reqJson, "hibernate", false) ? "hibernate" : "sleep");
        const std::string jobId = JsonGetString(reqJson, "job_id", JsonGetString(reqJson, "jobId", ""));
        const bool dryRun = JsonGetBool(reqJson, "dry_run", JsonGetBool(reqJson, "dryRun", false));
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
                out["action"] = powerAction;
                out["hibernate"] = powerAction == "hibernate";
                out["shutdown"] = shutdown;
                return out.dump();
            }
            std::string err;
            return tqm_.SetJobPowerWhenDone(jobId, powerAction, delaySec, err);
        }

        if (dryRun) {
            nlohmann::json out;
            out["ok"] = true;
            out["dry_run"] = true;
            out["suspending"] = false;
            out["shutting_down"] = false;
            out["delay_sec"] = delaySec;
            out["action"] = powerAction;
            out["hibernate"] = powerAction == "hibernate";
            out["shutdown"] = shutdown;
            return out.dump();
        }

        if (delaySec < 1) delaySec = 1;
        PowerCallback cb;
        {
            std::lock_guard<std::mutex> lock(cbMutex_);
            cb = powerCb_;
        }
        if (!cb) {
            return std::string("{\"ok\":false,\"error\":\"") + (shutdown ? "shutdown" : "suspend") +
                   " is not available\"}";
        }
        WriteAuditLog(shutdown ? "POWER_SHUTDOWN" : "POWER_SUSPEND",
                      std::string("immediate delay=") + std::to_string(delaySec) + " action=" + powerAction);
        std::thread([cb, delaySec, powerAction] {
            if (delaySec > 0) SleepMs(static_cast<uint32_t>(delaySec) * 1000);
            std::string err;
            if (!cb(powerAction, err)) WriteAuditLog("POWER_FAILED", err);
        }).detach();
        nlohmann::json out;
        out["ok"] = true;
        out["suspending"] = !shutdown;
        out["shutting_down"] = shutdown;
        out["delay_sec"] = delaySec;
        out["action"] = powerAction;
        out["hibernate"] = powerAction == "hibernate";
        out["shutdown"] = shutdown;
        return out.dump();
    }
    else if (action == "power_cancel") {
        std::string err;
        return tqm_.ClearPowerWhenDone(err);
    }
    else if (action == "power_status") {
        nlohmann::json out = nlohmann::json::parse(tqm_.PowerStatusJson(), nullptr, false);
        if (!out.is_object()) out = nlohmann::json::object();
        std::string privilegeError;
        const bool canPower = pc::HasPowerPrivilege(privilegeError);
        out["can_power"] = canPower;
        out["power_privilege_error"] = privilegeError;
        return out.dump();
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
        req.idleFirstEventSec = static_cast<int>(JsonGetInt(reqJson, "idle_first_event_sec", 0));
        req.estimatedEndTimeStr = JsonGetString(reqJson, "estimate_end", "");
        req.agentType = JsonGetString(reqJson, "engine", "");
        if (req.agentType.empty()) req.agentType = JsonGetString(reqJson, "agent_type", "");
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
                << ",\"engine\":\"" << JsonEscape(snap.agentType) << "\""
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
            oss << "{\"ok\":true,\"id\":" << id
                << ",\"engine\":\"" << JsonEscape(state.agentType) << "\""
                << ",\"agent_type\":\"" << JsonEscape(state.agentType)
                << "\",\"status\":\"" << JsonEscape(state.status)
                << "\",\"summary\":\"" << JsonEscape(summary) << "\"}";
            return oss.str();
        }

        std::string jsonEvents = LlmEventsToJson(events, state);
        return jsonEvents;
    }
    else if (action == "get_usage_history") {
        int sinceSec = static_cast<int>(JsonGetInt(reqJson, "since_sec", 86400));
        int64_t since = GetNowEpochSec() - sinceSec;
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
                st.activity = snap.activity;
                st.activityTarget = snap.activityTarget;
                st.activitySinceEpochMs = snap.activitySinceEpochMs;
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
            << ",\"engine\":\"" << JsonEscape(st.agentType) << "\""
            << ",\"agent_type\":\"" << JsonEscape(st.agentType) << "\""
            << ",\"status\":\"" << JsonEscape(st.status) << "\""
            << ",\"current_tool\":\"" << JsonEscape(st.currentTool) << "\""
            << ",\"current_tool_action\":\"" << JsonEscape(st.currentToolAction) << "\""
            << ",\"last_thought\":\"" << JsonEscape(st.lastThought) << "\""
            << ",\"last_message\":\"" << JsonEscape(st.lastMessage) << "\""
            << ",\"last_error\":\"" << JsonEscape(st.lastError) << "\""
            << ",\"stop_reason\":\"" << JsonEscape(st.stopReason) << "\""
            << ",\"model\":\"" << JsonEscape(st.model) << "\""
            << ",\"activity\":\"" << JsonEscape(st.activity) << "\""
            << ",\"activity_target\":\"" << JsonEscape(st.activityTarget) << "\""
            << ",\"activity_since_epoch_ms\":" << st.activitySinceEpochMs
            << ",\"permission_denials\":" << st.permissionDenials
            << ",\"tool_errors\":" << st.toolErrors
            << ",\"step_count\":" << st.stepCount
            << ",\"event_count\":" << st.eventCount
            << ",\"total_tokens\":" << st.totalTokens
            << ",\"cost_usd\":" << st.costUsd
            << ",\"stream_file\":\"" << JsonEscape(st.streamFilePath) << "\""
            << ",\"supervisor_notes\":\"" << JsonEscape(st.supervisorNotes) << "\""
            << ",\"supervisor_goal\":\"" << JsonEscape(st.supervisorGoal) << "\"";
        if (!st.rateLimitType.empty()) {
            oss << ",\"rate_limit\":{\"type\":\"" << JsonEscape(st.rateLimitType)
                << "\",\"status\":\"" << JsonEscape(st.rateLimitStatus)
                << "\",\"utilization\":" << st.rateLimitUtilization
                << ",\"resets_at_epoch_sec\":" << st.rateLimitResetsAtSec << "}";
        }
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
    else if (action.rfind("task_", 0) == 0) {
        nlohmann::json req = nlohmann::json::parse(reqJson, nullptr, false);
        if (!req.is_object()) req = nlohmann::json::object();
        std::string err;
        std::string resp;
        if (action == "task_create") {
            std::string cardDefaultsJson;
            if (req.contains("cardDefaults") && !req["cardDefaults"].is_null()) {
                cardDefaultsJson = req["cardDefaults"].is_string()
                    ? req["cardDefaults"].get<std::string>()
                    : req["cardDefaults"].dump();
            }
            resp = tqm_.TaskCreate(JsonGetString(reqJson, "title", ""),
                                   JsonGetString(reqJson, "intent", ""),
                                   JsonGetString(reqJson, "id", ""),
                                   ExtractRawJsonValue(reqJson, "origin"), err,
                                   cardDefaultsJson);
        } else if (action == "task_update") {
            std::string session = JsonGetString(reqJson, "session", "");
            if (session.empty() && req.contains("origin") && req["origin"].is_object()) {
                session = req["origin"].value("session", std::string());
            }
            std::string cardDefaultsJson;
            const bool hasCardDefaults = req.contains("cardDefaults");
            if (hasCardDefaults && !req["cardDefaults"].is_null()) {
                cardDefaultsJson = req["cardDefaults"].is_string()
                    ? req["cardDefaults"].get<std::string>()
                    : req["cardDefaults"].dump();
            }
            resp = tqm_.TaskUpdate(JsonGetString(reqJson, "id", ""),
                                   req.contains("title") && req["title"].is_string(),
                                   req.value("title", std::string()),
                                   req.contains("intent") && req["intent"].is_string(),
                                   req.value("intent", std::string()),
                                   hasCardDefaults, cardDefaultsJson, session, err);
        } else if (action == "task_show") {
            const int64_t logLimit = req.contains("logLimit") ? req.value("logLimit", int64_t(200))
                                                              : req.value("log_limit", int64_t(200));
            resp = tqm_.TaskShow(JsonGetString(reqJson, "id", ""), logLimit, err);
        } else if (action == "task_list") {
            const int64_t sinceVer = req.contains("since_version") ? req.value("since_version", int64_t(-1))
                                                                 : req.value("sinceVersion", int64_t(-1));
            int64_t finishedLimit = -1;
            if (req.contains("finished_limit") && req["finished_limit"].is_number_integer()) {
                finishedLimit = req["finished_limit"].get<int64_t>();
            } else if (req.contains("finishedLimit") && req["finishedLimit"].is_number_integer()) {
                finishedLimit = req["finishedLimit"].get<int64_t>();
            }
            resp = tqm_.TaskList(JsonGetString(reqJson, "state", ""),
                                 req.value("limit", int64_t(0)), err, sinceVer,
                                 finishedLimit,
                                 JsonGetString(reqJson, "before", ""),
                                 JsonGetString(reqJson, "query", ""));
        } else if (action == "task_note") {
            std::string session = JsonGetString(reqJson, "session", "");
            if (session.empty() && req.contains("origin") && req["origin"].is_object()) {
                session = req["origin"].value("session", std::string());
            }
            resp = tqm_.TaskNote(JsonGetString(reqJson, "id", ""),
                                 JsonGetString(reqJson, "text", ""),
                                 session, err);
        } else if (action == "task_close") {
            resp = tqm_.TaskClose(JsonGetString(reqJson, "id", ""),
                                  JsonGetString(reqJson, "result", ""),
                                  JsonGetString(reqJson, "reason", ""), err);
        } else if (action == "task_reopen") {
            resp = tqm_.TaskReopen(JsonGetString(reqJson, "id", ""),
                                   JsonGetString(reqJson, "reason", ""), err);
        } else if (action == "task_purge") {
            const int64_t olderThanSec = req.contains("older_than_sec")
                ? req.value("older_than_sec", int64_t(-1))
                : req.value("olderThanSec", int64_t(-1));
            resp = tqm_.TaskPurgeOlderThan(olderThanSec, err);
        } else if (action == "task_wait") {
            const int timeoutMs = req.contains("timeout_ms") ? req.value("timeout_ms", 30000)
                                                             : req.value("timeoutMs", 30000);
            const int64_t logLimit = req.contains("logLimit") ? req.value("logLimit", int64_t(200))
                                                              : req.value("log_limit", int64_t(200));
            resp = tqm_.TaskWait(JsonGetString(reqJson, "id", ""), timeoutMs, logLimit, err);
        } else {
            return "{\"ok\":false,\"error\":\"unknown task action: " + JsonEscape(action) + "\"}";
        }
        return resp;
    }
    else if (action.rfind("direction_", 0) == 0) {
        nlohmann::json req = nlohmann::json::parse(reqJson, nullptr, false);
        if (!req.is_object()) req = nlohmann::json::object();
        std::string err;
        std::string resp;
        if (action == "direction_show") {
            resp = tqm_.DirectionShow(JsonGetString(reqJson, "id", ""), err);
        } else if (action == "direction_list") {
            const int64_t sinceVer = req.contains("since_version") ? req.value("since_version", int64_t(-1))
                                                                 : req.value("sinceVersion", int64_t(-1));
            std::string taskId = JsonGetString(reqJson, "taskId", "");
            if (taskId.empty()) taskId = JsonGetString(reqJson, "task_id", "");
            if (taskId.empty()) taskId = JsonGetString(reqJson, "task", "");
            resp = tqm_.DirectionList(taskId, JsonGetString(reqJson, "state", ""),
                                      req.value("limit", int64_t(0)), err, sinceVer);
        } else if (action == "direction_close") {
            resp = tqm_.DirectionClose(JsonGetString(reqJson, "id", ""),
                                       JsonGetString(reqJson, "result", ""),
                                       JsonGetString(reqJson, "reason", ""), err);
        } else if (action == "direction_reopen") {
            resp = tqm_.DirectionReopen(JsonGetString(reqJson, "id", ""),
                                        JsonGetString(reqJson, "reason", ""), err);
        } else if (action == "direction_purge") {
            const int64_t olderThanSec = req.contains("older_than_sec")
                ? req.value("older_than_sec", int64_t(-1))
                : req.value("olderThanSec", int64_t(-1));
            resp = tqm_.DirectionPurgeOlderThan(olderThanSec, err);
        } else {
            return "{\"ok\":false,\"error\":\"unknown direction action: " + JsonEscape(action) + "\"}";
        }
        return resp;
    }
    else if (action.rfind("card_", 0) == 0) {
        nlohmann::json req = nlohmann::json::parse(reqJson, nullptr, false);
        if (!req.is_object()) req = nlohmann::json::object();
        auto getTaskId = [&]() -> std::string {
            std::string t = JsonGetString(reqJson, "taskId", "");
            if (t.empty()) t = JsonGetString(reqJson, "task_id", "");
            if (t.empty()) t = JsonGetString(reqJson, "task", "");
            return t;
        };
        auto parseAfter = [&]() -> std::vector<std::string> {
            std::vector<std::string> out;
            if (!req.contains("after")) return out;
            if (req["after"].is_array()) {
                for (const auto& item : req["after"]) {
                    if (item.is_string() && !item.get<std::string>().empty()) {
                        out.push_back(item.get<std::string>());
                    }
                }
            } else if (req["after"].is_string()) {
                std::istringstream iss(req["after"].get<std::string>());
                std::string tok;
                while (std::getline(iss, tok, ',')) {
                    const size_t s = tok.find_first_not_of(" \t\r\n");
                    if (s == std::string::npos) continue;
                    const size_t e = tok.find_last_not_of(" \t\r\n");
                    out.push_back(tok.substr(s, e - s + 1));
                }
            }
            return out;
        };
        auto getParamsJson = [&]() -> std::string {
            if (!req.contains("params") || req["params"].is_null()) return "";
            if (req["params"].is_string()) return req["params"].get<std::string>();
            return req["params"].dump();
        };
        auto getSession = [&]() -> std::string {
            std::string session = JsonGetString(reqJson, "session", "");
            if (session.empty() && req.contains("origin") && req["origin"].is_object()) {
                session = req["origin"].value("session", std::string());
            }
            return session;
        };
        std::string err;
        std::string resp;
        if (action == "card_add") {
            resp = tqm_.CardAdd(getTaskId(),
                                JsonGetString(reqJson, "number", ""),
                                JsonGetString(reqJson, "title", ""),
                                JsonGetString(reqJson, "body", ""),
                                parseAfter(),
                                JsonGetString(reqJson, "template", ""),
                                getParamsJson(),
                                ExtractRawJsonValue(reqJson, "origin"), err);
        } else if (action == "card_list") {
            resp = tqm_.CardList(getTaskId(), err);
        } else if (action == "card_show") {
            std::string idOrNum = JsonGetString(reqJson, "id", "");
            if (idOrNum.empty()) idOrNum = JsonGetString(reqJson, "number", "");
            resp = tqm_.CardShow(idOrNum, getTaskId(), err);
        } else if (action == "card_edit") {
            resp = tqm_.CardEdit(getTaskId(),
                                 JsonGetString(reqJson, "number", ""),
                                 req.contains("title") && req["title"].is_string(),
                                 req.value("title", std::string()),
                                 req.contains("body") && req["body"].is_string(),
                                 req.value("body", std::string()),
                                 req.contains("after"),
                                 parseAfter(),
                                 req.contains("template") && req["template"].is_string(),
                                 req.value("template", std::string()),
                                 req.contains("params"),
                                 getParamsJson(),
                                 getSession(), err);
        } else if (action == "card_remove") {
            resp = tqm_.CardRemove(getTaskId(),
                                   JsonGetString(reqJson, "number", ""),
                                   getSession(), err);
        } else if (action == "card_run") {
            if (!btm_) {
                return "{\"ok\":false,\"error\":\"behavior trees are not available\"}";
            }
            const bool ready = JsonGetBool(reqJson, "ready", false);
            int maxConcurrent = 1;
            if (req.contains("max") && req["max"].is_number_integer()) {
                maxConcurrent = req["max"].get<int>();
            }
            const bool force = JsonGetBool(reqJson, "force", false);
            std::string originJson;
            if (req.contains("origin") && req["origin"].is_object()) {
                originJson = req["origin"].dump();
            }
            resp = btm_->CardRun(getTaskId(),
                                 JsonGetString(reqJson, "number", ""),
                                 ready, maxConcurrent, force, originJson);
        } else if (action == "card_redo") {
            const bool run = JsonGetBool(reqJson, "run", false);
            if (run && !btm_) {
                return "{\"ok\":false,\"error\":\"behavior trees are not available\"}";
            }
            const bool force = JsonGetBool(reqJson, "force", false);
            std::string idOrNum = JsonGetString(reqJson, "id", "");
            if (idOrNum.empty()) idOrNum = JsonGetString(reqJson, "number", "");
            std::string originJson;
            if (req.contains("origin") && req["origin"].is_object()) {
                originJson = req["origin"].dump();
            }
            resp = tqm_.CardRedo(getTaskId(),
                                 idOrNum,
                                 req.contains("body") && req["body"].is_string(),
                                 req.value("body", std::string()),
                                 req.contains("params"),
                                 getParamsJson(),
                                 JsonGetString(reqJson, "reason", ""),
                                 force,
                                 originJson, err);
            if (run) {
                nlohmann::json redoDoc = nlohmann::json::parse(resp, nullptr, false);
                if (redoDoc.is_object() && redoDoc.value("ok", false)) {
                    const std::string newNum = redoDoc.value("number", std::string());
                    const std::string runResp = btm_->CardRun(getTaskId(), newNum, false, 1, force, originJson);
                    nlohmann::json runDoc = nlohmann::json::parse(runResp, nullptr, false);
                    if (!runDoc.is_object() || !runDoc.value("ok", false)) {
                        return runResp.empty() ? "{\"ok\":false,\"error\":\"failed to run rescue card\"}" : runResp;
                    }
                    redoDoc["btId"] = runDoc.value("btId", std::string());
                    redoDoc["state"] = runDoc.value("state", std::string("running"));
                    redoDoc["started"] = true;
                    resp = redoDoc.dump();
                }
            }
        } else {
            return "{\"ok\":false,\"error\":\"unknown card action: " + JsonEscape(action) + "\"}";
        }
        return resp;
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
                                     ExtractRawJsonValue(reqJson, "origin"),
                                     JsonGetString(reqJson, "task", ""));
            if (err.empty() && JsonGetBool(reqJson, "cancel_on_client_exit", false)) {
                const uint32_t clientPid = static_cast<uint32_t>(JsonGetInt(reqJson, "client_pid", 0));
                if (clientPid > 0) {
                    nlohmann::json addedDoc = nlohmann::json::parse(resp, nullptr, false);
                    const std::string dirId = addedDoc.is_object()
                                                ? addedDoc.value("direction_id", std::string())
                                                : std::string();
                    if (!dirId.empty()) {
                        std::string watchErr;
                        tqm_.WatchDirectionClient(dirId, clientPid, watchErr);
                    }
                }
            }
        } else if (action == "taskqueue_list") {
            int64_t sinceSec = 0;
            const std::string sinceStr = JsonGetString(reqJson, "since", "");
            if (!sinceStr.empty() && !ParseDurationToSec(sinceStr, sinceSec)) {
                err = "invalid since '" + sinceStr + "' (use a number followed by m, h or d, e.g. 6h)";
            } else {
                const std::string tqFilter = JsonGetString(reqJson, "taskqueue", "");
                const std::string stateFilter = JsonGetString(reqJson, "state", "");
                const std::string btFilter = JsonGetString(reqJson, "bt", "");
                const std::string taskFilter = JsonGetString(reqJson, "task", "");
                const std::string beforeFilter = JsonGetString(reqJson, "before", "");
                const std::string queryFilter = JsonGetString(reqJson, "query", "");
                int64_t finishedLimit = -1;
                {
                    nlohmann::json reqObj = nlohmann::json::parse(reqJson, nullptr, false);
                    if (reqObj.is_object()) {
                        if (reqObj.contains("finished_limit") && reqObj["finished_limit"].is_number_integer()) {
                            finishedLimit = reqObj["finished_limit"].get<int64_t>();
                        } else if (reqObj.contains("finishedLimit") && reqObj["finishedLimit"].is_number_integer()) {
                            finishedLimit = reqObj["finishedLimit"].get<int64_t>();
                        }
                    }
                }
                resp = tqm_.TaskQueueList(tqFilter, stateFilter, sinceSec, err,
                                          JsonGetString(reqJson, "origin", ""), btFilter,
                                          JsonGetBool(reqJson, "include_hidden", false), taskFilter,
                                          finishedLimit, beforeFilter, queryFilter);
                // Virtual task-group rows and their provisional leaves, derived from the live
                // state on every query. Nothing is stored, so a finished group drops both rows.
                if (err.empty() && btm_ && taskFilter.empty() && beforeFilter.empty() && queryFilter.empty() &&
                    JsonGetBool(reqJson, "include_planned", false)) {
                    nlohmann::json doc = nlohmann::json::parse(resp, nullptr, false);
                    if (doc.is_object() && doc.contains("jobs") && doc["jobs"].is_array()) {
                        BtRemainingInfo info = btm_->GetRemainingInfo();
                        std::unordered_map<std::string, const TaskGroupInfo*> groupByBt;
                        for (const auto& g : info.groups) {
                            groupByBt[g.id] = &g;
                            if (!tqFilter.empty() && g.taskqueue != tqFilter) continue;
                            if (!btFilter.empty() && g.id != btFilter) continue;
                            if (!TaskGroupStateMatches(stateFilter, g.state)) continue;
                            nlohmann::json job;
                            job["id"] = g.directionId.empty() ? ("group:" + g.id) : ("direction:" + g.directionId);
                            job["summary"] = g.summary.empty() ? g.id : g.summary;
                            job["state"] = g.state;
                            job["virtual"] = true;
                            job["taskqueue"] = g.taskqueue;
                            job["created_at"] = "";
                            job["start_time"] = g.startTime;
                            if (!g.directionId.empty()) {
                                job["direction_id"] = g.directionId;
                                job["direction"] = {{"id", g.directionId}, {"kind", "bt"}, {"bt_id", g.id}, {"detail", g.detail}};
                            }
                            if (!g.taskId.empty()) job["task_id"] = g.taskId;
                            job["group"] = {{"id", g.id}, {"detail", g.detail}};
                            // Group rows show the origin of the tree, like the jobs it adds.
                            const std::string groupOrigin = tqm_.BtOriginJson(g.id);
                            if (!groupOrigin.empty()) {
                                nlohmann::json origin = nlohmann::json::parse(groupOrigin, nullptr, false);
                                if (origin.is_object()) job["origin"] = std::move(origin);
                            }
                            doc["jobs"].push_back(std::move(job));
                        }
                        if (stateFilter.empty() || stateFilter == "pending" || stateFilter == "planned") {
                            for (const auto& t : info.planned) {
                                if (!tqFilter.empty() && t.queueName != tqFilter) continue;
                                if (!btFilter.empty() && t.btId != btFilter) continue;
                                nlohmann::json job;
                                job["id"] = "btplan:" + t.proposedJobId;
                                job["summary"] = t.summary.empty() ? t.nodeKey : t.summary;
                                job["state"] = "planned";
                                job["virtual"] = true;
                                job["taskqueue"] = t.queueName;
                                job["timeout_sec"] = t.estimateSec;
                                job["estimate_sec"] = t.estimateSec;
                                job["created_at"] = "";
                                job["bt"] = {{"id", t.btId}, {"node", t.nodeKey}, {"planned", true}};
                                auto git = groupByBt.find(t.btId);
                                if (git != groupByBt.end() && git->second) {
                                    if (!git->second->directionId.empty()) job["direction_id"] = git->second->directionId;
                                    if (!git->second->taskId.empty()) job["task_id"] = git->second->taskId;
                                }
                                doc["jobs"].push_back(std::move(job));
                            }
                        }
                        resp = doc.dump();
                    }
                }
            }
        } else if (action == "taskqueue_changes") {
            uint64_t since = 0;
            nlohmann::json req = nlohmann::json::parse(reqJson, nullptr, false);
            if (req.is_object() && req.contains("since")) {
                if (req["since"].is_number_unsigned()) {
                    since = req["since"].get<uint64_t>();
                } else if (req["since"].is_number_integer()) {
                    const int64_t v = req["since"].get<int64_t>();
                    since = v > 0 ? static_cast<uint64_t>(v) : 0;
                } else if (req["since"].is_string()) {
                    try {
                        since = std::stoull(req["since"].get<std::string>());
                    } catch (...) {
                        err = "invalid since";
                    }
                } else if (!req["since"].is_null()) {
                    err = "invalid since";
                }
            }
            if (err.empty()) {
                resp = tqm_.TaskQueueChanges(since, JsonGetBool(reqJson, "include_hidden", false), err);
            }
        } else if (action == "taskqueue_get") {
            const std::string id = JsonGetString(reqJson, "id", "");
            resp = tqm_.TaskQueueGet(id,
                                     JsonGetBool(reqJson, "include_instructions", true),
                                     static_cast<int>(JsonGetInt(reqJson, "rev", 0)), err);
            // The id may be a direction or task group rather than a job; return the direction/tree instead.
            if (!err.empty() && err.find("job not found") != std::string::npos) {
                if (IsDirectionOrDirId(id)) {
                    const std::string dirId = StripDirectionRowPrefix(id);
                    nlohmann::json dirRec;
                    if (tqm_.DirectionGet(dirId, dirRec)) {
                        const std::string kind = dirRec.value("kind", "");
                        const std::string btId = dirRec.value("btId", "");
                        if (kind == "bt" && btm_ && !btId.empty()) {
                            nlohmann::json btDoc = nlohmann::json::parse(btm_->Get(btId, false), nullptr, false);
                            if (btDoc.is_object() && btDoc.value("ok", false)) {
                                btDoc["kind"] = "taskGroup";
                                btDoc["direction_id"] = dirId;
                                resp = btDoc.dump();
                                err.clear();
                            }
                        } else {
                            std::string dirErr;
                            std::string dirResp = tqm_.DirectionShow(dirId, dirErr);
                            if (dirErr.empty()) {
                                resp = dirResp;
                                err.clear();
                            }
                        }
                    }
                } else if (btm_) {
                    // Task-group rows carry a "group:" prefix that the behavior tree manager does not know.
                    nlohmann::json btDoc = nlohmann::json::parse(btm_->Get(StripGroupRowPrefix(id), false), nullptr, false);
                    if (btDoc.is_object() && btDoc.value("ok", false)) {
                        btDoc["kind"] = "taskGroup";
                        resp = btDoc.dump();
                        err.clear();
                    }
                }
            }
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
                if (!err.empty()) {
                    if (err.find("managed by a behavior tree") != std::string::npos && btm_) {
                        // A tree-owned job: retry the tree's branch instead.
                        TaskQueueJob job;
                        if (tqm_.GetJobSnapshot(retryId, job) && !job.bt.id.empty()) {
                            resp = btm_->Retry(job.bt.id, job.bt.node, resetAttempts);
                            err.clear();
                        }
                    } else if (err.find("job not found") != std::string::npos) {
                        if (IsDirectionOrDirId(retryId)) {
                            const std::string dirId = StripDirectionRowPrefix(retryId);
                            nlohmann::json dirRec;
                            if (tqm_.DirectionGet(dirId, dirRec)) {
                                if (dirRec.value("kind", "") == "bt" && btm_ && !dirRec.value("btId", "").empty()) {
                                    resp = btm_->Retry(dirRec.value("btId", ""), "", resetAttempts);
                                    err.clear();
                                } else {
                                    std::string dirErr;
                                    resp = tqm_.DirectionRetryJobs(dirId, resetAttempts, refreshInstr,
                                                                   TaskQueueCaller::External, dirErr);
                                    if (dirErr.empty()) err.clear();
                                    else err = dirErr;
                                }
                            }
                        } else if (btm_) {
                            // The id may be a task group rather than a job.
                            resp = btm_->Retry(StripGroupRowPrefix(retryId), "", resetAttempts);
                            err.clear();
                        }
                    }
                }
            } else if (JsonGetBool(reqJson, "failed", false)) {
                // Bulk retry: every failed job, optionally limited to one queue and a time window.
                resp = tqm_.TaskQueueRetryFailed(JsonGetString(reqJson, "taskqueue", ""),
                                                 sinceSec, resetAttempts, refreshInstr, TaskQueueCaller::External, err);
            } else {
                err = "taskqueue_retry requires an id, or failed=true for bulk retry";
            }
        } else if (action == "taskqueue_supersede") {
            const std::string id = JsonGetString(reqJson, "id", "");
            const std::string reason = JsonGetString(reqJson, "reason", "");
            std::string newId = JsonGetString(reqJson, "new_id", "");
            if (newId.empty()) newId = JsonGetString(reqJson, "newId", "");
            const std::string patch = ExtractRawJsonValue(reqJson, "patch");
            const std::string origin = ExtractRawJsonValue(reqJson, "origin");
            resp = tqm_.TaskQueueSupersede(id, reason, newId, patch, origin, err);
        } else if (action == "taskqueue_update") {
            resp = tqm_.TaskQueueUpdate(JsonGetString(reqJson, "id", ""),
                                        ExtractRawJsonValue(reqJson, "patch"), TaskQueueCaller::External, err);
        } else if (action == "taskqueue_cancel") {
            const std::string id = JsonGetString(reqJson, "id", "");
            resp = tqm_.TaskQueueCancel(id, err);
            // The id may be a direction or task group rather than a job; cancel it instead.
            if (!err.empty() && err.find("job not found") != std::string::npos) {
                if (IsDirectionOrDirId(id)) {
                    const std::string dirId = StripDirectionRowPrefix(id);
                    nlohmann::json dirRec;
                    if (tqm_.DirectionGet(dirId, dirRec)) {
                        if (dirRec.value("kind", "") == "bt" && btm_ && !dirRec.value("btId", "").empty()) {
                            resp = btm_->Cancel(dirRec.value("btId", ""));
                            err.clear();
                        } else {
                            std::string dirErr;
                            resp = tqm_.DirectionCancelJobs(dirId, dirErr);
                            if (dirErr.empty()) err.clear();
                            else err = dirErr;
                        }
                    }
                } else if (btm_) {
                    resp = btm_->Cancel(StripGroupRowPrefix(id));
                    err.clear();
                }
            }
        } else if (action == "taskqueue_close") {
            const std::string id = JsonGetString(reqJson, "id", "");
            resp = tqm_.TaskQueueClose(id, err);
            // A direction/task group has no job to close; stop the tree or jobs.
            if (!err.empty() && err.find("job not found") != std::string::npos) {
                if (IsDirectionOrDirId(id)) {
                    const std::string dirId = StripDirectionRowPrefix(id);
                    nlohmann::json dirRec;
                    if (tqm_.DirectionGet(dirId, dirRec)) {
                        if (dirRec.value("kind", "") == "bt" && btm_ && !dirRec.value("btId", "").empty()) {
                            resp = btm_->Cancel(dirRec.value("btId", ""));
                            err.clear();
                        } else {
                            std::string dirErr;
                            resp = tqm_.DirectionCancelJobs(dirId, dirErr);
                            if (dirErr.empty()) err.clear();
                            else err = dirErr;
                        }
                    }
                } else if (btm_) {
                    resp = btm_->Cancel(StripGroupRowPrefix(id));
                    err.clear();
                }
            }
        } else if (action == "taskqueue_takeover") {
            resp = tqm_.TaskQueueTakeover(JsonGetString(reqJson, "id", ""),
                                          JsonGetString(reqJson, "by", "cli"),
                                          JsonGetString(reqJson, "reason", ""), err);
        } else if (action == "taskqueue_complete") {
            resp = tqm_.TaskQueueComplete(JsonGetString(reqJson, "id", ""),
                                          JsonGetString(reqJson, "report", ""),
                                          JsonGetString(reqJson, "result", "succeeded"),
                                          JsonGetString(reqJson, "reason", ""), err);
        } else if (action == "taskqueue_pause" || action == "taskqueue_resume") {
            const bool paused = (action == "taskqueue_pause");
            const std::string name = JsonGetString(reqJson, "taskqueue", "");
            resp = tqm_.TaskQueuePause(name, paused, err);
            // The name may be a direction or task group rather than a queue.
            if (!err.empty() && err.find("task queue not found") != std::string::npos) {
                if (IsDirectionOrDirId(name)) {
                    const std::string dirId = StripDirectionRowPrefix(name);
                    nlohmann::json dirRec;
                    if (tqm_.DirectionGet(dirId, dirRec)) {
                        if (dirRec.value("kind", "") == "bt" && btm_ && !dirRec.value("btId", "").empty()) {
                            std::string btResp = btm_->Pause(dirRec.value("btId", ""), paused);
                            if (btResp.find("\"ok\":true") != std::string::npos) {
                                resp = btResp;
                                err.clear();
                            }
                        } else {
                            std::string dirErr;
                            resp = tqm_.DirectionPauseJobs(dirId, paused, dirErr);
                            if (dirErr.empty()) err.clear();
                            else err = dirErr;
                        }
                    }
                } else if (btm_) {
                    std::string btResp = btm_->Pause(StripGroupRowPrefix(name), paused);
                    if (btResp.find("\"ok\":true") != std::string::npos) {
                        resp = btResp;
                        err.clear();
                    }
                }
            }
        } else if (action == "taskqueue_pause_job") {
            const std::string id = JsonGetString(reqJson, "id", "");
            resp = tqm_.TaskQueuePauseJob(id, true, err);
            if (!err.empty() && err.find("job not found") != std::string::npos) {
                if (IsDirectionOrDirId(id)) {
                    const std::string dirId = StripDirectionRowPrefix(id);
                    nlohmann::json dirRec;
                    if (tqm_.DirectionGet(dirId, dirRec)) {
                        if (dirRec.value("kind", "") == "bt" && btm_ && !dirRec.value("btId", "").empty()) {
                            resp = btm_->Pause(dirRec.value("btId", ""), true);
                            err.clear();
                        } else {
                            std::string dirErr;
                            resp = tqm_.DirectionPauseJobs(dirId, true, dirErr);
                            if (dirErr.empty()) err.clear();
                            else err = dirErr;
                        }
                    }
                } else if (id.rfind("group:", 0) == 0 && btm_) {
                    resp = btm_->Pause(StripGroupRowPrefix(id), true);
                    err.clear();
                }
            }
        } else if (action == "taskqueue_resume_job") {
            const std::string id = JsonGetString(reqJson, "id", "");
            resp = tqm_.TaskQueuePauseJob(id, false, err);
            if (!err.empty() && err.find("job not found") != std::string::npos) {
                if (IsDirectionOrDirId(id)) {
                    const std::string dirId = StripDirectionRowPrefix(id);
                    nlohmann::json dirRec;
                    if (tqm_.DirectionGet(dirId, dirRec)) {
                        if (dirRec.value("kind", "") == "bt" && btm_ && !dirRec.value("btId", "").empty()) {
                            resp = btm_->Pause(dirRec.value("btId", ""), false);
                            err.clear();
                        } else {
                            std::string dirErr;
                            resp = tqm_.DirectionPauseJobs(dirId, false, dirErr);
                            if (dirErr.empty()) err.clear();
                            else err = dirErr;
                        }
                    }
                } else if (id.rfind("group:", 0) == 0 && btm_) {
                    resp = btm_->Pause(StripGroupRowPrefix(id), false);
                    err.clear();
                }
            }
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
        } else if (action == "taskqueue_remove") {
            const std::string id = JsonGetString(reqJson, "id", "");
            const bool force = JsonGetBool(reqJson, "force", false);
            resp = tqm_.TaskQueueRemove(id, force, err);
            // The id may be a direction or task group rather than a job; remove the tree/jobs instead.
            if (!err.empty() &&
                (err.find("job not found") != std::string::npos || err.find("virtual") != std::string::npos)) {
                if (IsDirectionOrDirId(id)) {
                    const std::string dirId = StripDirectionRowPrefix(id);
                    nlohmann::json dirRec;
                    if (tqm_.DirectionGet(dirId, dirRec)) {
                        if (dirRec.value("kind", "") == "bt" && btm_ && !dirRec.value("btId", "").empty()) {
                            resp = btm_->Remove(dirRec.value("btId", ""));
                            err.clear();
                        } else {
                            std::string dirErr;
                            resp = tqm_.DirectionRemoveJobs(dirId, force, dirErr);
                            if (dirErr.empty()) err.clear();
                            else err = dirErr;
                        }
                    }
                } else if (btm_) {
                    resp = btm_->Remove(StripGroupRowPrefix(id));
                    err.clear();
                }
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
            counts["closed"] = c.closed;
            counts["takenOver"] = c.takenOver;
            counts["waitingForPermit"] = c.waitingForPermit;
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
            out["closed"] = c.closed;
            out["takenOver"] = c.takenOver;
            out["waitingForPermit"] = c.waitingForPermit;
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
                    fail["class"] = (snap.state == ProcState::TimedOut || snap.state == ProcState::IdleTimedOut || snap.state == ProcState::StartupTimedOut) ? "timeout" : (snap.exitCode != 0 ? "exit_nonzero" : "none");
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
            resp = btm_->Validate(btJson, JsonGetBool(reqJson, "strict", false));
        } else if (action == "bt_start") {
            std::string btJson, err;
            if (!extractBtPayload(btJson, err)) {
                return "{\"ok\":false,\"error\":\"" + JsonEscape(err) + "\"}";
            }
            const std::string taskId = JsonGetString(reqJson, "task", "");
            if (!taskId.empty()) {
                if (!tqm_.ValidateTaskActive(taskId, err)) {
                    return "{\"ok\":false,\"error\":\"" + JsonEscape(err) + "\"}";
                }
                auto btDef = json::parse(btJson, nullptr, false);
                if (btDef.is_object()) {
                    btDef["taskId"] = taskId;
                    btJson = btDef.dump();
                }
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
            if (JsonGetBool(reqJson, "cancel_on_client_exit", false)) {
                const uint32_t clientPid = static_cast<uint32_t>(JsonGetInt(reqJson, "client_pid", 0));
                if (clientPid > 0) {
                    nlohmann::json startedDoc = nlohmann::json::parse(resp, nullptr, false);
                    if (startedDoc.is_object() && startedDoc.value("ok", false)) {
                        const std::string dirId = startedDoc.value("directionId", std::string());
                        if (!dirId.empty()) {
                            std::string watchErr;
                            tqm_.WatchDirectionClient(dirId, clientPid, watchErr);
                        }
                    }
                }
            }
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
        } else if (action == "bt_remove") {
            std::string id = req.value("id", std::string());
            resp = btm_->Remove(id);
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
            if (!cfg.contains("usage_round_time")) {
                cfg["usage_round_time"] = GetUsageRoundTime();
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
            const nlohmann::json before = cfg;
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
            if (cfg.contains("usage_round_time") && cfg["usage_round_time"].is_string()) {
                SetUsageRoundTime(cfg["usage_round_time"].get<std::string>());
            }
            // Restart a listener only when its block actually changed. A caller that
            // sends the whole config back (dashboard settings save, CLI) used to
            // restart the terminal server and Chat on every unrelated change, and the
            // GUI blocked on the restart chain while closing its Settings dialog.
            auto blockOf = [](const nlohmann::json& j, const char* key) {
                return j.is_object() && j.contains(key) ? j[key] : nlohmann::json();
            };
            auto blockChanged = [&](const char* key) {
                return patch.contains(key) && blockOf(before, key) != blockOf(cfg, key);
            };
            const bool webuiChanged = blockChanged("webui");
            const bool chatChanged = blockChanged("chat");
            const bool terminalChanged = blockChanged("terminal");
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
    else if (action.rfind("routine_", 0) == 0) {
        using json = nlohmann::json;
        json req = json::parse(reqJson, nullptr, false);
        if (req.is_discarded() || !req.is_object()) {
            return "{\"ok\":false,\"error\":\"invalid JSON request\"}";
        }
        if (!routineManager_) {
            return "{\"ok\":false,\"error\":\"routines are not available\"}";
        }
        const std::string name = req.value("name", std::string());
        std::string cwd = req.value("cwd", std::string());
        if (cwd.empty()) cwd = WideToUtf8(GetCurrentDirectoryWide());

        json resp;
        std::string err;
        if (action == "routine_list") {
            resp = json::parse(routineManager_->List(cwd), nullptr, false);
        } else if (action == "routine_show") {
            if (name.empty()) return "{\"ok\":false,\"error\":\"name is required\"}";
            resp = json::parse(routineManager_->Show(name, cwd), nullptr, false);
            if (!resp.is_discarded() && resp.value("ok", false) && resp.contains("lastRun") &&
                resp["lastRun"].is_object()) {
                const std::string runId = resp["lastRun"].value("runId", std::string());
                const std::string state = resp["lastRun"].value("state", std::string());
                if (!runId.empty() && state != "succeeded" && state != "failed" && state != "cancelled" && btm_) {
                    json tree = json::parse(btm_->Get(runId, true), nullptr, false);
                    if (!tree.is_discarded() && tree.value("ok", false)) {
                        const std::string liveState = tree.value("state", state);
                        if (liveState != state) {
                            const bool wasStale = resp["lastRun"].value("staleAtStart", false);
                            json diff = resp["lastRun"].value("inputDiff", json::object());
                            if (liveState == "succeeded" || liveState == "failed") {
                                FinalizeRoutineRun(*routineManager_, triageManager_.get(), btm_, tqm_,
                                                   name, cwd, runId, liveState, wasStale, diff);
                            } else {
                                json patch = {{"state", liveState}};
                                if (liveState == "cancelled") patch["finishedAt"] = GetCurrentTimestampStr();
                                routineManager_->UpdateLatestRun(name, patch, err);
                            }
                            resp["lastRun"]["state"] = liveState;
                            if (liveState == "succeeded" || liveState == "failed" || liveState == "cancelled") {
                                resp["lastRun"]["finishedAt"] = GetCurrentTimestampStr();
                                if (resp.contains("runs") && resp["runs"].is_array() && !resp["runs"].empty()) {
                                    resp["runs"].back() = resp["lastRun"];
                                }
                            }
                        }
                    }
                }
            }
            if (!resp.is_discarded() && resp.value("ok", false) && resp.contains("lastRun") &&
                resp["lastRun"].is_object()) {
                const std::string runId = resp["lastRun"].value("runId", std::string());
                resp["nodes"] = runId.empty() ? json::array() : RoutineNodeResults(btm_, tqm_, runId);
                AddRoutineNodeDiff(resp, btm_, tqm_, resp.value("runs", json::array()), resp["nodes"]);
            }
        } else if (action == "routine_check") {
            if (name.empty()) return "{\"ok\":false,\"error\":\"name is required\"}";
            resp = json::parse(routineManager_->Check(name, cwd), nullptr, false);
        } else if (action == "routine_save") {
            if (name.empty()) return "{\"ok\":false,\"error\":\"name is required\"}";
            std::string btText;
            if (req.contains("bt") && !req["bt"].is_null()) {
                if (req["bt"].is_string()) btText = req["bt"].get<std::string>();
                else btText = req["bt"].dump();
            } else if (req.contains("file") && req["file"].is_string()) {
                const std::wstring absPath = ResolveAbsolutePathWide(Utf8ToWide(req["file"].get<std::string>()));
                std::ifstream ifs(absPath, std::ios::binary);
                if (!ifs) return "{\"ok\":false,\"error\":\"cannot read BT file\"}";
                std::ostringstream ss;
                ss << ifs.rdbuf();
                btText = ss.str();
            } else {
                return "{\"ok\":false,\"error\":\"routine_save requires 'bt' or 'file'\"}";
            }
            std::vector<std::string> inputs;
            if (req.contains("inputs") && req["inputs"].is_array()) {
                for (const auto& item : req["inputs"]) {
                    if (item.is_string()) inputs.push_back(item.get<std::string>());
                }
            }
            const std::string by = req.value("by", std::string("hancho-cli"));
            bool fromMcp = false;
            if (req.contains("origin") && req["origin"].is_object()) {
                const std::string client = req["origin"].value("client", std::string());
                fromMcp = !client.empty() && client.find("hancho-cli") == std::string::npos;
            }
            if (fromMcp) {
                json meta = {{"request", "routine_save via MCP"},
                             {"rationale", req.value("reason", std::string())},
                             {"by", by}};
                resp = json::parse(routineManager_->SaveProposal(
                    name, btText, inputs,
                    req.value("summary", std::string()),
                    req.value("notes", std::string()),
                    req.value("scope", std::string("project")),
                    cwd, by,
                    req.value("reason", std::string()), meta), nullptr, false);
            } else {
                resp = json::parse(routineManager_->Save(
                    name, btText, inputs,
                    req.value("summary", std::string()),
                    req.value("notes", std::string()),
                    req.value("scope", std::string("project")),
                    cwd, by,
                    req.value("reason", std::string())), nullptr, false);
            }
        } else if (action == "routine_save_from_bt") {
            if (name.empty()) return "{\"ok\":false,\"error\":\"name is required\"}";
            resp = json::parse(routineManager_->SaveFromBt(
                name, req.value("btId", req.value("fromBt", std::string())),
                req.value("scope", std::string("project")), cwd,
                req.value("by", std::string("hancho-cli")),
                req.value("proposed", false),
                req.value("reason", std::string())), nullptr, false);
        } else if (action == "routine_ask") {
            const std::string requestText = req.value("request", req.value("text", std::string()));
            if (requestText.empty()) return "{\"ok\":false,\"error\":\"request text is required\"}";
            const int timeoutMs = static_cast<int>(JsonGetInt(reqJson, "timeout_ms", 600000));
            resp = json::parse(HandleRoutineAsk(*routineManager_, btm_, &tqm_, requestText,
                                                name, cwd,
                                                req.value("scope", std::string("project")),
                                                req.value("by", std::string("chat")), timeoutMs),
                               nullptr, false);
        } else if (action == "routine_approve") {
            if (name.empty()) return "{\"ok\":false,\"error\":\"name is required\"}";
            resp = json::parse(routineManager_->Approve(
                name, cwd, req.value("userSaid", std::string()),
                req.value("by", std::string("user")),
                req.value("via", std::string("chat"))), nullptr, false);
        } else if (action == "routine_reject") {
            if (name.empty()) return "{\"ok\":false,\"error\":\"name is required\"}";
            resp = json::parse(routineManager_->Reject(
                name, cwd, req.value("reason", std::string()),
                req.value("by", std::string("user"))), nullptr, false);
        } else if (action == "routine_run") {
            if (name.empty()) return "{\"ok\":false,\"error\":\"name is required\"}";
            if (!btm_) return "{\"ok\":false,\"error\":\"behavior trees are not available\"}";
            resp = json::parse(routineManager_->PrepareRun(name, cwd), nullptr, false);
            if (resp.is_discarded() || !resp.value("ok", false)) {
                return resp.is_discarded() ? "{\"ok\":false,\"error\":\"invalid routine response\"}" : resp.dump();
            }
            const std::string runId = resp["runId"].get<std::string>();
            std::string btText = resp["bt"].get<std::string>();
            const std::string taskqueue = resp["taskqueue"].get<std::string>();
            const bool wasStale = resp.value("state", std::string()) == "stale";
            json inputDiff = json::object();
            for (const char* key : {"state", "added", "removed", "changed", "absorbed"}) {
                if (resp.contains(key)) inputDiff[key] = resp[key];
            }
            const std::string taskId = JsonGetString(reqJson, "task", "");
            if (!taskId.empty()) {
                if (!tqm_.ValidateTaskActive(taskId, err)) {
                    return "{\"ok\":false,\"error\":\"" + JsonEscape(err) + "\"}";
                }
                auto btDef = json::parse(btText, nullptr, false);
                if (btDef.is_object()) {
                    btDef["taskId"] = taskId;
                    btText = btDef.dump();
                }
            }
            if (req.contains("origin") && req["origin"].is_object()) {
                tqm_.SetBtOrigin(runId, req["origin"].dump());
            }
            json start = json::parse(btm_->Start(btText, false), nullptr, false);
            if (start.is_discarded() || !start.value("ok", false)) {
                return start.is_discarded() ? "{\"ok\":false,\"error\":\"BT start failed\"}" : start.dump();
            }
            json runRecord;
            runRecord["runId"] = runId;
            runRecord["name"] = name;
            runRecord["state"] = "running";
            runRecord["startedAt"] = GetCurrentTimestampStr();
            runRecord["finishedAt"] = "";
            runRecord["taskqueue"] = taskqueue;
            runRecord["staleAtStart"] = wasStale;
            runRecord["inputDiff"] = inputDiff;
            routineManager_->AppendRun(name, runRecord, err);

            json out;
            out["ok"] = true;
            out["name"] = name;
            out["runId"] = runId;
            out["taskqueue"] = taskqueue;
            out["state"] = "running";
            out["waited"] = false;
            out["inputsState"] = wasStale ? "stale" : "ok";
            out["inputDiff"] = inputDiff;

            const bool wait = req.value("wait", false);
            if (wait) {
                const int timeoutMs = static_cast<int>(JsonGetInt(reqJson, "timeout_ms", 60000));
                json waited = json::parse(btm_->Wait(runId, 0, timeoutMs, false), nullptr, false);
                const bool timedOut = waited.value("timedOut", false);
                const std::string finalState = waited.value("state", "running");
                out["waited"] = true;
                out["timedOut"] = timedOut;
                out["state"] = finalState;
                if (!timedOut) {
                    FinalizeRoutineRun(*routineManager_, triageManager_.get(), btm_, tqm_, name,
                                       cwd, runId, finalState, wasStale, inputDiff);
                }
                out["nodes"] = RoutineNodeResults(btm_, tqm_, runId);
                AddRoutineNodeDiff(out, btm_, tqm_, RoutineManager::ReadRuns(name), out["nodes"]);
                out["bt"] = std::move(waited);
            } else {
                out["bt"] = std::move(start);
            }
            resp = std::move(out);
        } else {
            return "{\"ok\":false,\"error\":\"Unknown routine IPC action: " + JsonEscape(action) + "\"}";
        }

        HWND hwnd = notifyHwnd_.load();
        if (hwnd) PostMessageW(hwnd, WM_PC_IPC_EVENT, 0, 0);
        return resp.is_discarded() ? "{\"ok\":false,\"error\":\"invalid routine response\"}" : resp.dump();
    }
    return "{\"ok\":false,\"error\":\"Unknown IPC action: " + JsonEscape(action) + "\"}";
}

} // namespace pc
