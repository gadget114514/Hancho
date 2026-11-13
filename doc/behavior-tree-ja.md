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
| `if` | `"cond"`, `"then"`, 任意 `"else"` | `"cond"` を評価し、成功なら `"then"`、失敗なら `"else"` (省略時は即 `succeeded`) を実行。 |
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
| `job` | `"command"`, `"summary"`, `"timeoutSec"` (`> 0`) | 任意: `"estimateSec"`（残り時間・ETA 表示用の見積もり秒数。タイムアウトは別に適用）、`"cwd"`, `"env"`, `"idle"` (`{outputSec, cpuSec}`), `"success"` (`{exitCode, filesNewer, outputRegex, testReport}`), `"resources"` (`{write:[...], read:[...]}`), `"instructionsText"`, `"instructionsFile"`, `"maxAttempts"` (既定 `1`), `"capture"`。タスクキューに `<btId>-<連番>` のジョブ ID で登録して起動。起動時に `ChainGuard` が適用され、`&&` / `||` / `;` 等のシェル連結は `launchFailed` となる。 |
| `worker` | `"summary"`, `"timeoutSec"` (`> 0`), `"prompt"` または `"promptFile"` | 任意: `"engine"`, `"model"`, `"outputSchema"` (JSON Schema オブジェクトまたは文字列), `"cwd"`, `"env"`, `"idle"`, `"success"`, `"resources"`, `"maxAttempts"`, `"capture"`。エンジンレジストリ経由で LLM ワーカーを起動する。 |
| `wait` | `"sec"` (`>= 0`) | `"sec"` 秒待ってから `succeeded` になる。 |
| `set` | `"var"` (または `"name"`), `"value"` | `"value"` 内の `${...}` を展開して `vars[var]` に格納し、`varSet` イベントを出して即 `succeeded`。 |
| `escalate` | `"question"` | 任意: `"guidance"`, `"expect"` (許可する回答文字列配列), `"include"` (`{logs:[nodeIds], vars:[varNames], files:[relPaths], lines:100, maxBytes:16384}`), `"timeoutSec"` (既定 `86400`), `"fallback"` (`{afterSec, engine, model}`)。この枝を止め、ツリー状態を `waitingForLlm` にして判断パケットを保存し、`bt_resume` (または `fallback`) を待つ。 |
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
| `jsonValue` | `"file"`, `"path"`, 比較演算子 (`eq`/`ne`/`in`/`gt`/`lt`/`exists`) | JSON ファイルの `"path"` (例 `"summary.failed"`, `"items[0].name"`) の値を比較。 |
| `var` | `"name"` (または `"var"`), 比較演算子 (`eq`/`ne`/`in`/`gt`/`lt`/`exists`) | `vars[name]` の現在値を比較 (両辺が数値として解釈できる場合 `gt`/`lt` は数値比較)。 |
| `nodeState` | `"node"`, および `"eq"` (または `"state"` / `"ne"` / `"in"`) | 指定ノードの現在状態 (`idle`, `running`, `succeeded`, `failed`, `skipped`) を比較。 |

---

## 4. 変数 (`${...}`)、クォート規則、`capture`

### 4.1 `${...}` の展開と `command` での自動クォート
- ノード実行時に文字列フィールド (`command`, `summary`, `cwd`, `env`, `prompt`, `instructionsText`, `set.value`, `fail.detail`, `escalate.question`/`guidance`, 条件ノードのパス・比較値) 内の `${name}` を展開します。
- **未定義変数の検出**: ノードの実行・評価時に未定義の `${name}` があると、そのノードは `detail: "undefinedVar: <name>"` で即座に失敗します。
- **`command` での自動クォート**: `job` の `"command"` 内に書かれた `${...}` は、`QuoteCommandArg` により単一の Windows コマンドライン引数として自動的にクォートされます (空白や特殊文字を含む値でも引数が分裂しません)。一方、`prompt`・`summary`・`set.value`・条件式など `"command"` 以外のフィールドではクォートを付けずにそのまま展開されます。

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

### 4.3 `capture` による変数抽出
`job` / `worker` ノードの終了時にログ末尾や JSON ファイルから変数を抽出できます:
```json
"capture": {
  "errFile": { "regex": "ERROR in ([^:]+):", "group": 1 },
  "failCount": { "jsonFile": "build/report.json", "path": "summary.failed" }
}
```

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

- **`bt_retry`** (`id`, 任意 `from`, `resetCounters`): 失敗した葉ノード (または `from` で指定したノード) とそこに至る祖先を `idle` に戻して再開します。`resetCounters: true` で `runs` 回数も 0 に戻します。
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
- **残り時間と仮タスク**: `bt_get` は `pendingEstimateSec`（現在の分岐でまだ起動していない job/worker 葉の見積もり合計）と `plannedTasks`（ノードキー・キュー・見積もり）を返します。ダッシュボードは同じ葉を仮のキュータスク行として表示します（`state: "planned"`、`id: btplan:<btId>:<node>`）。これらは問い合わせのたびにツリー状態から作り直すため、retry・fallback・`if` の分岐・キャンセルで実行されない枝に移ると、該当行は自動的に消えます。
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
