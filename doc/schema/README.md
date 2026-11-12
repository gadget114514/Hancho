# Hancho Event Schema (`hancho.event/1`)

**[English](README.md) | [日本語 (Japanese)](README-ja.md)**

This directory contains the canonical JSON Schema for Hancho's normalized event stream format: `hancho.event.v1.json`.

Normalized events are written line-by-line (JSON Lines / `.events.jsonl`) to `logs/proc_<id>_<name>.events.jsonl` by `LlmStreamTracker` and exposed via MCP (`get_worker_stream`) and CLI (`hancho-cli worker stream`).

---

## 1. Strictly Typed `data` by Event Kind

As decided in `doc/design-v3.md` §7 (2026-11-11, task card `071-schema-data-types.md`), the schema defines `data` strictly per `kind`.

Previously, `additionalProperties` was unspecified and defaulted to `true`, which allowed arbitrary unknown fields to pass silently. In the tightened schema:
- Every `kind` definition specifies `"additionalProperties": false` on its `data` schema.
- Unrecognized or misspelled fields inside `data` are rejected.
- `plan` events constrain `items[].status` to a closed enum (`pending`, `in_progress`, `completed`, `cancelled`) and reject unknown fields on item objects (`additionalProperties: false`).

### Closed Event Kinds and `data` Schemas

| `kind` | Description | Required fields | Optional fields | `additionalProperties` |
|---|---|---|---|---|
| `run.started` | Agent run startup | — | `cwd`, `toolsOffered`, `permissionMode` | `false` |
| `turn.started` | Single model turn startup | — | `messageId` | `false` |
| `tool.call` | Tool invocation | `tool`, `activity` | `toolUseId`, `target`, `input`, `note` | `false` |
| `tool.result` | Tool execution result | `ok` | `toolUseId`, `error`, `summary` | `false` |
| `thought` | Agent internal reasoning | `text` | `truncated` | `false` |
| `message` | Agent assistant response text | `text` | `truncated` | `false` |
| `plan` | Structured todo / plan update | `items` | — | `false` |
| `usage` | Token and cost statistics | At least one token count (`inputTokens`, `outputTokens`, `thinkingTokens`, `cacheReadTokens`, `cacheWriteTokens`, `totalTokens`) | `costUsd`, `cumulative` | `false` |
| `rate_limit` | Rate limit notice | `status` | `window`, `utilization`, `resetsAt` | `false` |
| `subagent.started` | Sub-agent spawned | `agentId` | `type`, `description`, `toolUseId` | `false` |
| `subagent.ended` | Sub-agent finished | `agentId`, `status` | `tokens`, `toolUses`, `durationMs` | `false` |
| `run.ended` | Agent run completed | `status` (`completed`, `error`, `cancelled`) | `stopReason`, `numTurns`, `durationMs`, `permissionDenials` | `false` |
| `error` | Agent error report | `message` | `code`, `retryable` | `false` |
| `raw` | Unparsed or unclassified line | `text` | — | `false` |

---

## 2. Free-Form Exceptions (例外一覧とその理由)

To keep the system extensible without allowing arbitrary drift, all exceptions to strict typing are cataloged here:

### 1. `tool.call` -> `data.input`

- **Schema definition**: `{"type": ["object", "string"], "description": "..."}`
- **Why it is free-form**: Tool parameters are defined entirely by the tool being invoked (e.g. `Edit` uses `{file_path, old_string, new_string}`, `Bash` uses `{command}`, custom MCP tools use domain-specific arguments). Because Hancho is an out-of-process control plane that supports arbitrary CLI agents and extensible toolsets, imposing fixed property schemas on `input` would prevent agents from using new or custom tools.
- **Enforced bounds**: While internal properties are unconstrained, `input` must be a JSON object or string (raw/unparsed arguments); primitive types like booleans, numbers, or null are rejected.

No other properties in `data` may remain free-form. If a new free-form exception is required in the future, it must be documented in this section with an architectural rationale.

---

## 3. Outer Envelope Compatibility Rules

1. **Outer fields**: The top-level `hancho.event/1` object and metadata objects (`source`, `agent`, `raw`) allow minor field additions within v1 (`additionalProperties: true`). Downstream consumers must ignore unknown top-level or metadata fields.
2. **Breaking changes**: Modifying the meaning of existing fields or removing them requires upgrading the schema identifier to `hancho.event/2`.
3. **Unknown kind**: Consumers encountering an unrecognized `kind` must treat the event as `raw`.
