# Resource Locks and Parallel Tasks (design note)

**[English](resource-locks.md) | [日本語 (Japanese)](resource-locks-ja.md)**

How to run several tasks (for example LLM agent runs) at the same time in a large project: each
task declares what it touches, and only tasks whose declarations overlap are serialized.

## 1. Who does what

| | Hancho (mechanism) | tools/ww (policy, optional) |
|---|---|---|
| Knows about | processes, jobs, dependencies, **named resources** | task cards, git, worktrees, `Authorised edits`, engines |
| Does | start/stop/timeout/retry, `dependsOn`, `concurrency`, **resource conflict checks**, why a job waits | card -> resource declaration, mode (shared / worktree), worktree creation, out-of-scope edit detection, commit/merge |
| git / worktree | **does not know**; it does not care whether a job's `cwd` is a worktree | all of it |

- Hancho does not know ww. ww runs on its own without Hancho.
- Worktrees are a ww feature; Hancho gets no worktree-specific feature. Relative
  resource paths resolve against the job's `cwd`. A job started with `ww-run -Worktree` still has
  the main checkout as its `cwd` (ww-run creates the worktree itself, after the job started), so
  its path resources would conflict with main-checkout jobs: **declare no path resources for
  worktree jobs** (named locks such as `@build` are fine). `ww-run -Jobs -Worktree` does this.

## 2. Hancho: the job's `resources`

```json
{ "id": "ww-012", "command": "...", "summary": "card 012", "timeoutSec": 7200,
  "cwd": "C:\\proj",
  "resources": {
    "write": ["src/llm_stream.cpp", "src/llm_stream.h", "doc/**", "@build"],
    "read":  ["src/taskqueue.h"]
  } }
```

### 2.1 Kinds of resource

| Form | Meaning |
|---|---|
| `@name` (`@build`, `@git`) | Named lock. Exact, case-insensitive match. Shared globally, independent of `cwd`. |
| `@name:{root}`, `@name:{cwd}` | Scoped named lock. `{root}` becomes the git top level above the job's `cwd` (the nearest folder with a `.git` directory or file, so each linked worktree is its own root; `cwd` itself when there is none or the job runs on a remote host). `{cwd}` becomes the normalized `cwd`. Jobs in different checkouts then hold different locks. |
| Path (`src/a.cpp`, `C:\proj\src`) | File or directory; a directory covers everything below it. Relative paths are made absolute against the job's `cwd` (kept relative when `cwd` is empty). |
| Bare file name (`Foo.cs`, `*.md`) | No `/`: the name anywhere below `cwd`, i.e. `**/Foo.cs` (same as ww). `**` alone is the whole `cwd`. |
| Glob (`src/**/*.cpp`, `doc/*`) | `*` (within one segment), `**` (across segments), `?`. |

Paths are normalized before comparison: `\` -> `/`, ASCII lower case, `.` / `..` resolved,
trailing `/` removed (Windows semantics).

Choosing the scope of a heavy external tool (an IDE or editor that locks its project, a
build daemon, an emulator, a tool with one licence):

- The tool allows one instance per machine: `@tool`.
- The tool allows one instance per project folder, and each worktree may run its own:
  `@tool:{root}`. Jobs in the same checkout wait for each other; jobs in other worktrees do not.
- To verify such a tool headlessly, run its batch/CLI mode as a job with the same lock and
  check the result with `success.testReport` (JUnit or NUnit XML) instead of the exit code.

### 2.2 Conflict rule

Jobs A and B conflict when any of these overlap (read/read never conflicts):

- A.write x B.write
- A.write x B.read
- A.read x B.write

Overlap test, segment by segment (conservative: when unsure, it overlaps). It is the same rule
as `Test-PathPatternOverlap` in `tools/ww/ww-run.lib.ps1`, so Hancho and ww-run agree:

- Two literal segments must be equal; a glob segment (`*.cpp`) must match the literal one; two
  glob segments are taken as overlapping.
- `**` matches any number of segments.
- When one side runs out first, it is an ancestor directory of the other (`src` covers
  `src/a.cpp`), except when a `**` ran over part of it and it ends in a file name: `**/foo.cs`
  does not overlap `src/b.cpp`, but it overlaps `src`, `src/foo.cs` and `**/foo.cs`.

A false positive only serializes more; it never breaks anything.

### 2.3 Scheduler and start order

- Resources are physical (files, the build), so **a conflict stops a job even when the jobs are
  in different queues**. Queues group work logically; resources exclude physically.
- Ready jobs of **all queues** are ordered by priority (high first), then registration order,
  then queue name and number, and started from the front.
- A candidate waits when it conflicts with a running job or with **a job ahead of it that is
  waiting** (FIFO per resource).
  - Example: reader A runs, writer W waits, then reader B arrives in another queue. B does not
    conflict with A, but it queues behind W, so a writer is never starved by a stream of readers.
  - A job ahead that waits for its own queue's `concurrency` is not overtaken by conflicting
    jobs of other queues either.
- So **conflicting jobs run one at a time, in priority + registration order**. Use `priority`
  to reorder; express "B uses A's result" with `dependsOn`.
- A job takes all its locks when it starts and releases them all when it ends (no hold-and-wait),
  so there is no deadlock.
- Independent of `concurrency`: `concurrency` caps how many jobs of a queue run; resources say
  which jobs may run together.
- A job without `resources` locks nothing and never waits (backward compatible).

### 2.4 Visibility

- `taskqueue graph` adds `resource_wait` per job: `"<job id>: <resource>"` when a running job
  holds it, `"<job id> (queued ahead): <resource>"` when it waits behind an earlier job, else
  `null`. The response also carries `resource_locks` (feature on/off).
- The CLI `taskqueue graph` view prints `(resource held by ...)`.
- `resources` is accepted by `taskqueue add` / `taskqueue update`, persisted, and returned by `taskqueue get`.

### 2.5 On / off

- Settings checkbox "Task queue: never run jobs with conflicting resources together".
  **On by default.**
- Saved in the settings file (`config.json`, `resource_locks`) and restored on the next start.
- When off, declared `resources` are kept but ignored; turning it on again applies from the next tick.
- ww-run's own pre-flight does not depend on this setting (ww may run without Hancho).

### 2.6 Known limits

- Declarations are self-reported; Hancho does not watch file access (detection is the
  caller's job, e.g. ww).
- Because of FIFO, a job ahead (e.g. in a queue whose `concurrency` is full) can hold back
  conflicting jobs of other queues. Raise `priority` for urgent jobs.
- Semantic dependencies (A changes an API that B uses) are not resources; use `dependsOn`.

## 3. tools/ww: modes

| Mode | Flag | Runs in parallel when | Pros | Cons |
|---|---|---|---|---|
| main | `-Main`, or a card without `Authorised edits` | only one main run | nothing to declare; may build/test | no parallelism |
| **shared** | `-Shared`, the default for a card with `Authorised edits` | its `Authorised edits` do not overlap other main/shared runs | shared build cache, no merge | relies on accurate declarations; no build/test while parallel |
| worktree (existing) | `-Worktree` | always | full isolation | merge and build-cache cost |

### 3.1 Shared-mode rules

- The run record `ww-data/running/<id>.json` stores `Edits` (declared paths) and `Shared`.
  A plain main run counts as `Edits = ["**"]` (the whole checkout).
- Pre-flight (`Test-RunConflict`): the same card is always refused; main/shared runs are compared
  by their declarations. A run whose declaration is unknown (no record, no `Edits`) counts as the
  whole checkout. A refused run exits with **75** (EX_TEMPFAIL: nothing ran, try again later),
  distinct from setup errors (1) and engine failures.
- A card without `Authorised edits` locks the whole checkout even with `-Shared` (same as main).
- Paths in the card's `## Read-only` (or `## Reads`) section are **read-only**: runs that only
  read the same file run together; only an edit of it by another run conflicts.
- A bare file name (`Foo.cs`) is treated as `**/Foo.cs`.
- The prompt adds: other runs edit this checkout in parallel; edit only the Authorised edits;
  skip compile/tests and list them under Verification pending; do not commit.
- After the run, files edited according to the event log that are outside the declaration (and
  outside `ww-data`) are recorded as `EditViolations` in `.last.json` and the log, with a
  warning. The exit code is not changed.

### 3.2 With Hancho

Use the card's `Authorised edits` as `resources.write` and its `Read-only` paths as
`resources.read`. Hancho never starts overlapping cards, and ww-run's pre-flight applies
the same rule (bare names, overlap test), so both layers agree.

- Let `ww-run.cmd -Jobs 012,013 ...` write the jobs from the cards instead of copying paths by hand;
  the declaration then comes from one place.
- A job that runs ww-run as a **main** run (`-Main`, or a card without `Authorised edits`) must
  declare `"write": ["**"]`, the lock ww-run itself takes. Without it Hancho starts it
  next to other runs and ww-run refuses with exit code 75. The task queue then defers the job (job
  field `defer`, on by default for exit code 75): it goes back to pending and starts again after
  60 s without using up `maxAttempts`, so nothing breaks, but the job keeps polling instead of
  waiting for the lock. `-Jobs` writes `**` for such cards.
- Worktree jobs declare no path resources (see section 1).

```json
{ "id": "ww-012", "summary": "card 012", "timeoutSec": 7200, "cwd": "C:\\proj",
  "resources": { "write": ["src/a.cpp", "src/a.h"] },
  "command": "C:\\proj\\tools\\ww\\ww-run.cmd 012 -Shared -Engine claude -Root C:\\proj -JsonStdout" },
{ "id": "ww-013", "summary": "card 013", "timeoutSec": 7200, "cwd": "C:\\proj",
  "resources": { "write": ["src/b.cpp"] },
  "command": "C:\\proj\\tools\\ww\\ww-run.cmd 013 -Shared -Engine claude -Root C:\\proj -JsonStdout" },
{ "id": "verify", "summary": "build + test", "timeoutSec": 3600, "cwd": "C:\\proj",
  "resources": { "write": ["**"] }, "dependsOn": [{"id": "ww-012", "on": "done"}, {"id": "ww-013", "on": "done"}],
  "command": "C:\\proj\\build.cmd" }
```

Use `--concurrency` 2 or more. The verify job writes `**`, so it runs alone after the parallel runs.

## 4. Scope

1. Hancho: `TaskQueueJob::resources`, parsing/validation/persistence/update, the
   scheduler check, `resource_wait` in graph, CLI/MCP descriptions, smoke tests.
2. Hancho: one start order across queues (FIFO per resource), settings checkbox and
   persistence.
3. ww: `-Shared`, the `## Read-only` section, `Edits`/`Reads` in run records, overlap check,
   shared-mode prompt, out-of-scope edit detection, ww-status display, self tests, README.
