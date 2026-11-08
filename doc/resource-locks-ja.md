# リソースロックと並列タスク（設計メモ）

**[English](resource-locks.md) | [日本語 (Japanese)](resource-locks-ja.md)**

大きなプロジェクトで複数のタスク（LLM エージェントの実行など）を同時に動かすための設計です。
「タスクが触るもの」を宣言させ、宣言が重なるタスク同士だけを直列化します。

## 1. 役割分担

| | Hancho（仕組み） | tools/ww（運用・オプション） |
|---|---|---|
| 知っていること | プロセス、ジョブ、依存、**名前付きリソース** | タスクカード、git、worktree、`Authorised edits`、エンジン |
| やること | 起動・停止・タイムアウト・リトライ、`dependsOn`、`concurrency`、**リソース競合の判定**、待ち理由の表示 | カード → リソース宣言、モード選択（shared / worktree）、worktree の作成、宣言外編集の検出、commit/マージ |
| git / worktree | **知らない**。ジョブの `cwd` が worktree かどうかも気にしない | 全部ここ |

- Hancho は ww を知りません。ww は Hancho 無しでも単体で動きます。
- worktree は ww の機能です。Hancho に worktree 専用機能は入れません。相対パスのリソースはジョブの
  `cwd` 基準で解決されます。`ww-run -Worktree` のジョブでも `cwd` は main のチェックアウトのままです（worktree は
  ジョブ開始後に ww-run 自身が作る）。そのためパスのリソースを宣言すると main のジョブと競合します。
  **worktree のジョブにはパスのリソースを宣言しません**（`@build` などの名前付きロックは可）。`ww-run -Jobs -Worktree` はそうします。

## 2. Hancho: ジョブの `resources`

```json
{ "id": "ww-012", "command": "...", "summary": "card 012", "timeoutSec": 7200,
  "cwd": "C:\\proj",
  "resources": {
    "write": ["src/llm_stream.cpp", "src/llm_stream.h", "doc/**", "@build"],
    "read":  ["src/taskqueue.h"]
  } }
```

### 2.1 リソースの種類

| 書き方 | 意味 |
|---|---|
| `@name`（例 `@build`, `@git`） | 名前付きロック。大文字小文字を区別せず完全一致で比較。`cwd` に関係なく全体で共有される |
| パス（`src/a.cpp`, `C:\proj\src`） | ファイルまたはディレクトリ。ディレクトリはその下全体を含む。相対パスはジョブの `cwd` 基準で絶対化（`cwd` が空なら相対のまま比較） |
| ファイル名だけ（`Foo.cs`, `*.md`） | `/` を含まないもの。`cwd` の下のどこにあってもよい名前、つまり `**/Foo.cs`（ww と同じ）。`**` だけは `cwd` 全体 |
| glob（`src/**/*.cpp`, `doc/*`） | `*`（`/` を越えない）、`**`（越える）、`?` |

パスは `\` → `/`、ASCII 小文字化、`.` / `..` の解決、末尾 `/` の除去で正規化してから比較します（Windows 前提）。

### 2.2 競合ルール

ジョブ A と B は次のどれかが重なると競合します。read 同士は競合しません。

- A.write × B.write
- A.write × B.read
- A.read × B.write

重なり判定はセグメント（`/` 区切り）単位です（保守的。迷ったら「重なる」）。`tools/ww/ww-run.lib.ps1` の
`Test-PathPatternOverlap` と同じ規則なので、Hancho と ww-run の判定は一致します。

- リテラル同士は一致が必要。glob のセグメント（`*.cpp`）はリテラルにマッチすれば一致。glob 同士は重なるとみなす。
- `**` は任意個のセグメントにマッチする。
- 片方が先に尽きたら、それはもう一方の祖先ディレクトリとみなす（`src` は `src/a.cpp` を含む）。ただし `**` がその一部を
  飲み込んでいて、末尾がファイル名の場合は除く: `**/foo.cs` は `src/b.cpp` と重ならないが、`src`・`src/foo.cs`・`**/foo.cs` とは重なる。

誤検知は「直列化が増える」だけで壊れはしない、という方針です。

### 2.3 スケジューラと実行順

- リソースは物理的なもの（ファイル、ビルド環境）なので、**キューが違っても競合すれば止めます**。キューは論理的なまとまり、リソースは物理的な排他です。
- 依存を満たした候補を**全キューまとめて**「優先度（高い順）→ 登録順 → キュー名 → 番号」の順に並べ、先頭から起動します。
- 候補は、実行中ジョブと競合するか、**自分より前に並んでいて待っているジョブ**と競合すると待ちます（リソースごとの FIFO）。
  - 例: 読み取り A が実行中、書き込み W が待機、後から別キューの読み取り B が来た → B は A とは競合しないが W の後ろに並ぶ。書き込みが読み取りの連続で待たされ続けることを防ぎます。
  - 前に並んでいるジョブが自分のキューの `concurrency` で待っている場合も、それと競合する別キューのジョブは追い越しません。
- つまり **競合するジョブ同士は、優先度と登録順で決まる順に 1 つずつ実行されます**。順序を変えたいときは `priority` を使います。「A の結果を B が使う」という意味的な順序は `dependsOn` で表します。
- ロックは起動時にまとめて取り、終了時にまとめて返すので、デッドロックは起きません（保持しながら待つことがない）。
- `concurrency` とは独立です。`concurrency` は「キュー内の同時実行数の上限」、リソースは「同時に動いてよい組み合わせ」の制約です。
- 宣言のないジョブは何もロックせず、誰も待たせません（後方互換）。

### 2.4 可視化

- `taskqueue graph` の各ジョブに `resource_wait` を追加します。実行中のジョブが保持していれば `"<ジョブ id>: <リソース>"`、前に並んだジョブの後ろで待っていれば `"<ジョブ id> (queued ahead): <リソース>"`、なければ `null`。応答には `resource_locks`（機能のオン・オフ）も入ります。
- CLI の `taskqueue graph` 表示に `(resource held by ...)` を出します。
- `resources` は `taskqueue add` / `taskqueue update` / 永続化 JSON / `taskqueue get` で扱います。

### 2.5 オン・オフ

- 設定画面のチェックボックス「タスクキュー: リソースが競合するジョブを同時に実行しない」で切り替えます。**デフォルトはオン**です。
- 設定ファイル（`config.json` の `resource_locks`）に保存され、次回起動時にも引き継がれます。
- オフにしても宣言（`resources`）は保持され、判定にだけ使われなくなります。オンに戻すと次の tick から再び効きます。
- ww-run 側の事前チェックはこの設定と無関係に動きます（ww は Hancho なしでも使うため）。

### 2.6 既知の制約

- 宣言は自己申告で、Hancho は実際のファイルアクセスを監視しません（検出は利用側、例えば ww が行う）。
- FIFO なので、前に並んだジョブ（例えば `concurrency` が埋まったキューのジョブ）が、競合する別キューのジョブを足止めすることがあります。急ぐジョブは `priority` を上げます。
- 意味的な依存（A が API を変え B がそれを使う）はリソースでは表せません。`dependsOn` を使います。

## 3. tools/ww: 2 つのモード

| モード | 指定 | 並列条件 | 長所 | 短所 |
|---|---|---|---|---|
| main | `-Main`、または `Authorised edits` の無いカード | main 実行は 1 本だけ | 何も宣言しなくてよい、ビルド/テスト可 | 並列不可 |
| **shared** | `-Shared`。`Authorised edits` のあるカードではデフォルト | `Authorised edits` が他の main/shared 実行と重ならない | ビルドキャッシュ共有、マージ不要 | 宣言の正確さ頼み、並列中のビルド/テストは不可 |
| worktree（従来） | `-Worktree` | 常に並列可 | 完全分離 | マージとビルドキャッシュのコスト |

### 3.1 shared モードの規則

- 実行記録 `ww-data/running/<id>.json` に `Edits`（宣言パス）と `Shared` を書きます。
  従来の main 実行は `Edits = ["**"]`（チェックアウト全体）として扱います。
- 事前チェック（`Test-RunConflict`）: 同じカードは常に拒否。main/shared の実行同士は宣言の重なりで判定。
  拒否した実行は終了コード **75**（EX_TEMPFAIL: 何も実行していない、後で再試行）で終わり、設定エラー（1）やエンジンの失敗と区別できます。
  宣言の分からない実行（記録なし・`Edits` なし）はチェックアウト全体とみなします。
- `Authorised edits` が無いカードは `-Shared` を指定してもチェックアウト全体のロックになります（実質 main と同じ）。
- カードの `## Read-only`（または `## Reads`）セクションのパスは**読み取り専用**として扱います。読み取り同士は並列に動けて、他の実行がそのファイルを編集する場合だけ競合します。
- 単独のファイル名（`Foo.cs`）は `**/Foo.cs` として扱います。
- プロンプトに「他の実行が同じチェックアウトを並列に編集している。Authorised edits 以外を編集しない。コンパイル/テストは行わず Verification pending に書く。commit しない」を付けます。
- 実行後、イベントログから編集されたファイルを集め、宣言外の編集（`ww-data` 配下を除く）を `EditViolations` として `.last.json` とログに記録し、警告を出します。終了コードは変えません。

### 3.2 Hancho と組み合わせる場合

カードの `Authorised edits` を `resources.write` に、`Read-only` を `resources.read` にします。Hancho 側で
重なるカードは起動されず、ww-run 側の事前チェックも同じ判定（ファイル名だけの扱い、重なり判定）なので二重に安全です。

- パスを手で書き写さず、`ww-run.cmd -Jobs 012,013 ...` でカードからジョブを生成します。宣言の出どころが 1 つになります。
- ww-run を **main** 実行で動かすジョブ（`-Main`、または `Authorised edits` の無いカード）は `"write": ["**"]` を宣言します
  （ww-run 自身が取るのと同じロック）。宣言しないと Hancho は他の実行と同時に起動し、ww-run が終了コード 75 で
  拒否します。タスクキューはそのジョブを延期します（ジョブの `defer`。終了コード 75 は既定で有効）。pending に戻って 60 秒後に
  再び起動し、`maxAttempts` は消費しないので壊れはしませんが、ロックを待つ代わりに再試行を繰り返すことになります。
  `-Jobs` はこうしたカードに `**` を書きます。
- worktree のジョブはパスのリソースを宣言しません（1 章参照）。

```json
{ "id": "ww-012", "summary": "card 012", "timeoutSec": 7200, "cwd": "C:\\proj",
  "resources": { "write": ["src/a.cpp", "src/a.h"] },
  "command": "C:\\proj\\tools\\ww\\ww-run.cmd 012 -Shared -Engine claude -Root C:\\proj -JsonStdout" },
{ "id": "ww-013", "summary": "card 013", "timeoutSec": 7200, "cwd": "C:\\proj",
  "resources": { "write": ["src/b.cpp"] },
  "command": "C:\\proj\\tools\\ww\\ww-run.cmd 013 -Shared -Engine claude -Root C:\\proj -JsonStdout" },
{ "id": "verify", "summary": "build + test", "timeoutSec": 3600, "cwd": "C:\\proj",
  "resources": { "write": ["**"] }, "dependsOn": [{"id": "ww-012", "on": "done"}, {"id": "ww-013", "on": "done"}],
  "command": "C:\\proj\\build.cmd" }
```

`--concurrency` は 2 以上にします。検証ジョブは `**` を write して並列実行が終わってから単独で走らせます。

## 4. 実装範囲

1. Hancho: `TaskQueueJob::resources`、解析・検証・永続化・update、スケジューラ判定、graph の `resource_wait`、CLI/MCP の説明、スモークテスト。
2. Hancho: 全キュー共通の実行順（リソースごとの FIFO）、設定画面でのオン・オフと保存。
3. ww: `-Shared`、`## Read-only` セクション、実行記録の `Edits`/`Reads`、重なり判定、shared 用プロンプト、宣言外編集の検出、ww-status 表示、セルフテスト、README。
