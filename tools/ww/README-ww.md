# ww — task-card runner

> **What this is:** a tool that an **LLM agent copies into a project, adapts to that project,
> and then uses inside it**. The copy in this repository (`Hancho/tools/ww`) is the
> template. It is not a product to install, and it is not meant to be used unchanged from here.
> Each project owns its own copy and its own changes. See
> [How it is meant to be used](#how-it-is-meant-to-be-used-copy-adapt-use).

`tools/ww` runs task cards (`ww-data\tasks\<id>-*.md`) with an LLM coding agent and keeps an
eye on them:

- `ww-run` — run one card with an agent, record the run, show progress.
- `ww-watch` — wrap `ww-run` with **stall detection** and the **split-first retry policy**: it
  watches for progress (worker/descendant CPU, `git status` of the watch folders, reports and run log) and
  reacts when the run stops making progress for `-StallMinutes`.
- `ww-status` — what is running, what changed, what is waiting for you.

## Agents (engines)

| `-Engine` | Command ww-run starts | `-Model` |
|---|---|---|
| `agy` (default) | `agy.exe -p <prompt> --output-format stream-json --dangerously-skip-permissions` | default `gemini-3.8-flash-medium` |
| `opencode` | `opencode.cmd run --format json <prompt>` | passed as `--model` when given |
| `claude` | `claude -p <prompt> --output-format stream-json --verbose --dangerously-skip-permissions` | passed as `--model` when given |
| `codex` | `codex exec --json --dangerously-bypass-approvals-and-sandbox <prompt>` | passed as `--model` when given |

Set the engine with `-Engine` or `WW_ENGINE`, the model with `-Model` or `WW_MODEL`. The agent
must be installed and on `PATH`. `-DryRun` prints the exact command.

`claude.exe` and `codex.exe` are also what interactive sessions run as, so ww only counts a
claude/codex process as a ww run when its command line carries a ww prompt
(`Execute ...tasks/<id>-...`). An open Claude Code or Codex session never blocks a run.

## How it is meant to be used (copy, adapt, use)

ww is written to be **taken over by an LLM agent**. The agent copies it into the project,
changes it to fit, and from then on runs it there as the project's own internal tool. The
scripts are plain PowerShell with self tests, so an agent can read, change and check them safely.

Steps for the agent (or a person) setting up a project:

1. **Copy** `Hancho/tools/ww` (from the repository, or from
   `<install dir>\tools\ww` after running the installer, e.g.
   `%LOCALAPPDATA%\Programs\Hancho\tools\ww`) into the project as `tools/ww` and commit it there. The
   project's copy is what gets run; later changes in this repository do not reach it.
2. **Create the data folder** `<project>\ww-data\` with `tasks\` for the cards, and optionally
   `ww-data\config.json`, e.g. `{ "watch": ["src", "tests"] }` (see
   [Test hooks](#test-hooks)). Gitignore `ww-data\logs\`, `ww-data\running\` and
   `ww-data\stall-state.json`.
3. **Adapt** what the project needs. Typical changes:
   - the prompt ww-run sends (`$prompt` in `ww-run.ps1`), e.g. "Read AGENTS.md first", or the
     worktree instructions about skipping compile/tests;
   - the default engine and model (`-Engine` / `-Model` defaults, `Get-EngineCommand`);
   - progress signals, e.g. CPU of an editor that compiles on its own (see
     [adding a process CPU signal](#progress-signals-and-adding-a-process-cpu-signal));
   - card conventions (`Authorised edits`, `Estimate N min`, `Splittable: no`) if the project
     writes cards differently.
4. **Check** the copy: run `tools\ww\ww-run.selftest.ps1`, `tools\ww\ww-watch.selftest.ps1`
   and `tools\ww\tests\ww-watch.tests.ps1`, and fix or update the tests for the changes made.
5. **Use** it from the project root: `tools\ww\ww-run.cmd 012 -Engine claude`,
   `tools\ww\ww-watch.cmd 012`, `tools\ww\ww-status.cmd`.

The scripts find the project from the **current directory** (its git top level, then
`<root>\ww-data`). `-Root <project>` (all three scripts) or `WW_ROOT` overrides that, e.g. when
Hancho starts them from elsewhere.

## Relationship to Hancho

`tools/ww` is an **independent tool** whose template happens to live in this repository. It is
optional:

- It does not need Hancho. The scripts only need PowerShell, git and an agent
  (`agy`, `opencode`, `claude` or `codex`).
- Hancho does not need it. Nothing in `Hancho.exe` / `hancho-cli.exe` calls or
  expects these scripts; you can use Hancho without ever touching `tools/ww`.
- They can be combined if you want: start `ww-run` / `ww-watch` from Hancho like any
  other command, and pass `-JsonStdout` so the raw event stream goes to stdout. See
  [Using it from Hancho](#using-it-from-hancho).

## Using it from Hancho

Hancho starts `ww-run` (or `ww-watch`) like any other command. Four things matter:

1. **Point at the project.** Pass `-Root <project>` (or `--cwd <project>` to hancho-cli), so the run
   does not depend on where hancho-cli was started.
2. **Close stdin.** Use `--close-stdin` (MCP: `open_stdin: false`); the agent must not wait for input.
3. **Machine-readable output.** Pass `-JsonStdout` to `ww-run` so the agent's raw JSON events go
   to stdout, and `--json-stream --agent-type <engine>` to hancho-cli so it parses them.
4. **One timeout/retry owner.** Either let hancho-cli own the timeout (`--timeout`) and run `ww-run`,
   or run `ww-watch` (its own stall detection and retries) and give hancho-cli only a generous
   `--timeout` as a safety net. Do not add task-queue retries on top of `ww-watch`.

One card, from the command line:

```powershell
hancho-cli.exe start --name "ww-012" --cwd C:\path\to\project --close-stdin --timeout 7200 `
  --json-stream --agent-type claude `
  --cmd "C:\path\to\project\tools\ww\ww-run.cmd 012 -Engine claude -Root C:\path\to\project -JsonStdout"

hancho-cli.exe list                      # find the id
hancho-cli.exe logs --id <id> --tail 40  # live output
hancho-cli.exe kill --id <id>            # stops ww-run, PowerShell and the agent (whole tree)
```

Wait for it (blocking; for Claude Code's `run_in_background` the notification comes when hancho-cli exits):

```powershell
hancho-cli.exe start --name "ww-012" --cwd C:\path\to\project --close-stdin --timeout 7200 --wait --wait-ms 0 --json `
  --cmd "C:\path\to\project\tools\ww\ww-run.cmd 012 -Engine codex -Root C:\path\to\project"
```

Several cards in order (task queue), e.g. 013 only after 012 succeeded:

```json
[
  {"id": "ww-012", "summary": "card 012", "timeoutSec": 7200,
   "command": "C:\\path\\to\\project\\tools\\ww\\ww-run.cmd 012 -Engine claude -Root C:\\path\\to\\project"},
  {"id": "ww-013", "summary": "card 013", "timeoutSec": 7200, "dependsOn": ["ww-012"],
   "command": "C:\\path\\to\\project\\tools\\ww\\ww-run.cmd 013 -Engine claude -Root C:\\path\\to\\project"}
]
```

```powershell
hancho-cli.exe taskqueue add --file ww-jobs.json --taskqueue ww --concurrency 1
hancho-cli.exe taskqueue graph ww
```

Keep `--concurrency 1` for plain main-checkout runs: ww-run refuses a second main run anyway.
Cards run with `-Worktree` can run in parallel.

### Parallel runs on the main checkout (`-Shared`)

`-Shared` runs a card on the main checkout next to other runs, as long as the paths they declare
do not overlap (design: [doc/resource-locks.md](../../doc/resource-locks.md)):

- `## Authorised edits` paths are **written**, `## Read-only` (or `## Reads`) paths are **read**.
  Two runs that only read the same file can run together; an edit of a file another run edits or
  reads is refused. A plain main run, or a card without Authorised edits, locks the whole checkout.
- The agent is told it shares the checkout: edit only the Authorised edits, skip compile/tests
  (listed under "Verification pending"), do not commit. Run the build/tests afterwards on their own.
- After the run, edits outside the Authorised edits (from the event log, `ww-data` excluded) are
  written to the log and to `EditViolations` in `<id>.last.json`, with a warning.
- **Shared is the default** when the card has `## Authorised edits`; `-Main` forces an exclusive
  run (the agent may then build/test). A card without Authorised edits runs as main.
- A bare file name (`Foo.cs`, read as `**/Foo.cs`) overlaps directories above it and the same name,
  but not another file such as `src/b.cpp`.
- When a run is refused because it or the other run locks the whole checkout, ww-run prints a
  `HINT:` to use `-Shared` (with Authorised edits) or `-Worktree`, and appends the refusal to
  `ww-data\logs\refused.log`.
- A refused run exits with **75** (nothing ran, try again later), so a caller can tell it from a
  setup error (1) or an engine failure. `-DryRun` also exits 75 when the run would be refused.
  A hancho-cli task queue defers such a job by default: it starts again after 60 s without using up
  `maxAttempts` (job field `defer`).

`ww-run.cmd -Help` prints all options and the steps for a supervisor LLM.

#### Generating the hancho-cli jobs (`-Jobs`)

`-Jobs` writes the task-queue jobs for several cards (runs nothing): each card becomes `ww-<id>`
running `ww-run.cmd <id> -Shared ...` with its Authorised edits as `resources.write` and its
Read-only paths as `resources.read`. Cards without Authorised edits and cards that overlap each
other are reported on stderr.

```powershell
# print the JSON
tools\ww\ww-run.cmd -Jobs 012,013 -Engine claude
# write it, add a final build/test job, and add everything to hancho-cli queue "ww" (hancho-cli from HANCHO_CLI or PATH)
tools\ww\ww-run.cmd -Jobs 012,013,014 -Engine claude -Verify "C:\path\to\project\build.cmd" -Queue ww -Concurrency 3
hancho-cli.exe taskqueue wait ww
```

`-Out <file>` writes the file without adding it (default with `-Queue`: `ww-data\jobs\`),
`-TimeoutSec` sets every job's timeout (7200), `-Worktree` makes worktree jobs without resources.
The hand-written equivalent; hancho-cli never starts jobs whose `resources` overlap (on by default;
Settings checkbox):

```json
[
  {"id": "ww-012", "summary": "card 012", "timeoutSec": 7200, "cwd": "C:\\path\\to\\project",
   "resources": {"write": ["src/a.cpp"], "read": ["src/common.h"]},
   "command": "C:\\path\\to\\project\\tools\\ww\\ww-run.cmd 012 -Shared -Engine claude -Root C:\\path\\to\\project"},
  {"id": "ww-013", "summary": "card 013", "timeoutSec": 7200, "cwd": "C:\\path\\to\\project",
   "resources": {"write": ["src/b.cpp"], "read": ["src/common.h"]},
   "command": "C:\\path\\to\\project\\tools\\ww\\ww-run.cmd 013 -Shared -Engine claude -Root C:\\path\\to\\project"},
  {"id": "verify", "summary": "build + tests", "timeoutSec": 3600, "cwd": "C:\\path\\to\\project",
   "resources": {"write": ["**"]}, "dependsOn": [{"id": "ww-012", "on": "done"}, {"id": "ww-013", "on": "done"}],
   "command": "C:\\path\\to\\project\\build.cmd"}
]
```

```powershell
hancho-cli.exe taskqueue add --file ww-jobs.json --taskqueue ww --concurrency 3
hancho-cli.exe taskqueue graph ww    # resource_wait shows who holds what
```

From an LLM agent through MCP, call `start_process` with the same command, `cwd`,
`open_stdin: false` and a `timeout`, then `wait_process` / `get_process_output`.

`ww-status` keeps working next to this: it reads the run records in `ww-data`, not hancho-cli.

## Pros and cons

### Using `tools/ww` (instead of starting the agent by hand)

Pros:

- **One task card per run, no duplicates.** The same card never runs twice, and only one
  plain main-checkout run is allowed at a time.
- **Parallel runs on the main checkout.** `-Shared` runs cards together when their declared
  Authorised edits / Read-only paths do not overlap, and reports edits outside the declaration.
- **Parallel runs in worktrees.** `-Worktree` runs a card in `.claude\worktrees\ww-<id>` on its
  own branch, so it can run next to the main run.
- **Stall detection and retry.** `ww-watch` notices when a run stops making progress and retries
  or asks for the card to be split, instead of waiting forever.
- **Readable progress.** The JSON event stream becomes one line per step in `.progress.log`, and
  `ww-status` shows every active run, its estimate and what is waiting for you.
- **Run records.** Each run leaves a log, the exact prompt, the raw events and a `.last.json`
  with exit code and reports written; dead records are cleaned up automatically.
- **Safe preview.** `-DryRun` prints the plan without changing anything.

Cons:

- **Windows and PowerShell only.**
- **Fixed conventions.** Cards must be `ww-data\tasks\<id>-*.md`; the `Authorised edits` and
  `Estimate N min` sections are read by the scripts.
- **Four agents only** (`agy`, `opencode`, `claude`, `codex`; another needs a launch line and an
  event parser in `ww-run.lib.ps1`), and the agent runs without permission prompts or sandbox
  (`--dangerously-skip-permissions` / `--dangerously-bypass-approvals-and-sandbox`), so the card
  is the only guard.
- **Heuristic stall detection.** Progress is judged from CPU time, `git status` of the watch
  folders and report/log changes, so a long silent step can look like a stall (tune
  `-StallMinutes`, `-MinCpuDelta` and `watch` in `config.json`). Work done by a separate
  process such as an editor is not seen; see
  [adding a process CPU signal](#progress-signals-and-adding-a-process-cpu-signal).
- **Worktree runs skip compile/tests**; they are listed as "Verification pending" in the report
  and have to be done on the main checkout.
- **Extra files** in `ww-data` (logs, run records, state) that you may want to gitignore.

### Running it under Hancho (optional)

Pros:

- The run's output is visible in the Hancho dashboard and kept in its persistent logs.
- Stopping the job kills the whole process tree (ww, PowerShell, the engine) via the Job Object.
- The task queue can order several cards with dependencies, timeouts and retries, and LLM agents
  can start and watch runs through the MCP tools.

Cons:

- One more layer: retries and timeouts exist both in `ww-watch` and in the task queue. Use one
  of them, or the two will retry the same card.
- You need `-JsonStdout` for machine-readable output; without it you get the formatted lines.
- `ww-status` and the Hancho dashboard show the same run in two places.

## Usage

```
Tools\ww\ww-watch.cmd <card-id> [-Engine agy|opencode|claude|codex] [-Model <model>] [-Root <project>] [-StallMinutes 8] [-PollSeconds 30] [-MaxRetries 2] [-DryRun]
```

- `<card-id>` — card id such as `038-2`. If the id is an **index card** (has `NNN-n-*.md`
  children), `ww-watch` expands it depth-first and runs the leaves in name order.
- `-Engine` — agent: `agy` (default), `opencode`, `claude` or `codex` (see [Agents](#agents-engines)). Also `WW_ENGINE`.
- `-Model` — model for the agent; agy defaults to `gemini-3.8-flash-medium`, the others use their own default. Also `WW_MODEL`.
- `-Agent <name>` — Claude Code agent (`.claude\agents\<name>.md`) to run with `-Engine claude`; passed as `claude --agent <name>`. Other engines refuse it. Also `WW_AGENT`, which `ww-watch` passes on too. `-Jobs` writes it into each hancho-cli job. Example: `ww-run.cmd 012 -Engine claude -Agent pc-runner`.
- `-Root` — project root (default: git top level of the current directory). Also `WW_ROOT`.
- `-DryRun` — print the expansion tree and each leaf's splittable status; run nothing.

## Engines and Models

`ww-run` and `ww-watch` support two execution engines:

| Engine | Command / Invocation | Notes |
|---|---|---|
| **`agy`** (default) | `agy.exe -p "<prompt>" --output-format stream-json --model <model> --dangerously-skip-permissions --print-timeout 180m` | Non-interactive stream-json worker. Model defaults to `gemini-3.8-flash-medium`. `--dangerously-skip-permissions` bypasses confirmation prompts. |
| **`opencode`** | `opencode.cmd run --format json "<prompt>"` | Legacy worker. Copies `opencode.json` when running in worktrees. |

> [!NOTE]
> **`--print-timeout` note**: agy's `--print-timeout` defaults to 5 minutes, which prematurely aborts long operations (like long builds or test suites). `ww-run` passes a long `--print-timeout 180m`, letting `ww-watch` and `Hancho` own the real timeout and stall detection.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | Ok (all leaves succeeded) |
| 1 | Pre-flight failed (another worker run is active) or ambiguous card id |
| 2 | Any other failure |
| 3 | **Human decision required** — an unsplittable card stalled after `MaxRetries` re-runs |
| 4 | **Split required** — a splittable card stalled; decompose it before re-running |
| 75 | ww-run refused the card (passed through): it is already running or overlaps another run's paths |

## Policy

On a stall the watcher kills the run and then:

1. If the card is **not** marked unsplittable, it stops with exit **4** and writes
   `stall-<id>.md` with `SPLIT_REQUIRED`. Claude must decompose the card into sub-cards
   (`<id>-1-*.md`, `<id>-2-*.md`, …) and make the card an index, or mark it unsplittable.
2. If the card contains the line **`Splittable: no`**, it is re-run at most `MaxRetries`
   times. Each re-run is told a previous run stalled and to continue from partial work.
3. If it still stalls after `MaxRetries`, the watcher stops with exit **3** and writes
   `stall-<id>.md` with `HUMAN_DECISION_REQUIRED`; a human then decides.

## Progress signals and adding a process CPU signal

`ww-watch` treats a run as making progress when either of these changes:

- **Worker-tree CPU**: CPU time of the agent process and all its descendants (builds, tests and
  tools the agent starts itself), threshold `-MinCpuDelta` (default 2 s).
- **Progress hash**: `git status` of the `watch` folders (`config.json`), the `reports\` listing
  and the current run log.

Work done by a process that the agent does **not** start (an editor or IDE that compiles or
runs tests on its own, a long-running dev server, a remote build agent) is invisible to these
signals, so such a run can look stalled. ww has no built-in setting for this on purpose. When a
project needs it, **an LLM agent (or a person) modifies the ww scripts for that project**.
Prompt an agent with this section; the change is small and has four parts:

1. `ww-watch.lib.ps1` / `Get-Snapshot`: find the extra process(es) by name, e.g.
   `@(Get-Process -Name "<ProcessName>" -ErrorAction SilentlyContinue | ForEach-Object Id)`, and
   add their CPU to the snapshot as a new field (`ExtraCpu = [double](Get-CpuSeconds $ids)`).
2. `ww-watch.lib.ps1` / `Test-Progress`: add a parameter `[double]$minExtraCpuDelta`, treat a
   CPU **drop** as progress (the process restarted), and a delta `>= $minExtraCpuDelta` as
   progress. Give it its **own, larger threshold**: an idle editor burns a few seconds of CPU
   every few minutes, and a threshold as small as `-MinCpuDelta` would hide real stalls (30 s
   worked for an editor that compiles and runs tests).
3. `ww-watch.ps1`: add `-MinExtraCpuDelta` and pass it to `Test-Progress`.
4. `ww-watch.selftest.ps1`: extend `Snap` with the new field and cover idle trickle (below
   threshold = no progress), real work (above = progress), a drop (= progress), and "worker
   below delta while the extra process is busy but below its threshold" (= stall).

Keep the process name in that project's copy of the scripts (or read it from
`ww-data\config.json`), not as a default in the shared copy. Run both self tests afterwards.

## State

Retry/stall counters persist across invocations in `ww-data/stall-state.json`.
A successful run removes the card's entry.

## Test hooks

`ww-watch.ps1` reads these environment variables (used by `tests/ww-watch.tests.ps1`):

- `WW_DATA_DIR` — overrides `data` (state file + `reports\`).
- `WW_TASKS_DIR` — overrides the tasks folder.
- `WW_RUN_SCRIPT` — overrides the script started per leaf (default `ww-run.ps1`).

All three scripts find their folders with `Resolve-WwPaths` (`ww-run.lib.ps1`), nothing is
hardcoded to one project:

- root: `WW_ROOT` (or `-Root`), else the git top level of the current directory.
- data: `WW_DATA_DIR`, else `<root>\ww-data`.
- settings: `<data>\config.json`, optional. `watch` lists the folders (relative to root) whose
  changes count as run progress and that `ww-status` scans for recently changed files:

  ```json
  { "watch": ["src", "Tools"] }
  ```

  Without it, every change outside the data folder counts as progress.

The self tests use `WW_ROOT`/`WW_DATA_DIR` to point a `-DryRun` (or a status view) at a fixture.

Self tests (no opencode, no real worktree):

```
powershell -NoProfile -ExecutionPolicy Bypass -File Tools\ww\ww-run.selftest.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File Tools\ww\ww-watch.selftest.ps1
```

The run self test also covers progress-line formatting (sample events plus synthetic read/edit/
bash/text/error/unknown/non-JSON lines), `Estimate N min` parsing, and dead-record cleanup with
a fake dead host and the self test's own live PID.

## Live progress log

`ww-run` starts opencode with `--format json` and stdin closed (opening it makes opencode wait
for input and print nothing), then reads the JSON event stream line by line while it runs. For
each run it writes:

| File | Contents |
|---|---|
| `ww-data/logs/<Task>-<stamp>.events.jsonl` | every raw event line, unchanged |
| `ww-data/logs/<Task>-<stamp>.progress.log` | one readable line per tool call / step |
| `ww-data/logs/<Task>-<stamp>.prompt.txt` | the exact prompt passed to opencode |
| `ww-data/logs/<Task>-<stamp>.log` | start/end summary (unchanged) |

Pass `-JsonStdout` (or set `WW_JSON_STDOUT=1`) to stream raw JSON lines to stdout for Hancho (`hancho-cli worker status|stream`), suppressing formatted progress lines on the console.

Progress lines look like `HH:mm:ss  <kind>  <short target>`:

```
19:04:12  read  src/Foo.cs
19:04:18  edit  src/Foo.cs
19:04:31  bash  npm test
19:05:02  text  DONE
19:05:04  error  ...
```

`read`/`edit`/`write`/`read_file`/`edit_file`/`write_file` use `filePath` or `Path`, `bash`/`run_command` use `command` or `CommandLine`, other tools
use `parameters` or `title`/`input`; targets are cut to 120 chars, assistant text to 160. Unknown
event types become `HH:mm:ss  <type>` and non-JSON lines become `HH:mm:ss  raw  <line>`; an
unexpected event never stops the run. The run record stores `Progress` (the progress-log path),
`Engine` (`agy` or `opencode`), and `WorkerPid` (retaining `OpencodePid` for backwards compatibility).

## ww-status

`Tools\ww\ww-status.cmd` runs the dead-record cleanup first, then for every record in
`ww-data/running/` shows the task, worker engine (`worker (agy)` or `worker (opencode)`),
main/worktree, elapsed time and the card's
`Estimate N min`. At or past the estimate it prints `OVER ESTIMATE (stop rule)` in red. When a
progress log exists it shows the last 5 steps and `last step <N> min ago` instead of the old
`waiting (model reply / tests)` CPU guess. The files-changed, git and "waiting for you"
sections are unchanged. `-Watch` refreshes every 10 s.

## Dead run records

A record in `ww-data/running/<Task>.json` is dead when its `Host` PID is not alive **and**
no worker (`agy.exe` or `opencode.exe ... run`) command line names that card. This happens when the `ww-run` process
is killed and its `finally` block never runs. The shared helper `Clear-DeadRunRecords` (in
`ww-run.lib.ps1`) runs in `ww-status` on every refresh and in `ww-run` before its duplicate
check. For each dead record it:

- writes `running/<Task>.last.json` with `Ended` = detection time, `ExitCode` = `null`,
  `Dead` = `true` and `ReportsWritten` (same rule as a normal run);
- appends `ended (dead record cleaned by <script> at <time>)` to the record's log;
- deletes `running/<Task>.json` and, if `running.json` names that task, `running.json`;
- prints one line per cleaned record.

A record whose `Host` PID is still alive is never touched (even if the PID was reused).

## Worktree runs (parallel with the main run)

One agent can work in the main checkout while others work in git worktrees (code only). Only the same task card, or a second main-checkout run, is refused: a
worktree run never blocks the main run and a main run never blocks a worktree run.

```
Tools\ww\ww-run.cmd <card-id> -Worktree [-Base main] [-DryRun]
```

- Creates `<root>\.claude\worktrees\ww-<card-id>` on branch `ww/<card-id>-wt` from
  `-Base` (default `main`). An existing worktree is reused only when it is on that branch; the
  tool never deletes worktrees.
- opencode runs with the worktree as its working directory. Because opencode rejects reading
  files outside it, the card is copied to the worktree's `ww-data/tasks/` and the prompt
  names the **relative** path; the gitignored `opencode.json` is copied too when the worktree
  does not have one (an existing one is never overwritten). Logs and run records stay in the
  main `ww-data/`.
- The agent writes `ww-data/reports/<Task>.md` inside the worktree and commits it there.
  After the run `ww-run` copies that report back to the main `ww-data/reports/<Task>.md`.
- Pre-flight refuses a worktree run when an Authorised edit names an existing path whose
  folder is untracked/ignored in git (for example a gitignored vendor folder before card
  150): `needs tracked files; run on main or land 150 first`. Such a worktree would start
  without the files the card is allowed to edit.
- `-DryRun` prints the plan (worktree, branch, run record, log, duplicate and pre-flight
  result) and changes nothing.
- Run records: `ww-data/running/<card-id>.json` with `Worktree` = path or `null`, plus
  `ww-data/running/<card-id>.last.json`. `ww-data/running.json` and `last_run.json`
  are still written for the main run only. `ww-status` lists every active record; `ww-watch`
  follows the watched task's record (falling back to `running.json`).

### What is not verified

Worktree runs are told to skip compile/tests and list them under "Verification pending" in
the report. Some build/test tools are bound to one checkout (an open editor, a running dev
server), and running them from a worktree would verify the main checkout instead (a false PASS).

### Merging a worktree run

On the main checkout, after reviewing the worktree report:

```
git merge --no-ff ww/<card-id>-wt
```

then run the project's own build and tests.

Only then is the code actually verified. Afterwards the worktree can be removed with
`git worktree remove .claude/worktrees/ww-<card-id>` (a human action; the tool never does it).
