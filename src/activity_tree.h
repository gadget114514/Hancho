#pragma once

// Tree rows for the monitor pane: each Claude sub-agent is a branch, and the activities it
// performed are its children. Pure functions of the stream events and the sub-agent states, so
// the rules can be tested without the GUI (tests/activity_tree_smoke.cpp).

#include "llm_stream.h"
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace pc {

// A branch that completed closes this long after its end (unless it has running or failed
// descendants, or the user opened or closed it).
constexpr int64_t kActivityTreeCloseDelayMs = 3000;

struct ActivityTreeRow {
    int depth = 0;
    bool isBranch = false;     // true: a sub-agent row; false: an activity row
    std::string agentId;       // branch: the sub-agent id; activity: the agent it belongs to ("" = root)
    bool open = false;         // branch only: its children are listed below
    bool manual = false;       // branch only: the open/closed state comes from a user override
    std::string kind;          // activity: LlmAgentState::activity kind; branch: sub-agent status
    std::string target;        // activity: its target; branch: the sub-agent description
    std::string subagentType;  // branch: the sub-agent's type, e.g. "general-purpose"
    int count = 1;             // activity: consecutive steps of one kind merged into this row
    int64_t sinceEpochMs = 0;  // activity: when it started; branch: when it started
    int64_t endEpochMs = 0;    // branch: when it ended; 0 while running
    uint64_t tokens = 0;       // branch: tokens used by the sub-agent
    int toolUses = 0;          // branch: tool calls made by the sub-agent
    uint64_t seq = 0;          // activity: the event seq (0 for branches)
    uint64_t firstSeq = 0;     // activity: the event seq of its first event (stable key for the list)
    int firstEvent = -1;       // activity: index into the events vector of its first event (-1 for branches)
    int lastEvent = -1;        // activity: index of its last merged event
    std::string description;   // activity: the tool call's own description, if any
    // Filled by the monitor pane (not by BuildActivityTree): what the list shows for the row.
    bool ongoing = false;      // still running: its duration keeps growing
    uint64_t tokIn = 0;        // activity: input tokens of its events
    uint64_t tokOut = 0;       // activity: output tokens
    uint64_t tokCache = 0;     // activity: cache read + write tokens
    uint64_t tokThink = 0;     // activity: thinking tokens
};

// User choices by sub-agent id: true = opened, false = closed. An entry replaces the automatic state.
using ActivityTreeOverrides = std::map<std::string, bool>;

// Display order, top first. A running branch is open down to its leaves, and parallel running
// branches are all open. A closed branch hides everything below it. Consecutive activities of
// one kind merge (thinking and replies do not). `running` tells whether the process still runs:
// a branch left in "running" after the process ended is shown as "stopped".
//
// Without sub-agents the result is the activity rows in event order (oldest first); the caller
// keeps its own flat list for that case.
std::vector<ActivityTreeRow> BuildActivityTree(const std::vector<LlmStreamEvent>& events,
                                               const std::vector<LlmSubagentState>& subagents,
                                               int64_t nowMs, bool running,
                                               const ActivityTreeOverrides& overrides);

}  // namespace pc
