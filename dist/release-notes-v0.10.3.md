## Hancho v0.10.3

### Changes
- **Task list rows no longer flicker**: the dashboard now fetches all finished tasks/jobs (`finished_limit: 0`) when building the "keep yesterday/today" set, so rows no longer appear and disappear as the 30-item recent window slides.
- **Deterministic task-open reason card**: `task show <id> --why` and the dashboard's "Why is task open" dialog list structured blockers (planned/open directions, card dependencies, blocked/pending/waitingForPermit/waitingForLlm jobs, superseded runs and BT escalations). No LLM is called. The same `why` array is returned by `--json`, MCP `task_show` and `task_wait`; a `task_` blocker id is reported as `taskId`, otherwise as `jobId`.
- **Static runtime for vendored codecs**: msgpack-c and zlib-ng are built with the static `/MT` MSVC runtime (set before the zlib-ng subdirectory), so the executables need no VC++ redistributable.
- **Help and skill docs**: CLI help and the Hancho skills now explain `--why` and where the reason list appears.

### 変更（日本語）
- **タスク一覧の行の点滅を修正**: 「昨日・今日を保持」する判定に必要な完了済みタスク／ジョブを全件取得（`finished_limit: 0`）するようにし、直近 30 件の窓がずれることで行が出たり消えたりしなくなりました。
- **タスクが open な理由のカード**: `task show <id> --why` とダッシュボードの「なぜ open か」ダイアログが、構造化されたブロッカー（planned/open の direction、カードの依存待ち、blocked/pending/waitingForPermit/waitingForLlm のジョブ、置き換えられた run、BT の escalate）を列挙します。LLM は呼ばず、`--json`、MCP `task_show`、`task_wait` の `why` 配列にも同じ一覧が入ります。`task_` の id は `taskId`、それ以外は `jobId` として表示します。
- **同梱コーデックの静的ランタイム**: msgpack-c と zlib-ng を `/MT` の静的 MSVC ランタイムでビルドします（zlib-ng の追加前に設定）。VC++ 再頒布可能パッケージは不要です。
- **ヘルプと skill の整備**: CLI のヘルプと Hancho skill に `--why` と理由一覧の参照先を追記しました。
