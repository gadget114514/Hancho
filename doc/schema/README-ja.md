# Hancho イベントスキーマ (`hancho.event/1`)

**[English](README.md) | [日本語 (Japanese)](README-ja.md)**

このディレクトリには、Hancho の正規化イベントストリーム形式（`hancho.event.v1.json`）の JSON Schema を配置しています。

正規化されたイベントは `LlmStreamTracker` により `logs/proc_<id>_<name>.events.jsonl` へ 1 行 1 JSON オブジェクト（JSON Lines）で追記され、MCP（`get_worker_stream`）および CLI（`hancho-cli worker stream`）を通じて取得できます。

---

## 1. 種別ごとの `data` の型拘束

`doc/design-v3.md` §7 の決定（2026-11-11、課題カード `071-schema-data-types.md`）に基づき、イベントスキーマの `data` は種別（`kind`）ごとに厳格に型付けされています。

従前は `additionalProperties` が既定（許可）だったため、未知のフィールドが検証をすり抜ける状態でした。改定後のスキーマでは以下の通り締めています:
- 全 14 種別の `data` 定義において `"additionalProperties": false` を明示し、未知のフィールドを拒否します。
- `plan` イベントでは、`items[].status` を閉じた列挙（`pending`, `in_progress`, `completed`, `cancelled`）に制限し、配列要素内の未知のフィールドも拒否します（`additionalProperties: false`）。

### 種別一覧と `data` のスキーマ

| `kind` | 意味 | 必須フィールド | 任意フィールド | `additionalProperties` |
|---|---|---|---|---|
| `run.started` | エージェント実行開始 | — | `cwd`, `toolsOffered`, `permissionMode` | `false` |
| `turn.started` | 単一ターン開始 | — | `messageId` | `false` |
| `tool.call` | ツール呼び出し | `tool`, `activity` | `toolUseId`, `target`, `input`, `note` | `false` |
| `tool.result` | ツール実行結果 | `ok` | `toolUseId`, `error`, `summary` | `false` |
| `thought` | エージェント内部思考 | `text` | `truncated` | `false` |
| `message` | 回答テキスト | `text` | `truncated` | `false` |
| `plan` | 計画・TODO更新 | `items` | — | `false` |
| `usage` | トークン・コスト統計 | トークン数のうち最低 1 つ（`inputTokens`, `outputTokens`, `thinkingTokens`, `cacheReadTokens`, `cacheWriteTokens`, `totalTokens`） | `costUsd`, `cumulative` | `false` |
| `rate_limit` | レート制限通知 | `status` | `window`, `utilization`, `resetsAt` | `false` |
| `subagent.started` | サブエージェント起動 | `agentId` | `type`, `description`, `toolUseId` | `false` |
| `subagent.ended` | サブエージェント終了 | `agentId`, `status` | `tokens`, `toolUses`, `durationMs` | `false` |
| `run.ended` | 実行終了 | `status`（`completed`, `error`, `cancelled`） | `stopReason`, `numTurns`, `durationMs`, `permissionDenials` | `false` |
| `error` | エラー報告 | `message` | `code`, `retryable` | `false` |
| `raw` | 未分類行・生ログ | `text` | — | `false` |

---

## 2. 自由形式の例外一覧とその理由

スキーマの拡張性を担保しつつ緩やかなドリフトを防ぐため、自由形式を残す例外箇所とその理由を以下にまとめます。

### 1. `tool.call` の `data.input`

- **スキーマ定義**: `{"type": ["object", "string"], "description": "..."}`
- **自由形式とする理由**: ツールの引数は呼び出されるツールごとに全く異なります（`Edit` は `{file_path, old_string, new_string}`、`Bash` は `{command}`、独自 MCP ツールは独自の引数など）。Hancho はプロセス外の制御プレーンであり、個別のツール定義を規定しないため、`input` の内部構造を固定すると新しいツールや独自ツールの利用が妨げられます。
- **制約範囲**: 内部のキー名は自由ですが、空スキーマ（`{}`）を廃止し、JSON オブジェクトまたは生文字列（`type: ["object", "string"]`）に限定しています。数値や真偽値などの無関係なプリミティブ型は拒否されます。

これ以外の `data` フィールドには自由形式の例外を認めません。将来例外を追加・変更する場合は、設計上の理由とともに本節へ追記します。

---

## 3. 外側のエンベロープ互換規則

1. **外側フィールドの追加**: トップレベルの `hancho.event/1` オブジェクトおよびメタデータ（`source`, `agent`, `raw`）はマイナー拡張を許容します（`additionalProperties: true`）。利用側は知らない外側フィールドを無視します。
2. **破壊的変更**: 既存フィールドの意味の変更や削除を行う場合は、スキーマ識別子を `hancho.event/2` へ昇格します。
3. **未知の種別**: 利用側は知らない `kind` を受け取った場合、`raw` として扱います。
