# direction のレコードと 0.10.0 の名前の変更 — タスクカード 103-1〜103-8

状態: 実装済み（2026-11-25、`main` / 0.10.0）。103-1〜103-8 すべて実装・テスト済み（旧名は別名として維持）。

用語と決定は `doc/terminology.md` に合わせる（task / direction / job / attempt、exec / agent、トリアージのカード = direction）。このタスクカードは、用語集の「direction のレコード」「0.10.0 での名前の変更」「ぶつかる名前は新しく作る」を実装に落としたもの。

## なぜやるか

- task と job の間の単位（BT 1 本、`taskqueue add` 1 回分）が、データにない。BT は `group:<id>` の仮想行として一覧に出るだけで、`taskqueue add` 1 回分はまとまりとして残らない。
- トリアージの判断を direction 単位にする（102-10）、カードを閉じる（102-7）、タスクカードを Hancho に保存する（104-x）は、どれも direction のレコードを前提にする。
- 同じ言葉が階層の違うものを指している（job / worker / agent / peer / direction）。0.10.0 で API・JSON・CLI まで揃える。

## 仮の決定

用語集で「実装のカードで決める」とした項目への仮の答え。違えば、そのタスクカードを流す前に直す。

| 項目 | 仮の答え |
|---|---|
| direction の id | 種類によらず `dir_` + 連番（`dir_1`、`dir_2`、…）で採番する。BT の id とは別。BT の direction は `btId` を持ち、BT の状態にも `directionId` を書く |
| 保存場所 | `<data-dir>/directions/<id>.json`。タスクのレコードと同じく、job を purge しても残す。消えた job は `removedJobs` に写す（タスクと同じやり方） |
| 状態 | 保存せず中の job から計算する（タスクと同じ）。`planned`（104-x）/ `running` / `succeeded` / `failed` / `cancelled`、それと明示的に閉じた `closed`（結果と理由付き）。`supersededBy`（104-3 の救済）を持つ direction は引き継がれたものとして扱い、タスクの状態計算やトリアージ対象からは外す。BT の direction は BT の状態をそのまま使う |
| ロックの順番 | `TaskQueueManager::mutex_` → direction の store → `TaskStore`。store は `TaskQueueManager` を呼び返さない |
| 暗黙の direction | 0.10 より前の job（`directionId` がない）は、読み込み時に job 1 つにつき 1 本の暗黙の direction とみなす（id は `job:<jobId>`、レコードは作らない）。102-7 の「古い失敗の移行」はこれを使う |
| LLM リストの保存 | 保存するキーは `worker` のまま変えない（0.9.x に戻しても読めるように）。画面・CLI・MCP の名前だけ `agent` にする |
| Chat のメッセージの `direction` | 書くときは `flow` と `direction` の両方を書き、読むときは `flow` を先に見る。`direction` を書かなくするのは 0.11 以降 |
| 古い名前 | すべて別名として受け付け続ける。使ったときは `bt validate` / CLI の警告で新しい名前を案内するが、エラーにはしない |
| stable に戻したとき | 0.9.x は `directionId` や新しい型名を知らない。新しい型名（`exec` / `agent`）で書いた BT は 0.9.x では動かない。0.10.0 のリリースノートに書く（103-8） |

## タスクカード

| カード | 内容 | 前提 |
|---|---|---|
| 103-1 | direction のレコード（コア）と job の `directionId` | — |
| 103-2 | direction の状態・閉じる・CLI / MCP / IPC | 103-1 |
| 103-3 | 一覧の仮想行 `group:<id>` → `direction:<id>` | 103-2 |
| 103-4 | BT の JSON: `exec` / `agent` | — |
| 103-5 | agent list（`--list agent`） | — |
| 103-6 | Chat: peer / host / `flow` | — |
| 103-7 | MCP・CLI: `engine`、`run_agent`、`agent stream/probe/health` | — |
| 103-8 | 文書・ヘルプ・skill を用語集に合わせる | 103-1〜103-7 |

- 依存: 103-1 → 103-2 → 103-3、103-1〜103-7 → 103-8。依存のない 103-4〜103-7 は、103-1 と並行して流せる。ただし `src/cli.cpp`・`src/mcp_server.cpp` を触るタスクカード（103-2、103-5、103-6、103-7）は、同時に流すと衝突しやすいので 1 本ずつにする。
- 102-10（トリアージを direction 単位にする）は 103-2 の後。104-x（タスクカードを Hancho に保存する）は 103-2 の後。105-3 以降（CI 対応、`doc/ci-cards.md`）も 103-2 の後。

---

各タスクカードとも（100-x・102-x と同じ）:

- 指定したファイル以外はなるべく触らない。触ったら結果ファイルにその理由を書く。
- 各ファイルの改行コードを保つ。
- ビルド・テストは自分で実行しない（BT の次のノードが行う）。コミットしない。
- 既存のテストを弱めたり消したりしない。新しい振る舞いにはテストを足す。古い名前の既存テストは残し、別名として通ることの確認に使う。
- ユーザーに見える文言は `Tr(L"日本語", L"English", L"Español")` の形で 3 言語にする。

## 103-1: direction のレコード（コア）

ファイル: 新規 `src/direction_store.h` / `.cpp`、`CMakeLists.txt`、`src/taskqueue.h`、`src/taskqueue.cpp`、`src/bt_manager.h`、`src/bt_manager.cpp`、`src/behavior_tree.h`、`src/behavior_tree_parse.cpp`、`src/behavior_tree_state.cpp`、`src/routine.cpp`、`tests/taskqueue_smoke.cpp`、`tests/bt_integration_smoke.cpp`。

- `DirectionStore` を作る（`TaskStore` と同じ作り。自分の mutex だけを持つ）。レコードの項目: `id`、`kind`（`bt` / `jobs`）、`taskId`（なければ空）、`btId`（`bt` のとき）、`createdAt`、`origin`（作成したセッション）、`title`（BT の名前、または `taskqueue add` の最初の job の summary）、`closed`（`{at, result, reason}`、閉じていなければなし）、`removedJobs`。
- `TaskQueueJob` に `directionId` を足す。保存・読み込み（JSON は `direction_id`）、`taskqueue get` / `list` の出力に出す。
- 作るところ:
  - `taskqueue add`（1 回で 1 本。`--file` の中の job 全部と、job 1 つだけの追加も）。`--task` なしで複数 job を 1 回で追加したときも、同じ direction に属する job が暗黙の task でバラバラにならないよう、最初の job の id（または `directionId`）を共有の暗黙の `taskId` とする。
  - `bt start`、`routine run`（BT 1 本で 1 本）。BT 定義または状態にすでに `directionId` があればそれを使い（再起動・リカバリ、および 104-2 の `planned` direction からの昇格）、なければ `DirectionStore` で採番して BT の状態に `directionId` を保存する。再起動・リカバリ後もその BT が出す job は同じ direction に属する。
  - `taskqueue retry`（同じ job の requeue）は新しい direction を作らない。`taskqueue supersede` は、元の job と同じ direction に入れる（内容を変えたやり直しを 1 本の direction の中で続ける）。
- job を purge するときは、その id と最終状態を direction の `removedJobs` に写す。
- `--task` 付きなら direction の `taskId` にも入れる。タスクの経過ログに `direction_started`（`directionId`、`kind`、`title`）を書く。
- テスト: `taskqueue add` 1 回で direction が 1 本でき、中の job が全部同じ `directionId` を持つ、BT の job が BT の direction に属する、再起動後も保たれる、retry で増えない、supersede で同じ direction に入る、purge 後も `removedJobs` に残る、0.10 より前のキュー（`direction_id` なし）を読める。

## 103-2: direction の状態・閉じる・CLI / MCP / IPC

ファイル: `src/direction_store.h`、`src/direction_store.cpp`、`src/taskqueue.h`、`src/taskqueue.cpp`、`src/task_store.cpp`、`src/ipc_server.cpp`、`src/mcp_server.cpp`、`src/cli.cpp`、`tests/taskqueue_smoke.cpp`。

- 前提: 103-1。
- `DirectionState(id)`: 上の「仮の決定」の状態を中の job から計算する（系譜で引き継がれた job は最新だけを見る）。暗黙の direction（`job:<jobId>`）にも使える。
- 操作:
  ```
  hancho-cli direction show <id>            # レコード、状態、中の job
  hancho-cli direction list [--task <id>] [--state ...] [--limit N]
  hancho-cli direction close <id> --result succeeded|failed|ignored --reason "..."
  hancho-cli direction reopen <id> --reason "..."
  hancho-cli direction purge --older-than <sec>   # 閉じた、または完了した暗黙 task の古い direction を削除
  ```
  MCP（`direction_show` / `direction_list` / `direction_close` / `direction_reopen` / `direction_purge`）と IPC にも同じ操作。`task show` の出力に、task の中の direction の一覧（id、kind、title、状態）を足す。
- 閉じた direction の job は、実行中または待機中（未終了）のものがあれば閉じられない（エラー。先に `cancel`）。
- タスクの経過ログに `direction_finished`（終わったときの状態）、`direction_closed` / `direction_reopened`（理由付き）を書く。
- タスクの状態の決め方を direction 単位にする: 閉じた direction（および `supersededBy` で引き継がれた direction）の job は数えない。残りの direction に `running` があれば `running`、それ以外は一番新しい direction の状態を採用する（`failed` / `cancelled` / `succeeded` / `planned` や `open` なら `open`）。古い失敗は、あとから作られた救済 direction が `planned` なら `open`、成功すれば `succeeded` に変わる（閉じたものがあれば `closedDirections: N` を併記）。102-7 はこの上に、トリアージの判断で閉じる操作を足す。
- タスク一覧では、タスクの中の direction を新しい順（`createdAt` / `startedAt`、同じなら id の自然順）に並べる。
- テスト: 状態の計算の各行、閉じる・開き直す、閉じた direction を数えないタスクの状態、実行中・待機中の job があると閉じられない、CLI と MCP の出力。

## 103-3: 一覧の仮想行を `direction:<id>` にする

ファイル: `src/taskqueue.cpp`、`src/task_lineage_view.h`、`src/ipc_server.cpp`、`src/mcp_server.cpp`、`src/cli.cpp`、`src/cli_top.h`、`src/gui_dashboard.cpp`、`src/webui/app.js`、`src/webui_assets.cpp`（埋め込みも両方更新）、`tests/bt_ipc_smoke.cpp`、`tests/taskqueue_smoke.cpp`、`tests/cli_top_smoke.cpp`。

- 前提: 103-2。
- 一覧の BT の仮想行（`group:<btId>`、「タスクグループ」）を、direction の行（`direction:<directionId>`、表示名「direction」）にする。`taskqueue add` 1 回分（job が 2 つ以上）も `BuildTaskDisplayRows`（`src/task_lineage_view.h`）の `TaskDisplayRowKind::DirectionHeading` として同じ行でまとめる。job が 1 つだけの `jobs` direction は direction 見出し行を付けない（今と同じ見た目）。
- 階層は task → direction → job の 3 段。タスクの見出し行（`indent = 0`）の下に direction の行（`indent = 1`）、その下に job の行（`indent = 2`、direction 見出しを省略した単一 job は `indent = 1`）。タスク見出し行の結果列（`✗✗✓`）は direction 単位（direction 見出し行の結果列が中の job 単位）にする。
- `group:<id>` を受け取っていた操作（キャンセル、一時停止、再開、リトライ、右クリックのメニュー）は `direction:<id>` も受け付け、`group:<btId>` も別名として受け付け続ける（BT の id から direction を引く。`kind: "jobs"` の direction に対する cancel / retry は属する該当 job 群をまとめて操作する）。
- 「タスクグループ」の文言を「direction」に変える（3 言語。英語・スペイン語も `direction`）。
- テスト: BT と `taskqueue add`（2 job 以上）の direction がそれぞれ 1 行にまとまる、`group:` と `direction:` のどちらでも操作できる、TUI の表示。

## 103-4: BT の JSON を `exec` / `agent` にする

ファイル: `src/behavior_tree_parse.cpp`、`src/behavior_tree_internal.h`、`src/behavior_tree_exec.cpp`、`src/behavior_tree_state.cpp`、`src/bt_templates.h`、`src/bt_debug_view.cpp`、`src/bt_manager_api.cpp`、`src/cli.cpp`（`bt template` / `bt validate` の出力）、`tests/behavior_tree_smoke.cpp`、`tests/behavior_tree_smoke_part2.cpp`。

- ノードの `type` に `exec`（今の `job`）と `agent`（今の `worker`）を足す。内部の扱いは同じ。古い `job` / `worker` は別名として読み、`bt validate` で警告 `deprecated_node_type`（新しい名前を案内）を出す。
- `defaults.exec` / `defaults.agent` を足す。古い `defaults.job` / `defaults.worker` も読む。両方あれば新しい方を使い、警告を出す。
- ノードの型を指す警告の名前を新しい型名に合わせる（`unguarded_worker` → `unguarded_agent`、`blame_before_worker` → `blame_before_agent` など）。古い警告名でフィルタしている利用者のため、警告の JSON に `alias`（古い名前）も入れる。
- 雛形（`bt_templates.h`、`resilient-worker` など）は新しい型名で書き直す。雛形の名前も `resilient-agent` にし、古い名前を別名として残す。
- BT の状態を保存するときは、読んだときの型名ではなく新しい型名で書く。0.9.x で作った実行中の BT を読み直せること。
- テスト: 新旧どちらの型名・`defaults` でも同じ木になる、警告、雛形が新しい型名で検証を通る、古い状態ファイルの読み直し。

## 103-5: agent list（`--list agent`）

ファイル: `src/llm_config.h`、`src/llm_config.cpp`、`src/cli.cpp`、`src/mcp_server.cpp`、`src/ipc_server.cpp`、`src/gui_dashboard.cpp`（LLM リストの画面のコンボボックス）、`tests/worker_engines_smoke.cpp`。

- `llm add/list/remove/approve/set_enabled` の `--list` に `agent` を足す（今の `worker` と同じリスト）。`worker` は別名。MCP の `llm_*` の `list` も同じ。
- 画面の表示名を「agent」にする。保存するキーは `worker` のまま（仮の決定）。
- 出力の JSON のリスト名は `agent` にし、`worker` で問い合わせたときも `agent` で返す（既存の `tests/worker_engines_smoke.cpp` で返却 JSON の `"list": "worker"` を期待している箇所は `"agent"` に更新してよい）。
- テスト: `agent` と `worker` のどちらでも同じリストを読み書きできる、保存ファイルのキーが変わらない。

## 103-6: Chat の peer / host / `flow`

ファイル: `src/cli.cpp`（`chat` の各コマンド）、`src/ipmsg.cpp`、`src/ipmsg_handler.cpp`、`src/gui_ipmsg.cpp`、`src/gui_dashboard.cpp`（Chat のメッセージの `direction` を読む所）、`src/mcp_server.cpp`、`src/ipc_server.cpp`、Chat のテスト（今あるものに足す）。

- `--from-agent` / `--to-agent` / `--audience agent` に `--from-peer` / `--to-peer` / `--audience peer` を足す。古い名前は別名。メッセージの JSON の `from_agent` / `to_agent` も `from_peer` / `to_peer` を足し、両方書く。
- `chat peers` / `chat refresh` の LAN 上の Hancho を host と呼ぶ: `chat hosts` を足し、`chat peers` は別名。MCP の `chat_peers` に `chat_hosts` を足す。
- メッセージの送受信の向き `direction`（`in` / `out`）に `flow` を足す（仮の決定のとおり両方書く）。
- テスト: 新旧どちらのオプションでも同じメッセージになる、`flow` と `direction` の両方が書かれ、どちらか一方だけのメッセージも読める。

## 103-7: MCP・CLI の `engine`、`run_agent`、`agent stream/probe/health`

ファイル: `src/mcp_server.cpp`、`src/ipc_server.cpp`、`src/ipc_server_remote.cpp`、`src/cli.cpp`、`src/main.cpp`、`src/process_manager.h`、`src/webui/app.js`、`src/webui_assets.cpp`（埋め込みも両方更新）、`tests/bt_ipc_smoke.cpp`、`tests/webui_smoke.cpp`。

- MCP・IPC の引数 `agent_type` に `engine` を足す（中身はエンジン名）。`agent_type` は別名。出力の JSON も `engine` を足し、当面は両方書く。
- MCP の `run_worker` に `run_agent` を足す。`get_worker_state` / `get_worker_stream` / `wait_worker` / `update_worker_info` / `worker_probe` / `worker_health` にも `agent` の名前を足す。古い名前は別名として残す（ツールの一覧には両方出し、古い方の説明に「別名」と書く）。
- CLI の `worker stream` / `worker probe` / `worker health` に `agent stream` / `agent probe` / `agent health` を足す。`worker` は別名。
- 内部の関数名・ファイル名（`worker_probe.cpp` など）は変えない（差分を小さくする）。
- テスト: 新旧どちらの名前でも同じ結果、MCP のツール一覧に両方出る。

## 103-8: 文書・ヘルプ・skill を用語集に合わせる

ファイル: `skills/hancho/SKILL.md`、`src/cli.cpp`（help の出力）、`doc/obsoleted/behavior-tree*.md`、`doc/obsoleted/cli*.md`、`doc/obsoleted/mcp*.md`、`doc/obsoleted/taskqueue*.md`、`doc/obsoleted/chat*.md`、`README.md`、`README-ja.md`、`doc/task-lineage-plan.md`、`doc/terminology.md`。

- 前提: 103-1〜103-7。

- 103-1〜103-7 で入れた新しい名前で書き直す。古い名前は「別名（0.9.x まで）」として 1 か所にまとめて載せる。
- 用語（task / direction / job / attempt、exec / agent、worker、agent list、peer / host、トリアージのカード = direction）を、用語集の定義のとおりに使う。
- 0.10.0 のリリースノートの下書き（`dist/` に置くのはリリースのとき）に、stable（0.9.x）に戻したときの注意を書く: 系譜・タスク・direction のつながりが消える、新しい型名の BT は動かない。
- `doc/terminology.md` の「0.10.0 での名前の変更」「ぶつかる名前は新しく作る」を「実装済み」に変え、名前が案から決まったものを書き換える。
- テスト: `hancho-cli help` / `help --json` に新しい名前が出る、skill の例が `bt validate` を通る。
