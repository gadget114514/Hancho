#include "mcp_server.h"
#include "ipc_client.h"
#include "common.h"
#include "nlohmann/json.hpp"
#include <fstream>
#include <iostream>
#include <sstream>
#include <io.h>
#include <fcntl.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace pc {

// The top-level JSON-RPC fields of one request line. Parsed with nlohmann::json so that keys
// inside "params" (e.g. a tool argument named "id") are never mistaken for the request's own;
// the MCP TypeScript SDK writes "id" after "params".
struct McpRequest {
    bool valid = false;
    std::string idRaw = "null";   // raw JSON value echoed back in the response
    std::string method;
    std::string toolName;         // params.name
    std::string argumentsJson = "{}";  // params.arguments
    std::string cancelRequestIdRaw;    // params.requestId of notifications/cancelled
};

static McpRequest ParseMcpRequest(const std::string& line) {
    McpRequest req;
    auto j = nlohmann::json::parse(line, nullptr, false);
    if (j.is_discarded() || !j.is_object()) return req;
    req.valid = true;
    try {
        auto id = j.find("id");
        if (id != j.end() && (id->is_string() || id->is_number())) req.idRaw = id->dump();
        auto method = j.find("method");
        if (method != j.end() && method->is_string()) req.method = method->get<std::string>();
        auto params = j.find("params");
        if (params != j.end() && params->is_object()) {
            auto name = params->find("name");
            if (name != params->end() && name->is_string()) req.toolName = name->get<std::string>();
            auto args = params->find("arguments");
            if (args != params->end() && args->is_object()) req.argumentsJson = args->dump();
            auto reqId = params->find("requestId");
            if (reqId != params->end() && (reqId->is_string() || reqId->is_number())) {
                req.cancelRequestIdRaw = reqId->dump();
            }
        }
    } catch (const nlohmann::json::exception&) {
        // dump() throws on invalid UTF-8; treat the line as unparseable.
        req.valid = false;
    }
    return req;
}

// Extract a raw JSON value (object, array, string or scalar) for a top-level key.
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

static std::string BuildMcpToolResult(const std::string& rpcIdRaw, const std::string& contentText, bool isError = false) {
    std::ostringstream oss;
    oss << "{\"jsonrpc\":\"2.0\",\"id\":" << rpcIdRaw
        << ",\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"" << JsonEscape(contentText) << "\"}]"
        << (isError ? ",\"isError\":true" : "")
        << "}}";
    return oss.str();
}

// stdout is shared by the read loop and the bt_wait threads; one line per message.
static std::mutex g_outMutex;

static void EmitLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_outMutex);
    std::cout << line << "\n" << std::flush;
}

// bt_wait calls in flight. Each runs on its own thread and waits in short IPC slices,
// so a notifications/cancelled from the client (for example when its tool-call
// timeout fires) stops it within one slice instead of holding a wait open.
class PendingBtWaits {
public:
    static constexpr int64_t kDefaultTimeoutMs = 600000;  // 10 minutes
    static constexpr int64_t kSliceMs = 2000;

    void Start(const std::string& rpcId, const nlohmann::json& args) {
        Reap();
        auto entry = std::make_unique<Entry>();
        entry->rpcId = rpcId;
        Entry* e = entry.get();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            entries_.push_back(std::move(entry));
        }
        e->thread = std::thread([this, e, args] { Run(*e, args); });
    }

    void Cancel(const std::string& rpcId) {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& e : entries_) {
            if (e->rpcId == rpcId) e->cancelled = true;
        }
        WriteAuditLog("MCP_CANCEL", "requestId=" + rpcId);
    }

    void CancelAllAndJoin() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto& e : entries_) e->cancelled = true;
        }
        std::vector<std::unique_ptr<Entry>> all;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            all.swap(entries_);
        }
        for (auto& e : all) {
            if (e->thread.joinable()) e->thread.join();
        }
    }

private:
    struct Entry {
        std::string rpcId;
        std::atomic<bool> cancelled{false};
        std::atomic<bool> done{false};
        std::thread thread;
    };

    void Reap() {
        std::vector<std::unique_ptr<Entry>> finished;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (auto it = entries_.begin(); it != entries_.end();) {
                if ((*it)->done) {
                    finished.push_back(std::move(*it));
                    it = entries_.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& e : finished) {
            if (e->thread.joinable()) e->thread.join();
        }
    }

    // Reply unless the client has cancelled the request (MCP: no response after cancel).
    void Finish(Entry& e, const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!e.cancelled) EmitLine(message);
        }
        e.done = true;
    }

    void Run(Entry& e, nlohmann::json args) {
        int64_t totalMs = kDefaultTimeoutMs;
        if (args.contains("timeoutMs") && args["timeoutMs"].is_number_integer()) {
            totalMs = args["timeoutMs"].get<int64_t>();
        } else if (args.contains("timeout_ms") && args["timeout_ms"].is_number_integer()) {
            totalMs = args["timeout_ms"].get<int64_t>();
        }
        args.erase("timeout_ms");
        const uint64_t startTick = GetTickCount64();

        while (true) {
            if (e.cancelled) {
                WriteAuditLog("MCP_TOOL_RESP", "tool=bt_wait cancelled");
                e.done = true;
                return;
            }
            int64_t slice = kSliceMs;
            if (totalMs >= 0) {
                int64_t remain = totalMs - static_cast<int64_t>(GetTickCount64() - startTick);
                if (remain < 0) remain = 0;
                if (remain < slice) slice = remain;
            }
            args["timeoutMs"] = slice;

            std::string ipcResp, err;
            if (!SendIpcRequest(args.dump(), ipcResp, err, true)) {
                WriteAuditLog("MCP_TOOL_ERR", "tool=bt_wait error=" + err);
                Finish(e, BuildMcpToolResult(e.rpcId, "IPC error: " + err, true));
                return;
            }
            const bool ok = JsonGetBool(ipcResp, "ok", false);
            const bool timedOut = JsonGetBool(ipcResp, "timedOut", false);
            const bool overall = totalMs >= 0 &&
                static_cast<int64_t>(GetTickCount64() - startTick) >= totalMs;
            if (!ok || !timedOut || overall) {
                WriteAuditLog("MCP_TOOL_RESP", std::string("tool=bt_wait ok=") + (ok ? "true" : "false") +
                                                   " bytes=" + std::to_string(ipcResp.size()));
                Finish(e, BuildMcpToolResult(e.rpcId, ipcResp, !ok));
                return;
            }
        }
    }

    std::mutex mutex_;
    std::vector<std::unique_ptr<Entry>> entries_;
};

int RunMcpServer() {
    // Set stdin and stdout to binary mode to avoid CRLF translation issues
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);

    WriteAuditLog("MCP_SERVER", "MCP stdio server started (Log dir: " + GetLogsDirectoryUtf8() + ")");

    PendingBtWaits waits;
    // "name version" from the client's initialize request; recorded as the origin of jobs it adds.
    std::string mcpClient = "mcp";
    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) continue;

        McpRequest req = ParseMcpRequest(line);
        if (!req.valid) {
            EmitLine("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32700,\"message\":\"Parse error\"}}");
            continue;
        }
        const std::string& method = req.method;
        const std::string& rpcId = req.idRaw;

        if (method == "initialize") {
            WriteAuditLog("MCP_INIT", line);
            {
                auto initObj = nlohmann::json::parse(line, nullptr, false);
                if (initObj.is_object() && initObj.contains("params") && initObj["params"].is_object()) {
                    const auto& params = initObj["params"];
                    if (params.contains("clientInfo") && params["clientInfo"].is_object()) {
                        const auto& ci = params["clientInfo"];
                        std::string name = ci.value("name", std::string());
                        std::string ver = ci.value("version", std::string());
                        if (!name.empty()) mcpClient = ver.empty() ? name : name + " " + ver;
                    }
                }
            }
            std::ostringstream oss;
            oss << "{\"jsonrpc\":\"2.0\",\"id\":" << rpcId
                << ",\"result\":{"
                << "\"protocolVersion\":\"2024-11-05\","
                << "\"capabilities\":{\"tools\":{}},"
                << "\"serverInfo\":{\"name\":\"Hancho\",\"version\":\"" PC_VERSION "\"}"
                << "}}";
            EmitLine(oss.str());
        }
        else if (method == "notifications/cancelled") {
            // The client gave up on a request (often its own tool-call timeout). Stop a
            // pending bt_wait so it neither answers late nor keeps a wait open.
            if (!req.cancelRequestIdRaw.empty()) waits.Cancel(req.cancelRequestIdRaw);
            continue;
        }
        else if (method.rfind("notifications/", 0) == 0) {
            // Notification requires no response
            continue;
        }
        else if (method == "ping") {
            EmitLine("{\"jsonrpc\":\"2.0\",\"id\":" + rpcId + ",\"result\":{}}");
        }
        else if (method == "tools/list") {
            std::ostringstream oss;
            oss << "{\"jsonrpc\":\"2.0\",\"id\":" << rpcId << ",\"result\":{\"tools\":["
                << "{"
                << "\"name\":\"start_process\","
                << "\"description\":\"Start and manage an external LLM CLI or program (e.g. claude, opencode) under a Win32 Job Object and display it on the Hancho GUI Dashboard. Automatically writes live output to log_file and creates done_file atomically when the process exits. Set wait=true and wait_ms=0 to block until completion. To run multiple commands in order, use taskqueue_add instead; chained commands such as '&&' are rejected.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"cmd\":{\"type\":\"string\",\"description\":\"Command line to execute (e.g. 'claude -p ...' or 'opencode')\"},"
                << "\"name\":{\"type\":\"string\",\"description\":\"Friendly display label for the dashboard\"},"
                << "\"mode\":{\"type\":\"string\",\"enum\":[\"pipe\",\"pty\",\"console\"],\"description\":\"Execution mode: 'pipe' (default; non-interactive stdout/stdin, no ConPTY), 'pty' (Win32 ConPTY interactive terminal), or 'console' (separate visible console window)\"},"
                << "\"pty\":{\"type\":\"boolean\",\"description\":\"Set true to enable ConPTY (same as mode='pty'). Default false.\"},"
                << "\"open_stdin\":{\"type\":\"boolean\",\"description\":\"Pipe mode only: keep stdin writable for send_process_input. Default true; set false to close stdin after launch so non-interactive programs see EOF.\"},"
                << "\"cwd\":{\"type\":\"string\",\"description\":\"Working directory path (default: current directory)\"},"
                << "\"log_file\":{\"type\":\"string\",\"description\":\"Optional custom path for the real-time output log file (defaults to logs/proc_<id>_<name>_<timestamp>.log)\"},"
                << "\"done_file\":{\"type\":\"string\",\"description\":\"Optional custom path for the completion marker file created atomically when the process exits (defaults to logs/proc_<id>.done)\"},"
                << "\"timeout_sec\":{\"type\":\"integer\",\"description\":\"Max runtime timeout in seconds (0 = unlimited)\"},"
                << "\"idle_timeout_sec\":{\"type\":\"integer\",\"description\":\"Auto-kill if no output for N seconds (0 = unlimited)\"},"
                << "\"estimate_end\":{\"type\":\"string\",\"description\":\"Optional estimated end time shown in gray in the End Time column while running (e.g. '18:30' or '2026-10-22 18:30:00'). If omitted, timeout_sec makes the column show remaining seconds.\"},"
                << "\"agent_type\":{\"type\":\"string\",\"description\":\"LLM agent type: 'claude', 'agy', 'opencode', 'codex', 'aider', 'ollama', or 'auto' (default: auto-detected from command/name). Enables JSONL stream parsing (<log>.stream.jsonl) and model capture.\",\"enum\":[\"auto\",\"claude\",\"agy\",\"antigravity\",\"opencode\",\"codex\",\"aider\",\"ollama\"]},"
                << "\"json_stream\":{\"type\":\"boolean\",\"description\":\"Enable JSONL stream parser and event log written to <log>.stream.jsonl (automatically enabled when agent_type is detected or set). View via GUI 'LLM JSON Stream Events' or hancho-cli worker stream <id>.\"},"
                << "\"supervisor_notes\":{\"type\":\"string\",\"description\":\"Notes or context provided by the supervisor LLM for this worker agent\"},"
                << "\"supervisor_goal\":{\"type\":\"string\",\"description\":\"High-level goal or instruction provided by the supervisor LLM for this worker agent\"},"
                << "\"wait\":{\"type\":\"boolean\",\"description\":\"Wait until the process exits before returning (blocking completion notification)\"},"
                << "\"wait_ms\":{\"type\":\"integer\",\"description\":\"With wait=true: maximum wait in ms (0 = unlimited). With wait=false: initial output wait. Default: 500.\"}"
                << "},\"required\":[\"cmd\"]}"
                << "},"
                << "{"
                << "\"name\":\"run_worker\","
                << "\"description\":\"Delegate one task to a worker LLM CLI: start it, block until it exits, and return only its final result, outcome and token totals. Nothing else enters your context; the raw event stream stays in stream_file. Use stream-json output for the worker (e.g. claude -p ... --output-format stream-json --verbose); other output falls back to the last lines of output. Stdin is closed. If wait_ms elapses the worker keeps running: continue with wait_worker.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"cmd\":{\"type\":\"string\",\"description\":\"Worker command line, one command only\"},"
                << "\"name\":{\"type\":\"string\",\"description\":\"Friendly display label for the dashboard\"},"
                << "\"cwd\":{\"type\":\"string\",\"description\":\"Working directory path (default: current directory)\"},"
                << "\"agent_type\":{\"type\":\"string\",\"description\":\"Stream format: 'claude', 'agy', 'opencode', or 'auto' (default: detected from the command)\"},"
                << "\"timeout_sec\":{\"type\":\"integer\",\"description\":\"Max runtime timeout in seconds (0 = unlimited)\"},"
                << "\"idle_timeout_sec\":{\"type\":\"integer\",\"description\":\"Auto-kill if no output for N seconds (0 = unlimited)\"},"
                << "\"supervisor_goal\":{\"type\":\"string\",\"description\":\"High-level goal shown on the dashboard\"},"
                << "\"supervisor_notes\":{\"type\":\"string\",\"description\":\"Notes shown on the dashboard\"},"
                << "\"wait_ms\":{\"type\":\"integer\",\"description\":\"Max wait in ms (default 0 = until the worker exits)\"}"
                << "},\"required\":[\"cmd\"]}"
                << "},"
                << "{"
                << "\"name\":\"wait_worker\","
                << "\"description\":\"Keep waiting for a worker started by run_worker, and return the same final result. While it is still running, returns finished=false with its current activity.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"integer\",\"description\":\"Managed process ID\"},"
                << "\"wait_ms\":{\"type\":\"integer\",\"description\":\"Max wait in ms (default 0 = until the worker exits)\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"get_worker_stream\","
                << "\"description\":\"Read structured JSON streaming events (tools called, arguments, reasoning/thoughts, assistant responses, status) emitted by Claude, agy, or OpenCode worker agents. Events are automatically captured and saved to logs/proc_<id>_<name>_stream.jsonl. Pass schema='v1' for the stable hancho.event/1 shape.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"integer\",\"description\":\"Managed process ID of the agent\"},"
                << "\"tail\":{\"type\":\"integer\",\"description\":\"Number of recent events to return (default 50, 0 = all)\"},"
                << "\"type\":{\"type\":\"string\",\"description\":\"Filter by event type in the legacy shape, or by kind when schema='v1' (e.g. 'tool.call', 'message')\"},"
                << "\"since_seq\":{\"type\":\"integer\",\"description\":\"Return only events with sequence number greater than since_seq\"},"
                << "\"format\":{\"type\":\"string\",\"enum\":[\"json\",\"summary\"],\"description\":\"Return format: 'json' (structured events array, default) or 'summary' (compact text summary)\"},"
                << "\"schema\":{\"type\":\"string\",\"enum\":[\"legacy\",\"v1\"],\"description\":\"Event shape: 'legacy' (default, internal type strings) or 'v1' (versioned hancho.event/1 from .events.jsonl)\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"get_worker_state\","
                << "\"description\":\"Get current live state of an LLM agent process, including active tool, current tool action, thinking status, last thoughts, assistant message, token usage, supervisor notes and goal.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"integer\",\"description\":\"Managed process ID of the agent\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"update_worker_info\","
                << "\"description\":\"Update supervisor instructions, notes, or goals for an ongoing or queued LLM worker agent process.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"integer\",\"description\":\"Managed process ID of the agent\"},"
                << "\"supervisor_notes\":{\"type\":\"string\",\"description\":\"Supervisor notes/context for the worker agent\"},"
                << "\"supervisor_goal\":{\"type\":\"string\",\"description\":\"Supervisor goal for the worker agent\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"list_processes\","
                << "\"description\":\"List all managed processes with their status, PID, process tree count, memory usage, elapsed/idle time, and latest output line.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"integer\",\"description\":\"Optional specific process ID to query\"}"
                << "}}"
                << "},"
                << "{"
                << "\"name\":\"get_process_output\","
                << "\"description\":\"Read stdout/stderr or ConPTY terminal output of a managed process.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"integer\",\"description\":\"Managed process ID\"},"
                << "\"tail\":{\"type\":\"integer\",\"description\":\"Number of trailing lines to fetch (0 = all)\"},"
                << "\"wait_ms\":{\"type\":\"integer\",\"description\":\"Optional milliseconds to wait before reading\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"send_process_input\","
                << "\"description\":\"Send text input to a running process's stdin or ConPTY session.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"integer\",\"description\":\"Managed process ID\"},"
                << "\"input\":{\"type\":\"string\",\"description\":\"Text to send\"},"
                << "\"newline\":{\"type\":\"boolean\",\"description\":\"Append newline (default true)\"},"
                << "\"wait_ms\":{\"type\":\"integer\",\"description\":\"Milliseconds to wait for response output after sending (default 500)\"}"
                << "},\"required\":[\"id\",\"input\"]}"
                << "},"
                << "{"
                << "\"name\":\"kill_process\","
                << "\"description\":\"Forcefully terminate a managed process and all of its child/grandchild processes via Win32 Job Object.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"integer\",\"description\":\"Managed process ID (or 0 to kill all running processes)\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"wait_process\","
                << "\"description\":\"Wait for a managed process to exit (up to wait_ms) and return its final status and output.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"integer\",\"description\":\"Managed process ID\"},"
                << "\"wait_ms\":{\"type\":\"integer\",\"description\":\"Max milliseconds to wait (default 30000)\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_add\","
                << "\"description\":\"Register one or more jobs in a task queue. Hancho decides execution order, timeouts, success checks and automatic retries. Each job must have a one-line summary and its own timeoutSec. Use dependsOn to chain jobs instead of shell '&&'.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"taskqueue\":{\"type\":\"string\",\"description\":\"Task queue name, e.g. 'main'\"},"
                << "\"jobs\":{\"type\":\"array\",\"description\":\"Job objects: id, command, summary, timeoutSec (required); cwd, env, dependsOn (['id'] or [{id,on}]), idle {outputSec,cpuSec}, maxAttempts, retryDelaySec, success {exitCode,filesNewer,outputRegex}, killExclude, resources {write:[...],read:[...]} (paths/globs relative to cwd or @name locks; a bare file name such as Foo.cs means **/Foo.cs; jobs with overlapping write/write or write/read never run together, read/read may), defer {exitCodes,delaySec,limit} (default [75],60,60: an attempt exiting with one of exitCodes means nothing ran; the job is re-run after delaySec without using up maxAttempts; {exitCodes:[]} turns it off), priority, instructionsFile or instructionsText\",\"items\":{\"type\":\"object\"}},"
                << "\"concurrency\":{\"type\":\"integer\",\"description\":\"Max jobs running at the same time for this queue (default 1)\"},"
                << "\"notifyFile\":{\"type\":\"string\",\"description\":\"Optional file updated on jobFailed / taskqueueStalled / taskqueueDrained\"},"
                << "\"originLabel\":{\"type\":\"string\",\"description\":\"Optional short name for the work these jobs belong to (e.g. 'release v2.9.2'), shown in the dashboard's Origin column. Your session id and working folder are recorded automatically.\"}"
                << "},\"required\":[\"taskqueue\",\"jobs\"]}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_list\","
                << "\"description\":\"List task queue jobs with state, attempts, last reason and blockedBy.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"taskqueue\":{\"type\":\"string\",\"description\":\"Optional task queue name (all queues when omitted)\"},"
                << "\"state\":{\"type\":\"string\",\"description\":\"Optional state filter: pending, running, succeeded, failed, blocked, cancelled\"},"
                << "\"since\":{\"type\":\"string\",\"description\":\"Optional time window: only jobs updated within this period, e.g. 30m, 6h, 2d\"},"
                << "\"origin\":{\"type\":\"string\",\"description\":\"Optional: 'self' for only the jobs your session added, or an origin label or session id (prefix)\"}"
                << "}}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_get\","
                << "\"description\":\"Get a job with attempt history, instructions text, and sourceChanged/sourceMissing flags.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"string\",\"description\":\"Job id\"},"
                << "\"includeInstructions\":{\"type\":\"boolean\",\"description\":\"Include instructions text (default true)\"},"
                << "\"rev\":{\"type\":\"integer\",\"description\":\"Optional instructions revision to return\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_retry\","
                << "\"description\":\"Move a failed/blocked/cancelled job back to pending, optionally refreshing its instructions from the source file. Blocked dependents become pending automatically. With failed=true and no id, retries every failed job (optionally limited by taskqueue and since).\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"string\",\"description\":\"Job id (omit for bulk retry with failed=true)\"},"
                << "\"failed\":{\"type\":\"boolean\",\"description\":\"Bulk mode: retry failed jobs instead of a single id\"},"
                << "\"taskqueue\":{\"type\":\"string\",\"description\":\"Bulk mode: limit to this task queue\"},"
                << "\"since\":{\"type\":\"string\",\"description\":\"Bulk mode: only jobs that failed within this period, e.g. 30m, 6h, 2d\"},"
                << "\"resetAttempts\":{\"type\":\"boolean\",\"description\":\"Reset the attempt counter (default true)\"},"
                << "\"refreshInstructions\":{\"type\":\"boolean\",\"description\":\"Reload instructions from the source file (default true)\"}"
                << "}}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_update\","
                << "\"description\":\"Update a pending/failed/blocked job (summary, command, cwd, timeoutSec, idle, maxAttempts, retryDelaySec, defer, success, env, killExclude, resources, priority, instructions). 'notes' may also be written after a job finishes (review result). Dependencies are changed with taskqueue_link/unlink.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"string\",\"description\":\"Job id\"},"
                << "\"summary\":{\"type\":\"string\"},"
                << "\"command\":{\"type\":\"string\"},"
                << "\"cwd\":{\"type\":\"string\"},"
                << "\"timeoutSec\":{\"type\":\"integer\"},"
                << "\"idle\":{\"type\":\"object\"},"
                << "\"maxAttempts\":{\"type\":\"integer\"},"
                << "\"retryDelaySec\":{\"type\":\"integer\"},"
                << "\"defer\":{\"type\":\"object\",\"description\":\"{exitCodes:[...],delaySec,limit}; replaces the deferral setting\"},"
                << "\"success\":{\"type\":\"object\"},"
                << "\"env\":{\"type\":\"object\"},"
                << "\"killExclude\":{\"type\":\"array\",\"items\":{\"type\":\"string\"}},"
                << "\"resources\":{\"type\":\"object\",\"description\":\"{write:[...],read:[...]}; replaces the declared resources\"},"
                << "\"priority\":{\"type\":\"integer\",\"description\":\"Higher starts first among ready jobs\"},"
                << "\"instructionsFile\":{\"type\":\"string\"},"
                << "\"instructionsText\":{\"type\":\"string\"},"
                << "\"notes\":{\"type\":\"string\",\"description\":\"Review result / notes, writable even after the job finishes\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_cancel\","
                << "\"description\":\"Cancel a job. A running attempt is killed and is not retried.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"string\",\"description\":\"Job id\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_pause\","
                << "\"description\":\"Stop starting new jobs in a task queue (running jobs continue).\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"taskqueue\":{\"type\":\"string\"}"
                << "},\"required\":[\"taskqueue\"]}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_resume\","
                << "\"description\":\"Resume a paused task queue.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"taskqueue\":{\"type\":\"string\"}"
                << "},\"required\":[\"taskqueue\"]}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_wait\","
                << "\"description\":\"Wait until a task queue version changes (long poll) and return its jobs.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"taskqueue\":{\"type\":\"string\"},"
                << "\"sinceVersion\":{\"type\":\"integer\",\"description\":\"Version last seen by the caller\"},"
                << "\"timeoutMs\":{\"type\":\"integer\",\"description\":\"Max wait in milliseconds (default 30000)\"}"
                << "},\"required\":[\"taskqueue\"]}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_purge\","
                << "\"description\":\"Remove finished jobs from a task queue.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"taskqueue\":{\"type\":\"string\"},"
                << "\"states\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"description\":\"States to purge (default succeeded, failed, cancelled)\"}"
                << "},\"required\":[\"taskqueue\"]}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_link\","
                << "\"description\":\"Add one dependency to an existing pending/blocked/failed job.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"job\":{\"type\":\"string\",\"description\":\"Job that gains the dependency\"},"
                << "\"dependsOn\":{\"type\":\"string\",\"description\":\"Dependency job id\"},"
                << "\"on\":{\"type\":\"string\",\"enum\":[\"success\",\"done\"],\"description\":\"Start condition (default success)\"}"
                << "},\"required\":[\"job\",\"dependsOn\"]}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_unlink\","
                << "\"description\":\"Remove one dependency from an existing pending/blocked/failed job.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"job\":{\"type\":\"string\"},"
                << "\"dependsOn\":{\"type\":\"string\"}"
                << "},\"required\":[\"job\",\"dependsOn\"]}"
                << "},"
                << "{"
                << "\"name\":\"taskqueue_graph\","
                << "\"description\":\"Return the dependency graph with each job's state, ready/waiting/blocked status and dependents.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"taskqueue\":{\"type\":\"string\",\"description\":\"Optional task queue filter\"},"
                << "\"root\":{\"type\":\"string\",\"description\":\"Optional job id: return only its upstream/downstream component\"}"
                << "}}"
                << "},"
                << "{"
                << "\"name\":\"bt_validate\","
                << "\"description\":\"Validate a behavior tree JSON definition without running it.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"bt\":{\"type\":\"object\",\"description\":\"Behavior tree definition object\"},"
                << "\"file\":{\"type\":\"string\",\"description\":\"Path to a behavior tree JSON file\"}"
                << "}}"
                << "},"
                << "{"
                << "\"name\":\"bt_start\","
                << "\"description\":\"Register and start a behavior tree for autonomous multi-step execution (branching, retries, workers). Monitor with bt_wait.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"bt\":{\"type\":\"object\",\"description\":\"Behavior tree definition object (id, summary, intent, cwd, vars, limits, worker, root)\"},"
                << "\"file\":{\"type\":\"string\",\"description\":\"Path to a behavior tree JSON file\"},"
                << "\"originLabel\":{\"type\":\"string\",\"description\":\"Optional short name shown as the Origin of the jobs this tree adds\"}"
                << "}}"
                << "},"
                << "{"
                << "\"name\":\"bt_get\","
                << "\"description\":\"Get behavior tree state and variables. Set compact=true for lightweight summary.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"string\",\"description\":\"Behavior tree id\"},"
                << "\"compact\":{\"type\":\"boolean\",\"description\":\"Return compact tree and one-line-per-node text (default false)\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"bt_list\","
                << "\"description\":\"List registered behavior trees with state, version, and nodeRuns.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{}}"
                << "},"
                << "{"
                << "\"name\":\"bt_wait\","
                << "\"description\":\"Wait (long poll) until a behavior tree finishes or reaches waitingForLlm/paused. Set compact=true to omit the heavy tree JSON and save tokens.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"string\",\"description\":\"Behavior tree id\"},"
                << "\"sinceVersion\":{\"type\":\"integer\",\"description\":\"Version last seen by caller (default 0)\"},"
                << "\"timeoutMs\":{\"type\":\"integer\",\"description\":\"Max wait in milliseconds (default 600000 = 10 min; negative = no limit)\"},"
                << "\"compact\":{\"type\":\"boolean\",\"description\":\"If true, omit the full tree JSON object to save tokens (returns status, compact summary, and any escalation packet; default false)\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"bt_resume\","
                << "\"description\":\"Answer an active escalate node (stored in ${<node>.answer}) and resume the tree branch. Requires answer and rationale.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"string\",\"description\":\"Behavior tree id\"},"
                << "\"node\":{\"type\":\"string\",\"description\":\"Escalate node id or path\"},"
                << "\"answer\":{\"type\":\"string\",\"description\":\"Decision answer (must match expect if specified)\"},"
                << "\"rationale\":{\"type\":\"string\",\"description\":\"Required rationale recorded in escalation status\"},"
                << "\"vars\":{\"type\":\"object\",\"description\":\"Optional extra variables to set before resuming\"}"
                << "},\"required\":[\"id\",\"node\",\"answer\",\"rationale\"]}"
                << "},"
                << "{"
                << "\"name\":\"bt_retry\","
                << "\"description\":\"Retry a failed/cancelled behavior tree from the failed leaf or a specified node.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"string\",\"description\":\"Behavior tree id\"},"
                << "\"from\":{\"type\":\"string\",\"description\":\"Optional node id/path to retry from\"},"
                << "\"resetCounters\":{\"type\":\"boolean\",\"description\":\"Reset retry/repeat counters (default false)\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"bt_update\","
                << "\"description\":\"Replace the definition of non-running nodes in a behavior tree without changing tree structure.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"string\",\"description\":\"Behavior tree id\"},"
                << "\"node\":{\"type\":\"string\",\"description\":\"Optional target node id or path\"},"
                << "\"bt\":{\"type\":\"object\",\"description\":\"Replacement tree or node object\"},"
                << "\"patch\":{\"type\":\"object\",\"description\":\"Node field patch object\"},"
                << "\"file\":{\"type\":\"string\",\"description\":\"Path to replacement JSON file\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"bt_cancel\","
                << "\"description\":\"Cancel a behavior tree and kill any running jobs it launched.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"string\",\"description\":\"Behavior tree id\"}"
                << "},\"required\":[\"id\"]}"
                << "},"
                << "{"
                << "\"name\":\"bt_pause\","
                << "\"description\":\"Pause or unpause a behavior tree and its underlying task queue.\","
                << "\"inputSchema\":{\"type\":\"object\",\"properties\":{"
                << "\"id\":{\"type\":\"string\",\"description\":\"Behavior tree id\"},"
                << "\"paused\":{\"type\":\"boolean\",\"description\":\"true to pause, false to resume (default true)\"}"
                << "},\"required\":[\"id\"]}"
                << "}"
                << "]}}";
            EmitLine(oss.str());
        }
        else if (method == "tools/call") {
            const std::string& toolName = req.toolName;
            const std::string& argsJson = req.argumentsJson;
            WriteAuditLog("MCP_TOOL_CALL", "tool=" + toolName + " args=" + argsJson);
            std::ostringstream ipcReq;

            if (toolName == "start_process") {
                std::string cmd = JsonGetString(argsJson, "cmd", "");
                std::string name = JsonGetString(argsJson, "name", "");
                std::string mode = JsonGetString(argsJson, "mode", "pipe");
                if (JsonGetBool(argsJson, "pty", false)) {
                    mode = "pty";
                }
                bool openStdin = JsonGetBool(argsJson, "open_stdin", true);
                std::string cwd = ResolveAbsolutePathUtf8(JsonGetString(argsJson, "cwd", ""));
                std::string logFile = JsonGetString(argsJson, "log_file", "");
                std::string doneFile = JsonGetString(argsJson, "done_file", "");
                int64_t timeoutSec = JsonGetInt(argsJson, "timeout_sec", 0);
                int64_t idleTimeoutSec = JsonGetInt(argsJson, "idle_timeout_sec", 0);
                std::string estimateEnd = JsonGetString(argsJson, "estimate_end", "");
                std::string agentType = JsonGetString(argsJson, "agent_type", "");
                bool jsonStream = JsonGetBool(argsJson, "json_stream", false);
                std::string supervisorNotes = JsonGetString(argsJson, "supervisor_notes", "");
                std::string supervisorGoal = JsonGetString(argsJson, "supervisor_goal", "");
                bool wait = JsonGetBool(argsJson, "wait", false);
                int64_t waitMs = JsonGetInt(argsJson, "wait_ms", -1);
                if (waitMs < 0) {
                    waitMs = wait ? 0 : 500;
                }

                ipcReq << "{\"action\":\"start\""
                       << ",\"cmd\":\"" << JsonEscape(cmd) << "\""
                       << ",\"name\":\"" << JsonEscape(name) << "\""
                       << ",\"mode\":\"" << JsonEscape(mode) << "\""
                       << ",\"open_stdin\":" << (openStdin ? "true" : "false")
                       << ",\"cwd\":\"" << JsonEscape(cwd) << "\""
                       << ",\"log_file\":\"" << JsonEscape(logFile) << "\""
                       << ",\"done_file\":\"" << JsonEscape(doneFile) << "\""
                       << ",\"timeout_sec\":" << timeoutSec
                       << ",\"idle_timeout_sec\":" << idleTimeoutSec
                       << ",\"estimate_end\":\"" << JsonEscape(estimateEnd) << "\""
                       << ",\"agent_type\":\"" << JsonEscape(agentType) << "\""
                       << ",\"json_stream\":" << (jsonStream ? "true" : "false")
                       << ",\"supervisor_notes\":\"" << JsonEscape(supervisorNotes) << "\""
                       << ",\"supervisor_goal\":\"" << JsonEscape(supervisorGoal) << "\""
                       << ",\"wait\":" << (wait ? "true" : "false")
                       << ",\"wait_ms\":" << waitMs << "}";
            }
            else if (toolName == "run_worker" || toolName == "run_agent") {  // run_agent: name used by v2.3.0
                ipcReq << "{\"action\":\"start\""
                       << ",\"cmd\":\"" << JsonEscape(JsonGetString(argsJson, "cmd", "")) << "\""
                       << ",\"name\":\"" << JsonEscape(JsonGetString(argsJson, "name", "")) << "\""
                       << ",\"mode\":\"pipe\",\"open_stdin\":false,\"json_stream\":true"
                       << ",\"cwd\":\"" << JsonEscape(ResolveAbsolutePathUtf8(JsonGetString(argsJson, "cwd", ""))) << "\""
                       << ",\"agent_type\":\"" << JsonEscape(JsonGetString(argsJson, "agent_type", "")) << "\""
                       << ",\"timeout_sec\":" << JsonGetInt(argsJson, "timeout_sec", 0)
                       << ",\"idle_timeout_sec\":" << JsonGetInt(argsJson, "idle_timeout_sec", 0)
                       << ",\"supervisor_notes\":\"" << JsonEscape(JsonGetString(argsJson, "supervisor_notes", "")) << "\""
                       << ",\"supervisor_goal\":\"" << JsonEscape(JsonGetString(argsJson, "supervisor_goal", "")) << "\""
                       << ",\"wait\":true,\"result_only\":true"
                       << ",\"wait_ms\":" << JsonGetInt(argsJson, "wait_ms", 0) << "}";
            }
            else if (toolName == "wait_worker" || toolName == "wait_agent") {  // wait_agent: name used by v2.3.0
                ipcReq << "{\"action\":\"wait\",\"result_only\":true"
                       << ",\"id\":" << JsonGetInt(argsJson, "id", 0)
                       << ",\"wait_ms\":" << JsonGetInt(argsJson, "wait_ms", 0) << "}";
            }
            else if (toolName == "get_worker_stream" || toolName == "get_agent_stream") {
                int64_t id = JsonGetInt(argsJson, "id", 0);
                int64_t tail = JsonGetInt(argsJson, "tail", 50);
                std::string typeFilter = JsonGetString(argsJson, "type", "");
                if (typeFilter.empty()) typeFilter = JsonGetString(argsJson, "filter", "");
                int64_t sinceSeq = JsonGetInt(argsJson, "since_seq", 0);
                std::string format = JsonGetString(argsJson, "format", "json");
                std::string schema = JsonGetString(argsJson, "schema", "");
                ipcReq << "{\"action\":\"get_agent_stream\",\"id\":" << id
                       << ",\"tail\":" << tail
                       << ",\"type\":\"" << JsonEscape(typeFilter) << "\""
                       << ",\"since_seq\":" << sinceSeq
                       << ",\"format\":\"" << JsonEscape(format) << "\""
                       << ",\"schema\":\"" << JsonEscape(schema) << "\"}";
            }
            else if (toolName == "get_worker_state" || toolName == "get_agent_state") {
                int64_t id = JsonGetInt(argsJson, "id", 0);
                ipcReq << "{\"action\":\"get_agent_state\",\"id\":" << id << "}";
            }
            else if (toolName == "update_worker_info" || toolName == "update_agent_info") {
                int64_t id = JsonGetInt(argsJson, "id", 0);
                std::string notes = JsonGetString(argsJson, "supervisor_notes", "");
                if (notes.empty()) notes = JsonGetString(argsJson, "notes", "");
                std::string goal = JsonGetString(argsJson, "supervisor_goal", "");
                if (goal.empty()) goal = JsonGetString(argsJson, "goal", "");
                ipcReq << "{\"action\":\"update_agent_info\",\"id\":" << id
                       << ",\"supervisor_notes\":\"" << JsonEscape(notes) << "\""
                       << ",\"supervisor_goal\":\"" << JsonEscape(goal) << "\"}";
            }
            else if (toolName == "list_processes") {
                int64_t id = JsonGetInt(argsJson, "id", 0);
                ipcReq << "{\"action\":\"list\",\"id\":" << id << "}";
            }
            else if (toolName == "get_process_output") {
                int64_t id = JsonGetInt(argsJson, "id", 0);
                int64_t tail = JsonGetInt(argsJson, "tail", 100);
                int64_t waitMs = JsonGetInt(argsJson, "wait_ms", 0);
                ipcReq << "{\"action\":\"logs\",\"id\":" << id << ",\"tail\":" << tail << ",\"wait_ms\":" << waitMs << "}";
            }
            else if (toolName == "send_process_input") {
                int64_t id = JsonGetInt(argsJson, "id", 0);
                std::string input = JsonGetString(argsJson, "input", "");
                bool newline = JsonGetBool(argsJson, "newline", true);
                int64_t waitMs = JsonGetInt(argsJson, "wait_ms", 500);
                ipcReq << "{\"action\":\"send\",\"id\":" << id
                       << ",\"input\":\"" << JsonEscape(input) << "\""
                       << ",\"newline\":" << (newline ? "true" : "false")
                       << ",\"wait_ms\":" << waitMs << "}";
            }
            else if (toolName == "kill_process") {
                int64_t id = JsonGetInt(argsJson, "id", 0);
                if (id <= 0) {
                    ipcReq << "{\"action\":\"kill_all\"}";
                } else {
                    ipcReq << "{\"action\":\"kill\",\"id\":" << id << "}";
                }
            }
            else if (toolName == "wait_process") {
                int64_t id = JsonGetInt(argsJson, "id", 0);
                int64_t waitMs = JsonGetInt(argsJson, "wait_ms", 30000);
                ipcReq << "{\"action\":\"wait\",\"id\":" << id << ",\"wait_ms\":" << waitMs << "}";
            }
            else if (toolName == "taskqueue_add") {
                std::string tqName = JsonGetString(argsJson, "taskqueue", "");
                std::string jobsRaw = ExtractRawJsonValue(argsJson, "jobs");
                std::string notifyFile = JsonGetString(argsJson, "notifyFile", "");
                if (notifyFile.empty()) notifyFile = JsonGetString(argsJson, "notify_file", "");
                ipcReq << "{\"action\":\"taskqueue_add\",\"taskqueue\":\"" << JsonEscape(tqName) << "\""
                       << ",\"jobs\":" << (jobsRaw.empty() ? "[]" : jobsRaw)
                       << ",\"concurrency\":" << JsonGetInt(argsJson, "concurrency", 0)
                       << ",\"notify_file\":\"" << JsonEscape(notifyFile) << "\""
                       << ",\"origin\":" << BuildCallerOriginJson(mcpClient, JsonGetString(argsJson, "originLabel", "")) << "}";
            }
            else if (toolName == "taskqueue_list") {
                ipcReq << "{\"action\":\"taskqueue_list\""
                       << ",\"taskqueue\":\"" << JsonEscape(JsonGetString(argsJson, "taskqueue", "")) << "\""
                       << ",\"state\":\"" << JsonEscape(JsonGetString(argsJson, "state", "")) << "\""
                       << ",\"since\":\"" << JsonEscape(JsonGetString(argsJson, "since", "")) << "\"";
                std::string originFilter = JsonGetString(argsJson, "origin", "");
                if (originFilter == "self") originFilter = CallerSelfOriginFilter();
                ipcReq << ",\"origin\":\"" << JsonEscape(originFilter) << "\"}";
            }
            else if (toolName == "taskqueue_get") {
                bool includeInstr = JsonGetBool(argsJson, "includeInstructions",
                                                JsonGetBool(argsJson, "include_instructions", true));
                ipcReq << "{\"action\":\"taskqueue_get\""
                       << ",\"id\":\"" << JsonEscape(JsonGetString(argsJson, "id", "")) << "\""
                       << ",\"include_instructions\":" << (includeInstr ? "true" : "false")
                       << ",\"rev\":" << JsonGetInt(argsJson, "rev", 0) << "}";
            }
            else if (toolName == "taskqueue_retry") {
                bool resetAttempts = JsonGetBool(argsJson, "resetAttempts",
                                                 JsonGetBool(argsJson, "reset_attempts", true));
                bool refreshInstr = JsonGetBool(argsJson, "refreshInstructions",
                                                JsonGetBool(argsJson, "refresh_instructions", true));
                ipcReq << "{\"action\":\"taskqueue_retry\""
                       << ",\"id\":\"" << JsonEscape(JsonGetString(argsJson, "id", "")) << "\""
                       << ",\"failed\":" << (JsonGetBool(argsJson, "failed", false) ? "true" : "false")
                       << ",\"taskqueue\":\"" << JsonEscape(JsonGetString(argsJson, "taskqueue", "")) << "\""
                       << ",\"since\":\"" << JsonEscape(JsonGetString(argsJson, "since", "")) << "\""
                       << ",\"reset_attempts\":" << (resetAttempts ? "true" : "false")
                       << ",\"refresh_instructions\":" << (refreshInstr ? "true" : "false") << "}";
            }
            else if (toolName == "taskqueue_update") {
                ipcReq << "{\"action\":\"taskqueue_update\""
                       << ",\"id\":\"" << JsonEscape(JsonGetString(argsJson, "id", "")) << "\""
                       << ",\"patch\":" << argsJson << "}";
            }
            else if (toolName == "taskqueue_cancel") {
                ipcReq << "{\"action\":\"taskqueue_cancel\",\"id\":\""
                       << JsonEscape(JsonGetString(argsJson, "id", "")) << "\"}";
            }
            else if (toolName == "taskqueue_pause" || toolName == "taskqueue_resume") {
                ipcReq << "{\"action\":\"" << toolName << "\",\"taskqueue\":\""
                       << JsonEscape(JsonGetString(argsJson, "taskqueue", "")) << "\"}";
            }
            else if (toolName == "taskqueue_wait") {
                int64_t sinceVersion = JsonGetInt(argsJson, "sinceVersion",
                                                  JsonGetInt(argsJson, "since_version", 0));
                int64_t timeoutMs = JsonGetInt(argsJson, "timeoutMs",
                                               JsonGetInt(argsJson, "timeout_ms", 30000));
                ipcReq << "{\"action\":\"taskqueue_wait\""
                       << ",\"taskqueue\":\"" << JsonEscape(JsonGetString(argsJson, "taskqueue", "")) << "\""
                       << ",\"since_version\":" << sinceVersion
                       << ",\"timeout_ms\":" << timeoutMs << "}";
            }
            else if (toolName == "taskqueue_purge") {
                std::string statesRaw = ExtractRawJsonValue(argsJson, "states");
                ipcReq << "{\"action\":\"taskqueue_purge\""
                       << ",\"taskqueue\":\"" << JsonEscape(JsonGetString(argsJson, "taskqueue", "")) << "\""
                       << ",\"states\":" << (statesRaw.empty() ? "[]" : statesRaw) << "}";
            }
            else if (toolName == "taskqueue_link") {
                std::string dep = JsonGetString(argsJson, "dependsOn", "");
                if (dep.empty()) dep = JsonGetString(argsJson, "depends_on", "");
                ipcReq << "{\"action\":\"taskqueue_link\""
                       << ",\"job\":\"" << JsonEscape(JsonGetString(argsJson, "job", "")) << "\""
                       << ",\"depends_on\":\"" << JsonEscape(dep) << "\""
                       << ",\"on\":\"" << JsonEscape(JsonGetString(argsJson, "on", "")) << "\"}";
            }
            else if (toolName == "taskqueue_unlink") {
                std::string dep = JsonGetString(argsJson, "dependsOn", "");
                if (dep.empty()) dep = JsonGetString(argsJson, "depends_on", "");
                ipcReq << "{\"action\":\"taskqueue_unlink\""
                       << ",\"job\":\"" << JsonEscape(JsonGetString(argsJson, "job", "")) << "\""
                       << ",\"depends_on\":\"" << JsonEscape(dep) << "\"}";
            }
            else if (toolName == "taskqueue_graph") {
                ipcReq << "{\"action\":\"taskqueue_graph\""
                       << ",\"taskqueue\":\"" << JsonEscape(JsonGetString(argsJson, "taskqueue", "")) << "\""
                       << ",\"root\":\"" << JsonEscape(JsonGetString(argsJson, "root", "")) << "\"}";
            }
            else if (toolName.rfind("bt_", 0) == 0) {
                auto argsObj = nlohmann::json::parse(argsJson, nullptr, false);
                if (argsObj.is_discarded() || !argsObj.is_object()) {
                    argsObj = nlohmann::json::object();
                }
                argsObj["action"] = toolName;
                if (toolName == "bt_start") {
                    std::string label = argsObj.value("originLabel", std::string());
                    argsObj.erase("originLabel");
                    argsObj["origin"] = nlohmann::json::parse(BuildCallerOriginJson(mcpClient, label), nullptr, false);
                }
                if (argsObj.contains("file") && argsObj["file"].is_string()) {
                    std::string fileArg = argsObj["file"].get<std::string>();
                    std::wstring absW = ResolveAbsolutePathWide(Utf8ToWide(fileArg));
                    argsObj["file"] = WideToUtf8(absW);
                    if (!argsObj.contains("bt") && !argsObj.contains("patch")) {
                        std::ifstream ifs(absW, std::ios::binary);
                        if (!ifs) {
                            EmitLine(BuildMcpToolResult(
                                rpcId, "{\"ok\":false,\"error\":\"cannot open file: " + JsonEscape(fileArg) + "\"}", true));
                            continue;
                        }
                        std::ostringstream fss;
                        fss << ifs.rdbuf();
                        argsObj["bt"] = fss.str();
                    }
                }
                if (toolName == "bt_wait") {
                    // Long waits run off the read loop so that notifications/cancelled
                    // (and other calls) are still read while this one is waiting.
                    waits.Start(rpcId, argsObj);
                    continue;
                }
                ipcReq << argsObj.dump();
            }
            else {
                EmitLine(BuildMcpToolResult(rpcId, "Unknown tool: " + toolName, true));
                continue;
            }

            std::string ipcResp, err;
            if (!SendIpcRequest(ipcReq.str(), ipcResp, err, true)) {
                WriteAuditLog("MCP_TOOL_ERR", "tool=" + toolName + " error=" + err);
                EmitLine(BuildMcpToolResult(rpcId, "IPC error: " + err, true));
            } else {
                bool ok = JsonGetBool(ipcResp, "ok", false);
                WriteAuditLog("MCP_TOOL_RESP", "tool=" + toolName + " ok=" + (ok ? "true" : "false") + " bytes=" + std::to_string(ipcResp.size()));
                EmitLine(BuildMcpToolResult(rpcId, ipcResp, !ok));
            }
        }
        else {
            EmitLine("{\"jsonrpc\":\"2.0\",\"id\":" + rpcId +
                     ",\"error\":{\"code\":-32601,\"message\":\"Method not found\"}}");
        }
    }
    waits.CancelAllAndJoin();
    return 0;
}

} // namespace pc
