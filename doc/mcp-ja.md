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
```

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
