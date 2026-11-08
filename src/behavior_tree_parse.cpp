#include "behavior_tree_internal.h"
#include "common.h"

#include <set>
#include <sstream>

namespace pc {

namespace {

struct ParseContext {
    const IBtEngineCatalog* catalog = nullptr;
    BtWorkerDefaults workerDefaults;
    std::string btCwd;
    std::set<std::string> seenIds;
    int nodeCount = 0;
    std::vector<std::string> errors;

    void AddError(const std::string& path, const std::string& msg) {
        errors.push_back(path + ": " + msg);
    }
};

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

void ParseJobCommonFields(const json& j, const std::string& path, BtNode& node, ParseContext& ctx) {
    if (j.contains("cwd") && j["cwd"].is_string()) node.cwd = j["cwd"].get<std::string>();
    if (j.contains("maxAttempts") && j["maxAttempts"].is_number_integer()) {
        node.maxAttempts = j["maxAttempts"].get<int>();
        if (node.maxAttempts < 1) ctx.AddError(path + ".maxAttempts", "must be >= 1");
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
        if (!j["idle"].is_object()) ctx.AddError(path + ".idle", "must be an object");
        else {
            node.idle.outputSec = j["idle"].value("outputSec", 0);
            node.idle.cpuSec = j["idle"].value("cpuSec", 0);
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
                                  int depth, ParseContext& ctx) {
    auto node = std::make_unique<BtNode>();
    node->pathKey = path;
    node->role = role;
    node->depth = depth;

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
        outSlot = ParseNode(j[key], path + "." + key, childRole, depth + 1, ctx);
    };

    if (t == "sequence" || t == "fallback" || t == "parallel") {
        if (!j.contains("children") || !j["children"].is_array() || j["children"].empty()) {
            ctx.AddError(path, "'" + rawType + "' requires a non-empty 'children' array");
        } else {
            size_t idx = 0;
            for (const auto& c : j["children"]) {
                node->children.push_back(ParseNode(c, path + ".children[" + std::to_string(idx++) + "]", "", depth + 1, ctx));
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
            else node->elseChild = ParseNode(j["else"], path + ".else", "else", depth + 1, ctx);
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
            else node->untilChild = ParseNode(j["until"], path + ".until", "until", depth + 1, ctx);
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
        if (j.contains("instructionsText") && j["instructionsText"].is_string()) node->instructionsText = j["instructionsText"].get<std::string>();
        if (j.contains("instructionsFile") && j["instructionsFile"].is_string()) node->instructionsFile = j["instructionsFile"].get<std::string>();
        ParseJobCommonFields(j, path, *node, ctx);
    } else if (t == "worker") {
        if (j.contains("prompt") && j["prompt"].is_string()) node->prompt = j["prompt"].get<std::string>();
        if (j.contains("promptFile") && j["promptFile"].is_string()) node->promptFile = j["promptFile"].get<std::string>();
        if (node->prompt.empty() && node->promptFile.empty()) ctx.AddError(path, "'worker' requires non-empty 'prompt' or 'promptFile'");
        ReqStr(j, "summary", path, t, node->summary, ctx);
        ReqPosInt(j, "timeoutSec", path, t, node->timeoutSec, ctx);
        if (j.contains("engine") && j["engine"].is_string()) node->engine = j["engine"].get<std::string>();
        if (j.contains("model") && j["model"].is_string()) node->model = j["model"].get<std::string>();
        if (j.contains("outputSchema")) {
            if (j["outputSchema"].is_object()) node->outputSchema = j["outputSchema"].dump();
            else if (j["outputSchema"].is_string()) node->outputSchema = j["outputSchema"].get<std::string>();
            else ctx.AddError(path + ".outputSchema", "must be a JSON object or string");
        }
        ParseJobCommonFields(j, path, *node, ctx);
        if (ctx.catalog) {
            std::string effEngine = !node->engine.empty() ? node->engine : ctx.workerDefaults.engine;
            std::string effModel = !node->model.empty()
                ? node->model
                : ((node->engine.empty() || node->engine == ctx.workerDefaults.engine) ? ctx.workerDefaults.model : "");
            std::string engErr;
            if (!ctx.catalog->Knows(effEngine, effModel, engErr)) ctx.AddError(path, engErr);
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
                    std::string effEng = !node->fallback.engine.empty() ? node->fallback.engine : ctx.workerDefaults.engine;
                    std::string effMod = !node->fallback.model.empty()
                        ? node->fallback.model
                        : ((node->fallback.engine.empty() || node->fallback.engine == ctx.workerDefaults.engine)
                               ? ctx.workerDefaults.model
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
    } else {
        ctx.AddError(path, "unknown node type '" + rawType + "'");
    }
    return node;
}

bool ParseTreeDocument(const std::string& jsonText, const IBtEngineCatalog* catalog, std::vector<std::string>& outErrors) {
    outErrors.clear();
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
            if (w.contains("engine") && w["engine"].is_string()) ctx.workerDefaults.engine = w["engine"].get<std::string>();
            if (w.contains("model") && w["model"].is_string()) ctx.workerDefaults.model = w["model"].get<std::string>();
            if (catalog && (!ctx.workerDefaults.engine.empty() || !ctx.workerDefaults.model.empty())) {
                std::string engErr;
                if (!catalog->Knows(ctx.workerDefaults.engine, ctx.workerDefaults.model, engErr)) ctx.AddError("worker", engErr);
            }
        }
    }
    if (doc.contains("vars") && !doc["vars"].is_object()) ctx.AddError("vars", "must be an object");
    if (!doc.contains("root") || !doc["root"].is_object()) ctx.AddError("root", "missing required object 'root'");
    else ParseNode(doc["root"], "root", "", 1, ctx);

    if (!ctx.errors.empty()) { outErrors = std::move(ctx.errors); return false; }
    return true;
}

} // namespace

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
    cwd_ = doc.value("cwd", std::string());
    notifyFile_ = doc.value("notifyFile", std::string());

    limits_ = BtLimits();
    if (doc.contains("limits") && doc["limits"].is_object()) {
        limits_.maxNodeRuns = doc["limits"].value("maxNodeRuns", 200);
        limits_.maxWallSec = doc["limits"].value("maxWallSec", 86400);
    }

    workerDefaults_ = BtWorkerDefaults();
    if (doc.contains("worker") && doc["worker"].is_object()) {
        workerDefaults_.engine = doc["worker"].value("engine", std::string());
        workerDefaults_.model = doc["worker"].value("model", std::string());
    }
    ctx.workerDefaults = workerDefaults_;
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

    impl_->root = ParseNode(doc["root"], "root", "", 1, ctx);
    return true;
}

std::unique_ptr<BtNode> ParseSingleNodeJson(const json& j,
                                            const std::string& path,
                                            const std::string& role,
                                            int depth,
                                            const IBtEngineCatalog* catalog,
                                            const BtWorkerDefaults& workerDefaults,
                                            const std::string& btCwd,
                                            const std::vector<std::string>& existingIdsOutsideTarget,
                                            int existingNodesOutsideTarget,
                                            std::string& outError) {
    ParseContext ctx;
    ctx.catalog = catalog;
    ctx.workerDefaults = workerDefaults;
    ctx.btCwd = btCwd;
    for (const auto& id : existingIdsOutsideTarget) {
        if (!id.empty()) ctx.seenIds.insert(id);
    }
    ctx.nodeCount = existingNodesOutsideTarget;

    auto node = ParseNode(j, path, role, depth, ctx);
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
