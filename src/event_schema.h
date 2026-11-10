#pragma once

#include "llm_stream.h"
#include "nlohmann/json.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace pc {

// Source fields of one normalized hancho.event/1 event. processId is always written; the rest
// are omitted when empty (or 0 for attempt).
struct HanchoEventSource {
    int processId = 0;
    std::string jobId;
    int attempt = 0;
    std::string btId;
    std::string node;
};

// The closed hancho.event/1 kind for one internal stream event. Unknown types become "raw".
const char* HanchoEventKindFor(const LlmStreamEvent& ev);

// Normalizes one internal LlmStreamEvent into a hancho.event/1 object. parentAgentId is the agent
// that issued the tool_use this sub-agent event belongs to (empty for the root agent). rawFile is
// the .stream.jsonl file the event came from, rawLine its physical line (0 when unknown).
nlohmann::json BuildHanchoEventV1(const LlmStreamEvent& ev, const HanchoEventSource& source,
                                  const std::string& parentAgentId, const std::string& rawFile,
                                  uint64_t rawLine);

// Builds the outer hancho.event/1 object from already-decided parts. Used by the normalizer and
// by declarative/external adapters that produce a kind and data directly.
nlohmann::json BuildHanchoEventObject(const HanchoEventSource& source,
                                      const std::string& agentType, const std::string& model,
                                      const std::string& sessionId, const std::string& agentId,
                                      const std::string& parentAgentId, uint64_t seq, int64_t epochMs,
                                      const std::string& kind, nlohmann::json data,
                                      const std::string& rawFile, uint64_t rawLine);

// logs/proc_<id>_<name>_stream.jsonl -> logs/proc_<id>_<name>.events.jsonl.
std::string HanchoEventsFilePathForStream(const std::string& streamFilePath);

// Reads normalized events from an .events.jsonl file. Keeps at most `tail` of the most recent
// events (0 = all) whose kind equals kindFilter (empty = all) and whose seq is greater than
// sinceSeq. Unreadable lines and non-hancho.event/1 objects are skipped.
std::vector<nlohmann::json> ReadHanchoEventsFile(const std::string& path, size_t tail,
                                                 const std::string& kindFilter, uint64_t sinceSeq);

} // namespace pc
