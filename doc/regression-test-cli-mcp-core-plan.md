# Plan: regression tests for hancho-cli, MCP, and the core

Status: design approved (including demo options in the release binary, section 4). P0, P1 and the triage slice are done (sections 13, 14 and 15). Next: triage disable for all scenarios, cross-layer check, and the rest of the phases.

This is the first slice of [regression-test-plan.md](regression-test-plan.md). That document covers every feature of Hancho. This one covers the three surfaces that users and agents call: hancho-cli, MCP, and the core. GUI, WebUI, chat (ipmsg), terminal server, `wct`, and the installer are out of scope for now.

## 1. Decisions

| # | Topic | Decision |
| :-- | :--- | :--- |
| 1 | C++ dummy `hancho-dummy.exe` | Keep it for `demo_smoke`. The regression tests use a separate Python dummy. Two implementations exist on purpose; `demo_smoke` keeps checking the C++ one. |
| 2 | How the core finds the Python dummy | The runner passes the path to the `.cmd` on the command line when it starts the core. |
| 3 | How MCP is invoked | Same as claude and opencode. The runner runs `hancho-cli.exe mcp install --data-dir <tmp>` (`--client claude` or `--client opencode`) in a temp working directory, reads the generated config, and starts `Hancho.exe mcp` with the command and arguments from that config over stdio. |
| 4 | Python | Python 3, standard library only. The `.cmd` launchers call `python` (or `py -3` if `python` is absent). |
| 5 | Binary under test | The release build `build\Release`. |
| 6 | Demo and release | The release build and the demo build are the same code. Demo behavior (pinned clock, demo data, dummy job) is switched on by a runtime option, not by a separate build. See section 4 for what this means for the release package. |
| 7 | Layout of the runner | Recommended in the previous plan: C++ is replaced by Python for the runner (decision 4). Lives in `tests/regression/`. |
| 8 | Timeouts and parallelism | See section 9. |
| 9 | Formats | JSON for scenarios and goldens. |
| 10 | Where it runs | Local (Windows) and CI. Python 3 is available on both. |
| 11 | Relation to existing `tests/` | `tests/regression/` is not part of CMake. The C++ smoke tests are unchanged. |

## 2. Layout

```
tests/regression/
├── run.py                    # runner (Python 3, standard library only)
├── run.cmd                   # launcher: python run.py %*
├── dummy_llm.py              # Python dummy LLM (Claude stream-json style output)
├── dummy_llm.cmd             # launcher: python dummy_llm.py %*
├── op-map.json               # abstract operation -> L1 action / L2 command / L3 tool
├── not-covered.txt           # inventory items without a scenario, with a reason
├── scenarios/
│   ├── taskqueue/
│   ├── bt/
│   ├── direction/
│   └── ...
└── golden/
    ├── core/                 # L1 responses
    ├── cli/                  # L2 stdout, exit code
    └── mcp/                  # L3 tools/list and tools/call results
```

The runner takes the binary directory on the command line, for example `--bin-dir build\Release`.

## 3. Components

### 3.1 Runner (`run.py`)

Commands:

```
run.cmd run     --bin-dir build\Release [--filter <glob>] [--layers core,cli,mcp] [--report <dir>]
run.cmd check   <scenario> --bin-dir build\Release       # run one, show diff
run.cmd record  <scenario> --bin-dir build\Release       # overwrite goldens (explicit only)
run.cmd coverage --bin-dir build\Release                 # inventory vs scenarios
```

Responsibilities are described in section 5.

### 3.2 Dummy LLM (`dummy_llm.py`, `dummy_llm.cmd`)

- Writes Claude stream-json style output: token usage and subagent events.
- `--scenario usage|subagents|mixed` and `--seed <n>` select the content. The same arguments give the same output.
- The `.cmd` file is the only entry point the core uses. The core is given its path (decision 2).
- It is used only by the regression tests. The C++ `hancho-dummy.exe` and `demo_smoke` are not changed (decision 1).

### 3.3 MCP client

The runner acts as the MCP host, the same as Claude Code or opencode:

1. Create a temp working directory and a temp data directory.
2. Run `hancho-cli.exe mcp install --data-dir <tmp-data>` in the temp working directory (`--client claude`, and `--client opencode` for the second pass). This writes `.mcp.json` or `opencode.json` there.
3. Read the generated config. Start the command it names, with its arguments, over stdin/stdout.
4. Send `initialize`, `tools/list`, then `tools/call` for each step.

Because the runner uses the generated config, a change in how `mcp install` writes the entry is also a regression, and it is checked.

## 4. Demo mode in the release build

The release build must be the code under test (decision 5). The demo behavior is therefore compiled in and switched on by a runtime option (decision 6). Two things follow:

- **Release package.** The demo option would be part of the shipped binary, but it does nothing unless set. This changes the current rule that demo code is never shipped (`doc/demo.md`, `build_demo.cmd`). **This needs your confirmation before implementation.** The alternative is a separate release-only flag that makes the regression build differ from the release by one compile option; that contradicts decision 6.
- **Options.** The demo behavior is switched on by `--demo`, `--demo-now`, or `HANCHO_DEMO=1`, as now. Those options also pass the path to the Python dummy (decision 2), `--dummy-llm <path>`.

The C++ core gets two options in place of compile-time branches:

| Option | Effect |
| :--- | :--- |
| `--demo` / `--demo-now <time>` / `HANCHO_DEMO=1` | Pin the clock and create demo data. Same as today. |
| `--dummy-llm <path>` | Use this command for dummy jobs instead of `hancho-dummy.exe`. |

`build_demo.cmd` and `build-demo\` remain for `demo_smoke` (which tests the C++ dummy). The regression run does not use them.

## 5. Core lifecycle in a run

1. Create a fresh data directory `<tmp>\regress-<scenario>-<pid>\`.
2. Copy the scenario's `demo.json` and seed files into it.
3. Start the core from `--bin-dir`: `Hancho.exe --headless --data-dir <dir> --demo --dummy-llm <path to dummy_llm.cmd>`, with `HANCHO_AUTOSTART=off` set for the clients.
4. Wait until the pipe answers a status request. The exact action is chosen in P0. A connection failure is a runner error, not a regression.
5. Run the steps for each layer: L1 (raw IPC), L2 (`hancho-cli.exe --json`), L3 (MCP as in section 3.3).
6. Stop the core with `Hancho.exe quit --data-dir <dir>` and wait for exit.
7. Compare goldens and delete the data directory unless `--keep` is given.

Isolation: on Windows the pipe name is derived from the data directory (`src/common.cpp` `GetPipeNameWide`), so each scenario has its own pipe and scenarios can run in parallel.

## 6. Determinism

| Source | Control |
| :--- | :--- |
| Clock | `--demo` pins it (section 4). |
| Data directory | Fresh per scenario, `--data-dir`. |
| Dummy jobs | Python dummy with fixed `--scenario` and `--seed`. |
| Shell jobs | Only fixed commands, for example `cmd /c echo <fixed>`. |
| Waiting | `wait` actions with a timeout; final state is recorded, not timing. |
| PIDs | Normalized to `<pid>`. |
| Job and task IDs | Normalized to ordinal placeholders in order of first appearance. |
| Paths | Data directory `<data>`, temp directory `<tmp>`, working directory of the MCP host `<cwd>`. |
| Events | `hancho.event/1` events normalized by timestamp and sequence, checked against `doc/schema/`. |
| List order | Sorted by a declared key, unless the contract says order matters. |
| MCP `tools/list` | Compared as a whole, without normalization. |

## 7. Coverage

Three inventories are generated from the source:

| Layer | Source | Size (approx.) |
| :--- | :--- | :--- |
| L1 core | Action names dispatched in `src/ipc_server.cpp` | Dozens |
| L2 cli | Command groups in `src/cli.cpp` / `src/cli_top.h` | About 20 groups |
| L3 mcp | `tools/list` from the running `Hancho.exe mcp` | 124 tools (confirmed in P0) |

Every item must have a scenario at its layer, or a reason in `not-covered.txt`. `run.py coverage` prints the gaps and CI fails on unexplained gaps.

## 8. Scenario and golden formats

Scenario (`scenarios/<area>/<id>.json`):

```json
{
  "id": "taskqueue.add-list-get",
  "covers": { "core": ["taskqueue_add", "taskqueue_list", "taskqueue_get"],
              "cli": ["taskqueue add", "taskqueue list", "taskqueue get"],
              "mcp": ["taskqueue_add", "taskqueue_list", "taskqueue_get"] },
  "setup": { "demo": true, "now": "2026-11-21 15:00:00", "seed": 1, "timeout_s": 60 },
  "operations": [
    { "op": "taskqueue.add", "args": { "queue": "demo", "name": "a" }, "expect": "ok" },
    { "op": "taskqueue.list", "args": { "queue": "demo" }, "expect": "ok", "keep": true },
    { "op": "taskqueue.get", "args": { "queue": "demo", "id": "$job[a]" }, "expect": "ok", "keep": true }
  ],
  "layers": ["core", "cli", "mcp"],
  "normalize": ["ids", "paths", "pid", "sorted-lists"],
  "serial": false
}
```

- `op` is abstract. `op-map.json` maps it to an L1 action, an L2 command, and an L3 tool name. A layer without the operation is written as `"not-applicable"`.
- `$job[a]` refers to a value captured from an earlier response.
- `expect` is `ok` or `error`. An expected error is recorded as a golden too.

Golden (`golden/<layer>/<id>/<step>.json`): UTF-8, JSON with sorted keys and 2-space indentation, LF line endings. Written only by `record`.

## 9. Timeouts, parallelism, and CI

Recommended values (decision 8):

| Item | Value | Reason |
| :--- | :--- | :--- |
| Per-step timeout | 60 s | Normal operations finish in well under a second. |
| Per-step timeout for `wait` operations | 120 s | Jobs with dummy LLM output may take a few seconds. |
| Per-scenario timeout | 5 min | Kills a hung scenario without stopping the run. |
| Core start timeout | 30 s | Core restores queues on start. |
| Parallel workers | 4 | Each scenario has its own core and pipe. |
| `serial: true` | Only when a scenario uses a fixed external resource (port, shared file). | |

On a timeout the runner kills the core's process tree, records the step as a failure, and moves on. A failed step is never treated as a golden mismatch: it is reported as a runner failure.

CI: the same command runs on each push to `main` and `stable`. Python 3 is required on the CI agent. The job fails on a golden mismatch, on a runner failure, or on an unexplained coverage gap. The JSON report and the diffs are kept as artifacts.

## 10. Phases

1. **P0 Determinism audit.** Run each layer twice with the same inputs and diff. Choose the core's readiness action. Confirm the MCP tool count. No product change except the two options in section 4.
2. **P1 Harness and one vertical slice.** `run.py` with core lifecycle, L1 pipe client, L2 CLI runner, L3 MCP client, `dummy_llm.py` + `.cmd`, normalizers `ids` / `paths` / `pid` / `sorted-lists`. Scenario `taskqueue.add-list-get` on all three layers.
3. **P2 Task queue.** All `taskqueue_*` operations on all layers, including `link`, `unlink`, `graph`, `retry`, `supersede`, `pause`, `resume`, `cancel`, `purge`.
4. **P3 Behavior tree and direction.** All `bt_*` operations and the node types in [bt-spec.md](bt-spec.md), and `direction_*`.
5. **P4 Task, routine, triage, translation.**
6. **P5 Memory, usage, LLM stream** via the Python dummy.
7. **P6 Coverage gate** in CI.

Each phase ends with: scenarios of the phase pass on `build\Release`, goldens reviewed, coverage updated.

## 11. Risks

- **Release package contains demo code** (section 4). Needs confirmation.
- **Pipe or process leaks** after a crash. The runner kills the process tree and checks that the pipe is gone before the next scenario.
- **Golden churn.** Re-record only with `record`, and review the golden diff in the PR.
- **Python dummy and C++ dummy drift.** They are separate by decision 1. Their outputs are not compared to each other.
- **Windows only** in P0–P2. The POSIX client and core are not tested yet.

## 12. Open items

1. Done: the demo option may be compiled into the release binary (inert unless set), approved.
2. Confirm the first slice's operations beyond `taskqueue.add-list-get`. Suggested: `taskqueue.retry`, `bt.validate`, `direction.add-show`.

## 13. P0 audit result (2026-11-28)

Build: `build\Release` (`Hancho.exe`, `hancho-cli.exe` 0.10.3). The demo code is compiled into both, `--dummy-llm` is added, and the `dummy` engine is registered only in demo mode.

Determinism checks, two runs each, fresh data directories, `--demo` with `now` = `2026-11-21 15:00:00`:

| Layer | Operations | Result |
| :--- | :--- | :--- |
| L2 cli | `demo prepare`, `demo status`, `taskqueue list`, `taskqueue get demo-finalize`, `usage stats --since 24h`, `usage stats --since 30d` | All six outputs byte-identical |
| L3 mcp | `initialize`, `tools/list`, `tools/call taskqueue_list`, `tools/call usage_stats` over stdio | stdout byte-identical after replacing the data directory path |
| L1 core | Raw IPC | Not audited yet. The pipe framing and the data-directory tag are checked in P1. |

Findings:

1. `tools/list` returns 124 tools. The earlier estimate of 87 was wrong.
2. Job commands contain the absolute path of `hancho-dummy.exe` (`D:\wsgit\Hancho\build\Release\hancho-dummy.exe`). The normalizer must replace it with `<bin>`. The file is not built by `cmake --build build --target Hancho hancho-cli`, so the Python dummy from P1 is needed for the regression runs.
3. Without `--data-dir`, `hancho-cli` warns `multiple Hancho cores are running; pass --data-dir explicitly` and falls back to the project's `.hancho`. The runner must pass `--data-dir` on every call.
4. `hancho-cli` has no `quit` subcommand. Use `Hancho.exe quit --data-dir <dir>`.
5. Timestamps in these outputs come from the pinned clock, so no timing normalizer was needed for these operations.

## 14. P1 result (2026-11-28)

Implemented in `tests/regression/`:

| File | Role |
| :--- | :--- |
| `run.py` / `run.cmd` | Runner (Python 3, standard library only). Commands: `run`, `check <scenario>`, `record`. |
| `dummy_llm.py` / `dummy_llm.cmd` | Python dummy LLM (stream-json style, deterministic for the same `--scenario` and `--seed`). |
| `op-map.json` | Maps each abstract operation to the L1 action, the L2 argv, and the L3 MCP tool. |
| `scenarios/taskqueue/add-list-get.json` | First scenario. Layers: core, cli, mcp-claude, mcp-opencode. |
| `golden/<layer>/<scenario>/<step>.json` | Recorded observations (10 files). |

How the layers run:

- Every layer starts its own headless core with `--headless --demo --demo-now <now> --dummy-llm tests/regression/dummy_llm.cmd` and stops it with `Hancho.exe quit`. The core is found through its active-core record (`dataDir`).
- L1 talks to the core's named pipe directly (ctypes). The request is plain JSON and the reply is plain JSON. One message per connection.
- L2 runs `hancho-cli.exe --json --data-dir <data> ...`.
- L3 writes the MCP entry with `hancho-cli.exe mcp install --client claude` (or `opencode`) in a temp directory, starts the command it names, and keeps the stdio session open for the whole scenario. The golden of `tools/list` is stored as step `00-tools-list`.
- `taskqueue.wait` is not recorded. The runner calls `taskqueue_wait` repeatedly, passing the version from the previous reply, until every job is in a terminal state. This was needed because `taskqueue_wait` returns when the queue version changes, not when a job finishes.

Results:

- `run` passed three times in a row on `build\Release`, with `failures=0 errors=0`.
- A deliberate change (`--seed 1` to `--seed 2` in the scenario) was reported as a diff in the `command` field, then reverted.

Findings during P1:

1. `rev` (revision counter) differs between runs. It counts every change in the core, including the background demo queue. It is dropped with the scenario's `drop_keys`. This is the only volatile field found in these outputs.
2. `Hancho.exe --headless` starts the real core as a detached process and returns at once. The runner therefore identifies the core by its active-core record, not by the process it started.
3. Demo queue jobs run in the background once the core starts. With the dummy missing (the default `hancho-dummy.exe` was not built into `build\Release`), a job failed and the triage LLM was started (`agy.exe`). The runner passes `--dummy-llm`, so demo jobs succeed. **Triage must also be disabled or pointed at the dummy in regression runs.** This is not done yet.
4. The first `taskqueue wait` on a fresh queue returns at once. Waiting for a terminal state must loop on `wait`, not call it once.

Not done in P1:

- Cross-layer comparison (checking that L1, L2 and L3 agree on the same operation). Each layer is compared only with its own golden.
- `coverage` command and the inventory (P6).
- Triage disable (finding 3).
- Clean-up of the core if the runner fails during startup (the detached core keeps running).

## 15. Triage slice (2026-11-28)

Scenario `scenarios/triage/failed-job-advice.json`: a job fails, the triage card gets an advice run that uses the Python dummy, and the card and its advice are read back. Layers: core, cli, mcp-claude, mcp-opencode.

Configuration in the scenario (`setup.files`):

- `config.json`: `triage_mode: "manual"`, `triage_llm_mode: "always"`, `triage_llm_engine: "triage-dummy"`.
- `engines.json`: engine `triage-dummy` runs `{dummy} --scenario triage {prompt}`.

Why these settings:

- `triage_llm_engine` is pinned to the dummy, so a regression run never starts a real agent for triage.
- `triage_mode: "manual"`: with `semi`, the safe candidate `retry` was applied automatically. That started a second attempt of the job and changed the card, so the observation depended on automatic decisions. `manual` keeps the card pending until the advice is read.
- The card is a **direction** card (`id` = direction id, `kind` = `direction_failure`), not a job card. The runner selects it with `capture` `{"list": "cards", "where": {"target.failedJobIds": "{id}"}, "field": "id"}`.

Changes made in this slice:

1. `--dummy-llm` is parsed by `Hancho.exe` only. The core is a child process and did not get it, so demo jobs fell back to `hancho-dummy.exe` (missing), failed, and created triage cards. Fix: `GetDummyLlmExecutableUtf8` (src/common.cpp) also reads the environment variable `HANCHO_DUMMY_LLM`. The runner sets it in `base_env`. After the fix the demo jobs succeed with the Python dummy.
2. `op-map.json`: `taskqueue.add` sends `triage` from the scenario variable `triage` (core, MCP, and the CLI jobs file). `add-list-get` sets `triage: false`; the triage scenario sets `triage: true`.
3. `run.py`: `wait_until_advice` waits only for the cards of the scenario's own job. Captures can select a card by fields (`capture_matching`, `card_matches`). A list value matches when it contains the wanted id.
4. `dummy_llm.py`: the card is the first JSON block of the instructions file that has `candidates` (or `cards`). The last JSON block is the output schema, which has no candidates; the earlier version picked that block and answered with an empty id.
5. Volatile text in observations is normalized (`VOLATILE_TEXT`): the per-run temp directory name, `PID : n`, `#n [`, and durations/uptime. `process_id` is dropped with `drop_keys`, because the process numbering of background demo jobs depends on timing.

Results:

- `record --filter triage` and `record --filter taskqueue` were run after the changes. `run` passed three times in a row, `failures=0 errors=0`, on `build\Release` (rebuilt with `Hancho`, `HanchoCore`, `hancho-cli`).
- Golden for the card: advice state `done`, candidate `retry`, summary `Demo advice for : choose retry` (the card id is not in the instructions block, so the summary has no id; cosmetic, deterministic).

Open items from this slice:

- Triage is pinned to the dummy only in the triage scenario. Other scenarios still use the default triage settings. Demo jobs now succeed, so no card is created there, but a failing job in another scenario would start the default triage agent. Disable triage (or pin the engine) for all regression runs before adding more failing-job scenarios.
- The advice summary has no card id because the instructions block does not carry the id at the top level. Fine for now.
- Not yet covered: decide/ack/ignore, triage_mode `semi`/`auto` behaviour, and the auto-retry path.
