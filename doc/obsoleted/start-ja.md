# スタート

**[English](start.md) | [日本語 (Japanese)](start-ja.md)**

```powershell
hancho-cli.exe mcp install --client opencode   # Claude Code は --client claude（.mcp.json）
hancho-cli.exe help --json                     # 機械可読ガイド（agent.quick_start）
```

- **1 ジョブ = 1 コマンド。** 複数ステップや依存は `taskqueue_add` の `dependsOn`。シェルの `&&` / `||` / `&`、PowerShell の `;`、待ちループは拒否されます。
- **タイムアウトはジョブごと**（`timeoutSec`）。合算しません。1 行の `summary` も必須です。
- **成功は検証**されます（exit code + 任意の `filesNewer` / `outputRegex`）。
- **状態**: `waiting` = キュー済みで実行待ち、`pending` = 依存未達で未キュー、`blocked` = 依存の失敗。`taskqueue_wait` / `taskqueue_graph` で観測し、原因を直して `taskqueue_retry` で復帰します。
- 詳細は `hancho-cli.exe help --json` の `agent`（`working_model` / `data_dir_consistency` / `decision` / `states` / `quick_start`）を参照してください。

作業構造: **監督 LLM（計画・宣言・観測）→ Hancho（実行・タイムアウト・ログ・再実行）→ 子プロセスのワーカー LLM / ビルドコマンド（1 ジョブ 1 コマンド）**。監督は作業自体を実行せず、コマンドを連鎖せず、成功を偽装しません。

## ビルド方法

Windows 10/11 (x64)、Visual Studio 2022 (C++17)、CMake 3.20 以降が必要です。

```powershell
cmake -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
```

ビルド成果物:
- `build/Release/Hancho.exe` — GUI ダッシュボード、Named Pipe IPC サーバー、MCP stdio サーバー（`conpty.dll` / `OpenConsole.exe` と同梱）
- `build/Release/hancho-cli.exe` — コンソール CLI クライアント

## 実行ファイル: `Hancho.exe` と `hancho-cli.exe`

| 実行ファイル | 役割 | 主なコマンド |
| :--- | :--- | :--- |
| `Hancho.exe` | GUI ダッシュボード、Named Pipe IPC サーバー、MCP stdio サーバー。すべての ConPTY プロセスを所有。 | `Hancho.exe gui`<br>`Hancho.exe mcp`<br>`Hancho.exe help` |
| `hancho-cli.exe` | ダッシュボードへ要求を送るコンソールクライアント。独自 GUI なし。 | `hancho-cli.exe list`<br>`hancho-cli.exe start -- cmd.exe`<br>`hancho-cli.exe help --json` |

- **`hancho-cli.exe`** を推奨します。ダッシュボードが未起動なら同じフォルダの `Hancho.exe` を自動起動します。**両方の exe を同じフォルダに置いてください。**
- 旧形式 `Hancho.exe <command>` も同じ処理に転送されます（互換用）。
- `hancho-cli.exe help --json` が機械可読な正規ガイドです（`concepts`, `data_dir_option`, `effective_data_dir`, `commands`, `setup.mcp.auto_install`, `agent` など）。
