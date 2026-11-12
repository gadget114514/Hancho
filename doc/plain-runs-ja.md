# キューの外の実行の状態保存（Plain Runs）

**[English](plain-runs.md) | [日本語 (Japanese)](plain-runs-ja.md)**

Hancho ではタスクキュー（`TaskQueueJob`）だけでなく、キューの外で起動された単発プロセスや手動実行プロセスも、終了時に最小の状態を保存し、L1 の実行記録（`RunRecord`）および `memory rebuild` による再生の対象とします（`doc/design-v3.md` §7 決定 2026-11-11、カード `072-1`）。

---

## 1. 対象となる実行

以下の起動経路で動いた、タスクキューに属さない実行が対象です:

1. **`hancho-cli run` / `hancho-cli start` による単発実行**:
   - `--file`、`--exit-on-drain`、`--report` などのキュー一括実行オプションを指定せず、1 コマンドを起動した場合。
2. **`HANCHO_EVENT_PIPE` を伴う手動プロセス**:
   - `HANCHO_EVENT_PIPE` 環境変数を受け取って起動されたプロセス、または `hancho-cli event emit` でイベントを Hancho に報告するラッパースクリプトや CLI。
3. **GUI / IPC `action: "start"` による直接実行**:
   - ダッシュボードの入力欄や IPC 経由で直接起動され、タスクキューのジョブ ID を持たない管理プロセス。

---

## 2. jobId と attempt の発行経路

キューの外の実行には、既存のタスクキューと衝突しない一意な ID を発行します:

- **jobId**:
  - 呼び出し側から `jobKey`（または `--job-id`）が指定されている場合はそれを優先。
  - 未指定の場合、`TaskQueueManager::IssuePlainJobId(processId, name)` により `j_plain_<name>_<pid>_<seq>`（例: `j_plain_build_1234_1`）を発行。
- **attempt**:
  - キューの外の実行は原則として 1 回きりの試行（one-shot）であるため、既定で `1` となります。

---

## 3. 保存形式と場所

試行の最小の状態（開始・終了・終了コード・ログ・プロセス ID）は、既存のタスクキュー状態ファイル（`logs/taskqueues/*.json`）と全く同一のスキーマで永続化されます:

- **保存先**: `logs/taskqueues/plain.json`
- **キュー名**: `"plain"`

### `plain.json` の構造例

```json
{
  "name": "plain",
  "concurrency": 0,
  "paused": false,
  "version": 1,
  "notify_file": "",
  "max_timeout_sec": 0,
  "default_max_attempts": 1,
  "jobs": [
    {
      "id": "j_plain_deploy_555_1",
      "taskqueue": "plain",
      "name": "deploy",
      "summary": "Deploy production artifacts",
      "command": "deploy.cmd --prod",
      "cwd": "D:/wsgit/app",
      "state": "succeeded",
      "created_at": "2026-11-10 15:00:00",
      "updated_at": "2026-11-10 15:01:00",
      "attempts": [
        {
          "n": 1,
          "process_id": 555,
          "start": "2026-11-10 15:00:00",
          "end": "2026-11-10 15:01:00",
          "reason": "ok",
          "exit_code": 0,
          "log_file": "D:/wsgit/app/logs/proc_555.log",
          "last_lines_file": "D:/wsgit/app/logs/taskqueue/j_plain_deploy_555_1.1.tail.txt",
          "check_detail": ""
        }
      ]
    }
  ]
}
```

---

## 4. `memory rebuild` との結合

- `memory rebuild`（`RebuildRunRecordsJson()`）は `logs/taskqueues/*.json` を走査します。
- `plain.json` もそのまま走査対象となるため、キューの外で動いた作業も決定的に L1 `RunRecord`（`rr_<jobId>_<attempt>`）へと変換・インデックス化されます。
- JSON ストリーム（`.events.jsonl`）がない通常コマンドでも、コマンド名、開始・終了時刻、所要時間、終了コード、ログ末尾のエラー行が RunRecord として記録されます。

---

## 5. 失敗分類と取得不能項目の扱い（072-2）

1. **決定的な失敗分類**:
   - タスクキューの外で終了した試行、あるいは事前に失敗診断が付与されていない試行に対して、以下の決定的な規則による分類が適用されます:
     - **`launch_error`**: プロセス起動失敗（バイナリが見つからない等）。
     - **`killed`**: 明示的な終了またはキャンセル。
     - **`timeout`**: `timeoutSec` 超過またはタイムアウト終了。
     - **`idle_stall`**: 出力・CPU 停止。
     - **`crash`**: NTSTATUS 例外終了コード（例: `0xC0000005`）。
     - **`exit_nonzero`**: 非 0 の終了コードによる終了。
     - **`success_check_failed`**: 終了コードは 0 だが条件不満足。
2. **取得不能項目の扱い**:
   - LLM イベントストリームが存在しない場合に取得できない項目（トークン数、`filesEdited`、および `tool_error_loop` や `turn_or_context_limit` 等の高度な失敗分類）は空のまま保持されます。推測による値の補完は行いません。
   - イベントファイルが存在しない場合、`filesRead` や `filesEdited` は空配列（`[]`）となり、`usage` は省略されます。

---

## 6. 検索と集計への反映（072-2）

キューの外の実行記録も、L1 メモリの一級市民として扱われます:

- **`memory search --kind run`**:
  - キューのジョブと同様にインデックス化され、検索結果に返されます。
  - `--taskqueue plain` や `--failure-class <class>` による絞り込みも可能です。
- **`memory stats`**:
  - 総実行数、成功率、所要時間統計（中央値・p90）、失敗分類の内訳、ジョブ別の集計に反映されます。

---

## 7. 既存キューへの影響の分離

既存のタスクキュー（`main.json` や Behavior Tree のブランチキューなど）の動作に悪影響を与えないよう、以下が保証されています:

1. **スケジューラーの不干渉**:
   - `plain` キュー内のジョブはすでに終了済み（`succeeded` または `failed`）として保存され、`pending` にならないため、タスクキュースケジューラー（`SchedulerLoop`）が勝手に実行することはありません。
2. **一括リトライからの除外**:
   - `hancho-cli taskqueue retry --failed` などの全キュー一括再試行において、`plain` キューは自動的にスキップされます。
3. **ChainGuard / コマンド衝突チェックの除外**:
   - `FindUnfinishedJobByCommand` は `plain` キューをスキップするため、以前に単発実行で失敗したコマンドが残っていても、後続の新規プロセス起動をブロックしません。
4. **一意な ID 空間**:
   - `j_plain_` プレフィックスにより、既存のジョブ ID との衝突を防ぎます。
