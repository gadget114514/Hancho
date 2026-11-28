## Hancho v0.10.3

### Changes
- **Register a standalone BT as a task direction**: `hancho-cli bt register --file <bt.json> --task <id>` (MCP `bt_register`) creates a planned direction under an open task without starting the tree or creating jobs; a later `bt start` with the same definition reuses it.
- **Configurable task-list window**: the settings dialog now labels the finished-item count as `finished_limit` and adds a numeric display-days setting (`task_list_display_days`, 1 or more, default 2). The "keep recent tasks" window uses this value instead of a hard-coded yesterday.
- **Task list rows no longer flicker**: the dashboard now fetches all finished tasks/jobs (`finished_limit: 0`) when building the "keep yesterday/today" set, so rows no longer appear and disappear as the 30-item recent window slides.
- **Deterministic task-open reason card**: `task show <id> --why` and the dashboard's "Why is task open" dialog list structured blockers (planned/open directions, card dependencies, blocked/pending/waitingForPermit/waitingForLlm jobs, superseded runs and BT escalations). No LLM is called. The same `why` array is returned by `--json`, MCP `task_show` and `task_wait`; a `task_` blocker id is reported as `taskId`, otherwise as `jobId`.
- **Static runtime for vendored codecs**: msgpack-c and zlib-ng are built with the static `/MT` MSVC runtime (set before the zlib-ng subdirectory), so the executables need no VC++ redistributable.
- **Help and skill docs**: CLI help and the Hancho skills now explain `--why` and where the reason list appears.

### 変更（日本語）
- **単体 BT をタスクの direction として登録**: `hancho-cli bt register --file <bt.json> --task <id>`（MCP `bt_register`）で、BT を起動せずジョブも作らず、オープン中のタスクに planned の direction を追加できます。後で同じ定義で `bt start` すると同じ direction を再利用します。
- **タスク一覧の表示範囲を設定可能に**: 設定ダイアログで完了済み項目数を `finished_limit` と明示し、表示日数（`task_list_display_days`、1 以上、既定 2）を数値で指定できるようにしました。「最近のタスクを保持する」窓を固定の昨日ではなくこの値にします。
- **タスク一覧の行の点滅を修正**: 「昨日・今日を保持」する判定に必要な完了済みタスク／ジョブを全件取得（`finished_limit: 0`）するようにし、直近 30 件の窓がずれることで行が出たり消えたりしなくなりました。
- **タスクが open な理由のカード**: `task show <id> --why` とダッシュボードの「なぜ open か」ダイアログが、構造化されたブロッカー（planned/open の direction、カードの依存待ち、blocked/pending/waitingForPermit/waitingForLlm のジョブ、置き換えられた run、BT の escalate）を列挙します。LLM は呼ばず、`--json`、MCP `task_show`、`task_wait` の `why` 配列にも同じ一覧が入ります。`task_` の id は `taskId`、それ以外は `jobId` として表示します。
- **同梱コーデックの静的ランタイム**: msgpack-c と zlib-ng を `/MT` の静的 MSVC ランタイムでビルドします（zlib-ng の追加前に設定）。VC++ 再頒布可能パッケージは不要です。
- **ヘルプと skill の整備**: CLI のヘルプと Hancho skill に `--why` と理由一覧の参照先を追記しました。
