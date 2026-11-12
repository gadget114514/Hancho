# 使い方 2: Claude の MCP サーバーとして登録する

**[English](mcp.md) | [日本語 (Japanese)](mcp-ja.md)**

`.mcp.json`（Claude Code 用）または `claude_desktop_config.json` に以下を追加すると、Claude から専用ツールとして直接呼び出せます。

```json
{
  "mcpServers": {
    "hancho": {
      "command": "C:/wsgit/Hancho/build/Release/Hancho.exe",
      "args": ["mcp"]
    }
  }
}
```

`hancho-cli.exe` に設定を書かせることもできます（既存設定にマージ、冪等）:

```powershell
hancho-cli.exe mcp install                              # .mcp.json (Claude Code)
hancho-cli.exe mcp install --client opencode            # opencode.json
hancho-cli.exe mcp install --data-dir .hancho # プロジェクト内データも一緒に
hancho-cli.exe mcp install --dry-run --json             # 書き込まずに確認
hancho-cli.exe mcp uninstall                             # 項目を外す（データは残す）
hancho-cli.exe mcp uninstall --delete-data-dir           # データディレクトリも消す
```

`mcp uninstall` は `mcp install` の逆で、Hancho の項目だけを外し、ほかのサーバー設定は残します。データは既定で残り、すべて JSON / JSONL のまま Hancho なしで読めます。やめる判断の基準は [design-v3.md 5.3〜5.5](design-v3.md) を参照してください。

## 提供される MCP ツール

**プロセス操作**

| ツール | 説明 |
| :--- | :--- |
| `start_process` | 1 コマンドを起動（`cmd`, `name`, `mode` [`pty`\|`pipe`\|`console`], `pty`, `open_stdin`, `cwd`, `log_file`, `done_file`, `timeout_sec`, `idle_timeout_sec`, `estimate_end`, `wait`, `wait_ms`）。連鎖コマンドは拒否されます。 |
| `list_processes` | 管理プロセスの一覧（`id` 任意）。 |
| `get_process_output` | 出力を取得（`id`, `tail`, `wait_ms`）。 |
| `send_process_input` | stdin / ConPTY へ入力（`id`, `input`, `newline`, `wait_ms`）。 |
| `wait_process` | 終了待機（`id`, `wait_ms`）。 |
| `kill_process` | プロセスツリーを強制終了（`id`; `0` で全実行中）。タスクキュー由来の試行は killed として再実行判定されます。 |

**タスクキュー操作**

| ツール | 説明 |
| :--- | :--- |
| `taskqueue_add` | ジョブ登録（`taskqueue`, `jobs[]`, `concurrency?`, `notifyFile?`）。各ジョブに `id`, `command`, `summary`, `timeoutSec` が必要。 |
| `taskqueue_list` | ジョブ一覧（`taskqueue?`, `state?`）。 |
| `taskqueue_get` | ジョブ詳細・試行履歴・指示書（`id`, `includeInstructions?`, `rev?`）。 |
| `taskqueue_retry` | failed/blocked/cancelled を再実行（`id`, `resetAttempts?`, `refreshInstructions?`）。blocked の後続も再キュー。一括: `id` を省いて `failed: true` にすると失敗ジョブをすべて再実行（`taskqueue`, `since`（例: `6h`）で絞り込み可）。 |
| `taskqueue_update` | pending/failed/blocked のジョブを更新。 |
| `taskqueue_cancel` | ジョブをキャンセル（実行中は kill、再実行しない）。 |
| `taskqueue_pause` / `taskqueue_resume` | キューの新規開始を停止 / 再開。 |
| `taskqueue_wait` | キューの version 変化をロングポーリング（`taskqueue`, `sinceVersion`, `timeoutMs`）。 |
| `taskqueue_purge` | 終了済みジョブを削除（`taskqueue`, `states[]`）。 |
| `taskqueue_link` / `taskqueue_unlink` | 依存を 1 本追加 / 削除（`job`, `dependsOn`, `on` = `success`\|`done`）。 |
| `taskqueue_graph` | 依存グラフ（`ready` / `waiting_for` / `dependents`）。 |

**メモリ操作**

| ツール | 説明 |
| :--- | :--- |
| `memory_search` | L1 実行記録・L2 ノート・L3 知識の BM25 風検索（`query`, `kinds?`, `scope?`, `file?`, `jobName?`, `taskqueue?`, `engine?`, `model?`, `outcome?`, `failureClass?`, `since?`, `limit?`）。 |
| `memory_get` | 1 件の全文と出典（`id`, `kind?`）。 |
| `memory_brief` | ジョブに関係する記録を固定形式で要約（`jobName?`, `taskqueue?`, `file?`, `files?`, `scopes?`, `budgetChars?`, `includeAgentNotes?`, `maxPreviousRuns?`）。前回の実行 / よくある失敗 / 関連ノート / プロジェクト知識を、各行に id 付きで返す。 |
| `memory_stats` | L1 の集計（成功率、所要時間の中央値・p90、失敗分類、トークン数と費用。ジョブ・エンジン・モデル別）。 |
| `memory_approve` | L2 ノートを L3 プロジェクト知識へ昇格（`id`, `worker?`, `by?`）。`config.json` の `memory.l3Promoters` に指定された許可ワーカーのみ実行可能。未設定または未許可の場合は拒否。 |

**LLM 使用量集計**

| ツール | 説明 |
| :--- | :--- |
| `usage_stats` | 日次ロールアップログから集計した LLM トークン使用量と推定費用を取得（`since?`, `until?`, `groupBy?`: `day`\|`month`\|`engine`\|`model`）。 |
| `usage_export` | 集計した LLM 使用量を CSV または JSON で書き出し（`format?`: `csv`\|`json`, `since?`, `until?`, `groupBy?`, `out?`）。 |

L3 のプロジェクト知識への昇格は人（`hancho-cli memory approve <id>` または GUI）に加えて、`config.json` の `memory.l3Promoters` に列挙された特定の許可ワーカーのみが MCP の `memory_approve` を通じて行えます。未許可のエージェントは拒否されます。
