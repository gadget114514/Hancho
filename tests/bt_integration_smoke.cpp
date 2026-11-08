// Headless integration smoke test for BtManager + TaskQueueManager + ProcessManager (Part 1).

#include "bt_integration_smoke_common.h"

using namespace bt_int_test;

int main() {
    wchar_t tempBase[MAX_PATH] = {};
    GetTempPathW(MAX_PATH, tempBase);
    std::wstring tempDirW = std::wstring(tempBase) + L"pc_bt_int_smoke_" + std::to_wstring(GetCurrentProcessId());
    RemoveDirRecursive(tempDirW);
    CreateDirectoryW(tempDirW.c_str(), nullptr);
    SetDataDirectoryOverride(tempDirW);
    std::string tempDirUtf8 = WideToUtf8(tempDirW);

    // Prepare custom worker script and engines.json in the data directory.
    std::wstring workerBatW = tempDirW + L"\\mock_worker.bat";
    std::string workerBatContent =
        "@echo off\r\n"
        "if \"%MOCK_WORKER_BAD_SCHEMA%\"==\"1\" (\r\n"
        "    echo {\"wrong_key\":\"nope\"} > \"%PC_BT_RESULT_FILE%\"\r\n"
        "    echo Worker produced invalid schema\r\n"
        "    exit /b 0\r\n"
        ")\r\n"
        "echo {\"target\":\"module_alpha\",\"count\":7} > \"%PC_BT_RESULT_FILE%\"\r\n"
        "echo Worker analyzed module_alpha\r\n"
        "exit /b 0\r\n";
    WriteFileAtomicWide(workerBatW, workerBatContent);

    std::string workerCmd = QuoteCommandArg(WideToUtf8(workerBatW)) + " --model {model} --prompt {prompt}";
    json enginesJson = {
        {"default", "test-worker"},
        {"engines", {
            {"test-worker", {
                {"command", workerCmd},
                {"models", json::array({"test-m1"})},
                {"defaultModel", "test-m1"}
            }}
        }}
    };
    WriteFileAtomicWide(tempDirW + L"\\engines.json", enginesJson.dump(2));

    {
        TestHarness h;

        // 1. Action happy path: sequence of two cmd /c jobs + fileNewer condition
        {
            std::wstring step1OutW = tempDirW + L"\\step1_out.txt";
            std::wstring step2OutW = tempDirW + L"\\step2_out.txt";
            DeleteFileW(step1OutW.c_str());
            DeleteFileW(step2OutW.c_str());

            json btDef = {
                {"id", "bt-action-happy"},
                {"summary", "Action happy path"},
                {"cwd", tempDirUtf8},
                {"notifyFile", "notify_happy.json"},
                {"root", {
                    {"type", "sequence"},
                    {"children", json::array({
                        {
                            {"type", "job"},
                            {"id", "step1"},
                            {"summary", "Create step1_out.txt"},
                            {"command", "cmd.exe /c echo HELLO_STEP1 > step1_out.txt"},
                            {"timeoutSec", 30}
                        },
                        {
                            {"type", "fileNewer"},
                            {"id", "check_step1"},
                            {"path", "step1_out.txt"},
                            {"than", "${step1.start}"}
                        },
                        {
                            {"type", "job"},
                            {"id", "step2"},
                            {"summary", "Create step2_out.txt"},
                            {"command", "cmd.exe /c echo HELLO_STEP2 > step2_out.txt"},
                            {"timeoutSec", 30}
                        }
                    })}
                }}
            };

            json valResp = json::parse(h.btm->Validate(btDef.dump()), nullptr, false);
            Check(valResp.value("ok", false) && valResp.value("valid", false),
                  "Validate accepts bt-action-happy");

            json startResp = json::parse(h.btm->Start(btDef.dump()), nullptr, false);
            Check(startResp.value("ok", false), "Start bt-action-happy ok");

            json waitShort = json::parse(h.btm->Wait("bt-action-happy", 0, 0), nullptr, false);
            if (waitShort.value("state", "") == "running") {
                Check(waitShort.value("timedOut", false) && !waitShort.contains("tree") &&
                          !waitShort.contains("compact") && waitShort.contains("version"),
                      "Wait timeout while running returns a light reply: " + waitShort.dump());
            }

            json waitResp = json::parse(h.btm->Wait("bt-action-happy", 0, 15000), nullptr, false);
            Check(waitResp.value("ok", false) && waitResp.value("state", "") == "succeeded",
                  "Wait bt-action-happy reached succeeded (got " + waitResp.value("state", "") + ")");

            Check(FileExistsW(step1OutW) && FileExistsW(step2OutW),
                  "both step1_out.txt and step2_out.txt created");
            Check(FileExistsW(BtManager::GetBtStateFileWide("bt-action-happy")),
                  "state file exists on disk for bt-action-happy");
            Check(FileExistsW(BtManager::GetBtEventsFileWide("bt-action-happy")),
                  "events jsonl exists on disk for bt-action-happy");
            std::string eventsContent = ReadFileUtf8Wide(BtManager::GetBtEventsFileWide("bt-action-happy"));
            Check(eventsContent.find("btSucceeded") != std::string::npos,
                  "bt-action-happy events file contains btSucceeded");
            Check(FileExistsW(tempDirW + L"\\notify_happy.json"),
                  "notifyFile written on btSucceeded");
        }

        // 2. Failure + Retry: job exits non-zero, fix file created, Retry(id, fromNode) re-runs only failed node
        {
            std::wstring count1FileW = tempDirW + L"\\run_first.txt";
            std::wstring fixFileW = tempDirW + L"\\fix_ready.flag";
            DeleteFileW(count1FileW.c_str());
            DeleteFileW(fixFileW.c_str());

            json btDef = {
                {"id", "bt-fail-retry"},
                {"cwd", tempDirUtf8},
                {"root", {
                    {"type", "sequence"},
                    {"children", json::array({
                        {
                            {"type", "job"},
                            {"id", "first"},
                            {"summary", "First step"},
                            {"command", "cmd.exe /c echo FIRST >> run_first.txt"},
                            {"timeoutSec", 30}
                        },
                        {
                            {"type", "job"},
                            {"id", "flaky"},
                            {"summary", "Needs fix_ready.flag"},
                            {"command", "cmd.exe /c if exist fix_ready.flag (exit 0) else (exit 1)"},
                            {"timeoutSec", 30}
                        }
                    })}
                }}
            };

            h.btm->Start(btDef.dump());
            json waitFail = json::parse(h.btm->Wait("bt-fail-retry", 0, 15000), nullptr, false);
            Check(waitFail.value("state", "") == "failed",
                  "bt-fail-retry failed when fix_ready.flag missing");

            // Create the fix file and retry from 'flaky'
            WriteFileAtomicWide(fixFileW, "fixed");
            json retryResp = json::parse(h.btm->Retry("bt-fail-retry", "flaky", false), nullptr, false);
            Check(retryResp.value("ok", false), "Retry bt-fail-retry from flaky ok");

            json waitOk = json::parse(h.btm->Wait("bt-fail-retry", 0, 15000), nullptr, false);
            Check(waitOk.value("state", "") == "succeeded",
                  "bt-fail-retry reached succeeded after Retry");

            // Verify 'first' only ran once
            int firstRuns = waitOk["tree"]["root"]["children"][0]["status"].value("runs", 0);
            int flakyRuns = waitOk["tree"]["root"]["children"][1]["status"].value("runs", 0);
            Check(firstRuns == 1 && flakyRuns == 2,
                  "first ran once (1) and flaky ran twice (2) after Retry");
        }

        // 3. Worker node with custom engine: outputSchema passes, <node>.output.* and <node>.result populated
        {
            std::wstring verifyOutW = tempDirW + L"\\worker_verify.txt";
            DeleteFileW(verifyOutW.c_str());

            json btDef = {
                {"id", "bt-worker-ok"},
                {"cwd", tempDirUtf8},
                {"vars", {{"long_note", std::string(300, 'x')}}},
                {"root", {
                    {"type", "sequence"},
                    {"children", json::array({
                        {
                            {"type", "worker"},
                            {"id", "w1"},
                            {"engine", "test-worker"},
                            {"model", "test-m1"},
                            {"summary", "Analyze target"},
                            {"prompt", "Analyze the codebase and write structured output."},
                            {"timeoutSec", 60},
                            {"outputSchema", {
                                {"type", "object"},
                                {"required", json::array({"target", "count"})},
                                {"properties", {
                                    {"target", {{"type", "string"}}},
                                    {"count", {{"type", "integer"}}}
                                }}
                            }}
                        },
                        {
                            {"type", "job"},
                            {"id", "use_out"},
                            {"summary", "Use ${w1.output.target}:${w1.output.count}"},
                            {"command", "cmd.exe /c echo TARGET=${w1.output.target} COUNT=${w1.output.count} > worker_verify.txt"},
                            {"timeoutSec", 30}
                        }
                    })}
                }}
            };

            json startW = json::parse(h.btm->Start(btDef.dump()), nullptr, false);
            Check(startW.value("ok", false), "Start bt-worker-ok ok");

            json waitW = json::parse(h.btm->Wait("bt-worker-ok", 0, 15000), nullptr, false);
            Check(waitW.value("state", "") == "succeeded",
                  "bt-worker-ok succeeded (got " + waitW.value("state", "") + ", detail=" + waitW.value("detail", "") + ")");

            const json& vars = waitW["tree"]["vars"];
            Check(vars.value("w1.output.target", "") == "module_alpha" &&
                  vars.value("w1.output.count", "") == "7",
                  "w1.output.target and w1.output.count populated in vars");
            Check(vars.value("w1.result", "").find("Worker analyzed module_alpha") != std::string::npos,
                  "w1.result populated from worker output");

            // bt_wait cuts long variable values; bt_get returns them in full.
            const std::string trimmed = vars.value("long_note", "");
            Check(trimmed.size() < 300 && trimmed.find("truncated, 300 bytes") != std::string::npos,
                  "bt_wait trims long variable values: " + trimmed);
            json getW = json::parse(h.btm->Get("bt-worker-ok", false), nullptr, false);
            Check(getW["tree"]["vars"].value("long_note", "") == std::string(300, 'x'),
                  "bt_get returns full variable values");

            // The worker sees the node's outputSchema in its instructions file.
            std::string w1Job;
            const json& w1Jobs = getW["tree"]["root"]["children"][0]["status"]["jobs"];
            if (w1Jobs.is_array() && !w1Jobs.empty()) w1Job = w1Jobs.back().get<std::string>();
            std::string instr = ReadFileUtf8Wide(
                TaskQueueManager::GetInstructionsFileWide(getW.value("taskqueue", ""), w1Job, 1));
            Check(instr.find("Required result file") != std::string::npos &&
                      instr.find("PC_BT_RESULT_FILE") != std::string::npos &&
                      instr.find("\"count\"") != std::string::npos,
                  "worker instructions include the outputSchema: " + instr);
            std::string verifyContent = ReadFileUtf8Wide(verifyOutW);
            Check(verifyContent.find("module_alpha") != std::string::npos &&
                  verifyContent.find("7") != std::string::npos,
                  "next node expanded ${w1.output.target} and ${w1.output.count}");
        }

        // 4. Worker outputSchema failure: worker exits 0 with missing required fields -> outputInvalid
        {
            json btDef = {
                {"id", "bt-worker-bad-schema"},
                {"cwd", tempDirUtf8},
                {"root", {
                    {"type", "worker"},
                    {"id", "wbad"},
                    {"engine", "test-worker"},
                    {"model", "test-m1"},
                    {"summary", "Bad schema worker"},
                    {"prompt", "Produce bad schema."},
                    {"timeoutSec", 60},
                    {"env", {{"MOCK_WORKER_BAD_SCHEMA", "1"}}},
                    {"outputSchema", {
                        {"type", "object"},
                        {"required", json::array({"target", "count"})},
                        {"properties", {
                            {"target", {{"type", "string"}}},
                            {"count", {{"type", "integer"}}}
                        }}
                    }}
                }}
            };

            h.btm->Start(btDef.dump());
            json waitBad = json::parse(h.btm->Wait("bt-worker-bad-schema", 0, 15000), nullptr, false);
            Check(waitBad.value("state", "") == "failed" && waitBad.value("detail", "").rfind("outputInvalid: ", 0) == 0,
                  "worker with invalid schema fails with outputInvalid (got state=" +
                  waitBad.value("state", "") + ", detail=" + waitBad.value("detail", "") + ")");
        }

        // 5. Escalation round-trip: writes <node>.1.json and <node>.1.md, Wait returns immediately, Resume completes
        {
            json btDef = {
                {"id", "bt-esc-roundtrip"},
                {"intent", "Test escalation lifecycle"},
                {"cwd", tempDirUtf8},
                {"root", {
                    {"type", "sequence"},
                    {"children", json::array({
                        {
                            {"type", "escalate"},
                            {"id", "gate"},
                            {"question", "Proceed with deployment?"},
                            {"guidance", "Choose ship or abort"},
                            {"expect", json::array({"ship", "abort"})},
                            {"timeoutSec", 300}
                        },
                        {
                            {"type", "job"},
                            {"id", "after_gate"},
                            {"summary", "Decision was ${gate.answer} via ${chosen_env}"},
                            {"command", "cmd.exe /c echo DECIDED_${gate.answer}_${chosen_env}"},
                            {"timeoutSec", 30}
                        }
                    })}
                }}
            };

            h.btm->Start(btDef.dump());
            json waitEsc = json::parse(h.btm->Wait("bt-esc-roundtrip", 0, 5000), nullptr, false);
            Check(waitEsc.value("state", "") == "waitingForLlm",
                  "Wait returned immediately with state waitingForLlm");
            Check(waitEsc.contains("escalation") &&
                  waitEsc["escalation"].value("question", "") == "Proceed with deployment?",
                  "Wait response includes escalation packet");

            std::wstring escJsonW = BtManager::GetBtEscalationFileWide("bt-esc-roundtrip", "gate", 1, L".json");
            std::wstring escMdW = BtManager::GetBtEscalationFileWide("bt-esc-roundtrip", "gate", 1, L".md");
            Check(FileExistsW(escJsonW) && FileExistsW(escMdW),
                  "escalation packet files gate.1.json and gate.1.md written to disk");

            json resumeResp = json::parse(
                h.btm->Resume("bt-esc-roundtrip", "gate", "ship", "All checks green", R"({"chosen_env":"prod"})"),
                nullptr, false);
            Check(resumeResp.value("ok", false), "Resume bt-esc-roundtrip ok");

            json waitDone = json::parse(h.btm->Wait("bt-esc-roundtrip", 0, 15000), nullptr, false);
            Check(waitDone.value("state", "") == "succeeded",
                  "bt-esc-roundtrip reached succeeded after Resume");
            Check(waitDone["tree"]["vars"].value("gate.answer", "") == "ship" &&
                  waitDone["tree"]["vars"].value("chosen_env", "") == "prod",
                  "gate.answer and extra vars set by Resume");
        }
    }

    RunPart2IntegrationTests(tempDirW, tempDirUtf8);

    RemoveDirRecursive(tempDirW);
    std::printf("%s (%d passed, %d failures)\n", g_failures ? "FAILED" : "ALL PASSED", g_passes, g_failures);
    return g_failures ? 1 : 0;
}
