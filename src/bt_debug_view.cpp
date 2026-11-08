#include "bt_debug_view.h"
#include "common.h"
#include "nlohmann/json.hpp"

#include <windows.h>
#include <algorithm>
#include <fstream>
#include <sstream>

namespace pc {

using nlohmann::json;

namespace {

// Child slots in the order they are shown. "children" is an array; the rest hold one node.
const char* const kSingleChildSlots[] = {"cond", "then", "else", "child", "until"};

std::string Str(const json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || it->is_null()) return "";
    if (it->is_string()) return it->get<std::string>();
    return it->dump();
}

std::string OneLine(std::string s, size_t maxLen = 80) {
    for (char& c : s) {
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    }
    if (s.size() > maxLen) {
        size_t cut = maxLen - 3;
        // Do not cut inside a UTF-8 sequence.
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) cut--;
        s = s.substr(0, cut) + "...";
    }
    return s;
}

// Short text for a condition node: "jsonValue out/result.json status eq needs_review".
std::string ConditionText(const json& n) {
    std::ostringstream ss;
    for (const char* k : {"file", "path", "node", "name", "than", "regex"}) {
        std::string v = Str(n, k);
        if (!v.empty()) ss << ' ' << v;
    }
    for (const char* op : {"eq", "ne", "in", "gt", "lt", "exists"}) {
        auto it = n.find(op);
        if (it != n.end()) ss << ' ' << op << ' ' << (it->is_string() ? it->get<std::string>() : it->dump());
    }
    return ss.str();
}

std::string BuildNodeLabel(const json& n, const std::string& type, const std::string& role,
                           const std::string& state, const json& status) {
    std::ostringstream ss;
    ss << '[' << state << "] ";
    if (!role.empty() && role != "child") ss << role << ": ";
    ss << type;
    std::string id = Str(n, "id");
    if (!id.empty()) ss << ' ' << id;

    int runs = status.is_object() ? status.value("runs", 0) : 0;
    if (type == "retry" || type == "repeat") {
        ss << " (" << runs << '/' << n.value("max", 0) << ')';
    } else if (type == "job" || type == "worker") {
        if (type == "worker") {
            // Engine and model as written in the node; empty means the tree/PC default.
            std::string engine = Str(n, "engine");
            std::string model = Str(n, "model");
            if (!engine.empty() || !model.empty()) {
                ss << " [" << (engine.empty() ? "default" : engine);
                if (!model.empty()) ss << ' ' << model;
                ss << ']';
            }
        }
        std::string summary = Str(n, "summary");
        if (!summary.empty()) ss << " \"" << OneLine(summary, 60) << '"';
        if (runs > 1) ss << " x" << runs;
    } else if (type == "escalate") {
        std::string q = Str(n, "question");
        if (q.empty()) q = Str(n, "reason");
        if (!q.empty()) ss << " \"" << OneLine(q, 60) << '"';
        std::string answer = status.is_object() ? Str(status, "answer") : "";
        if (!answer.empty()) ss << " -> " << OneLine(answer, 30);
    } else if (type == "wait") {
        ss << ' ' << n.value("sec", 0) << 's';
    } else if (type == "timeout") {
        ss << ' ' << n.value("sec", 0) << 's';
    } else if (type == "parallel") {
        auto it = n.find("success");
        if (it != n.end()) ss << " success=" << (it->is_string() ? it->get<std::string>() : it->dump());
    } else if (type == "subtree") {
        std::string f = Str(n, "file");
        if (!f.empty()) ss << ' ' << f;
    } else if (type == "set") {
        std::string var = Str(n, "var");
        if (!var.empty()) ss << ' ' << var << " = " << OneLine(Str(n, "value"), 40);
    } else if (!n.contains("children") && !n.contains("child")) {
        // Condition leaves: show what they test.
        std::string c = ConditionText(n);
        if (!c.empty()) ss << OneLine(c, 70);
    }

    if (status.is_object()) {
        auto jobs = status.find("jobs");
        if (jobs != status.end() && jobs->is_array() && !jobs->empty() && jobs->back().is_string()) {
            ss << "  job " << jobs->back().get<std::string>();
        }
        std::string detail = Str(status, "detail");
        if (!detail.empty()) ss << "  - " << OneLine(detail, 60);
    }
    return ss.str();
}

void BuildNode(const json& n, const std::string& btId, const std::string& role,
               const std::string& pathKey, int depth, BtDebugNode& out) {
    out.btId = btId;
    out.role = role;
    out.nodeId = Str(n, "id");
    out.type = n.is_object() ? Str(n, "type") : "";
    if (out.type.empty()) out.type = "?";
    out.key = btId + "/" + (out.nodeId.empty() ? pathKey : out.nodeId);

    json status = n.is_object() ? n.value("status", json()) : json();
    out.state = status.is_object() ? Str(status, "state") : "";
    if (out.state.empty()) out.state = "idle";
    if (status.is_object()) {
        auto jobs = status.find("jobs");
        if (jobs != status.end() && jobs->is_array()) {
            for (const auto& j : *jobs) {
                if (j.is_string()) out.jobIds.push_back(j.get<std::string>());
            }
        }
    }
    if (!n.is_object()) {
        out.label = "[?] (not an object)";
        out.detailJson = n.dump(2, ' ', false, json::error_handler_t::replace);
        return;
    }
    out.label = BuildNodeLabel(n, out.type, role, out.state, status);

    json detail = n;
    detail.erase("children");
    for (const char* slot : kSingleChildSlots) {
        auto it = detail.find(slot);
        if (it != detail.end() && it->is_object() && it->contains("type")) detail.erase(slot);
    }
    out.detailJson = detail.dump(2, ' ', false, json::error_handler_t::replace);

    // The engine limits depth to 32; stop well past that so a corrupt file cannot recurse forever.
    if (depth > 64) return;

    for (const char* slot : kSingleChildSlots) {
        auto it = n.find(slot);
        if (it == n.end() || !it->is_object() || !it->contains("type")) continue;
        BtDebugNode child;
        BuildNode(*it, btId, slot, pathKey + "." + slot, depth + 1, child);
        out.children.push_back(std::move(child));
    }
    auto kids = n.find("children");
    if (kids != n.end() && kids->is_array()) {
        size_t i = 0;
        for (const auto& c : *kids) {
            BtDebugNode child;
            BuildNode(c, btId, "", pathKey + ".children[" + std::to_string(i) + "]", depth + 1, child);
            out.children.push_back(std::move(child));
            ++i;
        }
    }
}

} // namespace

std::wstring GetBehaviorTreesDirWide() {
    return GetLogsDirectoryWide() + L"\\behaviortrees";
}

bool ParseBtDebugTree(const std::string& jsonText, BtDebugNode& out, std::string& err) {
    json bt = json::parse(jsonText, nullptr, false);
    if (bt.is_discarded()) {
        err = "invalid JSON";
        return false;
    }
    if (!bt.is_object()) {
        err = "not a JSON object";
        return false;
    }
    std::string id = Str(bt, "id");
    if (id.empty()) {
        err = "missing id";
        return false;
    }

    out = BtDebugNode();
    out.key = id;
    out.btId = id;
    out.type = "bt";
    out.state = Str(bt, "state");
    if (out.state.empty()) out.state = "idle";

    std::ostringstream ss;
    ss << '[' << out.state << "] " << id;
    std::string summary = Str(bt, "summary");
    if (!summary.empty()) ss << " \"" << OneLine(summary, 70) << '"';
    if (bt.contains("nodeRuns")) {
        ss << "  runs " << bt.value("nodeRuns", 0);
        auto limits = bt.find("limits");
        if (limits != bt.end() && limits->is_object() && limits->contains("maxNodeRuns")) {
            ss << '/' << limits->value("maxNodeRuns", 0);
        }
    }
    std::string updated = Str(bt, "updatedAt");
    if (!updated.empty()) ss << "  " << updated;
    out.label = ss.str();

    json detail = bt;
    detail.erase("root");
    out.detailJson = detail.dump(2, ' ', false, json::error_handler_t::replace);

    auto root = bt.find("root");
    if (root != bt.end() && root->is_object()) {
        BtDebugNode rootNode;
        BuildNode(*root, id, "", "root", 0, rootNode);
        out.children.push_back(std::move(rootNode));
    }
    return true;
}

std::vector<BtDebugNode> LoadBtDebugTrees(const std::wstring& dir, std::vector<std::string>& errors) {
    struct Loaded {
        BtDebugNode node;
        std::string updatedAt;
    };
    std::vector<Loaded> loaded;

    WIN32_FIND_DATAW fd = {};
    HANDLE h = FindFirstFileW((dir + L"\\*.json").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            std::wstring path = dir + L"\\" + fd.cFileName;
            std::string name = WideToUtf8(fd.cFileName);
            std::ifstream f(path, std::ios::binary);
            if (!f) {
                errors.push_back(name + ": cannot open");
                continue;
            }
            std::stringstream buf;
            buf << f.rdbuf();
            std::string text = buf.str();
            Loaded item;
            std::string err;
            if (!ParseBtDebugTree(text, item.node, err)) {
                errors.push_back(name + ": " + err);
                continue;
            }
            json bt = json::parse(text, nullptr, false);
            item.updatedAt = Str(bt, "updatedAt");
            loaded.push_back(std::move(item));
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }

    std::sort(loaded.begin(), loaded.end(), [](const Loaded& a, const Loaded& b) {
        if (a.updatedAt != b.updatedAt) return a.updatedAt > b.updatedAt;
        return a.node.btId < b.node.btId;
    });
    std::vector<BtDebugNode> out;
    out.reserve(loaded.size());
    for (auto& l : loaded) out.push_back(std::move(l.node));
    return out;
}

namespace {
void FindByJob(const BtDebugNode& n, const std::string& jobId, std::string& found) {
    if (std::find(n.jobIds.begin(), n.jobIds.end(), jobId) != n.jobIds.end()) found = n.key;
    for (const auto& c : n.children) FindByJob(c, jobId, found);
}
} // namespace

std::string FindBtNodeKeyByJob(const std::vector<BtDebugNode>& trees, const std::string& jobId) {
    std::string found;
    if (jobId.empty()) return found;
    for (const auto& t : trees) FindByJob(t, jobId, found);
    return found;
}

} // namespace pc
