#include "ipc_server.h"
#include "bt_manager.h"
#include "chain_guard.h"
#include "llm_stream.h"
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
    : pm_(pm), tqm_(tqm), btm_(nullptr), notifyHwnd_(notifyHwnd) {}

IpcServer::IpcServer(ProcessManager& pm, TaskQueueManager& tqm, BtManager* btm, HWND notifyHwnd)
    : pm_(pm), tqm_(tqm), btm_(btm), notifyHwnd_(notifyHwnd) {}

IpcServer::~IpcServer() {
    Stop();
}

bool IpcServer::Start() {
    stop_ = false;
    acceptThread_ = std::thread(&IpcServer::AcceptLoop, this);
    return true;
}

void IpcServer::Stop() {
    if (stop_.exchange(true)) return;
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
            Sleep(100);
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

std::string IpcServer::HandleRequestJson(const std::string& reqJson) {
    std::string action = JsonGetString(reqJson, "action", "");
    if (action != "ping") {
        WriteAuditLog("IPC_REQ", reqJson);
    }
    std::ostringstream oss;

    if (action == "ping") {
        return "{\"ok\":true,\"status\":\"running\"}";
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
            Sleep(static_cast<DWORD>(waitMs));
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

    else if (action.rfind("taskqueue_", 0) == 0) {
        std::string err;
        std::string resp;
        if (action == "taskqueue_add") {
            resp = tqm_.TaskQueueAdd(JsonGetString(reqJson, "taskqueue", ""),
                                     ExtractRawJsonValue(reqJson, "jobs"),
                                     JsonGetInt(reqJson, "concurrency", 0),
                                     JsonGetString(reqJson, "notify_file", ""), err,
                                     ExtractRawJsonValue(reqJson, "origin"));
        } else if (action == "taskqueue_list") {
            int64_t sinceSec = 0;
            const std::string sinceStr = JsonGetString(reqJson, "since", "");
            if (!sinceStr.empty() && !ParseDurationToSec(sinceStr, sinceSec)) {
                err = "invalid since '" + sinceStr + "' (use a number followed by m, h or d, e.g. 6h)";
            } else {
                resp = tqm_.TaskQueueList(JsonGetString(reqJson, "taskqueue", ""),
                                          JsonGetString(reqJson, "state", ""), sinceSec, err,
                                          JsonGetString(reqJson, "origin", ""));
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
            resp = btm_->Start(btJson);
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
    return "{\"ok\":false,\"error\":\"Unknown IPC action: " + JsonEscape(action) + "\"}";
}

} // namespace pc
