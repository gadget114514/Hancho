# CLAUDE.md

This file is read by two kinds of agents. First determine which one you are, then follow only the relevant section.

| Role | Who | Sections to follow |
|---|---|---|
| Supervisor | The Claude session talking to the user (main / sub / worktree) | 1 Common rules + 2 Supervisor |
| Worker | An agent launched by the task runner to implement one task card (opencode / agy / Claude Haiku, etc.) | 1 Common rules + 3 Worker + `AGENTS.md` |

Determine your role: if your prompt starts with "You ARE the ww worker" or similar and asks you to execute a task card (`<tasks-dir>/NNN-*.md`), you are a Worker. Otherwise you are the Supervisor.
**Section 2 (Supervisor) does not apply to Workers.** A Worker must not read it and use it to refuse or delegate implementation.

---

## 1 Common rules (all agents)

### General
- Token saving: keep communication and code concise.
- Keep clean architecture.
- Documentation: all project plans and task logs go in `<docs-dir>`.
- Debug logs: prefix messages with `[Filename] [UniqueNumber]`.
- Debug GUI: English titles, format `[3-digit ID]_[ActionName]` (e.g. `001_Reset`).

### Naming & Namespaces
- Root namespace: all scripts use `<RootNamespace>` or a sub-namespace (`<RootNamespace>.Combat`, `<RootNamespace>.Enemy`, ...).
- Never use `Editor` as a namespace segment. Use `DataEditor` instead (e.g. `<RootNamespace>.DataEditor`).
- Forbidden words: do not use generic engine types such as `Material` or `Resource` in names. Use domain terms such as Loot, Ingredient, or Supply instead.
- Do not use keywords reserved by the engine (e.g. `NavMesh`). Pick a unique name instead.

### Third-party code
- Never modify third-party packages, libraries, or generated code. Put integration code in your own namespace instead.

### Refresh after completing a task
After a task is complete, ask the engine to refresh its assets. If your CLI has no dedicated refresh command, use a generic one:
`<cli> execute-dynamic-code --code "AssetDatabase.Refresh(); return 1;"`
- Workers: run this right before writing the report for tasks that use the engine. Do not run it for shared tasks that do not.
- Supervisor: run it in the main checkout after a card job finishes (success or failure). For worktree cards, run it after merging back to main.
- Do not run it while the engine is in Play Mode; run it after stopping. Record the result in the report or chat.

---

## 2 Supervisor (Supervisor only — Workers skip this section)

### Division of responsibilities
The Supervisor does not write or edit code (C#, shaders, prefabs, assembly definitions, etc.). Always delegate implementation to a Worker via the task runner.
- Supervisor tasks: making decisions, writing task cards (`<tasks-dir>/`), recording decisions (`<decisions-dir>/`), documentation, and reviewing Worker reports.
- Validation the Supervisor may do: compiling, running tests, checking logs. Prefer headless tests; use the in-editor runner only for tests that require a booted editor. See `AGENTS.md` for details.
- When existing code must change, state the allowed files and scope in the task card and get the user's approval before handing it to a Worker. Present diffs and proposals to the user as text in chat.
- Documentation (Markdown/text): you may freely add, edit, or rewrite it, including as new Artifacts.
- Plan mode: do not ask for permission until the plan is complete.

### Session layout
- Keep one main session at `<project-root>` for engine validation. All other sessions are subs (worktrees).
- Always start subs with a worktree on. Validate worktree changes by compiling/testing before merging (e.g. a headless runner with an overlay); do visual/in-editor validation after merging to main (commit main first if it has uncommitted changes).

### Running cards — use the task runner
- Use the configured task runner for job execution. Do not call the worker directly from a background shell; use the queue so jobs appear in the dashboard.
- Register one card at a time with the queue, setting a timeout estimate and locking the shared resource (e.g. the editor) for engine cards. Use the success condition on the report file.
- For multiple cards or flows where the next step depends on the result (ordered execution, decisions on failure, resume cards), use a behavior tree.
- Do not put `&&` or `;` in job commands (they are rejected); express dependencies via `dependsOn` or the behavior tree.
- Only one card that uses the engine at a time (respect the editor lock and status tool).

### External command rules
Before launching an external command (worker runner, builds, tests, engine CLI, etc.), estimate how long it should take. If it is still running past that estimate, terminate the process (including children) and clean up any run records such as `running.json`. After termination, diagnose the cause (waiting on input, hang, or heavy work) before retrying or splitting the job.

---

## 3 Worker (Worker only)

You are the implementer. The Supervisor's "do not write code" and "delegate to the runner" rules **do not apply to you**.
- Implement your assigned card directly with your tools and always write the report `<reports-dir>/<card-id>.md` before exiting (including if you stop early, saying where and why you stopped).
- Only change the files and scope listed in the card's "allowed files and scope". If a change outside that scope is needed, do not implement it; record it in `<decisions-dir>/NNN.md` and stop.
- Do not run the worker runner / status / watch commands, and do not launch another worker.
- An entry for your task in `running.json` is your own run record, not a duplicate run.
- Follow `AGENTS.md` for tool, Git, and temp-file rules.
