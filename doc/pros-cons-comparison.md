# Hancho: Pros/Cons Comparison

Pros/Cons analyses for choosing how to run work, comparing Hancho with LLM-native sub-agents and other overlapping technologies.

Back to [README](../README.md).

---

## Sub-Agent vs. Hancho Execution

A short Pros/Cons analysis for choosing how to run work: **LLM-native sub-agents** vs. **Hancho-managed CLI processes**.

### When Token Cost Is No Longer the Difference

Token usage used to be the main reason to hand work to Hancho. Now that the supervisor's cost of waiting on a Hancho job (`taskqueue_wait`, `bt_wait`) is about the same as delegating to a sub-agent, the choice comes down to **durability, deterministic control and visibility**, not price.

| | Sub-agent | Hancho |
|---|---|---|
| **Lifetime** | Lives inside the parent's turn. Lost if the parent is interrupted, crashes, compacts its context or the session closes. | Runs in the dashboard, independent of the LLM session. Another session can pick it up later with `wait` / `get_state`. You can start an hour-long job and walk away. |
| **Control flow** | "Run A and B, then C; retry twice; escalate on failure" is reasoned out by the parent LLM on every turn. It can slip and is not reproducible. | Written once as task-queue dependencies or a behavior tree, then executed deterministically by Hancho. The LLM only writes the plan and answers `escalate` nodes. |
| **Visibility and intervention** | A black box until it returns. | The dashboard and BT view show every worker's state and output live. You can kill, retry, pause or send input at any point. |
| **Kinds of worker** | Claude inside the same client only. | Other engines (Codex, Gemini/agy, opencode) and ordinary processes (builds, tests, servers) in the same queue or tree. "Review with a different model" or "continue when the tests pass" is natural. |
| **Concurrency and load** | However many the parent calls at once. | Queue `concurrency`, priorities and resource locks keep a large batch from running away. |
| **Setup and context** | None. Inherits the parent's conversation. | Windows-only install. Each job's spec (prompt, command, timeout, success check) must be explicit. |

**Rule of thumb:** short, disposable work whose result the parent uses right away → sub-agent. Long work, work that must not be lost, work with dependencies or retries, work a human wants to watch or stop, or work that mixes in non-Claude workers → Hancho.

### Sub-Agent Execution (LLM-native)

**Pros**
- Deep integration with the supervising LLM: shares context, reasons adaptively, uses tools, and can change plans mid-flight.
- No extra binary or setup; works wherever the LLM client runs.
- Ideal for short, reasoning-heavy, code-editing and exploratory tasks.
- Natural error diagnosis and retry with human-like judgment.

**Cons**
- Bounded by the parent's turn and context window; lost when the parent session is interrupted or closed.
- Poor fit for long-running, interactive, or process-tree-heavy workloads (builds, dev servers, TUI agents).
- Limited observability: no live terminal grid, idle detection, or job-object tree control.
- Concurrency and dependency orchestration are client-dependent and often implicit.
- Hard to persist or recover state across LLM session restarts.

### Hancho Execution (external CLI processes)

**Pros**
- Runs real CLI agents/tools (`claude`, `opencode`, `codex`, build scripts) as managed OS processes with Win32 Job Object tree control.
- Live dashboard: up to 16 terminals, idle detection, per-job timeouts, retries, dependencies, and persisted task queues.
- `.done` markers and per-process logs enable simple, robust completion detection by LLMs/automation.
- Claude Code sub-agents started by a Hancho run are visible: the monitor pane shows each one as a tree branch with its steps, tokens and tool count, and `hancho-cli agent status` lists them.
- The LLM acts as supervisor only: dependencies, retries and branching (task queue, behavior trees) run deterministically inside Hancho.
- Mixes LLM engines (Claude, Codex, Gemini/agy, opencode) and plain processes in one queue or tree.
- MCP + CLI integration and project-local data directories.
- State recovery across restarts.

**Cons**
- Windows-only; requires installation and setup.
- Processes are comparatively opaque: the LLM sees captured output, not internal reasoning.
- Task specs must be explicit (command, timeout, success checks).
- Adds operational complexity and resource overhead.
- Interactive debugging requires attaching via the dashboard.
- Sub-agent detail is only what the stream reports (steps, status, tokens, tool count); it does not show a sub-agent's reasoning.

### Recommendation

Use **sub-agents** for short reasoning and code changes whose result the parent needs right away; use **Hancho** for parallel, long-running, interactive, or build-heavy CLI workloads, and for any work that must survive the session, follow a fixed plan, or be watched and steered. Best of both: let the LLM supervisor delegate heavy jobs to Hancho while sub-agents handle analysis and edits.

---

## Other Alternatives vs. Hancho

Pros/Cons of the alternatives that overlap most with Hancho. Each list describes the alternative; the comparison line states where Hancho differs.

### Claude Code Built-in Background Execution (`run_in_background`, Monitor)

**Pros**
- Zero setup: built into the LLM client, nothing to install.
- Completion notifies the LLM automatically; no polling or `.done` markers needed.
- Cross-platform wherever the client runs.

**Cons**
- Tied to the session: background jobs and their state do not survive a client restart.
- No dashboard; output is visible only through the LLM.
- No dependencies, priorities, retries, idle detection, or success checks.
- Interactive input (ConPTY, TUI agents) is not supported.
- Process-tree cleanup depends on the client.

**vs. Hancho:** Use built-in background execution for a few one-off long commands within a session. Use Hancho when jobs must outlive the session, form a dependency graph, run interactively, or be watched live.

### tmux / screen-based Agent Managers (e.g. claude-squad)

**Pros**
- De facto standard on Unix; mature, lightweight, scriptable.
- Humans can attach to any agent's terminal and take over interactively.
- Agent managers often pair each agent with a git worktree for isolation.
- Sessions survive terminal disconnects (and SSH).

**Cons**
- Not Windows-native (needs WSL/MSYS); Windows tools and toolchains are awkward to drive.
- No MCP interface: a supervising LLM must scrape panes with `send-keys` / `capture-pane`.
- No structured output (JSON), completion markers, timeouts, retries, or dependencies.
- No job-object-style process-tree control.

**vs. Hancho:** Hancho offers the same "many live agent terminals" model natively on Windows, plus MCP/JSON control and a task queue. tmux remains the better fit on Linux/macOS and over SSH.

### Claude Agent SDK / Headless `claude -p` / LangGraph (code-orchestrated agents)

**Pros**
- Fine-grained programmatic control: prompts, tools, permissions, structured (JSON) results.
- Orchestration logic (branching, loops, state graphs) is code that can be versioned and tested.
- Cross-platform; easy to embed in services and CI.

**Cons**
- Process management, logging, timeouts, retries, and recovery must be written by hand.
- No built-in live view of running agents; observability is whatever you build.
- Workers usually run in-process or headless, so humans cannot attach and steer them interactively.
- Tied to one SDK/vendor; arbitrary third-party CLI agents need custom wrappers.

**vs. Hancho:** The SDKs build orchestration into an application; Hancho runs existing CLI agents and tools as observable OS processes without new code. They combine well: start SDK- or `claude -p`-based scripts as Hancho jobs to gain dependencies, retries, persistence, and the dashboard.
