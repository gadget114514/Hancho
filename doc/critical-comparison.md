# Hancho: A Critical Comparison

A deliberately skeptical look at Hancho: what it is, what it is not, where it is weaker than the alternatives, and when not to use it. For the favorable case, see [Pros/Cons](pros-cons-comparison.md).

Back to [README](../README.md).

---

## What Hancho Is, Stripped Down

Hancho is not an LLM harness and not an agent. It is an **out-of-process control plane for CLI agents**: it starts processes, enforces timeouts, checks success, retries, orders dependencies, walks behavior trees, and shows the results on a dashboard. The agent loop — model calls, prompt assembly, tools, permissions, context — stays inside whichever CLI it launches.

That framing is also the main criticism: Hancho controls only the shell around the work, so its guarantees stop at the process boundary. It cannot see why a worker chose a step, cannot enforce how the worker uses its context, and cannot substitute for a harness.

## Critical Weaknesses

### 1. A control plane with no brain

- Every job needs a harness. Without `claude`, `opencode`, `codex` or another CLI, Hancho can only run plain commands.
- It adds a layer rather than removing one: the user now operates a harness **and** an orchestrator.
- Any decision that needs judgment must go back to the LLM through `escalate`, which reintroduces turns and tokens.
- Its control flow is deterministic only for the parts you can write down in advance.

### 2. Windows-only, desktop-only

- Built on Win32, ConPTY, Job Objects and Named Pipes. There is no Linux/macOS/SSH story comparable to tmux.
- One machine, local processes. No distributed workers, cloud scaling or CI-native mode.
- Installation, a data directory and MCP configuration are required per project.

### 3. Opaque workers

- The supervisor sees captured stream events, not the worker's reasoning.
- Rich tracking exists only for supported stream formats (`claude`, `agy`, `opencode`, `codex`). Any other tool degrades to raw logs.
- Success, retries and timeouts are only as good as the explicit command, timeout and success check the user writes.

### 4. Rigid where it matters

- Queues and behavior trees express plans you can write down. Exploratory or ambiguous work still needs the LLM in the loop.
- Authoring queue specs and behavior-tree JSON is a new skill and a new failure surface: wrong conditions, loops, timeouts and dependency mistakes.
- Two sources of truth: Hancho's job/BT state and each agent's own session state. Reconciling them is manual.

### 5. Duplicated and commoditized

- Harnesses keep absorbing the features Hancho offers: background execution, sub-agents, hooks, MCP, worktrees, agent teams and resumable sessions.
- SDKs and cloud orchestrators cover the programmatic and CI side.
- Hancho sits between them. Its niche is narrow: many heterogeneous CLI agents on one Windows desktop, running long and watched by a human.

### 6. Not memory, not a sandbox, not a cluster scheduler

- Durable job state is not semantic memory. It does not solve long-conversation context.
- It launches arbitrary commands; permissions and sandboxing remain each CLI's responsibility.
- Resource locks and concurrency are single-machine coordination, not cluster scheduling.

### 7. The economics are situational

- Token savings are no longer the reason to use it; sub-agents cost about the same.
- The payoff is durability, observability and heterogeneity, which only matter for long, parallel, multi-engine or human-watched work.
- For a few short tasks, the setup and mental overhead is a net loss.

## Honest Positioning

| Need | Better fit than Hancho |
|---|---|
| One long conversation that remembers | Harness plus hierarchical memory |
| Short reasoning or code edits | Native sub-agents |
| Linux/macOS or remote SSH work | tmux / claude-squad |
| Programmatic, versioned orchestration | Agent SDK / LangGraph |
| Cloud or CI parallel jobs | CI or cloud orchestrator |
| Many heterogeneous CLI agents on Windows, watched and durable | Hancho |

## When Not to Use Hancho

- Your work fits in one session and one model.
- You need cross-platform or remote execution.
- You cannot express the job as a command with a timeout and a success check.
- You want the orchestrator to understand or judge the agent's reasoning.
- You need real sandboxing or multi-tenant isolation.

## What Would Make It Stronger

- First-class adapters and a stable event schema for arbitrary CLIs.
- A cross-platform core or a remote/headless mode.
- Richer, semantic memory on top of durable job state.
- Better failure analysis: why a job failed, not just that it did.
- Clear guidance on when a harness-native feature has made Hancho unnecessary.

The detailed design for these is in [Hancho v3 Design](design-v3-en.md).
