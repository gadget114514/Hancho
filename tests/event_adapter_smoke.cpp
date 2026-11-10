// Headless smoke test for the Tier 1 declarative adapters (src/event_adapter.cpp).

#include "../src/event_adapter.h"
#include <windows.h>
#include <cstdio>
#include <string>

using namespace pc;

static int g_failures = 0;

static void Check(bool cond, const std::string& label) {
    std::printf("%s %s\n", cond ? "[PASS]" : "[FAIL]", label.c_str());
    std::fflush(stdout);
    if (!cond) g_failures++;
}

static const char* kAdapter = R"({
  "adapter": "mycli",
  "match": { "field": "/type" },
  "rules": [
    { "when": { "/type": "tool_start" }, "kind": "tool.call",
      "map": { "tool": "/name", "target": "/args/path", "toolUseId": "/id" },
      "activity": { "from": "/name", "table": { "read_file": "read", "write_file": "edit" }, "default": "tool" } },
    { "when": { "/type": "tool_end" }, "kind": "tool.result",
      "map": { "toolUseId": "/id", "ok": "!/error", "error": "/error" } },
    { "when": { "/type": "usage" }, "kind": "usage",
      "map": { "inputTokens": "/in", "outputTokens": "/out" } },
    { "when": { "/type": "done" }, "kind": "run.ended",
      "map": { "status": "/status" } }
  ]
})";

int main() {
    SetConsoleOutputCP(CP_UTF8);

    // JSON Pointer resolution.
    nlohmann::json doc = nlohmann::json::parse(R"({"a":{"b":[10,20]},"x/y":{"~k":true}})");
    Check(ResolveJsonPointer(doc, "/a/b/1") && *ResolveJsonPointer(doc, "/a/b/1") == 20,
          "pointer resolves an array element");
    Check(ResolveJsonPointer(doc, "/x~1y/~0k") && *ResolveJsonPointer(doc, "/x~1y/~0k") == true,
          "pointer unescapes ~1 and ~0");
    Check(ResolveJsonPointer(doc, "/a/b/9") == nullptr, "pointer returns null when out of range");
    Check(ResolveJsonPointer(doc, "a") == nullptr, "pointer requires a leading slash");

    // Loader validation.
    HanchoAdapter adapter;
    std::string err;
    Check(LoadHanchoAdapterFromJson(nlohmann::json::parse(kAdapter), adapter, err),
          "design example adapter loads: " + err);
    Check(adapter.name == "mycli" && adapter.matchField == "/type" && adapter.rules.size() == 4,
          "adapter keeps name, match field and rules");

    {
        HanchoAdapter bad;
        Check(!LoadHanchoAdapterFromJson(nlohmann::json::parse(R"({"adapter":"x","rules":[]})"), bad, err),
              "an empty rules array is rejected");
        Check(!LoadHanchoAdapterFromJson(
                  nlohmann::json::parse(R"({"adapter":"x","rules":[{"kind":"nope"}]})"), bad, err),
              "an unknown kind is rejected");
        Check(!LoadHanchoAdapterFromJson(
                  nlohmann::json::parse(R"({"adapter":"x","rules":[{"kind":"message","map":{"text":1}}]})"),
                  bad, err),
              "a non-string map value is rejected");
        Check(!LoadHanchoAdapterFromJson(
                  nlohmann::json::parse(R"({"adapter":"x","rules":[{"kind":"message","activity":{}}]})"),
                  bad, err),
              "activity without 'from' is rejected");

        HanchoAdapter tier3;
        Check(LoadHanchoAdapterFromJson(
                  nlohmann::json::parse(R"({"adapter":"t3","exec":"converter.exe"})"), tier3, err),
              "a Tier 3 adapter may omit rules: " + err);
        Check(tier3.exec == "converter.exe" && tier3.rules.empty(),
              "the Tier 3 adapter keeps its exec command");
        Check(!LoadHanchoAdapterFromJson(
                  nlohmann::json::parse(R"({"adapter":"t3","exec":1})"), tier3, err),
              "a non-string exec is rejected");
        Check(!LoadHanchoAdapterFromJson(nlohmann::json::parse(R"({"adapter":"x"})"), tier3, err),
              "an adapter needs rules or exec");
    }

    // Rule mapping.
    nlohmann::json data;
    std::string kind = NormalizeHanchoAdapterLine(
        adapter, R"({"type":"tool_start","id":"t1","name":"read_file","args":{"path":"src/a.cpp"}})", data);
    Check(kind == "tool.call", "tool_start maps to tool.call");
    Check(data["tool"] == "read_file" && data["target"] == "src/a.cpp" && data["toolUseId"] == "t1" &&
              data["activity"] == "read",
          "tool_start maps tool, target, toolUseId and activity");

    kind = NormalizeHanchoAdapterLine(adapter, R"({"type":"tool_start","name":"shell"})", data);
    Check(kind == "tool.call" && data["activity"] == "tool" && !data.contains("target"),
          "activity falls back to the default and missing pointers are skipped");

    kind = NormalizeHanchoAdapterLine(adapter, R"({"type":"tool_end","id":"t1"})", data);
    Check(kind == "tool.result" && data["ok"] == true && !data.contains("error"),
          "negated pointer is true when the field is absent");

    kind = NormalizeHanchoAdapterLine(adapter, R"({"type":"tool_end","id":"t1","error":"boom"})", data);
    Check(kind == "tool.result" && data["ok"] == false && data["error"] == "boom",
          "negated pointer is false when the field is present");

    kind = NormalizeHanchoAdapterLine(adapter, R"({"type":"usage","in":100,"out":20})", data);
    Check(kind == "usage" && data["inputTokens"] == 100 && data["outputTokens"] == 20,
          "usage maps token counts");

    kind = NormalizeHanchoAdapterLine(adapter, R"({"type":"done","status":"completed"})", data);
    Check(kind == "run.ended" && data["status"] == "completed", "done maps to run.ended");

    kind = NormalizeHanchoAdapterLine(adapter, R"({"type":"mystery","x":1})", data);
    Check(kind == "raw" && data["text"] == R"({"type":"mystery","x":1})", "unmatched rules become raw");

    kind = NormalizeHanchoAdapterLine(adapter, "not json", data);
    Check(kind == "raw" && data["text"] == "not json", "non-JSON lines become raw");

    kind = NormalizeHanchoAdapterLine(adapter, R"({"x":1})", data);
    Check(kind == "raw", "a line without the match field becomes raw");

    // v1 -> internal event mapping (live state).
    LlmStreamEvent ev = LlmStreamEventFromHanchoV1(
        nlohmann::json::parse(R"({"kind":"tool.call","data":{"tool":"Edit","toolUseId":"t1","target":"a","activity":"edit"}})"));
    Check(ev.type == "tool_call" && ev.toolName == "Edit" && ev.toolUseId == "t1",
          "v1 tool.call maps to the internal event");

    ev = LlmStreamEventFromHanchoV1(
        nlohmann::json::parse(R"({"kind":"tool.result","data":{"ok":false,"error":"denied"}})"));
    Check(ev.type == "tool_result" && ev.toolError && ev.content == "denied",
          "v1 tool.result maps ok/error");

    ev = LlmStreamEventFromHanchoV1(
        nlohmann::json::parse(R"({"kind":"usage","data":{"inputTokens":7,"outputTokens":3,"costUsd":0.5}})"));
    Check(ev.type == "usage" && ev.inputTokens == 7 && ev.outputTokens == 3 && ev.costUsd == 0.5,
          "v1 usage maps token counts and cost");

    ev = LlmStreamEventFromHanchoV1(
        nlohmann::json::parse(R"({"kind":"run.ended","data":{"status":"error","stopReason":"max_turns"}})"));
    Check(ev.type == "result" && ev.status == "error" && ev.stopReason == "max_turns",
          "v1 run.ended maps status and stop reason");

    if (g_failures == 0) std::printf("ALL PASS\n");
    else std::printf("%d check(s) failed\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
