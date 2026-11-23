# 修正失敗によるモデル切り替え — カード 101-1〜101-6

親: `doc/fix-failure-engine-switch-plan.md`（ブランチ `feature/fix-engine-switch`、0.10 系）

計画書の「未決定」には、supervisor が仮の答えを入れた（下の表）。違えば、そのカードが動く前に直す。

## 仮の決定

| # | 未決定 | 仮の答え |
|---|---|---|
| 1 | 回数の数え方 | 候補（エンジン・モデル）ごとに、失敗クラスごとに数える。切り替えたら新しい候補の数は 0 から。`after` に届かない失敗は今までどおりノードの失敗になり、外側の `retry` / `catch` が同じ候補で走らせ直す。`maxRetries` とは独立。 |
| 2 | 行き詰まりの判定 | 失敗クラスにする: `fix_no_progress`（前回と同じテストが落ちた）、`fix_oscillating`（直近 3 回のどれかと同じ集合に戻った）、`fix_failed`（それ以外の、前進している失敗）。`switchEngineOn` にそのまま書ける。 |
| 3 | 修正 → テストのループ | 新しいノード型は作らない。テストの `job` ノードに `blame: "<worker id>"` を書くと、そのジョブの失敗を、名前で指した worker ノードの失敗として数える（101-3）。 |
| 4 | 自己申告のマーカー | Hancho の約束として固定する。`HANCHO-RESULT: root_cause_not_found` を組み込みの分類規則にし、クラス `root_cause_not_found` を付ける。 |
| 5 | 前の候補の変更 | 既定は残す（`onSwitch: "keep"`）。`"revert"` は今回は作らない（VCS に依存するため）。指定されたら検証エラーにせず警告 `on_switch_revert_unsupported` を出す。 |
| 6 | 止める条件の置き場所 | worker ノードの `switchLimits`（BT の `defaults.worker` にも書ける）。 |
| 7 | モデルの問題か環境の問題か | 今ある失敗の診断を使う。`resource_conflict` / `permission_denied` / `rate_limited` / `provider_down` / `auth_error` は、`switchEngineOn` に書かれていても修正失敗としては数えない（同じ失敗の繰り返しにはならない）。切り替えの直前に、次の候補を probe する（`defaults.worker.probe` が有効なとき）。 |
| 8 | 切り替えの実績 | `engineSwitched` / 新しい `engineSwitchOutcome` イベントと、BT がタスクに属していればタスクの経過ログ（`kind: "engine_switched"`）に残す。memory や使用量への集計は今回はしない。 |

## カード

| カード | 内容 | 主なファイル | テスト |
|---|---|---|---|
| 101-1 | `switchEngineOn` の回数付きの要素 | `behavior_tree_internal.h`, `behavior_tree_parse.cpp`, `behavior_tree_exec.cpp`, `behavior_tree_state.cpp` | behavior_tree_smoke |
| 101-2 | 自己申告 `root_cause_not_found` と引き継ぎメモ | `taskqueue.cpp`（分類規則）, `behavior_tree_runtime.cpp` / `behavior_tree_exec.cpp` | taskqueue_smoke, behavior_tree_smoke |
| 101-3 | `blame` と行き詰まりの判定 | `behavior_tree_*`, `behavior_tree_parse.cpp` | behavior_tree_smoke, bt_integration_smoke |
| 101-4 | 切り替え先への引き継ぎ | `behavior_tree_exec.cpp`（プロンプトの組み立て）, `bt_manager.cpp`（タスクログ） | behavior_tree_smoke, bt_integration_smoke |
| 101-5 | 止める条件と escalate の中身 | `behavior_tree_*` | behavior_tree_smoke |
| 101-6 | 雛形・検証の警告・文書・スキル | `bt_templates.h`, `behavior_tree_parse.cpp`, `doc/behavior-tree*.md`, `skills/hancho/SKILL.md`, `cli.cpp`（help） | behavior_tree_smoke, hancho-cli help |

---

## 101-1 回数付きの切り替え条件

- `switchEngineOn` の要素に、文字列に加えてオブジェクト `{ "class": "<class>", "after": <n> }` を許す（worker ノードと `defaults.worker` の両方。`behavior_tree_parse.cpp` の 543 行付近と 607 行付近）。`after` は 1〜10 の整数で、省略は 1。文字列の要素は `after: 1` と同じ。
- `BtNode` の `std::vector<std::string> switchEngineOn` を、クラスと `after` の組の配列に変える（`behavior_tree_internal.h:120`）。今の参照箇所（`ShouldSwitchEngine`、`CopyNode` 相当の `behavior_tree_state.cpp:611`、検証の警告 `behavior_tree_parse.cpp:910` 付近）をすべて合わせる。
- 候補ごとの数: `BtNode` に `std::map<std::string, int> switchFailCounts;`（今の候補でのクラスごとの失敗数）を足す。worker ノードが失敗するたびに、そのクラスの数を 1 増やす。`ShouldSwitchEngine` は「次の候補がある」かつ「そのクラスの要素があり、数が `after` 以上」で true。
- 切り替えたら（`TrySwitchEngine`）`switchFailCounts` を空にする。`ResetSubtreeForRun` の `resetHistory` が true のときも空にする。false のときは残す（`engineCandidateIndex` と同じ扱い）。エンジンが回復して候補を先頭に戻すとき（`behavior_tree_runtime.cpp:172`）も空にする。
- 保存と読み込み（`behavior_tree_state.cpp` の `engineCandidateIndex` を書いている所）に `switchFailCounts` を足す。
- `after` に届かない失敗は、今までどおりノードの失敗として返す（外側の `retry` / `catch` が同じ候補で走らせ直す）。
- テスト（`tests/behavior_tree_smoke*.cpp` の既存の 091-3 のテストと同じ書き方で）: 文字列の要素は 1 回目で切り替わる / `after: 2` は 1 回目は切り替わらず同じ候補で再実行され、2 回目で切り替わる / 切り替え後の数は 0 から / 保存して読み直しても数が残る / `after: 0` や 11、`class` なしは検証エラー。

## 101-2 自己申告と引き継ぎメモ

- 分類器（`src/taskqueue.cpp` の組み込み規則。`kPermissionPattern` などがある 1238 行付近）に、行頭の `HANCHO-RESULT: root_cause_not_found` を見つけたらクラス `root_cause_not_found`（confidence high、action `retry`）を付ける規則を足す。ログの中の JSON 文字列の一部としての一致では付けない（行頭のみ）。
- worker の結果ファイル（`PC_BT_RESULT_FILE`）に `"handoff": { "suspected": "...", "tried": "...", "ruledOut": "..." }` があれば、その試行の `BtEngineAttempt` に `handoff`（JSON）として残す。成功・失敗どちらでも読む。各値は 2000 バイトで切る。
- `BtEngineAttempt` に `handoff`（json）と `failSignature`（101-3 で使う文字列の配列）を足し、保存・読み込み・`bt get` の出力に出す。
- worker ノードの失敗で切り替えなかった場合も、次の実行で引き継げるように、最後の試行の `handoff` をノードに `lastHandoff` として残す（101-4 で使う）。
- テスト: マーカー行でクラスが付く / JSON の中の文字列では付かない / handoff が試行に残り、保存後も読める / 2000 バイトで切られる。

## 101-3 `blame` と行き詰まりの判定

- `job` ノードに `blame: "<worker node id>"` を許す。検証: 指した id が同じ BT の worker ノードでなければエラー。`blame` を書いた job が、指した worker より前に実行される位置にあれば警告 `blame_before_worker`。
- `blame` を書いた job が失敗したとき（成功条件の不成立も含む）:
  1. 失敗の署名を作る: `success.testReport` があれば失敗したテスト名の集合。なければ、job ノードの `failLineRegex`（既定 `^\[FAIL\].*`）にマッチする行の集合を、`success.contentFile` があればそのファイルから、なければジョブのログから取る（最大 200 行、各行 300 バイト）。集合は並べ替えて重複を除く。
  2. 指した worker ノードの、今の候補での直近の署名と比べる: 同じなら `fix_no_progress`、直近 3 回のどれかと同じなら `fix_oscillating`、それ以外は `fix_failed`。署名が空なら `fix_failed`。
  3. そのクラスで worker ノードの `switchFailCounts` を 1 増やし、署名を worker ノードの `recentFailSignatures`（最大 3 件、保存対象）に積む。最後の `BtEngineAttempt` の候補と同じなら、その `failSignature` にも入れる。
  4. worker ノードの `ShouldSwitchEngine` が true なら、worker ノードの候補を次に進める（`TrySwitchEngine` と同じ記録とイベント。ただし worker ノードはもう終わっているので再起動はしない。次にその worker ノードが走るときに新しい候補が使われる）。
  5. job ノード自身の失敗はそのまま返す（外側のループが worker を走らせ直す）。
- 失敗の診断クラスが `resource_conflict` / `permission_denied` / `rate_limited` / `provider_down` / `auth_error` のときは、1〜4 をしない（環境の問題は数えない）。
- 切り替えで候補が変わったら、`recentFailSignatures` は空にする。
- テスト: テストの job の失敗が worker の数に入る / 同じ署名で `fix_no_progress`、違う署名で `fix_failed`、A→B→A で `fix_oscillating` / `switchEngineOn: [{"class":"fix_no_progress","after":1}]` で worker の次の実行が次の候補になる / 環境系のクラスは数えない / 保存・読み直し後も署名が残る。

## 101-4 切り替え先への引き継ぎ

- worker ノードを起動するとき、今の候補が先頭でなく（切り替えが起きた後）、`engineAttempts` に失敗した試行があれば、プロンプトの末尾に「前の候補の記録」を足す。中身は試行ごとに: エンジン/モデル、失敗クラス、失敗したテスト（`failSignature` の先頭 10 件）、`handoff` の 3 項目。合計 6000 バイトまで（古いほうから削る）。英語で、見出しは `Previous attempts by other models (do not repeat what did not work):`。
- 同じ候補で走らせ直すとき（切り替えていないとき）は、`lastHandoff` があれば `Your previous attempt noted:` として足す。
- worker ノードの `onSwitch`（`"keep"` のみ。`"revert"` は警告 `on_switch_revert_unsupported` を出して keep と同じ扱い）。
- BT がタスクに属していれば（100-2b の `taskId`）、切り替えのたびに `TaskLog(taskId, "engine_switched", "", btId, "<from> -> <to> (<class>)", "")` を書く。
- テスト: 切り替え後のプロンプトに前の候補の記録が入る / 切り替え前は入らない / 6000 バイトで切られる / タスク付きの BT でタスクログに `engine_switched` が残る。

## 101-5 止める条件と escalate の中身

- worker ノード（と `defaults.worker`）に `switchLimits` を足す: `{ "maxCandidates": <n>, "maxSec": <n>, "stopOnSameSymptom": <bool> }`。既定は `maxCandidates` = 3、`maxSec` = 0（無制限）、`stopOnSameSymptom` = true。
  - `maxCandidates`: 試した候補の数がこれに届いたら、それ以上切り替えない。
  - `maxSec`: そのノードで最初に起動してからの経過秒がこれを越えたら、それ以上切り替えない。
  - `stopOnSameSymptom`: 直前の候補の最後の `failSignature` と、今の候補の最後の署名が同じ（空でない）なら、それ以上切り替えない。
- 切り替えを止めたときは、`engineSwitchStopped` イベント（理由 `max_candidates` / `max_sec` / `same_symptom`）を出し、ノードの失敗詳細に理由を入れる。外側の `catch` の `"*"` の escalate に落ちる。
- escalate のパケットに、escalate の位置から見て直前に失敗した worker ノードの記録を足す: 各候補の handoff、全候補に共通する失敗したテスト（署名の積集合）、`stopOnSameSymptom` で止まったなら `"likelyNotModel": true`。キーは `engineAttempts`。
- テスト: 3 つの上限それぞれで止まり、イベントと理由が出る / パケットに共通の失敗テストと `likelyNotModel` が入る。

## 101-6 雛形・検証・文書

- `src/bt_templates.h` の `resilient-worker` と `card-sequence` に、テストの job の `blame` と、`switchEngineOn` の `{"class":"fix_no_progress","after":1}`、`{"class":"fix_failed","after":2}`、`{"class":"root_cause_not_found","after":1}` を足す。プロンプトに自己申告と handoff の書き方の一文を入れる。
- 検証の警告: worker の `switchEngineOn` に `fix_*` があるのに、その worker を `blame` する job がない → `fix_class_without_blame`。`bt validate --strict` でエラーになる。
- 文書: `doc/behavior-tree.md` / `doc/behavior-tree-ja.md` に「修正失敗で切り替える」節（`after`、`blame`、`fix_*` クラス、自己申告、handoff、`switchLimits`、`onSwitch`）。`skills/hancho/SKILL.md` の「BT を書くときはハング対策を最初から入れる」に 2〜3 行。`hancho-cli help --json` の bt の説明に `blame` と `switchLimits` を足す。
- `doc/fix-failure-engine-switch-plan.md` の状態を「実装済み（カード 101-1〜101-6）」に変える。

## 共通の約束

- 変えるのはカードに書いたファイルとそのテストだけ。C++ のソースは LF。コミットはしない。
- 既存のテストを弱めたり消したりしない。
