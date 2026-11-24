# カード 100-1: タスク系譜のデータ層

親: `doc/task-lineage-plan.md`（0.10.0 / ブランチ `feature/task-lineage`）

このカードは系譜をジョブに記録して保存・出力するところまで。タスクのレコード（`task create` など）は 100-2 で作る。このカードの `taskId` は「系譜の最初のジョブの id」（暗黙のタスク）で、100-2 で「タスクを指定して追加したジョブはそのタスクの id」が加わる。表示（ダッシュボード / Web UI / CLI の一覧）、`run --exit-on-drain`、やり直しジョブを自動で追加する経路は別カードで扱うので、ここでは触らない。

## 1. フィールド

`src/taskqueue.h` の `struct TaskQueueJob`（`TaskQueueOrigin origin;` の後）に追加する。

```cpp
// Task lineage (doc/task-lineage-plan.md). A job added to redo a failed job names it in
// `supersedes`; every job of one lineage shares `taskId` (the first job's id). The redone
// job records the job that took over in `supersededBy`. Empty taskId = the job is its own task.
std::string supersedes;
std::string taskId;
std::string supersededBy;
```

JSON のキーは snake_case で `supersedes` / `task_id` / `superseded_by`。

- `src/taskqueue.cpp` の `NormalizeJobJsonKeys` の `kPairs` に `{"taskId", "task_id"}` と `{"supersededBy", "superseded_by"}` を足す。
- 外部から渡せるのは `supersedes` だけ。`taskqueue_add` の JSON に `task_id` / `superseded_by` があっても無視する（Hancho が決める）。

## 2. 保存と読み込み

- 保存済みのジョブを読む関数（`job.permissionResumeLimit = ... "permission_resume_limit"` を読んでいる、2330 行付近から始まる関数）で 3 つを読む。
- ジョブを JSON にする関数（3290 行付近、`"permission_resume_limit"` を書いている所）で、**空でないときだけ** `"supersedes"` / `"task_id"` / `"superseded_by"` を書く。系譜のないジョブの出力は今と同じにする。
- `taskqueue list` / `taskqueue get` / MCP の出力がこの関数を通るなら、それで一覧にも出る。別の経路でジョブを JSON にしている所があれば、そこにも同じ 3 つを足す。
- `src/taskqueue.h` の 490〜575 行付近の plain ジョブ用 JSON（nlohmann）には足さなくてよい。

## 3. `TaskQueueAdd` での検証と結び付け

`ParseNewJob`（4107 行付近）で `supersedes` を読む（`JsonFieldStr(j, "supersedes", "")`）。結び付けは `TaskQueueAdd`（4359 行付近）で、重複 id チェックと依存チェックの後、`queues_[taskqueue] = tq;` の前に行う。

`supersedes` が空でない新しいジョブごとに:

1. 指定されたジョブを `FindJob` で探す（キューをまたいでよい）。なければエラー `job '<id>': unknown supersedes job '<target>'`。
2. 同じ `add` で追加するジョブ同士を指すのはエラー `job '<id>': supersedes must name an existing job`。
3. 指定されたジョブが BT 管理（`bt.id` が空でない）ならエラー `job '<id>': cannot supersede a behavior tree job; use bt_retry`。
4. **系譜の先頭（最新）に進める**: 指定されたジョブの `supersededBy` が空でなく、そのジョブがまだ存在する間は `supersededBy` をたどる（ループ防止に最大 1000 回）。たどり着いたジョブを `head` とし、新しいジョブの `supersedes` を `head.id` に書き換える。
5. `head.state` が `failed` / `blocked` / `cancelled` / `closed` / `takenOver` のいずれでもなければエラー `job '<id>': can only supersede a finished, unsuccessful job ('<head>' is <state>)`。succeeded や実行中・待機中のジョブはやり直せない。
6. 新しいジョブの `taskId` = `head.taskId` が空なら `head.id`、空でなければ `head.taskId`。
7. `head.supersededBy` = 新しいジョブの id、`head.updatedAt` を更新する。`head` が別のキューにあるなら、そのキューも `SaveQueue` する。

検証はすべて、どのキューも変更する前に済ませる（途中で失敗したら何も変えない）。

`TaskQueueAdd` の応答に、系譜を結び付けたジョブごとの情報を足す: `"lineage":[{"id":"<new>","supersedes":"<head>","task_id":"<taskId>"}]`（結び付けがなければキーごと出さない）。4 で `supersedes` を書き換えたときも、書き換え後の値を出す。

## 4. そのほか

- `taskqueue update`（4780 行付近）では 3 つとも変更できない。パッチに入っていても無視する（エラーにしない）。
- ジョブの削除・purge で系譜の途中が消えても、残ったジョブの値はそのままにする。たどる側（4 の手順や今後の表示）が存在しない id を飛ばす。
- requeue（同じジョブのやり直し）では 3 つとも変えない。

## 5. テスト

`tests/taskqueue_smoke.cpp` に、既存のテストと同じ書き方で次を足す。

- `supersedes` なしで追加したジョブは 3 つとも空で、JSON に 3 つのキーが出ない。
- failed のジョブ A を `supersedes: A` で B が引き継ぐと、B.taskId == A、B.supersedes == A、A.supersededBy == B。保存して読み直しても同じ。
- さらに `supersedes: A`（古いほう）で C を追加すると、C.supersedes が B に書き換わり、C.taskId == A、B.supersededBy == C。
- succeeded / pending のジョブを指すとエラーになり、キューは変わらない。
- 存在しない id、同じ add 内のジョブ、BT 管理のジョブを指すとエラーになる。
- camelCase の `supersedes` はそのまま通る。`task_id` / `superseded_by` を渡しても無視される。

## 5b. 追加: やり直しの理由 `supersede_reason`（2026-11-23 追加）

やり直しを決めたセッションが消えても理由を追えるように、引き継いだ側のジョブに理由を残す。

- `TaskQueueJob` に `std::string supersedeReason;` を足す（`supersededBy` の後）。JSON のキーは `supersede_reason`。`NormalizeJobJsonKeys` に `{"supersedeReason", "supersede_reason"}` を足す。
- `ParseNewJob` で読む。`supersedes` が空なら値は捨てる（エラーにしない）。
- 2000 バイトを超えたらエラー `job '<id>': supersede_reason exceeds 2000 bytes`。
- `supersedes` があるのに理由が空なら、`TaskQueueAdd` の応答の `warnings` に `{"job":"<id>","warning":"supersede_without_reason"}` の形で警告を足す（追加は成功させる）。既存の `warnings` の組み立て方に合わせる。
- 保存・読み込み・JSON 出力は他の 3 つと同じ扱い（空でないときだけ書く）。応答の `lineage` の各要素にも `"reason"` を足す（空なら出さない）。
- `taskqueue update` では変更できない（無視する）。
- テスト: 理由付きで引き継ぐと保存・読み直し後も残る / 理由なしだと警告が出て追加は成功する / `supersedes` なしの理由は捨てられる / 2000 バイト超はエラー。

## 6. 完了条件

- `cmake --build build --config Release --target Hancho hancho-cli taskqueue_smoke` が通る。
- `build\Release\taskqueue_smoke.exe` が成功する（既存のテストも含めて）。
- 変更するのは `src/taskqueue.h`、`src/taskqueue.cpp`、`tests/taskqueue_smoke.cpp` だけ。コミットはしない。
