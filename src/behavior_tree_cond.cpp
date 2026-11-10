#include "behavior_tree_internal.h"
#include "common.h"

#ifdef _WIN32
#include <windows.h>
#endif
#include <cmath>
#include <cstdlib>
#include <regex>
#include <sstream>

namespace pc {

namespace {

bool ReadFileMtime(const std::wstring& pathW, uint64_t& outFiletime) {
    return GetFileMtimeWide(pathW, outFiletime);
}

bool TryParseDouble(const json& v, double& out) {
    if (v.is_number()) {
        out = v.get<double>();
        return true;
    }
    if (v.is_string()) {
        const std::string& s = v.get<std::string>();
        if (s.empty()) return false;
        char* endPtr = nullptr;
        double d = std::strtod(s.c_str(), &endPtr);
        if (endPtr && *endPtr == '\0') {
            out = d;
            return true;
        }
    }
    return false;
}

std::string JsonNormalizedString(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_boolean()) return v.get<bool>() ? "true" : "false";
    if (v.is_number_integer()) return std::to_string(v.get<int64_t>());
    if (v.is_null()) return "null";
    return v.dump();
}

bool ValuesEqual(const json& a, const json& b) {
    if (a == b) return true;
    double da = 0.0, db = 0.0;
    if (TryParseDouble(a, da) && TryParseDouble(b, db)) {
        return std::fabs(da - db) < 1e-9;
    }
    return JsonNormalizedString(a) == JsonNormalizedString(b);
}

bool LooksLikeJsonlPath(const std::string& path) {
    static const std::string ext = ".jsonl";
    return path.size() >= ext.size() &&
           path.compare(path.size() - ext.size(), ext.size(), ext) == 0;
}

// Selects a value from the last valid object of a JSONL file (for example a hancho.event/1
// .events.jsonl). Bare "kind", "activity" and "failure" (and "failure.<field>") resolve against
// that last event so conditions can name the v1 fields directly; "last.<path>" is explicit.
bool NavigateLastJsonlValue(const std::string& text, const std::string& rawPath, json& outVal) {
    json last;
    json lastFailure;
    bool found = false;
    bool foundFailure = false;
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        json parsed = json::parse(line, nullptr, false);
        if (!parsed.is_discarded() && parsed.is_object()) {
            if (parsed.contains("failure") && parsed["failure"].is_object()) {
                lastFailure = parsed["failure"];
                foundFailure = true;
            } else if (parsed.contains("data") && parsed["data"].is_object() &&
                       parsed["data"].contains("failure") &&
                       parsed["data"]["failure"].is_object()) {
                lastFailure = parsed["data"]["failure"];
                foundFailure = true;
            }
            last = std::move(parsed);
            found = true;
        }
    }
    if (!found) return false;

    std::string path = rawPath;
    if (path.rfind("$.", 0) == 0) path = path.substr(2);
    else if (path.rfind("$", 0) == 0) path = path.substr(1);
    if (path.empty() || path == "last") {
        outVal = last;
        return true;
    }
    const bool explicitLast = path.rfind("last.", 0) == 0;
    if (explicitLast) path = path.substr(5);

    if (path == "activity") return NavigateJsonPath(last, "data.activity", outVal);
    if (path == "failure" || path.rfind("failure.", 0) == 0) {
        json failure;
        // Prefer the newest failure in the file over the newest line: a failed job often
        // writes jobQueued/retry events after its jobFailed record. An explicit "last."
        // path keeps the strict last-line meaning.
        if (foundFailure && !explicitLast) {
            failure = lastFailure;
        } else if (!NavigateJsonPath(last, "failure", failure) &&
                   !NavigateJsonPath(last, "data.failure", failure)) {
            return false;
        }
        const std::string rest = path == "failure" ? std::string() : path.substr(8);
        if (rest.empty()) {
            outVal = failure;
            return true;
        }
        return NavigateJsonPath(failure, rest, outVal);
    }
    return NavigateJsonPath(last, path, outVal);
}

bool RegexMatchesLines(const std::string& text, const std::string& pattern) {
    try {
        std::regex re(pattern);
        if (std::regex_search(text, re)) return true;
        std::istringstream iss(text);
        std::string line;
        while (std::getline(iss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (std::regex_search(line, re)) return true;
        }
    } catch (const std::regex_error&) {
        return false;
    }
    return false;
}

} // namespace

bool BehaviorTree::Impl::CompareJsonValue(const BehaviorTree& bt,
                                          const BtNode& node,
                                          bool valueExists,
                                          const json& actualVal,
                                          std::string& outUndefinedVar) const {
    if (node.hasExists) {
        if (valueExists != node.condExists) return false;
        if (!valueExists) return true;
    } else if (!valueExists) {
        return false;
    }

    auto expandExpected = [&](const json& exp, json& outExp) -> bool {
        if (exp.is_string()) {
            std::string s;
            if (!ExpandString(bt, exp.get<std::string>(), false, s, outUndefinedVar)) return false;
            outExp = s;
            return true;
        }
        outExp = exp;
        return true;
    };

    if (node.hasEq) {
        json exp;
        if (!expandExpected(node.condEq, exp)) return false;
        if (!ValuesEqual(actualVal, exp)) return false;
    }
    if (node.hasNe) {
        json exp;
        if (!expandExpected(node.condNe, exp)) return false;
        if (ValuesEqual(actualVal, exp)) return false;
    }
    if (node.hasIn) {
        if (!node.condIn.is_array()) return false;
        bool matched = false;
        for (const auto& item : node.condIn) {
            json exp;
            if (!expandExpected(item, exp)) return false;
            if (ValuesEqual(actualVal, exp)) {
                matched = true;
                break;
            }
        }
        if (!matched) return false;
    }
    if (node.hasGt) {
        json exp;
        if (!expandExpected(node.condGt, exp)) return false;
        double da = 0.0, de = 0.0;
        if (TryParseDouble(actualVal, da) && TryParseDouble(exp, de)) {
            if (!(da > de)) return false;
        } else {
            if (!(JsonNormalizedString(actualVal) > JsonNormalizedString(exp))) return false;
        }
    }
    if (node.hasLt) {
        json exp;
        if (!expandExpected(node.condLt, exp)) return false;
        double da = 0.0, de = 0.0;
        if (TryParseDouble(actualVal, da) && TryParseDouble(exp, de)) {
            if (!(da < de)) return false;
        } else {
            if (!(JsonNormalizedString(actualVal) < JsonNormalizedString(exp))) return false;
        }
    }
    return true;
}

bool BehaviorTree::Impl::EvaluateCondition(BehaviorTree& bt, BtNode& node, int64_t nowMs) {
    node.hasStatus = true;
    node.detail.clear();
    const std::string& t = node.type;
    std::string undefVar;

    auto finishCond = [&](bool ok, const std::string& detail = "") -> bool {
        node.state = ok ? "succeeded" : "failed";
        node.detail = detail;
        bt.EmitEvent(nowMs, "nodeFinished", node.nodeKey, node.state, node.detail, node.runs);
        return ok;
    };

    if (t == "fileExists") {
        std::string relPath;
        if (!ExpandString(bt, node.condPath, false, relPath, undefVar)) {
            return finishCond(false, "undefinedVar: " + undefVar);
        }
        std::wstring fullW = ResolveBtPathWide(bt.cwd_, relPath);
        bool found = PathExistsWide(fullW);
        return finishCond(found);
    }

    if (t == "fileNewer") {
        std::string relPath;
        if (!ExpandString(bt, node.condPath, false, relPath, undefVar)) {
            return finishCond(false, "undefinedVar: " + undefVar);
        }
        uint64_t pathFt = 0;
        if (!ReadFileMtime(ResolveBtPathWide(bt.cwd_, relPath), pathFt)) {
            return finishCond(false, "missing file: " + relPath);
        }

        // Check if condThan is "${<node>.start}"
        if (node.condThan.size() > 9 && node.condThan.rfind("${", 0) == 0 &&
            node.condThan.substr(node.condThan.size() - 7) == ".start}") {
            std::string refNodeKey = node.condThan.substr(2, node.condThan.size() - 9);
            const BtNode* refNode = FindNodeConst(refNodeKey, bt.id_);
            if (!refNode || refNode->lastStartTimestamp.empty()) {
                return finishCond(false, "undefinedVar: " + refNodeKey + ".start");
            }
            uint64_t refFt = static_cast<uint64_t>(refNode->lastStartMs);
            return finishCond(pathFt > refFt);
        }

        std::string thanPath;
        if (!ExpandString(bt, node.condThan, false, thanPath, undefVar)) {
            return finishCond(false, "undefinedVar: " + undefVar);
        }
        uint64_t thanFt = 0;
        if (!ReadFileMtime(ResolveBtPathWide(bt.cwd_, thanPath), thanFt)) {
            return finishCond(false, "missing reference file: " + thanPath);
        }
        return finishCond(pathFt > thanFt);
    }

    if (t == "outputMatches") {
        const BtNode* target = FindNodeConst(node.condNode, bt.id_);
        if (!target) return finishCond(false, "unknown node: " + node.condNode);
        std::string text = target->lastLogTail;
        if (text.empty() && !target->lastLogFile.empty()) {
            text = ReadFileUtf8Wide(Utf8ToWide(target->lastLogFile));
        }
        text = GetTailLines(text, node.condLines > 0 ? node.condLines : 200);
        std::string pattern;
        if (!ExpandString(bt, node.condRegex, false, pattern, undefVar)) {
            return finishCond(false, "undefinedVar: " + undefVar);
        }
        return finishCond(RegexMatchesLines(text, pattern));
    }

    if (t == "exitCode") {
        const BtNode* target = FindNodeConst(node.condNode, bt.id_);
        if (!target || !target->hasExitCode) return finishCond(false);
        json actual = target->lastExitCode;
        bool ok = CompareJsonValue(bt, node, true, actual, undefVar);
        if (!undefVar.empty()) return finishCond(false, "undefinedVar: " + undefVar);
        return finishCond(ok);
    }

    if (t == "jsonValue") {
        std::string relFile, jPath;
        if (!ExpandString(bt, node.condFile, false, relFile, undefVar) ||
            !ExpandString(bt, node.condPath, false, jPath, undefVar)) {
            return finishCond(false, "undefinedVar: " + undefVar);
        }
        std::string content = ReadFileUtf8Wide(ResolveBtPathWide(bt.cwd_, relFile));
        if (content.empty()) {
            return finishCond(false, "cannot read file: " + relFile);
        }
        json subVal;
        bool exists = false;
        if (LooksLikeJsonlPath(relFile)) {
            exists = NavigateLastJsonlValue(content, jPath, subVal);
        } else {
            json doc = json::parse(content, nullptr, false);
            if (doc.is_discarded()) {
                return finishCond(false, "invalid JSON in " + relFile);
            }
            exists = NavigateJsonPath(doc, jPath, subVal);
        }
        bool ok = CompareJsonValue(bt, node, exists, subVal, undefVar);
        if (!undefVar.empty()) return finishCond(false, "undefinedVar: " + undefVar);
        return finishCond(ok);
    }

    if (t == "var") {
        std::string varName = node.condVarName;
        if (varName.find("${") != std::string::npos) {
            if (!ExpandString(bt, varName, false, varName, undefVar)) {
                return finishCond(false, "undefinedVar: " + undefVar);
            }
        }
        std::string valStr;
        bool exists = LookupVar(bt, varName, valStr);
        json valJson = exists ? json(valStr) : json(nullptr);
        bool ok = CompareJsonValue(bt, node, exists, valJson, undefVar);
        if (!undefVar.empty()) return finishCond(false, "undefinedVar: " + undefVar);
        return finishCond(ok);
    }

    if (t == "nodeState") {
        const BtNode* target = FindNodeConst(node.condNode, bt.id_);
        if (!target) return finishCond(false, "unknown node: " + node.condNode);
        json actual = target->state;
        bool ok = CompareJsonValue(bt, node, true, actual, undefVar);
        if (!undefVar.empty()) return finishCond(false, "undefinedVar: " + undefVar);
        return finishCond(ok);
    }

    return finishCond(false, "unsupported condition type: " + t);
}

} // namespace pc
