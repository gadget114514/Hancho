# Routines — named, repeatable BT workflows

A **routine** is a behavior-tree definition saved under a name so the same plan can be re-run without writing the BT again. A routine can be authored by hand or proposed by a worker LLM (`routine ask`); either way, the approved tree is made only of jobs and `foreach` nodes, and every run gets a fresh id so job ids never collide. LLM-authored definitions are always **proposals** and never run until a person approves them.

Routines are useful for fixed repo chores such as "build all targets and run every `*_smoke` test", "regenerate derived files", or "run the release checks". See the checked-in example at `.hancho/routines/smoke-all.json`.

## Where definitions live

| Scope | Definition path | Notes |
| :--- | :--- | :--- |
| `project` | `<cwd>/.hancho/routines/<name>.json` | Versioned with the repository. Add `.hancho/routines/` to version control even when the rest of `.hancho/` is ignored. A routine saved this way carries the project root as its `cwd`. |
| `data` | `<data-dir>/routines/<name>.json` | Machine-local, useful for personal workflows. |

If both scopes contain the same name, **project wins**. Run records are always kept in `<data-dir>/routines/<name>.runs.json` (last 50 runs), including the run id, state, input diff and per-node results.

The definition file contains `name`, `summary`, `cwd`, `bt` (the normal BT JSON), `inputs` (globs for freshness checking), `inputsSnapshot` (path → SHA-256 at save time), `foreachSnapshot`, `notes` and `history` (`{at, by, reason}` entries). `bt` is validated with exactly the same validator as `bt validate`.

## CLI

```powershell
hancho-cli.exe routine list
hancho-cli.exe routine show smoke-all
hancho-cli.exe routine check smoke-all
hancho-cli.exe routine run smoke-all --wait --timeout-ms 3600000
hancho-cli.exe routine save --name smoke-all --file plan.json --inputs CMakeLists.txt --inputs "tests/*.cpp" --summary "Build and smoke" --project --reason "initial"
```

| Command | What it does |
| :--- | :--- |
| `list` | Names, summaries, scope, last run and freshness state (`[STALE]` plus a diff count). |
| `show <name>` | Full definition, the last run's per-node results (state, duration, test report totals), `newlyFailed` / `fixed` versus the previous run, and the input diff. |
| `run <name> [--wait] [--timeout-ms <n>]` | Starts the saved BT with a fresh run id and task queue. `--wait` blocks and exits non-zero on failure. |
| `save --name ... --file ... [--inputs <glob> ...] [--summary ...] [--project\|--data] --reason ...` | Registers or updates a routine, validates its BT, refreshes both snapshots and appends a history entry. Always give a `--reason`. |
| `check <name>` | Compares the current input files with the saved snapshot and reports `state: ok` or `stale` plus `added` / `removed` / `changed` / `absorbed`. |
| `ask "<what you want>" [--name <n>] [--cwd <dir>] [--wait] [--scope ...]` | Asks a worker LLM to create (or, when `--name` exists, fix) a routine. The definition is validated and saved as a proposal. |
| `approve <name> --user-said "<your words>"` | Approves a proposal (`approvedVia: chat`). An empty `--user-said` is a rejection. |
| `reject <name> --reason "<why>"` | Discards a proposal and records the reason in history. |
| `save --name <n> --from-bt <btId> [--project\|--data]` | Registers the definition of a previously saved BT (`<data-dir>/behaviortrees/<btId>.json`) as an approved routine. |

Use `--json` for scripting. `routine --help` prints all options.

## `routine ask`: LLM-authored proposals

`routine ask` runs as a worker job in the `routine-ask` task queue, so it appears on the dashboard and ordinary failure diagnosis/triage applies. The selected worker comes from the approved, enabled `worker` LLM list in rank order ( unhealthy engines are skipped, 091-6 ); if that list is empty the default engine is used and the result carries a warning.

The prompt contains the request, `cwd`, the routine format, the `bt template test-suite` example, the list of existing routines and, when fixing, the current definition, the `routine check` diff, the last run's node table and the failed node's log tail. The worker writes `{name, summary, bt, inputs, notes, rationale}` to `PC_BT_RESULT_FILE`; `rationale` explains the proposal to the approver. Hancho then validates the result itself:

- strict `bt validate` (`bt validate --strict` equivalent),
- a **dry run** that expands every `foreach` glob into the concrete job list (`dryRun.count`, commands),
- ChainGuard (no chained commands) and a rejection of `worker` / `escalate` nodes.

If validation fails, the worker gets the errors and is asked to fix the definition **once**. If it is still invalid, the job fails and the reason is returned.

A valid definition is stored as `<dir>/.proposals/<name>.json` with `status: proposed`. For an update, the approved `<name>.json` is left untouched, so `routine run` keeps using the old version while a revision is pending. `routine show` displays the proposal, the dry-run jobs and the `rationale`. `routine run` refuses a routine that has only a proposal. Approval via the dashboard (`approvedVia: dashboard`) or `routine approve --user-said` (`approvedVia: chat`) promotes the proposal to the approved definition; `routine reject` discards it. Every history entry records `by` (client, engine/model) and `reason`.

A person using `routine save` (CLI or dashboard) still gets an approved routine; `routine_save` from MCP/LLM is stored as a proposal instead.

## MCP

The same operations are exposed as MCP tools: `routine_list`, `routine_show`, `routine_run`, `routine_save`, `routine_check`, `routine_ask`, `routine_approve` and `routine_reject`. This lets a supervisor agent run a named workflow, ask for a new routine, and approve/reject a proposal without shelling out.

## Run ids and jobs

Each `routine run` generates `<name>-<yyyymmdd-hhmmss>` (with a counter suffix if that id already exists) and uses the task queue `routine-<name>`. The id is written into the BT as its `id`, so every job created by the tree is prefixed with it. `routine show` and the dashboard use the id to group results; the last 50 records are retained even when the dashboard is restarted.

## `stale` and `absorbed`

`inputs` globs are expanded and hashed when the routine is saved. On `check` / `list` / `run` / `show` the current files are compared with that snapshot:

- `added` / `removed` / `changed` mean the routine is **stale** — something it depends on has changed.
- Files added or removed under a `foreach` node's `glob` (respecting `exclude`) are reported as **absorbed** instead: the tree expands them at run time, so they do not make the routine stale. For example, adding another `tests/*_smoke.cpp` only grows the `smokes` branch.

A stale routine still runs. If it succeeds, the snapshots are refreshed and a `routine-run` history entry is appended. If it fails while stale, triage creates a persistent `routine_stale_failure` card whose recommended action is **`update_routine`**. Choosing it launches `routine ask` for that routine (with the failing nodes and log tail in the prompt); the result is a proposal awaiting approval, recorded on the card as `routineAsk` / `proposalPending`. You can still fix it by hand with `routine save --reason "..."`. A non-stale failure is handled by the ordinary job-failure cards.

## Dashboard

The Web UI has a **Routines** tab: it lists routines with freshness and last-run state, starts one with `Run` (via the `routine_run` IPC action), and shows the last run's per-node results, test-report totals, `newlyFailed` / `fixed` and the stale diff. The **LLM に頼む** field asks for a new routine; each row has an **LLM に直させる** button to fix it. A pending proposal is marked `[提案中]` and shows its request, rationale, dry-run job list and Approve/Reject buttons in the detail pane. The native Win32 dashboard does not have a separate routine page; it opens the Web UI for this.

## Related documents

- [Behavior Trees](behavior-tree.md) — BT JSON format, `foreach`, `${foreach:...}`, variables and node types.
- [Task Queue](taskqueue.md) — how jobs launched by a routine are queued and retried.
- [Triage](triage.md) — failure cards and decisions, including `update_routine`.
