#pragma once

// Tier 1 declarative adapters (doc/design-v3.md 1.4): a JSON description of how to turn one line
// of a CLI's JSON output into a hancho.event/1 event. No C++ is needed to add a CLI.
//
// Adapter files live at <data-dir>/adapters/<name>.json and are selected by the process agent type.

#include "event_schema.h"
#include "llm_stream.h"
#include "nlohmann/json.hpp"
#include <map>
#include <string>
#include <vector>

namespace pc {

struct HanchoAdapterRule {
    std::map<std::string, nlohmann::json> when; // JSON Pointer -> expected value
    std::string kind;                           // hancho.event/1 kind
    std::map<std::string, std::string> map;     // data field -> JSON Pointer ("!ptr" negates)
    bool hasActivity = false;
    std::string activityFrom;
    std::map<std::string, std::string> activityTable;
    std::string activityDefault = "tool";
};

struct HanchoAdapter {
    std::string name;
    std::string matchField; // optional JSON Pointer that must exist for the adapter to consider a line
    std::string exec;       // Tier 3: converter command (stdin raw lines, stdout hancho.event lines)
    std::vector<HanchoAdapterRule> rules;
};

// <data-dir>/adapters and <data-dir>/adapters/<name>.json.
std::string HanchoAdapterDirectory();
std::string HanchoAdapterPathFor(const std::string& name);

bool LoadHanchoAdapterFromJson(const nlohmann::json& json, HanchoAdapter& out, std::string& err);
bool LoadHanchoAdapterFile(const std::string& path, HanchoAdapter& out, std::string& err);

// Normalizes one raw output line. Returns the hancho.event/1 kind and fills data. Lines that do
// not match a rule (or are not a JSON object) become "raw" with data.text set to the line.
std::string NormalizeHanchoAdapterLine(const HanchoAdapter& adapter, const std::string& line,
                                       nlohmann::json& data);

// Resolves a JSON Pointer (RFC 6901) against a JSON value. Returns nullptr when it does not exist.
const nlohmann::json* ResolveJsonPointer(const nlohmann::json& root, const std::string& pointer);

// Maps a v1 event back onto the internal stream event so live state and events_ keep working.
LlmStreamEvent LlmStreamEventFromHanchoV1(const nlohmann::json& event);

// True when kind is one of the closed hancho.event/1 kinds.
bool IsHanchoEventKind(const std::string& kind);

} // namespace pc
