# Hancho ビヘイビアツリー (BT) 設計案

> **関連資料:**
> - [ビヘイビアツリーガイド (日本語版)](behavior-tree-ja.md) / [English Guide](behavior-tree.md): 利用者・LLM 向け総合ガイド
> - [トークン消費削減ガイド](behavior-tree-token-optimization-ja.md): BT によるトークン削減設計とベストプラクティス

## 1. 目的

タスクキューは「順序・依存・再実行」を PC に移した。しかし **分岐の判断** はまだ LLM 側に残っている:

- ビルドが失敗したら → 修正エージェントを走らせて → もう一度ビルド (3 回まで)
- テストが通ったら → 次へ。落ちたら → ログからテスト名を拾って再実行
- `result.json` の `status` が `needs_review` なら → LLM に判断を仰いで止まる

こうした判断は機械的に決められるのに、LLM が毎回起こされ、結果を読み、次を決めている。判断を忘れる・取り違える・起こされないまま止まる、というタスクキュー以前と同じ失敗が起きる。

**目的:** LLM は「タスクの手順と分岐規則」を BT のデータとして 1 回書いて渡す。PC がそれを実行し、条件判定・分岐・ループ・再試行を行う。LLM が起こされるのは、BT が「機械的には決められない」と判断したとき (`escalate`) と、完了・失敗したときだけにする。

## 2. 役割分担

| LLM がやること | PC がやること |
|---|---|
| BT を書いて `bt_start` で渡す | BT の検証 (構文・上限・ChainGuard) |
| `escalate` で止まった BT を読んで判断し、`bt_resume` で値を返す | ノードの実行、条件の評価、分岐・ループ・再試行 |
| 完了・失敗した BT の結果を読む | 各アクションをタスクキューのジョブとして起動 (タイムアウト・成功判定・リソースロックはそのまま使う) |
| | 状態の永続化 (PC 再起動後も続きから)、通知 |

## 3. 設計原則

1. **アクションの実体はタスクキューのジョブ。** BT はジョブの起動順と分岐を決めるだけ。タイムアウト・idle 検出・成功条件・ツリー kill・リソースロック・ログ・GUI の一覧は既存の仕組みをそのまま使う。BT 専用のプロセス起動経路は作らない。
2. **イベント駆動。** ゲームの BT のように毎フレーム tick しない。ジョブの終了、待ち時間の満了、`bt_resume`、PC 起動時にだけ tick する。
3. **必ず止まる。** 無限ループは書けない。`repeat` / `retry` は回数上限が必須で、BT 全体にも実行ノード数と経過時間の上限がある。
4. **PC は中身を解釈しない。** 条件はファイル・終了コード・ログの正規表現・JSON の値・変数だけで判定する。意味の判断が必要なら `escalate` で LLM に返す。
5. **成功を書き換えられない。** タスクキューと同じく、MCP から条件ノードやアクションを成功扱いにはできない (`escalate` への回答は値を返すだけで、その値をどう扱うかは BT に書かれた条件が決める)。

## 4. データ形式 (JSON)

タスクキューと同じく JSON。ノードは `type` で種類を表す。`id` は BT 内で一意 (省略時は PC が `n1`, `n2` … を振る)。

```json
{
  "id": "feature-x",
  "summary": "Build, test, and fix feature X",
  "intent": "Make feature X build and pass tests without changing public APIs.",
  "taskqueue": "main",
  "cwd": "C:\\work\\proj",
  "limits": { "maxNodeRuns": 200, "maxWallSec": 21600 },
  "worker": { "engine": "agy", "model": "gemini-4-argon-high" },
  "vars": { "target": "Release" },
  "root": {
    "type": "sequence",
    "children": [
      { "type": "retry", "max": 3, "id": "build-loop",
        "child": {
          "type": "fallback",
          "children": [
            { "type": "job", "id": "build",
              "summary": "Build ${target}",
              "command": "cmake --build build --config ${target}",
              "timeoutSec": 1800,
              "capture": { "errFile": { "regex": "^(\\S+\\.cpp)\\(\\d+\\): error", "group": 1 } } },
            { "type": "sequence", "children": [
                { "type": "worker", "id": "fix",
                  "summary": "Fix build error in ${errFile}",
                  "prompt": "Fix the build error in ${errFile}. Log: ${build.logFile}",
                  "timeoutSec": 3600 },
                { "type": "fail" }
            ] }
          ] } },
      { "type": "job", "id": "test", "summary": "Run tests",
        "command": "ctest --test-dir build -C ${target}", "timeoutSec": 1800 },
      { "type": "if",
        "cond": { "type": "jsonValue", "file": "out/result.json", "path": "status", "eq": "needs_review" },
        "then": { "type": "escalate", "id": "review",
                  "question": "Approve the result of feature X?",
                  "guidance": "Approve if only known flaky tests failed; otherwise reject.",
                  "expect": ["approve", "reject"],
                  "include": { "logs": ["test"], "vars": ["target"], "files": ["out/result.json"] },
                  "timeoutSec": 7200,
                  "fallback": { "afterSec": 1800, "engine": "claude", "model": "claude-opus-5-5" } } }
    ]
  }
}
```

この例で PC がやること: ビルド → 失敗したらエラーのファイル名をログから拾って修正エージェントを起動 → 再ビルド (最大 3 回) → テスト → `result.json` を見て、必要なときだけ LLM に返す。LLM はその間一度も起こされない。

### 4.1 ノードの種類

**複合ノード**

| type | 動作 |
|---|---|
| `sequence` | 子を順に実行。1 つでも失敗したら失敗。全部成功で成功 |
| `fallback` | 子を順に実行。1 つでも成功したら成功。全部失敗で失敗 (別名 `selector`) |
| `parallel` | 子を同時に開始。`success: "all"` (既定) / `"any"` / 数値。決着したら残りをキャンセル。同時数はタスクキューの `concurrency` とリソースロックに従う |
| `if` | `cond` (条件ノード) が成功なら `then`、失敗なら `else` (省略時は成功扱い) |

**デコレーター**

| type | 動作 |
|---|---|
| `retry` | 子が失敗したら最初からやり直す。`max` 必須 |
| `repeat` | 子が成功する間繰り返す。`max` 必須。`until` (条件ノード) を満たしたら成功で抜ける |
| `invert` | 成功と失敗を入れ替える |
| `forceSuccess` / `forceFailure` | 結果を固定する |
| `timeout` | 子全体の時間制限 (`sec`)。超えたら実行中のジョブをキャンセルして失敗 |

**アクション (葉)**

| type | 動作 |
|---|---|
| `job` | タスクキューのジョブを 1 つ登録して起動。フィールドはタスクキューのジョブと同じ (`command`, `summary`, `timeoutSec`, `success`, `idle`, `resources`, `instructionsText` …)。ジョブが succeeded なら成功、failed/cancelled なら失敗 |
| `worker` | LLM ワーカーにプロンプトを渡して実行させる。コマンドラインは書かず、`engine` (省略時は BT の `worker.engine`、それもなければ PC の既定) と `model` で選ぶ。PC がエンジン定義 (4.3) からコマンドを組み立て、`job` と同じくタスクキューのジョブとして起動する。`prompt` / `promptFile`、`summary`、`timeoutSec` は必須。`success` / `capture` / `resources` は `job` と同じ。最終応答は `${<id>.result}` に、構造化した結果は `${<id>.output.<path>}` に入る (4.3) |
| `wait` | `sec` 秒待って成功 |
| `set` | 変数に値を入れて成功 |
| `escalate` | 機械的に決められない判断を呼び出し元の LLM に返す。その枝を止めて通知し、`bt_resume` を待つ。回答は変数 `${<id>.answer}` に入る。詳しくは 5.6 |
| `fail` / `succeed` | そのまま失敗 / 成功 |
| `subtree` | 別ファイルの BT を読み込んで実行 (`file`)。変数は親から引き継ぐ |

**条件 (葉、即時に判定)**

| type | 成功する条件 |
|---|---|
| `fileExists` | `path` (glob 可) が存在する |
| `fileNewer` | `path` が `than` (ファイル、または `${node.start}`) より新しい |
| `outputMatches` | ノード `node` の最後のアテンプトのログ末尾 (既定 200 行) が `regex` に一致 |
| `exitCode` | ノード `node` の最後の終了コードが `eq` / `in` |
| `jsonValue` | `file` を JSON として読み、`path` の値が `eq` / `ne` / `in` / `gt` / `lt` / `exists` を満たす |
| `var` | 変数が `eq` / `ne` / `in` / `gt` / `lt` / `exists` を満たす |
| `nodeState` | ノード `node` の最後の結果が `succeeded` / `failed` |

条件は即時に評価する (プロセスを起動しない)。コマンドの実行結果で判定したいときは、`job` を置いてその成否か `exitCode` を使う。

### 4.2 変数 (ブラックボード)

- `vars` で初期値を与える。`set`、`capture`、`escalate` の回答で書き換わる。
- `job` の `capture` で、ログの正規表現のグループ (`regex` + `group`) または JSON ファイルの値 (`jsonFile` + `path`) を変数に取り込む。
- PC が用意する変数: `${<node>.state}`, `${<node>.exitCode}`, `${<node>.logFile}`, `${<node>.jobId}`, `${<node>.runs}`, `${<node>.start}`, `${bt.id}`, `${bt.runs}`。
- `command` の中の `${var}` は、`{summary}` と同じく **1 個の引数としてクォートして** 置き換える (値に `&` などが入っても連鎖にならない)。`instructionsText` / `summary` / `path` 系はそのまま置き換える。
- 未定義の変数を参照したら、そのノードは失敗 (理由 `undefinedVar`)。

### 4.3 ワーカーエンジンの抽象化

BT は特定の LLM CLI に縛られない。`worker` ノードと `escalate` の `fallback` は、エンジン名とモデルだけを持つ。CLI ごとの違いは PC のエンジン定義が吸収する。

**エンジン定義 (エンジンレジストリ)**

PC は claude / agy / opencode / codex の定義を内蔵し、データディレクトリの `engines.json` で上書き・追加できる (例: 社内の CLI、同じ CLI の別設定)。

```json
{
  "default": "agy",
  "engines": {
    "agy":      { "command": "agy.exe -p {prompt} --output-format stream-json --model {model} {permissionArgs} --print-timeout {timeoutMin}m",
                  "defaultModel": "gemini-3.8-flash-medium", "stream": "agy",
                  "permissionArgs": "--dangerously-skip-permissions" },
    "claude":   { "command": "claude -p {prompt} --output-format stream-json --verbose --model {model} {permissionArgs}",
                  "stream": "claude", "permissionArgs": "--dangerously-skip-permissions" },
    "opencode": { "command": "opencode.cmd run --format json --model {model} {prompt}", "stream": "opencode" },
    "codex":    { "command": "codex exec --json --model {model} {permissionArgs} {prompt}",
                  "stream": "codex", "permissionArgs": "--dangerously-bypass-approvals-and-sandbox" }
  }
}
```

- `{prompt}`: 1 個の引数としてクォートした**短い起動文**。プロンプト本文は指示書ファイル (`{instructions}`、タスクキューの指示書と同じ仕組み) に書き、起動文は「`<指示書のパス>` を読んで実行せよ。結果は `PC_BT_RESULT_FILE` に書け」とする。CLI ごとの引数長の上限とクォートの違いを避けるため。
- `{model}`: ノードの `model` → BT の `worker.model` → エンジンの `defaultModel` の順。どれもなく、`command` に `{model}` があれば、その引数ごと省く (`--model {model}` の組を落とす)。
- `stream`: 出力の読み方 (`llm_stream` のパーサー)。最終応答・トークン数・状態の取り出しに使う。
- `permissionArgs`: 確認なしで動かすための引数。エンジン定義の側だけに置き、BT からは指定できない (BT を書く LLM が権限を広げられないようにする)。
- `engine` / `model` に、エンジン定義にない名前を書いたら、検証 (`bt_validate` / `bt_start`) でエラーにする。

**結果の受け取り (エンジンに依存しない形)**

- 最終応答のテキスト: エンジンの `stream` パーサーが取り出した最後の応答を `${<id>.result}` に入れる (先頭 64 KB まで)。
- 構造化した結果: PC はワーカーごとに `PC_BT_RESULT_FILE` (環境変数) を渡す。ワーカーがそこに JSON を書けば、`${<id>.output.<path>}` で参照でき、条件ノード `jsonValue` でも `"file": "${<id>.resultFile}"` で判定できる。`worker` の `outputSchema` (JSON Schema の一部: `required` と `type`) を書けば、PC が満たしているかを確かめ、満たさなければそのノードは失敗 (`outputInvalid`) にする。
- 成否: 既定は「プロセスが終了コード 0 で終わり、`outputSchema` があれば結果ファイルがそれを満たす」。`success` で `job` と同じ条件を足せる。

**BT のコアとの境界**

- BT のコアはエンジン名を解決しない。`worker` ノードは `BtWorkerSpec` (engine, model, prompt 本文, summary, timeoutSec, success, resources, outputSchema) としてランナーに渡す。コマンドラインの組み立て、指示書の書き出し、`PC_BT_RESULT_FILE` の用意は、ランナー側の `WorkerEngineRegistry` (`src/worker_engines.{h,cpp}`) が行う。
- 検証のためにコアが知る必要があるのは「そのエンジン名とモデルが受け付けられるか」だけなので、コアはそれを問い合わせるインターフェース (`IBtEngineCatalog`) だけを受け取る。テストでは偽のカタログを使う。

## 5. 実行モデル

### 5.1 状態

BT 全体: `running` / `waitingForLlm` (どこかの枝が `escalate` で止まっている) / `succeeded` / `failed` / `cancelled` / `paused`。
ノード: `idle` / `running` / `succeeded` / `failed` / `skipped`。

### 5.2 tick

- 次のときだけ tick する: 開始時、ジョブが終端状態になったとき (タスクキューのイベントを購読)、`wait` / `timeout` / `retryDelay` の時刻、`bt_resume`、`bt_retry`、PC 起動時。
- tick はルートから降りて、`running` の葉まで進む。新しく走らせる葉に当たったら起動して止まる。終わった葉の結果を親に返し、親が次を決める。
- 実行中の `sequence` の前の子を毎 tick 再評価する「reactive」型は第 1 段階では作らない (条件が変わったら中断する、は `timeout` と `parallel` の `any` で足りる想定)。

### 5.3 ジョブとの対応

- `job` ノードを実行するたびに、BT の `taskqueue` (既定 `bt-<BT id>`) にジョブ `<BT id>.<node id>.<n>` を登録する。`n` は `retry` / `repeat` で再実行されるたびに 1 増える。
- ジョブ側に `bt: { id, node, run }` を持たせ、`taskqueue_get` (MCP / CLI) から BT に戻れるようにする。GUI には出さない (8 章)。
- タスクキュー自身の再実行 (`maxAttempts`) は、BT のジョブでは既定 1。インフラ的な失敗 (PC 再起動による `interrupted`、`deferred`) は今と同じく数えない。「失敗したらどうするか」は BT の `retry` / `fallback` に書く。
- BT 外から BT のジョブを `taskqueue_retry` / `taskqueue_update` するのは拒否し、`bt_retry` を使うよう返す (BT の状態とずれないように)。`taskqueue_cancel` は許可し、ノードの失敗として扱う。

### 5.4 上限

- `retry` / `repeat` の `max` は必須 (上限 100)。
- BT 全体: `limits.maxNodeRuns` (既定 200、上限 10000) のジョブ起動回数、`limits.maxWallSec` (既定 24 時間)。超えたら失敗 (理由 `limitExceeded`) で通知。
- ツリーの深さ 32、ノード数 1000、`subtree` の入れ子 8 まで。

### 5.5 失敗と再開

- BT が失敗したら、`bt_get` で「どのノードで、なぜ」がたどれる (失敗したノードの経路、最後のジョブのアテンプトとログ末尾)。
- `bt_retry <BT id> [--from <node id>]`: 失敗したノードから再開する。既定では最後に失敗した葉を `idle` に戻し、親の `retry` などの回数はそのまま。`--reset-counters` で回数も戻す。
- 定義の修正: `bt_update` で `running` でないノードの定義を差し替えられる (例: `command` の修正)。ツリーの形 (子の追加・削除) は変えられない。形を変えたいときは新しい BT として出し直す。

### 5.6 escalate: 判断を呼び出し元の LLM に返す

判断するのは、原則として BT を出した呼び出し元の LLM。PC は判断しない。

**流れ**
1. 呼び出し元は `bt_start` の後、`bt_wait` (ロングポール。時間切れなら呼び直す) で待つ。
2. `escalate` に着いたら、PC はその枝だけを止め (並列の他の枝は続ける)、判断パケット (下記) を保存し、`bt_wait` を返す。`escalated` イベントを記録し、`notifyFile` を更新する。
3. 呼び出し元は判断パケットを読んで `bt_resume` (`node`, `answer`, `rationale`, 任意の `vars`) を返す。`expect` にない回答は拒否する。
4. PC は回答を `${<id>.answer}` に入れ、その枝を再開する。`escalate` ノード自体は回答を受け取れば成功。回答をどう使うかは後ろの `if` などに書く。

**コンテキストの扱い: 判断パケットだけで判断できるようにする**

呼び出し元の会話のコンテキストには頼らない (待っている間に圧縮される、セッションが終わる、何時間も後に聞かれる、ことがあるため)。判断に必要な材料は PC が集めて渡す。

BT を書くときに LLM が「後の自分」に向けて書いておくもの:

| フィールド | 内容 |
|---|---|
| `intent` (BT 全体) | この作業全体の目的。1 段落 |
| `question` (必須) | 何を判断してほしいか |
| `guidance` | 判断の基準 |
| `expect` | 許す回答の一覧。省略時は自由回答 |
| `include` | 添える材料。`logs` (ノード id。最後のアテンプトのログ末尾 200 行)、`vars`、`files` (cwd 相対、1 ファイル 64 KB まで) |
| `timeoutSec` | 回答がないまま過ぎたらノードは失敗 (理由 `escalationTimeout`)。既定 24 時間 |
| `fallback` | 下記 |

PC が escalate の時点で作る判断パケット (`bt_wait` / `bt_get` が返し、`behaviortrees/<id>/escalations/<node>.<n>.json` と `.md` に保存する):
- `intent`, `question`, `guidance`, `expect`
- ルートからこのノードまでの経路と、通ってきたノードの結果・回数 (例: `build-loop 3/3 failed`, `fix 2 runs`)
- `include` の材料。その時点のスナップショットとして保存する (後でファイルが変わってもぶれない)
- 変数の現在値
- 回答のしかた (`bt_resume` の呼び方)

**回答者の 3 つの場合**

| 呼び出し元 | 回答のしかた |
|---|---|
| `bt_wait` で待っている | 同じ会話で判断パケットを読んで答える。会話のコンテキストも使えるので最も精度が高い |
| ターンを終えて寝ている | `notifyFile` (Claude Code の Monitor / フック等) で起こされ、判断パケットを読んで答える |
| もういない | `fallback` (`afterSec`, `engine`, `model`) があれば、`afterSec` 秒回答がないとき PC が LLM ワーカーを起動する。`worker` ノードと同じ仕組み (4.3) で、判断パケットの `.md` を指示書として渡す。ワーカーは `PC_BT_RESULT_FILE` に `{ "answer": ..., "rationale": ... }` を書いて答える (`bt_resume` と同じ検証をする)。ワーカーは会話のコンテキストを持たないので、判断パケットだけが頼り。エンジンは呼び出し元と違ってよい |

- 回答には短い `rationale` を必須とし、`escalate` の記録に残す。後から「なぜこう進んだか」をたどれるようにするため。
- 1 つの `escalate` に回答者は 1 人。`bt_resume` が先に来たら `fallback` のワーカーはキャンセルする。

## 6. API

### MCP
| ツール | 内容 |
|---|---|
| `bt_validate` | 実行せずに検証だけ (構文、上限、ChainGuard、未定義変数の静的検出) |
| `bt_start` | BT を登録して開始。`bt` (JSON) か `file` |
| `bt_get` | ノードごとの状態・回数・変数・実行中のジョブ。`compact` で木の 1 行表示 |
| `bt_list` | BT 一覧と状態 |
| `bt_wait` | 状態の変化をロングポール (`taskqueue_wait` と同じ形)。`escalate` に着いたとき (判断パケットを含めて返す) と終了で返る |
| `bt_resume` | `escalate` に回答する (`node`, `answer`, `rationale`, 任意の `vars`) |
| `bt_retry`, `bt_update`, `bt_cancel`, `bt_pause` | 5.5 の通り |

### CLI
`hancho-cli bt validate|start|get|list|wait|resume|retry|update|cancel|pause` (MCP と同じ引数、`--json`)。

### 通知
- `logs/bt-<id>.events.jsonl` に `nodeStarted` / `nodeFinished` / `varSet` / `escalated` / `btFinished` を記録。
- `notifyFile` (BT 単位で指定可) を `escalated` / `btFailed` / `btSucceeded` で更新する。LLM はこれ (または `bt_wait`) で起きる。

## 7. 永続化

- `<PC データディレクトリ>/behaviortrees/<id>.json` に、定義 (登録時の版) と実行状態 (ノード状態・回数・変数・ノードとジョブの対応) を保存する。
- 形式: BT の定義そのものに、BT 全体の実行状態 (`state`, `nodeRuns`, `vars`, `createdAt`, `updatedAt`) と、各ノードの `status` を足したもの。デバッグ表示 (8.1) はこのファイルだけを読む。

```json
{
  "id": "feature-x", "summary": "...", "intent": "...", "taskqueue": "bt-feature-x",
  "state": "running", "nodeRuns": 5, "limits": { "maxNodeRuns": 200 },
  "vars": { "target": "Release", "errFile": "src/a.cpp" },
  "createdAt": "...", "updatedAt": "...",
  "root": { "type": "sequence", "status": { "state": "running" }, "children": [
    { "type": "retry", "id": "build-loop", "max": 3, "status": { "state": "running", "runs": 2 },
      "child": { "type": "job", "id": "build", "summary": "...", "command": "...",
                 "status": { "state": "failed", "runs": 2, "jobs": ["feature-x-1", "feature-x-3"],
                             "detail": "exit 1" } } }
  ] }
}
```

- ノードの `status`: `state` (`idle` / `running` / `succeeded` / `failed` / `skipped`)、`runs` (実行回数)、`jobs` (このノードが起動したジョブ id、古い順)、`detail` (直近の結果の理由)、`escalate` では `answer` / `rationale`。まだ実行していないノードは `status` を持たない。
- 状態が変わるたびに保存 (一時ファイルに書いて置き換え)。PC 起動時に読み込み、`running` だったジョブはタスクキューの `interrupted` 処理に任せ、その結果を受けて tick を続ける。
- 終了した BT は、タスクキューの purge と同じ規則で古いものを消せる。

## 8. ユーザー (人間) からの見え方

**BT は LLM が書いて PC が実行する仕組み。ユーザー (開発者) は普段は意識しなくてよいが、どういう BT が動いているかは見られるようにする** (当初は完全に隠す方針だったが、使うのは開発者なので BT の中身を見たいという判断で変更した)。

- BT を見る入口は、ツールバーの **BT** ボタン (Ctrl+Shift+B) と、BT 由来のジョブの右クリックメニュー「ビヘイビアツリーで表示」。開くのは 8.1 のビュー。
- タスク一覧そのものには BT 専用の列や用語は足さない。
- BT が起動したジョブは、タスク一覧に**普通のジョブとして**並ぶ。表示はジョブの `summary` だけで、BT id / ノード id は出さない (ジョブ id にも出さないよう、id は `<BT summary から作った短い名前>-<連番>` のような普通の形にする)。
- まだ起動していない後続のノードはタスク一覧に出さない (分岐次第で走らないかもしれないため)。ユーザーから見ると「次のタスクが順に現れて流れていく」だけになる。
- `escalate` は LLM への通知であり、ユーザーには何も表示しない。
- ユーザーがジョブを手で操作した場合 (キャンセル、GUI の再実行・強制成功) は、PC がそれを BT の該当ノードの結果として受け取る。ユーザーは BT の存在を知らないまま普段通りに操作できる (5.3 の「BT 外からの `taskqueue_retry` は拒否」は MCP / CLI のみ。GUI 操作は拒否せず、`bt_retry` 相当に読み替える)。
- BT の書き方は LLM 向けの文書 (MCP ツールの説明、`--help` / `hancho-cli help`、`doc/behavior-tree.md`) に置く。help は LLM が読む前提のものなので BT の使い方を書く。

### 8.1 ビヘイビアツリーのビュー

タスクと BT の関係を TreeView で表示するウィンドウ (タイトル「ビヘイビアツリー」)。ツールバーの **BT** ボタン、**Ctrl+Shift+B**、BT 由来のジョブの右クリック「ビヘイビアツリーで表示」で開く。

- 左: TreeView。最上位が BT (`[state] id "summary" runs n/max 更新時刻`)、その下にノードの木。各行は `[state] (cond:/then:/else:/until:) type id` に、種類ごとの要点 (`retry` の回数 `2/3`、`job` の summary と最後のジョブ id、`escalate` の質問と回答、条件ノードの判定内容、`status.detail`) を付ける。ジョブ id の後ろにはタスクキュー上のジョブの現在の状態を付ける。
- 行の背景色は状態で分ける (running / failed / succeeded はタスク一覧と同じテーマ色)。
- 右: 選んだ行の詳細。ノードの定義と `status` (子ノードを除いた JSON)、続けて `jobs` の各ジョブの `taskqueue_get` の結果。
- タスク一覧でジョブを選んだ状態で開くと、そのジョブを起動したノードを選んで表示する。
- 2 秒ごとに `behaviortrees/*.json` を読み直す。内容が変わったときだけ木を作り直し、開閉状態と選択は保つ。
- ボタン: 再読み込み、詳細をコピー、フォルダーを開く、閉じる。読めなかったファイルは下の行に出す。

## 9. 実装段階

1. **コア:** `src/behavior_tree.{h,cpp}` (データモデル、JSON 読み書き、検証、tick、変数展開)。GUI・プロセスに依存しない純粋ロジックとし、ジョブとワーカーの起動と終了通知はインターフェース越しに受ける。エンジン名の可否は `IBtEngineCatalog` に問い合わせる。
1b. **エンジンレジストリ:** `src/worker_engines.{h,cpp}` (内蔵の定義、`engines.json` の読み込み、コマンドラインの組み立て、結果ファイルと `outputSchema` の確認)。純粋ロジックとし、`tests/worker_engines_smoke.cpp` で 4 エンジンのコマンド組み立てとクォートを試す。`tests/behavior_tree_smoke.cpp` で、偽のジョブ実行器を使って全ノード型・上限・再開・永続化からの復帰を試す。
2. **タスクキュー連携:** `TaskQueueJob` に `bt` 参照を追加、ジョブ終端イベントで BT に通知、BT 由来のジョブへの `taskqueue_retry` 拒否。
3. **MCP / CLI / IPC:** 6 章のツール。`doc/behavior-tree.md` / `-ja.md` を LLM 向けに追加する (README と features からは参照しない)。
4. **GUI:** 8 章の通り、BT 由来のジョブを普通のジョブとして表示し、GUI 操作を BT に読み替える。BT 専用の画面は作らない。
5. **LLM 向けの書き方ガイド:** `tools/agents/` に BT の書き方と例 (ビルド修正ループ、テスト → 修正、レビュー待ち) を入れ、エージェントが手で順番を回す代わりに BT を出すよう促す。

## 10. 決めておきたいこと

| 項目 | 案 (推奨) | 他の選択肢 |
|---|---|---|
| アクションの実体 | タスクキューのジョブ (3 章 原則 1) | BT 専用の実行器を作る (タイムアウト・リソースロック・GUI を作り直すことになる) |
| 形式 | JSON (タスクキューと揃える) | BehaviorTree.CPP 互換の XML、YAML |
| LLM への返し方 | `escalate` ノード + `bt_wait` / `notifyFile` | BT が失敗したときだけ返す (分岐の途中で聞けない) |
| reactive 型 | 第 1 段階では作らない | 最初から `reactiveSequence` を入れる |
| BT のジョブの `maxAttempts` | 既定 1 (再試行は BT に書く) | タスクキューの既定 (2) を使う |
