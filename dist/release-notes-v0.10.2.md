## Hancho v0.10.2

### Changes
- **Task list tree stays intact**: an explicit direction record (not an implicit `job:` direction) now always gets a heading, even after its behavior tree has finished and it has only one job. The task → direction → job shape stays visible; the "Running" column continues to show only work that is currently running.
- **Task list refresh fix**: changed cells are no longer blanked out by the in-place refresh that replaced the full rebuild.
- **Installer copyright**: the setup executable carries `Copyright (c) 2026 gadget114514, 2nek`.
- **Escalation no longer hangs forever**: BT templates now give escalation `timeoutSec: 1800`; on timeout the node defaults to `stop` (a configured proxy fallback may still answer first). A build/test that exits 1 within 30 seconds is retried once before escalation, and the question now quotes the failed step, exit code, class and log tail.
- **BT state and lineage propagate**: every BT run belongs to a task and records `supersedes` lineage. The tree's state is cached on its direction, replaced directions follow their successor, and direction/task states are recomputed on job and attempt changes. Change events are emitted only when the derived state actually changes.
- **Task list job indentation**: job rows in the task tree use four spaces in both the dashboard and the CLI.
- **BT working directory**: behavior trees start in the submitter's working directory.
- **BT specification document**: `doc/bt-spec.md` documents the direction ↔ BT relationship, state derivation and propagation, node types, variables, escalation and routines.
- **Dashboard / data-dir alignment**: a running core publishes its data directory and a later CLI or dashboard adopts it, so a headless core started by another process stays visible. `.hancho` is used only when it exists in the current project folder; parent folders are not searched.
- **`hancho-cli dashboard [show|status]`**: asks the running core to show its desktop dashboard using the core's own data directory.
- **Running Tasks KPI**: the job count was removed from the 稼働中タスク card; only the task count is shown.

### 変更（日本語）
- **タスク一覧のツリー形状を維持**: 明示的な direction レコード（`implicit` の `job:` 行ではないもの）は、BT が終了してジョブが1本だけになっても見出しを残します。タスク → direction → ジョブの階層が見え続け、「実行中」列は今実行中のものだけを表示します。
- **タスク一覧の更新修正**: 全行の作り直しをやめた差分更新で、変化したセルが空になる問題を修正しました。
- **インストーラーの copyright**: `Copyright (c) 2026 gadget114514, 2nek` を記載しています。
- **エスカレーションが延々と待たなくなりました**: BT テンプレートの escalate に `timeoutSec: 1800` を設定し、期限切れ時は既定で `stop` を自動回答します（設定された proxy fallback が先に判断することも可能）。30 秒以内に終了コード 1 で失敗したビルド／テストは、エスカレーション前に 1 回だけ自動再試行します。問いには失敗したステップ、終了コード、失敗クラス、ログ末尾を埋め込みます。
- **BT の状態と系譜が伝播します**: すべての BT 実行がタスクに属し、`supersedes` の系譜を保存します。BT の状態は direction にキャッシュされ、置き換えられた direction は後継の状態を反映します。ジョブ／attempt の変化で direction とタスクの状態を再計算し、状態が実際に変わったときだけ変更イベントを発行します。
- **タスク一覧のジョブ行のインデント**: ダッシュボードと CLI の両方で、ジョブ行を 4 スペース字下げにしました。
- **BT の作業ディレクトリ**: ビヘイビアツリーは投入者の作業ディレクトリで起動します。
- **BT 仕様ドキュメント**: `doc/bt-spec.md` に direction と BT の関係、状態の導出・伝播、ノード種別、変数、escalate、routine を整理しました。
- **ダッシュボードと data-dir のすりあわせ**: 稼働中の core が data-dir を公開し、後から起きた CLI / ダッシュボードが採用します。別プロセスが headless 起動した core でも見え続けます。`.hancho` はカレントのプロジェクトフォルダだけを探し、親フォルダは遡りません。
- **`hancho-cli dashboard [show|status]`**: 稼働中の core に、その core の data-dir でダッシュボードを表示させます。
- **稼働中タスク KPI**: ジョブ数を廃止し、タスク数だけを表示します。
