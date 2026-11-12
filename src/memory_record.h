#pragma once

// L1 run records (doc/design-v3.md 3.3, 3.7).
//
// One record is made for each finished TaskQueueAttempt, without an LLM, from the
// attempt stored in logs/taskqueues/*.json and the normalized logs/proc_*.events.jsonl.
// The same inputs always produce the same record, so `memory rebuild` can recreate the
// records and every record can be traced back to its events file and seq range.

#include "nlohmann/json.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace pc {
namespace memory {

// Everything BuildRunRecord may consult. Absolute paths are resolved by the caller;
// when eventsFile / logFile are empty they are located from processId, jobId and attempt,
// which keeps a live record identical to one rebuilt later.
struct RunRecordInput {
    std::string jobId;
    std::string taskqueue;
    std::string jobName;
    std::string summary;
    std::string instructions;
    std::string command;
    std::string cwd;

    std::string btId;
    std::string btNode;
    int btRun = 0;
    int attempt = 1;

    std::string originClient;
    std::string originSession;
    std::string originCwd;

    std::string start;
    std::string end;
    std::string createdAt;
    int exitCode = -1;
    std::string reason;          // attempt reason: ok, failed, timedOut, killed, ...
    std::string jobState;        // job state, used to classify a cancelled attempt
    std::string failureClass;
    std::string failureSummary;
    std::string checkDetail;
    int timeoutSec = 0;

    int processId = 0;
    std::string eventsFile;      // absolute .events.jsonl path
    std::string logFile;         // absolute attempt .log path
    std::string lastLinesFile;   // .tail.txt path when the full log is gone
    std::string logText;         // full log text when the caller already has it
};

// rr_<jobId>_<attempt>, matching the id in doc/design-v3.md 3.3.
std::string RunRecordId(const std::string& jobId, int attempt);

// Evaluates deterministic failure classification rules for unclassified failures
// (doc/design-v3.md §7, card 072-2). Matches queue attempts' determination:
// launch_error, killed, timeout, idle_stall, crash (NTSTATUS), exit_nonzero, success_check_failed.
// Returns pair of {failureClass, failureSummary}. If no deterministic rule matched, returns {"", ""}.
std::pair<std::string, std::string> ClassifyDeterministicFailure(
    const std::string& reason, int exitCode, const std::string& outcome,
    const std::string& checkDetail = "", int timeoutSec = 0, int64_t durationSec = 0);

// Input for creating a RunRecord from an execution outside a task queue (doc/design-v3.md §7, card 072-2).
struct PlainRunRecordInput {
    std::string jobId;
    int attempt = 1;
    std::string taskqueue = "plain";
    std::string jobName;
    std::string summary;
    std::string command;
    std::string cwd;
    std::string start;
    std::string end;
    int exitCode = 0;
    std::string reason;
    int timeoutSec = 0;
    int processId = 0;
    std::string logFile;
    std::string lastLinesFile;
    std::string checkDetail;
};

// Builds one hancho.memory.run/1 object. Deterministic for the same input files.
nlohmann::json BuildRunRecord(const RunRecordInput& input);

// Builds one hancho.memory.run/1 record for a plain run.
nlohmann::json BuildPlainRunRecord(const PlainRunRecordInput& input);

// Appends the record to memory/records/YYYY-MM.jsonl unless the id already exists.
// Also enforces memory/ capacity limit if configured.
// Returns {"ok","id","path","created"} and never throws.
std::string AppendRunRecordJson(const RunRecordInput& input);

// Appends a plain run's RunRecord to memory/records/YYYY-MM.jsonl.
std::string AppendPlainRunRecordJson(const PlainRunRecordInput& input);

// Rebuilds memory/records/*.jsonl from logs/taskqueues/*.json and the events/log files
// they point at. Does nothing when there are no taskqueue files (so records written by
// hand are not lost). Returns {"ok","skipped","records","files","months"}.
std::string RebuildRunRecordsJson();

std::string RunRecordsDirectoryUtf8();

// Memory storage statistics across L1, L2, L3 and index (doc/design-v3.md 3.7).
struct MemoryStorageStats {
    int64_t totalBytes = 0;
    int64_t limitBytes = 0; // 0 = no limit configured
    int64_t l1Bytes = 0;
    int64_t l2Bytes = 0;
    int64_t l3Bytes = 0;
    int64_t indexBytes = 0;
    int64_t otherBytes = 0;
    bool nearLimit = false; // true if limitBytes > 0 && totalBytes >= limitBytes * 0.8
    bool exceeded = false;  // true if limitBytes > 0 && totalBytes > limitBytes
    std::string warning;
};

using StorageStats = MemoryStorageStats;

// Options for memory prune (doc/design-v3.md 3.7).
struct PruneOptions {
    bool dryRun = false;
    int retentionDays = 90;   // default 90 days; <= 0 disables age-based pruning
    int64_t limitBytes = -1;  // -1 means read from config.json (memory_max_bytes)
    int64_t nowEpoch = 0;     // 0 means current time
};

struct PruneResult {
    bool ok = true;
    std::string error;
    bool dryRun = false;
    int retentionDays = 90;
    std::vector<std::string> prunedFiles;   // relative to data dir, e.g. "memory/records/2026-07.jsonl"
    std::vector<std::string> prunedMonths;  // "2026-07"
    int64_t bytesFreed = 0;
    int64_t totalBytesBefore = 0;
    int64_t totalBytesAfter = 0;
    int64_t limitBytes = 0;
    bool nearLimit = false;
    bool exceeded = false;
    std::string warning;
};

// Returns configured memory capacity limit in bytes from <data-dir>/config.json
// (keys: "memory_max_bytes", "memory_limit_bytes", or "memory_max_mb"). 0 if unlimited.
int64_t GetConfiguredMemoryLimitBytes();

// Returns configured default retention days from <data-dir>/config.json
// ("memory_retention_days"). Defaults to 90.
int GetConfiguredRetentionDays();

// Computes current storage size of memory/ directory (L1, L2, L3, index, total).
MemoryStorageStats GetMemoryStorageStats();

// Prunes old L1 run record month files older than retentionDays (default 90)
// and enforces memory/ capacity limit (deleting oldest L1 files if exceeded).
// L2 and L3 files are NEVER deleted.
PruneResult PruneRunRecords(const PruneOptions& options);

// JSON wrapper for PruneRunRecords.
std::string PruneRunRecordsJson(const PruneOptions& options);

// Enforces memory limits if configured, pruning oldest L1 files if needed.
PruneResult EnforceMemoryLimits(bool dryRun = false);

} // namespace memory
} // namespace pc
