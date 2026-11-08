#pragma once

// Internal data structures and helpers for BehaviorTree (split across src/behavior_tree_*.cpp).

#include "behavior_tree.h"
#include "nlohmann/json.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pc {

using nlohmann::json;

struct BtCaptureRule {
    // Either regex capture from job log tail, or JSON path from a JSON file.
    std::string regex;
    int group = 1;
    std::string jsonFile;
    std::string path;
};

struct BtEscalateInclude {
    std::vector<std::string> logs;
    std::vector<std::string> vars;
    std::vector<std::string> files;
    int logLines = 100;
    size_t fileMaxBytes = 16384;
};

struct BtEscalateFallback {
    bool enabled = false;
    int afterSec = 0;
    std::string engine;
    std::string model;
};

struct BtNode {
    std::string id;       // explicit id from JSON (empty if omitted)
    std::string pathKey;  // e.g. "root", "root.children[0].child"
    std::string nodeKey;  // id if non-empty, else pathKey
    std::string type;     // normalized type ("selector" -> "fallback" in exec, raw kept in rawDef)
    std::string role;     // "", "child", "cond", "then", "else", "until"
    int depth = 1;

    // Original JSON definition of this node (without runtime "status", children kept for rebuild)
    json rawDef;

    // Composite / decorator parameters
    std::string parallelSuccessMode = "all"; // "all", "any", or "count"
    int parallelSuccessCount = 0;
    int maxCount = 0;       // retry / repeat max
    int delaySec = 0;       // retry delay
    int sec = 0;            // wait / timeout seconds

    // Job / worker parameters
    std::string summary;
    std::string command;
    std::string cwd;
    int timeoutSec = 0;
    std::map<std::string, std::string> env;
    BtJobSuccess success;
    BtJobIdle idle;
    BtJobResources resources;
    std::string instructionsText;
    std::string instructionsFile;
    int maxAttempts = 1;
    std::string engine;
    std::string model;
    std::string prompt;
    std::string promptFile;
    std::string outputSchema;
    std::map<std::string, BtCaptureRule> capture;

    // Set leaf
    std::string setVar;
    std::string setValue;

    // Fail leaf
    std::string failDetail;

    // Escalate leaf
    std::string question;
    std::string guidance;
    std::vector<std::string> expect;
    BtEscalateInclude include;
    int escalateTimeoutSec = 86400;
    BtEscalateFallback fallback;

    // Condition leaf parameters
    std::string condFile;
    std::string condPath;
    std::string condThan;
    std::string condNode;
    std::string condRegex;
    int condLines = 200;
    std::string condVarName;
    bool hasEq = false;
    json condEq;
    bool hasNe = false;
    json condNe;
    bool hasIn = false;
    json condIn;
    bool hasGt = false;
    json condGt;
    bool hasLt = false;
    json condLt;
    bool hasExists = false;
    bool condExists = true;

    // Child pointers
    std::vector<std::unique_ptr<BtNode>> children;
    std::unique_ptr<BtNode> condChild;
    std::unique_ptr<BtNode> thenChild;
    std::unique_ptr<BtNode> elseChild;
    std::unique_ptr<BtNode> singleChild;
    std::unique_ptr<BtNode> untilChild;

    // Runtime state (persisted in status)
    bool hasStatus = false;
    std::string state = "idle"; // "idle", "running", "succeeded", "failed", "skipped"
    int runs = 0;
    std::vector<std::string> jobs;
    std::string detail;
    std::string answer;
    std::string rationale;

    // Transient / engine runtime state for this node
    std::string activeJobId;
    std::string fallbackJobId;
    bool isWorkerJob = false;
    int lastExitCode = 0;
    bool hasExitCode = false;
    std::string lastLogFile;
    std::string lastLogTail;
    std::string lastStartTimestamp;
    int64_t lastStartMs = 0;
    int64_t waitDeadlineMs = 0;
    int64_t timeoutDeadlineMs = 0;
    int64_t retryDeadlineMs = 0;
    int64_t escalateDeadlineMs = 0;
    int64_t fallbackDeadlineMs = 0;
    bool fallbackLaunched = false;
    int ifBranch = 0; // 0 = not evaluated, 1 = then, 2 = else
    bool repeatWaitingUntil = false;
    BtEscalationPacket currentPacket;
};

struct RunningJobInfo {
    BtNode* node = nullptr;
    bool isFallback = false;
    bool isWorker = false;
};

struct BehaviorTree::Impl {
    json rawRootDoc;
    std::unique_ptr<BtNode> root;
    std::map<std::string, RunningJobInfo> runningJobs; // jobId -> RunningJobInfo

    // Lookup helpers
    BtNode* FindNode(const std::string& key, const std::string& btId) const;
    const BtNode* FindNodeConst(const std::string& key, const std::string& btId) const;

    // Variable and expansion helpers (behavior_tree_vars.cpp)
    bool LookupVar(const BehaviorTree& bt, const std::string& expr, std::string& outVal) const;
    bool ExpandString(const BehaviorTree& bt, const std::string& input, bool quoteForCommand,
                      std::string& outExpanded, std::string& outUndefinedVar) const;
    void ApplyCaptures(BehaviorTree& bt, BtNode& node, const std::string& logTail, int64_t nowMs);
    void StoreWorkerOutputs(BehaviorTree& bt, BtNode& node, const std::string& finalResponse,
                            const std::string& resultFileJson, int64_t nowMs);

    // Condition evaluation (behavior_tree_cond.cpp)
    bool CompareJsonValue(const BehaviorTree& bt, const BtNode& node, bool valueExists,
                          const json& actualVal, std::string& outUndefinedVar) const;
    bool EvaluateCondition(BehaviorTree& bt, BtNode& node, int64_t nowMs);

    // State restoration (behavior_tree_state.cpp)
    void RestoreNodeStatus(BehaviorTree& bt, BtNode& node, const json& jNode,
                           int64_t baseMs, int& nextJobSeq,
                           std::map<std::string, std::string>& vars);

    // Escalate helpers (behavior_tree_escalate.cpp)
    void StartEscalateNode(BehaviorTree& bt, BtNode& node, int64_t nowMs);
    void LaunchEscalateFallback(BehaviorTree& bt, BtNode& node, int64_t nowMs);
    bool ApplyEscalateAnswer(BehaviorTree& bt, BtNode& node, const std::string& answer,
                             const std::string& rationale,
                             const std::map<std::string, std::string>& extraVars,
                             int64_t nowMs, std::string& err);

    // Execution helpers (behavior_tree_exec.cpp)
    void ResetSubtreeForRun(BtNode& node, bool resetHistory);
    void CancelSubtreeJobs(BehaviorTree& bt, BtNode& node, bool markSkipped);
    void TickNode(BehaviorTree& bt, BtNode& node, int64_t nowMs);
    bool HasWaitingEscalation(const BtNode& node) const;
    void CollectNextWakeMs(const BtNode& node, int64_t& minWakeMs) const;
    void CheckTimers(BehaviorTree& bt, BtNode& node, int64_t nowMs, bool& anyFired);
};

// Path resolution relative to BT cwd
std::wstring ResolveBtPathWide(const std::string& cwdUtf8, const std::string& relOrAbsUtf8);
std::string FormatEpochMsIso(int64_t nowMs);
bool NavigateJsonPath(const json& root, const std::string& path, json& outVal);

std::unique_ptr<BtNode> ParseSingleNodeJson(const json& j,
                                            const std::string& path,
                                            const std::string& role,
                                            int depth,
                                            const IBtEngineCatalog* catalog,
                                            const BtWorkerDefaults& workerDefaults,
                                            const std::string& btCwd,
                                            const std::vector<std::string>& existingIdsOutsideTarget,
                                            int existingNodesOutsideTarget,
                                            std::string& outError);

} // namespace pc
