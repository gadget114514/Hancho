# Plan: regression tests for every requirement and feature

Status: draft (design only, nothing implemented yet).

## 1. Goal

Detect unintended behavior changes ("degradation") in Hancho by running every user-visible feature under identical conditions and comparing the result with a recorded baseline.

- Same fixed clock, same fixed data, same inputs → same normalized output.
- Any difference is reported as a regression unless it is explicitly accepted by re-recording the baseline in a reviewed commit.

Non-goals:

- Replacing the existing `*_smoke` unit tests. They check internal logic in isolation; this plan adds a layer above them that checks observable behavior end to end.
- Pixel-level comparison of the GUI (see section 7).
- Testing third-party code under `thirdparty/` and `src/libsixel-windows/`.

## 2. Definitions

| Term | Meaning here |
| :--- | :--- |
| Scenario | One fixed run: a data setup, an ordered list of steps (CLI commands, MCP/IPC calls), and the outputs to keep. |
| Step | One invocation of a feature, for example `hancho-cli taskqueue list`. |
| Observation | What a step produces: stdout, stderr, exit code, and any files it writes under the data directory. |
| Normalizer | Rules that remove values that legitimately differ between runs (paths, PIDs, durations) before comparison. |
| Golden (baseline) | The normalized observations recorded from a known-good build and stored in the repository. |
| Requirement | A statement in the existing docs (`doc/*.md`, `README.md`, `skills/hancho/SKILL.md`, `doc/schema/`) or a feature listed in the CLI/MCP registry. Each one must map to at least one scenario. |

## 3. Determinism requirements

A regression test is only meaningful if the same build gives the same output twice. The following sources of variation must be controlled.

| Source | Control |
| :--- | :--- |
| Current time | Pin the clock. `demo.cpp` already does this (`SetDemoMode`, `demo.json` `now`). It is currently compiled only with `HANCHO_BUILD_DEMO=ON`, so the regression build must enable the same mechanism (see section 5). |
| Data directory | Always a fresh temporary directory set with `--data-dir`, never the user's real data directory. |
| Random IDs, UUIDs | Seeded generator for test builds, or normalize to placeholders (`<id:1>`, `<id:2>`, ... in order of first appearance). |
| Process IDs, temp paths | Normalize to `<pid>` and `<data>`. |
| Durations, `elapsed`, timing fields | Pinned clock makes them fixed; otherwise normalize to `<duration>`. |
| Thread scheduling, output order | Sort lists by a stable key in the normalizer, or make the feature sort its output. Each such choice is recorded per scenario. |
| LLM engines | Use `hancho-dummy` with fixed `--scenario` and `--seed`. Never call a real model. |
| Network (chat peers, remote WebSocket) | Local loopback only, with fixed ports chosen by the runner, or the scenario is marked `skip-network`. |
| Environment (locale, PATH, console) | The runner sets them explicitly (for example `LANG`, codepage) and records them in the scenario header. |

Each scenario declares which normalizers it uses. A scenario that cannot be made deterministic is not added until it can.

## 4. Requirement inventory and coverage

The inventory is generated, not hand-written, so that new features cannot be added without coverage.

Sources of truth:

1. **CLI command groups** registered in `src/cli.cpp`: `taskqueue`, `bt`, `direction`, `routine`, `triage`, `task`, `translate`, `llm`, `permit`, `power`, `chat`, `memory`, `usage`, `dashboard`, `worker`, `demo`, and others.
2. **MCP tools** registered in `src/cli.cpp` (the tool list around `"task_create"` … `"agent_health"`). This list currently has about 87 entries.
3. **IPC commands** handled in `src/ipc_server.cpp`.
4. **Behavior tree node types** in `doc/bt-spec.md`.
5. **Event schema** `hancho.event/1` in `doc/schema/`.
6. **Documented behavior** in `doc/*.md` (non-cards documents) and `README.md`.

A generator script lists the inventory and the scenarios that reference each item. CI fails if an item has no scenario, unless it is in an explicit `not-covered.txt` with a reason.

Traceability: every scenario header lists the requirement IDs it covers, for example `cli.taskqueue.add`, `mcp.task_create`, `bt.node.retry`, `doc.task-lineage-plan.sec3`.

## 5. Build and runner

### 5.1 Build

- Reuse the demo mechanism, but make it a separate, explicit configuration: `HANCHO_BUILD_REGRESSION=ON` in its own build directory (`build-regress\`), which also turns on `HANCHO_DEMO_BUILD`.
- Like `build-demo\`, the regression build is never packaged. The release steps in `CLAUDE.md` continue to use only `build\`.
- Open question: whether `build\` (the normal build) should always include the runner and dummy LLM but keep `--demo` disabled. This would make the regression run a single command in the daily workflow. Decide in section 10.

### 5.2 Runner

A new target `regress` (C++, same toolchain as the rest of the repo, no new dependency):

```
build-regress\Release\regress.exe run  [--filter <glob>] [--report <dir>]
build-regress\Release\regress.exe check <scenario>            # run one, show diff
build-regress\Release\regress.exe record <scenario> [--all]   # overwrite golden (explicit)
build-regress\Release\regress.exe list-uncovered              # coverage gap report
```

Responsibilities:

1. Create a fresh temporary data directory per scenario and copy the scenario's `demo.json` and seed files into it.
2. Start the target executable with the scenario's arguments, with a timeout per step. Capture stdout, stderr, exit code.
3. Collect the files the scenario asks to keep (for example `taskqueues/demo.json`, `usage/daily/*.jsonl`).
4. Apply the normalizers.
5. Compare with the golden files. Print a unified diff on mismatch. Write a machine-readable JSON report for CI.
6. Never write to golden files in `run` or `check`. Only `record` writes them.

### 5.3 Scenario format

Files live in `tests/regression/scenarios/<area>/<id>.json`:

```json
{
  "id": "taskqueue.add-and-list",
  "covers": ["cli.taskqueue.add", "cli.taskqueue.list"],
  "setup": { "demo": true, "seed": 1 },
  "steps": [
    { "run": ["taskqueue", "add", "--queue", "demo", "--name", "a"], "expect_exit": 0 },
    { "run": ["taskqueue", "list", "--queue", "demo", "--json"], "keep": "stdout" }
  ],
  "normalize": ["ids", "paths", "pid", "sorted-lists"]
}
```

Golden output for scenario `X` is stored in `tests/regression/golden/<area>/<id>/`, one file per kept observation.

### 5.4 Normalizers

Named, reviewed, and tested themselves:

- `ids`: replace generated IDs with ordinal placeholders.
- `paths`: replace the data directory and temp directory with `<data>` / `<tmp>`, and normalize `\` to `/`.
- `pid`: replace process IDs.
- `sorted-lists`: sort JSON arrays whose order is not part of the contract, by a declared key.
- `floats`: round floats to a declared number of digits.
- `durations`: replace elapsed-time fields with `<duration>` when the clock is not pinned.

Normalizers never delete a whole observation. If a value is removed, the placeholder stays, so a field that disappears still shows up as a diff.

## 6. Feature areas and first scenarios

Each area gets scenarios for the normal path, the main error path, and one edge case. Examples:

| Area | Examples of covered behavior |
| :--- | :--- |
| Task queue | add / list / get / update / retry / cancel / pause / resume, dependencies (`link`, `unlink`, `graph`), purge |
| Behavior tree | validate, start, get, list, pause, resume, retry, cancel, template, wait; each node type in `doc/bt-spec.md` |
| Direction | add / list / show / edit / run / close / reopen / import / export / translate |
| Task | create / show / list / note / close / reopen / purge / wait |
| Routine | list / show / run / check / ask / approve / reject / save |
| Triage | list / get / decide / handoff / manual / ack / advise |
| Usage | stats for 1h / 24h / 30d, export (uses the pinned demo data) |
| LLM stream | parse fixed stream-json from `hancho-dummy` into usage and subagent state |
| Memory | search / get / brief / stats / note / retract / approve |
| Chat | status / peers / send / inbox / read / log (loopback only) |
| Translation | status / get / clear |
| Power | status (suspend, shutdown, cancel are not run; they are checked for validation output only) |
| Event schema | every event type emitted by a fixed scenario validates against `doc/schema/` |
| Dashboard | `dashboard_snapshot` JSON (see section 7) |

## 7. GUI

Pixel comparison is not planned: fonts, DPI, and window placement make it flaky on Windows. Instead, the GUI is tested through the model it renders:

- Each GUI view has a pure data model (for example the task-list rows and the labels in `task_lineage_view.h`). The regression scenario records that model as JSON.
- Where a view has no separate model, extracting one is a prerequisite task, not part of the regression scenario.
- Screenshots for the README stay manual (`doc/images/`).

## 8. CI and workflow

- Local: `regress.exe run` is part of the pre-release checklist, before `build\` is packaged.
- CI: a job runs `regress run --report` on each push to `main` and `stable`. A failure blocks merge. Report artifact: the JSON report plus diffs.
- Baseline changes: a PR that changes golden files must say so in its description, and the golden diff is reviewed like code.

## 9. Phases

1. **P0 Audit (no code change):** list determinism gaps by running each CLI group twice with the same inputs and diffing the output. Output: a table of nondeterministic fields.
2. **P1 Runner skeleton:** `regress` target, scenario loader, data-dir isolation, normalizers `ids`, `paths`, `pid`, `sorted-lists`. Pilot scenarios: `usage` (1h, 24h, 30d) and `taskqueue` (add, list, graph). Golden files recorded and checked in.
3. **P2 CLI coverage:** scenarios for every CLI command group in section 6.
4. **P3 MCP and IPC coverage:** run the same features through the MCP server (or IPC where MCP is not reachable from the runner). Same scenario format, `covers` lists `mcp.*` IDs.
5. **P4 Behavior tree, routine, triage, event schema.** Includes the BT node types in `doc/bt-spec.md`.
6. **P5 GUI models and CI gate.** Add the uncovered-item check to CI as a failing check.

Each phase ends with: all scenarios pass on the current build, golden files reviewed, and the coverage report shows the phase's items as covered.

## 10. Decisions needed

1. Should the normal `build\` include the runner and `hancho-dummy`, with only `--demo` disabled? (Recommended: yes for the runner and dummy, because they are test-only and never shipped in the zip if excluded from packaging. Needs a change to the packaging step in `CLAUDE.md`.)
2. Language of the runner: C++ (recommended, same toolchain, no new dependency) or a PowerShell script (faster to write, weaker on Windows path and process handling).
3. Where the golden files live: `tests/regression/golden/` in the repository (recommended, reviewable in diffs), or a separate branch.
4. Which MCP invocation path the runner uses: stdio MCP server process, or `hancho-cli` equivalents where they exist.

## 11. Risks

- **Golden churn:** every intended change needs a re-record. Mitigation: record only with an explicit command, and show the golden diff in the PR.
- **Hidden nondeterminism:** a field nobody thought of changes between runs. Mitigation: P0 audit, and run each scenario twice in CI before comparing with golden.
- **Size:** `taskqueue_smoke.cpp` alone is about 361 KB. Mitigation: scenarios are data files, not C++ code; the runner is generic.
- **Platform:** the current demo build is Windows-only (`build_demo.cmd`). POSIX support is out of scope for the first phases.
- **Slow runs:** target under 5 minutes locally. Mitigation: `--filter` and a fast subset for daily use.

## 12. Related documents

- [demo.md](demo.md): the fixed-time and fixed-data mechanism that this plan depends on.
- [terminology.md](terminology.md): the names to use in scenarios and reports.
- [bt-spec.md](bt-spec.md): the behavior tree node types to cover.
