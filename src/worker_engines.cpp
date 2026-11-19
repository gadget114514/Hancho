#include "worker_engines.h"
#include "permit_config.h"
#include "platform/platform.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>

namespace pc {

using nlohmann::json;

std::string QuoteCommandArg(const std::string& s) {
    return platform::QuoteCommandArg(s);
}

namespace {

std::string LowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string JoinStrings(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += sep;
        out += parts[i];
    }
    return out;
}

void ReplaceAll(std::string& s, const std::string& from, const std::string& to) {
    if (from.empty()) return;
    size_t pos = 0;
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, from.size(), to);
        pos += to.size();
    }
}

std::string CollapseSpaces(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool inSpace = true; // trim leading spaces
    for (char c : s) {
        if (c == ' ' || c == '\t') {
            if (!inSpace) {
                out.push_back(' ');
                inSpace = true;
            }
        } else {
            out.push_back(c);
            inSpace = false;
        }
    }
    if (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

bool NeedsArgQuoting(const std::string& s) {
    return platform::NeedsCommandArgQuoting(s);
}

bool IsValidModelCharacters(const std::string& model) {
    for (unsigned char c : model) {
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

bool MatchesJsonType(const json& val, const std::string& typeName, bool& outKnownType) {
    outKnownType = true;
    if (typeName == "object") return val.is_object();
    if (typeName == "array") return val.is_array();
    if (typeName == "string") return val.is_string();
    if (typeName == "number") return val.is_number();
    if (typeName == "integer") return val.is_number_integer();
    if (typeName == "boolean") return val.is_boolean();
    if (typeName == "null") return val.is_null();
    outKnownType = false;
    return false;
}

bool ValidateValueAgainstSchema(const json& val, const json& schema, const std::string& path, std::string& err) {
    if (schema.is_null()) return true;
    if (schema.is_string()) {
        bool known = false;
        std::string t = schema.get<std::string>();
        if (!MatchesJsonType(val, t, known)) {
            err = path + ": expected type '" + t + "'";
            return false;
        }
        return true;
    }
    if (!schema.is_object()) {
        err = path + ": invalid schema (expected object)";
        return false;
    }

    auto typeIt = schema.find("type");
    if (typeIt != schema.end()) {
        if (typeIt->is_string()) {
            bool known = false;
            std::string t = typeIt->get<std::string>();
            if (!MatchesJsonType(val, t, known)) {
                err = known ? (path + ": expected type '" + t + "'")
                            : (path + ": unsupported schema type '" + t + "'");
                return false;
            }
        } else if (typeIt->is_array()) {
            bool anyMatch = false;
            for (const auto& item : *typeIt) {
                if (!item.is_string()) {
                    err = path + ": schema 'type' array elements must be strings";
                    return false;
                }
                bool known = false;
                if (MatchesJsonType(val, item.get<std::string>(), known)) {
                    anyMatch = true;
                    break;
                }
                if (!known) {
                    err = path + ": unsupported schema type '" + item.get<std::string>() + "'";
                    return false;
                }
            }
            if (!anyMatch) {
                err = path + ": value does not match any allowed type";
                return false;
            }
        } else {
            err = path + ": schema 'type' must be a string or array";
            return false;
        }
    }

    auto reqIt = schema.find("required");
    if (reqIt != schema.end()) {
        if (!reqIt->is_array()) {
            err = path + ": schema 'required' must be an array";
            return false;
        }
        if (!val.is_object()) {
            err = path + ": expected object for 'required' check";
            return false;
        }
        for (const auto& reqItem : *reqIt) {
            if (!reqItem.is_string()) {
                err = path + ": schema 'required' entries must be strings";
                return false;
            }
            std::string key = reqItem.get<std::string>();
            if (!val.contains(key)) {
                err = path + ": missing required field '" + key + "'";
                return false;
            }
        }
    }

    auto propsIt = schema.find("properties");
    if (propsIt != schema.end()) {
        if (!propsIt->is_object()) {
            err = path + ": schema 'properties' must be an object";
            return false;
        }
        if (val.is_object()) {
            for (auto it = propsIt->begin(); it != propsIt->end(); ++it) {
                auto valProp = val.find(it.key());
                if (valProp != val.end()) {
                    std::string subPath = path == "$" ? it.key() : (path + "." + it.key());
                    if (!ValidateValueAgainstSchema(*valProp, it.value(), subPath, err)) {
                        return false;
                    }
                }
            }
        }
    }

    auto itemsIt = schema.find("items");
    if (itemsIt != schema.end() && val.is_array()) {
        for (size_t i = 0; i < val.size(); ++i) {
            std::string subPath = path + "[" + std::to_string(i) + "]";
            if (!ValidateValueAgainstSchema(val[i], *itemsIt, subPath, err)) {
                return false;
            }
        }
    }

    return true;
}

} // namespace

WorkerEngineRegistry::WorkerEngineRegistry() {
    ResetToBuiltins();
}

void WorkerEngineRegistry::ResetToBuiltins() {
    defaultEngine_ = "agy";
    engines_.clear();

    WorkerEngineDef agy;
    agy.command = "agy.exe -p {prompt} --output-format stream-json --model {model} {permissionArgs} --print-timeout {timeoutMin}m";
    agy.defaultModel = "gemini-3.8-flash-medium";
    agy.stream = "agy";
    agy.permissionArgs = "--dangerously-skip-permissions";
    agy.advicePermissionArgs = "--mode plan";
    engines_["agy"] = std::move(agy);

    WorkerEngineDef claude;
    claude.command = "claude -p {prompt} --output-format stream-json --verbose --model {model} {permissionArgs} {permitArgs}";
    claude.defaultModel = "";
    claude.stream = "claude";
    claude.permissionArgs = "--dangerously-skip-permissions";
    claude.advicePermissionArgs = "--permission-mode plan";
    engines_["claude"] = std::move(claude);

    WorkerEngineDef opencode;
    opencode.command = "opencode.cmd run --format json --model {model} {prompt}";
    opencode.defaultModel = "";
    opencode.stream = "opencode";
    opencode.permissionArgs = "";
    // 091-7: OPENCODE_PERMISSION is an inline permission block (v1) and has higher
    // precedence than the user's global opencode.json(c), which Hancho never touches.
    opencode.permitEnv = "OPENCODE_PERMISSION";
    // OpenCode has no CLI read-only switch; without --auto it asks (and a headless run denies).
    opencode.advicePermissionArgs = "";
    engines_["opencode"] = std::move(opencode);

    WorkerEngineDef codex;
    codex.command = "codex exec --json --model {model} {permissionArgs} {prompt}";
    codex.defaultModel = "";
    codex.stream = "codex";
    codex.permissionArgs = "--dangerously-bypass-approvals-and-sandbox";
    codex.advicePermissionArgs = "--sandbox read-only";
    engines_["codex"] = std::move(codex);
}

bool WorkerEngineRegistry::LoadFromJson(const std::string& jsonText, std::string& err) {
    err.clear();
    json root = json::parse(jsonText, nullptr, false);
    if (root.is_discarded() || !root.is_object()) {
        err = "engines.json must be a valid JSON object";
        return false;
    }

    auto enginesIt = root.find("engines");
    if (enginesIt != root.end()) {
        if (!enginesIt->is_object()) {
            err = "'engines' must be an object";
            return false;
        }
        for (auto it = enginesIt->begin(); it != enginesIt->end(); ++it) {
            const std::string& name = it.key();
            const json& obj = it.value();
            if (name.empty() || !obj.is_object()) {
                err = "engine definition '" + name + "' must be an object";
                return false;
            }
            auto existingIt = engines_.find(name);
            bool isExisting = (existingIt != engines_.end());
            WorkerEngineDef def = isExisting ? existingIt->second : WorkerEngineDef();

            if (obj.contains("command")) {
                if (!obj["command"].is_string() || obj["command"].get<std::string>().empty()) {
                    err = "engine '" + name + "': 'command' must be a non-empty string";
                    return false;
                }
                def.command = obj["command"].get<std::string>();
                if (!obj.contains("defaultModel")) def.defaultModel.clear();
                if (!obj.contains("permissionArgs")) def.permissionArgs.clear();
                if (!obj.contains("permitArgs")) def.permitArgs.clear();
                if (!obj.contains("permitEnv")) def.permitEnv.clear();
                if (!obj.contains("advicePermissionArgs")) def.advicePermissionArgs.clear();
                if (!obj.contains("models")) def.models.clear();
                if (!obj.contains("adapter")) def.adapter.clear();
                if (!obj.contains("capabilities")) def.capabilities = WorkerEngineCapabilities{};
            } else if (!isExisting) {
                err = "engine '" + name + "' is missing required 'command'";
                return false;
            }

            if (obj.contains("defaultModel")) {
                if (!obj["defaultModel"].is_string()) {
                    err = "engine '" + name + "': 'defaultModel' must be a string";
                    return false;
                }
                def.defaultModel = obj["defaultModel"].get<std::string>();
            }
            if (obj.contains("stream")) {
                if (!obj["stream"].is_string()) {
                    err = "engine '" + name + "': 'stream' must be a string";
                    return false;
                }
                def.stream = obj["stream"].get<std::string>();
            }
            if (obj.contains("permissionArgs")) {
                if (!obj["permissionArgs"].is_string()) {
                    err = "engine '" + name + "': 'permissionArgs' must be a string";
                    return false;
                }
                def.permissionArgs = obj["permissionArgs"].get<std::string>();
            }
            if (obj.contains("permitArgs")) {
                if (!obj["permitArgs"].is_string()) {
                    err = "engine '" + name + "': 'permitArgs' must be a string";
                    return false;
                }
                def.permitArgs = obj["permitArgs"].get<std::string>();
            }
            if (obj.contains("permitEnv")) {
                if (!obj["permitEnv"].is_string()) {
                    err = "engine '" + name + "': 'permitEnv' must be a string";
                    return false;
                }
                def.permitEnv = obj["permitEnv"].get<std::string>();
            }
            if (obj.contains("advicePermissionArgs")) {
                if (!obj["advicePermissionArgs"].is_string()) {
                    err = "engine '" + name + "': 'advicePermissionArgs' must be a string";
                    return false;
                }
                def.advicePermissionArgs = obj["advicePermissionArgs"].get<std::string>();
            }
            if (obj.contains("models")) {
                if (!obj["models"].is_array()) {
                    err = "engine '" + name + "': 'models' must be an array of strings";
                    return false;
                }
                def.models.clear();
                for (const auto& m : obj["models"]) {
                    if (!m.is_string() || m.get<std::string>().empty()) {
                        err = "engine '" + name + "': 'models' entries must be non-empty strings";
                        return false;
                    }
                    def.models.push_back(m.get<std::string>());
                }
            }
            if (obj.contains("adapter")) {
                if (!obj["adapter"].is_string()) {
                    err = "engine '" + name + "': 'adapter' must be a string";
                    return false;
                }
                def.adapter = obj["adapter"].get<std::string>();
            }
            if (obj.contains("capabilities")) {
                const json& caps = obj["capabilities"];
                if (!caps.is_object()) {
                    err = "engine '" + name + "': 'capabilities' must be an object";
                    return false;
                }
                for (const char* key : {"jsonStream", "resume", "reportsUsage", "reportsSubagents"}) {
                    if (!caps.contains(key)) continue;
                    if (!caps[key].is_boolean()) {
                        err = "engine '" + name + "': 'capabilities." + std::string(key) + "' must be a boolean";
                        return false;
                    }
                }
                if (caps.contains("jsonStream")) def.capabilities.jsonStream = caps["jsonStream"].get<bool>();
                if (caps.contains("resume")) def.capabilities.resume = caps["resume"].get<bool>();
                if (caps.contains("reportsUsage")) def.capabilities.reportsUsage = caps["reportsUsage"].get<bool>();
                if (caps.contains("reportsSubagents")) def.capabilities.reportsSubagents = caps["reportsSubagents"].get<bool>();
                if (caps.contains("permissionModes")) {
                    if (!caps["permissionModes"].is_array()) {
                        err = "engine '" + name + "': 'capabilities.permissionModes' must be an array of strings";
                        return false;
                    }
                    def.capabilities.permissionModes.clear();
                    for (const auto& mode : caps["permissionModes"]) {
                        if (!mode.is_string()) {
                            err = "engine '" + name + "': 'capabilities.permissionModes' entries must be strings";
                            return false;
                        }
                        def.capabilities.permissionModes.push_back(mode.get<std::string>());
                    }
                }
            }

            engines_[name] = std::move(def);
        }
    }

    auto defIt = root.find("default");
    if (defIt != root.end()) {
        if (!defIt->is_string() || defIt->get<std::string>().empty()) {
            err = "'default' must be a non-empty string";
            return false;
        }
        std::string d = defIt->get<std::string>();
        if (engines_.find(d) == engines_.end()) {
            err = "default engine '" + d + "' is not defined";
            return false;
        }
        defaultEngine_ = d;
    }

    return true;
}

const WorkerEngineDef* WorkerEngineRegistry::Find(const std::string& engine) const {
    std::string engName = engine.empty() ? defaultEngine_ : engine;
    auto it = engines_.find(engName);
    if (it == engines_.end()) return nullptr;
    return &it->second;
}

bool WorkerEngineRegistry::Knows(const std::string& engine, const std::string& model, std::string& err) const {
    err.clear();
    std::string engName = engine.empty() ? defaultEngine_ : engine;
    if (engName.empty()) {
        err = "no engine specified and no default engine configured";
        return false;
    }
    auto it = engines_.find(engName);
    if (it == engines_.end()) {
        err = "unknown engine: " + engName;
        return false;
    }
    const WorkerEngineDef& def = it->second;
    if (!model.empty()) {
        if (!IsValidModelCharacters(model)) {
            err = "invalid characters in model name for engine '" + engName + "'";
            return false;
        }
        if (def.command.find("{model}") == std::string::npos && def.models.empty()) {
            err = "engine '" + engName + "' does not support a model parameter";
            return false;
        }
        if (!def.models.empty()) {
            bool found = (model == def.defaultModel) ||
                         (std::find(def.models.begin(), def.models.end(), model) != def.models.end());
            if (!found) {
                err = "unknown model '" + model + "' for engine '" + engName + "'";
                return false;
            }
        }
    }
    return true;
}

bool WorkerEngineRegistry::BuildCommand(const std::string& engine,
                                        const std::string& model,
                                        const std::string& launchPrompt,
                                        int timeoutSec,
                                        std::string& outCommand,
                                        std::string& outStreamType,
                                        std::string& err,
                                         std::string* outAdapter,
                                         const std::string* permissionArgsOverride,
                                         const std::string* permitArgsOverride) const {
    outCommand.clear();
    outStreamType.clear();
    if (outAdapter) outAdapter->clear();
    if (!Knows(engine, model, err)) return false;

    std::string engName = engine.empty() ? defaultEngine_ : engine;
    const WorkerEngineDef& def = engines_.at(engName);
    if (outAdapter) *outAdapter = def.adapter;

    std::string resolvedModel = !model.empty() ? model : def.defaultModel;
    std::string cmd = def.command;

    if (resolvedModel.empty()) {
        ReplaceAll(cmd, "--model {model}", "");
        ReplaceAll(cmd, "-m {model}", "");
        ReplaceAll(cmd, "{model}", "");
    } else {
        std::string modelArg = NeedsArgQuoting(resolvedModel) ? QuoteCommandArg(resolvedModel) : resolvedModel;
        ReplaceAll(cmd, "{model}", modelArg);
    }

    ReplaceAll(cmd, "{permissionArgs}", permissionArgsOverride ? *permissionArgsOverride : def.permissionArgs);
    ReplaceAll(cmd, "{permitArgs}", permitArgsOverride ? *permitArgsOverride : def.permitArgs);

    int timeoutMin = timeoutSec > 0 ? (timeoutSec + 59) / 60 : 30;
    if (timeoutMin < 1) timeoutMin = 1;
    ReplaceAll(cmd, "{timeoutMin}", std::to_string(timeoutMin));

    // Normalize whitespace before injecting {prompt} so spaces inside launchPrompt are untouched.
    cmd = CollapseSpaces(cmd);

    std::string quotedPrompt = QuoteCommandArg(launchPrompt);
    ReplaceAll(cmd, "{prompt}", quotedPrompt);

    outCommand = cmd;
    outStreamType = def.stream;
    return true;
}

std::string BuildWorkerPermitsJson(const nlohmann::json& approved) {
    json out = json::object();
    out["permits"] = approved.is_array() ? approved : json::array();
    return out.dump();
}

std::string WorkerEngineRegistry::BuildPermitEnvironment(const std::string& engine,
                                                          const nlohmann::json& approved,
                                                          std::vector<std::string>& warnings) const {
    const WorkerEngineDef* def = Find(engine);
    if (!def) {
        if (!approved.empty()) warnings.push_back("unknown engine '" + engine + "': permits not applied");
        return "";
    }
    if (def->permitEnv.empty()) return ""; // engine has no engine-specific permission config
    if (LowerAscii(engine) != "opencode") {
        if (!approved.empty()) warnings.push_back("engine '" + engine + "': permit conversion is not implemented");
        return "";
    }
    // opencode v1 (verified against 1.19.5 config.ts): OPENCODE_PERMISSION is JSON.parse'd and
    // merged into `permission`, so the value must be the rules object itself (no "permission"
    // key). A "deny" rule returns a tool error instead of prompting, so a headless session
    // continues; "ask" in a non-interactive session would end it.
    json external = json{{"*", "deny"}};
    json read = json{{"*.env", "deny"}, {"*.env.*", "deny"}};
    json edit = json::object();
    json bash = json{{"*", "deny"}};
    json webfetch = json{{"*", "deny"}};
    for (const auto& row : approved) {
        if (!row.is_object()) continue;
        const std::string kind = row.value("kind", std::string());
        const std::string target = permit::ExpandTarget(row.value("target", std::string()));
        if (target.empty()) continue;
        if (kind == "read_path") {
            external[target] = "allow";
            read[target] = "allow";
        } else if (kind == "write_path") {
            external[target] = "allow";
            edit[target] = "allow";
        } else if (kind == "command") {
            bash[target] = "allow";
        } else if (kind == "network") {
            webfetch[target] = "allow";
        }
    }
    json permission;
    permission["external_directory"] = std::move(external);
    permission["read"] = std::move(read);
    permission["edit"] = std::move(edit);
    permission["bash"] = std::move(bash);
    permission["webfetch"] = std::move(webfetch);
    return permission.dump();
}

std::string WorkerEngineRegistry::BuildPermitArgs(const std::string& engine,
                                                   const nlohmann::json& approved,
                                                   std::vector<std::string>& warnings) const {
    const WorkerEngineDef* def = Find(engine);
    if (!def || approved.empty()) return "";
    if (LowerAscii(engine) != "claude") {
        if (!def->permitEnv.empty() || def->command.find("--dangerously-skip-permissions") == std::string::npos) {
            warnings.push_back("engine '" + engine + "': permitted paths cannot be passed as command arguments");
        }
        return "";
    }
    std::vector<std::string> args;
    std::vector<std::string> allowed;
    for (const auto& row : approved) {
        if (!row.is_object()) continue;
        const std::string kind = row.value("kind", std::string());
        const std::string target = permit::ExpandTarget(row.value("target", std::string()));
        if (target.empty()) continue;
        if (kind == "read_path") {
            args.push_back("--add-dir " + QuoteCommandArg(target));
            allowed.push_back(QuoteCommandArg("Read(" + target + ")"));
        } else if (kind == "write_path") {
            args.push_back("--add-dir " + QuoteCommandArg(target));
            allowed.push_back(QuoteCommandArg("Edit(" + target + ")"));
        } else if (kind == "command") {
            allowed.push_back(QuoteCommandArg("Bash(" + target + ")"));
        } else if (kind == "network") {
            allowed.push_back(QuoteCommandArg("WebFetch(" + target + ")"));
        }
    }
    if (!allowed.empty()) args.push_back("--allowedTools " + JoinStrings(allowed, ","));
    return JoinStrings(args, " ");
}

bool ValidateOutput(const std::string& jsonText, const std::string& outputSchemaJson, std::string& err) {
    err.clear();
    if (outputSchemaJson.empty()) return true;

    json schema = json::parse(outputSchemaJson, nullptr, false);
    if (schema.is_discarded()) {
        err = "invalid outputSchema JSON";
        return false;
    }
    if (schema.is_null() || (schema.is_object() && schema.empty())) return true;

    if (jsonText.empty()) {
        err = "output is empty";
        return false;
    }
    json val = json::parse(jsonText, nullptr, false);
    if (val.is_discarded()) {
        err = "output is not valid JSON";
        return false;
    }

    return ValidateValueAgainstSchema(val, schema, "$", err);
}

} // namespace pc
