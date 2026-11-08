// Headless test for BuildActivityTree (src/activity_tree.cpp). Every time is passed in, so the
// close-after-3-seconds rule is checked at fixed instants. All events and sub-agent states below
// are SYNTHETIC: they are built in code, not captured from a Claude run.

#include "../src/activity_tree.h"
#include "../src/common.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace pc;

static int g_failures = 0;

static void Check(bool cond, const std::string& label) {
    std::printf("%s %s\n", cond ? "[PASS]" : "[FAIL]", label.c_str());
    std::fflush(stdout);
    if (!cond) g_failures++;
}

// A tool call of the given agent ("" = root). DescribeLlmActivity turns "Read" into a read activity.
static LlmStreamEvent ToolCall(uint64_t seq, const std::string& agentId, const std::string& tool,
                               const std::string& input, int64_t ms) {
    LlmStreamEvent ev;
    ev.seq = seq;
    ev.type = "tool_call";
    ev.toolName = tool;
    ev.toolInput = input;
    ev.agentId = agentId;
    ev.recvEpochMs = ms;
    return ev;
}

static LlmStreamEvent Thought(uint64_t seq, const std::string& agentId, int64_t ms) {
    LlmStreamEvent ev;
    ev.seq = seq;
    ev.type = "thinking";
    ev.content = "thinking";
    ev.agentId = agentId;
    ev.recvEpochMs = ms;
    return ev;
}

static LlmSubagentState Sub(const std::string& id, const std::string& parent, const std::string& status,
                            int64_t start, int64_t end, const std::string& description) {
    LlmSubagentState s;
    s.id = id;
    s.parentAgentId = parent;
    s.status = status;
    s.startedEpochMs = start;
    s.endedEpochMs = end;
    s.description = description;
    s.subagentType = "general-purpose";
    return s;
}

static bool HasBranch(const std::vector<ActivityTreeRow>& rows, const std::string& id, bool open) {
    for (const auto& r : rows) {
        if (r.isBranch && r.agentId == id) return r.open == open;
    }
    return false;
}

static int CountBranch(const std::vector<ActivityTreeRow>& rows, const std::string& id) {
    int n = 0;
    for (const auto& r : rows) n += (r.isBranch && r.agentId == id) ? 1 : 0;
    return n;
}

int main() {
    SetConsoleOutputCP(CP_UTF8);
    const ActivityTreeOverrides none;

    // A. A running branch is open down to its activities.
    {
        std::vector<LlmStreamEvent> ev = {ToolCall(1, "", "Read", R"({"file_path":"root.cpp"})", 900),
                                          ToolCall(2, "s1", "Read", R"({"file_path":"a.cpp"})", 1100)};
        std::vector<LlmSubagentState> subs = {Sub("s1", "", "running", 1000, 0, "explore")};
        auto rows = BuildActivityTree(ev, subs, 5000, true, none);
        // The tree shows sub-agents only: the root's read of root.cpp is left out.
        Check(rows.size() == 2, "A: running branch lists its activity, root steps left out (2 rows)");
        Check(HasBranch(rows, "s1", true), "A: running branch is open");
        Check(rows.size() == 2 && rows[1].depth == 1 && rows[1].agentId == "s1" && rows[1].target == "a.cpp",
              "A: the activity sits one level under its branch");
    }

    // B. A completed branch closes 3 seconds after it ends.
    {
        std::vector<LlmStreamEvent> ev = {ToolCall(1, "s2", "Read", R"({"file_path":"b.cpp"})", 1100)};
        std::vector<LlmSubagentState> subs = {Sub("s2", "", "completed", 1000, 2000, "done soon")};
        auto early = BuildActivityTree(ev, subs, 4000, true, none);
        Check(HasBranch(early, "s2", true) && early.size() == 2, "B: completed 2 s ago is still open");
        auto late = BuildActivityTree(ev, subs, 5500, true, none);
        Check(HasBranch(late, "s2", false) && late.size() == 1, "B: completed 3.5 s ago is one summary row");
        Check(late.size() == 1 && late[0].sinceEpochMs == 1000 && late[0].endEpochMs == 2000,
              "B: the summary keeps start and end times");
    }

    // C. A failed branch stays open.
    {
        std::vector<LlmStreamEvent> ev = {ToolCall(1, "s3", "Bash", R"({"command":"make"})", 1100)};
        std::vector<LlmSubagentState> subs = {Sub("s3", "", "failed", 1000, 2000, "broken")};
        auto rows = BuildActivityTree(ev, subs, 60000, true, none);
        Check(HasBranch(rows, "s3", true), "C: a failed branch is not closed");
    }

    // D. A user override beats the automatic state, both ways.
    {
        std::vector<LlmStreamEvent> ev;
        std::vector<LlmSubagentState> subs = {Sub("s4", "", "completed", 1000, 2000, "manual")};
        ActivityTreeOverrides opened = {{"s4", true}};
        auto rows = BuildActivityTree(ev, subs, 60000, true, opened);
        Check(HasBranch(rows, "s4", true), "D: a user-opened branch stays open after it completed");
        ActivityTreeOverrides closed = {{"s4", false}};
        std::vector<LlmSubagentState> running = {Sub("s4", "", "running", 1000, 0, "manual")};
        rows = BuildActivityTree(ev, running, 60000, true, closed);
        Check(HasBranch(rows, "s4", false) && rows.size() == 1 && rows[0].manual,
              "D: a user-closed running branch is closed and marked manual");
    }

    // E. Nested and parallel: two running roots, one of them with a running child.
    {
        std::vector<LlmStreamEvent> ev;
        std::vector<LlmSubagentState> subs = {Sub("s5", "", "running", 1000, 0, "left"),
                                              Sub("s6", "s5", "running", 1100, 0, "left inner"),
                                              Sub("s7", "", "running", 1200, 0, "right")};
        auto rows = BuildActivityTree(ev, subs, 5000, true, none);
        Check(rows.size() == 3 && HasBranch(rows, "s5", true) && HasBranch(rows, "s7", true),
              "E: parallel running roots are both open");
        Check(rows.size() == 3 && rows[1].agentId == "s6" && rows[1].depth == 1,
              "E: the child sits under its parent, one level deeper");
    }

    // F. A completed branch with a running descendant stays open. A closed branch hides its children.
    {
        std::vector<LlmStreamEvent> ev;
        std::vector<LlmSubagentState> subs = {Sub("s8", "", "completed", 1000, 2000, "outer"),
                                              Sub("s9", "s8", "running", 1500, 0, "inner")};
        auto rows = BuildActivityTree(ev, subs, 9000, true, none);
        Check(HasBranch(rows, "s8", true) && CountBranch(rows, "s9") == 1,
              "F: a completed branch with a running child is not closed");
        std::vector<LlmSubagentState> finished = {Sub("s10", "", "completed", 1000, 2000, "outer done"),
                                                  Sub("s11", "s10", "completed", 1500, 1900, "inner done")};
        rows = BuildActivityTree(ev, finished, 9000, true, none);
        Check(rows.size() == 1 && HasBranch(rows, "s10", false) && CountBranch(rows, "s11") == 0,
              "F: a closed branch hides its finished children");
    }

    // G. Events dropped from the stream (kMaxEvents) keep the branch summary.
    {
        std::vector<LlmStreamEvent> ev;
        LlmSubagentState s = Sub("s12", "", "completed", 1000, 2000, "evicted events");
        s.totalTokens = 310;
        s.toolUses = 1;
        std::vector<LlmSubagentState> subs = {s};
        auto rows = BuildActivityTree(ev, subs, 2500, true, none);
        Check(rows.size() == 1 && rows[0].isBranch && rows[0].tokens == 310 && rows[0].toolUses == 1 &&
                  rows[0].target == "evicted events",
              "G: a branch whose events are gone still shows its summary");
    }

    // H. Consecutive steps of one kind merge; thinking does not. (Inside a sub-agent: the root's own
    // steps are not shown, so row 0 is the branch.)
    {
        std::vector<LlmStreamEvent> ev;
        for (uint64_t i = 1; i <= 6; ++i) ev.push_back(ToolCall(i, "h1", "Read", R"({"file_path":"f.cpp"})", 1000 + static_cast<int64_t>(i)));
        ev.push_back(Thought(7, "h1", 1100));
        ev.push_back(ToolCall(8, "h1", "Read", R"({"file_path":"g.cpp"})", 1200));
        std::vector<LlmSubagentState> subs = {Sub("h1", "", "running", 900, 0, "merge")};
        auto rows = BuildActivityTree(ev, subs, 5000, true, none);
        Check(rows.size() == 4, "H: six reads merge into one row and thinking stays separate");
        Check(rows.size() == 4 && rows[1].kind == "read" && rows[1].count == 6,
              "H: the merged row counts six reads");
        Check(rows.size() == 4 && rows[2].kind == "thinking" && rows[2].count == 1,
              "H: a thought is its own row");
    }

    // I. Without sub-agents the tree is empty: the root agent's own steps are left out.
    {
        std::vector<LlmStreamEvent> ev = {ToolCall(1, "", "Read", R"({"file_path":"x.cpp"})", 1000),
                                          Thought(2, "", 1100),
                                          ToolCall(3, "", "Bash", R"({"command":"make"})", 1200)};
        auto rows = BuildActivityTree(ev, {}, 5000, true, none);
        Check(rows.empty(), "I: no sub-agents gives no rows");
        // The same steps inside a sub-agent are listed in event order.
        for (auto& e : ev) e.agentId = "i1";
        std::vector<LlmSubagentState> subs = {Sub("i1", "", "running", 900, 0, "order")};
        rows = BuildActivityTree(ev, subs, 5000, true, none);
        Check(rows.size() == 4 && rows[0].isBranch && rows[1].kind == "read" && rows[2].kind == "thinking" &&
                  rows[3].kind == "command",
              "I: a sub-agent's activities are in event order");
    }

    // J. The real order of a stream: every tool call is followed by its result.
    {
        std::vector<LlmStreamEvent> ev;
        auto addResult = [&](uint64_t seq, int64_t ms) {
            LlmStreamEvent r;
            r.seq = seq;
            r.type = "tool_result";
            r.agentId = "j1";
            r.recvEpochMs = ms;
            ev.push_back(r);
        };
        ev.push_back(ToolCall(1, "j1", "Read", R"({"file_path":"a.cpp"})", 1000));
        addResult(2, 1400);
        ev.push_back(ToolCall(3, "j1", "Read", R"({"file_path":"b.cpp"})", 1500));
        addResult(4, 1900);
        std::vector<LlmSubagentState> subs = {Sub("j1", "", "running", 900, 0, "stream")};
        auto rows = BuildActivityTree(ev, subs, 5000, true, none);
        Check(rows.size() == 3 && rows[1].kind == "read" && rows[1].count == 2,
              "J: results between tool calls do not split the merged row");
        Check(rows.size() == 3 && rows[1].endEpochMs == 1900, "J: a step ends when the next event of its agent arrives");
        Check(rows.size() == 3 && rows[2].kind == "waiting_model", "J: waiting for the model shows while it is the newest step");
    }

    // K. The Agent call and its result are shown only as the sub-agent's branch.
    {
        LlmStreamEvent call = ToolCall(1, "", "Agent", R"({"description":"look"})", 1000);
        call.toolUseId = "tu1";
        LlmStreamEvent result;
        result.seq = 2;
        result.type = "tool_result";
        result.toolUseId = "tu1";
        result.recvEpochMs = 1100;
        LlmSubagentState s = Sub("k1", "", "running", 1000, 0, "look");
        s.toolUseId = "tu1";
        auto rows = BuildActivityTree({call, result}, {s}, 5000, true, none);
        Check(rows.size() == 1 && rows[0].isBranch, "K: the Agent call adds no row beside its branch");
    }

    // L. A turn's end is no "done" row while a sub-agent runs or more work follows.
    {
        LlmStreamEvent result;
        result.seq = 1;
        result.type = "result";
        result.recvEpochMs = 1000;
        LlmSubagentState s = Sub("l1", "", "running", 900, 0, "long");
        auto rows = BuildActivityTree({result}, {s}, 5000, true, none);
        Check(rows.size() == 1 && rows[0].isBranch, "L: no done row while a sub-agent runs");
        s.status = "completed";
        s.endedEpochMs = 950;
        rows = BuildActivityTree({result}, {s}, 5000, true, none);
        Check(rows.size() == 1 && rows[0].isBranch, "L: the root's done row is left out once nothing runs");
        // Inside a sub-agent: its end shows while newest and is dropped when work follows.
        s.status = "running";
        s.endedEpochMs = 0;
        result.agentId = "l1";
        rows = BuildActivityTree({result}, {s}, 5000, false, none);
        Check(rows.size() == 2 && rows[1].kind == "done", "L: the done row shows once nothing runs");
        LlmStreamEvent later = ToolCall(2, "l1", "Read", R"({"file_path":"a.cpp"})", 1100);
        rows = BuildActivityTree({result, later}, {s}, 5000, false, none);
        Check(rows.size() == 2 && rows[1].kind == "read", "L: a turn's end is dropped when work follows");
    }

    if (g_failures == 0) std::printf("ALL PASS\n");
    else std::printf("%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
