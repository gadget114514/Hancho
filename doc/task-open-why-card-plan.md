# Card: deterministic "why is this task open" reason card

## Goal

Add a read-only, **deterministic** reason card that explains why a task is still `open`/not finished.
No LLM is involved in producing the reasons. LLM summarization of these facts is explicitly out of
scope for this card (it can be layered on later through triage why/advise).

The user asked: first add the mechanical card (blocker kind + direction/job id + wait reason), and
only ask an LLM to summarize when necessary. This task implements the mechanical part.

## Facts to enumerate (all already in the data model)

`TaskStateLocked` reports `open` when `planned`/`open` directions remain (src/taskqueue.cpp, around
10050-10130). Enumerate one entry per cause, with `kind`, `id`, `detail`, `source`:

- `planned_direction` / `open_direction` — direction id, kind, state (from `DirectionStateLocked`
  and the direction record).
- `pending_job` / `blocked_job` — job id, state, `blockedBy` (job.blockedBy), `holdReason`,
  `retryAtMs`, `resource_wait`.
- `waiting_for_permit` / `waiting_for_llm` / `paused` / `idle` — job or BT group id and state.
- `card_waiting_for` — card number and each unsatisfied `after` dependency (`waitingFor` in
  TaskShow's `cards`).
- `superseded_run` — old direction/job id and its `supersededBy` target.
- `escalate_question` — pending BT escalation (BT id / node id / question) when available from the
  BT state file or IPC.

Sort entries by a fixed key (`kind`, then `id`, then `detail`) so repeated calls return identical
output. Do not guess intent; say only what the recorded state proves (e.g. "waiting on job X",
"card 102-3 waits for 102-2", "direction dir_7 planned with no job yet").

## Backend

- Add a `TaskOpenWhy` method on `TaskQueueManager` (src/taskqueue.h / src/taskqueue.cpp) that
  returns `nlohmann::json` (array of entries) for a task id, using existing stores/queues under the
  existing lock. Keep the implementation self-contained and deterministic.
- Wire it into `TaskShow` so the response gains a top-level `"why"` array (always present, empty
  when there is no cause). `TaskWait` reuses TaskShow and inherits the field.
- IPC `task_show` handler is at src/ipc_server.cpp:2143-2146; no new action is required if the field
  is always computed. MCP `task_show` forwards raw IPC JSON, so it gets the field automatically.

## CLI

- `hancho-cli task show <id> --why` prints a human-readable `Why open (N):` block with one line per
  entry. With `--json`, the raw JSON (including `why`) is already printed by the existing shortcut.
- Parsing is in src/cli.cpp around 8105-8117; human formatter `PrintResponse` task_show branch is
  around 4616-4689. `--why` is a valueless boolean flag.
- Help text: src/cli.cpp:2783, :8044, :3249 (and the option list at :3257 if appropriate).

## GUI (read-only dialog, not the triage dialog)

- Add a right-click item on task-list rows: `なぜ open か` / `Why is task open...` (new command id,
  e.g. 3116) next to the existing "Why failed" item. Context menu builder:
  `ShowTaskQueueContextMenu` around src/gui_dashboard.cpp:6776; real-job menu around :7120-7194;
  dispatch around :7370-7377. The task id is available from `st->taskDisplayRows[idx].taskId`.
- Open a small read-only dialog that lists one row per reason (`kind`, `id`, `detail`) using the
  existing `TaskShowIpc` wrapper (src/gui_dashboard.cpp:2093-2111). Follow the `PastRuns` dialog
  skeleton: state struct :4694-4721, WndProc :5052-5265, opener :5268-5337 (window class
  registration, GWLP_USERDATA, read-only controls, theme colors, Close button, WM_NCDESTROY
  cleanup). A LISTVIEW plus a read-only detail EDIT is enough.
- Do **not** paste these reasons into the triage dialog: triage is a decision workspace keyed to a
  failure card and its `reason` field records a decision, not an explanation. If a reason has a
  triage card, an optional "Open in Triage" button may call
  `ShowTriageDialog(st, L"all", cardId)` (src/gui_dashboard.cpp:22475).

## Tests and verification

- Add tests in tests/taskqueue_smoke.cpp: a task with (a) a planned card, (b) a pending job with a
  dependency/`blockedBy`, (c) a `waitingForPermit` job, and (d) a superseded direction. Assert the
  `why` array contains the expected kinds/ids and is byte-identical across two `TaskShow` calls.
- Build: `cmake --build build --config Release --target Hancho hancho-cli taskqueue_smoke`
- Run `build\Release\taskqueue_smoke.exe` and report the tail (ALL PASSED / failure count).
- Do not commit; report the changed files and verification results to the supervisor.

## Repo state / constraints

- Branch `main`, working tree clean at start. Other agents may commit unrelated work concurrently;
  touch only the files needed for this card.
- Follow existing code style; no comments unless they explain non-obvious logic.
- Keep 1 job = 1 command; do not use shell command chaining.
