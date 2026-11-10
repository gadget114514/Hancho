#include "event_adapter.h"
#include "common.h"

#include <cctype>
#include <fstream>
#include <sstream>

namespace pc {

namespace {

std::string TrimLine(const std::string& line) {
    size_t first = 0;
    while (first < line.size() && std::isspace(static_cast<unsigned char>(line[first]))) ++first;
    size_t last = line.size();
    while (last > first && std::isspace(static_cast<unsigned char>(line[last - 1]))) --last;
    return line.substr(first, last - first);
}

std::string UnescapeToken(const std::string& token) {
    std::string out;
    out.reserve(token.size());
    for (size_t i = 0; i < token.size(); ++i) {
        if (token[i] == '~' && i + 1 < token.size()) {
            if (token[i + 1] == '0') { out.push_back('~'); ++i; continue; }
            if (token[i + 1] == '1') { out.push_back('/'); ++i; continue; }
        }
        out.push_back(token[i]);
    }
    return out;
}

bool Truthy(const nlohmann::json* value) {
    if (!value) return false;
    if (value->is_null()) return false;
    if (value->is_boolean()) return value->get<bool>();
    if (value->is_string()) return !value->get<std::string>().empty();
    if (value->is_number()) return value->get<double>() != 0.0;
    return true;
}

std::string StringField(const nlohmann::json& obj, const char* key) {
    auto it = obj.find(key);
    return (it != obj.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

uint64_t U64Field(const nlohmann::json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end()) return 0;
    if (it->is_number_unsigned()) return it->get<uint64_t>();
    if (it->is_number_integer()) return static_cast<uint64_t>(it->get<int64_t>());
    return 0;
}

int64_t I64Field(const nlohmann::json& obj, const char* key) {
    auto it = obj.find(key);
    if (it == obj.end()) return 0;
    if (it->is_number_integer()) return it->get<int64_t>();
    if (it->is_number_unsigned()) return static_cast<int64_t>(it->get<uint64_t>());
    return 0;
}

double NumField(const nlohmann::json& obj, const char* key) {
    auto it = obj.find(key);
    return (it != obj.end() && it->is_number()) ? it->get<double>() : 0.0;
}

bool BoolField(const nlohmann::json& obj, const char* key, bool fallback = true) {
    auto it = obj.find(key);
    return (it != obj.end() && it->is_boolean()) ? it->get<bool>() : fallback;
}

bool RuleMatches(const HanchoAdapterRule& rule, const nlohmann::json& line) {
    for (const auto& kv : rule.when) {
        const nlohmann::json* found = ResolveJsonPointer(line, kv.first);
        if (!found || *found != kv.second) return false;
    }
    return true;
}

} // namespace

bool IsHanchoEventKind(const std::string& kind) {
    static const char* kKinds[] = {
        "run.started", "turn.started", "tool.call", "tool.result", "thought", "message",
        "plan", "usage", "rate_limit", "subagent.started", "subagent.ended", "run.ended",
        "error", "raw"};
    for (const char* k : kKinds) {
        if (kind == k) return true;
    }
    return false;
}

std::string HanchoAdapterDirectory() {
    return WideToUtf8(GetEffectiveDataDirectoryWide()) + "/adapters";
}

std::string HanchoAdapterPathFor(const std::string& name) {
    if (name.empty()) return "";
    return HanchoAdapterDirectory() + "/" + name + ".json";
}

const nlohmann::json* ResolveJsonPointer(const nlohmann::json& root, const std::string& pointer) {
    if (pointer.empty() || pointer[0] != '/') return nullptr;
    const nlohmann::json* current = &root;
    size_t i = 1;
    while (i <= pointer.size()) {
        size_t slash = pointer.find('/', i);
        std::string key = UnescapeToken(pointer.substr(i, slash == std::string::npos ? std::string::npos : slash - i));
        if (current->is_object()) {
            auto it = current->find(key);
            if (it == current->end()) return nullptr;
            current = &*it;
        } else if (current->is_array()) {
            if (key.empty()) return nullptr;
            size_t index = 0;
            for (char c : key) {
                if (!std::isdigit(static_cast<unsigned char>(c))) return nullptr;
                index = index * 10 + static_cast<size_t>(c - '0');
            }
            if (index >= current->size()) return nullptr;
            current = &(*current)[index];
        } else {
            return nullptr;
        }
        if (slash == std::string::npos) break;
        i = slash + 1;
    }
    return current;
}

bool LoadHanchoAdapterFromJson(const nlohmann::json& root, HanchoAdapter& out, std::string& err) {
    err.clear();
    out = HanchoAdapter{};
    if (!root.is_object()) {
        err = "adapter must be a JSON object";
        return false;
    }
    if (root.contains("adapter")) {
        if (!root["adapter"].is_string() || root["adapter"].get<std::string>().empty()) {
            err = "'adapter' must be a non-empty string";
            return false;
        }
        out.name = root["adapter"].get<std::string>();
    } else if (root.contains("name") && root["name"].is_string()) {
        out.name = root["name"].get<std::string>();
    }
    if (out.name.empty()) {
        err = "'adapter' (name) is required";
        return false;
    }

    if (root.contains("match")) {
        const auto& match = root["match"];
        if (!match.is_object()) {
            err = "'match' must be an object";
            return false;
        }
        if (match.contains("field")) {
            if (!match["field"].is_string()) {
                err = "'match.field' must be a string";
                return false;
            }
            out.matchField = match["field"].get<std::string>();
        }
    }

    if (root.contains("exec")) {
        if (!root["exec"].is_string()) {
            err = "'exec' must be a string";
            return false;
        }
        out.exec = root["exec"].get<std::string>();
    }

    auto rulesIt = root.find("rules");
    if (rulesIt == root.end() || !rulesIt->is_array()) {
        if (out.exec.empty()) {
            err = "'rules' must be a non-empty array (or provide 'exec' for a Tier 3 adapter)";
            return false;
        }
        rulesIt = root.end();
    } else if (rulesIt->empty() && out.exec.empty()) {
        err = "'rules' must be a non-empty array";
        return false;
    }
    if (rulesIt != root.end()) {
        for (const auto& item : *rulesIt) {
            if (!item.is_object()) {
                err = "each rule must be an object";
                return false;
            }
            HanchoAdapterRule rule;
            if (!item.contains("kind") || !item["kind"].is_string() ||
                !IsHanchoEventKind(item["kind"].get<std::string>())) {
                err = "each rule needs a known 'kind'";
                return false;
            }
            rule.kind = item["kind"].get<std::string>();

            if (item.contains("when")) {
                if (!item["when"].is_object()) {
                    err = "'when' must be an object";
                    return false;
                }
                for (auto it = item["when"].begin(); it != item["when"].end(); ++it) {
                    rule.when[it.key()] = it.value();
                }
            }
            if (item.contains("map")) {
                if (!item["map"].is_object()) {
                    err = "'map' must be an object";
                    return false;
                }
                for (auto it = item["map"].begin(); it != item["map"].end(); ++it) {
                    if (!it.value().is_string()) {
                        err = "'map' values must be JSON Pointer strings";
                        return false;
                    }
                    rule.map[it.key()] = it.value().get<std::string>();
                }
            }
            if (item.contains("activity")) {
                const auto& activity = item["activity"];
                if (!activity.is_object() || !activity.contains("from") || !activity["from"].is_string()) {
                    err = "'activity' needs a 'from' JSON Pointer";
                    return false;
                }
                rule.hasActivity = true;
                rule.activityFrom = activity["from"].get<std::string>();
                if (activity.contains("table")) {
                    if (!activity["table"].is_object()) {
                        err = "'activity.table' must be an object";
                        return false;
                    }
                    for (auto it = activity["table"].begin(); it != activity["table"].end(); ++it) {
                        if (!it.value().is_string()) {
                            err = "'activity.table' values must be strings";
                            return false;
                        }
                        rule.activityTable[it.key()] = it.value().get<std::string>();
                    }
                }
                if (activity.contains("default")) {
                    if (!activity["default"].is_string()) {
                        err = "'activity.default' must be a string";
                        return false;
                    }
                    rule.activityDefault = activity["default"].get<std::string>();
                }
            }
            out.rules.push_back(std::move(rule));
        }
    }
    return true;
}

bool LoadHanchoAdapterFile(const std::string& path, HanchoAdapter& out, std::string& err) {
    err.clear();
    out = HanchoAdapter{};
    if (path.empty()) {
        err = "adapter path is empty";
        return false;
    }
    std::ifstream in(Utf8ToWide(path), std::ios::binary);
    if (!in) {
        err = "adapter file not found: " + path;
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    nlohmann::json root = nlohmann::json::parse(buffer.str(), nullptr, false);
    if (root.is_discarded()) {
        err = "adapter file is not valid JSON: " + path;
        return false;
    }
    return LoadHanchoAdapterFromJson(root, out, err);
}

std::string NormalizeHanchoAdapterLine(const HanchoAdapter& adapter, const std::string& line,
                                       nlohmann::json& data) {
    data = nlohmann::json::object();
    nlohmann::json parsed = nlohmann::json::parse(TrimLine(line), nullptr, false);
    if (!adapter.matchField.empty() && (!parsed.is_object() || !ResolveJsonPointer(parsed, adapter.matchField))) {
        data["text"] = line;
        return "raw";
    }

    for (const auto& rule : adapter.rules) {
        if (!parsed.is_object() || !RuleMatches(rule, parsed)) continue;

        data = nlohmann::json::object();
        for (const auto& kv : rule.map) {
            std::string pointer = kv.second;
            bool negate = false;
            if (!pointer.empty() && pointer[0] == '!') {
                negate = true;
                pointer = pointer.substr(1);
            }
            const nlohmann::json* value = ResolveJsonPointer(parsed, pointer);
            if (negate) {
                data[kv.first] = !Truthy(value);
            } else if (value) {
                data[kv.first] = *value;
            }
        }

        if (rule.hasActivity) {
            std::string activity = rule.activityDefault;
            const nlohmann::json* from = ResolveJsonPointer(parsed, rule.activityFrom);
            if (from && from->is_string()) {
                auto it = rule.activityTable.find(from->get<std::string>());
                if (it != rule.activityTable.end()) activity = it->second;
            }
            if (!activity.empty()) data["activity"] = activity;
        }
        return rule.kind;
    }

    data["text"] = line;
    return "raw";
}

LlmStreamEvent LlmStreamEventFromHanchoV1(const nlohmann::json& event) {
    LlmStreamEvent ev;
    ev.type = "raw";
    ev.transient = false;
    const std::string kind = StringField(event, "kind");
    const nlohmann::json data = event.contains("data") ? event["data"] : nlohmann::json::object();

    if (kind == "tool.call") {
        ev.type = "tool_call";
        ev.toolName = StringField(data, "tool");
        ev.toolUseId = StringField(data, "toolUseId");
        auto input = data.find("input");
        if (input != data.end()) {
            ev.toolInput = input->is_string() ? input->get<std::string>() : input->dump();
        }
    } else if (kind == "tool.result") {
        ev.type = "tool_result";
        ev.toolUseId = StringField(data, "toolUseId");
        ev.toolError = !BoolField(data, "ok", true);
        ev.content = StringField(data, "error");
        if (ev.content.empty()) ev.content = StringField(data, "summary");
    } else if (kind == "thought") {
        ev.type = "thinking";
        ev.content = StringField(data, "text");
    } else if (kind == "message") {
        ev.type = "message";
        ev.content = StringField(data, "text");
    } else if (kind == "plan") {
        ev.type = "tool_call";
        ev.toolName = "TodoWrite";
        nlohmann::json todos = nlohmann::json::array();
        auto items = data.find("items");
        if (items != data.end() && items->is_array()) todos = *items;
        ev.toolInput = nlohmann::json{{"todos", todos}}.dump();
    } else if (kind == "usage") {
        ev.type = "usage";
        ev.inputTokens = U64Field(data, "inputTokens");
        ev.outputTokens = U64Field(data, "outputTokens");
        ev.thinkingTokens = U64Field(data, "thinkingTokens");
        ev.cacheReadTokens = U64Field(data, "cacheReadTokens");
        ev.cacheWriteTokens = U64Field(data, "cacheWriteTokens");
        ev.totalTokens = U64Field(data, "totalTokens");
        ev.costUsd = NumField(data, "costUsd");
    } else if (kind == "rate_limit") {
        ev.type = "rate_limit";
        ev.rateLimitStatus = StringField(data, "status");
        ev.rateLimitType = StringField(data, "window");
        ev.rateLimitUtilization = data.contains("utilization") ? NumField(data, "utilization") : -1.0;
        ev.rateLimitResetsAtSec = I64Field(data, "resetsAt");
    } else if (kind == "subagent.started") {
        ev.type = "subagent_start";
        ev.agentId = StringField(data, "agentId");
        ev.subagentType = StringField(data, "type");
        ev.taskDescription = StringField(data, "description");
        ev.toolUseId = StringField(data, "toolUseId");
    } else if (kind == "subagent.ended") {
        ev.type = "subagent_end";
        ev.agentId = StringField(data, "agentId");
        ev.taskStatus = StringField(data, "status");
        ev.taskTokens = U64Field(data, "tokens");
        ev.taskToolUses = U64Field(data, "toolUses");
        ev.taskDurationMs = I64Field(data, "durationMs");
    } else if (kind == "run.started") {
        ev.type = "status";
        ev.content = "init";
        ev.cwd = StringField(data, "cwd");
        ev.toolsOffered = static_cast<int>(U64Field(data, "toolsOffered"));
    } else if (kind == "turn.started") {
        ev.type = "step";
        ev.content = "step_start";
        ev.messageId = StringField(data, "messageId");
    } else if (kind == "run.ended") {
        ev.type = "result";
        ev.status = StringField(data, "status");
        ev.stopReason = StringField(data, "stopReason");
        ev.numTurns = static_cast<int>(U64Field(data, "numTurns"));
        ev.apiDurationMs = I64Field(data, "durationMs");
        ev.permissionDenials = static_cast<int>(U64Field(data, "permissionDenials"));
    } else if (kind == "error") {
        ev.type = "error";
        ev.content = StringField(data, "message");
    }
    return ev;
}

} // namespace pc
