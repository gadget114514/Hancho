# LLM エージェント向けビヘイビアツリーガイド (`bt_*`)

Hancho のビヘイビアツリー (BT) は、条件分岐 (`if`/`fallback`)、回数制限つきの修正ループ (`retry`/`repeat`)、LLM ワーカー呼び出し (`worker`)、ファイル・JSON・ログ判定、判断エスカレーション (`escalate`) を含む複数段の作業を 1 つの JSON 定義として登録し、ステップ間のトークン消費なしに Hancho 側で自動進行させる仕組みです。

> **関連資料:**
> - [トークン消費削減ガイド](behavior-tree-token-optimization-ja.md): BT によるトークン削減原理と `compact: true` 等のベストプラクティス
> - [設計・実装計画書](behavior-tree-plan.md): 内部アーキテクチャ・設計思想・統合計画
> - [English Guide](behavior-tree.md)

---

## 1. `bt_*` と `taskqueue_add` の使い分け

| 状況 | 推奨ツール |
|---|---|
| 単発コマンド、または `dependsOn` (`success` / `done`) だけで表現できる固定 DAG | `taskqueue_add` |
| `if`/`else` 分岐、ビルド・修正・再ビルドの反復 (`retry`/`repeat`)、`worker` による LLM 実行、構造化出力の検証 (`jsonValue`, `outputMatches`)、途中の判断待ち (`escalate`) を伴う作業 | `bt_start` + `bt_wait` (+ `bt_resume`) |

**呼び出し元 LLM の基本フロー:**
1. `bt_validate` / `bt_start` (または CLI `hancho-cli bt start --file <path>`) で BT を登録・開始する。
2. `bt_wait` (`id`, `sinceVersion`, `timeoutMs`, `compact`) で終端状態 (`succeeded`, `failed`, `cancelled`) または `waitingForLlm` になるまでロングポールする。`bt_wait` は終了や `escalate` ではすぐ返るので、`timeoutMs` を長くしても判断が遅れることはない。先に `timeoutMs` が過ぎた場合、応答はツリーを含まない `{state, version, nodeRuns, timedOut: true}` だけになる。トークン消費を抑えるため、終了時にツリー全体の JSON が不要な場合は `compact: true` を指定すると `tree` フィールドが省略され、1行サマリ `compact` のみ返る。既定は MCP ツールで `600000` (10 分)、`hancho-cli bt wait` で `60000`。負の値なら無制限に待つ。MCP クライアントが呼び出しを取り消した場合 (`notifications/cancelled`。クライアント自身のツールタイムアウトなど)、待ちは約 2 秒以内に止まり応答は返さない。そのためクライアントが先に諦めても、長い `timeoutMs` で問題ない。
3. `state == "waitingForLlm"` で返ったら、レスポンス内の `escalation` 判断パケットを読み、`bt_resume` に `id`, `node`, `answer`, 必須の `rationale` (および任意の `vars`) を渡して再開する。
4. 失敗 (`failed`) 後に外部要因を直すか `bt_update` で非 `running` ノードの定義を修正した場合は、`bt_retry` で失敗ノードから再開する。

**応答の大きさ。** 応答はすべて LLM が読むので、状態ドキュメント全体を返すのは `bt_get` (`compact` なし) だけです。
- `bt_start`、`bt_resume`、`bt_retry`、`bt_update`、`bt_cancel`、`bt_pause`、`compact: true` の `bt_get`、`compact: true` の `bt_wait` は `{ok, id, state, detail, version, nodeRuns, taskqueue, compact}` を返し、`waitingForLlm` のときは `escalation` も付く。`tree` は含まない。
- `bt_wait` (`compact: false`、終了や `escalate` で返ったとき) はこれに `tree` を加える。ただし 200 バイトを超える変数の値やノードの `prompt` / `instructionsText` は、先頭部分と `...[truncated, N bytes]` に切り詰められる。
- `bt_get` は変数をすべて省略せずに `tree` を返す。

---

## 2. トップレベル JSON 形式

```json
{
  "id": "feature-auth",
  "summary": "Build, test, and verify auth module",
  "intent": "トークン更新処理を実装し、既存の公開 API を壊さずに全単体テストを通す。",
  "taskqueue": "bt-feature-auth",
  "cwd": "D:/repo",
  "notifyFile": "artifacts/bt-notify.json",
  "limits": {
    "maxNodeRuns": 200,
    "maxWallSec": 86400
  },
  "worker": {
    "engine": "claude",
    "model": "sonnet"
  },
  "vars": {
    "config": "Release",
    "target": "auth_tests"
  },
  "root": { "...": "..." }
}
```

### トップレベルフィールド一覧

| フィールド | 必須 | 既定値 | 内容 |
|---|---|---|---|
| `id` | **必須** | — | BT の一意な非空文字列 ID。 |
| `root` | **必須** | — | ルートノードオブジェクト。 |
| `summary` | 任意 | `""` | BT 全体の 1 行要約。 |
| `intent` | 任意 | `""` | 作業全体の目的 (1 段落)。`escalate` の判断パケットに含まれる。 |
| `taskqueue` | 任意 | `"bt-" + id` | BT のジョブを登録するタスクキュー名。`parallel` の子はこの名前に `.p<hash>` を付けた専用キューを使う。 |
| `cwd` | 任意 | `""` | 各ノードおよび相対パスの基準作業ディレクトリ。 |
| `notifyFile` | 任意 | `""` | `escalated` / `btFailed` / `btSucceeded` 発生時にアトミック更新される通知ファイル。 |
| `limits.maxNodeRuns` | 任意 | `200` | BT 全体の `job` + `worker` 起動上限 (`1..10000`)。 |
| `limits.maxWallSec` | 任意 | `86400` | BT 全体の経過時間上限 (秒、`> 0`)。超過時は `limitExceeded` で失敗。 |
| `worker.engine` | 任意 | レジストリ既定 (`agy`) | `worker` および `escalate.fallback` の既定エンジン名。 |
| `worker.model` | 任意 | エンジンの既定モデル (`agy` は `gemini-3.8-flash-medium`) | 既定エンジン使用時の既定モデル名。 |
| `defaults` | 任意 | — | 明示値の無いノードに補完する値: `worker.idle`, `worker.maxAttempts`, `worker.probe`, `job.idle`。明示値 (明示 `0` を含む) が常に優先される。§5.3 参照。 |
| `vars` | 任意 | `{}` | 初期変数マップ (文字列・真偽値・数値は文字列化して保持)。 |

---

## 3. ノード型リファレンス

すべてのノードは必須の `"type"` と任意の `"id"` (ツリー内で一意) を持つ JSON オブジェクトです。
- `"id"` を省略した場合、JSON パス (`"root"`, `"root.children[0]"`, `"root.children[1].child"` など) がそのままノード識別子 (`pathKey` / `nodeKey`) になります。
- API (`bt_resume`, `bt_retry`, `bt_update`) や条件ノード (`exitCode`, `outputMatches`, `nodeState`) では `<nodeId>`、`<pathKey>`、または `<btId>/<nodeKey>` で指定できます。

### 3.1 複合・制御ノード

| `type` | 子スロット | フィールドと動作 |
|---|---|---|
| `sequence` | `"children": [...]` (非空配列) | 子を順番に実行。すべて成功すれば `succeeded`、1 つでも失敗した時点で `failed`。 |
| `fallback` (別名 `selector`) | `"children": [...]` (非空配列) | 子を順番に実行し、最初に成功した時点で `succeeded`。すべて失敗したときのみ `failed`。 |
| `parallel` | `"children": [...]` (非空配列) | 全子ノードを同時起動。任意 `"success"`: `"all"` (既定)、`"any"`、または整数 `1..N` (成功が必要な子の数)。結果確定時に残りの実行中子ノードをキャンセル。子ごとに専用のタスクキュー（ツリーIDと分岐から決まる）が割り当てられ、実際に並列実行される（リソースロックが競合すれば待機）。 |
| `foreach` | `"glob"` (文字列), `"as"` (単純識別子), `"do"` (ノードの雛形) | `"glob"` に一致するファイル 1 件ごとに `"do"` を展開して実行する (パスは `cwd` 基準。`*` / `?` はセグメント内、`**` は複数セグメントに一致)。任意 `"mode"`: `"sequence"` (既定、最初の失敗で停止)、`"all"` (全子を流してから 1 つでも失敗なら失敗)、`"parallel"` (子ごとに別キュー)。任意 `"exclude"` (相対 glob の配列) と `"minMatches"` (一致数が少なければ失敗)。`"do"` 内では `${<as>.stem}` / `${<as>.name}` / `${<as>.path}` / `${<as>.index}` (0 起点) が使える。一致ファイルが変わると次回展開で子が作り直される。 |
| `if` | `"cond"`, `"then"`, 任意 `"else"` | `"cond"` を評価し、成功なら `"then"`、失敗なら `"else"` (省略時は即 `succeeded`) を実行。 |
| `catch` | `"child"` (オブジェクト), `"handlers"` (非空配列) | `"child"` を実行し、失敗したら最初に一致した handler の `do` を実行する (`class` は文字列・配列・`"*"`、`cause`、両方指定時は AND)。`"thenRetry": true` なら `"child"` を `"maxRetries"` 回まで再実行 (既定 `1`、`1..10`)。`"otherwise"` は `"fail"` (既定) またはノードオブジェクト。§11.3 参照。 |
| `retry` | `"child"` | 必須 `"max"` (`1..100`)、任意 `"delaySec"` (または `"retryDelaySec"`, 既定 `0`)。`"child"` が失敗する間、最大 `"max"` 回まで実行する。 |
| `repeat` | `"child"`, 任意 `"until"` | 必須 `"max"` (`1..100`)、任意 `"delaySec"` (既定 `0`)。`"child"` が成功するたびに反復。`"until"` がある場合は各成功後に `"until"` を評価し、`"until"` が成功すれば `succeeded` (`"max"` 回以内に `"until"` が成功しなければ `failed`)。`"until"` 省略時は `"max"` 回成功で `succeeded`。`"child"` が失敗したら即 `failed`。 |
| `invert` | `"child"` | `"child"` の `succeeded` と `failed` を反転する。 |
| `forceSuccess` | `"child"` | `"child"` の終了後、結果を常に `succeeded` にする。 |
| `forceFailure` | `"child"` | `"child"` の終了後、結果を常に `failed` にする。 |
| `timeout` | `"child"` | 必須 `"sec"` (`> 0`)。`"sec"` 秒以内に終わらなければ `"child"` をキャンセルし `detail: "timeout"` で失敗。 |

*(注: `"subtree"` は現段階では未実装であり、`bt_validate` / `bt_start` 時に `"subtree is not supported yet"` で拒否されます。)*

### 3.2 アクション葉ノード

| `type` | 必須フィールド | 任意フィールドと動作 |
|---|---|---|
| `job` | `"command"`, `"summary"`, `"timeoutSec"` (`> 0`) | 任意: `"estimateSec"`（残り時間・ETA 表示用の見積もり秒数。タイムアウトは別に適用）、`"cwd"`, `"env"`, `"idle"` (`{outputSec, cpuSec}`), `"success"` (`{exitCode, filesNewer, outputRegex, testReport, contentFile, minBytes, contentRegex, engineResult}`), `"resources"` (`{write:[...], read:[...]}`), `"instructionsText"`, `"instructionsFile"`, `"maxAttempts"` (既定 `1`), `"capture"`。タスクキューに `<btId>-<ノードID>-<実行回>` のジョブ ID で登録して起動。起動時に `ChainGuard` が適用され、`&&` / `||` / `;` 等のシェル連結は `launchFailed` となる。 |
| `worker` | `"summary"`, `"timeoutSec"` (`> 0`), `"prompt"` または `"promptFile"` | 任意: `"engine"`, `"model"`, `"engines"` (候補列。`engine`/`model` とは同時指定不可), `"switchEngineOn"` (次の候補へ進む失敗クラス), `"probe"` (本番前の健康診断), `"outputSchema"` (JSON Schema オブジェクトまたは文字列), `"cwd"`, `"env"`, `"idle"`, `"success"`, `"resources"`, `"maxAttempts"`, `"capture"`。エンジンレジストリ経由で LLM ワーカーを起動する。§5.3 / §5.4 参照。 |
| `probe` | `"engine"` | 任意: `"model"`, `"timeoutSec"` (既定 `60`)。stdin を閉じた状態で最小プロンプトを送り、最初の出力があり正常終了すれば成功。失敗は常に `probe_failed` (元の分類は `detail` に記載)。結果は `<dataDir>/probes.json` にキャッシュ (成功 10 分、失敗 2 分)。 |
| `wait` | `"sec"` (`>= 0`) | `"sec"` 秒待ってから `succeeded` になる。 |
| `set` | `"var"` (または `"name"`), `"value"` | `"value"` 内の `${...}` を展開して `vars[var]` に格納し、`varSet` イベントを出して即 `succeeded`。 |
| `escalate` | `"question"` | 任意: `"guidance"`, `"expect"` (許可する回答文字列配列), `"include"` (`{logs:[nodeIds], vars:[varNames], files:[relPaths], lines:100, maxBytes:16384}`), `"timeoutSec"` (既定 `86400`), `"fallback"` (`{afterSec, engine, model}`)。この枝を止め、ツリー状態を `waitingForLlm` にして判断パケットを保存し、`bt_resume` (または `fallback`) を待つ。回答 `"takeover"` は `expect` に無くても常に受け付けられ、ノードに `takenOver: true` と `takeover` オブジェクトを記録する（ノード自身は `succeeded`）。`bt_get` / `compact` とダッシュボードに表示される。 |
| `fail` | *(なし)* | 任意 `"detail"` (または `"reason"`, 既定 `"fail node"`)。指定理由で即 `failed`。 |
| `succeed` | *(なし)* | 即 `succeeded`。 |

### 3.3 条件葉ノード

条件葉ノードは評価と同じ tick で即座に `succeeded` (真) または `failed` (偽) になります。`jsonValue` と `var` で使える比較演算子: `"eq"`, `"ne"`, `"in"` (配列), `"gt"`, `"lt"`, `"exists"` (真偽値)。

| `type` | 必須フィールド | 動作 |
|---|---|---|
| `fileExists` | `"path"` | `cwd` 基準のファイルが存在すれば成功。末尾の要素では単純な glob（例: `*.json`）も使えます。 |
| `fileNewer` | `"path"`, `"than"` | `"path"` が `"than"` (別ファイルパス、または `"${build.start}"` のような ISO-8601 時刻文字列) より新しければ成功。 |
| `outputMatches` | `"node"`, `"regex"` | 任意 `"lines"` (既定 `200`)。指定ノードの直近ログ末尾 `"lines"` 行に正規表現がマッチすれば成功。 |
| `exitCode` | `"node"`, および `"eq"` / `"ne"` / `"in"` のいずれか | 指定ノードの直近終了コードを比較。 |
| `failure` | `"node"`, および `"class"` (文字列または配列) と／または `"cause"` (文字列) | 参照先ノードが失敗し、失敗クラスおよび／または `likelyCauses[].id` が一致すれば成功。両方指定時は AND。少なくとも一方は必須。未知のノード id は検証エラー。未実行・成功ノードは常に false。§11.2 参照。 |
| `jsonValue` | `"file"`, `"path"`, 比較演算子 (`eq`/`ne`/`in`/`gt`/`lt`/`exists`) | JSON ファイルの `"path"` (例 `"summary.failed"`, `"items[0].name"`) の値を比較。 |
| `var` | `"name"` (または `"var"`), 比較演算子 (`eq`/`ne`/`in`/`gt`/`lt`/`exists`) | `vars[name]` の現在値を比較 (両辺が数値として解釈できる場合 `gt`/`lt` は数値比較)。 |
| `nodeState` | `"node"`, および `"eq"` (または `"state"` / `"ne"` / `"in"`) | 指定ノードの現在状態 (`idle`, `running`, `succeeded`, `failed`, `skipped`) を比較。 |

---

## 4. 変数 (`${...}`)、クォート規則、`capture`

### 4.1 `${...}` の展開と `command` での自動クォート
- ノード実行時に文字列フィールド (`command`, `summary`, `cwd`, `env`, `prompt`, `instructionsText`, `set.value`, `fail.detail`, `escalate.question`/`guidance`, 条件ノードのパス・比較値) 内の `${name}` を展開します。
- **未定義変数の検出**: ノードの実行・評価時に未定義の `${name}` があると、そのノードは `detail: "undefinedVar: <name>"` で即座に失敗します。
- **`command` での自動クォート**: `job` の `"command"` 内に書かれた `${...}` は、`QuoteCommandArg` により単一の Windows コマンドライン引数として自動的にクォートされます (空白や特殊文字を含む値でも引数が分裂しません)。一方、`prompt`・`summary`・`set.value`・条件式など `"command"` 以外のフィールドではクォートを付けずにそのまま展開されます。
- **一覧展開 `${foreach:<glob>:<field>:<sep>}`**: 起動時に `<glob>` を展開し、一致ごとの `<field>` (`stem` / `name` / `path` / `index`) を `<sep>` (既定は空白 1 つ) で連結します。各値は別のコマンド引数として扱われ、自動クォートは適用されません。`foreach` ノードの `exclude` は効かないため、一部を除外したい場合は glob 自体を絞ってください。

### 4.2 自動設定されるノード変数
ノード `<node>` の実行開始・終了時に以下の変数が自動設定されます。

| 変数 | 対象ノード | 内容 |
|---|---|---|
| `${<node>.start}` | `job`, `worker` | アテンプト開始時刻 (ISO-8601 UTC)。`fileNewer` の `"than"` にそのまま渡せる。 |
| `${<node>.exitCode}` | `job`, `worker` | 終了コード文字列 (例 `"0"`, `"1"`)。 |
| `${<node>.logFile}` | `job`, `worker` | アテンプトのログファイルパス。 |
| `${<node>.resultFile}` | `worker` | `PC_BT_RESULT_FILE` のパス (`<dataDir>/behaviortrees/<btId>/results/<jobId>.json`)。 |
| `${<node>.result}` | `worker` | ワーカーの最終応答テキスト (ストリームが無い場合はログ末尾)。 |
| `${<node>.output}` | `worker` | `PC_BT_RESULT_FILE` に書かれた生 JSON 文字列。 |
| `${<node>.output.<path>}` | `worker` | `PC_BT_RESULT_FILE` 内の任意のフィールド・配列要素 (例 `${w1.output.target}`, `${w1.output.files[0]}`)。 |
| `${<node>.answer}` | `escalate` | `bt_resume` または `fallback` ワーカーが返した回答文字列。 |
| `${<node>.failureClass}` | `job`, `worker` | 直近アテンプトの失敗クラス (例: `startup_hang`, `idle_stall`, `rate_limited`, `permission_denied`)。成功時は空文字。 |
| `${<node>.failureSummary}` | `job`, `worker` | 1 行の失敗要約。成功時は空文字。 |
| `${<node>.likelyCauses}` | `job`, `worker` | 原因 id のカンマ区切り (例: `no_model,waiting_for_input`)。成功時は空文字。 |
| `${<node>.suggestedAction}` | `job`, `worker` | 推奨アクション (`retry`, `retry_with_change` など)。成功時は空文字。 |
| `${<node>.retryAt}` | `job`, `worker` | rate limit の再試行可能時刻。無い場合は空文字。 |
| `${<node>.engine}` / `${<node>.model}` | `worker` | 直近アテンプトで実際に使ったエンジン／モデル (エンジン切り替え後も最新値)。 |

### 4.3 `capture` による変数抽出
`job` / `worker` ノードの終了時にログ末尾や JSON ファイルから変数を抽出できます:
```json
"capture": {
  "errFile": { "regex": "ERROR in ([^:]+):", "group": 1 },
  "failCount": { "jsonFile": "build/report.json", "path": "summary.failed" }
}
```

### 4.4 `foreach` と `${foreach:...}` の例
```json
{
  "id": "test-suite",
  "root": {
    "type": "sequence",
    "children": [
      {
        "type": "job",
        "id": "build",
        "summary": "Build all targets",
        "command": "cmake --build build --config Release --target Hancho hancho-cli ${foreach:tests/*_smoke.cpp:stem: }",
        "timeoutSec": 3600
      },
      {
        "type": "foreach",
        "id": "smokes",
        "glob": "tests/*_smoke.cpp",
        "as": "t",
        "mode": "all",
        "exclude": ["tests/posix_core_smoke.cpp"],
        "do": {
          "type": "job",
          "id": "test_${t.stem}",
          "command": "build\\Release\\${t.stem}.exe",
          "summary": "Run ${t.stem}",
          "timeoutSec": 900
        }
      }
    ]
  }
}
```
ビルドコマンドは glob で見つかった全 smoke ターゲットをビルドし、`foreach` ノードは生成された exe を順に実行して `exclude` のものは飛ばします。名前を付けて繰り返し使う場合は [ルーチン](routines-ja.md) を参照してください。

---

## 5. `worker` ノードとエンジン定義 (`engines.json`)

### 5.1 内蔵エンジンと `<dataDir>/engines.json`
4 種類の内蔵エンジン (`agy` [既定、`defaultModel`: `"gemini-3.8-flash-medium"`], `claude`, `opencode`, `codex`) を備えています。`<dataDir>/engines.json` を置くと上書き・追加できます:
```json
{
  "default": "agy",
  "engines": {
    "agy": {
      "command": "agy.exe -p {prompt} --output-format stream-json --model {model} {permissionArgs} --print-timeout {timeoutMin}m",
      "defaultModel": "gemini-3.8-flash-medium",
      "stream": "agy",
      "permissionArgs": "--dangerously-skip-permissions"
    },
    "claude": {
      "command": "claude -p {prompt} --output-format stream-json --verbose --model {model} {permissionArgs}",
      "defaultModel": "claude-sonnet-4-6",
      "models": ["claude-sonnet-4-6", "claude-opus-4-6"],
      "stream": "claude",
      "permissionArgs": "--dangerously-skip-permissions"
    }
  }
}
```
- **権限フラグ (`permissionArgs`) は必ず `engines.json` 側に書き、BT の `prompt` には書かないでください。**
- テンプレート内で使えるプレースホルダー: `{prompt}` (`QuoteCommandArg` で常にクォート)、`{model}` (空の場合は直前の `--model` / `-m` ごと省略)、`{permissionArgs}`、`{timeoutSec}`、`{timeoutMin}` (`ceil(timeoutSec / 60)`)。

### 5.2 構造化出力 (`PC_BT_RESULT_FILE` と `outputSchema`)
- `worker` ジョブには環境変数 `PC_BT_RESULT_FILE` (`<dataDir>\behaviortrees\<btId>\results\<jobId>.json`) が渡されます。
- `"outputSchema"` (`type`, `required`, `properties`, `enum`, `items`) を指定すると、スキーマと結果ファイルのパスをワーカーの指示書ファイルに追記し、ワーカー終了時に `PC_BT_RESULT_FILE` を検証します。終了コードが `0` でもファイル欠落・JSON 不正・スキーマ不一致なら `detail: "outputInvalid: <理由>"` (例: `outputInvalid: output is empty`) で失敗します。

### 5.3 `engines` / `switchEngineOn` / `probe` / `defaults`

`worker` (および LLM を起動する `job`) は、単一の `engine`/`model` の代わりに事前承認済みのエンジン候補列を書けます。

```json
{ "type": "worker", "id": "impl", "prompt": "<worker prompt>", "summary": "<task summary>",
  "timeoutSec": 5400,
  "probe": true,
  "engines": [
    { "engine": "opencode", "model": "<primary-model>" },
    { "engine": "agy", "model": "<backup-model>" }
  ],
  "switchEngineOn": ["startup_hang", "auth_error", "probe_failed"] }
```

- 先頭から順に試します。`switchEngineOn` に含まれる失敗クラスなら次の候補へ進み、それ以外 (例: `test_failed`) では切り替えません。候補を使い切ると失敗します。
- `engine`/`model` と `engines` の同時指定はパースエラー。`engines` は 1 つ以上必要です。
- 切り替えのたびに `engineSwitched` イベントを発行し、`${<node>.engine}` / `${<node>.model}` を更新します。ノード状態には `engineCandidateIndex` と `engineAttempts` (engine / model / result / failureClass / detail / list / entry) を保存します。
- 起動の直前に LLM リストを再読み込みし、`approved` かつ `enabled` でない候補は飛ばします。
- job のコマンドでは `{engine}` / `{model}` プレースホルダが使えます。例: `ww-run.cmd 091-5 -Main -Engine {engine} -Model {model} -Root <root> -JsonStdout` と `"engine": "opencode", "model": "..."` の組み合わせ。プレースホルダの無いコマンドは切り替え対象になりません。`{engine}` / `{model}` は実行時プレースホルダで、`bt template` の `<...>` (置換用) とは別です。
- `"probe": true` は本番起動前に各候補を probe します。失敗したら本番を起動せず `probe_failed` で終わります (`switchEngineOn` に入れておけば次の候補へ)。
- `defaults` はノードに明示値の無い項目を補完します: `defaults.worker.idle` (`firstEventSec`, `outputSec`, `cpuSec`)、`defaults.worker.maxAttempts`、`defaults.worker.probe`、`defaults.job.idle`。明示値 (明示 `0` を含む) が常に優先されます。worker の既定は LLM らしい `job` ノード (`ww-run`、既知 LLM CLI、`engine` / `agentType` 付き) にも適用され、通常のビルド job には `defaults.job.idle` が適用されます。

### 5.4 LLM リスト、権限 (permits)、`needs`

候補の承認は `<dataDir>/llms.json` の 3 つの独立したリスト — `worker` (実作業)、`consult` (escalate の代理回答)、`supervisor` (Hancho が起動する監督役) — と `history` で管理します。BT の `worker` ノードの `engines` は `worker` リスト、escalate の `fallback` は `consult` リスト、Hancho が起動する supervisor は `supervisor` リストを使います。使われるのは `approved` かつ `enabled` の行だけです。LLM が CLI / MCP で追加した行は `proposed` で、人が承認するまで使われません (ダッシュボードの LLM ページなら `approvedVia: dashboard`、チャットで確認して `llm approve --user-said` なら `approvedVia: chat`)。engine / model / provider の変更は行を `proposed` に戻します。rank 変更と無効化は安全側の変更として承認不要です。すべての変更は `history` に残ります。

CLI / MCP の対応:

```powershell
hancho-cli llm list --list worker --json
hancho-cli llm add --list worker --engine opencode --model opencode-go/deepseek-v4.1-flash --reason "primary"
hancho-cli llm approve --list worker --engine opencode --model opencode-go/deepseek-v4.1-flash --user-said "yes, use opencode"
hancho-cli llm remove --list worker --engine opencode --model opencode-go/deepseek-v4.1-flash --reason "retired"
hancho-cli llm enable --list worker --engine opencode --model opencode-go/deepseek-v4.1-flash --reason "back in service"
hancho-cli llm disable --list worker --engine opencode --model opencode-go/deepseek-v4.1-flash --reason "quota exhausted"
hancho-cli worker probe --engine opencode --model opencode-go/deepseek-v4.1-flash --json
hancho-cli worker probe --engine dummy --timeout 5 --no-cache --json
hancho-cli worker health --json
hancho-cli worker health --reset opencode/opencode-go/deepseek-v4.1-flash --json
```

MCP ツール: `llm_list` (`list`)、`llm_add` (`list`, `engine`, `model`, `reason`、任意で `provider`/`rank`/`note`)、`llm_remove` (`list`, `engine`, `model`, `reason`)、`llm_set_enabled` (`list`, `engine`, `model`, `enabled`, `reason`)、`llm_approve` (`list`, `engine`, `model`, `userSaid`)、`worker_probe` (`engine`、任意で `model`/`timeout`/`no_cache`)、`worker_health` (引数なしで一覧、または `engine`/`model`/`reset`)。`reason` / `userSaid` は記載した場所では必須です。`no_cache` は CLI の `--no-cache` に対応します。

ワーカーの権限は `<dataDir>/permits.json` (`{"permits":[...],"history":[...]}`) にあります。行は `kind` (`read_path`/`write_path`/`command`/`network`)、`target` (glob)、任意の `engines`、`scope` (`global` / `taskqueue:<名前>` / `bt:<id>`)、承認フィールドを持ちます。ジョブと BT ノードは `"needs": [{"kind": "read_path", "target": "..."}]` で必要な権限を宣言でき、未承認の needs は `bt validate` の警告と `--strict` でのエラー (`permit_not_approved`) になります。拒否されたワーカーは失敗クラス `permission_denied` になり、`permissionResumeLimit` (既定 2) 回のセッション再開後、自動申請した permit とともに `waitingForPermit` で保留され、承認されると自動で再実行されます (`onPermissionDenied: "fail"` で保留を無効化)。承認済み permit は起動時にエンジン設定へ変換され、ユーザーの全体設定ファイルは書き換えられません。

```powershell
hancho-cli permit list --json
hancho-cli permit request --kind read_path --target "D:/repo/tools/**" --reason "read build tools"
hancho-cli permit approve <id> --user-said "yes, allow reading tools"
hancho-cli permit revoke <id> --reason "no longer needed"
```

MCP ツール: `permit_list` (`scope`)、`permit_request` (`kind`, `target`, `reason`、任意で `engine`/`engines`/`scope`)、`permit_approve` (`id`, `userSaid`)、`permit_revoke` (`id`, `reason`)。ダッシュボードにも LLM ページと権限 (Permits) ページがあり、そこで承認できます。

---

## 6. 上限 (Limits)

| 項目 | 既定値 | 範囲 | 超過時の扱い |
|---|---|---|---|
| `retry.max` / `repeat.max` | *(必須)* | `1..100` | `bt_validate` / `bt_start` で検証エラー。 |
| `limits.maxNodeRuns` | `200` | `1..10000` | 実行中に超えると `detail: "limitExceeded"` で BT 失敗。 |
| `limits.maxWallSec` | `86400` (24時間) | `> 0` | 実行中に超えると `detail: "limitExceeded"` で BT 失敗。 |
| ツリー深さ | — | `<= 32` | `bt_validate` / `bt_start` で検証エラー。 |
| 総ノード数 | — | `<= 1000` | `bt_validate` / `bt_start` で検証エラー。 |

---

## 7. `escalate` (判断パケット・`bt_wait`・`bt_resume`・`fallback`)

1. `escalate` に到達するとその枝だけを止め (`parallel` の別枝は継続)、`<dataDir>/behaviortrees/<id>/escalations/<node>.<run>.json` と `.md` に判断パケットを保存します。
2. `bt_wait` が `"state": "waitingForLlm"` と `"escalation"` オブジェクトを伴って即座に返ります。
3. 呼び出し元は `bt_resume` (`id`, `node`, `answer`, 必須の `rationale`, 任意の `vars`) で回答します。`expect` がある場合はリスト外の回答を拒否します。
4. `"fallback"`: `{"afterSec": 300, "engine": "claude", "model": "sonnet"}` を指定すると、`afterSec` 秒間 `bt_resume` が来ないときに `.md` 判断パケットを指示書としてフォールバックワーカーを起動します (`fallback.command` は使用不可で検証エラーになります)。
5. `timeoutSec` (既定 `86400`) 以内に回答がなければ `detail: "escalationTimeout"` で失敗します。

---

## 8. 再開・更新・キャンセル・一時停止 (`bt_retry`, `bt_update`, `bt_cancel`, `bt_pause`)

- **`bt_retry`** (`id`, 任意 `from`, `resetCounters`): 失敗した葉ノード (または `from` で指定したノード) とそこに至る祖先を `idle` に戻して再開します。`--from <node>` は指定ノード**とその後に実行されるノード**をやり直します。`sequence` / `fallback` / `parallel` の後ろの兄弟、`if` で選ばれなかった方の枝、`catch` の handler / `otherwise`、および前の枝が失敗したことで走った代替の枝 (escalate ゲートなど) も破棄して再評価します。指定ノードより前の成功済みの兄弟はそのまま残し、やり直しません。破棄した部分木の `${gate.answer}` などの古い変数値も消えます。`resetCounters: true` で `runs` 回数も 0 に戻します。
- **`bt_update`** (`id`, 任意 `node`, `bt`/`patch`/`file`): `running` でないノードの定義を差し替えます。永続化状態と整合させるため、木構造 (`type`・`id`・子の数とスロット) の変更は拒否されます。
- **`bt_cancel`** (`id`): BT を `cancelled` にし、実行中の BT ジョブをすべてキャンセルします。
- **`bt_pause`** (`id`, `paused`): BT と対応タスクキューを一時停止 (`paused`) または再開 (`running`) します。
- **`bt tasks`** (`id`): ツリーの全ジョブを、並列分岐のキューをまたいで一覧します。CLI では `hancho-cli taskqueue list --bt <id>` でも同じ一覧を出せます。
- **タスクキュー側の保護**: BT が起動したジョブに対する MCP / CLI からの `taskqueue_retry`・`taskqueue_update` は `"this job is managed by a behavior tree; use bt_retry"` で拒否されます。

---

## 9. 完全な JSON サンプル 3 例

### 例 1: `worker` を使ったビルド修正ループ
```json
{
  "id": "build-fix-loop",
  "summary": "Release ビルドを実行し、失敗時は worker で修正して再ビルドする",
  "cwd": "D:/repo",
  "limits": { "maxNodeRuns": 20, "maxWallSec": 3600 },
  "worker": { "engine": "claude", "model": "sonnet" },
  "root": {
    "type": "retry",
    "id": "loop",
    "max": 3,
    "child": {
      "type": "sequence",
      "children": [
        {
          "type": "fallback",
          "children": [
            {
              "type": "job",
              "id": "build",
              "summary": "Build Release target",
              "command": "cmake --build build --config Release",
              "timeoutSec": 600
            },
            {
              "type": "sequence",
              "children": [
                {
                  "type": "worker",
                  "id": "fixer",
                  "summary": "Fix build failure from ${build.logFile}",
                  "prompt": "Inspect build log ${build.logFile}, fix the compile error, and write summary JSON to PC_BT_RESULT_FILE.",
                  "timeoutSec": 900,
                  "outputSchema": {
                    "type": "object",
                    "required": ["fixedFile"],
                    "properties": { "fixedFile": { "type": "string" } }
                  }
                },
                { "type": "fail", "detail": "trigger retry to re-run build after fix in ${fixer.output.fixedFile}" }
              ]
            }
          ]
        }
      ]
    }
  }
}
```

### 例 2: テスト → 修正 → 再テスト
```json
{
  "id": "test-fix-retest",
  "summary": "スモークテストを実行し、失敗時のみ修正・再ビルド・再テストを行う",
  "cwd": "D:/repo",
  "root": {
    "type": "fallback",
    "children": [
      {
        "type": "job",
        "id": "initial_test",
        "summary": "Run smoke test suite",
        "command": "ctest --test-dir build -C Release --output-on-failure",
        "timeoutSec": 300
      },
      {
        "type": "sequence",
        "children": [
          {
            "type": "worker",
            "id": "diagnose_and_fix",
            "summary": "Diagnose test failure and patch code",
            "prompt": "Tests failed (exit ${initial_test.exitCode}, log ${initial_test.logFile}). Fix the bug without weakening tests.",
            "timeoutSec": 900
          },
          {
            "type": "job",
            "id": "rebuild",
            "summary": "Rebuild after fix",
            "command": "cmake --build build --config Release",
            "timeoutSec": 600
          },
          {
            "type": "job",
            "id": "retest",
            "summary": "Re-run smoke test suite",
            "command": "ctest --test-dir build -C Release --output-on-failure",
            "timeoutSec": 300,
            "success": { "exitCode": 0, "outputRegex": "100% tests passed" }
          }
        ]
      }
    ]
  }
}
```

### 例 3: レビュー判断を伴う変更 (`escalate`)
```json
{
  "id": "schema-migration-review",
  "summary": "マイグレーション案を作成し、破壊的変更がある場合は escalate で承認を待つ",
  "intent": "本番稼働中のカラムを落とさずに安全に DB マイグレーションを適用する。",
  "cwd": "D:/repo",
  "root": {
    "type": "sequence",
    "children": [
      {
        "type": "worker",
        "id": "plan_step",
        "summary": "Analyze schema diff and write migration plan",
        "prompt": "Compare schema.sql against prod and write risk analysis JSON to PC_BT_RESULT_FILE.",
        "timeoutSec": 600,
        "outputSchema": {
          "type": "object",
          "required": ["risk", "sqlFile"],
          "properties": {
            "risk": { "type": "string", "enum": ["low", "breaking"] },
            "sqlFile": { "type": "string" }
          }
        }
      },
      {
        "type": "if",
        "cond": { "type": "var", "name": "plan_step.output.risk", "eq": "breaking" },
        "then": {
          "type": "sequence",
          "children": [
            {
              "type": "escalate",
              "id": "review_gate",
              "question": "Migration ${plan_step.output.sqlFile} is marked breaking. Approve or abort?",
              "guidance": "互換ビューが含まれている場合のみ approve を選ぶこと。",
              "expect": ["approve", "abort"],
              "include": {
                "logs": ["plan_step"],
                "vars": ["plan_step.output.sqlFile", "plan_step.output.risk"],
                "files": ["${plan_step.output.sqlFile}"]
              },
              "timeoutSec": 3600,
              "fallback": { "afterSec": 600, "engine": "claude", "model": "opus" }
            },
            {
              "type": "if",
              "cond": { "type": "var", "name": "review_gate.answer", "eq": "approve" },
              "then": { "type": "succeed" },
              "else": { "type": "fail", "detail": "Supervisor aborted breaking migration" }
            }
          ]
        }
      },
      {
        "type": "job",
        "id": "apply_migration",
        "summary": "Apply migration ${plan_step.output.sqlFile}",
        "command": "cmd.exe /c tools\\apply_sql.bat ${plan_step.output.sqlFile}",
        "timeoutSec": 300
      }
    ]
  }
}
```

---

## 10. `bt_get` の読み方と主な失敗理由 (`detail`)

- **`bt_get` (`compact: true`)**:
  - `"compact"` フィールドに 1 ノード 1 行のインデント付きツリー文字列 (`[state] type id "summary"  job <jobId>  - <detail>`) を返します。
  - `"tree"` は返しません。定義・状態・変数をすべて見るときは `compact` なしの `bt_get` を使います。
- **残り時間と仮タスク**: `bt_get` は `pendingEstimateSec`（現在の分岐でまだ起動していない job/worker 葉の見積もり合計）と `plannedTasks`（ノードキー・キュー・見積もり）を返します。ダッシュボードは同じ葉を仮のキュータスク行として表示します（`state: "planned"`、`id: btplan:<btId>-<ノードID>-<実行回>`。起動後に付くジョブ ID を先出し）。これらは問い合わせのたびにツリー状態から作り直すため、retry・fallback・`if` の分岐・キャンセルで実行されない枝に移ると、該当行は自動的に消えます。
- **保存ファイル**:
  - 状態ファイル: `<dataDir>/behaviortrees/<id>.json`
  - イベントログ: `<dataDir>/bt-<id>.events.jsonl` (`btStarted`, `nodeStarted`, `nodeFinished`, `varSet`, `escalated`, `btSucceeded`, `btFailed`, `btCancelled`)
- **主な失敗 `detail` 一覧**:
  - `exit <N>` / `timeout` / `idleTimeout`: タスクキュージョブの非ゼロ終了・タイムアウト。
  - `launchFailed`: ジョブ起動失敗 (`ChainGuard` による `&&` / `||` / `;` 拒否など)。
  - `outputInvalid: <理由>`: `worker` が終了コード 0 で終わったが、`PC_BT_RESULT_FILE` が無いか `outputSchema` に違反。
  - `undefinedVar: <name>`: 実行時に未定義の `${name}` を参照した。
  - `escalationTimeout`: `escalate.timeoutSec` 以内に回答が得られなかった。
  - `limitExceeded`: `limits.maxNodeRuns` または `limits.maxWallSec` を超過した。
  - `jobMissing`: 再起動時の復元で、実行中だったジョブ ID がタスクキュー上に存在しなかった。

---

## 11. ハングに強い BT の書き方

LLM ワーカーは何もイベントを出さないまま 1 時間止まり、単純な `timeoutSec` が発火するまで気付かないことがあります。ハング対策は失敗後に reactive に入れるのではなく、**BT を書く時点で**組み込んでください。§5.3 / §5.4 の部品は次のように組み合わせます。

1. `defaults.worker.idle.firstEventSec` で最初のイベントが出ないワーカーを検知し、`outputSec` でその後の無出力を検知します。
2. `defaults.worker.probe: true` (または `probe` 葉) で本番前に各エンジンを確認します。壊れたエンジンは 1 時間を無駄にせず `probe_failed` で終わります。
3. `engines` + `switchEngineOn` で、`startup_hang` / `auth_error` / `probe_failed` のときに次の**承認済み**候補へ移ります。
4. `catch` + `failure` で失敗クラス・原因ごとに分岐し、すべて失敗した場合は escalate に逃がします。
5. ww カードのジョブコマンドで `{engine}` / `{model}` を使えるようにし、必要な権限は `needs` で先に宣言します。

### 11.1 `bt validate` の警告と `--strict`

`bt validate` は検証を成功させたまま `warnings[]` に警告を返します。`--strict` (MCP の `bt_validate` で `strict: true`) は各警告を `errors[]` に昇格させ、`ok` / `valid` を false にします。`bt start` も `warnings` を返しますが、ツリーは開始します。

| `code` | 意味 |
|---|---|
| `no_hang_watch` | worker / LLM job に `idle.firstEventSec` / `idle.outputSec` も `defaults` も無い |
| `opencode_without_model` | opencode を起動するのに `model` / `--model` が無い |
| `unguarded_worker` | worker の祖先に `catch` / `fallback` / `retry` が無い |
| `engines_without_switch` | 候補が 2 つ以上なのに `switchEngineOn` が無い |
| `switch_without_engines` | `switchEngineOn` があるのに候補が 2 つ未満 |
| `engine_not_permitted` | `engines` / job の候補が該当リストに無い・無効・未承認 |
| `unswitchable_command` | LLM job のコマンドが `{engine}` / `{model}` を使っていない |
| `escalate_without_takeover` | `expect` に `takeover` が無い |
| `permit_not_approved` | 宣言した `needs` が未承認 |
| `permit_invalid` | `needs` の形式が不正 |

```powershell
hancho-cli bt validate --file plan.json --json          # {"ok":true,"valid":true,"warnings":[...]}
hancho-cli bt validate --file plan.json --strict        # 警告をエラーにする
hancho-cli bt template                                  # テンプレート一覧
hancho-cli bt template resilient-worker --json > plan.json
```

MCP は `bt_validate` が `bt` / `file` / `strict` を受け取り、`bt_template` が `name` を受け取ります (省略時は一覧)。テンプレートの `<...>` は検証前に実値へ置き換えてください。`{engine}` / `{model}` は実行時プレースホルダなのでそのまま残します。

### 11.2 例: `bt template resilient-worker --json`

`build\Release\hancho-cli.exe bt template resilient-worker --json` の実出力:

```json
{
  "defaults": {
    "worker": {
      "idle": {
        "firstEventSec": 180,
        "outputSec": 600
      },
      "probe": true
    }
  },
  "id": "resilient-worker",
  "intent": "Run one resilient worker; replace <...> placeholders before validation",
  "root": {
    "child": {
      "engines": [
        {
          "engine": "opencode",
          "model": "<primary-model>"
        },
        {
          "engine": "agy",
          "model": "<backup-model>"
        }
      ],
      "id": "impl",
      "prompt": "Stay inside the project (cwd): do not read or write files outside it. If the task requires one, add it to needs and request a permit instead. <worker prompt>",
      "summary": "<task summary>",
      "switchEngineOn": [
        "startup_hang",
        "auth_error",
        "probe_failed"
      ],
      "timeoutSec": 5400,
      "type": "worker"
    },
    "handlers": [
      {
        "class": [
          "startup_hang",
          "idle_stall"
        ],
        "do": {
          "type": "set",
          "value": "engine-switch-handled",
          "var": "hang"
        }
      },
      {
        "class": "rate_limited",
        "do": {
          "sec": 600,
          "type": "wait"
        },
        "thenRetry": true
      },
      {
        "class": "permission_denied",
        "do": {
          "type": "set",
          "value": "requested",
          "var": "permit"
        },
        "thenRetry": false
      },
      {
        "class": "*",
        "do": {
          "expect": [
            "continue",
            "stop",
            "takeover"
          ],
          "guidance": "The worker failed after all approved engine candidates were tried.",
          "include": {
            "logLines": 100,
            "logs": [
              "impl"
            ]
          },
          "question": "<what should the supervisor decide?>",
          "type": "escalate"
        }
      }
    ],
    "id": "impl_guard",
    "maxRetries": 2,
    "otherwise": "fail",
    "type": "catch"
  },
  "summary": "A worker with hang watch, preflight probe, engine fallback and escalation"
}
```

`defaults.worker.idle` + `probe: true` が起動時監視、2 つの `engines` + `switchEngineOn` が自動退避、`catch` の各 handler がハング・レート制限・権限拒否をカバーし、`"*"` handler が `takeover` 可能な escalate へ逃がします。

### 11.3 作成手順

1. `bt template resilient-worker` (ww カード列なら `card-sequence`) から始め、`<...>` プレースホルダを置き換える。
2. worker の候補は `llm list --list worker`、escalate の代理回答は `llm list --list consult` から選ぶ。足りなければ `llm add --list <l> --reason "..."` で提案し、**使う前に**人が承認してもらう (ダッシュボードの LLM ページ、またはチャットで確認して `llm approve --user-said "..."` で記録)。
3. `bt validate --strict` を実行し、警告をゼロにする。
4. 必要なら `worker probe` で事前確認し、`worker health` で共有のエンジン健全性を確認する。
5. そのうえで `bt start` し、`bt wait` / `bt get --compact` で監視する。

BT に事前に組み込む時間が無かった場合は、`skills/hancho/SKILL.md` の「ワーカーが失敗・ハングしたとき」の事後手順を適用してください。

### 11.4 `failure` 条件と `catch` の例

```json
{
  "type": "failure",
  "node": "impl",
  "class": ["startup_hang", "idle_stall"],
  "cause": "no_model"
}
```

```json
{ "type": "catch", "id": "impl_guard",
  "child": { "type": "worker", "id": "impl", "...": "..." },
  "handlers": [
    { "class": ["startup_hang", "idle_stall"], "do": { "type": "set", "var": "fix", "value": "switch-model" }, "thenRetry": true },
    { "class": "rate_limited", "do": { "type": "wait", "sec": 600 } },
    { "class": "permission_denied", "do": { "type": "set", "var": "permit", "value": "requested" } },
    { "class": "*", "do": { "type": "escalate", "question": "...", "expect": ["continue", "stop", "takeover"] } }
  ],
  "otherwise": "fail",
  "maxRetries": 2 }
```

- `child` が成功すれば `catch` も成功。失敗すると上から順に handler を調べ、最初に一致したものの `do` を実行します。`class` は文字列・配列・`"*"` (任意の失敗に一致)、`cause` は `likelyCauses[].id` との一致、両方指定時は AND です。
- `do` が成功して `thenRetry: true` なら `child` を再実行します (上限は `maxRetries`)。上限超過や不一致のときは `otherwise` (`"fail"` 既定、またはノードオブジェクト) へ。
- 選択された handler は `handler[<index>] class=... cause=...` の形でノード状態・`detail`・`nodeFinished` イベントに残ります。
- `failure` 条件は `node` が必須で、`class` / `cause` の少なくとも一方が必要です。対象が未実行・成功、診断が無い、不一致の場合は false です。
- escalate の `include.logs` に失敗ノードを入れると、その完全な診断が判断パケットの `include.diagnostics[<node>]` と Markdown の `Included Failure Diagnostics` 節に載ります。
