# Hancho の direction と BT / BT 仕様

この文書は v0.10.2 時点の実装を基準に、Hancho の `direction`（指示）とビヘイビアツリー（BT）の関係、および BT 定義の仕様を説明する。用語の詳細は `doc/terminology.md` / `doc/terminology-ja.md` を参照。

## 1. 階層モデル

```
task       依頼・意図・経過ログ
 └ direction  キューへの1回の登録（BT 1本、または taskqueue add 1回分）
    └ job     1プロセス=1コマンド
       └ attempt  1試行
```

- `direction` は状態を保存せず、配下の job / BT から毎回導出される（後述のキャッシュは表示・通知用）。
- BT は `direction` の「実行本体」であり、1 本の BT が 1 つの direction に対応する。

## 2. direction と BT の関係

### 2.1 登録

`BtManager::Start`（`src/bt_manager_api.cpp`）は BT 定義を受け取ると:

1. `taskId` が空なら `BT: <summary>` というタスクを自動作成して定義へ注入する（`BtManager::Start`）。
2. `BehaviorTree::LoadDefinition` で `taskId` / `directionId` を読み込む。
3. `TaskQueueManager::EnsureBtDirection(btId, directionId, taskId, title, origin, ..., triage)` を呼び、direction レコードを作成または更新する。
4. direction レコードには `kind: "bt"`、`btId`、`taskId`、`title`、`origin`、`supersedes`（ある場合）が保存される。
5. 得られた direction id を `tree->SetDirectionId()` で BT へ戻す。

`taskqueue list` は実行中の BT について仮想行 `direction:<dirId>`（旧形式 `group:<btId>`）を生成し、未開始ノードを `planned` 行として出す。これらは保存されず、BT が進むと消える。

### 2.2 状態の導出（`DirectionStateLocked`, `src/taskqueue.cpp`）

優先順位は次のとおり。

1. `closed` レコード → `closed`
2. `supersededBy` がある → 置き換え先（head）の状態を再帰的に返す
3. `btId` があり `btState` キャッシュまたは状態ファイル `<logs>/behaviortrees/<btId>.json` がある → その BT 状態
4. `direction_id` が一致する job がある → `ComputeTaskState`（最新系譜が全成功なら `succeeded`）
5. 自分を `supersedes` する子 direction がある → 子の状態を集約
6. BT 紐付きでタスク状態が参照可能なら、自分を除いたタスク状態を継承
7. 以上が無い場合 → カードは `planned`、BT 紐付きは `open`

`planned` は「未開始のカード direction」だけに使い、BT に紐づく direction には使わない。

### 2.3 状態変更の伝播

- job / attempt の状態が変わると `RefreshJobStateChainLocked` が direction 状態を再計算する。
- direction 状態が実際に変わったときだけ direction レコードの `state` と `rev` を更新し、`direction_finished` をタスクログへ書く。
- 続けて task 状態を再計算し、変更時のみ保存する。
- task 状態が変わると、その task に属する direction も再計算する（双方向伝播）。
- 状態が変わらなかった場合は rev もイベントも発行しない。

BT 側も `SaveStateFileLocked` 時に `SetDirectionBtState` で `btState` / `btDetail` を direction へ書き込むため、状態ファイルが消えても BT 状態が direction に残る。

### 2.4 系譜

- direction レコードの `supersedes` / `supersededBy` でやり直しのつながりを表す（`direction redo`）。
- 置き換えられた direction は head の状態を報告するため、古い run の `cancelled` がタスク状態を引きずらない。
- 現状の BT 定義には `supersedes` を書くフィールドが無い。系譜は direction / job の操作で表現する（今後の拡張点）。

## 3. BT 定義のトップレベル

```json
{
  "id": "my-bt",
  "summary": "説明",
  "intent": "意図（escalation packet に載る）",
  "cwd": "D:/work/proj",
  "taskId": "task_1",
  "directionId": "dir_3",
  "taskqueue": "bt-my-bt",
  "notifyFile": "notify.json",
  "limits": { "maxNodeRuns": 200, "maxWallSec": 86400 },
  "defaults": { "agent": { "idle": { "firstEventSec": 180, "outputSec": 600 }, "probe": true } },
  "vars": {},
  "root": { "type": "sequence", "children": [] }
}
```

- `cwd` を省略すると、投入者の作業ディレクトリで実行される。
- `taskqueue` 省略時は `bt-<id>`。
- `defaults.agent`（旧名 `defaults.worker`）は idle / probe の既定値。`defaults.exec` も使用可。
- `limits.maxWallSec` に達するとツリーは `limitExceeded` で失敗する。

## 4. ノード種別

| type | 用途 | 主なフィールド |
| --- | --- | --- |
| `sequence` | 子を順番に実行、最初の失敗で停止 | `children` |
| `fallback`（別名 `selector`） | 子を順番に実行、最初の成功で終了 | `children` |
| `parallel` | 子を並列実行 | `children` |
| `if` | 条件分岐 | `cond`, `then`, `else` |
| `catch` | 失敗ハンドラ | `child`, `handlers`, `otherwise`, `maxRetries` |
| `retry` / `repeat` | 繰り返し | `max`, `delaySec`, `child`, `until` |
| `foreach` | glob 展開して繰り返し | `glob`, `as`, `mode`, `do` |
| `invert` / `forceSuccess` / `forceFailure` | デコレータ | `child` |
| `timeout` | 子に制限時間 | `child`, `timeoutSec` |
| `exec`（旧 `job`） | 1コマンド | `command`, `timeoutSec`, `success`, `blame`, `engines` |
| `agent`（旧 `worker`） | LLM ワーカー | `prompt`, `model`, `timeoutSec`, `engines`, `outputSchema` |
| `probe` | エンジン健全性チェック | `engine`, `model` |
| `wait` | 待機 | `sec` |
| `set` | 変数設定 | `var`, `value` |
| `escalate` | supervisor への判断依頼 | `question`, `guidance`, `expect`, `include`, `timeoutSec`, `fallback` |
| `fail` / `succeed` | 明示的な terminal | — |
| `subtree` | 別 BT の埋め込み | — |

条件として使えるノード: `fileExists`, `fileNewer`, `outputMatches`, `exitCode`, `jsonValue`, `var`, `nodeState`, `failure`。`failure` は失敗診断の `class` / `cause` で一致させる。

### 4.1 exec / agent の success 判定

`success` オブジェクトで指定する（既定は終了コード 0）。

- `exitCode`
- `outputRegex`（ログ末尾 200 行）
- `filesNewer`（試行開始時刻より新しいファイル）
- `testReport`（JUnit/NUnit XML）
- `contentFile` + `contentRegex` + `minBytes`
- `engineResult`（LLM の最終結果がエラーでないこと）

`idle` は `outputSec` と `cpuSec` の両方を指定する。`firstEventSec` は起動直後の無出力を `startup_hang` として早期失敗にする。

### 4.2 エンジン切り替え / fallback

- `engines`: `[{"engine":"agy","model":"..."}, ...]` の承認済み候補。
- `switchEngineOn`: `startup_hang`, `auth_error`, `probe_failed`, または `{"class":"fix_no_progress","after":1}` のような閾値付きクラス。
- `escalate.fallback`: `{"afterSec": 3600, "engine": "...", "model": "..."}`。consult リストの承認済み行を順番に代理回答させ、全員 down の場合は実装側で停止する。

### 4.3 escalate

`escalate` だけが supervisor の判断を待つ。デフォルトの `timeoutSec` は 86400 だが、テンプレートでは 1800 を明示している。期限切れ時は `expect` に `stop` があれば自動で `stop` を回答し、無ければ `escalationTimeout` で失敗する。

判断パケットには `intent` / `question` / `guidance` / `expect` / 実行経路 / `include` で指定したログ末尾・失敗診断・変数・ファイルが入る。`bt resume --answer <a> --rationale "..."` で回答する。`takeover` は常に合法。`include` は必要最小限にする。

`question` / `guidance` は `${...}` 展開される。テンプレートでは `${bt.failedStep}`, `${bt.failedExitCode}`, `${bt.failedClass}`, `${bt.failedDurationSec}`, `${bt.failedLogTail}` を使って実際の失敗を埋め込む（未設定時は空文字）。
`bt.failedLogTail` 直近 8 行 / 最大 2000 バイト。

### 4.4 変数

`${name}` で展開する。主な組み込み変数:

- `bt.id`, `bt.runs`
- `${<node>.state}`, `.runs`, `.exitCode`, `.failureClass`, `.failureSummary`, `.likelyCauses`, `.suggestedAction`, `.retryAt`, `.engine`, `.model`, `.logFile`, `.jobId`, `.resultFile`, `.start`, `.answer`
- `${<node>.output.<jsonPath>}`（ワーカーの JSON 結果）
- `${foreach:...}`（起動時に展開）
- `${t.*}`（foreach の別名フィールド）

`{engine}` / `{model}` は LLM exec コマンド用のプレースホルダで、起動時に置換される。

## 5. 実行と状態

- `bt start --file <json> | --bt <json> [--paused true] [--task <id>]` で登録・開始。
- `bt wait --id <id>` は完了または `waitingForLlm`（escalate）で返る。
- `bt get` は `pendingEstimateSec` と `plannedTasks` を返す。未開始ノードは provisional `planned` 行としてダッシュボード / CLI / Web UI に出る。
- `bt retry --from <node>`、`bt resume`、`bt cancel`、`bt pause` で制御する。
- 状態ファイル: `<data-dir>/behaviortrees/<btId>.json`。イベント: `<logs>/bt-<btId>.events.jsonl`。
- 成果物: `<data-dir>/behaviortrees/<btId>/results/`。

## 6. routine とテンプレート

- routine は名前付き BT で `<cwd>/.hancho/routines/<name>.json`（project）または `<data-dir>/routines/<name>.json`。`routine run <name>` は実行ごとに `<name>-<yyyymmdd-hhmmss>` の id を作る。
- 組込テンプレート: `resilient-agent`, `card-sequence`, `test-suite`, `card`。`bt template <name>` で取得できる。
- `<...>` は利用者が置換するプレースホルダ。`{engine}` / `{model}` / `${t.*}` / `${foreach:...}` / `${bt.failed*}` は実行時プレースホルダ。
- テンプレートの `catch` は `fast_exit`（30 秒以内の終了コード 1）を 1 回だけ自動再試行し、その後 `otherwise` の escalate へ入る。`maxRetries: 1`。

## 7. 未実装 / 今後の拡張点

- BT 定義から `supersedes` を指定するフィールドは無い。direction 間の系譜は `direction redo` 等で表現する。
- direction を `sequence` / `selector` のような composite として定義する機能は未実装。子 direction の状態集約と実行制御を追加する必要がある。
- escalated の代理回答は `consult` リストに依存し、承認済み候補が無い場合は実装既定のエンジンを使う。
