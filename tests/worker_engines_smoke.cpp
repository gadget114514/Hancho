// Headless test for WorkerEngineRegistry and ValidateOutput (src/worker_engines.cpp).

#include "../src/worker_engines.h"
#include "../src/common.h"
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

int main() {
    SetConsoleOutputCP(CP_UTF8);

    // 1. Built-in engines and default engine
    {
        WorkerEngineRegistry reg;
        std::string err;
        Check(reg.DefaultEngine() == "agy", "default engine is agy");
        Check(reg.Knows("", "", err), "empty engine resolves to default");
        Check(reg.Knows("agy", "gemini-4-argon-high", err), "agy accepts model");
        Check(reg.Knows("claude", "claude-opus-5-5", err), "claude accepts model");
        Check(reg.Knows("opencode", "", err), "opencode accepts empty model");
        Check(reg.Knows("codex", "o3", err), "codex accepts model");
        Check(!reg.Knows("nonexistent", "", err) && !err.empty(), "unknown engine rejected");
    }

    // 2. Command lines for all four engines + model omission
    {
        WorkerEngineRegistry reg;
        std::string cmd, stream, err;

        // agy with default model and timeout
        Check(reg.BuildCommand("agy", "", "Read task.md", 1800, cmd, stream, err), "build agy default model");
        Check(stream == "agy", "agy stream type");
        Check(cmd == "agy.exe -p \"Read task.md\" --output-format stream-json --model gemini-3.8-flash-medium --dangerously-skip-permissions --print-timeout 30m",
              "agy default command: " + cmd);

        // agy with explicit model
        Check(reg.BuildCommand("agy", "gemini-4-argon-high", "Read task.md", 3600, cmd, stream, err), "build agy custom model");
        Check(cmd == "agy.exe -p \"Read task.md\" --output-format stream-json --model gemini-4-argon-high --dangerously-skip-permissions --print-timeout 60m",
              "agy custom model command: " + cmd);

        // claude with model omitted (drops --model {model})
        Check(reg.BuildCommand("claude", "", "Run instructions", 1200, cmd, stream, err), "build claude without model");
        Check(stream == "claude", "claude stream type");
        Check(cmd == "claude -p \"Run instructions\" --output-format stream-json --verbose --dangerously-skip-permissions",
              "claude command omits --model when empty: " + cmd);

        // claude with explicit model
        Check(reg.BuildCommand("claude", "claude-opus-5-5", "Run instructions", 1200, cmd, stream, err), "build claude with model");
        Check(cmd == "claude -p \"Run instructions\" --output-format stream-json --verbose --model claude-opus-5-5 --dangerously-skip-permissions",
              "claude command with model: " + cmd);

        // opencode with model omitted and with model set
        Check(reg.BuildCommand("opencode", "", "Do work", 600, cmd, stream, err), "build opencode without model");
        Check(stream == "opencode", "opencode stream type");
        Check(cmd == "opencode.cmd run --format json \"Do work\"", "opencode command omits --model: " + cmd);

        Check(reg.BuildCommand("opencode", "anthropic/claude-sonnet-4", "Do work", 600, cmd, stream, err), "build opencode with model");
        Check(cmd == "opencode.cmd run --format json --model anthropic/claude-sonnet-4 \"Do work\"",
              "opencode command with model: " + cmd);

        // codex with model omitted and with model set
        Check(reg.BuildCommand("codex", "", "Exec task", 900, cmd, stream, err), "build codex without model");
        Check(stream == "codex", "codex stream type");
        Check(cmd == "codex exec --json --dangerously-bypass-approvals-and-sandbox \"Exec task\"",
              "codex command omits --model: " + cmd);

        Check(reg.BuildCommand("codex", "gpt-5-codex", "Exec task", 900, cmd, stream, err), "build codex with model");
        Check(cmd == "codex exec --json --model gpt-5-codex --dangerously-bypass-approvals-and-sandbox \"Exec task\"",
              "codex command with model: " + cmd);
    }

    // 3. Prompt quoting with spaces, quotes, backslashes, and '&'
    {
        WorkerEngineRegistry reg;
        std::string cmd, stream, err;
        std::string tricky = "Read \"C:\\work\\a b\\task.md\" & echo \"done\\\"\"";
        Check(reg.BuildCommand("opencode", "", tricky, 600, cmd, stream, err), "build with tricky prompt");
        std::string expectedArg = QuoteCommandArg(tricky);
        Check(cmd == "opencode.cmd run --format json " + expectedArg, "prompt quoted as single arg: " + cmd);
        Check(expectedArg.front() == '"' && expectedArg.back() == '"', "quoted arg enclosed in double quotes");
        Check(expectedArg.find("\\\"C:\\work\\a b\\task.md\\\"") != std::string::npos, "embedded quotes escaped");
    }

    // 4. engines.json override and custom engine
    {
        WorkerEngineRegistry reg;
        std::string err, cmd, stream;
        std::string customJson = R"({
            "default": "custom-cli",
            "engines": {
                "claude": {
                    "defaultModel": "claude-sonnet-4-6"
                },
                "custom-cli": {
                    "command": "my-agent.exe --run {prompt} --model {model} {permissionArgs} --limit {timeoutMin}",
                    "defaultModel": "corp-v1",
                    "stream": "claude",
                    "permissionArgs": "--auto-approve",
                    "models": ["corp-v1", "corp-v2"]
                }
            }
        })";
        Check(reg.LoadFromJson(customJson, err), "load engines.json override and new engine: " + err);
        Check(reg.DefaultEngine() == "custom-cli", "default updated from engines.json");

        // Overridden claude now has defaultModel
        Check(reg.BuildCommand("claude", "", "hi", 300, cmd, stream, err), "claude uses overridden defaultModel");
        Check(cmd.find("--model claude-sonnet-4-6") != std::string::npos, "claude command has overridden defaultModel: " + cmd);

        // New custom engine works as default and validates allowed models
        Check(reg.BuildCommand("", "", "test prompt", 125, cmd, stream, err), "default resolves to custom-cli");
        Check(stream == "claude", "custom-cli stream is claude");
        Check(cmd == "my-agent.exe --run \"test prompt\" --model corp-v1 --auto-approve --limit 3",
              "custom-cli command built: " + cmd);

        Check(reg.Knows("custom-cli", "corp-v2", err), "custom-cli accepts allowed model corp-v2");
        Check(!reg.Knows("custom-cli", " disallowed-model ", err) && !err.empty(), "custom-cli rejects unlisted model");
        Check(!reg.BuildCommand("unknown-engine", "", "hi", 60, cmd, stream, err), "BuildCommand rejects unknown engine");
    }

    // 5. ValidateOutput (required and type)
    {
        std::string err;
        std::string schema = R"({
            "type": "object",
            "required": ["answer", "count"],
            "properties": {
                "answer": { "type": "string" },
                "count": { "type": "integer" },
                "score": { "type": "number" },
                "ok": { "type": "boolean" },
                "tags": { "type": "array", "items": { "type": "string" } }
            }
        })";

        Check(ValidateOutput("", "", err), "empty schema accepts anything");
        Check(ValidateOutput(R"({"answer":"approve","count":3,"score":1.5,"ok":true,"tags":["a","b"]})", schema, err),
              "valid output passes schema: " + err);
        Check(!ValidateOutput(R"({"answer":"approve"})", schema, err), "missing required field 'count' fails");
        Check(!ValidateOutput(R"({"answer":123,"count":3})", schema, err), "wrong type for 'answer' fails");
        Check(!ValidateOutput(R"({"answer":"approve","count":3.5})", schema, err), "non-integer for 'count' fails");
        Check(!ValidateOutput(R"({"answer":"approve","count":3,"tags":["a",42]})", schema, err), "wrong array item type fails");
        Check(!ValidateOutput("not json", schema, err), "invalid JSON fails schema validation");
        Check(!ValidateOutput("[]", schema, err), "array when object expected fails");
    }

    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
