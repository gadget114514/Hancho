#include "worker_engines.h"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <cctype>

namespace pc {

using nlohmann::json;

std::string QuoteCommandArg(const std::string& s) {
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : s) {
        if (c == '\\') {
            backslashes++;
            out.push_back(c);
        } else if (c == '"') {
            out.append(backslashes + 1, '\\');
            out.push_back('"');
            backslashes = 0;
        } else {
            backslashes = 0;
            out.push_back(c);
        }
    }
    out.append(backslashes, '\\');
    out.push_back('"');
    return out;
}

namespace {

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
    for (char c : s) {
        if (std::isspace(static_cast<unsigned char>(c)) ||
            c == '"' || c == '&' || c == '|' || c == '<' || c == '>' || c == '^' || c == '(' || c == ')') {
            return true;
        }
    }
    return false;
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
    engines_["agy"] = std::move(agy);

    WorkerEngineDef claude;
    claude.command = "claude -p {prompt} --output-format stream-json --verbose --model {model} {permissionArgs}";
    claude.defaultModel = "";
    claude.stream = "claude";
    claude.permissionArgs = "--dangerously-skip-permissions";
    engines_["claude"] = std::move(claude);

    WorkerEngineDef opencode;
    opencode.command = "opencode.cmd run --format json --model {model} {prompt}";
    opencode.defaultModel = "";
    opencode.stream = "opencode";
    opencode.permissionArgs = "";
    engines_["opencode"] = std::move(opencode);

    WorkerEngineDef codex;
    codex.command = "codex exec --json --model {model} {permissionArgs} {prompt}";
    codex.defaultModel = "";
    codex.stream = "codex";
    codex.permissionArgs = "--dangerously-bypass-approvals-and-sandbox";
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
                if (!obj.contains("models")) def.models.clear();
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
                                        std::string& err) const {
    outCommand.clear();
    outStreamType.clear();
    if (!Knows(engine, model, err)) return false;

    std::string engName = engine.empty() ? defaultEngine_ : engine;
    const WorkerEngineDef& def = engines_.at(engName);

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

    ReplaceAll(cmd, "{permissionArgs}", def.permissionArgs);

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
