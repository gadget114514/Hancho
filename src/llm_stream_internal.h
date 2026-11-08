#pragma once

// Internal to the llm_stream*.cpp files. Each agent's stream format lives in its own
// llm_stream_<agent>.cpp behind one of the entry points below, and llm_stream.cpp dispatches
// by agent type. Replacing one agent's file leaves the others untouched.

#include "llm_stream.h"
#include "nlohmann/json.hpp"
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace pc {
namespace llm {

// Field readers. A missing or wrongly typed field gives "" or 0.
inline std::string JsonStr(const nlohmann::json& obj, const char* key) {
    if (!obj.is_object() || !obj.contains(key)) return "";
    const auto& v = obj[key];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number() || v.is_boolean()) return v.dump();
    return "";
}

inline uint64_t JsonU64(const nlohmann::json& obj, const char* key) {
    return (obj.is_object() && obj.contains(key) && obj[key].is_number()) ? obj[key].get<uint64_t>() : 0;
}

inline std::string LowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Fills the token and cost fields of one usage object (any agent's naming) into outEvent.
void ApplyLlmUsageFields(const nlohmann::json& u, LlmStreamEvent& outEvent);

// Default toolAction and content, once a split event is built.
inline void FinishLlmEvent(LlmStreamEvent& ev) {
    if (ev.toolAction.empty() && !ev.toolName.empty()) ev.toolAction = "Calling " + ev.toolName;
    if (ev.content.empty()) ev.content = ev.toolInput.empty() ? ev.toolAction : ev.toolInput;
}

// Per-agent entry points. Each returns false when the line is not its format, so the caller
// falls back to the generic parser. A split parser appends one event per block or step.
bool ParseClaudeLines(const nlohmann::json& j, const std::string& trimmed, const std::string& agentType,
                      std::vector<LlmStreamEvent>& out);
bool ParseOpenCodeLines(const nlohmann::json& j, const std::string& trimmed, const std::string& agentType,
                        std::vector<LlmStreamEvent>& out);
bool ParseCodexLines(const nlohmann::json& j, const std::string& trimmed, const std::string& agentType,
                     std::vector<LlmStreamEvent>& out);

// agy's "init" / "step_update" lines, recognised by content. Fills outEvent for one event;
// the generic parser then applies its common finishing step.
bool ParseAgyLine(const nlohmann::json& j, LlmStreamEvent& outEvent);

}  // namespace llm
}  // namespace pc
