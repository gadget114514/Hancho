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

struct BtNode;

// One `catch` handler. `doNode` is parsed from the handler's "do" object and ticks like a
// normal child; `rawDef` keeps the authored handler fields for state persistence and updates.
struct BtCatchHandler {
    json rawDef = json::object();
    std::vector<std::string> classes;
    std::string cause;
    bool thenRetry = false;
    std::unique_ptr<BtNode> doNode;
};

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

// One pre-approved engine candidate for a worker node (doc/obsoleted/HanchoExpectations.md 8.3).
struct BtEngineChoice {
    std::string engine;
    std::string model;
    std::string list;      // LLM list the choice came from, when known
    std::string entry;     // compact description of the approving list row
};

// Record of one tried candidate, kept in the node status for auditability.
struct BtEngineAttempt {
    std::string engine;
    std::string model;
    std::string result;        // "succeeded" or "failed"
    std::string failureClass;
    std::string detail;
    std::string list;          // "worker" — the LLM list the choice came from
    std::string entry;         // compact description of the approving list row
    json handoff;              // optional {"suspected","tried","ruledOut"} from PC_BT_RESULT_FILE
    std::vector<std::string> failSignature; // populated in 101-3
};

struct BtNode {
    std::string id;       // explicit id from JSON (empty if omitted)
    std::string pathKey;  // e.g. "root", "root.children[0].child"
    std::string nodeKey;  // id if non-empty, else pathKey
    std::string type;     // normalized type ("selector" -> "fallback" in exec, raw kept in rawDef)
    std::string role;     // "", "child", "cond", "then", "else", "until"
    int depth = 1;
    // Task queue this node's jobs run in. Empty means the tree's queue. Each child of a
    // parallel node gets its own queue so the harness's "parallel" actually runs in parallel.
    std::string queueName;

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
    int estimateSec = 0;  // optional expected duration for ETA display (0 = use timeoutSec)
    std::map<std::string, std::string> env;
    BtJobSuccess success;
    BtJobIdle idle;
    BtJobResources resources;
    std::string instructionsText;
    std::string instructionsFile;
    int maxAttempts = 1;
    bool maxAttemptsConfigured = false;
    bool probe = false;
    bool probeConfigured = false;
    std::string engine;
    std::string model;
    std::string prompt;
    std::string promptFile;
    std::string outputSchema;
    std::map<std::string, BtCaptureRule> capture;

    // Engine switching (091-3). `engineCandidates` is the `engines` list; when it is
    // non-empty the node runs the first usable candidate and advances on a failure class
    // listed in `switchEngineOn`. `engine` / `model` may not be combined with `engines`.
    std::vector<BtEngineChoice> engineCandidates;
    std::vector<BtSwitchRule> switchEngineOn;
    BtSwitchLimits switchLimits;
    std::map<std::string, int> switchFailCounts;
    int engineCandidateIndex = 0;
    std::vector<BtEngineAttempt> engineAttempts;
    json lastHandoff;          // handoff from the most recent worker attempt (101-2, used in 101-4)
    std::string onSwitch = "keep"; // "keep" (default) or "revert" (warned as unsupported, 101-4)
    std::string blame;         // job node: worker id to blame on failure (101-3)
    std::string failLineRegex; // job node: regex for fail signature lines (101-3)
    std::vector<std::vector<std::string>> recentFailSignatures; // worker: last 3 signatures for current candidate (101-3)
    int64_t firstStartMs = 0;  // epoch ms of first worker launch on this node (101-5, for switchLimits.maxSec)
    std::string switchStoppedReason; // "max_candidates", "max_sec", or "same_symptom" (101-5)
    int probeTimeoutSec = 60;
    // 091-6: a worker whose engine is down waits for recovery instead of failing.
    bool waitingForEngine = false;
    int64_t engineWaitUntilMs = 0;
    std::vector<BtEngineChoice> fallbackCandidates;  // consult-list proxy answer candidates
    size_t fallbackCandidateIndex = 0;

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
    // "failure" condition: the referenced node must have failed with a matching class/cause.
    std::vector<std::string> condFailureClasses;
    std::string condFailureCause;

    // catch node parameters
    std::vector<BtCatchHandler> catchHandlers;
    int catchMaxRetries = 1;
    // catch execution phase: 0 child, 1 selected handler's do, 3 otherwise node
    int catchPhase = 0;
    int catchRetries = 0;
    int catchHandlerIndex = -1;
    std::string catchHandler;  // label of the selected handler, kept in state and nodeFinished detail

    // foreach node parameters (093-2). `doChild` is the unexpanded template; once the node
    // first ticks, one concrete copy per glob match is stored in `children`.
    std::string foreachGlob;
    std::string foreachAs = "t";
    std::string foreachMode = "sequence"; // "sequence", "parallel", or "all"
    std::vector<std::string> foreachExclude;
    int foreachMinMatches = 0;
    std::unique_ptr<BtNode> doChild;
    bool foreachExpanded = false;
    bool isForeachChild = false;

    // Child pointers
    std::vector<std::unique_ptr<BtNode>> children;
    std::unique_ptr<BtNode> condChild;
    std::unique_ptr<BtNode> thenChild;
    std::unique_ptr<BtNode> elseChild;
    std::unique_ptr<BtNode> singleChild;
    std::unique_ptr<BtNode> untilChild;
    std::unique_ptr<BtNode> otherwiseChild;

    // Runtime state (persisted in status)
    bool hasStatus = false;
    std::string state = "idle"; // "idle", "running", "succeeded", "failed", "skipped"
    int runs = 0;
    // Monotonic per-node job launch counter. Unlike `runs`, it is never reset by Retry(resetCounters),
    // so proposed job IDs stay unique within a BT (ids are "<btId>-<nodeKey>-<launchSeq>").
    int launchSeq = 0;
    std::vector<std::string> jobs;
    std::string detail;
    std::string answer;
    std::string rationale;
    // Set when the escalate answer is "takeover": the supervisor owns the work. The node
    // still counts as succeeded so the tree can continue, but the decision is persisted.
    bool takenOver = false;
    json takeover;

    // Transient / engine runtime state for this node
    std::string activeJobId;
    std::string fallbackJobId;
    bool isWorkerJob = false;
    // Engine/model actually used for the last worker launch (empty for a plain job).
    std::string lastEngine;
    std::string lastModel;
    // Failure diagnostic of the most recent attempt (the taskqueue JSON record). Cleared on
    // success so a stale failure never leaks into ${<node>.failureClass} or a `failure` condition.
    bool hasFailureDiagnostic = false;
    json failureDiagnostic = json::object();
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
    std::string effectiveCwd;
    std::string expandedTestReport;
    std::string expandedContentFile;
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
    // Stores/clears the failure diagnostic and its ${<node>.*} variables (8.1).
    void ApplyFailureDiagnostic(BehaviorTree& bt, BtNode& node, const std::string& diagnosticJson,
                                bool succeeded, int64_t nowMs);

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
    void ResetSubtreeForRun(BehaviorTree& bt, BtNode& node, bool resetHistory);
    void CancelSubtreeJobs(BehaviorTree& bt, BtNode& node, bool markSkipped);
    // 093-2: expands a foreach node's `do` template into one child per glob match. On failure
    // the node is failed and a nodeFinished event is emitted; on success children are ready to tick.
    bool ExpandForeachNode(BehaviorTree& bt, BtNode& node, int64_t nowMs);
    // Reopens the ancestors of `target` and discards the results of every child that runs
    // after the one containing `target` (behavior_tree_runtime.cpp).
    bool ReopenAncestorsToTarget(BehaviorTree& bt, BtNode* cur, BtNode* target, bool resetCounters);
    void TickNode(BehaviorTree& bt, BtNode& node, int64_t nowMs);
    // Advances a worker node to its next engine candidate when `failureClass` is switchable.
    // Records the attempt, emits `engineSwitched` and resets the node to idle for relaunch.
    bool TrySwitchEngine(BehaviorTree& bt, BtNode& node, const std::string& failureClass,
                         const std::string& detail, int64_t nowMs,
                         const json& handoff = json());
    bool AdvanceEngineCandidate(BehaviorTree& bt, BtNode& node, const std::string& failureClass,
                                const std::string& detail, int64_t nowMs,
                                const json& handoff, const std::vector<std::string>& failSignature,
                                bool relaunch, const std::string& why = std::string(),
                                bool ignoreSwitchEngineOn = false);
    bool ShouldSwitchEngine(const BtNode& node, const std::string& failureClass) const;
    // Advances to the next candidate when the failed attempt's engine is now marked down
    // and a later candidate is approved and up, whatever switchEngineOn says. switchLimits
    // still apply.
    bool TrySwitchFromDownEngine(BehaviorTree& bt, BtNode& node, const std::string& failureClass,
                                 const std::string& detail, int64_t nowMs, const json& handoff);
    // 091-6: hold a leaf while its engine is down instead of failing it.
    bool HoldForEngine(BehaviorTree& bt, BtNode& node, const std::string& engine,
                       const std::string& model, int64_t nowMs);
    void RunProbeLeaf(BehaviorTree& bt, BtNode& node, int64_t nowMs);
    bool HasWaitingEscalation(const BtNode& node) const;
    void CollectNextWakeMs(const BtNode& node, int64_t& minWakeMs) const;
    void CheckTimers(BehaviorTree& bt, BtNode& node, int64_t nowMs, bool& anyFired);
};

// Path resolution relative to BT cwd
std::wstring ResolveBtPathWide(const std::string& cwdUtf8, const std::string& relOrAbsUtf8);
std::string FormatEpochMsIso(int64_t nowMs);
bool NavigateJsonPath(const json& root, const std::string& path, json& outVal);

// 093-2: glob matcher used by the foreach node and ${foreach:...} expansion. Patterns are
// relative to cwdUtf8 and use '/' separators; `*` / `?` never cross a segment and `**`
// matches zero or more segments. Results are relative paths with '/' separators, sorted.
bool ExpandBtGlob(const std::string& cwdUtf8, const std::string& globUtf8,
                  const std::vector<std::string>& excludes, std::vector<std::string>& outPaths);
std::string BtPathStem(const std::string& relPath);
std::string BtPathName(const std::string& relPath);

std::unique_ptr<BtNode> ParseSingleNodeJson(const json& j,
                                            const std::string& path,
                                            const std::string& role,
                                            int depth,
                                             const IBtEngineCatalog* catalog,
                                             const BtDefaults& defaults,
                                             const std::string& btCwd,
                                            const std::vector<std::string>& existingIdsOutsideTarget,
                                            int existingNodesOutsideTarget,
                                            const std::string& baseQueue,
                                            const std::string& queueScope,
                                            std::string& outError);

} // namespace pc
