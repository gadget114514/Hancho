## Hancho v0.10.0

### Changes
- **Task / direction / job / attempt model**: a new four-level hierarchy. A task records the intent, a direction is one queued plan (a behavior tree or a task card), and jobs and attempts record execution. Triage cards and task cards are the same direction viewed by state.
- **`direction` CLI and MCP tools**: add, list, show, edit, remove, run, redo, import, export, translate, close, reopen and purge cards, including Markdown import/export, dependency-ready dispatch (`direction run --ready`) and rescue cards (`direction redo`).
- **Modernized behavior tree nodes**: `exec` (one command, alias `job`) and `agent` (LLM worker, alias `worker`), matching the `run_agent` tool and `llm list --list agent`.
- **3-row KPI cards and KPI settings tab**: Process, Task and Job rows can be toggled independently. KPI card labels are now clearer, and the Origin filter counts real jobs only.
- **Routine management dialog**: the toolbar `ルーチン` button opens a dialog to inspect, run and check named behavior trees. A routine run is now one direction under a task: pass `--task <id>` (MCP `routine_run`: `task`) to use an existing task, or omit it to have Hancho create one automatically.
- **Task list**: `Collapse All` (`全部閉じる`) and stable expansion state across refreshes; task headings show start/end times, running details and origin.
- **CI / headless automation**: opt-in `run --ephemeral`, `run --bt`, `--wait-scope direction`, `--cancel-on-exit`, `--follow`, `--logs-dir`, `--report-output` and `HANCHO_AUTOSTART`.
- **Engine fallback and failure classification**: failed attempts switch to the next healthy approved engine; LLM errors are classified from HTTP status and error codes rather than log text.
- **Fixes**: behavior-tree glob expansion with `**`, routine dialog refresh handling, and test-suite fixes on `main`.

### 変更（日本語）
- **`task` → `direction` → `job` → `attempt` の 4 階層モデル**を導入しました。タスクは意図を記録し、direction はキューへの 1 回の登録（BT またはタスクカード）、job と attempt が実行を記録します。タスクカードとトリアージカードは同じ direction を状態で呼び分けたものです。
- **`direction` の CLI / MCP コマンド群**を追加しました。カードの追加・一覧・表示・編集・削除・実行・救済・Markdown インポート／エクスポート・翻訳・クローズ／再オープン・削除に対応し、依存が揃ったカードの順次実行（`direction run --ready`）や失敗カードの救済（`direction redo`）ができます。
- **BT ノード名を刷新**しました。`exec`（単一コマンド、別名 `job`）と `agent`（LLM ワーカー、別名 `worker`）に統一し、`run_agent` ツールや `llm list --list agent` と名前を揃えました。
- **3 行 KPI カードと KPI 設定タブ**を追加しました。プロセス・タスク・ジョブの各行を個別に表示切り替えでき、KPI のラベルを整理し、Origin 絞り込みの件数を実ジョブだけにしました。
- **ルーチン管理ダイアログ**を追加しました。ツールバーの「ルーチン」ボタンから名前付き BT の確認・実行・鮮度チェックができます。ルーチン実行は 1 つの direction としてタスクに載り、`--task <id>`（MCP `routine_run` の `task`）で既存タスクに追加し、省略時は Hancho がタスクを自動生成します。
- **タスク一覧**に「全部閉じる」と更新をまたいだ展開状態の保持を追加し、タスク見出しに開始／終了時刻、実行中詳細、Origin を表示するようにしました。
- **CI / ヘッドレス自動化**: `run --ephemeral`、`run --bt`、`--wait-scope direction`、`--cancel-on-exit`、`--follow`、`--logs-dir`、`--report-output`、`HANCHO_AUTOSTART` をオプトインで利用できます。
- **エンジンの切り替えと失敗分類**を改善しました。失敗時に次の健全な承認済みエンジンへ切り替え、LLM のエラーをログ文字列ではなく HTTP ステータスとエラーコードで分類します。
- **修正**: BT の glob 展開（`**`）、ルーチンダイアログの再描画、`main` のテスト不具合を修正しました。
