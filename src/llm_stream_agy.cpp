// agy (Antigravity) stream-json: top-level "event", with nested "step_update" or "init".
// The generic parser calls this for any agent, because these lines are recognised by content.

#include "llm_stream_internal.h"

namespace pc {
namespace llm {

bool ParseAgyLine(const nlohmann::json& j, LlmStreamEvent& outEvent) {
    if (!j.contains("init") && !j.contains("step_update")) return false;

    const nlohmann::json* su = nullptr;
    if (j.contains("step_update") && j["step_update"].is_object()) su = &j["step_update"];

    if (su && su->contains("usage") && (*su)["usage"].is_object()) {
        ApplyLlmUsageFields((*su)["usage"], outEvent);
    } else if (j.contains("usage") && j["usage"].is_object()) {
        ApplyLlmUsageFields(j["usage"], outEvent);
    }

    if (j.contains("init")) {
        outEvent.type = "status";
        outEvent.status = "running";
        outEvent.content = "init";
        return true;
    }

    std::string stepType = su ? JsonStr(*su, "step_type") : "";
    std::string stateStr = su ? JsonStr(*su, "state") : "";

    if (stepType == "user_input") {
        outEvent.type = "user_input";
        outEvent.status = "generating";
        return true;
    }
    if (stepType == "agent_response") {
        outEvent.type = "message";
        outEvent.status = "generating";
        return true;
    }
    if (stepType == "tool") {
        std::string tname = su ? JsonStr(*su, "tool_name") : "";
        if (tname.empty() && su && su->contains("tool_info") && (*su)["tool_info"].is_object()) {
            tname = JsonStr((*su)["tool_info"], "name");
            if (tname.empty()) tname = JsonStr((*su)["tool_info"], "tool_name");
        }
        outEvent.toolName = tname;
        if (su && su->contains("tool_info") && (*su)["tool_info"].is_object()) {
            const auto& ti = (*su)["tool_info"];
            if (ti.contains("parameters")) {
                outEvent.toolInput = ti["parameters"].is_string() ? ti["parameters"].get<std::string>() : ti["parameters"].dump();
            }
            std::string to;
            if (ti.contains("output")) {
                to = ti["output"].is_string() ? ti["output"].get<std::string>() : ti["output"].dump();
            }
            if (stateStr == "DONE" || !to.empty()) {
                outEvent.type = "tool_result";
                outEvent.status = "tool_result";
                outEvent.content = to;
                if (outEvent.toolAction.empty()) outEvent.toolAction = outEvent.toolName.empty() ? "Result" : ("Result: " + outEvent.toolName);
            } else {
                outEvent.type = "tool_call";
                outEvent.status = "calling_tool";
                if (outEvent.toolAction.empty() && !outEvent.toolName.empty()) outEvent.toolAction = "Calling " + outEvent.toolName;
            }
        } else {
            outEvent.type = "tool_call";
            outEvent.status = "calling_tool";
            if (outEvent.toolAction.empty() && !outEvent.toolName.empty()) outEvent.toolAction = "Calling " + outEvent.toolName;
        }
        return true;
    }

    outEvent.type = "status";
    outEvent.status = stateStr.empty() ? "running" : LowerAscii(stateStr);
    return true;
}

}  // namespace llm
}  // namespace pc
