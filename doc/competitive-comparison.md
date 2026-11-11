# Hancho: Competitive Comparison

**[English](competitive-comparison.md) | [日本語 (Japanese)](competitive-comparison-ja.md)**

Hancho is not an agent framework. It is an out-of-process control plane for CLI agents: it runs existing CLIs as OS processes and keeps their durable work state. Most tools below solve a different layer, so this is a comparison of fit rather than a single winner.

Back to [README](../README.md). See also [Pros/Cons](pros-cons-comparison.md) and [Critical Comparison](critical-comparison.md).

---

## 1. Where each tool sits

| | Category | Execution model | Strengths | Weaknesses | Relationship to Hancho |
|---|---|---|---|---|---|
| **Hancho** (this project) | CLI process control plane | OS processes (pipe / ConPTY), task queue, behavior trees | Durable jobs, timeouts, retries, resource locks, logs, `.done`, GUI, heterogeneous CLI engines, failure analysis, cross-session memory | Windows-first (POSIX / remote are early), no LLM context management, no bundled RAG / vector DB, small ecosystem | — |
| **LangChain / LangGraph** | Agent / orchestration framework | In-process application code, graph or chain | Large integration ecosystem, RAG, memory abstractions, graph checkpoints, tracing (LangSmith) | Process supervision and arbitrary CLI control are left to you; in-process; SDK / vendor coupling | Complementary. Run LangGraph-based scripts as Hancho jobs when they need durability, retries and a dashboard |
| **AutoGen** | Multi-agent framework | In-process conversational agents, event-driven | Multi-agent conversations, tool use, flexible agent roles | No OS process supervision, durable queue or GUI for arbitrary CLIs | Complementary. Start an AutoGen runner as a Hancho job |
| **CrewAI** | Multi-agent framework | In-process crews and flows | Fast to express roles, tasks and flows | Weak on arbitrary CLI supervision, restart durability and operational visibility | Complementary. Supervise a crew run with Hancho |
| **Claude Agent SDK / OpenAI Agents SDK** | Vendor agent SDK | In-process / headless agent | Tools, handoffs, guardrails, tracing, typed control | Vendor-specific; process management, logging and retries are your code | Complementary. Run SDK scripts as supervised jobs |
| **Mem0 / Letta / Zep** | Memory services / libraries | Service or library next to the agent | Extraction, summarization, vector / graph retrieval, user and session memory | Do not orchestrate work; extraction is LLM-based and non-deterministic; no OS run state | Complementary. Use for rich semantic retrieval; Hancho keeps deterministic run facts and human-approved knowledge |

---

## 2. Memory compared

| | Hancho | LangChain-style hierarchical memory | Mem0 / Letta / Zep |
|---|---|---|---|
| Unit | **Attempt (L1) / fact (L2, L3)** | Conversation, message, summary | User, session, fact |
| Durability | Across processes, sessions, restarts and engines | Inside one application; persistence is wired by hand | Service-managed, persistent |
| Determinism | L1 is deterministic from events; L2/L3 are explicit and auditable | Summaries and retrieval are lossy and non-deterministic | Extraction depends on an LLM and is non-deterministic |
| Human approval | **L3 promotion is human-only** | None | None or limited |
| Retrieval | BM25-like keyword search; embeddings are an optional `IMemoryRanker` boundary | Vector / summary retrieval inside the loop | Vector and graph retrieval |
| Strength | Tied to what actually ran; auditable | Keeps a long conversation inside the context window | Strong semantic search and extraction |
| Weakness | No bundled RAG / embeddings | Cannot see other processes or sessions | Unrelated to execution control |

Hancho's memory is **work memory**, not a replacement for conversation memory or document RAG. When document retrieval is the goal, pair Hancho with LangChain, LlamaIndex, Mem0 or a similar tool and run it as a job.

---

## 3. When to use which

- **Hancho**: several CLI agents or plain commands, long-running work that must survive restarts, OS-level timeouts and process-tree control, a dashboard, failure classification, cross-session facts and human-approved knowledge.
- **LangChain / LangGraph**: an agent or RAG pipeline built inside your application, with graph orchestration, integrations and tracing.
- **AutoGen / CrewAI**: role-based multi-agent conversations and flows inside one application.
- **Claude / OpenAI Agent SDK**: a vendor-integrated agent with tools, handoffs and guardrails.
- **Mem0 / Letta / Zep**: high-quality extracted or semantic memory next to the agent.

The combination that fits Hancho best is: **Hancho supervises execution; a framework builds the agent; a memory service supplies rich retrieval.** Hancho stays useful as long as you need durable, inspectable work that outlives one process or one conversation.
