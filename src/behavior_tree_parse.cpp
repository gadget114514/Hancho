#include "behavior_tree_internal.h"
#include "common.h"
#include "permit_config.h"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <set>
#include <sstream>

namespace pc {

namespace {

struct ParseContext {
    const IBtEngineCatalog* catalog = nullptr;
    BtDefaults defaults;
    std::string btCwd;
    std::string baseQueue;  // tree-level task queue; parallel children get "<baseQueue>.p<hash>"
    std::set<std::string> seenIds;
    std::set<std::string> seenKeys; // ids and path keys, for validating node references
    int nodeCount = 0;
    std::vector<std::string> errors;

    void AddError(const std::string& path, const std::string& msg) {
        errors.push_back(path + ": " + msg);
    }
};

// Stable 8-hex FNV-1a hash. Used to keep per-parallel queue names short and deterministic:
// queue files truncate names to 32 chars, so the branch identity must fit in a few bytes.
std::string ShortHashHex(const std::string& s) {
    uint32_t hash = 2166136261u;
    for (unsigned char c : s) {
        hash ^= c;
        hash *= 16777619u;
    }
    char buf[9] = {};
    std::snprintf(buf, sizeof(buf), "%08x", hash);
    return buf;
}

// Queue scope of a parallel child: keep the parent scope when nested, then add this branch.
std::string ParallelChildQueue(const ParseContext& ctx, const std::string& parentScope,
                               const std::string& parallelPath, size_t childIndex) {
    const std::string& base = parentScope.empty() ? ctx.baseQueue : parentScope;
    return base + ".p" + ShortHashHex(parallelPath + "#" + std::to_string(childIndex));
}

std::string JsonToStrValue(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
    if (v.is_number()) return v.dump();
    return "";
}

bool ReqStr(const json& j, const char* key, const std::string& path, const std::string& nodeType,
            std::string& out, ParseContext& ctx) {
    if (!j.contains(key) || !j[key].is_string() || j[key].get<std::string>().empty()) {
        ctx.AddError(path, "'" + nodeType + "' requires non-empty string '" + key + "'");
        return false;
    }
    out = j[key].get<std::string>();
    return true;
}

bool ReqPosInt(const json& j, const char* key, const std::string& path, const std::string& nodeType,
               int& out, ParseContext& ctx) {
    if (!j.contains(key) || !j[key].is_number_integer() || j[key].get<int>() <= 0) {
        ctx.AddError(path, "'" + nodeType + "' requires positive integer '" + key + "'");
        return false;
    }
    out = j[key].get<int>();
    return true;
}

void ApplyNodeDefaults(const json& j, BtNode& node, ParseContext& ctx);

void ParseJobCommonFields(const json& j, const std::string& path, BtNode& node, ParseContext& ctx) {
    if (j.contains("cwd") && j["cwd"].is_string()) node.cwd = j["cwd"].get<std::string>();
    if (j.contains("maxAttempts") && j["maxAttempts"].is_number_integer()) {
        node.maxAttempts = j["maxAttempts"].get<int>();
        node.maxAttemptsConfigured = true;
        if (node.maxAttempts < 1) ctx.AddError(path + ".maxAttempts", "must be >= 1");
    }
    if (j.contains("probe")) {
        if (j["probe"].is_boolean()) {
            node.probe = j["probe"].get<bool>();
            node.probeConfigured = true;
        } else {
            ctx.AddError(path + ".probe", "must be a boolean");
        }
    }
    if (j.contains("env")) {
        if (!j["env"].is_object()) ctx.AddError(path + ".env", "must be an object");
        else {
            for (auto it = j["env"].begin(); it != j["env"].end(); ++it) {
                node.env[it.key()] = JsonToStrValue(it.value());
            }
        }
    }
    if (j.contains("idle")) {
        node.idle.configured = true;
        if (!j["idle"].is_object()) {
            if (j["idle"].is_number() && j["idle"].get<int>() == 0) {
                node.idle.outputSec = 0;
                node.idle.cpuSec = 0;
                node.idle.firstEventSec = 0;
            } else {
                ctx.AddError(path + ".idle", "must be an object");
            }
        } else {
            const auto& idl = j["idle"];
            node.idle.outputSec = idl.value("outputSec", idl.value("output_sec", 0));
            node.idle.cpuSec = idl.value("cpuSec", idl.value("cpu_sec", 0));
            node.idle.firstEventSec = idl.value("firstEventSec", idl.value("first_event_sec", 0));
        }
    }
    if (j.contains("success")) {
        if (!j["success"].is_object()) ctx.AddError(path + ".success", "must be an object");
        else {
            const json& s = j["success"];
            node.success.exitCode = s.value("exitCode", 0);
            if (s.contains("outputRegex") && s["outputRegex"].is_string()) {
                node.success.outputRegex = s["outputRegex"].get<std::string>();
            }
            if (s.contains("testReport") && s["testReport"].is_string()) {
                node.success.testReport = s["testReport"].get<std::string>();
            }
            if (s.contains("contentFile") && s["contentFile"].is_string()) {
                node.success.contentFile = s["contentFile"].get<std::string>();
            }
            if (s.contains("minBytes")) {
                if (s["minBytes"].is_number_integer()) node.success.minBytes = s["minBytes"].get<long long>();
                else ctx.AddError(path + ".success.minBytes", "must be an integer");
            }
            if (s.contains("contentRegex") && s["contentRegex"].is_string()) {
                node.success.contentRegex = s["contentRegex"].get<std::string>();
            }
            if (s.contains("engineResult")) {
                if (s["engineResult"].is_boolean()) node.success.engineResult = s["engineResult"].get<bool>();
                else ctx.AddError(path + ".success.engineResult", "must be a boolean");
            }
            if (s.contains("filesNewer") && s["filesNewer"].is_array()) {
                for (const auto& f : s["filesNewer"]) {
                    if (f.is_string()) node.success.filesNewer.push_back(f.get<std::string>());
                }
            }
        }
    }
    if (j.contains("resources")) {
        if (!j["resources"].is_object()) ctx.AddError(path + ".resources", "must be an object");
        else {
            const json& r = j["resources"];
            if (r.contains("write") && r["write"].is_array()) {
                for (const auto& w : r["write"]) if (w.is_string()) node.resources.write.push_back(w.get<std::string>());
            }
            if (r.contains("read") && r["read"].is_array()) {
                for (const auto& rd : r["read"]) if (rd.is_string()) node.resources.read.push_back(rd.get<std::string>());
            }
        }
    }
    if (j.contains("capture")) {
        if (!j["capture"].is_object()) ctx.AddError(path + ".capture", "must be an object");
        else {
            for (auto it = j["capture"].begin(); it != j["capture"].end(); ++it) {
                const std::string& varName = it.key();
                const json& rj = it.value();
                if (!rj.is_object()) {
                    ctx.AddError(path + ".capture." + varName, "capture rule must be an object");
                    continue;
                }
                BtCaptureRule rule;
                if (rj.contains("regex") && rj["regex"].is_string()) rule.regex = rj["regex"].get<std::string>();
                rule.group = rj.value("group", 1);
                if (rj.contains("jsonFile") && rj["jsonFile"].is_string()) rule.jsonFile = rj["jsonFile"].get<std::string>();
                if (rj.contains("path") && rj["path"].is_string()) rule.path = rj["path"].get<std::string>();
                if (rule.regex.empty() && rule.jsonFile.empty()) {
                    ctx.AddError(path + ".capture." + varName, "capture rule requires 'regex' or 'jsonFile'");
                }
                node.capture[varName] = std::move(rule);
            }
        }
    }
    ApplyNodeDefaults(j, node, ctx);
}

bool LooksLikeLlmJobCommand(const std::string& command) {
    std::string lower;
    lower.reserve(command.size());
    for (unsigned char c : command) lower.push_back(static_cast<char>(std::tolower(c)));
    if (lower.find("ww-run") != std::string::npos) return true;
    static const char* const kClis[] = {"claude", "opencode", "codex", "agy", "gemini", "aider", "llama"};
    for (const char* cli : kClis) {
        size_t pos = 0;
        while ((pos = lower.find(cli, pos)) != std::string::npos) {
            bool leftOk = pos == 0 || !std::isalnum(static_cast<unsigned char>(lower[pos - 1]));
            size_t after = pos + std::strlen(cli);
            bool rightOk = after >= lower.size() || !std::isalnum(static_cast<unsigned char>(lower[after]));
            if (leftOk && rightOk) return true;
            pos = after;
        }
    }
    return false;
}

bool IsLlmLikeNode(const json& j, const BtNode& node) {
    return node.type == "worker" || j.contains("engine") || j.contains("agentType") ||
           LooksLikeLlmJobCommand(node.command);
}

// Tree-level defaults fill only fields the node did not write itself; an explicit 0 wins.
void ApplyNodeDefaults(const json& j, BtNode& node, ParseContext& ctx) {
    const BtDefaults& d = ctx.defaults;
    bool llmLike = IsLlmLikeNode(j, node);
    if (!node.maxAttemptsConfigured && llmLike && d.workerMaxAttempts > 0) {
        node.maxAttempts = d.workerMaxAttempts;
    }
    if (!j.contains("idle")) {
        const BtJobIdle* src = nullptr;
        if (llmLike) {
            src = d.hasWorkerIdle() ? &d.workerIdle : (d.hasJobIdle() ? &d.jobIdle : nullptr);
        } else {
            src = d.hasJobIdle() ? &d.jobIdle : nullptr;
        }
        if (src) {
            node.idle.outputSec = src->outputSec;
            node.idle.cpuSec = src->cpuSec;
            node.idle.firstEventSec = src->firstEventSec;
            node.idle.configured = true;
        }
    }
    if (!node.probeConfigured && llmLike && d.hasWorkerProbe) node.probe = d.workerProbe;
}

// Shared by node "idle" and defaults.<worker|job>.idle.
void ParseIdleObject(const json& holder, const char* key, const std::string& path,
                     BtJobIdle& out, ParseContext& ctx) {
    if (!holder.contains(key)) return;
    out.configured = true;
    if (!holder[key].is_object()) {
        if (holder[key].is_number() && holder[key].get<int>() == 0) {
            out = BtJobIdle();
            out.configured = true;
        } else {
            ctx.AddError(path, "must be an object");
        }
        return;
    }
    const auto& idl = holder[key];
    out.outputSec = idl.value("outputSec", idl.value("output_sec", 0));
    out.cpuSec = idl.value("cpuSec", idl.value("cpu_sec", 0));
    out.firstEventSec = idl.value("firstEventSec", idl.value("first_event_sec", 0));
}

bool ParseComparisonOps(const json& j, BtNode& node) {
    bool any = false;
    if (j.contains("eq")) { node.hasEq = true; node.condEq = j["eq"]; any = true; }
    if (j.contains("ne")) { node.hasNe = true; node.condNe = j["ne"]; any = true; }
    if (j.contains("in")) { node.hasIn = true; node.condIn = j["in"]; any = true; }
    if (j.contains("gt")) { node.hasGt = true; node.condGt = j["gt"]; any = true; }
    if (j.contains("lt")) { node.hasLt = true; node.condLt = j["lt"]; any = true; }
    if (j.contains("exists") && j["exists"].is_boolean()) {
        node.hasExists = true;
        node.condExists = j["exists"].get<bool>();
        any = true;
    }
    return any;
}

std::unique_ptr<BtNode> ParseNode(const json& j, const std::string& path, const std::string& role,
                                  int depth, ParseContext& ctx, const std::string& queueScope) {
    auto node = std::make_unique<BtNode>();
    node->pathKey = path;
    node->role = role;
    node->depth = depth;
    node->queueName = queueScope;

    ctx.nodeCount++;
    if (ctx.nodeCount > 1000) { ctx.AddError(path, "tree exceeds maximum node count (1000)"); return node; }
    if (depth > 32) { ctx.AddError(path, "tree exceeds maximum depth (32)"); return node; }
    if (!j.is_object()) { ctx.AddError(path, "node must be a JSON object"); return node; }

    node->rawDef = j;
    node->rawDef.erase("status");

    if (j.contains("id")) {
        if (!j["id"].is_string() || j["id"].get<std::string>().empty()) {
            ctx.AddError(path + ".id", "id must be a non-empty string");
        } else {
            node->id = j["id"].get<std::string>();
            if (!ctx.seenIds.insert(node->id).second) ctx.AddError(path, "duplicate node id '" + node->id + "'");
        }
    }
    node->nodeKey = node->id.empty() ? node->pathKey : node->id;
    if (!node->id.empty()) ctx.seenKeys.insert(node->id);
    ctx.seenKeys.insert(node->pathKey);

    if (!j.contains("type") || !j["type"].is_string() || j["type"].get<std::string>().empty()) {
        ctx.AddError(path, "missing required string field 'type'");
        return node;
    }
    std::string rawType = j["type"].get<std::string>();
    node->type = (rawType == "selector") ? "fallback" : rawType;
    const std::string& t = node->type;

    auto requireChildObj = [&](const char* key, const std::string& childRole, std::unique_ptr<BtNode>& outSlot) {
        if (!j.contains(key) || !j[key].is_object()) {
            ctx.AddError(path, std::string("missing required object '") + key + "'");
            return;
        }
        outSlot = ParseNode(j[key], path + "." + key, childRole, depth + 1, ctx, queueScope);
    };

    if (t == "sequence" || t == "fallback" || t == "parallel") {
        if (!j.contains("children") || !j["children"].is_array() || j["children"].empty()) {
            ctx.AddError(path, "'" + rawType + "' requires a non-empty 'children' array");
        } else {
            size_t idx = 0;
            for (const auto& c : j["children"]) {
                // A parallel node's children may run at the same time, so each gets its own
                // queue. Serial composites keep the current scope.
                std::string childScope = (t == "parallel")
                    ? ParallelChildQueue(ctx, queueScope, path, idx)
                    : queueScope;
                node->children.push_back(ParseNode(c, path + ".children[" + std::to_string(idx) + "]", "",
                                                   depth + 1, ctx, childScope));
                idx++;
            }
        }
        if (t == "parallel" && j.contains("success")) {
            const json& s = j["success"];
            if (s.is_string() && (s.get<std::string>() == "all" || s.get<std::string>() == "any")) {
                node->parallelSuccessMode = s.get<std::string>();
            } else if (s.is_number_integer()) {
                int cnt = s.get<int>();
                int maxKids = static_cast<int>(node->children.size());
                if (cnt < 1 || (maxKids > 0 && cnt > maxKids)) {
                    ctx.AddError(path + ".success", "count must be between 1 and children count");
                }
                node->parallelSuccessMode = "count";
                node->parallelSuccessCount = cnt;
            } else {
                ctx.AddError(path + ".success", "must be 'all', 'any', or a positive integer");
            }
        }
    } else if (t == "if") {
        requireChildObj("cond", "cond", node->condChild);
        requireChildObj("then", "then", node->thenChild);
        if (j.contains("else")) {
            if (!j["else"].is_object()) ctx.AddError(path + ".else", "must be a node object");
            else node->elseChild = ParseNode(j["else"], path + ".else", "else", depth + 1, ctx, queueScope);
        }
    } else if (t == "retry" || t == "repeat") {
        if (!j.contains("max") || !j["max"].is_number_integer()) {
            ctx.AddError(path, "'" + t + "' requires integer field 'max'");
        } else {
            node->maxCount = j["max"].get<int>();
            if (node->maxCount < 1 || node->maxCount > 100) ctx.AddError(path + ".max", "'max' must be between 1 and 100");
        }
        node->delaySec = j.value("delaySec", j.value("retryDelaySec", 0));
        requireChildObj("child", "child", node->singleChild);
        if (t == "repeat" && j.contains("until")) {
            if (!j["until"].is_object()) ctx.AddError(path + ".until", "must be a node object");
            else node->untilChild = ParseNode(j["until"], path + ".until", "until", depth + 1, ctx, queueScope);
        }
    } else if (t == "catch") {
        requireChildObj("child", "child", node->singleChild);
        if (!j.contains("handlers") || !j["handlers"].is_array() || j["handlers"].empty()) {
            ctx.AddError(path + ".handlers", "'catch' requires a non-empty 'handlers' array");
        } else {
            size_t handlerIdx = 0;
            for (const auto& hj : j["handlers"]) {
                const std::string hPath = path + ".handlers[" + std::to_string(handlerIdx) + "]";
                BtCatchHandler h;
                if (!hj.is_object()) {
                    ctx.AddError(hPath, "handler must be an object");
                    handlerIdx++;
                    continue;
                }
                h.rawDef = hj;
                if (hj.contains("class")) {
                    const json& c = hj["class"];
                    if (c.is_string()) {
                        if (c.get<std::string>().empty()) ctx.AddError(hPath + ".class", "must be a non-empty string");
                        else h.classes.push_back(c.get<std::string>());
                    } else if (c.is_array()) {
                        if (c.empty()) ctx.AddError(hPath + ".class", "must not be an empty array");
                        for (const auto& item : c) {
                            if (!item.is_string() || item.get<std::string>().empty())
                                ctx.AddError(hPath + ".class", "elements must be non-empty strings");
                            else
                                h.classes.push_back(item.get<std::string>());
                        }
                    } else {
                        ctx.AddError(hPath + ".class", "must be a string or an array of strings");
                    }
                }
                if (hj.contains("cause")) {
                    if (!hj["cause"].is_string() || hj["cause"].get<std::string>().empty())
                        ctx.AddError(hPath + ".cause", "must be a non-empty string");
                    else
                        h.cause = hj["cause"].get<std::string>();
                }
                if (hj.contains("thenRetry")) {
                    if (!hj["thenRetry"].is_boolean()) ctx.AddError(hPath + ".thenRetry", "must be a boolean");
                    else h.thenRetry = hj["thenRetry"].get<bool>();
                }
                if (!hj.contains("do") || !hj["do"].is_object()) {
                    ctx.AddError(hPath, "handler requires object 'do'");
                } else {
                    h.doNode = ParseNode(hj["do"], hPath + ".do", "do", depth + 1, ctx, queueScope);
                }
                node->catchHandlers.push_back(std::move(h));
                handlerIdx++;
            }
        }
        node->catchMaxRetries = 1;
        if (j.contains("maxRetries")) {
            if (!j["maxRetries"].is_number_integer()) ctx.AddError(path + ".maxRetries", "must be an integer between 1 and 10");
            else {
                node->catchMaxRetries = j["maxRetries"].get<int>();
                if (node->catchMaxRetries < 1 || node->catchMaxRetries > 10)
                    ctx.AddError(path + ".maxRetries", "must be between 1 and 10");
            }
        }
        if (j.contains("otherwise")) {
            const json& o = j["otherwise"];
            if (o.is_string()) {
                if (o.get<std::string>() != "fail")
                    ctx.AddError(path + ".otherwise", "must be 'fail' or a node object");
            } else if (o.is_object()) {
                node->otherwiseChild = ParseNode(o, path + ".otherwise", "otherwise", depth + 1, ctx, queueScope);
            } else {
                ctx.AddError(path + ".otherwise", "must be 'fail' or a node object");
            }
        }
    } else if (t == "invert" || t == "forceSuccess" || t == "forceFailure") {
        requireChildObj("child", "child", node->singleChild);
    } else if (t == "timeout") {
        ReqPosInt(j, "sec", path, t, node->sec, ctx);
        requireChildObj("child", "child", node->singleChild);
    } else if (t == "job") {
        ReqStr(j, "command", path, t, node->command, ctx);
        ReqStr(j, "summary", path, t, node->summary, ctx);
        ReqPosInt(j, "timeoutSec", path, t, node->timeoutSec, ctx);
        if (j.contains("estimateSec")) ReqPosInt(j, "estimateSec", path, t, node->estimateSec, ctx);
        if (j.contains("instructionsText") && j["instructionsText"].is_string()) node->instructionsText = j["instructionsText"].get<std::string>();
        if (j.contains("instructionsFile") && j["instructionsFile"].is_string()) node->instructionsFile = j["instructionsFile"].get<std::string>();
        if (j.contains("engine") && j["engine"].is_string()) node->engine = j["engine"].get<std::string>();
        if (j.contains("model") && j["model"].is_string()) node->model = j["model"].get<std::string>();
        if (j.contains("engines")) {
            if (!j["engines"].is_array() || j["engines"].empty()) {
                ctx.AddError(path + ".engines", "must be a non-empty array of {engine, model} objects");
            } else {
                if (!node->engine.empty() || !node->model.empty()) {
                    ctx.AddError(path, "'engine'/'model' and 'engines' cannot both be specified");
                }
                for (size_t i = 0; i < j["engines"].size(); ++i) {
                    const json& c = j["engines"][i];
                    const std::string cpath = path + ".engines[" + std::to_string(i) + "]";
                    if (!c.is_object() || !c.contains("engine") || !c["engine"].is_string() ||
                        c["engine"].get<std::string>().empty()) {
                        ctx.AddError(cpath, "requires a non-empty string 'engine'");
                        continue;
                    }
                    BtEngineChoice choice;
                    choice.engine = c["engine"].get<std::string>();
                    if (c.contains("model") && c["model"].is_string()) choice.model = c["model"].get<std::string>();
                    if (ctx.catalog && !ctx.catalog->IsApprovedForList("worker", choice.engine, choice.model)) {
                        ctx.AddError(cpath, "candidate is not approved and enabled in the worker LLM list; "
                                            "run 'hancho-cli llm add --list worker --engine " + choice.engine +
                                            " --model " + choice.model + " --reason \"...\"' and approve it");
                    } else if (ctx.catalog) {
                        choice.list = "worker";
                        choice.entry = choice.engine + "/" + choice.model;
                    }
                    node->engineCandidates.push_back(std::move(choice));
                }
            }
        }
        if (j.contains("switchEngineOn")) {
            const json& sw = j["switchEngineOn"];
            if (!sw.is_array()) ctx.AddError(path + ".switchEngineOn", "must be an array of failure class strings");
            else for (const auto& item : sw) {
                if (!item.is_string() || item.get<std::string>().empty())
                    ctx.AddError(path + ".switchEngineOn", "entries must be non-empty strings");
                else node->switchEngineOn.push_back(item.get<std::string>());
            }
        }
        ParseJobCommonFields(j, path, *node, ctx);
    } else if (t == "worker") {
        if (j.contains("prompt") && j["prompt"].is_string()) node->prompt = j["prompt"].get<std::string>();
        if (j.contains("promptFile") && j["promptFile"].is_string()) node->promptFile = j["promptFile"].get<std::string>();
        if (node->prompt.empty() && node->promptFile.empty()) ctx.AddError(path, "'worker' requires non-empty 'prompt' or 'promptFile'");
        ReqStr(j, "summary", path, t, node->summary, ctx);
        ReqPosInt(j, "timeoutSec", path, t, node->timeoutSec, ctx);
        if (j.contains("estimateSec")) ReqPosInt(j, "estimateSec", path, t, node->estimateSec, ctx);
        if (j.contains("engine") && j["engine"].is_string()) node->engine = j["engine"].get<std::string>();
        if (j.contains("model") && j["model"].is_string()) node->model = j["model"].get<std::string>();
        if (j.contains("outputSchema")) {
            if (j["outputSchema"].is_object()) node->outputSchema = j["outputSchema"].dump();
            else if (j["outputSchema"].is_string()) node->outputSchema = j["outputSchema"].get<std::string>();
            else ctx.AddError(path + ".outputSchema", "must be a JSON object or string");
        }
        ParseJobCommonFields(j, path, *node, ctx);

        if (j.contains("engines")) {
            if (!j["engines"].is_array() || j["engines"].empty()) {
                ctx.AddError(path + ".engines", "must be a non-empty array of {engine, model} objects");
            } else {
                if (!node->engine.empty() || !node->model.empty()) {
                    ctx.AddError(path, "'engine'/'model' and 'engines' cannot both be specified");
                }
                for (size_t i = 0; i < j["engines"].size(); ++i) {
                    const json& c = j["engines"][i];
                    const std::string cpath = path + ".engines[" + std::to_string(i) + "]";
                    if (!c.is_object() || !c.contains("engine") || !c["engine"].is_string() ||
                        c["engine"].get<std::string>().empty()) {
                        ctx.AddError(cpath, "requires a non-empty string 'engine'");
                        continue;
                    }
                    BtEngineChoice choice;
                    choice.engine = c["engine"].get<std::string>();
                    if (c.contains("model") && c["model"].is_string()) choice.model = c["model"].get<std::string>();
                    if (c.contains("list") && c["list"].is_string()) choice.list = c["list"].get<std::string>();
                    if (c.contains("entry") && c["entry"].is_string()) choice.entry = c["entry"].get<std::string>();
                    if (ctx.catalog) {
                        std::string engErr;
                        if (!ctx.catalog->Knows(choice.engine, choice.model, engErr)) {
                            ctx.AddError(cpath, engErr);
                        } else if (!ctx.catalog->IsApprovedForList("worker", choice.engine, choice.model)) {
                            ctx.AddError(cpath, "candidate is not approved and enabled in the worker LLM list; "
                                                "run 'hancho-cli llm add --list worker --engine " +
                                                choice.engine + " --model " + choice.model +
                                                " --reason \"...\"' and approve it");
                        } else {
                            choice.list = "worker";
                            choice.entry = choice.engine + "/" + choice.model;
                        }
                    }
                    node->engineCandidates.push_back(std::move(choice));
                }
            }
        }
        if (j.contains("switchEngineOn")) {
            const json& sw = j["switchEngineOn"];
            if (!sw.is_array()) {
                ctx.AddError(path + ".switchEngineOn", "must be an array of failure class strings");
            } else {
                for (const auto& item : sw) {
                    if (!item.is_string() || item.get<std::string>().empty()) {
                        ctx.AddError(path + ".switchEngineOn", "entries must be non-empty strings");
                        continue;
                    }
                    node->switchEngineOn.push_back(item.get<std::string>());
                }
            }
        }
        if (ctx.catalog && node->engineCandidates.empty()) {
            std::string effEngine = !node->engine.empty() ? node->engine : ctx.defaults.worker.engine;
            std::string effModel = !node->model.empty()
                ? node->model
                : ((node->engine.empty() || node->engine == ctx.defaults.worker.engine) ? ctx.defaults.worker.model : "");
            std::string engErr;
            if (!ctx.catalog->Knows(effEngine, effModel, engErr)) ctx.AddError(path, engErr);
        }
    } else if (t == "probe") {
        if (j.contains("engine") && j["engine"].is_string()) node->engine = j["engine"].get<std::string>();
        if (j.contains("model") && j["model"].is_string()) node->model = j["model"].get<std::string>();
        node->timeoutSec = 60;
        if (j.contains("timeoutSec")) ReqPosInt(j, "timeoutSec", path, t, node->timeoutSec, ctx);
        if (j.contains("probe") && (!j["probe"].is_boolean() || !j["probe"].get<bool>())) {
            ctx.AddError(path + ".probe", "a probe node cannot be disabled");
        }
        if (ctx.catalog) {
            std::string effEngine = !node->engine.empty() ? node->engine : ctx.defaults.worker.engine;
            std::string engErr;
            if (!ctx.catalog->Knows(effEngine, node->model, engErr)) ctx.AddError(path, engErr);
        }
    } else if (t == "wait") {
        if (!j.contains("sec") || !j["sec"].is_number_integer() || j["sec"].get<int>() < 0) {
            ctx.AddError(path, "'wait' requires non-negative integer 'sec'");
        } else {
            node->sec = j["sec"].get<int>();
        }
    } else if (t == "set") {
        std::string varKey = j.contains("var") ? "var" : (j.contains("name") ? "name" : "");
        if (varKey.empty() || !j[varKey].is_string() || j[varKey].get<std::string>().empty()) {
            ctx.AddError(path, "'set' requires non-empty string 'var'");
        } else {
            node->setVar = j[varKey].get<std::string>();
        }
        if (!j.contains("value")) ctx.AddError(path, "'set' requires 'value'");
        else node->setValue = JsonToStrValue(j["value"]);
    } else if (t == "escalate") {
        ReqStr(j, "question", path, t, node->question, ctx);
        if (j.contains("guidance") && j["guidance"].is_string()) node->guidance = j["guidance"].get<std::string>();
        if (j.contains("expect")) {
            if (!j["expect"].is_array()) ctx.AddError(path + ".expect", "must be an array of strings");
            else {
                for (const auto& e : j["expect"]) {
                    if (!e.is_string() || e.get<std::string>().empty()) ctx.AddError(path + ".expect", "elements must be non-empty strings");
                    else node->expect.push_back(e.get<std::string>());
                }
            }
        }
        if (j.contains("include") && j["include"].is_object()) {
            const json& inc = j["include"];
            if (inc.contains("logs") && inc["logs"].is_array()) {
                for (const auto& x : inc["logs"]) if (x.is_string()) node->include.logs.push_back(x.get<std::string>());
            }
            if (inc.contains("vars") && inc["vars"].is_array()) {
                for (const auto& x : inc["vars"]) if (x.is_string()) node->include.vars.push_back(x.get<std::string>());
            }
            if (inc.contains("files") && inc["files"].is_array()) {
                for (const auto& x : inc["files"]) if (x.is_string()) node->include.files.push_back(x.get<std::string>());
            }
            if (inc.contains("logLines") && inc["logLines"].is_number_integer()) {
                node->include.logLines = std::max(1, inc["logLines"].get<int>());
            } else if (inc.contains("lines") && inc["lines"].is_number_integer()) {
                node->include.logLines = std::max(1, inc["lines"].get<int>());
            }
            if (inc.contains("fileMaxBytes") && inc["fileMaxBytes"].is_number_integer()) {
                node->include.fileMaxBytes = static_cast<size_t>(std::max(0, inc["fileMaxBytes"].get<int>()));
            } else if (inc.contains("maxBytes") && inc["maxBytes"].is_number_integer()) {
                node->include.fileMaxBytes = static_cast<size_t>(std::max(0, inc["maxBytes"].get<int>()));
            }
        }
        if (j.contains("timeoutSec")) ReqPosInt(j, "timeoutSec", path, t, node->escalateTimeoutSec, ctx);
        if (j.contains("fallback")) {
            if (!j["fallback"].is_object()) ctx.AddError(path + ".fallback", "must be an object");
            else {
                const json& fb = j["fallback"];
                node->fallback.enabled = true;
                node->fallback.afterSec = fb.value("afterSec", 0);
                if (node->fallback.afterSec <= 0) ctx.AddError(path + ".fallback.afterSec", "must be a positive integer");
                if (fb.contains("command")) {
                    ctx.AddError(path + ".fallback.command", "fallback.command is not supported; use engine/model");
                }
                if (fb.contains("engine") && fb["engine"].is_string()) node->fallback.engine = fb["engine"].get<std::string>();
                if (fb.contains("model") && fb["model"].is_string()) node->fallback.model = fb["model"].get<std::string>();
                if (ctx.catalog) {
                    std::string effEng = !node->fallback.engine.empty() ? node->fallback.engine : ctx.defaults.worker.engine;
                    std::string effMod = !node->fallback.model.empty()
                        ? node->fallback.model
                        : ((node->fallback.engine.empty() || node->fallback.engine == ctx.defaults.worker.engine)
                               ? ctx.defaults.worker.model
                               : "");
                    std::string engErr;
                    if (!ctx.catalog->Knows(effEng, effMod, engErr)) ctx.AddError(path + ".fallback", engErr);
                }
            }
        }
    } else if (t == "fail") {
        if (j.contains("detail") && j["detail"].is_string()) node->failDetail = j["detail"].get<std::string>();
        else if (j.contains("reason") && j["reason"].is_string()) node->failDetail = j["reason"].get<std::string>();
    } else if (t == "succeed") {
        // No required fields
    } else if (t == "subtree") {
        ctx.AddError(path, "subtree is not supported yet");
    } else if (t == "fileExists") {
        ReqStr(j, "path", path, t, node->condPath, ctx);
    } else if (t == "fileNewer") {
        ReqStr(j, "path", path, t, node->condPath, ctx);
        ReqStr(j, "than", path, t, node->condThan, ctx);
    } else if (t == "outputMatches") {
        ReqStr(j, "node", path, t, node->condNode, ctx);
        ReqStr(j, "regex", path, t, node->condRegex, ctx);
        node->condLines = j.value("lines", 200);
    } else if (t == "exitCode") {
        ReqStr(j, "node", path, t, node->condNode, ctx);
        if (!ParseComparisonOps(j, *node) || (!node->hasEq && !node->hasIn && !node->hasNe))
            ctx.AddError(path, "'exitCode' requires 'eq' or 'in'");
    } else if (t == "jsonValue") {
        ReqStr(j, "file", path, t, node->condFile, ctx);
        ReqStr(j, "path", path, t, node->condPath, ctx);
        if (!ParseComparisonOps(j, *node))
            ctx.AddError(path, "'jsonValue' requires a comparison operator (eq, ne, in, gt, lt, exists)");
    } else if (t == "var") {
        std::string k = j.contains("name") ? "name" : (j.contains("var") ? "var" : "");
        if (k.empty() || !j[k].is_string() || j[k].get<std::string>().empty())
            ctx.AddError(path, "'var' condition requires non-empty string 'name' (or 'var')");
        else
            node->condVarName = j[k].get<std::string>();
        if (!ParseComparisonOps(j, *node))
            ctx.AddError(path, "'var' condition requires a comparison operator (eq, ne, in, gt, lt, exists)");
    } else if (t == "nodeState") {
        ReqStr(j, "node", path, t, node->condNode, ctx);
        ParseComparisonOps(j, *node);
        if (!node->hasEq && j.contains("state") && j["state"].is_string()) {
            node->hasEq = true;
            node->condEq = j["state"];
        }
        if (!node->hasEq && !node->hasNe && !node->hasIn)
            ctx.AddError(path, "'nodeState' requires 'eq' (or 'state' / 'in')");
    } else if (t == "failure") {
        ReqStr(j, "node", path, t, node->condNode, ctx);
        if (j.contains("class")) {
            const json& c = j["class"];
            if (c.is_string()) {
                if (!c.get<std::string>().empty()) node->condFailureClasses.push_back(c.get<std::string>());
                else ctx.AddError(path + ".class", "must be a non-empty string");
            } else if (c.is_array()) {
                if (c.empty()) ctx.AddError(path + ".class", "must not be an empty array");
                for (const auto& item : c) {
                    if (!item.is_string() || item.get<std::string>().empty())
                        ctx.AddError(path + ".class", "elements must be non-empty strings");
                    else
                        node->condFailureClasses.push_back(item.get<std::string>());
                }
            } else {
                ctx.AddError(path + ".class", "must be a string or an array of strings");
            }
        }
        if (j.contains("cause")) {
            if (!j["cause"].is_string() || j["cause"].get<std::string>().empty())
                ctx.AddError(path + ".cause", "must be a non-empty string");
            else
                node->condFailureCause = j["cause"].get<std::string>();
        }
        if (node->condFailureClasses.empty() && node->condFailureCause.empty())
            ctx.AddError(path, "'failure' requires 'class' and/or 'cause'");
    } else {
        ctx.AddError(path, "unknown node type '" + rawType + "'");
    }
    return node;
}

// ---------------------------------------------------------------------------
// 091-4: lint warnings. These never make a tree invalid; --strict promotes them
// to errors in BehaviorTree::ValidateResponse.
// ---------------------------------------------------------------------------

void AddWarning(std::vector<BtWarning>& warnings, const std::string& path,
                const std::string& code, const std::string& message,
                const std::string& suggestion) {
    BtWarning w;
    w.path = path;
    w.code = code;
    w.message = message;
    w.suggestion = suggestion;
    warnings.push_back(std::move(w));
}

std::string LowerCopy(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

bool HasWholeWord(const std::string& lower, const char* word) {
    size_t pos = 0;
    const std::string w(word);
    while ((pos = lower.find(w, pos)) != std::string::npos) {
        bool leftOk = pos == 0 || !std::isalnum(static_cast<unsigned char>(lower[pos - 1]));
        size_t after = pos + w.size();
        bool rightOk = after >= lower.size() || !std::isalnum(static_cast<unsigned char>(lower[after]));
        if (leftOk && rightOk) return true;
        pos = after;
    }
    return false;
}

bool CommandHasModelFlag(const std::string& command) {
    const std::string lower = LowerCopy(command);
    return lower.find("--model") != std::string::npos || lower.find("-model") != std::string::npos;
}

bool IsOpencodeLauncher(const BtNode& n, const ParseContext& ctx) {
    if (n.engine == "opencode") return true;
    const std::string defaultEngine = ctx.defaults.worker.engine;
    if (n.type == "worker" && n.engine.empty() && defaultEngine == "opencode") return true;
    for (const auto& c : n.engineCandidates) if (c.engine == "opencode") return true;
    if (n.fallback.enabled && n.fallback.engine == "opencode") return true;
    return HasWholeWord(LowerCopy(n.command), "opencode");
}

bool LaunchHasModel(const BtNode& n, const ParseContext& ctx) {
    if (!n.model.empty()) return true;
    if (!n.engineCandidates.empty()) {
        for (const auto& c : n.engineCandidates) if (c.model.empty()) return false;
        return true;
    }
    if (n.type == "worker" && ctx.defaults.worker.engine == "opencode" &&
        !ctx.defaults.worker.model.empty()) return true;
    if (n.fallback.enabled && !n.fallback.model.empty()) return true;
    return CommandHasModelFlag(n.command);
}

bool IsLlmLaunchNode(const BtNode& n, const ParseContext& ctx) {
    if (n.type == "worker") return true;
    if (n.type != "job") return false;
    return !n.engine.empty() || !n.model.empty() || !n.engineCandidates.empty() ||
           n.rawDef.contains("agentType") || LooksLikeLlmJobCommand(n.command) ||
           IsOpencodeLauncher(n, ctx);
}

void CheckNodeWarnings(const BtNode& n, ParseContext& ctx, bool guarded,
                       std::vector<BtWarning>& warnings) {
    const bool llmLauncher = IsLlmLaunchNode(n, ctx);
    const bool hasWatch = n.idle.configured && (n.idle.firstEventSec > 0 || n.idle.outputSec > 0);

    if (llmLauncher && !hasWatch) {
        AddWarning(warnings, n.pathKey, "no_hang_watch",
                   "An LLM worker/job has neither idle.firstEventSec nor idle.outputSec, and no defaults.",
                   "Add \"idle\": {\"firstEventSec\": 180} or a \"defaults.worker.idle\" entry.");
    }
    if (llmLauncher && !guarded) {
        AddWarning(warnings, n.pathKey, "unguarded_worker",
                   "An LLM worker/job failure reaches the root without a catch, fallback or retry ancestor.",
                   "Wrap the node in a \"catch\" with handlers, or place it under \"fallback\" / \"retry\".");
    }

    if ((n.type == "worker" || n.type == "job" || n.type == "probe") &&
        IsOpencodeLauncher(n, ctx) && !LaunchHasModel(n, ctx)) {
        AddWarning(warnings, n.pathKey, "opencode_without_model",
                   "opencode is launched without a model (neither \"model\" nor --model/-Model).",
                   "Set \"model\", add a models entry to \"engines\", or pass --model in the command.");
    }

    // 091-7: declared needs must be approved before the node is allowed to run.
    if (n.rawDef.is_object() && n.rawDef.contains("needs") && n.rawDef["needs"].is_array()) {
        permit::Config cfg;
        std::string err;
        permit::Load(permit::DefaultPath(), cfg, err);
        const std::string scope = "bt:" + ctx.baseQueue;
        for (const auto& need : n.rawDef["needs"]) {
            if (!need.is_object()) continue;
            const std::string kind = need.value("kind", std::string());
            const std::string target = need.value("target", std::string());
            if (!permit::IsValidKind(kind) || target.empty()) {
                AddWarning(warnings, n.pathKey, "permit_invalid",
                           "needs entry has an invalid kind or empty target.",
                           "Use {\"kind\":\"read_path|write_path|command|network\",\"target\":\"...\"}.");
                continue;
            }
            if (!permit::IsApproved(cfg, kind, target, n.engine, scope)) {
                AddWarning(warnings, n.pathKey, "permit_not_approved",
                           "Permission '" + kind + " " + target + "' is declared in needs but is not approved.",
                           "Run `hancho-cli permit request --kind " + kind + " --target \"" + target +
                           "\" --reason \"...\"` and approve it before running.");
            }
        }
    }

    const size_t engineCount = n.engineCandidates.size();
    if (engineCount >= 2 && n.switchEngineOn.empty()) {
        AddWarning(warnings, n.pathKey, "engines_without_switch",
                   "Two or more engine candidates are listed but switchEngineOn is empty, so no switch can happen.",
                   "Add \"switchEngineOn\": [\"startup_hang\", \"auth_error\", \"probe_failed\"].");
    }
    if (!n.switchEngineOn.empty() && engineCount < 2) {
        AddWarning(warnings, n.pathKey, "switch_without_engines",
                   "switchEngineOn is set but fewer than two engine candidates are listed.",
                   "Add at least two approved entries to \"engines\", or remove \"switchEngineOn\".");
    }

    if (llmLauncher && n.type == "job" && !n.command.empty() &&
        n.command.find("{engine}") == std::string::npos &&
        n.command.find("{model}") == std::string::npos) {
        AddWarning(warnings, n.pathKey, "unswitchable_command",
                   "This LLM job command has no {engine}/{model} placeholder, so it cannot be switched on failure.",
                   "Use {engine} and {model} in the command (for example -Engine {engine} -Model {model}).");
    }

    if (n.type == "escalate" && !n.expect.empty()) {
        bool hasTakeover = false;
        for (const auto& e : n.expect) if (e == "takeover") hasTakeover = true;
        if (!hasTakeover) {
            AddWarning(warnings, n.pathKey, "escalate_without_takeover",
                       "escalate has \"expect\" but does not offer \"takeover\".",
                       "Add \"takeover\" to \"expect\" so the supervisor can be handed control.");
        }
    }

    if (!ctx.catalog) return;
    auto checkPermitted = [&](const std::string& path, const std::string& list,
                              const std::string& engine, const std::string& model) {
        std::string err;
        if (!ctx.catalog->Knows(engine, model, err)) {
            AddWarning(warnings, path, "engine_not_permitted",
                       "Engine/model is invalid or unknown: " + err,
                       "Add a valid engine/model to the " + list + " LLM list with 'hancho-cli llm add --list " +
                       list + "' and approve it.");
        } else if (!ctx.catalog->IsApprovedForList(list, engine, model)) {
            AddWarning(warnings, path, "engine_not_permitted",
                       "Engine/model is missing, disabled or still proposed in the " + list + " LLM list.",
                       "Run 'hancho-cli llm add --list " + list + " --engine " + engine +
                       " --model " + model + " --reason \"...\"' and approve it.");
        }
    };
    for (size_t i = 0; i < n.engineCandidates.size(); ++i) {
        const auto& c = n.engineCandidates[i];
        checkPermitted(n.pathKey + ".engines[" + std::to_string(i) + "]",
                       "worker", c.engine, c.model);
    }
    if (!n.engineCandidates.empty()) return;
    if (llmLauncher && (!n.engine.empty() || !n.model.empty())) {
        std::string effEngine = !n.engine.empty() ? n.engine : ctx.defaults.worker.engine;
        std::string effModel = !n.model.empty()
            ? n.model
            : ((n.engine.empty() || n.engine == ctx.defaults.worker.engine) ? ctx.defaults.worker.model : "");
        checkPermitted(n.pathKey + ".engine", "worker", effEngine, effModel);
    }
    if (n.type == "escalate" && n.fallback.enabled &&
        (!n.fallback.engine.empty() || !n.fallback.model.empty())) {
        std::string effEngine = !n.fallback.engine.empty() ? n.fallback.engine : ctx.defaults.worker.engine;
        std::string effModel = !n.fallback.model.empty()
            ? n.fallback.model
            : ((n.fallback.engine.empty() || n.fallback.engine == ctx.defaults.worker.engine)
                   ? ctx.defaults.worker.model : "");
        checkPermitted(n.pathKey + ".fallback", "consult", effEngine, effModel);
    }
}

void CollectNodeWarnings(const BtNode& n, ParseContext& ctx, bool guarded,
                         std::vector<BtWarning>& warnings) {
    CheckNodeWarnings(n, ctx, guarded, warnings);
    const bool childGuarded = guarded || n.type == "catch" || n.type == "fallback" || n.type == "retry";
    for (const auto& c : n.children) CollectNodeWarnings(*c, ctx, childGuarded, warnings);
    if (n.condChild) CollectNodeWarnings(*n.condChild, ctx, childGuarded, warnings);
    if (n.thenChild) CollectNodeWarnings(*n.thenChild, ctx, childGuarded, warnings);
    if (n.elseChild) CollectNodeWarnings(*n.elseChild, ctx, childGuarded, warnings);
    if (n.singleChild) CollectNodeWarnings(*n.singleChild, ctx, childGuarded, warnings);
    if (n.untilChild) CollectNodeWarnings(*n.untilChild, ctx, childGuarded, warnings);
    if (n.otherwiseChild) CollectNodeWarnings(*n.otherwiseChild, ctx, childGuarded, warnings);
    for (const auto& h : n.catchHandlers) {
        if (h.doNode) CollectNodeWarnings(*h.doNode, ctx, childGuarded, warnings);
    }
}

void ValidateFailureNodeReferences(const BtNode& node, ParseContext& ctx) {
    if (node.type == "failure" && !node.condNode.empty() &&
        ctx.seenKeys.find(node.condNode) == ctx.seenKeys.end()) {
        ctx.AddError(node.pathKey + ".node", "unknown node id '" + node.condNode + "'");
    }
    for (const auto& c : node.children) ValidateFailureNodeReferences(*c, ctx);
    if (node.condChild) ValidateFailureNodeReferences(*node.condChild, ctx);
    if (node.thenChild) ValidateFailureNodeReferences(*node.thenChild, ctx);
    if (node.elseChild) ValidateFailureNodeReferences(*node.elseChild, ctx);
    if (node.singleChild) ValidateFailureNodeReferences(*node.singleChild, ctx);
    if (node.untilChild) ValidateFailureNodeReferences(*node.untilChild, ctx);
    for (const auto& h : node.catchHandlers) {
        if (h.doNode) ValidateFailureNodeReferences(*h.doNode, ctx);
    }
    if (node.otherwiseChild) ValidateFailureNodeReferences(*node.otherwiseChild, ctx);
}

void ParseDefaultsBlock(const json& doc, ParseContext& ctx) {
    if (!doc.contains("defaults")) return;
    if (!doc["defaults"].is_object()) {
        ctx.AddError("defaults", "must be an object");
        return;
    }
    const json& d = doc["defaults"];
    if (d.contains("worker")) {
        if (!d["worker"].is_object()) {
            ctx.AddError("defaults.worker", "must be an object");
        } else {
            const json& w = d["worker"];
            ParseIdleObject(w, "idle", "defaults.worker.idle", ctx.defaults.workerIdle, ctx);
            if (w.contains("maxAttempts")) {
                if (!w["maxAttempts"].is_number_integer() || w["maxAttempts"].get<int>() < 1)
                    ctx.AddError("defaults.worker.maxAttempts", "must be a positive integer");
                else
                    ctx.defaults.workerMaxAttempts = w["maxAttempts"].get<int>();
            }
            if (w.contains("probe")) {
                if (!w["probe"].is_boolean()) ctx.AddError("defaults.worker.probe", "must be a boolean");
                else {
                    ctx.defaults.workerProbe = w["probe"].get<bool>();
                    ctx.defaults.hasWorkerProbe = true;
                }
            }
        }
    }
    if (d.contains("job")) {
        if (!d["job"].is_object()) ctx.AddError("defaults.job", "must be an object");
        else ParseIdleObject(d["job"], "idle", "defaults.job.idle", ctx.defaults.jobIdle, ctx);
    }
}

bool ParseTreeDocument(const std::string& jsonText, const IBtEngineCatalog* catalog,
                       std::vector<std::string>& outErrors,
                       std::vector<BtWarning>* outWarnings = nullptr) {
    outErrors.clear();
    if (outWarnings) outWarnings->clear();
    json doc = json::parse(jsonText, nullptr, false);
    if (doc.is_discarded() || !doc.is_object()) { outErrors.push_back("$: invalid JSON object"); return false; }

    ParseContext ctx;
    ctx.catalog = catalog;
    if (!doc.contains("id") || !doc["id"].is_string() || doc["id"].get<std::string>().empty())
        ctx.AddError("id", "missing required non-empty string 'id'");

    if (doc.contains("limits")) {
        if (!doc["limits"].is_object()) ctx.AddError("limits", "must be an object");
        else {
            const json& lim = doc["limits"];
            if (lim.contains("maxNodeRuns") && (!lim["maxNodeRuns"].is_number_integer() || lim["maxNodeRuns"].get<int>() < 1 || lim["maxNodeRuns"].get<int>() > 10000))
                ctx.AddError("limits.maxNodeRuns", "must be an integer between 1 and 10000");
            if (lim.contains("maxWallSec") && (!lim["maxWallSec"].is_number_integer() || lim["maxWallSec"].get<int>() <= 0))
                ctx.AddError("limits.maxWallSec", "must be a positive integer");
        }
    }
    if (doc.contains("worker")) {
        if (!doc["worker"].is_object()) ctx.AddError("worker", "must be an object");
        else {
            const json& w = doc["worker"];
            if (w.contains("engine") && w["engine"].is_string()) ctx.defaults.worker.engine = w["engine"].get<std::string>();
            if (w.contains("model") && w["model"].is_string()) ctx.defaults.worker.model = w["model"].get<std::string>();
            if (catalog && (!ctx.defaults.worker.engine.empty() || !ctx.defaults.worker.model.empty())) {
                std::string engErr;
                if (!catalog->Knows(ctx.defaults.worker.engine, ctx.defaults.worker.model, engErr)) ctx.AddError("worker", engErr);
            }
        }
    }
    ParseDefaultsBlock(doc, ctx);
    if (doc.contains("vars") && !doc["vars"].is_object()) ctx.AddError("vars", "must be an object");
    std::unique_ptr<BtNode> parsedRoot;
    if (!doc.contains("root") || !doc["root"].is_object()) ctx.AddError("root", "missing required object 'root'");
    else {
        parsedRoot = ParseNode(doc["root"], "root", "", 1, ctx, std::string());
        if (parsedRoot) ValidateFailureNodeReferences(*parsedRoot, ctx);
    }

    if (!ctx.errors.empty()) { outErrors = std::move(ctx.errors); return false; }
    if (outWarnings && parsedRoot) CollectNodeWarnings(*parsedRoot, ctx, false, *outWarnings);
    return true;
}

} // namespace

bool BehaviorTree::ValidateJson(const std::string& jsonText, const IBtEngineCatalog* catalog,
                                std::vector<std::string>& outErrors,
                                std::vector<BtWarning>& outWarnings) {
    return ParseTreeDocument(jsonText, catalog, outErrors, &outWarnings);
}

bool BehaviorTree::ValidateJson(const std::string& jsonText, const IBtEngineCatalog* catalog, std::vector<std::string>& outErrors) {
    return ParseTreeDocument(jsonText, catalog, outErrors);
}

bool BehaviorTree::ValidateJson(const std::string& jsonText, const IBtEngineCatalog* catalog, std::string& outError) {
    std::vector<std::string> errors;
    if (ValidateJson(jsonText, catalog, errors)) { outError.clear(); return true; }
    std::ostringstream oss;
    for (size_t i = 0; i < errors.size(); ++i) { if (i > 0) oss << "; "; oss << errors[i]; }
    outError = oss.str();
    return false;
}

std::string BehaviorTree::ValidateResponse(const std::string& jsonText,
                                           const IBtEngineCatalog* catalog,
                                           bool strict) {
    std::vector<std::string> errors;
    std::vector<BtWarning> warnings;
    const bool valid = ValidateJson(jsonText, catalog, errors, warnings);

    json warningArray = json::array();
    for (const auto& w : warnings) {
        warningArray.push_back(json{{"path", w.path}, {"code", w.code},
                                    {"message", w.message}, {"suggestion", w.suggestion}});
    }
    json out;
    out["warnings"] = std::move(warningArray);

    if (!valid) {
        std::ostringstream oss;
        for (size_t i = 0; i < errors.size(); ++i) { if (i > 0) oss << "; "; oss << errors[i]; }
        out["ok"] = false;
        out["valid"] = false;
        out["error"] = oss.str();
        out["errors"] = errors;
        return out.dump();
    }
    if (strict && !warnings.empty()) {
        std::vector<std::string> asErrors;
        std::ostringstream oss;
        for (size_t i = 0; i < warnings.size(); ++i) {
            const auto& w = warnings[i];
            asErrors.push_back(w.path + ": " + w.code + ": " + w.message +
                               " Suggestion: " + w.suggestion);
            if (i > 0) oss << "; ";
            oss << asErrors.back();
        }
        json codes = json::array();
        for (const auto& w : warnings) {
            if (w.code == "permit_not_approved" || w.code == "permit_invalid") {
                codes.push_back(w.code);
            }
        }
        out["ok"] = false;
        out["valid"] = false;
        out["error"] = oss.str();
        out["errors"] = asErrors;
        out["errorCodes"] = std::move(codes);
        return out.dump();
    }
    out["ok"] = true;
    out["valid"] = true;
    return out.dump();
}

bool BehaviorTree::LoadDefinition(const std::string& jsonText,
                                  const IBtEngineCatalog* catalog,
                                  std::string& err) {
    if (catalog) catalog_ = catalog;
    if (!ValidateJson(jsonText, catalog_, err)) return false;

    json doc = json::parse(jsonText, nullptr, false);
    ParseContext ctx;
    ctx.catalog = catalog_;

    impl_ = std::make_unique<Impl>();
    impl_->rawRootDoc = doc;
    id_ = doc["id"].get<std::string>();
    summary_ = doc.value("summary", std::string());
    intent_ = doc.value("intent", std::string());
    taskqueue_ = doc.value("taskqueue", "bt-" + id_);
    if (taskqueue_.empty()) taskqueue_ = "bt-" + id_;
    ctx.baseQueue = taskqueue_;
    cwd_ = doc.value("cwd", std::string());
    notifyFile_ = doc.value("notifyFile", std::string());

    limits_ = BtLimits();
    if (doc.contains("limits") && doc["limits"].is_object()) {
        limits_.maxNodeRuns = doc["limits"].value("maxNodeRuns", 200);
        limits_.maxWallSec = doc["limits"].value("maxWallSec", 86400);
    }

    defaults_ = BtDefaults();
    workerDefaults_ = BtWorkerDefaults();
    if (doc.contains("worker") && doc["worker"].is_object()) {
        workerDefaults_.engine = doc["worker"].value("engine", std::string());
        workerDefaults_.model = doc["worker"].value("model", std::string());
    }
    defaults_.worker = workerDefaults_;
    ctx.defaults = defaults_;
    ParseDefaultsBlock(doc, ctx);
    defaults_ = ctx.defaults;
    workerDefaults_ = defaults_.worker;
    ctx.btCwd = cwd_;

    vars_.clear();
    if (doc.contains("vars") && doc["vars"].is_object()) {
        for (auto it = doc["vars"].begin(); it != doc["vars"].end(); ++it) {
            vars_[it.key()] = JsonToStrValue(it.value());
        }
    }

    state_ = "idle";
    detail_.clear();
    nodeRuns_ = 0;
    nextJobSeq_ = 1;
    startMs_ = 0;
    createdAt_ = doc.value("createdAt", std::string());
    updatedAt_ = doc.value("updatedAt", std::string());
    events_.clear();
    nextEventSeq_ = 1;
    version_ = 0;

    impl_->root = ParseNode(doc["root"], "root", "", 1, ctx, std::string());
    return true;
}

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
                                             std::string& outError) {
    ParseContext ctx;
    ctx.catalog = catalog;
    ctx.defaults = defaults;
    ctx.btCwd = btCwd;
    ctx.baseQueue = baseQueue;
    for (const auto& id : existingIdsOutsideTarget) {
        if (!id.empty()) ctx.seenIds.insert(id);
    }
    ctx.nodeCount = existingNodesOutsideTarget;

    auto node = ParseNode(j, path, role, depth, ctx, queueScope);
    if (!ctx.errors.empty()) {
        std::ostringstream oss;
        for (size_t i = 0; i < ctx.errors.size(); ++i) {
            if (i > 0) oss << "; ";
            oss << ctx.errors[i];
        }
        outError = oss.str();
        return nullptr;
    }
    outError.clear();
    return node;
}

} // namespace pc
