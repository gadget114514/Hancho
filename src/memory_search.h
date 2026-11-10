#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace pc {
namespace memory {

struct Query {
    std::string text;
    std::vector<std::string> kinds;
    std::string scope;
    std::string file;
    std::string jobName;
    std::string taskqueue;
    std::string engine;
    std::string model;
    std::string outcome;
    std::string failureClass;
    std::string since;
    int limit = 20;
};

struct RankCandidate {
    std::string id;
    std::string kind;
    std::string text;
    int64_t createdEpoch = 0;
};

// Inputs for memory_brief (doc/design-v3.md 3.8). jobName and files are both optional; when
// neither is given the brief is built from the most recent runs.
struct BriefOptions {
    std::string jobName;
    std::string taskqueue;
    std::string engine;
    std::vector<std::string> files;
    std::vector<std::string> scopes; // job, file, taskqueue, project (default: job, file, project)
    int budgetChars = 1500;
    int maxPreviousRuns = 3;
    int statsWindow = 5;
    bool includeAgentNotes = true;
};

struct BriefResult {
    bool ok = true;
    std::string error;
    std::string text;
    std::vector<std::string> ids; // every id that ended up in text, in section order
    int chars = 0;
    int budgetChars = 1500;
    bool truncated = false;
};

// Input for an L2 note (doc/design-v3.md 3.4, 3.10). The writer validates the scope, redacts
// known secrets, enforces the agent rules (sources, 5 per attempt, no duplicate scope+text)
// and appends one replayable line to memory/notes.jsonl.
struct NoteInput {
    std::string text;
    std::vector<std::string> tags;
    std::string scope = "project"; // project, taskqueue:<name>, job:<name>, file:<path>, engine:<name>
    std::string authorType = "human"; // agent, human or hancho
    std::string authorClient;
    std::string authorSession;
    std::vector<std::string> sources;
    std::string supersedes;
    std::string expiresAt;
    std::string attemptId; // defaults to the first run source ("rr_...") for agent notes
};

class IMemoryRanker {
public:
    virtual ~IMemoryRanker() = default;
    virtual const char* Name() const = 0;
    virtual bool Rank(const std::string& query,
                      const std::vector<RankCandidate>& candidates,
                      std::vector<double>& outScores,
                      std::string& outError) const = 0;
};

std::string SearchJson(const Query& query, const IMemoryRanker* ranker = nullptr);
std::string GetJson(const std::string& id, const std::string& kind = "");
std::string ApproveJson(const std::string& noteId, const std::string& approvedBy);
std::string NoteJson(const NoteInput& input);
std::string RetractJson(const std::string& noteId, const std::string& retractedBy,
                        const std::string& reason);
std::string StatsJson(const Query& filter);
BriefResult BuildBrief(const BriefOptions& options);
std::string BriefJson(const BriefOptions& options);
std::string RebuildJson();
std::string HandleToolCall(const std::string& toolName, const std::string& argsJson);
bool ParseQueryJson(const std::string& argsJson, Query& out, std::string& outError);
std::string MemoryDirectoryUtf8();

} // namespace memory
} // namespace pc
