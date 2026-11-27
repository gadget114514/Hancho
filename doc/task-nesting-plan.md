# Task nesting design note

## Current model

`task → direction → job → attempt`. A direction is one queue registration (a behavior tree or a task card) and stores the id of the task it belongs to. BT runs now always belong to a task: `bt start` without `--task` creates one (`BT: <summary>`), and `supersedes` records which direction a run replaced.

## Target model (separate card)

Allow an optional nesting level:

`task → direction → task → direction → job → attempt`

- No new `project` entity. The root is still a task record.
- A child task gets `parentDirectionId` pointing at the direction that owns it. Root tasks keep it empty.
- A direction keeps its own `taskId` as today; the parent direction is derived from the child task.
- `supersedes` stays on the direction and means "this run replaced that run", not "this node contains that node".

## Work required

- Extend the task record with `parentDirectionId`; keep it empty for existing tasks so the change is backward compatible.
- Make task state aggregation recursive: a parent task's state comes from its child tasks and their directions.
- Make the dashboard task tree recursive, with cycle detection and a depth guard.
- Keep triage cards attached to directions, not to tasks; a nested task can have its own directions and cards.
- Add CLI/MCP fields for creating a task under a direction, and tests for nesting, state aggregation and cycle rejection.

## Out of scope

- BT nodes that spawn tasks. A `task` node would have to define state propagation, cancellation and retry semantics; treat it as a later card.
- Changes to this branch (`fix/bt-run-belongs-to-task`), which only covers task ownership and run lineage.
