// Headless test for WorkerEngineRegistry and ValidateOutput (src/worker_engines.cpp).

#include "../src/worker_engines.h"
#include "../src/llm_config.h"
#include "../src/permit_config.h"
#include "../src/worker_probe.h"
#include "../src/common.h"
#include <windows.h>
#include <cstdio>
#include <string>

using namespace pc;
using json = nlohmann::json;

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

    // 2b. Read-only advice permissions replace the worker permission flags
    {
        WorkerEngineRegistry reg;
        std::string cmd, stream, err;
        const WorkerEngineDef* agy = reg.Find("agy");
        Check(agy && agy->advicePermissionArgs == "--mode plan", "agy has read-only advice args");
        if (agy) {
            Check(reg.BuildCommand("agy", "", "Read task.md", 1800, cmd, stream, err, nullptr,
                                   &agy->advicePermissionArgs),
                  "build agy advice command");
            Check(cmd == "agy.exe -p \"Read task.md\" --output-format stream-json --model gemini-3.8-flash-medium --mode plan --print-timeout 30m",
                  "agy advice command uses --mode plan: " + cmd);
        }
        std::string overrideArgs = "";
        Check(reg.BuildCommand("claude", "", "Read task.md", 300, cmd, stream, err, nullptr, &overrideArgs),
              "build claude with an empty advice override");
        Check(cmd == "claude -p \"Read task.md\" --output-format stream-json --verbose",
              "empty advice override drops --dangerously-skip-permissions: " + cmd);
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
                    "models": ["corp-v1", "corp-v2"],
                    "adapter": "mycli",
                    "capabilities": {
                        "jsonStream": true,
                        "resume": false,
                        "reportsUsage": true,
                        "reportsSubagents": false,
                        "permissionModes": ["default", "auto"]
                    }
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

        // adapter and capabilities from engines.json
        const WorkerEngineDef* def = reg.Find("custom-cli");
        Check(def && def->adapter == "mycli", "engine keeps the adapter name");
        Check(def && def->capabilities.jsonStream && !def->capabilities.resume &&
                  def->capabilities.reportsUsage && !def->capabilities.reportsSubagents,
              "engine keeps capability booleans");
        Check(def && def->capabilities.permissionModes.size() == 2 &&
                  def->capabilities.permissionModes[1] == "auto",
              "engine keeps capability permission modes");
        std::string adapterOut;
        Check(reg.BuildCommand("custom-cli", "", "hi", 60, cmd, stream, err, &adapterOut) &&
                  adapterOut == "mycli",
              "BuildCommand reports the adapter name");
        Check(reg.BuildCommand("claude", "", "hi", 60, cmd, stream, err, &adapterOut) && adapterOut.empty(),
              "BuildCommand reports an empty adapter when none is set");

        std::string badCaps = R"({"engines":{"x":{"command":"x {prompt}","capabilities":{"resume":"yes"}}}})";
        Check(!reg.LoadFromJson(badCaps, err) && !err.empty(), "capabilities booleans are validated");
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

    // 8. Quick-fix script generation (deterministic one-file edit, no LLM)
    {
        QuickFixSpec spec;
        spec.file = "C:\\work\\a'b.cs";
        spec.find = "Retun";
        spec.replace = "Return";
        spec.check = "Return";
        spec.all = true;
        const std::string ps = BuildQuickFixPowerShell(spec);
        Check(ps.find("find text not found") != std::string::npos, "quickfix script fails when find is absent");
        Check(ps.find("check did not match") != std::string::npos, "quickfix script fails when check does not match");
        Check(ps.find("a''b.cs") != std::string::npos, "quickfix escapes single quotes for PowerShell");
        Check(ps.find("$text.Replace($find, $replace)") != std::string::npos, "quickfix --all replaces every occurrence");
        const std::string cmd = BuildQuickFixCommandLine(L"C:\\work\\quickfix.ps1");
        Check(cmd.find("-File \"C:\\work\\quickfix.ps1\"") != std::string::npos,
              "quickfix command runs the script file: " + cmd);
    }

    // 9. 091-3: per-role LLM lists, prior approval, history and probe cache.
    {
        const std::wstring dir = PathJoinWide(GetCurrentDirectoryWide(), L".hancho\\0913-smoke");
        EnsureDirectoryWide(dir);
        const std::wstring cfgPath = PathJoinWide(dir, L"llms.json");
        RemoveFileWide(cfgPath);

        auto r = llm::Add(cfgPath, "worker", "opencode", "opencode-go/deepseek-v4.1-flash",
                          "", 0, false, "primary", "cli", "seed worker");
        Check(r.ok, "llm add worker succeeds");
        r = llm::Add(cfgPath, "consult", "opencode", "opencode-go/deepseek-v4.1-flash",
                     "", 0, false, "same model, different role", "cli", "seed consult");
        Check(r.ok, "llm add consult succeeds");

        llm::Config cfg;
        std::string err;
        Check(llm::Load(cfgPath, cfg, err) && cfg.worker.size() == 1 && cfg.consult.size() == 1 &&
              cfg.supervisor.empty(), "worker and consult lists are independent");
        Check(cfg.worker[0].status == "proposed" && !cfg.worker[0].Usable(),
              "LLM-added row starts proposed and is not usable");

        r = llm::Approve(cfgPath, "worker", "opencode", "opencode-go/deepseek-v4.1-flash",
                         "yes, use opencode", "chat");
        Check(r.ok, "llm approve succeeds");
        llm::Load(cfgPath, cfg, err);
        Check(cfg.worker[0].status == "approved" && cfg.worker[0].approvedVia == "chat" &&
              cfg.worker[0].userSaid == "yes, use opencode", "approval records quote and route");
        Check(cfg.consult[0].status == "proposed", "approving worker did not approve consult");
        Check(llm::HasUsable(cfg, "worker", "opencode", "opencode-go/deepseek-v4.1-flash"),
              "approved worker candidate is usable");
        Check(!llm::HasUsable(cfg, "consult", "opencode", "opencode-go/deepseek-v4.1-flash"),
              "proposed consult candidate is not usable");
        Check(!llm::Approve(cfgPath, "worker", "opencode", "opencode-go/deepseek-v4.1-flash", "", "chat").ok,
              "empty --user-said is rejected");

        r = llm::SetEnabled(cfgPath, "worker", "opencode", "opencode-go/deepseek-v4.1-flash", false, "cli", "rest");
        Check(r.ok, "disable succeeds without re-approval");
        llm::Load(cfgPath, cfg, err);
        Check(!llm::HasUsable(cfg, "worker", "opencode", "opencode-go/deepseek-v4.1-flash"),
              "disabled approved row is not usable");
        Check(cfg.history.size() >= 4, "every mutation is recorded in history");
        r = llm::Edit(cfgPath, "worker", "opencode", "opencode-go/deepseek-v4.1-flash",
                      "", "opencode-go/new-model", "", "cli", "model change");
        Check(r.ok, "edit succeeds");
        llm::Load(cfgPath, cfg, err);
        Check(cfg.worker[0].status == "proposed", "editing model returns row to proposed");

        // Probe: dummy engine in a registry, success + cache + hanging engine classification.
        WorkerEngineRegistry reg;
        std::string engErr;
        Check(reg.LoadFromJson(R"({"engines":{
            "okdummy":{"command":"cmd /c echo OK","stream":"dummy","models":[]},
            "hangdummy":{"command":"cmd /c ping -n 20 127.0.0.1 >nul","stream":"dummy","models":[]}
        },"default":"okdummy"})", engErr), "dummy engines load");
        const std::wstring probePath = PathJoinWide(dir, L"probes.json");
        RemoveFileWide(probePath);
        ProbeOptions opts;
        opts.timeoutSec = 3;
        ProbeOutcome p1 = ProbeEngine(reg, "okdummy", "", opts, probePath);
        Check(p1.ok && !p1.cached, "probe succeeds against dummy engine");
        ProbeOutcome p2 = ProbeEngine(reg, "okdummy", "", opts, probePath);
        Check(p2.ok && p2.cached, "fresh probe result is served from cache");
        ProbeOutcome p3 = ProbeEngine(reg, "okdummy", "", opts, probePath);
        Check(p3.cached, "still cached");
        ProbeOutcome ph = ProbeEngine(reg, "hangdummy", "", opts, probePath);
        Check(!ph.ok && ph.failureClass == "startup_hang", "hanging engine classified as startup_hang");
        opts.noCache = true;
        ProbeOutcome p4 = ProbeEngine(reg, "okdummy", "", opts, probePath);
        Check(p4.ok && !p4.cached, "--no-cache bypasses the cache");
    }

    // 10. 091-7: permits.json, approvals, and conversion into engine launch config.
    {
        const std::wstring dir = PathJoinWide(GetCurrentDirectoryWide(), L".hancho\\0917-smoke");
        EnsureDirectoryWide(dir);
        const std::wstring path = PathJoinWide(dir, L"permits.json");
        RemoveFileWide(path);

        auto r = permit::Request(path, "read_path", "%TEMP%/pc_*/**", {"opencode"}, "",
                                 "test artifact directory", "cli");
        Check(r.ok && r.data.value("entry", json()).value("status", "") == "proposed",
              "permit request starts proposed");
        const std::string id = permit::MakeId("read_path", "%TEMP%/pc_*/**", "global");
        r = permit::List(path, "");
        Check(r.ok && r.data.value("proposed", json::array()).size() == 1,
              "proposed permits are listed as pending");

        r = permit::Approve(path, id, "", "chat");
        Check(!r.ok, "empty --user-said is rejected for permits");
        r = permit::Approve(path, id, "yes, allow reading test artifacts", "chat");
        Check(r.ok && r.data.value("entry", json()).value("approvedVia", "") == "chat",
              "chat approval records approvedVia and the quote");

        permit::Config cfg;
        std::string err;
        Check(permit::Load(path, cfg, err) && cfg.permits.size() == 1 &&
                  cfg.permits[0].status == "approved" && cfg.history.size() == 2,
              "every permit mutation is recorded in history");
        Check(permit::IsApproved(cfg, "read_path", permit::ExpandTarget("%TEMP%/pc_1/a.txt"),
                                 "opencode", ""),
              "approved glob covers a matching expanded target");
        Check(!permit::IsApproved(cfg, "read_path", "C:/Users/x/.config/opencode/AGENTS.md",
                                  "opencode", ""),
              "approved glob does not cover another target");

        r = permit::Request(path, "command", "git status *", {}, "", "diagnostics", "cli");
        const std::string cmdId = permit::MakeId("command", "git status *", "global");
        Check(r.ok && permit::ApproveViaDashboard(path, cmdId, "dashboard").ok,
              "dashboard approval works too");
        permit::Load(path, cfg, err);
        const auto approved = json{{"kind", "read_path"}, {"target", "%TEMP%/pc_*/**"},
                                   {"engines", json::array({"opencode"})}},
                            cmd = json{{"kind", "command"}, {"target", "git status *"},
                                        {"engines", json::array()}};
        json rows = json::array({approved, cmd});

        WorkerEngineRegistry reg;
        std::vector<std::string> warnings;
        const std::string envJson = reg.BuildPermitEnvironment("opencode", rows, warnings);
        Check(!envJson.empty() && warnings.empty(), "opencode permission config is produced");
        json env = json::parse(envJson, nullptr, false);
        Check(env["external_directory"]["*"] == "deny",
              "opencode default denies access outside the project");
        Check(env["external_directory"].contains("%TEMP%/pc_*/**") ||
                  env.dump().find("pc_*/**") != std::string::npos,
              "approved read path is allowed in opencode config");
        Check(env["bash"]["git status *"] == "allow",
              "approved command is allowed in opencode config");
        Check(!env.contains("permission"),
              "OPENCODE_PERMISSION carries the rules object (no extra 'permission' wrapper)");
        Check(env.dump().find("deny") != std::string::npos,
              "deny (not ask/--auto) is used for unapproved operations");

        std::vector<std::string> warn2;
        const std::string args = reg.BuildPermitArgs("claude", rows, warn2);
        Check(args.find("--add-dir") != std::string::npos &&
                  args.find("--allowedTools") != std::string::npos,
              "claude receives --add-dir and --allowedTools: " + args);
        std::string cmdLine, stream, buildErr;
        Check(reg.BuildCommand("claude", "", "Do work", 600, cmdLine, stream, buildErr, nullptr, nullptr,
                               &args) &&
                  cmdLine.find("--add-dir") != std::string::npos,
              "claude command includes approved permit args");
        Check(BuildWorkerPermitsJson(rows).find("git status *") != std::string::npos,
              "HANCHO_WORKER_PERMITS payload contains the approved rows");
        std::vector<std::string> warn3;
        Check(reg.BuildPermitEnvironment("nonexistent", rows, warn3).empty() && !warn3.empty(),
              "unsupported engines warn and receive no config");
    }

    std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    return g_failures ? 1 : 0;
}
