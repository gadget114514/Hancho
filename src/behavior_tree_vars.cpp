#include "behavior_tree_internal.h"
#include "common.h"

#include <regex>
#include <sstream>

namespace pc {

namespace {

std::string Utf8Prefix64K(const std::string& s, size_t maxBytes = 65536) {
    if (s.size() <= maxBytes) return s;
    size_t n = maxBytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) n--;
    return s.substr(0, n);
}

std::string JsonScalarOrDump(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
    if (v.is_null()) return "";
    return v.dump();
}

void FlattenJsonRecursive(std::map<std::string, std::string>& vars,
                          const std::string& prefix,
                          const json& val) {
    vars[prefix] = JsonScalarOrDump(val);
    if (val.is_object()) {
        for (auto it = val.begin(); it != val.end(); ++it) {
            FlattenJsonRecursive(vars, prefix + "." + it.key(), it.value());
        }
    } else if (val.is_array()) {
        for (size_t i = 0; i < val.size(); ++i) {
            FlattenJsonRecursive(vars, prefix + "[" + std::to_string(i) + "]", val[i]);
            FlattenJsonRecursive(vars, prefix + "." + std::to_string(i), val[i]);
        }
    }
}

bool RegexSearchMultiline(const std::string& text, const std::regex& re, std::smatch& outMatch) {
    if (std::regex_search(text, outMatch, re)) return true;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (std::regex_search(line, outMatch, re)) return true;
    }
    return false;
}

} // namespace

std::string BtVarPreview(const std::string& v, size_t maxBytes) {
    if (v.size() <= maxBytes) return v;
    return Utf8Prefix64K(v, maxBytes) + " ...[truncated, " + std::to_string(v.size()) + " bytes]";
}

std::string DiagString(const json& j, const char* key) {
    if (!j.is_object()) return "";
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return "";
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

std::string DiagLikelyCauseIds(const json& j) {
    std::string out;
    if (!j.is_object()) return out;
    auto it = j.find("likelyCauses");
    if (it == j.end() || !it->is_array()) return out;
    for (const auto& c : *it) {
        std::string id = c.is_object() ? DiagString(c, "id") : std::string();
        if (id.empty()) continue;
        if (!out.empty()) out += ",";
        out += id;
    }
    return out;
}

bool BehaviorTree::Impl::LookupVar(const BehaviorTree& bt, const std::string& expr, std::string& outVal) const {
    auto it = bt.vars_.find(expr);
    if (it != bt.vars_.end()) {
        outVal = it->second;
        return true;
    }

    if (expr == "bt.id") {
        outVal = bt.id_;
        return true;
    }
    if (expr == "bt.runs") {
        outVal = std::to_string(bt.nodeRuns_);
        return true;
    }

    // Try matching <node>.<prop> or <node>.output.<jsonPath>
    size_t dot = expr.find('.');
    if (dot != std::string::npos) {
        std::string nodeRef = expr.substr(0, dot);
        std::string prop = expr.substr(dot + 1);
        const BtNode* node = FindNodeConst(nodeRef, bt.id_);
        if (node) {
            if (prop == "state") {
                outVal = node->state;
                return true;
            }
            if (prop == "runs") {
                outVal = std::to_string(node->runs);
                return true;
            }
            if (prop == "exitCode") {
                if (!node->hasExitCode) return false;
                outVal = std::to_string(node->lastExitCode);
                return true;
            }
            if (prop == "failureClass") {
                outVal = node->hasFailureDiagnostic ? DiagString(node->failureDiagnostic, "class") : "";
                return true;
            }
            if (prop == "failureSummary") {
                outVal = node->hasFailureDiagnostic ? DiagString(node->failureDiagnostic, "summary") : "";
                return true;
            }
            if (prop == "likelyCauses") {
                outVal = node->hasFailureDiagnostic ? DiagLikelyCauseIds(node->failureDiagnostic) : "";
                return true;
            }
            if (prop == "suggestedAction") {
                outVal = node->hasFailureDiagnostic ? DiagString(node->failureDiagnostic, "suggestedAction") : "";
                return true;
            }
            if (prop == "retryAt") {
                outVal = node->hasFailureDiagnostic ? DiagString(node->failureDiagnostic, "retryAt") : "";
                return true;
            }
            if (prop == "engine") {
                outVal = node->lastEngine;
                return true;
            }
            if (prop == "model") {
                outVal = node->lastModel;
                return true;
            }
            if (prop == "logFile") {
                if (node->lastLogFile.empty()) return false;
                outVal = node->lastLogFile;
                return true;
            }
            if (prop == "jobId") {
                if (node->jobs.empty()) return false;
                outVal = node->jobs.back();
                return true;
            }
            if (prop == "resultFile") {
                if (node->jobs.empty()) return false;
                outVal = GetLogsDirectoryUtf8() + "\\behaviortrees\\" +
                         SanitizeFileNameComponent(bt.id_) + "\\results\\" +
                         SanitizeFileNameComponent(node->jobs.back()) + ".json";
                return true;
            }
            if (prop == "start") {
                if (node->lastStartTimestamp.empty()) return false;
                outVal = node->lastStartTimestamp;
                return true;
            }
            if (prop == "answer") {
                if (node->answer.empty()) return false;
                outVal = node->answer;
                return true;
            }
            if (prop.rfind("output.", 0) == 0) {
                auto rawIt = bt.vars_.find(node->nodeKey + ".output");
                if (rawIt != bt.vars_.end()) {
                    json parsedOut = json::parse(rawIt->second, nullptr, false);
                    json subVal;
                    if (!parsedOut.is_discarded() && NavigateJsonPath(parsedOut, prop.substr(7), subVal)) {
                        outVal = JsonScalarOrDump(subVal);
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

bool BehaviorTree::Impl::ExpandString(const BehaviorTree& bt,
                                      const std::string& input,
                                      bool quoteForCommand,
                                      std::string& outExpanded,
                                      std::string& outUndefinedVar) const {
    outExpanded.clear();
    outUndefinedVar.clear();

    size_t i = 0;
    while (i < input.size()) {
        if (input[i] == '$' && i + 1 < input.size() && input[i + 1] == '{') {
            size_t closePos = input.find('}', i + 2);
            if (closePos != std::string::npos) {
                std::string varName = input.substr(i + 2, closePos - (i + 2));
                if (varName.rfind("foreach:", 0) == 0) {
                    // ${foreach:<glob>:<field>:<sep>} expands to a joined list at launch time.
                    // Each value is a separate command argument, so quoting is not applied.
                    const std::string rest = varName.substr(8);
                    size_t c1 = rest.find(':');
                    size_t c2 = c1 == std::string::npos ? std::string::npos : rest.find(':', c1 + 1);
                    if (c1 == std::string::npos || c2 == std::string::npos) {
                        outUndefinedVar = varName;
                        return false;
                    }
                    const std::string glob = rest.substr(0, c1);
                    const std::string field = rest.substr(c1 + 1, c2 - c1 - 1);
                    std::string sep = rest.substr(c2 + 1);
                    if (sep.empty()) sep = " ";
                    if (field != "stem" && field != "name" && field != "path" && field != "index") {
                        outUndefinedVar = varName;
                        return false;
                    }
                    std::vector<std::string> paths;
                    if (glob.empty() || !ExpandBtGlob(bt.cwd_, glob, {}, paths)) {
                        outUndefinedVar = varName;
                        return false;
                    }
                    std::string joined;
                    for (size_t k = 0; k < paths.size(); ++k) {
                        std::string v;
                        if (field == "stem") v = BtPathStem(paths[k]);
                        else if (field == "name") v = BtPathName(paths[k]);
                        else if (field == "path") v = paths[k];
                        else v = std::to_string(k);
                        if (k > 0) joined += sep;
                        joined += v;
                    }
                    outExpanded += joined;
                    i = closePos + 1;
                    continue;
                }
                std::string val;
                if (!LookupVar(bt, varName, val)) {
                    outUndefinedVar = varName;
                    return false;
                }
                outExpanded += quoteForCommand ? QuoteCommandArg(val) : val;
                i = closePos + 1;
                continue;
            }
        }
        outExpanded.push_back(input[i]);
        i++;
    }
    return true;
}

void BehaviorTree::Impl::ApplyCaptures(BehaviorTree& bt, BtNode& node, const std::string& logTail, int64_t nowMs) {
    if (node.capture.empty()) return;

    std::string tailText = logTail;
    if (tailText.empty() && !node.lastLogFile.empty()) {
        tailText = ReadFileUtf8Wide(Utf8ToWide(node.lastLogFile));
    }
    tailText = GetTailLines(tailText, 200);

    for (const auto& [varName, rule] : node.capture) {
        if (!rule.regex.empty()) {
            try {
                std::regex re(rule.regex);
                std::smatch match;
                if (RegexSearchMultiline(tailText, re, match)) {
                    size_t grp = rule.group >= 0 ? static_cast<size_t>(rule.group) : 0;
                    if (grp < match.size()) {
                        std::string captured = match[grp].str();
                        bt.vars_[varName] = captured;
                        bt.EmitEvent(nowMs, "varSet", node.nodeKey, node.state, "", node.runs, varName, captured);
                    }
                }
            } catch (const std::regex_error&) {
                // Invalid regex in capture is ignored at capture time
            }
        } else if (!rule.jsonFile.empty()) {
            std::string expandedFile, undefVar;
            if (!ExpandString(bt, rule.jsonFile, false, expandedFile, undefVar)) continue;
            std::string expandedPath = rule.path;
            if (!rule.path.empty()) {
                if (!ExpandString(bt, rule.path, false, expandedPath, undefVar)) continue;
            }
            std::wstring filePathW = ResolveBtPathWide(bt.cwd_, expandedFile);
            std::string content = ReadFileUtf8Wide(filePathW);
            if (content.empty()) continue;
            json j = json::parse(content, nullptr, false);
            if (j.is_discarded()) continue;
            json targetVal;
            if (NavigateJsonPath(j, expandedPath, targetVal)) {
                std::string captured = JsonScalarOrDump(targetVal);
                bt.vars_[varName] = captured;
                bt.EmitEvent(nowMs, "varSet", node.nodeKey, node.state, "", node.runs, varName, captured);
            }
        }
    }
}

void BehaviorTree::Impl::StoreWorkerOutputs(BehaviorTree& bt,
                                            BtNode& node,
                                            const std::string& finalResponse,
                                            const std::string& resultFileJson,
                                            int64_t nowMs) {
    std::string prefix = !node.id.empty() ? node.id : node.nodeKey;
    std::string truncatedResult = Utf8Prefix64K(finalResponse, 65536);
    bt.vars_[prefix + ".result"] = truncatedResult;
    bt.EmitEvent(nowMs, "varSet", node.nodeKey, node.state, "", node.runs, prefix + ".result", truncatedResult);

    if (!resultFileJson.empty()) {
        json parsed = json::parse(resultFileJson, nullptr, false);
        if (!parsed.is_discarded()) {
            FlattenJsonRecursive(bt.vars_, prefix + ".output", parsed);
            if (parsed.is_object()) {
                for (auto it = parsed.begin(); it != parsed.end(); ++it) {
                    std::string sub = prefix + ".output." + it.key();
                    bt.EmitEvent(nowMs, "varSet", node.nodeKey, "succeeded", "", node.runs, sub, JsonScalarOrDump(it.value()));
                }
            }
        }
    }
}

void BehaviorTree::Impl::ApplyFailureDiagnostic(BehaviorTree& bt,
                                                BtNode& node,
                                                const std::string& diagnosticJson,
                                                bool succeeded,
                                                int64_t nowMs) {
    json parsed = json::parse(diagnosticJson, nullptr, false);
    bool usable = !succeeded && parsed.is_object() && !DiagString(parsed, "class").empty();
    if (usable) {
        node.hasFailureDiagnostic = true;
        node.failureDiagnostic = std::move(parsed);
    } else {
        node.hasFailureDiagnostic = false;
        node.failureDiagnostic = json::object();
    }

    std::string prefix = !node.id.empty() ? node.id : node.nodeKey;
    auto setVar = [&](const char* suffix, const std::string& value) {
        std::string key = prefix + "." + suffix;
        bt.vars_[key] = value;
        bt.EmitEvent(nowMs, "varSet", node.nodeKey, node.state, "", node.runs, key, value);
    };

    setVar("failureClass", usable ? DiagString(node.failureDiagnostic, "class") : "");
    setVar("failureSummary", usable ? DiagString(node.failureDiagnostic, "summary") : "");
    setVar("likelyCauses", usable ? DiagLikelyCauseIds(node.failureDiagnostic) : "");
    setVar("suggestedAction", usable ? DiagString(node.failureDiagnostic, "suggestedAction") : "");
    setVar("retryAt", usable ? DiagString(node.failureDiagnostic, "retryAt") : "");
    setVar("engine", node.lastEngine);
    setVar("model", node.lastModel);
}

} // namespace pc
