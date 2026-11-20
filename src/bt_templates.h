#pragma once

// Embedded behavior-tree templates for `hancho-cli bt template` and MCP `bt_template`
// (doc/HanchoExpectations.md 8.4). No files are looked up at runtime.

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
    "bt validate / bt start. {engine}, {model} and the foreach expansions "
    "${t.*} / ${foreach:...} are intentional runtime placeholders.";

inline const char* const kResilientWorkerTemplate = R"JSON({
  "id": "resilient-worker",
  "summary": "A worker with hang watch, preflight probe, engine fallback and escalation",
  "intent": "Run one resilient worker; replace <...> placeholders before validation",
  "defaults": {
    "worker": {
      "idle": { "firstEventSec": 180, "outputSec": 600 },
      "probe": true
    }
  },
  "root": {
    "type": "catch",
    "id": "impl_guard",
    "child": {
      "type": "worker",
      "id": "impl",
      "summary": "<task summary>",
       "prompt": "Stay inside the project (cwd): do not read or write files outside it. If the task requires one, add it to needs and request a permit instead. <worker prompt>",
      "timeoutSec": 5400,
      "engines": [
        { "engine": "opencode", "model": "<primary-model>" },
        { "engine": "agy", "model": "<backup-model>" }
      ],
      "switchEngineOn": ["startup_hang", "auth_error", "probe_failed"]
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
        "class": "*",
        "do": {
          "type": "escalate",
          "question": "<what should the supervisor decide?>",
          "guidance": "The worker failed after all approved engine candidates were tried.",
          "expect": ["continue", "stop", "takeover"],
          "include": { "logs": ["impl"], "logLines": 100 }
        }
      }
    ],
    "otherwise": "fail",
    "maxRetries": 2
  }
})JSON";

inline const char* const kCardSequenceTemplate = R"JSON({
  "id": "card-sequence",
  "summary": "Two ww cards in order, each hang-protected, with a build after each card",
  "intent": "Run ww cards with engine switching; replace <...> placeholders before validation",
  "defaults": {
    "worker": {
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
          "type": "job",
          "id": "card_<card-id-1>",
          "summary": "Run ww card <card-id-1>",
          "command": "tools\\ww\\ww-run.cmd <card-id-1> -Main -Engine {engine} -Model {model} -Root <root> -JsonStdout",
          "timeoutSec": 5400,
          "engines": [
            { "engine": "opencode", "model": "<primary-model>" },
            { "engine": "agy", "model": "<backup-model>" }
          ],
          "switchEngineOn": ["startup_hang", "auth_error", "probe_failed"]
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
            "class": "*",
            "do": {
              "type": "escalate",
              "question": "<what should the supervisor decide about card <card-id-1>?>",
              "guidance": "The card failed after all approved engine candidates were tried.",
              "expect": ["continue", "stop", "takeover"],
              "include": { "logs": ["card_<card-id-1>"], "logLines": 100 }
            }
          }
        ],
        "otherwise": "fail",
        "maxRetries": 2
      },
      {
        "type": "job",
        "id": "build_after_<card-id-1>",
        "summary": "Build after card <card-id-1>",
        "command": "cmake --build build --config Release",
        "timeoutSec": 900
      },
      {
        "type": "catch",
        "id": "card_<card-id-2>_guard",
        "child": {
          "type": "job",
          "id": "card_<card-id-2>",
          "summary": "Run ww card <card-id-2>",
          "command": "tools\\ww\\ww-run.cmd <card-id-2> -Main -Engine {engine} -Model {model} -Root <root> -JsonStdout",
          "timeoutSec": 5400,
          "engines": [
            { "engine": "opencode", "model": "<primary-model>" },
            { "engine": "agy", "model": "<backup-model>" }
          ],
          "switchEngineOn": ["startup_hang", "auth_error", "probe_failed"]
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
            "class": "*",
            "do": {
              "type": "escalate",
              "question": "<what should the supervisor decide about card <card-id-2>?>",
              "guidance": "The card failed after all approved engine candidates were tried.",
              "expect": ["continue", "stop", "takeover"],
              "include": { "logs": ["card_<card-id-2>"], "logLines": 100 }
            }
          }
        ],
        "otherwise": "fail",
        "maxRetries": 2
      },
      {
        "type": "job",
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
        "type": "job",
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
          "type": "job",
          "id": "test_${t.stem}",
          "command": "build\\Release\\${t.stem}.exe",
          "summary": "Run ${t.stem}",
          "timeoutSec": 900
        }
      }
    ]
  }
})JSON";

inline std::vector<BtTemplateEntry> BtTemplateEntries() {
    return {
        {"resilient-worker",
         "Worker with idle defaults, probe, two approved engines, catch handlers and takeover-ready escalation."},
        {"card-sequence",
         "ww cards in order, each hang-protected with engines and catch, plus a build job after each card."},
        {"test-suite",
         "Build targets with ${foreach:...}, then run each tests/*_smoke.cpp via a foreach node (mode all)."}
    };
}

inline nlohmann::json BtTemplateDefinition(const std::string& name) {
    const char* text = nullptr;
    if (name == "resilient-worker") text = kResilientWorkerTemplate;
    else if (name == "card-sequence") text = kCardSequenceTemplate;
    else if (name == "test-suite") text = kTestSuiteTemplate;
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
    std::string description;
    for (const auto& e : BtTemplateEntries()) {
        if (e.name == name) { description = e.description; break; }
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
