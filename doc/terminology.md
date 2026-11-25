# Glossary

Status: decided (2026-11-24). The names change in 0.10.0, including the API, JSON, and CLI.

Japanese version: `doc/terminology-ja.md`.

## Why this is needed

The same word pointed at several different things at different levels.

| Word | Meaning 1 | Meaning 2 | Meaning 3 |
|---|---|---|---|
| job | A row in the queue (one process run) | A BT node type (you write the command yourself) | — |
| worker | The role of a process run as a job | A BT node type (starts an LLM) | The name of an LLM list |
| task | `taskqueue` (the order of jobs) | A task group (a virtual row for a running BT) | A 0.10 task (a record of one request) |

We split them as follows.

| Current word and meaning | New name |
|---|---|
| job (meaning 1: a row in the queue) | **job** (unchanged) |
| job (meaning 2: node type) | **exec** |
| worker (meaning 1: role) | **worker** (unchanged) |
| worker (meaning 2: node type) | **agent** |
| worker (meaning 3: LLM list) | **agent list** |
| task (meaning 1: `taskqueue`) | **taskqueue** (unchanged; the order in which tasks run) |
| task (meaning 3: a record of one request) | **task** (unchanged) |
| A BT or group of jobs put on the queue (includes task groups) | **direction** |

## Hierarchy

```
task                 One request. Holds the intent and the progress log
 └ direction         One registration on the queue: one BT, or one taskqueue add
    └ job            One process run. One row in the queue
       └ attempt     One run of a job (repeated within maxAttempts)
```

- **A direction is the unit between a task and the queue.**
  - A BT is one direction. Each time a node runs, it emits a job for that direction.
  - `taskqueue add --file jobs.json` is one direction. The jobs in the file belong to that direction. Registering a single job is also one direction.
  - `routine run` is also one direction (its contents are a BT).
- **A BT is the mechanism that runs a direction.** A direction is "what to do" (one unit of instruction). A BT is "how to run it" (the execution mechanism).
  - The contents of a direction are written as a BT. BT nodes express what runs in sequence or in parallel, and what happens on failure (`retry`, the fix loop, engine switching, escalate). Hancho advances the BT, emits a job per node, looks at the result, and decides the next node.
  - Success, recovery, and waiting for a decision are determined by the BT's state. A failure that the BT recovers from inside does not fail the direction (and does not become a triage card). The direction fails only when the BT ends as `failed`.
  - When a task card is run, a BT is built from its body and a template and then executed (template `card` in `doc/task-card-store-cards.md`: agent → build → test → fix on failure). A routine is a named, saved direction, and its mechanism is a BT.
  - A direction from one `taskqueue add` does not use a BT. It is only jobs ordered with `dependsOn`, with no branching or recovery. (Written as a BT, it corresponds to a set of exec nodes in a row.) If the next step should depend on the result, use a BT.
  - No mechanism other than a BT is planned for running a direction. A direction record (below) is kept separately from the BT, and the BT id is referenced from the direction as `btId`.
- **Two node types emit jobs in a BT: exec and agent.**
  - exec: you write the command yourself. It can be anything (a build, a test, a script that starts an LLM).
  - agent: you write the engine, model, and instruction text, and Hancho builds the command from `engines.json` and starts the LLM.
  - Both emit one job at run time. The only difference is who builds the command. Success conditions, retries, and candidate switching can be written for either.
  - To switch candidates in an exec node, put `{engine}` / `{model}` in the command.
- **Relationship between worker and agent:** they are not the same.
  - A worker is the role of a process running a job. It can be a build, a test, a script, or an LLM.
  - An agent is an engine + model pair. It exists even when nothing is running (an agent list row, a candidate in `engines`, a candidate for escalate's proxy answer, or the target of `probe`).
  - While an agent runs as a job, that process is a worker (agent node, `run_worker`). A build or test worker is not an agent.
- **A lineage** is the sequence of jobs linked by retries (`supersedes`). It lives inside a task and may cross directions (a job from a failed direction is redone as a job in a new direction).
- **Retries inside a BT** (the `retry` node, the fix loop, `bt retry`) happen within the same direction. They do not form a lineage.
- **A standalone process:** a process started with `start` is not part of any task, direction, or job. There is no success check, retry, or usage aggregation.
- **A task card is a direction.** A large task (one feature) has several cards, and each card is run as one direction (for example, one BT: agent → build → test → fix on failure). Cards do not split the task; the task stays one.
  - A task ends when all its cards have succeeded or when it is closed.
  - When a card fails it becomes a triage item. Triage decisions (retry, close, and so on) are made on that card, not on the whole task.
- **A triage card is also a direction.** A triage card is the failed direction itself, waiting for a decision. It is not a separate unit (one direction has at most one triage card).
  - The cause, candidates, recommendation, and decision are held as information attached to that direction. The card's id is the direction's id.
  - However many jobs fail inside a direction, there is only one card. It lists all the failed jobs as evidence.
  - Deciding "close" closes that direction. Deciding "retry" either tries again inside the same direction, or creates a new direction that carries over via `supersedes` (a rescue). If the rescue direction fails, it becomes one more card.
  - In short, a task card (a direction not yet run or currently running) and a triage card (a failed direction) are the same direction, referred to differently by state.
  - A task card and directions can be one-to-many. If a run fails and is fixed and re-run, the same task card gets two directions. This is the same relationship as a saved BT (routine) and its runs.
  - Until now these lived only as Markdown in `doc/` (for example, cards 100-x in `doc/task-lineage-cards.md`), which Hancho did not know about. They are now saved inside the task and handled as directions not yet run (`planned`) (see "Direction records" below).

## Terms

### Units of execution

| Term | Meaning |
|---|---|
| task | One thing the supervisor is trying to do. A record created by `task create`. A job from a direction without `--task` belongs to an "implicit task" whose id is the first job's id in the lineage. A `routine run` without `--task` is the exception: it creates a real task (`Routine: <name>`) and adds its direction there |
| direction | One registration on the queue: one BT, or one `taskqueue add` (even with one job). Held as a record (see "Direction records" below). The BT virtual row that is currently called a "task group" (`group:<id>`) is the direction of a running BT |
| job | A unit that runs one command as one process. One row in the queue. Has `id` / `command` / `summary` / `timeoutSec` |
| attempt | One run of a job. Kept in `attempts[]` |
| lineage | The sequence of jobs linked by `supersedes` |
| standalone process | A process started with `start` / `start_process` |

### Containers and definitions

| Term | Meaning |
|---|---|
| queue (taskqueue) | The order in which tasks run. What actually lines up is the task's jobs, ordered with dependencies. The name does not change |
| BT (behavior tree) | A tree of nodes: a definition, written as one JSON, that changes the flow based on results. It is adopted as the mechanism that runs a direction (direction = what to do, BT = how to run it). Running it creates a direction |
| routine | A BT saved under a name |
| task card | A unit of work inside a large task (= a direction). A task has several cards, and each is run as one direction. The contents are the description of the work (files to touch, what to do, tests) and dependencies on other task cards. Numbers look like `102-3`: "plan number-sequence number". Just "card" means a task card (the "card 100-x" and "card 102-x" in `doc/`). To refer to a failed direction, say "triage card" to tell them apart (both are the same direction) |

### BT nodes

| Term | Current `type` | Meaning |
|---|---|---|
| exec node | `job` | You write the command to run yourself |
| agent node | `worker` | You write the engine, model, and instruction text, and Hancho starts the LLM |

Other nodes (`probe`, `wait`, `set`, `escalate`, `fail`, `succeed`, composite, decorator, and condition nodes) keep their names.

### Participants

| Term | Meaning |
|---|---|
| supervisor | The LLM talking with the user. Creates tasks, issues directions, and decides the next step from the results |
| Hancho | Owns the processes, and does monitoring, success checks, retries, and recording |
| worker | The role of a process running a job (LLM, build, test, script). The counterpart of supervisor: the supervisor decides and delegates work, and the worker carries it out |
| agent | An engine + model pair (definition in "LLM terms" below). While running as a job, it is a worker |
| peer | A party exchanging messages in Chat (an LLM or Hancho). The agent in Chat's current `--from-agent` / `--to-agent`. Different from an agent (an LLM running as a job) |
| host | A Hancho on the LAN, as a Chat party (an address such as `192.168.1.20:2426`). The peer of the current `chat peers` |

### LLM terms

| Term | Meaning |
|---|---|
| engine | The CLI that runs a model (`claude` / `opencode` / `codex` / `agy`). Handles tool execution, file reads and writes, and conversation management. Added through `engines.json`. Commonly called a harness, but Hancho calls it an engine (to match the names in JSON, CLI, the `{engine}` placeholder, and usage aggregation) |
| model | The model name passed to the engine |
| agent | An **engine + model** pair. Example: `claude` + `haiku`, or `agy` + `gemini-4-hydrogen-medium`. The same model under a different engine is a different agent |
| candidate | One agent listed in `engines` (an engine and model pair) |
| agent list | The approved agents that may be used (the current LLM list's `worker`). Each row is an engine and model pair, so the contents are already a list of agents |
| consult list | The approved candidates that may be used for escalate's proxy answer (currently `consult`; the name does not change) |
| switch | Running the same node again with the next candidate after a failure |

### Failures and decisions

| Term | Meaning |
|---|---|
| failure class | The kind of failure (`exit_nonzero`, `startup_hang`, `fix_no_progress`, and so on) |
| escalate | A node in a BT that asks the supervisor for a decision. The answer is given with `bt resume` |
| triage | The mechanism that decides how to handle a card (direction) that became a triage item (failed) within a task. It gives the cause, candidates (retry, retry with changed content, close, take over), and a recommendation. The target of a decision is the card, not the whole task |
| triage card | The failed direction itself, waiting for a decision (triage card = direction). Its id is the direction's id, and there is at most one per direction. The cause, candidates, recommendation, and decision are attached to the direction. A standalone process (`start`) has no direction, so it is shown as a notice, not a card |
| takeover | Recording that the supervisor will finish a job or an escalate itself |

## Direction records

- One record is created each time `bt start`, `routine run`, or `taskqueue add` runs (even with one job).
- Fields: `id`, kind (`bt` / `jobs`), `taskId`, creation time, and the session that created it (`origin`).
- A job belongs to a direction through `directionId`. Jobs emitted by a BT belong to that BT's direction.
- How ids are chosen (using the BT id as-is, or numbering with a prefix such as `dir_`) is decided in the implementation card (tentative answer: `dir_` + sequence number; see `doc/direction-cards.md`).
- The virtual row in the list changes from `group:<id>` to `direction:<id>`.
- Decision: a task card can be created first as a direction not yet run (state `planned`).
  - Contents: the body of the task card (or the BT definition itself), the name of the template used, and dependencies on other directions (`after`).
  - When run, a BT is built and the same record becomes `running`.
  - A retry is a new direction that carries over the original (`supersedes`). This uses the same shape as the job lineage, one level up.
  - Opening a task shows the progress of each task card (done, running, waiting on dependencies). Cards without dependencies can run in parallel.
  - The records are stored in the direction record, and the template that builds a BT from the body is `card` (`doc/task-card-store-cards.md`).

## Name changes in 0.10.0 (implemented)

In 0.10.0 the names in the API, JSON, and CLI were changed. The old names are still accepted as aliases (so existing BTs, routines, and templates keep working).

| Place | Now | 0.10.0 |
|---|---|---|
| BT JSON | `"type": "job"` | `"type": "exec"` |
| BT JSON | `"type": "worker"` | `"type": "agent"` |
| BT JSON | `defaults.job` / `defaults.worker` | `defaults.exec` / `defaults.agent` |
| BT warnings | `unguarded_worker` and similar, which refer to node types | Match the type name |
| LLM list | `--list worker` | `--list agent` |
| Virtual list row | `group:<id>`, "task group" | `direction:<id>`, "direction" |
| job fields | none | `directionId` |

## New names where the old ones clashed (implemented)

Where an existing name clashed with a new term, a new name was created. The old names are still accepted as aliases.

| Place | Old name (alias) | What clashes | New name |
|---|---|---|---|
| Chat CLI | `--from-agent` / `--to-agent` / `--audience agent` | Different from agent (an LLM running as a job) | `--from-peer` / `--to-peer` / `--audience peer` |
| Chat CLI | `chat peers` / `chat refresh`'s peer (a Hancho on the LAN) | Clashes with the peer above (a party in a message) | host (`chat hosts`) |
| Chat message JSON | `direction` (send/receive direction `in` / `out`; `src/cli.cpp`, `src/gui_dashboard.cpp`) | direction (one registration on the queue) | `flow` |
| MCP | `agent_type` (`run_worker` and others) | The content is an engine name, not an agent (engine + model) | `engine` |
| CLI and MCP | `run_worker`, `worker stream` / `worker probe` / `worker health` | The content is LLM-only, which corresponds to agent | `run_agent`, `agent stream` / `agent probe` / `agent health` |

- `order` and `instruction` are not used. Both were considered as candidates for direction before, but they clash with existing fields. `order` is the sequence number in the exported job JSON (`ExportQueueJobJson` in `src/cli.cpp`). `instructionsText` / `instructionsFile` are the instruction documents passed to the worker by a job and by BT nodes.

## Remaining work

- Name changes in 0.10.0 and direction records → task cards 103-1 to 103-8 (`doc/direction-cards.md`).
- Saving task cards in Hancho (the plan for direction records above) → task cards 104-1 to 104-6 (`doc/task-card-store-cards.md`).
- Rewrite `skills/hancho/SKILL.md`, help, `doc/behavior-tree*.md`, and similar to match this glossary → task card 103-8 (done together with the name change; updating the documents to new names before the names change would point people at names that are not yet accepted). `doc/task-lineage-plan.md` has already been rewritten.
