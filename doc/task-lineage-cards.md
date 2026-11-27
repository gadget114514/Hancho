# タスクと系譜: カード 100-2 〜 100-10

親: `doc/task-lineage-plan.md`。100-1 は `doc/task-lineage-100-1.md`。用語は `doc/terminology.md`。
worker は 1 枚ずつ順に実装する。前のカードの変更は作業ツリーにある前提で、その上に積む。各カードとも:

- 指定したファイル以外はなるべく触らない。触ったら結果ファイルにその理由を書く。
- 各ファイルの改行コードを保つ。
- ビルド・テストは自分で実行しない（BT の次のノードが行う）。コミットしない。
- 既存のテストを弱めたり消したりしない。新しい振る舞いにはテストを足す。
- ユーザーに見える文言は、既存の `Tr(L"日本語", L"English", L"Español")` の形に合わせて 3 言語にする。

## 仮の決定（2026-11-23。「とりあえず全部実装」の指示により supervisor が決めた。計画書の未決定・懸念への仮の答え）

| 項目 | 仮の決定 |
|---|---|
| 未決定 1: やり直しジョブの経路 | `taskqueue retry` は同じジョブの requeue なので `supersedes` は不要。内容を変えたやり直し用に `taskqueue supersede <id> --reason "..." [変更する項目]` を新設する（100-7）。supervisor が手で `taskqueue_add` するときは skill に `supersedes` と理由を付けるよう書く。 |
| 未決定 2: Web UI | 合わせる（100-9）。 |
| 未決定 3: `run --exit-on-drain` | 系譜基準を既定にする。引き継がれたジョブは数えない。旧挙動は `--count-superseded`（100-6）。 |
| 未決定 4: 呼び名 | API・CLI は `task`。画面は「タスク」。BT の仮想行（`group:<id>`）は画面上の呼び名を「BT」にして、タスクと区別する。→ その後 `doc/terminology.md` で決定: task の下の単位（BT 1 本、または `taskqueue add` 1 回分）を direction と呼び、0.10.0 の名前の変更で仮想行を `direction:<id>`・表示名「direction」に変える。 |
| 未決定 5: 保存場所 | `<data-dir>/tasks/<id>.json`（レコード）と `<data-dir>/tasks/<id>.log.jsonl`（経過ログ、追記）。ジョブが削除されても、タスクとログは残す。 |
| 未決定 6: `dependsOn` | 系譜の最新ジョブ基準。依存先 A の系譜の最新ジョブが成功したら、A に依存して `blocked` になっていたジョブは動き出す（100-4）。 |
| 未決定 7: TUI | 対応する（100-9。タスク列と、タスクごとのまとめ表示の切り替え）。 |
| 未決定 8: reopen / purge | 両方作る（100-2c）。 |
| 懸念 1: BT の仮想行（旧称タスクグループ。用語集では direction） | `--task` なしで起動した BT は今まで通り BT の仮想行。`--task` 付きの BT のジョブはタスクの下に並ぶ。 |
| 懸念 2: stable に戻すと消える | 0.10.0 のリリースノートに警告を書く（100-10）。コードでは対処しない。 |
| 懸念 3: purge | ジョブを削除する直前に、そのジョブの id・最終状態・`supersedes` / `supersededBy` をタスクのレコードの `removedJobs` に写す。 |
| 懸念 4: 状態の対応表 | 下の「状態の決め方」。 |
| 懸念 5: 整合性とロック | `TaskStore` は自分の mutex だけを持ち、`TaskQueueManager` を呼び返さない。ロックの順番は常に「`TaskQueueManager::mutex_` → `TaskStore` の mutex」。タスクの状態はジョブから計算するので保存しない（不整合が起きない）。経過ログはキューの保存の後に書き、書けなくても操作は失敗させない。 |
| 懸念 6: ジョブ単位の機能 | イベント（`EmitEventLocked` などのジョブイベント）に `task_id` を載せる（空なら載せない）。使用量・memory はジョブ単位のまま。 |
| 懸念 7: キューをまたぐ | 許す。`hidden` の内部ジョブはタスクに入れない（`--task` 付きの `hidden` ジョブはエラー）。 |
| 懸念 8: ログの肥大化 | 1 件あたりのサイズ上限（下記）。件数は制限しないが、`task show` は既定で最新 200 件だけ返す（`--log-limit`）。 |
| 懸念 9: `dependsOn` と `supersedes` | 未決定 6 と同じ。 |
| 懸念 10: routine・リモート | routine が起動する BT は自動でタスクを作らない。タスク情報はコアだけで持ち、リモートホストには送らない。 |
| 懸念 11: ライフサイクル | `task reopen` と `task purge --older-than <sec>`（閉じたタスクだけ削除）を作る（100-2c）。 |

### 状態の決め方（タスク）

タスクに属する direction（系譜）ごとに最新ジョブを求め（`supersededBy` をたどる。存在しない id は飛ばし、`removedJobs` の記録も使う）、次の優先順で決める。

1. タスクが閉じられていれば `closed`（`result` と `reason` を併記）。
2. direction が 0 件なら `open`。
3. どれかの direction が `running` / `paused` / `idle` / `waitingForLlm` → `running`（一時停止中があれば `paused: true` を併記）。進行中があれば、古い失敗に引っ張られない。
4. 未着手（`planned` / `open`）の direction が残っていれば `open`。古い成功や失敗で「終わり」と表示しない。
5. それ以外は**一番新しい direction** の状態を採用する。失敗した direction があっても、あとから作られた direction（救済・やり直し）の状態が勝つ。
   - `failed` / `blocked` / `closed` → `failed`
   - `cancelled` → `cancelled`
   - `succeeded` → `succeeded`
   - `open` / `planned` / `waitingForLlm` など → `open`
6. 残る direction がなければ `succeeded`。

新しさは direction の `createdAt`（なければ `startedAt`、暗黙 direction はジョブの `createdAt`）で決め、同じ時刻なら id の自然順で新しい方を採る。閉じた direction と `supersededBy` で引き継がれた direction は数えず、閉じた数は `closedDirections` に併記する。タスク一覧の direction も同じ新しさで並べ、新しいものを上に出す。

### サイズ上限

`title` 200 バイト、`intent` 16 KB、`note` 4 KB、`close` / `reopen` の `reason` 2000 バイト、`supersede_reason` 2000 バイト。超えたらエラー。

---

## 100-2a: タスクのレコード（コア）

ファイル: 新規 `src/task_store.h` / `src/task_store.cpp`、`src/taskqueue.h`、`src/taskqueue.cpp`、`CMakeLists.txt`、`tests/taskqueue_smoke.cpp`。

- `TaskStore`（`namespace pc`）: タスクのレコードの読み書き、経過ログの追記、id の採番、サイズ検査。
  - レコード: `id`、`title`、`intent`、`createdAt`、`updatedAt`、`origin`（`TaskQueueOrigin` と同じ形の JSON）、`closed`（`{result, reason, at}` または null）、`removedJobs`（`[{id, state, supersedes, supersededBy}]`）。
  - 経過ログ 1 行: `{"at","session","kind","job","bt","text"}`（空の項目は省く）。`kind` は `created` / `job_added` / `superseded` / `bt_started` / `bt_finished` / `escalate_answered` / `takeover` / `takeover_completed` / `triage_decided` / `job_cancelled` / `note` / `closed` / `reopened`。
  - id: `--id` 指定がなければ `task_<N>`（N は既存の最大 + 1）。指定された id も `task_` で始まらなければエラー。英数字・`_`・`-` のみ、64 文字まで。
  - データディレクトリは既存の `GetEffectiveDataDirectoryWide()` の下の `tasks`。起動時に全レコードを読む。
  - 変更のたびに増えるカウンタ `version` を持つ（GUI が差分取得に使う）。
- `TaskQueueManager` が `TaskStore` をメンバーとして持ち、次の操作を足す（戻り値は他の操作と同じく JSON 文字列）:
  - `TaskCreate(title, intent, id, originJson)`、`TaskShow(id, logLimit)`（レコード、計算した状態、属するジョブの一覧と系譜、経過ログの末尾）、`TaskList(stateFilter, limit)`、`TaskNote(id, text, session)`、`TaskClose(id, result, reason)`、`TaskReopen(id, reason)`、`TaskPurgeOlderThan(sec)`。
  - `TaskState(taskId)`: 上の「状態の決め方」。暗黙のタスク（レコードのない `taskId`）にも使える。
- `TaskQueueAdd` に引数 `task`（タスク id）を足す（既存の呼び出し元は空を渡す）:
  - タスクがない／閉じている → エラー。`hidden` のジョブ → エラー。
  - 追加したジョブの `taskId` にタスク id を入れる。経過ログに `job_added`（ジョブ id ごと）。
  - `supersedes` 付きのジョブ: 元ジョブの実効タスク id（`taskId`、空なら系譜の最初のジョブ id）が `task` と違えばエラー。元ジョブが閉じたタスクに属していればエラー（`task` 指定の有無にかかわらず）。結び付けたら経過ログに `superseded`（理由付き）。
  - 外部から追加するジョブの id が `task_` で始まったらエラー（暗黙のタスク id との衝突を防ぐ）。
- ジョブの削除（`TaskQueuePurge` / `TaskQueueRemove` / `TaskQueuePurgeOlderThan` / `RemoveJobsForTree`）: `taskId` がタスクのレコードを指していれば、削除の直前に `removedJobs` に写す。
- `CMakeLists.txt`: `src/taskqueue.cpp` が並んでいるすべての箇所に `src/task_store.cpp` を足す。
- テスト: 作成・採番・サイズ上限・`--id` の検査、`add --task`（成功・存在しない・閉じている・hidden）、`supersedes` とタスクの不一致、状態の決め方の各行、`close` / `reopen` / `purge`、ジョブ削除後も `removedJobs` で状態が出ること、保存して読み直しても同じこと。

## 100-2b: BT とタスク、イベントの task_id

ファイル: `src/behavior_tree_parse.cpp`、`src/behavior_tree_state.cpp`、BT の定義クラス（`intent_` を持つクラス）のヘッダ、`src/bt_manager.cpp`、`src/taskqueue.cpp`、テスト（`tests/taskqueue_smoke.cpp` と BT のテストのうち合う方）。

- BT 定義の最上位に `taskId` を足す。`intent` と同じように読み（`behavior_tree_parse.cpp` の `intent_ = doc.value("intent", ...)` 付近）、状態の保存（`behavior_tree_state.cpp` の `out["intent"]` 付近）にも書く。再起動後に読み直しても残ること。
- `BtManager::StartJob` / `StartWorker` で、その BT の `taskId` が空でなければジョブ JSON に `"task_id"` を入れる。`TaskQueueManager::AddBtJob` は（内部経路なので）`task_id` を読んでジョブの `taskId` に入れる。外部の `taskqueue_add` では今まで通り無視する。
- `TaskQueueManager` に `TaskLog(taskId, kind, jobId, btId, text, session)`（内部用）を足し、BT の開始・完了・失敗・キャンセル時に `bt_started` / `bt_finished`（text に最終状態）を書く。書く場所は BT の状態が変わる所（`bt_manager.cpp`）。タスクのレコードがない `taskId` には書かない。
- ジョブのイベント JSON に、ジョブの `taskId` が空でなければ `"task_id"` を足す（`EmitEventLocked` と、同じ形でイベントを組み立てている所）。
- テスト: `taskId` 付き BT のジョブが `taskId` を持つ、状態の保存と読み直し、イベントに `task_id` が出る。

## 100-2c: CLI / MCP / IPC の窓口

ファイル: `src/ipc_server.cpp`、`src/mcp_server.cpp`、`src/cli.cpp`、関連ヘッダ、テスト。

- IPC: `task_create` / `task_show` / `task_list` / `task_note` / `task_close` / `task_reopen` / `task_purge` / `task_wait`。`taskqueue_add`（2058 行付近）に `task`。BT の開始（2497 行付近と 2943 行付近）に `task`: 指定されたらタスクの存在と閉じていないことを確かめ、BT 定義の `taskId` に入れてから開始する。
- `task_wait`: タスクに属するジョブがすべて実行中・待機中でなくなるか、`timeout_ms` が来るまで待つ（既存の `TaskQueueWait` と同じ待ち方）。結果は `task_show` と同じ形。
- MCP: 上と同じ操作のツールを足し、`taskqueue_add` と `bt_start` のスキーマに `task` を足す。既存ツールの説明の書き方に合わせる。
- CLI（`hancho-cli`）:
  ```
  task create --title "..." [--intent "..." | --intent-file <path>] [--id <id>] [--json]
  task show <id> [--log-limit N] [--why] [--json]
  task list [--state open|running|failed|cancelled|succeeded|closed] [--limit N] [--json]
  task note <id> --text "..."
  task close <id> --result succeeded|failed --reason "..."
  task reopen <id> --reason "..."
  task purge --older-than <sec>
  task wait <id> [--timeout-ms N]
  taskqueue add ... --task <id>
  bt start ... --task <id>
  ```
   `task note` / `create` は origin（セッション）を既存の `taskqueue add` と同じ方法で付ける。`help --json` とテキストのヘルプに足す。
 - `task show` / `task wait` の応答には決定的な `why` 配列が常に含まれる（開いている理由がないときは空配列）。各要素は `kind` / `id` / `detail` / `source` を持ち、`kind` → `id` → `detail` の順に並ぶ。`--why` は人間向けの `Why open (N):` ブロックを表示する（`--json` では生 JSON を表示するため何も変えない）。LLM による要約は行わない。
 - テスト: IPC / MCP の引数の受け渡しを、既存のテストと同じやり方で確かめる。

## 100-3: 経過ログへの自動記録

ファイル: `src/taskqueue.cpp`、`src/bt_manager.cpp`（または escalate を扱う所）、`src/triage_manager.cpp`、テスト。

- ジョブの `taskId` がタスクのレコードを指すとき、次を経過ログに書く:
  - escalate への回答（`bt resume`）: `escalate_answered`、text に回答と rationale。BT の `taskId` を使う。
  - `TaskQueueTakeover`: `takeover`（by と reason）。`TaskQueueComplete`: `takeover_completed`（result と reason）。
  - トリアージの判断（`triage decide`）: `triage_decided`（候補と理由）。カードの対象ジョブの `taskId` を使う。
  - `TaskQueueCancel` / `TaskQueueClose`（ジョブ）: `job_cancelled`。
- テスト: それぞれで 1 件ずつログが増える。

## 100-4: 一覧の出力と依存の解決

ファイル: `src/taskqueue.cpp`、`src/taskqueue.h`、テスト。

- `TaskQueueList` / `TaskQueueGet` のジョブ JSON に `task`（`{"id","title","state"}`。レコードのない暗黙のタスクは `title` なし）を足す。`TaskQueueList` に `task` で絞り込む引数を足す（CLI `taskqueue list --task <id>`、MCP `taskqueue_list` の `task`）。
- 依存の解決（未決定 6）: `dependsOn` の判定で依存先ジョブを見る所（`ReevaluateAll` など）で、依存先が `supersededBy` を持つなら系譜の最新ジョブの状態で判定する。依存先の系譜の最新ジョブが成功したら、`blocked` だったジョブは待機に戻る（今の「依存先を直すと blocked の後続は自動で再キュー」と同じ扱い）。
- テスト: A 失敗 → B が blocked → A2（`supersedes: A`）成功 → B が動き出す。一覧の `task` 欄。

## 100-5: ダッシュボード

ファイル: `src/gui_dashboard.cpp`（タスクキューのリストビュー）、必要なら IPC クライアント側。

計画書「表示（ダッシュボード）」の決定どおりに作る。要点:
- リストビューのまま 2 段表示。タスク見出し行（`▸`/`▾`、title、状態、結果列 `✗✗✓`、ジョブ数）、子のジョブ行は `iIndent` で字下げ。グループ表示（`LVM_ENABLEGROUPVIEW`）は使わない。
- 暗黙のタスクでジョブ 1 つなら見出し行なし。実行中と未対応の失敗は自動で展開。引き継がれた失敗は薄く表示し、`↩ #N` / `→ #N` を出す。
- BT の仮想行（`group:<id>`）の表示名は「BT」。`--task` 付き BT のジョブはタスクの下に並ぶ。（0.10.0 の名前の変更で `direction:<id>`・「direction」に変える。`doc/terminology.md`）
- タスク行を選ぶと詳細ペインに意図と経過ログ。
- 「タスク表示 / ジョブ表示」の切り替え（ジョブ表示は今の一覧に「タスク」列）。集計はタスク基準を主、ジョブ基準を併記。
- 定期更新ではタスクの `version` を見て、変わったときだけ取り直す。
- テスト: 行の並びを決める処理は、リストビューから切り離した純粋な関数にして、既存の GUI のテスト（`tests/` で合うもの）か `taskqueue_smoke` で確かめる。画面の目視は supervisor が後でする。

## 100-6: `run --exit-on-drain`

ファイル: `src/taskqueue.h`（`TaskQueueRunJob` 付近の純粋関数）、`src/cli.cpp`、テスト。

- 既定で、`supersededBy` を持つジョブ（引き継がれたジョブ）を終了コードと JUnit の対象から外す。系譜の最新ジョブだけで判定する。
- `--count-superseded` で今の挙動（全ジョブ）に戻す。ヘルプに書く。
- テスト: A 失敗 → A2 成功のキューが既定で 0、`--count-superseded` で失敗のコード。

## 100-7: `taskqueue supersede` と skill

ファイル: `src/taskqueue.cpp` / `.h`、`src/ipc_server.cpp`、`src/mcp_server.cpp`、`src/cli.cpp`、`skills/` の Hancho skill、テスト。

- `taskqueue supersede <id> --reason "..." [--id <newId>] [--command ...] [--summary ...] [--timeout-sec N] [--instructions-file ...] [--patch <json>]`: 元ジョブを複製し、指定された項目を変えて、`supersedes: <id>` と理由付きで追加する。新しい id の既定は `<元の id>-s<N>`（衝突しない N）。元ジョブと同じキュー・同じタスク。BT 管理のジョブには使えない（エラー）。IPC / MCP（`taskqueue_supersede`）にも足す。
- skill（`skills/` にある Hancho の skill。リポジトリ内のもの）に足す: 作業を始める前に `task create` でタスクを作る、ジョブや BT は `--task` で付ける、判断と理由は `task note` に書く、内容を変えたやり直しは `taskqueue supersede --reason`、終わったら `task close`。
- テスト: 複製される項目、id の採番、BT ジョブの拒否、タスクの引き継ぎ。

## 100-8: 繰り返し失敗

ファイル: `src/taskqueue.cpp`、`src/triage*.cpp`、テスト。

- 同じ系譜で失敗したジョブが 3 つ（設定 `lineage_repeat_failures`、既定 3）になったら、トリアージのカード（種類 `lineage_repeated_failure`）を出す。推奨は escalate（人の判断）。候補に `ignore` と `cancel`。
- タスクのレコードがあれば経過ログに書く。`task show` と `taskqueue get` に `repeatedFailures: N` を出す。
- テスト: 3 回目でカードが 1 枚出る、2 回目では出ない、同じ系譜で重複して出ない。

## 100-9: Web UI と TUI

ファイル: `src/webui/app.js` と `src/webui_assets.cpp`（`GetAppJs()` に埋め込まれている。どちらが元かを確かめ、両方を同じ内容にする）、Web UI の HTTP / WebSocket 側、`src/cli_top.h`、テスト（`webui_smoke`、`cli_top_smoke`）。

- Web UI: ジョブ一覧に「タスク」列、タスクごとのまとめ表示の切り替え、タスクの詳細（意図・ジョブ・経過ログ）。
- TUI（`hancho-cli top`）: 「タスク」列と、タスクごとのまとめ表示の切り替えキー。ヘルプ行に書く。

## 100-10: 文書・ヘルプ・skill の仕上げ

ファイル: `doc/obsoleted/cli.md` / `doc/obsoleted/cli-ja.md`、`doc/obsoleted/mcp.md` / `doc/obsoleted/mcp-ja.md`、`doc/obsoleted/taskqueue.md` / `doc/obsoleted/taskqueue-ja.md`、`doc/obsoleted/hancho-taskqueue-spec.md`、`doc/obsoleted/cli-help.md` / `doc/obsoleted/cli-help-ja.md`、`src/cli.cpp` のヘルプ、`skills/`。

- タスク（`task *`）、`--task`、`supersedes` / `supersede_reason`、`taskqueue supersede`、状態の決め方、`run --exit-on-drain` の変更を書く。英語版と日本語版の両方。
- `README.md` / `README-ja.md` には書かない（リリース時に書く）。ただし `dist/` に `release-notes-draft-v0.10.0.md` を作り、0.9.x に戻すと系譜とタスクの情報が消える警告（懸念 2）を入れる。
