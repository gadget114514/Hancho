#pragma once

// Embedded behavior-tree templates for `hancho-cli bt template` and MCP `bt_template`
// (doc/obsoleted/HanchoExpectations.md 8.4). No files are looked up at runtime.

#include "nlohmann/json.hpp"

#include <iostream>
#include <string>
#include <vector>

namespace pc {

struct BtTemplateEntry {
    std::string name;
    std::string description;
};

inline constexpr const char* kBtTemplateNote =
    "Replace every <...> placeholder with a real value (do not use ${...}) before "
    "bt validate / bt start. {engine}, {model}, the foreach expansions "
    "${t.*} / ${foreach:...} and the failure fields ${bt.failedStep}, "
    "${bt.failedExitCode}, ${bt.failedClass}, ${bt.failedDurationSec} and "
    "${bt.failedLogTail} are intentional runtime placeholders.";

inline const char* const kResilientAgentTemplate = R"JSON({
  "id": "resilient-agent",
  "summary": "An agent with hang watch, preflight probe, engine fallback and escalation",
  "intent": "Run one resilient agent; replace <...> placeholders before validation",
  "defaults": {
    "agent": {
      "idle": { "firstEventSec": 180, "outputSec": 600 },
      "probe": true
    }
  },
  "root": {
    "type": "catch",
    "id": "impl_guard",
    "child": {
      "type": "sequence",
      "id": "impl_and_test",
      "children": [
        {
          "type": "agent",
          "id": "impl",
          "summary": "<task summary>",
          "prompt": "Stay inside the project (cwd): do not read or write files outside it. If the task requires one, add it to needs and request a permit instead. If you cannot find the root cause, print 'HANCHO-RESULT: root_cause_not_found' on its own line, write a 'handoff' object ({suspected, tried, ruledOut}) to PC_BT_RESULT_FILE, and exit non-zero. <agent prompt>",
          "timeoutSec": 5400,
          "engines": [
            { "engine": "opencode", "model": "<primary-model>" },
            { "engine": "agy", "model": "<backup-model>" }
          ],
          "switchEngineOn": [
            "startup_hang",
            "auth_error",
            "probe_failed",
            { "class": "fix_no_progress", "after": 1 },
            { "class": "fix_failed", "after": 2 },
            { "class": "root_cause_not_found", "after": 1 }
          ]
        },
        {
          "type": "exec",
          "id": "test",
          "summary": "Run tests",
          "command": "<test command>",
          "timeoutSec": 900,
          "blame": "impl"
        }
      ]
    },
    "handlers": [
      {
        "class": ["startup_hang", "idle_stall"],
        "do": { "type": "set", "var": "hang", "value": "engine-switch-handled" }
      },
       {
         "class": "rate_limited",
         "do": { "type": "wait", "sec": 600 },
         "thenRetry": true
       },
       {
         "class": "permission_denied",
         "do": { "type": "set", "var": "permit", "value": "requested" },
         "thenRetry": false
       },
       {
         "cause": "fast_exit",
         "do": { "type": "set", "var": "fast_exit_retried", "value": "1" },
         "thenRetry": true
       }
    ],
    "otherwise": {
      "type": "escalate",
      "id": "gate",
      "timeoutSec": 1800,
      "question": "Step ${bt.failedStep} failed (exit=${bt.failedExitCode}, class=${bt.failedClass}, ${bt.failedDurationSec}s). Recent log: ${bt.failedLogTail} Decide: continue, stop, or takeover.",
      "guidance": "A fast exit 1 was already retried once. Decide whether to continue, stop, or take over.",
      "expect": ["continue", "stop", "takeover"],
      "include": { "logs": ["impl"], "logLines": 100 }
    },
    "maxRetries": 1
  }
})JSON";

inline const char* const kCardSequenceTemplate = R"JSON({
  "id": "card-sequence",
  "summary": "Two ww cards in order, each hang-protected, with a build after each card",
  "intent": "Run ww cards with engine switching; replace <...> placeholders before validation",
  "defaults": {
    "agent": {
      "idle": { "firstEventSec": 180, "outputSec": 600 },
      "probe": true
    }
  },
  "root": {
    "type": "sequence",
    "children": [
      {
        "type": "catch",
        "id": "card_<card-id-1>_guard",
        "child": {
          "type": "exec",
          "id": "card_<card-id-1>",
          "summary": "Run ww card <card-id-1>",
          "command": "tools\\ww\\ww-run.cmd <card-id-1> -Main -Engine {engine} -Model {model} -Root <root> -JsonStdout",
          "timeoutSec": 5400,
          "engines": [
            { "engine": "opencode", "model": "<primary-model>" },
            { "engine": "agy", "model": "<backup-model>" }
          ],
          "switchEngineOn": [
            "startup_hang",
            "auth_error",
            "probe_failed",
            { "class": "fix_no_progress", "after": 1 },
            { "class": "fix_failed", "after": 2 },
            { "class": "root_cause_not_found", "after": 1 }
          ]
        },
        "handlers": [
          {
            "class": ["startup_hang", "idle_stall"],
            "do": { "type": "set", "var": "hang", "value": "engine-switch-handled" }
          },
          {
            "class": "rate_limited",
            "do": { "type": "wait", "sec": 600 },
            "thenRetry": true
          },
          {
            "cause": "fast_exit",
            "do": { "type": "set", "var": "fast_exit_retried", "value": "1" },
            "thenRetry": true
          }
        ],
        "otherwise": {
          "type": "escalate",
          "id": "gate_1",
          "timeoutSec": 1800,
          "question": "Card <card-id-1>: step ${bt.failedStep} failed (exit=${bt.failedExitCode}, class=${bt.failedClass}, ${bt.failedDurationSec}s). Recent log: ${bt.failedLogTail} Decide: continue, stop, or takeover.",
          "guidance": "A fast exit 1 was already retried once. Decide whether to continue, stop, or take over.",
          "expect": ["continue", "stop", "takeover"],
          "include": { "logs": ["card_<card-id-1>"], "logLines": 100 }
        },
        "maxRetries": 1
      },
      {
        "type": "exec",
        "id": "build_after_<card-id-1>",
        "summary": "Build after card <card-id-1>",
        "command": "cmake --build build --config Release",
        "timeoutSec": 900
      },
      {
        "type": "catch",
        "id": "card_<card-id-2>_guard",
        "child": {
          "type": "exec",
          "id": "card_<card-id-2>",
          "summary": "Run ww card <card-id-2>",
          "command": "tools\\ww\\ww-run.cmd <card-id-2> -Main -Engine {engine} -Model {model} -Root <root> -JsonStdout",
          "timeoutSec": 5400,
          "engines": [
            { "engine": "opencode", "model": "<primary-model>" },
            { "engine": "agy", "model": "<backup-model>" }
          ],
          "switchEngineOn": [
            "startup_hang",
            "auth_error",
            "probe_failed",
            { "class": "fix_no_progress", "after": 1 },
            { "class": "fix_failed", "after": 2 },
            { "class": "root_cause_not_found", "after": 1 }
          ]
        },
        "handlers": [
          {
            "class": ["startup_hang", "idle_stall"],
            "do": { "type": "set", "var": "hang", "value": "engine-switch-handled" }
          },
          {
            "class": "rate_limited",
            "do": { "type": "wait", "sec": 600 },
            "thenRetry": true
          },
          {
            "cause": "fast_exit",
            "do": { "type": "set", "var": "fast_exit_retried", "value": "1" },
            "thenRetry": true
          }
        ],
        "otherwise": {
          "type": "escalate",
          "id": "gate_2",
          "timeoutSec": 1800,
          "question": "Card <card-id-2>: step ${bt.failedStep} failed (exit=${bt.failedExitCode}, class=${bt.failedClass}, ${bt.failedDurationSec}s). Recent log: ${bt.failedLogTail} Decide: continue, stop, or takeover.",
          "guidance": "A fast exit 1 was already retried once. Decide whether to continue, stop, or take over.",
          "expect": ["continue", "stop", "takeover"],
          "include": { "logs": ["card_<card-id-2>"], "logLines": 100 }
        },
        "maxRetries": 1
      },
      {
        "type": "exec",
        "id": "build_after_<card-id-2>",
        "summary": "Build after card <card-id-2>",
        "command": "cmake --build build --config Release",
        "timeoutSec": 900
      }
    ]
  }
})JSON";

inline const char* const kTestSuiteTemplate = R"JSON({
  "id": "test-suite",
  "summary": "Build all targets, then run every smoke test found under tests/",
  "intent": "Build once and run a glob-driven smoke suite; replace <...> placeholders before validation",
  "root": {
    "type": "sequence",
    "children": [
      {
        "type": "exec",
        "id": "build",
        "summary": "Build Hancho and every smoke test",
        "command": "cmake --build build --config Release --target Hancho HanchoChat HanchoTerm wct hancho-cli ${foreach:tests/*_smoke.cpp:stem: }",
        "timeoutSec": 3600
      },
      {
        "type": "foreach",
        "id": "smokes",
        "glob": "tests/*_smoke.cpp",
        "as": "t",
        "mode": "all",
        "do": {
          "type": "exec",
          "id": "test_${t.stem}",
          "command": "build\\Release\\${t.stem}.exe",
          "summary": "Run ${t.stem}",
          "timeoutSec": 900
        }
      }
    ]
  }
})JSON";

inline const char* const kCardTemplate = R"JSON({
  "id": "card",
  "summary": "Run one task card (impl agent -> build -> test) with engine switching and catch",
  "intent": "Run one task card; replace <...> placeholders before validation",
  "defaults": {
    "agent": {
      "idle": { "firstEventSec": 180, "outputSec": 600 },
      "probe": true
    }
  },
  "root": {
    "type": "catch",
    "id": "impl_guard",
    "child": {
      "type": "sequence",
      "id": "impl_build_test",
      "children": [
        {
          "type": "agent",
          "id": "impl",
          "summary": "<card summary>",
          "prompt": "Stay inside the project (cwd): do not read or write files outside it. If the task requires one, add it to needs and request a permit instead. If you cannot find the root cause, print 'HANCHO-RESULT: root_cause_not_found' on its own line, write a 'handoff' object ({suspected, tried, ruledOut}) to PC_BT_RESULT_FILE, and exit non-zero. <card body>",
          "timeoutSec": 5400,
          "engines": [
            { "engine": "opencode", "model": "<primary-model>" },
            { "engine": "agy", "model": "<backup-model>" }
          ],
          "switchEngineOn": [
            "startup_hang",
            "auth_error",
            "probe_failed",
            { "class": "fix_no_progress", "after": 1 },
            { "class": "fix_failed", "after": 2 },
            { "class": "root_cause_not_found", "after": 1 }
          ]
        },
        {
          "type": "exec",
          "id": "build",
          "summary": "Build",
          "command": "<build command>",
          "timeoutSec": 900,
          "blame": "impl"
        },
        {
          "type": "exec",
          "id": "test",
          "summary": "Run tests",
          "command": "<test command>",
          "timeoutSec": 900,
          "blame": "impl"
        }
      ]
    },
    "handlers": [
      {
        "class": ["startup_hang", "idle_stall"],
        "do": { "type": "set", "var": "hang", "value": "engine-switch-handled" }
      },
      {
        "class": "rate_limited",
        "do": { "type": "wait", "sec": 600 },
        "thenRetry": true
      },
      {
        "class": "permission_denied",
        "do": { "type": "set", "var": "permit", "value": "requested" },
        "thenRetry": false
      },
      {
        "cause": "fast_exit",
        "do": { "type": "set", "var": "fast_exit_retried", "value": "1" },
        "thenRetry": true
      }
    ],
    "otherwise": {
      "type": "escalate",
      "id": "gate",
      "timeoutSec": 1800,
      "question": "Step ${bt.failedStep} failed (exit=${bt.failedExitCode}, class=${bt.failedClass}, ${bt.failedDurationSec}s). Recent log: ${bt.failedLogTail} Decide: continue, stop, or takeover.",
      "guidance": "A fast exit 1 was already retried once. Decide whether to continue, stop, or take over.",
      "expect": ["continue", "stop", "takeover"],
      "include": { "logs": ["impl"], "logLines": 100 }
    },
    "maxRetries": 1
  }
})JSON";

inline std::vector<BtTemplateEntry> BtTemplateEntries() {
    return {
        {"resilient-agent",
         "Agent with idle defaults, probe, two approved engines, catch handlers and takeover-ready escalation."},
        {"card-sequence",
         "ww cards in order, each hang-protected with engines and catch, plus a build exec after each card."},
        {"test-suite",
         "Build targets with ${foreach:...}, then run each tests/*_smoke.cpp via a foreach node (mode all)."},
        {"card",
         "Task card flow: impl agent -> build -> test (both blaming impl), with engine switching and catch."}
    };
}

inline nlohmann::json BtTemplateDefinition(const std::string& name) {
    const char* text = nullptr;
    if (name == "resilient-agent" || name == "resilient-worker") text = kResilientAgentTemplate;
    else if (name == "card-sequence") text = kCardSequenceTemplate;
    else if (name == "test-suite") text = kTestSuiteTemplate;
    else if (name == "card") text = kCardTemplate;
    else return nullptr;
    return nlohmann::json::parse(text, nullptr, false);
}

inline nlohmann::json BtTemplateListJson() {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& e : BtTemplateEntries()) {
        arr.push_back(nlohmann::json{{"name", e.name}, {"description", e.description}});
    }
    return nlohmann::json{{"ok", true}, {"templates", std::move(arr)}, {"note", kBtTemplateNote}};
}

inline nlohmann::json BtTemplateResult(const std::string& name) {
    if (name.empty()) return BtTemplateListJson();
    nlohmann::json def = BtTemplateDefinition(name);
    if (def.is_null()) {
        return nlohmann::json{{"ok", false}, {"error", "unknown bt template: " + name}};
    }
    const std::string canonicalName = (name == "resilient-worker") ? "resilient-agent" : name;
    std::string description;
    for (const auto& e : BtTemplateEntries()) {
        if (e.name == canonicalName) { description = e.description; break; }
    }
    return nlohmann::json{{"ok", true}, {"name", name}, {"description", description},
                          {"note", kBtTemplateNote}, {"bt", std::move(def)}};
}

inline std::string BtTemplateListText() {
    std::string out = "Behavior tree templates:\n\n";
    for (const auto& e : BtTemplateEntries()) {
        out += "  " + e.name + "  - " + e.description + "\n";
    }
    out += "\nNote: " + std::string(kBtTemplateNote) + "\n";
    return out;
}

inline int RunBtTemplateCommand(const std::vector<std::string>& args, bool jsonOutput) {
    std::string name;
    bool wantsHelp = false;
    for (size_t i = 3; i < args.size(); ++i) {
        const std::string& a = args[i];
        if (a == "--json" || a == "-j") continue;
        if (a == "--help" || a == "-h") wantsHelp = true;
        else if (!a.empty() && a[0] != '-' && name.empty()) name = a;
    }
    if (wantsHelp) {
        std::cout << "Usage: hancho-cli bt template [<name>] [--json]\n\n"
                  << BtTemplateListText()
                  << "\nWith a name, prints the full BT JSON (--json: raw JSON, ready to save "
                     "and pass to bt start --file).\n";
        return 0;
    }
    if (name.empty()) {
        if (jsonOutput) std::cout << BtTemplateListJson().dump(2) << "\n";
        else std::cout << BtTemplateListText();
        return 0;
    }
    nlohmann::json def = BtTemplateDefinition(name);
    if (def.is_null()) {
        if (jsonOutput) {
            std::cout << nlohmann::json{{"ok", false}, {"error", "unknown bt template: " + name}}.dump() << "\n";
        } else {
            std::cerr << "Error: unknown bt template: " << name << "\n";
        }
        return 1;
    }
    if (jsonOutput) {
        std::cout << def.dump(2) << "\n";
    } else {
        std::string description;
        for (const auto& e : BtTemplateEntries()) {
            if (e.name == name) { description = e.description; break; }
        }
        std::cout << description << "\nNote: " << kBtTemplateNote << "\n\n"
                  << def.dump(2) << "\n";
    }
    return 0;
}

} // namespace pc
