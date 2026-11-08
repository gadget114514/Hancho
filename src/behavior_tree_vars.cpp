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

} // namespace pc
