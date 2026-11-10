# Hancho v3 設計: 拡張指針の詳細化

**[English](design-v3-en.md) | [日本語 (Japanese)](design-v3.md)**

[批判的比較](critical-comparison-ja.md) の「強くするには」に挙げた 5 項目を、実装できる粒度まで詳しくした設計文書です。各項目について、現状、目標、設計、段階、非目標を書きます。対象は v0.9.1 時点のコードです。

[README](../README-ja.md) に戻る。

---

## 0. 位置づけ

### 0.1 変えないこと

Hancho は **CLI エージェントのためのプロセス外の制御プレーン**のままです。エージェントループ（モデル呼び出し、プロンプト、ツール、権限、コンテキスト）は起動した CLI の中に残します。v3 はこの定義を広げるのではなく、定義の中で弱いところを埋めます。

| 弱点（批判的比較より） | 対応する項目 |
|---|---|
| ワーカーの中身が見えない、対応 4 形式以外は生ログ | 1. 一級のアダプターと安定イベントスキーマ |
| Windows 専用、デスクトップ専用 | 2. クロスプラットフォームのコア、ヘッドレス、リモート |
| 永続ジョブ状態は意味的なメモリではない | 3. 意味的メモリ |
| 失敗したことしか分からない | 4. 失敗分析 |
| harness に機能を吸収されて陳腐化する | 5. Hancho が不要になったときの指針 |

### 0.2 項目間の依存

```
1. イベントスキーマ ──► 4. 失敗分析 ──► 3. 意味的メモリ
                          │                 ▲
                          └─────────────────┘ (失敗分類を記録に載せる)
2. クロスプラットフォーム  … 独立。1 の正規化イベントは IPC で運ぶ対象になる
5. 卒業指針               … 文書だけで先に出せる
```

イベントが正規化されていないと、失敗の理由もメモリの中身も、エージェントごとの解釈になってしまいます。そのため 1 を最初に行います。

### 0.3 現行構成（参照するコード）

| 領域 | ファイル | 主な型・関数 |
|---|---|---|
| ストリーム解析 | `src/llm_stream.h`, `src/llm_stream.cpp` | `LlmStreamEvent`, `LlmAgentState`, `LlmStreamTracker`, `DetectAgentType`, `DescribeLlmActivity` |
| エージェント別パーサー | `src/llm_stream_{claude,codex,opencode,agy}.cpp`, `src/llm_stream_internal.h` | `ParseClaudeLines`, `ParseOpenCodeLines`, `ParseCodexLines`, `ParseAgyLine` |
| エンジン定義 | `src/worker_engines.h` | `WorkerEngineRegistry`, `WorkerEngineDef`（engines.json） |
| タスクキュー | `src/taskqueue.h`, `src/taskqueue.cpp` | `TaskQueueJob`, `TaskQueueAttempt`, `TaskQueueDefer`, `TaskQueueOrigin`, `ApplyAttemptOutcome` |
| ビヘイビアツリー | `src/behavior_tree*.cpp`, `src/bt_manager*.cpp` | `escalate` ノード |
| プロセス | `src/process_manager.cpp` | ConPTY、Job Object |
| 通信 | `src/ipc_server.cpp`, `src/ipc_client.cpp`, `src/mcp_server.cpp` | Named Pipe、MCP |
| GUI | `src/gui_dashboard.cpp` | Direct2D ダッシュボード、実行状態監視ペイン |

---

## 1. 任意の CLI 向けの一級のアダプターと、安定したイベントスキーマ

### 1.1 現状

- `LlmStreamEvent.type` は自由な文字列です（`"tool_call"`, `"tool_result"`, `"thinking"` など）。フィールドはエージェントの都合で増えてきました（`rateLimitType`, `permissionDenials`, `taskStatus` など、Claude 固有のものが多い）。
- 解析できるのは C++ に組み込んだ 4 形式（`claude`, `agy`, `opencode`, `codex`）だけです。それ以外の CLI は生ログになり、監視ペインに何も出ません。
- エージェントの種類は `DetectAgentType` がコマンドラインから推定します。
- 利用側（GUI、MCP の `get_worker_stream`、BT 条件）は `LlmStreamEvent` を直接読むので、内部の変更がそのまま外部の互換性の問題になります。

### 1.2 目標

- **外に約束する形式**と**内部の解析**を分ける。外には版番号つきのスキーマだけを出す。
- C++ を書かずに、新しい CLI の対応を追加できる。
- 対応の正しさをテストで確かめられる。

### 1.3 Hancho Event Schema v1

1 行 1 イベントの JSONL です。スキーマ名は `hancho.event/1` です。

```json
{
  "schema": "hancho.event/1",
  "seq": 1842,
  "ts": "2026-11-10T09:12:33.120Z",
  "source": { "processId": 12, "jobId": "j_7f3a", "attempt": 2, "btId": "bt_main", "node": "build" },
  "agent": { "type": "claude", "model": "claude-opus-5-5", "sessionId": "…", "agentId": "", "parentAgentId": "" },
  "kind": "tool.call",
  "data": { "toolUseId": "toolu_01…", "tool": "Edit", "activity": "edit", "target": "src/a.cpp", "input": { "…": "…" } },
  "raw": { "file": "proc_12_build_stream.jsonl", "line": 931 }
}
```

`kind` は閉じた列挙です。

| kind | 意味 | `data` の必須フィールド | 任意フィールド |
|---|---|---|---|
| `run.started` | エージェント実行の開始 | — | `cwd`, `toolsOffered`, `permissionMode` |
| `turn.started` | モデル呼び出し 1 回の開始 | — | `messageId` |
| `tool.call` | ツール呼び出し | `tool`, `activity` | `toolUseId`, `target`, `input`, `note` |
| `tool.result` | ツールの結果 | `ok` | `toolUseId`, `error`, `summary` |
| `thought` | 思考 | `text` | `truncated` |
| `message` | 回答テキスト | `text` | `truncated` |
| `plan` | TODO の更新 | `items[]`（`content`, `status`） | — |
| `usage` | トークンと費用 | どれか 1 つ以上のトークン数 | `costUsd`, `cumulative` |
| `rate_limit` | レート制限の状態 | `status` | `window`, `utilization`, `resetsAt` |
| `subagent.started` | サブエージェント開始 | `agentId` | `type`, `description`, `toolUseId` |
| `subagent.ended` | サブエージェント終了 | `agentId`, `status` | `tokens`, `toolUses`, `durationMs` |
| `run.ended` | 実行の終了 | `status`（`completed` / `error` / `cancelled`） | `stopReason`, `numTurns`, `durationMs`, `permissionDenials` |
| `error` | エージェントが報告したエラー | `message` | `code`, `retryable` |
| `raw` | 解釈できなかった行 | `text` | — |

`activity` は既存の `DescribeLlmActivity` の分類（`read`, `edit`, `search`, `command`, `web`, `subagent`, `plan`, `tool`）をそのまま使います。

**互換の規則**

- フィールドの追加は同じ版の中で行う（minor）。利用側は知らないフィールドを無視する。
- フィールドの意味の変更や削除は新しい版（`hancho.event/2`）にする。旧版は少なくとも 1 つのリリースのあいだ並行して出す。
- 知らない `kind` を受け取った利用側は `raw` として扱う。
- スキーマ本体は `doc/schema/hancho.event.v1.json`（JSON Schema）として配布する。

**保存**: 正規化したイベントを `logs/proc_<id>_<name>.events.jsonl` に書きます。今の `.stream.jsonl`（CLI の生の行）は残し、`raw` から辿れるようにします。

### 1.4 アダプターの階層

| 階層 | 書き方 | 向く場面 |
|---|---|---|
| Tier 0 | なし | 生ログと終了コードだけ。今の未対応 CLI と同じ |
| Tier 1 宣言型 | `<data-dir>/adapters/<name>.json` | JSON を 1 行ずつ出す CLI。大半はこれで足りる |
| Tier 2 組み込み | C++（`llm_stream_<name>.cpp`） | 1 行を複数イベントに分ける、状態を持つなど複雑な形式。既存 4 種 |
| Tier 3 外部プロセス | 任意の言語 | 独自形式。stdin に生の行、stdout に `hancho.event` 行 |

Tier 3 は、アダプター JSON の `exec` に変換コマンドを書きます。

```json
{ "adapter": "mycli", "exec": "python adapters/mycli.py" }
```

Hancho は `exec` をプロセスとして起動し、CLI の生の行を stdin に流し、stdout の `hancho.event/1` 行を読みます。`rules` と `exec` の両方があるときは `exec` を使います。

**Tier 1 の例**

```json
{
  "adapter": "mycli",
  "match": { "field": "/type" },
  "rules": [
    { "when": { "/type": "tool_start" }, "kind": "tool.call",
      "map": { "tool": "/name", "target": "/args/path", "toolUseId": "/id" },
      "activity": { "from": "/name", "table": { "read_file": "read", "write_file": "edit", "shell": "command" }, "default": "tool" } },
    { "when": { "/type": "tool_end" }, "kind": "tool.result",
      "map": { "toolUseId": "/id", "ok": "!/error", "error": "/error" } },
    { "when": { "/type": "usage" }, "kind": "usage",
      "map": { "inputTokens": "/in", "outputTokens": "/out" } },
    { "when": { "/type": "done" }, "kind": "run.ended",
      "map": { "status": "/status" } }
  ]
}
```

- `map` の値は JSON Pointer。`!` は真偽の反転。
- どの規則にも当たらない行は `raw` になる。
- 宣言型でできないことは Tier 3 に回し、宣言型の言語を育てすぎない。

**CLI が自分でイベントを出す経路**: ラッパースクリプトや自作ツールが、`HANCHO_EVENT_PIPE`（Hancho が起動時に渡すパイプ名）へ `hancho.event` 行を直接書けるようにします。手で呼ぶ場合は `hancho-cli event emit --kind message --text "…"` を使います。これでシェルスクリプトのジョブでも「今何をしているか」を出せます。

### 1.5 エンジン定義の拡張

engines.json（`WorkerEngineDef`）に、アダプター名と能力の宣言を加えます。

```json
{
  "engines": {
    "mycli": {
      "command": "mycli run --json {prompt} {model}",
      "adapter": "mycli",
      "capabilities": {
        "jsonStream": true,
        "resume": false,
        "reportsUsage": true,
        "reportsSubagents": false,
        "permissionModes": ["default", "auto"]
      }
    }
  }
}
```

`DetectAgentType` による推定は、`adapter` の指定がないときの予備とします。能力の宣言は、監視ペインが「この CLI はトークン数を出さない」と表示を変えるのにも使います。

### 1.6 適合テスト

- アダプターごとに、記録した入力（`*.input.jsonl`）と期待するイベント（`*.expected.jsonl`）の組を `tests/adapters/<name>/` に置く。
- `hancho-cli adapter test <name>` で照合し、差分を出す。
- CLI の版が上がって形式が変わったときに、記録を足して回帰を見つける。

### 1.7 利用側

GUI の監視ペイン、MCP の `get_worker_stream`、BT の条件、4 章の失敗分析、3 章のメモリは、すべて正規化したイベントだけを読みます。`LlmStreamEvent` は内部の型になります。

### 1.8 段階

1. スキーマを文書にし、既存 4 パーサーの出力を v1 に写す。`.events.jsonl` を書く。
2. MCP と CLI で v1 を公開する（`get_worker_stream --schema v1`）。
3. Tier 1 と Tier 3、`HANCHO_EVENT_PIPE`、適合テスト。
4. 旧形式（`type` 文字列）の外部公開をやめる。

### 1.9 非目標

- CLI の内部の推論を推定すること。出てきたイベント以上のことは分からない。
- すべての CLI を組み込みで対応すること。

---

## 2. クロスプラットフォームのコア、またはリモート／ヘッドレスモード

### 2.1 現状

- ConPTY、Job Object、Named Pipe、名前付きミューテックスが土台です。
- コア（`process_manager`, `taskqueue`, `bt_manager`, `ipc_server`）と Direct2D の GUI が `Hancho.exe` の中に同居しています。GUI を出さずにコアだけを動かす方法がありません。
- `hancho-cli` は Named Pipe 越しのクライアントです。

### 2.2 目標

- 画面のないところ（CI、サーバー、スケジュール実行）でキューと BT を動かせる。
- Linux / macOS でもコアが動く。
- 別のマシンで動くワーカーを、手元から見て操作できる。

### 2.3 段階 A: ヘッドレス（Windows のまま）

- `Hancho.exe --headless`: ウィンドウとトレイを作らずにコアを動かす。後で独立した `hanchod` に分ける。
- GUI はコアの IPC クライアントにする。GUI を閉じてもジョブは止まらず、後から GUI をつなぎ直せる。
- CI 向けの一発実行:

```powershell
hancho-cli run --file queue.json --exit-on-drain --report junit.xml
```

  - キューのジョブがすべて終わったら終了する。終了コードは「全成功なら 0、失敗ありなら 1、取り消しや中断なら 2」。
  - `--report` で JUnit 形式の結果を書く。各ジョブの失敗理由（4 章）を `<failure message>` に入れる。

### 2.4 段階 B: プラットフォーム抽象層

OS に依存する部分を次の境界の後ろに置きます。

| 境界 | Windows | Linux / macOS |
|---|---|---|
| `IPty` | ConPTY | `forkpty` |
| `IProcessTree`（ツリーごとの終了、メモリと CPU の集計） | Job Object | `setsid` によるプロセスグループ。Linux では cgroup v2 があれば使う |
| `IIpcTransport` | Named Pipe | Unix ドメインソケット |
| `IInstanceLock`（データディレクトリごとの 1 インスタンス） | 名前付きミューテックス | `flock` |
| `IShellQuote`（引数のクォート、`QuoteCommandArg`） | Windows の規則 | POSIX の規則 |

- まず Win32 を直接呼んでいる箇所を洗い出し、境界の内側に移す。コアのロジック（キュー、BT、ストリーム解析）は OS に依存しないので、そのまま共有できる。
- Linux / macOS で正式に支えるのは**ヘッドレスのコアと CLI**まで。画面は当面、端末向けの簡易表示（`hancho-cli top`）だけにする。
- ChainGuard と ww 系スクリプトは PowerShell 前提なので、POSIX シェル版の規則を別に持つ。

### 2.5 段階 C: リモート

- IPC の要求と応答を、Named Pipe と同じ形のまま TCP（WebSocket）に載せる。
- 既定は localhost だけで待ち受ける。外から使う場合はトークン認証を必須にし、SSH トンネル越しを推奨する。TLS はトンネルに任せる。
- ジョブに `host` を加える。`host` を指定したジョブは、そのホストの `hanchod` で動く。

```json
{ "name": "linux-test", "host": "buildbox", "command": "make test", "timeoutSec": 1800 }
```

- ホストは `<data-dir>/hosts.json` に登録する（名前、アドレス、トークンの参照先）。トークン自体はファイルに書かず、Windows の資格情報マネージャーや環境変数から読む。
- リソースロックはホストごとに分ける。同じパス名でも別ホストなら衝突しない。
- ログ、イベント（1 章）、`.done` はプロトコル経由で取り寄せる。手元の GUI はリモートのジョブも同じ一覧に出す。

### 2.6 非目標

- クラスタのスケジューリング（空いているホストを選ぶ、自動で増やす）。
- マルチテナントの分離とサンドボックス。権限は今までどおり各 CLI と OS の責任。
- Linux / macOS の GUI ダッシュボード。

---

## 3. 永続ジョブ状態の上に載る、より意味的なメモリ

### 3.1 目的と範囲

**現状**: `processes_state.json`、`taskqueues/<name>.json`（`TaskQueueAttempt` の履歴）、`.stream.jsonl`、BT の状態、`TaskQueueOrigin` が残ります。事実は残っていますが、検索できず、次のジョブに渡す手段もありません。前の試行で分かったことは、次の試行のワーカーには伝わりません。

**メモリが答える問い**（例）

- このジョブは前回なぜ失敗したか。そのときどう直したか。
- このファイルを最近触ったジョブはどれか。そのジョブは成功したか。
- このビルドの普段の所要時間はどれくらいか（タイムアウトの決め方）。
- このエンジン、このモデルで、この種の作業の成功率はどれくらいか。
- 前のワーカーが残した注意点はあるか（「このテストは並列に動かすと落ちる」など）。

**分担**: [Pros/Cons の階層型メモリの節](pros-cons-comparison-ja.md) と同じ線を引きます。

| 置き場所 | 中身 |
|---|---|
| 各 harness のメモリ | 1 つの会話やセッションの中で持ち回る事実、会話の要約 |
| Hancho のメモリ | セッション、ワーカー、再起動、エンジンをまたいで残す**作業についての事実**。何を実行し、どうなり、何が分かったか |

Hancho は LLM のコンテキストを管理しません。ワーカーが何を見るかは、今までどおりワーカーの CLI が決めます。Hancho は求められたときに、関係のある記録を短く渡すだけです。

### 3.2 層

```
L3 プロジェクト知識   人が承認した事実。git にコミットする
   ▲ 昇格（人の操作）
L2 ノート             エージェント・人・Hancho が明示的に書いた短い事実。出典つき
   ▲ 参照
L1 実行記録           試行 1 回につき 1 件。イベントから決定的に作る
   ▲ 生成
L0 イベント           1 章の .events.jsonl
```

下の層ほど量が多く機械的で、上の層ほど少なく、人の判断が入ります。上の層は必ず下の層を出典として指します。

### 3.3 L1 実行記録（RunRecord）

試行 1 回につき 1 件、LLM を使わずに決定的に作ります。同じイベントからは同じ記録ができるので、いつでも作り直せます。

```json
{
  "schema": "hancho.memory.run/1",
  "id": "rr_j_7f3a_2",
  "jobId": "j_7f3a",
  "taskqueue": "main",
  "jobName": "build",
  "bt": { "id": "bt_main", "node": "build", "run": 3 },
  "attempt": 2,
  "origin": { "client": "claude-code 2.4.0", "session": "…", "cwd": "D:/wsgit/app" },
  "engine": "claude",
  "model": "claude-opus-5-5",
  "sessionId": "…",
  "goal": "Release ビルドを通す。警告はそのまま",
  "cwd": "D:/wsgit/app",
  "commandHash": "sha256:9b1c…",
  "start": "2026-11-10T09:02:11Z",
  "end": "2026-11-10T09:14:40Z",
  "durationSec": 749,
  "outcome": "failed",
  "failure": { "class": "success_check_failed", "summary": "filesNewer: build/Release/app.exe が更新されていない" },
  "exitCode": 0,
  "filesRead": ["CMakeLists.txt", "src/net.cpp"],
  "filesEdited": ["src/net.cpp"],
  "commands": ["cmake --build build --config Release"],
  "errors": ["LNK2019: unresolved external symbol __imp_strerror (x3)"],
  "todos": { "done": 3, "total": 5 },
  "finalMessage": "net.cpp のリンクエラーを直しましたが、libfoo が /MD のため…",
  "usage": { "totalTokens": 182340, "costUsd": 1.42 },
  "subagents": { "count": 1, "types": ["general-purpose"] },
  "sources": { "eventsFile": "proc_12_build.events.jsonl", "seqRange": [1, 2210], "logFile": "proc_12_build_20261110_090211.log" },
  "createdAt": "2026-11-10T09:14:41Z"
}
```

**フィールドの作り方**

| フィールド | 出所 |
|---|---|
| `goal` | `TaskQueueJob.summary` と `instructions` の先頭（300 文字まで） |
| `outcome`, `exitCode`, `start`, `end` | `TaskQueueAttempt` |
| `failure` | 4 章の診断 |
| `filesRead`, `filesEdited` | `tool.call` の `activity` が `read` / `edit` のものの `target`。`cwd` 相対に直し、重複を除き、各 50 件まで |
| `commands` | `activity` が `command` のもの。最初と最後を優先して 20 件まで |
| `errors` | `tool.result` の `error`、`error` イベント、ログ末尾のエラー行。数字やパスを伏せた形で重複を数え、多い順に 10 件まで |
| `todos` | 最後の `plan` イベント |
| `finalMessage` | 最後の `message`。2KB で切る |
| `usage` | `usage` イベントの合計 |

JSON ストリームのないジョブ（普通のコマンド）でも、`goal`、`outcome`、`exitCode`、時刻、`errors`（ログ末尾から）は作れます。

### 3.4 L2 ノート

エージェント、人、Hancho 自身が明示的に書く、短い事実です。

```json
{
  "schema": "hancho.memory.note/1",
  "id": "nt_01JC9Z…",
  "text": "libfoo は /MD でビルドされている。/MT でビルドし直さないとリンクできない",
  "tags": ["build", "link"],
  "scope": "job:build",
  "author": { "type": "agent", "origin": { "client": "claude-code 2.4.0", "session": "…" } },
  "sources": ["rr_j_7f3a_2"],
  "status": "active",
  "supersedes": null,
  "expiresAt": null,
  "createdAt": "2026-11-10T09:14:52Z"
}
```

- `scope` は、そのノートがどこに関係するかです。`project`、`taskqueue:<名前>`、`job:<名前>`、`file:<パス>`、`engine:<名前>` のどれかです。検索と注入（3.8）で使います。
- `status` は `active`、`superseded`（新しいノートに置き換えられた）、`retracted`（取り消された）のどれかです。
- `text` は 1KB までです。長い説明はファイルに置き、ノートからはパスを指します。

**書き手ごとの規則**

| 書き手 | 書き方 | 出典 |
|---|---|---|
| エージェント | MCP `memory_note`、または BT ワーカーの構造化出力の `notes` 欄 | 必須。自分の試行の RunRecord を自動で付ける |
| 人 | GUI、`hancho-cli memory note` | 任意 |
| Hancho | 規則による自動記録（下記） | 必須 |

**Hancho が自動で書くノート**（決定的な規則）

- 同じジョブ名が同じ失敗分類で 3 回続いた: 「`build` は直近 3 回 `success_check_failed`（filesNewer）で失敗」。
- 所要時間がタイムアウトの 80% を超えた試行が続いた: 「`test` は所要時間がタイムアウトに近い（p90 1650 秒 / 上限 1800 秒）」。
- 失敗の後に成功した: 失敗した試行と成功した試行の `filesEdited` の差を「直した可能性のあるファイル」として記録する。

### 3.5 L3 プロジェクト知識

人が承認したノートです。`approvedBy` と `approvedAt` が付き、`knowledge.jsonl` に移ります。このファイルは git にコミットする前提で、チームで共有します。

- 昇格は人だけができます（`hancho-cli memory approve <id>`、または GUI）。エージェントには許しません。
- L3 の中身は、注入（3.8）で最も優先されます。

### 3.6 集計（Stats）

L1 から計算する統計です。保存はせず、読むときに索引から作ります。

- ジョブ名、エンジン、モデルごとの成功率。
- 所要時間の中央値と p90（タイムアウトの見積もりに使う）。
- 多い失敗分類と、その推移。
- 試行あたりのトークン数と費用。

### 3.6.1 LLM 使用量の長期集計（月次）

**現状**

- `src/llm_stream.cpp` は LLM の 1 ステップごとに `LlmUsagePoint` を作り、メモリ上の `g_usageHistory` に積みます。
- 保持は `kUsageRetentionSec = 24 * 60 * 60`（24 時間）、上限は `kMaxUsagePoints = 200000`。24 時間より古い点は、読み込み時と追加時に捨てます。
- 永続化は設定 `restore_usage_graph`（既定オフ）。有効でも `logs/usage_history.jsonl` に追記するだけで、読み込み時に 24 時間へ刈ります。ファイル自体は縮みません。
- したがって、現状は 1 か月の積み上げができません。24 時間を超えた分はメモリから消え、ファイルには残っても読まれません。

**目標**

- 24 時間のライブグラフはそのまま残す。
- 日次・月次・年次の使用量を、エンジン／モデル別に、トークン種別と費用つきで出せるようにする。
- 12 か月分を保持しても、保存量は 1 MB 未満を目安にする。
- 「生の点は短命、集計は長命」という役割分担を固定する。

**設計**

層を分けます。

| 層 | 粒度 | 保持 | 用途 |
|---|---|---|---|
| U0 生ポイント | 1 LLM ステップ | メモリ 24 時間（任意でディスク 7 日） | ライブグラフ、当日の内訳 |
| U1 日次ロールアップ | 日 × エンジン × モデル | 24 か月 | 月次・年次、CSV 出力 |
| U2 月次ロールアップ | 月 × エンジン × モデル | 無期限（任意） | 高速表示、監査 |

U1 のレコード:

```json
{
  "schema": "hancho.usage.day/1",
  "date": "2026-11-10",
  "tzOffsetMin": 540,
  "engine": "claude",
  "model": "claude-opus-5-5",
  "inputTokens": 1200000,
  "outputTokens": 340000,
  "thinkingTokens": 210000,
  "cacheReadTokens": 8800000,
  "cacheWriteTokens": 410000,
  "totalTokens": 10960000,
  "costUsd": 12.34,
  "runs": 184,
  "firstEpochSec": 1762746131,
  "lastEpochSec": 1762832599
}
```

- `date` は**ローカル日付**。`tzOffsetMin` を一緒に持ち、後から UTC に直せるようにします。日付の境界は 00:00 ローカルです。
- 1 レコードは約 250 B。12 か月 × 31 日 × 10 モデル = 3,720 件で約 0.9 MB。モデルが 100 でも約 9 MB です。
- 月次は U1 の合計で足りるので、U2 は必須ではありません。表示が遅い場合だけ持ちます。

**書き込み**

- 生ポイントを書くときに、その点のローカル日付の U1 を更新します。
- 追記ログにします。`usage/daily/2026-11.jsonl` に `{date, engine, model, delta...}` を追記し、読むときに同じキーを合計します。途中で落ちても壊れません。
- 圧縮は月替わりと `hancho-cli usage compact` で行います。1 か月分のデルタを日 × エンジン × モデルの 1 行にまとめ、一時ファイルに書いて置き換えます。
- 生ポイントをディスクにも残す場合、`logs/usage_history.jsonl` は 7 日より古い行を落として書き直します（現状の追記のみをやめます）。

**集計 API**

| MCP | CLI | 動作 |
|---|---|---|
| `usage_stats` | `hancho-cli usage stats` | `since` / `until` / `groupBy`（`day` / `month` / `engine` / `model`）で合計を返す |
| `usage_export` | `hancho-cli usage export` | CSV / JSON で書き出す |

- 既存の `get_usage_history`（IPC）は U0 用に残します。U1 用に `get_usage_daily` を追加します。
- 返す値は `inputTokens`, `outputTokens`, `thinkingTokens`, `cacheReadTokens`, `cacheWriteTokens`, `totalTokens`, `costUsd`, `runs`。`costUsd` を報告しないエンジンは、`costKnown: false` を付けます。
- GUI の範囲選択を 24h / 7d / 30d / 12m に広げます。12m は U1 を月で合計して描きます。

**データが爆発しない理由**

- 増えるのは U1 だけで、1 日 × モデル数行です。生ポイントを 30 日残すと 1 日 2 万点 × 30 = 60 万点（約 120 MB）になりますが、U1 は 30 日で 300 行（約 75 KB）です。
- 生ポイントは 24 時間（必要なら 7 日）で捨てます。長く残すのは集計だけです。
- `usage_history.jsonl` の無限成長を止めるのが前提です。ここを直さないと、U1 を足してもディスクは増え続けます。

**段階**

| 段階 | 中身 |
|---|---|
| U1 | U1 の追記と月内表示、`usage stats --group-by day` |
| U2 | 12 か月表示、CSV 出力、圧縮コマンド |
| U3 | U2 月次、プロジェクト（データディレクトリ）別の集計 |

**非目標**

- 各プロバイダの請求額との突き合わせ。Hancho が持つのは CLI が報告した推定値です。
- プロンプトや応答本文の保存。集計はトークン数と費用だけです。
- 分散環境の集計。1 データディレクトリ内で完結します。

**未決事項**

- 日付境界をローカルにするか UTC にするか（表示はローカル、保存は UTC + オフセットが無難）。
- データディレクトリをまたいだ合計を出すか（プロジェクト別に閉じる方が安全）。
- `costUsd` を報告しないエンジンに、単価表から概算を当てるか。

### 3.7 保存

```
<data-dir>/memory/
  records/2026-11.jsonl   L1。月ごとのファイル。追記だけ
  notes.jsonl             L2。作成・置き換え・取り消しを追記するログ。再生して今の状態を作る
  knowledge.jsonl         L3。git にコミットする
  index/                  派生物（キーワード、ファイル、ジョブ名の索引）。いつでも作り直せる。git に入れない
```

- **保持期間**: L1 は既定で 90 日（`hancho-cli memory prune` で古い月のファイルを消す）。L2 は `expiresAt` まで（既定は無期限）。L3 は無期限。
- **容量の上限**: `memory/` 全体の上限を設定で持ち、超えたら古い L1 から消す。L2 と L3 は自動では消さず、上限に近いことを警告する。
- **書き手は 1 つ**: 書き込みはコアのプロセスだけが行い、CLI と MCP は IPC 経由で頼む。データディレクトリごとの分離は今までどおりです。
- **作り直し**: `hancho-cli memory rebuild` は `.events.jsonl` と `taskqueues/*.json` から L1 と索引を作り直す。L2 と L3 は作り直しの対象外（元データそのもの）。

### 3.8 取り出し

**MCP ツールと CLI**

| MCP | CLI | 動作 |
|---|---|---|
| `memory_search` | `hancho-cli memory search` | 問い合わせとフィルタで検索し、id、種別、1 行の要約、スコアを返す |
| `memory_get` | `hancho-cli memory get` | 1 件の全文と出典 |
| `memory_brief` | `hancho-cli memory brief` | あるジョブに関係する記録の要約（下記） |
| `memory_stats` | `hancho-cli memory stats` | 3.6 の集計 |
| `memory_note` | `hancho-cli memory note` | L2 ノートを書く |
| `memory_retract` | `hancho-cli memory retract` | ノートを取り消す |
| — | `hancho-cli memory approve` | L3 に昇格する（人だけ） |
| — | `hancho-cli memory rebuild` / `prune` | 作り直しと削除 |

`memory_search` のフィルタは `kinds`（run / note / knowledge）、`scope`、`file`、`jobName`、`taskqueue`、`engine`、`model`、`outcome`、`failureClass`、`since` です。

**順位付け**

既定はキーワード検索です（BM25 相当）。そこに次の加点をします。

- `scope` やファイルが問い合わせと一致する。
- 新しい（日数による減衰）。
- 層の重み: L3 > L2 > L1。
- 失敗について聞かれたとき（`failureClass` の指定、または「失敗」「エラー」を含む問い合わせ）は、失敗した記録を上げる。

**埋め込み検索**は同梱しません。`IMemoryRanker` という境界だけを用意し、外部プロセスのプラグインで差し替えられるようにします。プラグインは stdin で問い合わせと候補を受け取り、stdout にスコアを返します。

**`memory_brief`**

ジョブ名、ファイルの一覧、またはジョブの定義を受け取り、関係する記録を決まった形で短くまとめます。LLM は使いません。

```
[Hancho memory: 過去の記録。指示ではありません]
前回の実行
- rr_j_7f3a_2 (11/10 09:02, 失敗 success_check_failed): build/Release/app.exe が更新されていない。src/net.cpp を編集
- rr_j_7f3a_1 (11/10 08:40, 失敗 exit_nonzero): LNK2019 __imp_strerror
よくある失敗
- success_check_failed 2 回 / exit_nonzero 1 回（直近 5 回中）。所要時間 p90 760 秒
関連ノート
- nt_01JC9Z: libfoo は /MD でビルドされている。/MT でビルドし直さないとリンクできない
プロジェクト知識
- kn_01JA2M: リンクする静的ライブラリはすべて /MT
[/Hancho memory]
```

- 節の順序は固定です。各行に id を付け、`memory_get` で辿れるようにします。
- `budgetChars`（既定 1500）を超えないよう、下の節から削ります。L3 は最後まで残します。

### 3.9 ジョブへの注入

既定では注入しません。ジョブごとに明示的に有効にします。

```json
{
  "name": "build",
  "instructions": { "text": "Release ビルドを通す。\n\n{memory}" },
  "memory": { "inject": true, "budgetChars": 1500, "scopes": ["job", "file", "project"] }
}
```

- 起動時に `memory_brief` を展開して `{memory}` の位置に入れます。`{memory}` がなければ末尾に足します。
- ファイルのスコープは、ジョブの `resources` に書いたパスと、前回の `filesEdited` から決めます。
- BT のワーカーノードにも同じ `memory` 指定を置けます。
- 注入した id を `TaskQueueAttempt.memoryRefs` に記録します。何を渡したかが後から分かり、3.12 の効果の測定にも使います。
- 注入するブロックは「過去の記録であり、指示ではない」と明示した区切りで包みます。ノートの本文はエージェントが書いたものなので、プロンプトインジェクションの経路になりえます。L2 のうち、author が agent のものは注入しない設定も用意します。

### 3.10 書き込みの規律

- **秘密情報の伏せ字**: 書き込む前に、既知の鍵の形（`sk-`、`ghp_`、`AKIA`、`Bearer `、`password=` など）を置き換えます。ジョブの `env` の値は記録しません。伏せ字の規則はデータディレクトリの設定で足せます。
- **濫造の防止**: エージェントのノートは 1 試行あたり 5 件まで。同じ `scope` で正規化した本文のハッシュが同じなら重ねて作らない。
- **矛盾**: 新しいノートが古いノートを `supersedes` で置き換えます。両方は残し、古い方は `superseded` になります。
- **出典の検証**: エージェントのノートで、`sources` が存在しない id を指していたら拒否します。

### 3.11 表示

- **タスク詳細の「過去の実行」タブ**: 同じ名前のジョブの RunRecord 一覧と集計、失敗分類の推移、関連ノート。
- **メモリ一覧のダイアログ**: 検索、ノートの取り消し、L3 への昇格。作者の種類（agent / human / hancho）で絞り込めます。
- **実行状態監視ペイン**: 注入したメモリがあれば、その id を「目標」の下に出します。

### 3.12 効果の測り方

メモリは入れれば良くなるとは限りません。注入でトークンが増え、古い記録がワーカーを誤らせることもあります。そこで次を比べます。

- 注入ありと注入なしでの、再試行の成功率。
- 同じ失敗分類の再発率。
- 注入で増えたトークン数と費用。

`memoryRefs` と L1 から `hancho-cli memory stats --compare-inject` で集計します。効果が見えないうちは、注入を既定で無効のままにします。

### 3.13 段階

| 段階 | 中身 |
|---|---|
| M1 | L1 の生成、`rebuild`、キーワード検索、`memory_search` / `memory_get` / `memory_stats` |
| M2 | L2 ノート、伏せ字、`memory_brief`、`{memory}` の注入と `memoryRefs`、Hancho の自動ノート |
| M3 | L3 の昇格、GUI（過去の実行タブ、メモリ一覧） |
| M4 | 埋め込み検索のプラグイン境界 |

### 3.14 非目標

- harness の会話コンテキストや階層型メモリの置き換え。
- LLM による要約を常に走らせること（必要なら、BT の `escalate` や普通のジョブとして明示的に行う）。
- ベクトルデータベースや埋め込みモデルの同梱。

---

## 4. 失敗の分析を厚くする

### 4.1 現状

- 試行には `TaskQueueAttempt.reason`、`exitCode`、`lastLinesFile`、`checkDetail` が残ります。
- ストリームからは `toolError`、`permissionDenials`、`rateLimitType` / `rateLimitStatus` / `rateLimitResetsAtSec`、`stopReason` が取れます。
- 終了コード 75 による延期（`TaskQueueDefer`）があります。
- しかし、これらを突き合わせて「なぜ」を出す仕組みはありません。利用者がログを読んで判断しています。再試行も、理由を問わず `maxAttempts` まで同じように行います。

### 4.2 失敗の分類

| 分類 | 判定の材料 | 既定の対処 |
|---|---|---|
| `launch_error` | プロセスが起動しなかった | 再試行しない（`fix_spec`） |
| `timeout` | `timeoutSec` を超えた | `raise_timeout` を提案。1 回だけ再試行 |
| `idle_stall` | 出力も CPU も止まった | 再試行 |
| `exit_nonzero` | 終了コードが 0 以外で、ほかの分類に当たらない | 再試行 |
| `crash` | 例外による終了（`0xC0000005` など） | 再試行。続けば `human` |
| `success_check_failed` | 成功条件のどれか（`exitCode` / `filesNewer` / `outputRegex`）が満たされない。どれかを `detail` に書く | 再試行 |
| `rate_limited` | `rate_limit` イベントの `status` が `rejected`、またはそれに相当するエラー | `retry_after`（リセット時刻まで待つ。`maxAttempts` を消費しない） |
| `auth_error` | 認証エラーの `error` イベント、決まった文言 | 再試行しない（`human`） |
| `permission_denied` | `permissionDenials` が多く、成果がない | 再試行しない（`fix_spec`: 権限の設定を見直す） |
| `turn_or_context_limit` | `stopReason` が最大ターンやコンテキスト上限 | `escalate` か、作業を分ける |
| `tool_error_loop` | 同じツールの失敗が短い間に繰り返された | `escalate` |
| `incomplete` | 正常に終わったが、TODO が残り、成功条件も満たさない | 再試行 |
| `killed` | 人や上位の操作で止めた | 再試行しない |
| `dependency_blocked` | 依存するジョブの失敗 | 依存元の診断を参照 |
| `resource_conflict` | リソースロックの取得で止まった | 待つ |
| `unknown` | どれにも当たらない | 再試行。`escalate` を提案 |

### 4.3 診断レコード

試行ごとに 1 つ、診断を付けます。

```json
{
  "class": "rate_limited",
  "confidence": "high",
  "summary": "five_hour のレート制限で拒否された。リセットは 14:00",
  "detail": { "window": "five_hour", "resetsAt": "2026-11-10T14:00:00+09:00" },
  "evidence": [
    { "events": "proc_12_build.events.jsonl", "seq": 2207 },
    { "log": "proc_12_build_20261110_090211.log", "line": 4410 }
  ],
  "lastActivity": { "activity": "command", "target": "cmake --build build", "sinceSec": 12 },
  "suggestedAction": "retry_after",
  "retryAt": "2026-11-10T14:00:30+09:00"
}
```

- `confidence` は `high` / `medium` / `low`。規則が直接の証拠で決まれば `high`、状況からの推定なら `medium` か `low`。
- `evidence` は必ず元のイベントかログ行を指します。診断は要約で、証拠は元データです。
- `lastActivity` は、失敗の直前にエージェントが何をしていたかです。「タイムアウトした」より「`cmake --build` の途中でタイムアウトした」の方が役に立ちます。
- `suggestedAction` は `retry`、`retry_after`、`raise_timeout`、`fix_spec`、`escalate`、`human` のどれかです。

### 4.4 分類器

- 順序のある規則の並びで、上から評価し、最初に当たった規則を採ります。決定的で、同じ入力からは同じ診断が出ます。
- 入力は、試行の情報（終了コード、時刻、成功条件の結果）、1 章の正規化イベント、ログの末尾です。
- 規則は組み込みのものに加え、データディレクトリの `failure-rules.json` で足したり上書きしたりできます。

```json
{
  "rules": [
    { "class": "auth_error", "when": { "logRegex": "(?i)(invalid api key|401 unauthorized)" },
      "confidence": "high", "action": "human" },
    { "class": "exit_nonzero", "when": { "exitCode": [2], "jobName": "lint" },
      "summary": "lint の指摘あり", "action": "fix_spec" }
  ]
}
```

- 規則で決まらない（`unknown` や `low`）ときは、任意で LLM に見立てを頼めます。BT の既存の `escalate` を使い、証拠（イベントの末尾、ログの末尾、試行の情報）だけを渡します。LLM の見立ては `confidence: low` の参考として別欄に置き、自動の再試行の判断には使いません。

### 4.5 再試行の方針

分類に応じて再試行を変えられるようにします。

```json
{
  "maxAttempts": 3,
  "retry": {
    "on": ["idle_stall", "exit_nonzero", "crash", "success_check_failed", "incomplete"],
    "never": ["auth_error", "permission_denied", "launch_error"],
    "waitForRateLimit": true
  }
}
```

- `rate_limited` は `waitForRateLimit` が有効なら、リセット時刻まで待ってから再試行し、`maxAttempts` を消費しません。今の `TaskQueueDefer` と同じ扱いです。
- `retry` を書かなければ、今までどおり理由を問わず再試行します。

### 4.6 表示

- **GUI**: タスク詳細とプロセス詳細に「なぜ失敗したか」の欄を置き、`summary`、`lastActivity`、証拠へのリンク（クリックで該当のイベントやログ行へ移る）を出します。タスク一覧の状態列にも分類を短く出します。
- **CLI**: `hancho-cli taskqueue why <jobId>` で、最後の試行の診断と、それまでの試行の分類の並びを出します。
- **MCP**: `taskqueue_get` と `taskqueue_wait` の結果に診断を含めます。上位のエージェントは、ログを読まずに次の手を選べます。
- **BT**: 条件で `failure.class` と `failure.suggestedAction` を参照できるようにします。

```json
{ "type": "selector", "children": [
  { "type": "condition", "expr": "failure.class == 'rate_limited'", "then": "wait_and_retry" },
  { "type": "condition", "expr": "failure.class == 'turn_or_context_limit'", "then": "split_task" }
]}
```

- **集計**: キュー、エンジン、モデルごとの失敗分類の内訳。3 章の L1 に分類が残るので、推移も見られます。

### 4.7 段階

1. 分類器と診断レコード（組み込み規則）。`taskqueue why`、MCP への追加。
2. 分類に応じた再試行の方針。GUI の表示。
3. `failure-rules.json`、BT の条件、任意の LLM の見立て。

---

## 5. harness 側の機能で Hancho が不要になったときの指針

### 5.1 考え方

Hancho は harness の足りないところを補うものです。harness が同じことをできるようになったら、その用途では Hancho をやめるのが正しい判断です。この章は、その判断を利用者に任せきりにせず、はっきりした基準として示します。

### 5.2 Hancho が要る理由

次のどれかが必要なら、Hancho を使う意味があります。どれも要らなければ使いません。

1. **耐久性**: harness や端末、PC を再起動しても、ジョブとその状態が残る。
2. **異種混在**: 複数の CLI やエンジン（Claude、Codex、OpenCode、普通のビルドコマンド）を 1 つの計画の中で動かす。
3. **一覧性**: 多数の並列ジョブを、人が 1 か所で見て、止めたり入力したりする。
4. **決定的な制御**: 順序、依存、タイムアウト、成功条件、再試行を、LLM の判断なしで確実に行う。
5. **マシン内の調整**: リソースロックや並列度で、同じマシン上のジョブの衝突を防ぐ。

### 5.3 機能の対応表

Hancho の機能ごとに、置き換え候補と、置き換えてよい条件を示します。この表はリリースごとに見直します。

| Hancho の機能 | harness 側の候補 | 置き換えてよい条件 |
|---|---|---|
| 長いビルドやテストの裏での実行 | harness のバックグラウンド実行 | harness を終了しても止まらず、終わったことを後から確実に知れる |
| タスクキュー（依存、再試行） | harness のタスク機能、エージェントチーム | 依存と再試行を宣言的に書け、セッションをまたいで残る |
| ビヘイビアツリー | Agent SDK、ワークフロー機能 | 制御をコードとして版管理でき、判断のノードを LLM に戻せる |
| 監視ペイン、ダッシュボード | harness の UI、セッション一覧 | 異なる CLI のジョブも同じ画面に出る |
| サブエージェントの追跡 | harness 自身の表示 | サブエージェントの状態とトークンが見える |
| スケジュール実行 | harness のスケジュールタスク、OS のタスクスケジューラ | 失敗時の再試行と通知がある |
| リソースロック | なし（現時点） | — |
| 失敗分析（4 章） | harness の診断表示 | 理由の分類と証拠が出る |
| 意味的メモリ（3 章） | harness のメモリ機能 | セッションとエンジンをまたいで共有できる |
| CI での実行（2 章） | CI、クラウドのオーケストレーター | 手元で動かす必要がない |

1 つの harness だけを使い、上の条件をすべて満たすなら、Hancho は要りません。

### 5.4 やめ方

やめるときに困らないよう、次を保ちます。

- **データは開いた形式のまま**: キュー、BT、イベント、メモリはすべて JSON / JSONL。Hancho がなくても読める。
- **書き出し**: `hancho-cli export` で、キューを依存順のスクリプトや CI のワークフローの雛形に、BT を手順書（Markdown）に、L3 の知識を Markdown（`CLAUDE.md` などに貼れる形）に書き出す。
- **片付け**: `mcp install` と `agent install-template` の逆を行うコマンドを新しく用意する（`hancho-cli mcp uninstall` で MCP の設定を外し、`agent uninstall-template` で `hancho-runner` などのテンプレートを消す）。データディレクトリは残すか消すかを利用者が選ぶ。

### 5.5 Hancho 側の運用

- リリースごとに 5.3 の表を見直し、[批判的比較](critical-comparison-ja.md) にも反映する。
- harness と重なった機能は「互換」扱いにして、新しい投資をやめる。消すのは、置き換え先が 5.3 の条件を満たしてから。
- コアを小さく保つ。新しい機能は、5.2 の 5 つの理由のどれかを強めるものだけにする。

---

## 6. ロードマップ

| 版 | 中身 |
|---|---|
| v0.10 | 1 の段階 1〜2（スキーマ、`.events.jsonl`、MCP 公開）、2 の段階 A（ヘッドレス、`run --exit-on-drain`）、5 の文書 |
| v0.11 | 4 の段階 1〜2（分類器、診断、再試行の方針、GUI）、1 の段階 3（Tier 1 / Tier 3、`HANCHO_EVENT_PIPE`） |
| v0.12 | 3 の M1〜M2（実行記録、ノート、検索、注入） |
| v1.0 | 2 の段階 B（Linux のヘッドレス）、1 の段階 4、3 の M3、4 の段階 3 |
| v1.0 以降 | 2 の段階 C（リモート）、3 の M4 |

3.6.1 の U1（日次集計）は、既存の usage 経路への変更が小さいため v0.11 で先行できます。U2 以降は v0.12 以降に回します。

---

## 7. 未決事項

- イベントスキーマの JSON Schema ファイルを、どこまで厳密にするか（`data` の型まで縛るか）。
- Linux / macOS の画面を、端末向けの簡易表示だけにするか。
- RunRecord を、JSON ストリームのない普通のプロセス（キューの外で起動したもの）にも作るか。
- L3 の昇格を、人以外（特定のワーカー）にも許すか。
- リモートの認証を、トークン以外（SSH 鍵の流用など）にも広げるか。
- 失敗の分類のうち、`incomplete` と `success_check_failed` の境目をどう決めるか。
