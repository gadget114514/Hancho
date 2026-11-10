# LLM JSON Stream Format (hancho-cli -> Hancho)

Hancho (`hancho-cli.exe` / Hancho) can parse JSON Lines (JSONL) emitted by LLM coding agents and record structured events to `<logfile>.stream.jsonl` and the live stream view.

## Enabling

- Automatic: when launching via `hancho-cli start` and `agent_type` is detected (`claude`, `agy`/`antigravity`, `opencode`, `codex`, `aider`, `ollama`) or `json_stream` is true. `StartProcessRequest.jsonStream`/`--json-stream` may also be used.
- The parser is in `src/llm_stream.cpp` / `src/llm_stream.h` (`ParseLlmStreamLine`, `LlmStreamTracker`).
- Detected agent types: `DetectAgentType(commandLine, name)` looks for `claude`, `agy`/`antigravity`, `opencode`, `codex`, `aider`, `ollama`.

## Parsed LLM Stream Event (`LlmStreamEvent`)

| Field | Type | Notes |
|---|---|---|
| `seq` | uint64 | Monotonic sequence number assigned by tracker |
| `timestamp` | string | ISO-like/time string; filled by tracker if missing |
| `rawJson` | string | Original trimmed JSON line |
| `type` | string | Normalized: `tool_call`, `tool_result`, `thinking`, `message`, `status`, `step`, `error`, `usage`, `other`, or fallback to raw `type` |
| `agentType` | string | Agent type from event or default (`claude`/`agy`/`opencode`/`codex`/`generic`) |
| `model` | string | Model id/name if present in stream (see below) |
| `toolName` | string | Tool name (e.g. `view_file`, `edit_file`, `Bash`, Claude `tool_calls[].name`/`function.name`) |
| `toolAction` | string | Human-readable action/summary |
| `toolInput` | string | JSON string of arguments/parameters/input |
| `content` | string | Thinking/reasoning/text content |
| `status` | string | `thinking`, `calling_tool`, `generating`, `idle`, `completed`, `error`, etc. |
| `stepIndex` | int | From `step_index` or `step` |
| `totalTokens` | uint64 | If present |
| `costUsd` | double | If present |

## Agent State (`LlmAgentState`)

| Field | Type | Notes |
|---|---|---|
| `agentType` | string | Current agent type |
| `model` | string | Current model if observed in stream |
| `status` | string | Aggregated status (`idle`/`thinking`/`calling_tool`/`tool_finished`/`generating`/`completed`/`error`) |
| `currentTool`/`currentToolAction` | string | Active tool info |
| `lastThought`/`lastMessage`/`lastError` | string | Truncated previews |
| `stepCount` | int | Latest step index |
| `eventCount` | int | Count of stored events |
| `totalTokens`/`costUsd` | ... | Aggregated if present |
| `streamFilePath` | string | Path to `.stream.jsonl` |

## Recognized JSON Shapes

The parser is permissive and looks for common fields across agents:

### Agent/model hints
- `agent` (string)
- `model`, `modelId`, `model_name`
- `llm.model` / `llm.name`
- `metadata.model`

### Tool calls
- Claude-style: top-level `tool_calls` array; or `type` like `tool_use`/`tool_call`/`function_call`. Names from `toolName`/`name`/`tool`/`function.name`; inputs from `parameters`/`arguments`/`input`.
- Some streams use `PLANNER_RESPONSE` with `tool_calls`.

### Tool results
- `type` in `tool_result`/`function_result`/`tool_response`; content from `content`/`output`/`result`.

### Thinking/reasoning
- `type` in `thinking`/`thought`/`reasoning` or fields `thinking`/`thought`/`reasoning`.

### Messages/deltas
- `type` in `message`/`assistant`/`user`/`content_block_delta`/`delta`; content from `content`/`text`/`message`/`delta.content`/`delta.text`.

### Status/error/result
- `type` in `status`/`error`/`result`/`done`/`finish`/`completed`. `error` may be string or object with `message`.

### Claude partial messages (`--include-partial-messages`)
- `stream_event` lines are transient: they update the live state (status, last thought/message, current tool, activity) and count usage, but they are not stored in the event list or saved to `<stream>.jsonl`.
- `message_delta` carries the final usage of a message. Usage is counted once per `message.id`, and the `result` line's usage fills in whatever the messages did not report.
- `system` lines other than `init` (status, thinking_tokens, task_summary, ...) are transient too.
- `thinking_tokens` in `output_tokens_details` are read as thinking tokens. Claude's output tokens already include them, so they are not added to the total again.

### Claude sub-agents (Agent tool)
- `system` / `task_started`: `task_id`, `tool_use_id`, `description`, `subagent_type` become a `subagent_start` event (`agent_id` = `task_id`, `tool_use_id` = the Agent call).
- `system` / `task_notification`: `task_id`, `tool_use_id`, `status`, `summary`, `usage` (`total_tokens`, `tool_uses`, `duration_ms`) become a `subagent_end` event.
- A sub-agent's `assistant` / `user` lines carry `parent_tool_use_id` and `agent_id`. They update only that sub-agent. Their status, activity, tool and message fields do not replace the main agent's.
- Token usage is counted once per message id, so lines of other agents between one message's blocks do not count it twice.
- A `result` line that arrives while a sub-agent is still running leaves the run `running` (activity `subagent`) until that sub-agent ends.
- Events carry `tool_use_id`, `parent_tool_use_id`, `agent_id`, `subagent_type`, `task_description`, `task_status` when present. The agent state carries a `subagents` array (`id`, `tool_use_id`, `parent_agent_id`, `subagent_type`, `description`, `status`, `started_epoch_ms`, `ended_epoch_ms`, `total_tokens`, `tool_uses`, `activity`, `activity_target`). Nested sub-agents are supported by `parent_agent_id`; the nested case is tested with synthetic lines only.

## Outputs

- **Per-process stream file**: `<logfile>.stream.jsonl` (written by `LlmStreamTracker` as raw JSON lines are ingested).
- **Normalized event file**: `<logfile>.events.jsonl` (one `hancho.event/1` object per line).
- **GUI**: Stream dialog shows events list and preview; agent icon shows tooltip with model when known.
- **IPC/MCP**: `GetAgentStream`/IPC and MCP endpoints can expose events/state (see `ipc_server.cpp`, `mcp_server.cpp`).

## Legacy `type` strings and migration to `hancho.event/1`

**Decision: `hancho.event/1` is the default schema for both entry points.**

- `hancho-cli worker stream <id>` returns `hancho.event/1` from `<log>.events.jsonl` unless `--schema legacy` is passed.
- MCP `get_worker_stream` returns `hancho.event/1` unless `schema: "legacy"` is passed.

The legacy shape is the one built from the internal `LlmStreamEvent.type` strings. It is **deprecated as of v0.9.3** and stays available for at least one more release:

| Version | `--schema legacy` / `schema: "legacy"` |
|---|---|
| v0.9.3 – v0.10.x | available; returns the old `type` strings |
| v0.11.0 and later | removed; only `hancho.event/1` is returned |

The raw `.stream.jsonl` file is still written after the legacy view is removed, so no captured data is lost.

### Migration steps

1. Drop `--schema legacy` from CLI calls and `schema: "legacy"` from MCP calls. The v1 shape is the default.
2. Read `kind` instead of `type`, and read the event payload from `data` instead of the old top-level fields.
3. Change `--type` / MCP `type` filters from legacy type names to v1 kind names. With v1 the filter matches `kind` (for example `tool.call`, not `tool_call`).
4. Treat an unknown `kind` as `raw`; new kinds may be added within the same schema version.
5. Keep using `.stream.jsonl` when the original CLI lines are needed; v1 events point back to them through `raw.file` and `raw.line`.
6. `hancho-cli worker stream --summary` still uses the legacy shape through v0.10.x; pass `--schema legacy` with it, or read the v1 events directly.

Logs written before v0.9.2 have no `.events.jsonl`; use `--schema legacy` to read them from `.stream.jsonl` until v0.11.0.

### `type` to `kind` mapping

| Legacy `type` | v1 `kind` | Main `data` fields |
|---|---|---|
| `tool_call` | `tool.call` (or `plan` for `TodoWrite` / plan activity) | `tool`, `toolUseId`, `activity`, `target`, `input` |
| `tool_result` | `tool.result` | `ok`, `toolUseId`, `error`, `summary` |
| `thinking` | `thought` | `text`, `truncated` |
| `message` | `message` | `text`, `truncated` |
| `usage` | `usage` | `inputTokens`, `outputTokens`, `thinkingTokens`, `cacheReadTokens`, `cacheWriteTokens`, `totalTokens`, `costUsd` |
| `rate_limit` | `rate_limit` | `status`, `window`, `utilization`, `resetsAt` |
| `subagent_start` | `subagent.started` | `agentId`, `type`, `description`, `toolUseId` |
| `subagent_end` | `subagent.ended` | `agentId`, `status`, `tokens`, `toolUses`, `durationMs` |
| `result` | `run.ended` | `status`, `stopReason`, `numTurns`, `durationMs`, `permissionDenials` |
| `status` with `content: "init"` | `run.started` | `cwd`, `toolsOffered`, `permissionMode` |
| `status` (other) / `step` start | `turn.started` | `messageId` |
| `step` with tokens or cost | `usage` | token counts, `costUsd` |
| `error` | `error` | `message`, `code`, `retryable` |
| anything else (for example `user_input`) | `raw` | `text` |

Outer fields also move: `seq` stays `seq`; `timestamp` becomes `ts`; `agentType`, `model`, `sessionId`, `agentId` and `parentAgentId` move under `agent`; process/job/attempt/BT details move under `source`; the source line moves under `raw`.

## Notes

- Non-JSON lines are ignored by the parser.
- The tracker caps stored events (`kMaxEvents`) and persists raw lines for post-mortem.
- Model capture is best-effort from stream content; Hancho cannot reliably infer the LLM model from process metadata alone.

## Per-agent differences (as implemented)

The parser normalizes many variants; notable mappings below:

| Agent | Typical shapes seen/handled | Notes |
|---|---|---|
| Claude Code / Claude API style | `type: "thought"`/`"thinking"`, `type: "tool_call"` or `tool_calls[]` with `function.name`/`name` and `arguments`/`input`/`parameters`; `type: "tool_result"`/tool response; streaming deltas `content_block_delta` with `delta.type/content`. | Top-level `tool_calls` array or nested `tool_call`. `agent`/`model` may appear. |
| OpenCode | `type: "tool_use"`, `tool_call`, or objects with `tool`/`name`; messages/deltas; sometimes `PLANNER_RESPONSE` with `tool_calls`. | Detected by name/cmd containing `opencode`. Model often in `model`/`llm.model`. |
| Aider | JSON/tool events vary; parser looks for generic `tool_*`, `thinking`/`thought`, and message fields. | Detected by `aider`. Often less standardized; best-effort parsing. |
| Codex (OpenAI Codex CLI) | `reasoning`/`thought`/`thinking` style, tool calls/results in various forms; parser accepts `reasoning` as thinking and generic `tool_call`/`function_call`. | Detected by `codex`. |
| agy / Antigravity | Similar to Claude/opencode patterns; `type: "tool_call"`, `tool_use`, thinking fields. | Detected by `agy` or `antigravity`. |
| Ollama / local models | Varies; parser falls back to generic JSON with `content`/`message`/`text`. | Detected by `ollama`. May not emit structured tool events consistently. |

### Field mapping details
- **Tool name**: `toolName` ← `name`/`tool`/`tool_name` or `function.name` (from `tool_call`/`tool_calls[]`).
- **Tool input**: `toolInput` ← JSON string of `input`/`arguments`/`parameters`.
- **Tool action**: `toolAction` ← `toolAction`/`action`/`summary` or derived as `"Calling " + toolName`.
- **Thinking**: normalized to `type: "thinking"`, content from `thinking`/`thought`/`reasoning`/`message`/`content`.
- **Tool result**: `type: "tool_result"`, content from `content`/`output`/`result`.
- **Message/delta**: `type: "message"` (user → `user_input`), content from `content`/`message`/`text` or `delta.content`/`delta.text`.
- **Status/error/result**: mapped to `status`/`error`/`result` types; `result`/`done`/`finish`/`completed` → `type: "result"`, `status: "completed"`.

### Model detection
- Checked on event parse and state update: top-level `model`, `modelId`, `model_name`; `llm.model`/`llm.name`; `metadata.model`. Also captured from `result` events if present. Stored in `LlmStreamEvent.model` and `LlmAgentState.model`, surfaced in GUI tooltip.

## Known differences / notes
- **Schema varies by vendor/version**: Claude, OpenCode, Codex, Aider emit different JSON schemas; the parser is intentionally permissive and normalizes to a common `LlmStreamEvent`.
- **Streaming vs batch**: some emit deltas, others emit full messages/tool calls.
- **Model reporting**: not all agents include `model` in every event; availability depends on the agent/CLI version.
- **Tool naming**: names differ across agents (e.g. Claude tool names vs OpenCode internal names).
