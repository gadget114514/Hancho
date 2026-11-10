#pragma once

// L1 run records (doc/design-v3.md 3.3, 3.7).
//
// One record is made for each finished TaskQueueAttempt, without an LLM, from the
// attempt stored in logs/taskqueues/*.json and the normalized logs/proc_*.events.jsonl.
// The same inputs always produce the same record, so `memory rebuild` can recreate the
// records and every record can be traced back to its events file and seq range.

#include "nlohmann/json.hpp"

#include <string>

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

    int processId = 0;
    std::string eventsFile;      // absolute .events.jsonl path
    std::string logFile;         // absolute attempt .log path
    std::string lastLinesFile;   // .tail.txt path when the full log is gone
    std::string logText;         // full log text when the caller already has it
};

// rr_<jobId>_<attempt>, matching the id in doc/design-v3.md 3.3.
std::string RunRecordId(const std::string& jobId, int attempt);

// Builds one hancho.memory.run/1 object. Deterministic for the same input files.
nlohmann::json BuildRunRecord(const RunRecordInput& input);

// Appends the record to memory/records/YYYY-MM.jsonl unless the id already exists.
// Returns {"ok","id","path","created"} and never throws.
std::string AppendRunRecordJson(const RunRecordInput& input);

// Rebuilds memory/records/*.jsonl from logs/taskqueues/*.json and the events/log files
// they point at. Does nothing when there are no taskqueue files (so records written by
// hand are not lost). Returns {"ok","skipped","records","files","months"}.
std::string RebuildRunRecordsJson();

std::string RunRecordsDirectoryUtf8();

} // namespace memory
} // namespace pc
