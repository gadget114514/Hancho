#pragma once

// Debug model for behavior trees (doc/behavior-tree-plan.md). Reads the persisted state files
// (<logs>/behaviortrees/<id>.json) and turns each one into a display tree for the hidden debug
// dialog. No GUI and no process code here, so tests/bt_debug_view_smoke.cpp can drive it.

#include <string>
#include <vector>

namespace pc {

struct BtDebugNode {
    // Stable identity across reloads: "<btId>" for the tree itself, "<btId>/<nodeId>" for a node
    // with an id, "<btId>/<parent key path>.children[2]" style for a node without one.
    std::string key;
    std::string btId;
    std::string nodeId;
    std::string type;      // "bt" for the tree itself, otherwise the node type
    std::string role;      // "cond" / "then" / "else" / "until" / "child" / "" (children[])
    std::string state;     // status.state, "idle" when the node has not run
    std::string label;     // one line, UTF-8
    std::vector<std::string> jobIds; // status.jobs (oldest first)
    std::string detailJson;          // the node with its child nodes removed, pretty printed
    std::vector<BtDebugNode> children;
};

// <logs dir>\behaviortrees (created on demand by the engine; this only builds the path).
std::wstring GetBehaviorTreesDirWide();

// Parses one persisted state file. Returns false and fills err when it is not a BT object.
bool ParseBtDebugTree(const std::string& jsonText, BtDebugNode& out, std::string& err);

// Loads every *.json directly under dir, sorted by updatedAt (newest first), then id.
// Files that fail to parse are skipped and reported in errors ("<file>: <reason>").
std::vector<BtDebugNode> LoadBtDebugTrees(const std::wstring& dir, std::vector<std::string>& errors);

// Key of the node whose status.jobs contains jobId (the last match), or "" when none.
std::string FindBtNodeKeyByJob(const std::vector<BtDebugNode>& trees, const std::string& jobId);

} // namespace pc
