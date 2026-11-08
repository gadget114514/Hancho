#pragma once

#include "common.h"
#include <string>
#include <vector>
#include <deque>
#include <mutex>
#include <unordered_map>
#include <cstdint>

namespace pc {

// Structured representation of a single streamed JSON event from Claude, agy, or OpenCode.
struct LlmStreamEvent {
    uint64_t seq = 0;
    std::string timestamp;
    std::string rawJson;
    std::string type;          // "tool_call", "tool_result", "thinking", "message", "status", "step", "error", "usage", "other"
    std::string agentType;     // "claude", "agy", "opencode", "generic"
    std::string model;         // model name/id if present in stream
    std::string toolName;      // e.g. "view_file", "edit_file", "Bash"
    std::string toolAction;    // e.g. "Viewing file", "Editing src/gui_dashboard.cpp"
    std::string toolInput;     // arguments or parameters
    std::string content;       // thinking reasoning or assistant message text
    std::string status;        // "thinking", "calling_tool", "generating", "idle", "completed", "error"
    std::string messageId;     // Claude message.id; usage is per message, not cumulative
    std::string sessionId;     // Claude session_id / OpenCode sessionID
    std::string stopReason;    // Claude stop_reason, OpenCode step_finish reason
    std::string cwd;           // Claude system init working directory
    std::string rateLimitType; // Claude rate_limit_event: "five_hour", "seven_day", ...
    std::string rateLimitStatus; // "allowed", "rejected", ...
    double rateLimitUtilization = -1.0; // share of that window used, 0..1; -1 if unknown
    int64_t rateLimitResetsAtSec = 0;
    uint64_t apiDurationMs = 0;   // Claude result duration_api_ms
    uint64_t numTurns = 0;        // Claude result num_turns
    uint64_t permissionDenials = 0; // Claude result permission_denials count
    uint64_t toolsOffered = 0;    // Claude system init tools count
    bool toolError = false;       // a tool_result reported an error
    bool transient = false;       // Claude partial-message chunk: updates the live state only, never stored or saved
    bool runTotal = false;        // Claude result line: its usage is the run total, not one message's
    int64_t recvEpochMs = 0;   // wall clock when the line was received (for activity durations)
    int stepIndex = 0;
    uint64_t totalTokens = 0;
    uint64_t inputTokens = 0;
    uint64_t outputTokens = 0;
    uint64_t thinkingTokens = 0;
    uint64_t cacheReadTokens = 0;
    uint64_t cacheWriteTokens = 0;
    double costUsd = 0.0;
    // Claude sub-agents (Agent tool). A sub-agent line carries parentToolUseId and agentId.
    std::string toolUseId;       // tool_use id of a tool_call, or the tool_use a tool_result answers
    std::string parentToolUseId; // tool_use id of the Agent call that a sub-agent line belongs to
    std::string agentId;         // sub-agent id (task_id), on sub-agent lines and task events
    std::string subagentType;    // e.g. "general-purpose"
    std::string taskDescription; // sub-agent description
    std::string taskStatus;      // task_notification status: "completed", "failed", "killed", ...
    uint64_t taskTokens = 0;     // task_notification usage.total_tokens
    uint64_t taskToolUses = 0;   // task_notification usage.tool_uses
    uint64_t taskDurationMs = 0; // task_notification usage.duration_ms
};

// One sub-agent of a Claude run, started by an Agent tool call (task_started) and ended by
// task_notification. Lines it emits (parent_tool_use_id set) update this entry only.
struct LlmSubagentState {
    std::string id;                  // task_id (same as agent_id)
    std::string toolUseId;           // the Agent tool_use that started it
    std::string parentAgentId;       // agent whose line held that Agent tool_use; empty for the root agent
    std::string subagentType;
    std::string description;
    std::string status = "running";  // "running", then task_notification status ("completed", ...)
    int64_t startedEpochMs = 0;
    int64_t endedEpochMs = 0;
    uint64_t totalTokens = 0;
    int toolUses = 0;
    std::string activity;            // same kinds as LlmAgentState::activity
    std::string activityTarget;
};

// Summary state of an LLM agent process
struct LlmAgentState {
    std::string agentType;      // "claude", "agy", "opencode", or empty
    std::string model;          // model name/id if present in stream
    std::string status = "idle"; // "idle", "thinking", "calling_tool", "generating", "completed", "error"
    std::string currentTool;
    std::string currentToolAction;
    std::string lastThought;
    std::string lastMessage;
    std::string lastError;
    std::string sessionId;
    std::string stopReason;
    std::string cwd;
    int toolCalls = 0;
    int toolErrors = 0;
    uint64_t toolsOffered = 0;
    uint64_t numTurns = 0;
    uint64_t apiDurationMs = 0;
    uint64_t permissionDenials = 0;
    std::string rateLimitType;
    std::string rateLimitStatus;
    double rateLimitUtilization = -1.0;
    int64_t rateLimitResetsAtSec = 0;
    // What the agent is doing now, in plain terms (see DescribeLlmActivity).
    std::string activity;          // "read", "edit", "search", "command", "web", "subagent", "plan", "tool",
                                   // "thinking", "replying", "waiting_model", "done", "error", or empty
    std::string activityTarget;    // file, command, pattern, URL, ...
    int64_t activitySinceEpochMs = 0;
    std::vector<LlmSubagentState> subagents;  // Claude sub-agents, oldest first
    int stepCount = 0;
    int eventCount = 0;
    uint64_t totalTokens = 0;
    double costUsd = 0.0;
    std::string streamFilePath;
    std::string supervisorNotes;
    std::string supervisorGoal;
    std::string updatedAt;
};

// Auto-detect agent type ("claude", "agy", "opencode") from command line or process name.
std::string DetectAgentType(const std::string& commandLine, const std::string& name = "");

// Parse a single raw JSON line emitted by claude, agy, or opencode.
// Returns true if successfully parsed as a JSON event.
bool ParseLlmStreamLine(const std::string& line, const std::string& defaultAgentType, LlmStreamEvent& outEvent);

// Parse a raw JSON line into events. Per agent, the line is split into events by its own format:
//  - "claude": "assistant" / "user" lines carry several content blocks, one event per block.
//  - "opencode": one tool_use line gives a tool_call and, once finished, a tool_result.
// All other lines and agents yield at most one event via ParseLlmStreamLine.
// Returns false if the line is not a JSON object.
bool ParseLlmStreamLines(const std::string& line, const std::string& defaultAgentType, std::vector<LlmStreamEvent>& outEvents);

// Format events as JSON array or human-readable summary
std::string LlmEventsToJson(const std::vector<LlmStreamEvent>& events, const LlmAgentState& state);
// The sub-agent list as a JSON array (used by LlmEventsToJson and the IPC agent state).
std::string LlmSubagentsToJson(const std::vector<LlmSubagentState>& subagents);
std::string LlmEventsToSummary(const std::vector<LlmStreamEvent>& events, const LlmAgentState& state);

// Classify an event into a plain activity kind (see LlmAgentState::activity) and its target
// (file path, command line, search pattern, URL, ...). Returns false for events that do not
// start an activity (usage, status, step, other).
bool DescribeLlmActivity(const LlmStreamEvent& ev, std::string& kind, std::string& target);

// The "description" the agent gave for a tool call, e.g. why a Bash command runs. Empty when none,
// and for subagents (their target already is the description).
std::string DescribeLlmNote(const LlmStreamEvent& ev);

// Human-readable usage summary (tokens / cache / cost) for one event. Empty if no usage data.
std::string LlmEventUsageText(const LlmStreamEvent& ev);

// Process-wide token usage history used for dashboard graphs.
struct LlmUsagePoint {
    int64_t epochSec = 0;
    uint64_t inputTokens = 0;
    uint64_t outputTokens = 0;
    uint64_t thinkingTokens = 0;
    uint64_t cacheReadTokens = 0;
    uint64_t cacheWriteTokens = 0;
    uint64_t totalTokens = 0;
    double costUsd = 0.0;
    std::string agentType;
    std::string model;
};

void RecordLlmUsagePoint(int64_t epochSec, const LlmUsagePoint& point);
std::vector<LlmUsagePoint> GetLlmUsageHistory(int64_t sinceEpochSec);
void SetUsageHistoryPersistence(bool enabled);
bool IsUsageHistoryPersistence();

// Events kept per stream tracker. -1 keeps them all (until memory runs out); otherwise the value
// is kept within [kMinLlmMaxEvents, kMaxLlmMaxEvents], and any other value (0, other negatives)
// becomes kMinLlmMaxEvents. Applies to every running process at its next event.
constexpr int64_t kDefaultLlmMaxEvents = 2000;
constexpr int64_t kMinLlmMaxEvents = 100;
constexpr int64_t kMaxLlmMaxEvents = 100000;
int64_t ClampLlmMaxEvents(int64_t requested);
void SetLlmMaxEvents(int64_t requested);
int64_t GetLlmMaxEvents();
uint64_t GetLlmUsageRecordCalls();

// Thread-safe tracker that ingests raw output chunks, extracts JSONL lines,
// persists them to logs/proc_<id>_<name>_stream.jsonl, and maintains the current state.
class LlmStreamTracker {
public:
    LlmStreamTracker() = default;

    void Init(int processId, const std::string& agentType, const std::string& streamFilePath,
              const std::string& supervisorNotes = "", const std::string& supervisorGoal = "",
              const std::string& defaultModel = "");

    // Ingests raw chunks from process stdout (pipe or ConPTY)
    void IngestChunk(const char* data, size_t len);

    // Update supervisor context
    void UpdateSupervisorInfo(const std::string& notes, const std::string& goal);

    // Query state and events
    LlmAgentState GetState() const;
    std::vector<LlmStreamEvent> GetEvents(int tail = 20, const std::string& typeFilter = "", uint64_t sinceSeq = 0) const;
    std::string GetEventsJson(int tail = 20, const std::string& typeFilter = "", uint64_t sinceSeq = 0) const;
    std::string GetEventsSummary(int tail = 20, const std::string& typeFilter = "") const;
    std::string GetEventsRawJsonl(int tail = 20) const;

    bool HasEvents() const;
    uint64_t EventCount() const;

private:
    // Token counts of one usage object, used as the baseline for the next delta.
    struct UsageBaseline {
        uint64_t total = 0;
        uint64_t input = 0;
        uint64_t output = 0;
        uint64_t thinking = 0;
        uint64_t cacheRead = 0;
        uint64_t cacheWrite = 0;
    };

    void ProcessLine(const std::string& line);
    void ApplyEvent(LlmStreamEvent& ev, const std::string& line);
    void ApplyPartialEvent(LlmStreamEvent& ev);
    void ApplyActivity(const LlmStreamEvent& ev);
    // Claude baseline for one message id (created on first use, bounded by kMaxTrackedMessages).
    UsageBaseline& MessageBaselineLocked(const std::string& messageId);
    // The sub-agent entry for an id or tool_use id, created when missing (bounded by kMaxSubagents).
    LlmSubagentState& SubagentEntryLocked(const std::string& agentId, const std::string& toolUseId, int64_t nowMs);
    bool AnySubagentRunningLocked() const;
    // Which agent issued a tool_use (its agent id, empty for the root agent). An Agent call's owner
    // is the parent of the sub-agent it starts. Bounded by kMaxToolUseOwners.
    void RecordToolUseOwnerLocked(const std::string& toolUseId, const std::string& agentId);
    std::string ParentAgentIdLocked(const std::string& toolUseId) const;

    mutable std::mutex mutex_;
    int processId_ = 0;
    std::string agentType_;
    std::string conversationId_;
    uint64_t lastAgyModelProbeMs_ = 0;
    std::vector<LlmUsagePoint> pendingUsage_;  // recorded by IngestChunk after releasing mutex_
    std::string streamFilePath_;
    std::wstring streamFilePathWide_;
    std::string lineBuffer_;
    bool discardingLine_ = false;
    std::deque<LlmStreamEvent> events_;
    LlmAgentState state_;
    uint64_t nextSeq_ = 1;
    // Claude usage is per message: each message id keeps its own baseline, so lines of other
    // messages (sub-agents) in between do not reset it and the same usage is not counted twice.
    std::unordered_map<std::string, UsageBaseline> messageBaselines_;
    std::deque<std::string> messageBaselineOrder_;  // insertion order, for the size cap
    std::unordered_map<std::string, std::string> toolUseOwners_;  // tool_use id -> issuing agent id
    std::deque<std::string> toolUseOwnerOrder_;                   // insertion order, for the size cap
    UsageBaseline lastUsage_;                       // baseline for the other agents' usage
    double lastUsageCost_ = 0.0;
    std::string partialTextType_;   // "thinking" or "message": the block whose chunks are in partialText_
    std::string partialText_;       // head of the streamed text of the current block (only the head is shown)
    // Claude usage already counted, per message, compared with the run total of the result line.
    uint64_t countedInput_ = 0;
    uint64_t countedOutput_ = 0;
    uint64_t countedThinking_ = 0;
    uint64_t countedCacheRead_ = 0;
    uint64_t countedCacheWrite_ = 0;
    static constexpr size_t kMaxPartialTextBytes = 1024;
    // A result arrived while a sub-agent was still running: the run ends with that sub-agent.
    bool resultPending_ = false;
    bool loggedAllocFailure_ = false;  // the bad_alloc note is written once per tracker
    static constexpr size_t kMaxTrackedMessages = 1024;
    // Large on purpose: a finished branch's summary row must survive in the monitor. Only past this
    // count is the oldest finished sub-agent dropped.
    static constexpr size_t kMaxSubagents = 4096;
    static constexpr size_t kMaxToolUseOwners = 4096;
    static constexpr uint64_t kAgyModelProbeIntervalMs = 5000;
    // Claude tool_result lines (file contents etc.) routinely exceed 64KB.
    static constexpr size_t kMaxLineBytes = 16 * 1024 * 1024;
};

} // namespace pc
