## Hancho v0.10.2

### Changes
- **Task column shows task IDs**: the task queue list's Task column shows the task ID instead of its title.
- **Recent tasks stay visible**: tasks created yesterday or today and their children are kept regardless of the finished-row limit; other finished rows are limited to 30.
- **IPC and BT state fixes**: an empty IPC request no longer produces invalid JSON, a completed overlapped read keeps its error state, removing a BT records the cancellation on its direction, and the task state is recomputed even when a job has no direction of its own.
- **Task state follows the newest direction**: running work wins over an older failure, a planned recovery card shows `open`, and the task succeeds once the newest direction succeeds. Directions inside a task are listed newest first; a running task/direction no longer shows an end time but the remaining time instead.
- **Task list countdown and column widths**: running task/direction rows show the estimate/timeout countdown (or over-budget / elapsed text), and manually sized columns stay put when the window is resized.
- **Compact IPC responses**: the CLI and dashboard request msgpack + deflate responses and fall back to plain JSON with older cores. Brief process lists reduce the payload and the IPC read path no longer drops a chunk that completes immediately.
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
- **Vendored codecs**: `thirdparty/msgpack-c` and `thirdparty/zlib-ng` are built into Hancho (static `/MT` runtime); the About page lists their copyrights. IPC responses are sanitized to valid UTF-8, and smoke tests cover 16 KB / 64 KB / 128 KB / 1 MB payloads with json/msgpack and none/lznt1/deflate round trips.

### 変更（日本語）
- **タスク欄はタスク ID**: タスクキューリストの「タスク」列に、タイトルではなくタスク ID を表示します。
- **最近のタスクは残ります**: 昨日・今日作成されたタスクとその子供は終了済みでも上限なく保持し、それ以外の終了済み行は 30 件までに制限します。
- **IPC と BT 状態の修正**: 空の IPC リクエストで不正な JSON にならないようにし、即座に完了した重複読み取りのエラー状態を保持します。BT 削除時に direction へキャンセル状態を記録し、ジョブに direction がなくてもタスク状態を再計算します。
- **タスクの状態は最新の direction に従います**: 実行中があれば古い失敗より実行中を優先し、予定（planned）の救済カードがある間は `open`、最新の direction が成功すればタスクも成功になります。タスク内の direction は新しい順に並び、実行中のタスク／direction は終了時間ではなく残り時間を表示します。
- **タスク一覧の残り時間と列幅**: 実行中の行に見積もり／タイムアウトからの残り時間（超過時は超過時間、設定がなければ経過時間）を表示します。手動で調整した列幅は画面サイズを変えてもリセットされません。
- **IPC 応答のコンパクト化**: CLI とダッシュボードが msgpack + deflate の応答を要求し、古い core とはプレーン JSON にフォールバックします。プロセス一覧を簡略化して転送量を減らし、即座に完了する読み取りのチャンクが失われる問題も修正しました。
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
- **コーデックの同梱**: `thirdparty/msgpack-c` と `thirdparty/zlib-ng` を静的（`/MT`）で組み込み、版ダイアログの About に copyright を追記しました。IPC 応答を正しい UTF-8 にサニタイズし、16KB / 64KB / 128KB / 1MB のペイロードと json/msgpack × none/lznt1/deflate の round trip をスモークテストしました。
