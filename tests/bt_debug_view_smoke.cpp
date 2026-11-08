// Headless test for the behavior tree debug model (src/bt_debug_view.cpp). The sample state file
// tests/data/bt/feature-x.json is SYNTHETIC: written by hand in the format of
// doc/behavior-tree-plan.md section 7, not produced by a real run.

#include "../src/bt_debug_view.h"
#include "../src/common.h"
#include <windows.h>
#include <cstdio>
#include <string>
#include <vector>

using namespace pc;

static int g_failures = 0;

static void Check(bool cond, const std::string& label) {
    std::printf("%s %s\n", cond ? "[PASS]" : "[FAIL]", label.c_str());
    std::fflush(stdout);
    if (!cond) g_failures++;
}

static const BtDebugNode* FindKey(const BtDebugNode& n, const std::string& key) {
    if (n.key == key) return &n;
    for (const auto& c : n.children) {
        if (const BtDebugNode* f = FindKey(c, key)) return f;
    }
    return nullptr;
}

int main() {
    std::wstring dir = Utf8ToWide(PC_REPO_DIR) + L"\\tests\\data\\bt";
    std::vector<std::string> errors;
    std::vector<BtDebugNode> trees = LoadBtDebugTrees(dir, errors);
    Check(errors.empty(), "sample loads without errors");
    Check(trees.size() == 1, "one tree loaded");
    if (trees.size() != 1) return 1;

    const BtDebugNode& bt = trees[0];
    Check(bt.key == "feature-x" && bt.type == "bt" && bt.state == "waitingForLlm", "tree row key/type/state");
    Check(bt.label.find("runs 5/200") != std::string::npos, "tree label shows runs/limit: " + bt.label);
    Check(bt.detailJson.find("\"root\"") == std::string::npos, "tree detail omits root");
    Check(bt.children.size() == 1 && bt.children[0].key == "feature-x/root", "root node keyed by path");

    const BtDebugNode* loop = FindKey(bt, "feature-x/build-loop");
    Check(loop && loop->label.find("(2/3)") != std::string::npos, "retry shows runs/max");
    Check(loop && loop->children.size() == 1 && loop->children[0].role == "child", "retry child slot");
    Check(loop && loop->detailJson.find("fallback") == std::string::npos, "detail omits child nodes");

    const BtDebugNode* build = FindKey(bt, "feature-x/build");
    Check(build && build->jobIds.size() == 2, "job ids collected");
    Check(build && build->label.find("job feature-x-3") != std::string::npos, "label shows last job: " + (build ? build->label : ""));

    const BtDebugNode* unnamed = FindKey(bt, "feature-x/root.children[0].child.children[1]");
    Check(unnamed && unnamed->state == "failed" && unnamed->label.find("fail node") != std::string::npos,
          "node without id keyed by path, detail in label");

    const BtDebugNode* cond = FindKey(bt, "feature-x/root.children[2].cond");
    Check(cond && cond->role == "cond" && cond->label.find("cond: jsonValue") != std::string::npos &&
              cond->label.find("eq needs_review") != std::string::npos,
          "condition label: " + (cond ? cond->label : ""));
    const BtDebugNode* review = FindKey(bt, "feature-x/review");
    Check(review && review->role == "then" && review->label.find("Approve the result") != std::string::npos,
          "escalate label shows question");

    const BtDebugNode* fix = FindKey(bt, "feature-x/fix");
    Check(fix && fix->type == "worker" && fix->label.find("[agy gemini-4-argon-high]") != std::string::npos &&
              fix->label.find("job feature-x-2") != std::string::npos,
          "worker label shows engine/model and job: " + (fix ? fix->label : ""));
    Check(FindBtNodeKeyByJob(trees, "feature-x-2") == "feature-x/fix", "find node by job id");
    Check(FindBtNodeKeyByJob(trees, "nope").empty(), "unknown job id");

    BtDebugNode out;
    std::string err;
    Check(!ParseBtDebugTree("{\"summary\":\"x\"}", out, err) && err == "missing id", "missing id rejected");
    Check(!ParseBtDebugTree("not json", out, err), "invalid JSON rejected");
    Check(ParseBtDebugTree("{\"id\":\"empty\"}", out, err) && out.children.empty() && out.state == "idle",
          "tree without root");

    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
