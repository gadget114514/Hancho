# 用語集

状態: 決定（2026-11-24）。名前は 0.10.0 で API・JSON・CLI まで変える。

## なぜ必要か

同じ言葉が、階層の違う複数のものを指していた。

| 言葉 | 意味 1 | 意味 2 | 意味 3 |
|---|---|---|---|
| job | キューの 1 行（1 プロセスの実行） | BT のノードの型（コマンドを自分で書く） | — |
| worker | job として動くプロセスという役割 | BT のノードの型（LLM を起動する） | LLM リストの名前 |
| task | `taskqueue`（job の並び） | タスクグループ（実行中の BT の仮想行） | 0.10 のタスク（依頼 1 件の記録） |

これを次のように分ける。

| 今の言葉と意味 | 新しい名前 |
|---|---|
| job（意味 1: キューの 1 行） | **job**（そのまま） |
| job（意味 2: ノードの型） | **exec** |
| worker（意味 1: 役割） | **worker**（そのまま） |
| worker（意味 2: ノードの型） | **agent** |
| worker（意味 3: LLM リスト） | **agent list** |
| task（意味 1: `taskqueue`） | **taskqueue**（そのまま。task を実行する順番の意味） |
| task（意味 3: 依頼 1 件の記録） | **task**（そのまま） |
| キューに出す BT や job のまとまり（タスクグループを含む） | **direction** |

## 階層

```
task                 依頼 1 件。意図・経過ログを持つ
 └ direction         キューへの 1 回の登録。BT 1 本、または taskqueue add 1 回分
    └ job            1 プロセスの実行。キューの 1 行
       └ attempt     job の 1 回の実行（maxAttempts の範囲で繰り返す）
```

- **direction は task とキューの間の単位。**
  - BT は 1 本で 1 つの direction になる。ノードを実行するたびに、その direction の job を出す。
  - `taskqueue add --file jobs.json` は 1 回で 1 つの direction になる。ファイルの中の job がその direction に属する。job を 1 つだけ登録したときも 1 つの direction になる。
  - `routine run` も 1 つの direction（中身は BT）。
- **BT のノードは exec と agent の 2 種類が job を出す。**
  - exec: 実行するコマンドを自分で書く。中身は何でもよい（ビルド、テスト、LLM を起動するスクリプト）。
  - agent: エンジン・モデル・指示文を書き、Hancho が `engines.json` からコマンドを作って LLM を起動する。
  - どちらも実行時には job を 1 つ出す。違いはコマンドを誰が組み立てるかだけ。成功条件、リトライ、候補の切り替えはどちらにも書ける。
  - exec で候補を切り替えるには、コマンドに `{engine}` / `{model}` を入れる。
- **worker と agent の関係:** 同じではない。
  - worker は、job を実行しているプロセスという役割。ビルド、テスト、スクリプト、LLM のどれでもよい。
  - agent は engine + model の組。実行していなくても存在する（agent list の 1 行、`engines` の候補、escalate の代理回答の候補、`probe` の相手）。
  - agent を job として動かしているあいだ、そのプロセスは worker になる（agent ノード、`run_worker`）。ビルドやテストの worker は agent ではない。
- **系譜**は、やり直し（`supersedes`）でつながった job の列。task の中にあり、direction をまたいでよい（失敗した direction の job を、新しい direction の job でやり直す）。
- **BT の中のやり直し**（`retry` ノード、fix ループ、`bt retry`）は同じ direction の中で起きる。系譜にはならない。
- **単発プロセス:** `start` で起動したプロセスは task にも direction にも job にも入らない。成功判定、リトライ、使用量の集計はない。
- **タスクカードは direction。** 大きな task（機能 1 件）は複数のカードを持ち、1 枚を 1 本の direction として実行する（たとえば BT 1 本: agent → build → test → 失敗なら fix）。カードは task を分けたものではない（task は 1 つのまま）。
  - task が終わるのは、中のカードがすべて成功したか閉じられたとき。
  - カードが失敗するとトリアージになる。トリアージの判断（やり直す、閉じるなど）はそのカードに対して行い、task 全体には行わない。
- **トリアージのカードも direction。** トリアージのカードは、失敗して判断を待っている direction そのもので、別の単位ではない（direction 1 本につきトリアージのカードは多くても 1 枚）。
  - 原因・候補・推奨・判断は、その direction に付く情報として持つ。カードの id は direction の id。
  - direction の中で job が何本落ちても、カードは 1 枚。証拠として失敗した job を全部載せる。
  - 判断で「閉じる」と、その direction が閉じる。「やり直す」と、同じ direction の中でもう一度試すか、`supersedes` で引き継ぐ新しい direction（救済）を作る。救済の direction が失敗すれば、それがまた 1 枚のカードになる。
  - つまり、タスクカード（まだ実行していない、または実行中の direction）とトリアージのカード（失敗した direction）は、同じ direction を状態で呼び分けたもの。
  - タスクカードと direction は 1 対多になりうる。実行が失敗し、直して流し直すと、同じタスクカードに direction が 2 本できる。保存した BT（ルーチン）とその実行の関係と同じ。
  - これまでは `doc/` の Markdown（例: `doc/task-lineage-cards.md` のカード 100-x）にしかなく、Hancho は知らなかった。task の中に保存し、まだ実行していない direction（`planned`）として扱う案がある（下の「direction のレコード」）。

## 用語

### 実行の単位

| 用語 | 意味 |
|---|---|
| task | supervisor がやろうとしていること 1 件。`task create` で作るレコード。`--task` なしで出した direction の job は、系譜の最初の job の id を持つ「暗黙の task」に属する |
| direction | キューへの 1 回の登録。BT 1 本、または `taskqueue add` 1 回分（job 1 つでも）。レコードとして持つ（下の「direction のレコード」）。今「タスクグループ」と呼んでいる BT の仮想行（`group:<id>`）は、実行中の BT の direction |
| job | 1 つのコマンドを 1 プロセスで実行する単位。キューの 1 行。`id` / `command` / `summary` / `timeoutSec` を持つ |
| attempt | job の 1 回の実行。`attempts[]` に残る |
| 系譜（lineage） | `supersedes` でつながった job の列 |
| 単発プロセス | `start` / `start_process` で起動したプロセス |

### 入れ物と定義

| 用語 | 意味 |
|---|---|
| キュー（taskqueue） | task を実行する順番。実際に並ぶのは task の job で、順番・依存つきで並べる。名前は変えない |
| BT（behavior tree） | ノードの木。結果によって次を変える流れを 1 つの JSON で書いた定義。実行すると direction になる |
| ルーチン（routine） | 名前を付けて保存した BT |
| タスクカード（task card） | 大きな task の中に入っている作業の 1 単位（= direction）。task は複数のカードを持つ。1 枚を 1 本の direction として実行する。中身は作業の説明（触るファイル、やること、テスト）と、ほかのタスクカードへの依存。番号は `102-3` のように「計画の番号-連番」で付ける。単に「カード」と言えばタスクカードのこと（`doc/` の「カード 100-x」「カード 102-x」）。失敗した direction を指すときは「トリアージのカード」と書いて区別する（どちらも同じ direction） |

### BT のノード

| 用語 | 今の `type` | 意味 |
|---|---|---|
| exec ノード | `job` | 実行するコマンドを自分で書く |
| agent ノード | `worker` | エンジン・モデル・指示文を書き、Hancho が LLM を起動する |

ほかのノード（`probe`、`wait`、`set`、`escalate`、`fail`、`succeed`、複合・装飾・条件ノード）は名前を変えない。

### 登場人物

| 用語 | 意味 |
|---|---|
| supervisor | ユーザーと話している LLM。task を作り、direction を出し、結果を見て次を決める |
| Hancho | プロセスを所有し、監視・成功判定・リトライ・記録をする |
| worker | job を実行しているプロセスという役割（LLM、ビルド、テスト、スクリプト）。supervisor と対になる言葉で、supervisor が決めて任せた仕事を worker が実行する |
| agent | engine + model の組（定義は下の「LLM まわり」）。job として動かしているあいだは worker になる |
| peer | Chat でメッセージをやり取りする相手（LLM や Hancho）。今の Chat の `--from-agent` / `--to-agent` の agent。agent（job として動く LLM）とは別 |
| host | Chat の LAN 上の相手の Hancho（`192.168.1.20:2426` のようなアドレス）。今の `chat peers` の peer |

### LLM まわり

| 用語 | 意味 |
|---|---|
| エンジン（engine） | モデルを動かす CLI（`claude` / `opencode` / `codex` / `agy`）。ツールの実行、ファイルの読み書き、会話の管理をする。`engines.json` で追加する。一般には harness と呼ばれるものだが、Hancho では engine と呼ぶ（JSON・CLI・`{engine}` プレースホルダ・使用量の集計と名前を揃える） |
| モデル（model） | エンジンに渡すモデル名 |
| agent | **engine + model** の組。例: `claude` + `haiku`、`agy` + `gemini-4-hydrogen-medium`。同じモデルでも、エンジンが違えば別の agent |
| 候補（candidate） | `engines` に並べた agent 1 つ（エンジンと model の組） |
| agent list | 使ってよい承認済みの agent の一覧（今の LLM リストの `worker`）。1 行がエンジンと model の組なので、中身はもともと agent の一覧 |
| consult list | escalate の代理回答に使ってよい承認済みの候補の一覧（今の `consult`。名前は変えない） |
| 切り替え | 失敗したとき、次の候補で同じノードをやり直すこと |

### 失敗と判断

| 用語 | 意味 |
|---|---|
| 失敗クラス（failure class） | 失敗の種類（`exit_nonzero`、`startup_hang`、`fix_no_progress` など） |
| escalate | BT の途中で supervisor に判断を求めるノード。回答は `bt resume` |
| トリアージ（triage） | task の中でトリアージになった（失敗した）カード（direction）を、どう処理するかを決める仕組み。原因・候補（やり直す、内容を変えてやり直す、閉じる、引き取る）・推奨を出す。判断の対象はカードで、task 全体ではない |
| トリアージのカード | 失敗して判断を待っている direction そのもの（トリアージのカード = direction）。id は direction の id で、direction 1 本につき多くても 1 枚。原因・候補・推奨・判断は direction に付く。単発プロセス（`start`）は direction を持たないので、カードではなくお知らせにする |
| 引き取り（takeover） | supervisor が job や escalate を自分で片付けると記録すること |

## direction のレコード

- `bt start`、`routine run`、`taskqueue add`（job 1 つでも）のたびに 1 件作る。
- 項目: `id`、種類（`bt` / `jobs`）、`taskId`、作成時刻、作成したセッション（`origin`）。
- job は `directionId` で direction に属する。BT が出す job は、その BT の direction に属する。
- id の決め方（BT の id をそのまま使うか、`dir_` のようなプレフィックスで採番するか）は実装のカードで決める（仮の答え: `dir_` + 連番。`doc/direction-cards.md`）。
- 一覧の仮想行は `group:<id>` から `direction:<id>` に変える。
- 案（未決定）: タスクカードを、まだ実行していない direction（状態 `planned`）として先に作れるようにする。
  - 中身: タスクカードの本文（または BT の定義そのもの）、使う雛形の名前、ほかの direction への依存（`after`）。
  - 実行すると BT を作り、同じレコードが `running` になる。
  - やり直しは、元の direction を引き継ぐ（`supersedes`）新しい direction にする。job の系譜と同じ形を 1 段上で使う。
  - task を開けば、タスクカードごとの進み具合（済み・実行中・依存待ち）が見える。依存のないものは並行して流せる。
  - 保存場所（task のレコードの中か、`taskId` を持つ direction のレコードか）と、本文から BT を作る雛形の形は、実装のタスクカードで決める（仮の答え: direction のレコードに持ち、雛形 `card` で BT を作る。`doc/task-card-store-cards.md`）。

## 0.10.0 での名前の変更

0.10.0 で API・JSON・CLI の名前を変える。古い名前は別名として受け付け続ける（既存の BT、ルーチン、雛形を壊さない）。

| 場所 | 今 | 0.10.0 |
|---|---|---|
| BT の JSON | `"type": "job"` | `"type": "exec"` |
| BT の JSON | `"type": "worker"` | `"type": "agent"` |
| BT の JSON | `defaults.job` / `defaults.worker` | `defaults.exec` / `defaults.agent` |
| BT の警告 | `unguarded_worker` など、ノードの型を指すもの | 型の名前に合わせる |
| LLM リスト | `--list worker` | `--list agent` |
| 一覧の仮想行 | `group:<id>`、「タスクグループ」 | `direction:<id>`、「direction」 |
| job の項目 | なし | `directionId` |

## ぶつかる名前は新しく作る

既存の名前が新しい用語とぶつかるところは、新しい名前を作る。古い名前は別名として受け付け続ける。名前は案で、実装のカードで確定する。

| 場所 | 今 | 何がぶつかるか | 新しい名前（案） |
|---|---|---|---|
| Chat の CLI | `--from-agent` / `--to-agent` / `--audience agent` | agent（job として動く LLM）とは別物 | `--from-peer` / `--to-peer` / `--audience peer` |
| Chat の CLI | `chat peers` / `chat refresh` の peer（LAN 上の Hancho） | 上の peer（メッセージの相手）とぶつかる | host（`chat hosts`） |
| Chat のメッセージの JSON | `direction`（送受信の向き `in` / `out`。`src/cli.cpp`、`src/gui_dashboard.cpp`） | direction（キューへの 1 回の登録） | `flow` |
| MCP | `agent_type`（`run_worker` など） | 中身はエンジン名で、agent（engine + model）ではない | `engine` |
| CLI・MCP | `run_worker`、`worker stream` / `worker probe` / `worker health` | 中身は LLM 専用なので agent にあたる | `run_agent`、`agent stream` / `agent probe` / `agent health` |

- order と instruction は使わない。どちらも direction の前に候補にしたが、既存の項目とぶつかる。`order` は `export` の job の JSON（`src/cli.cpp` の `ExportQueueJobJson`）の並び順の番号、`instructionsText` / `instructionsFile` は job と BT のノードで worker に渡す指示書。

## 残っている作業

- 0.10.0 の名前の変更と direction のレコード → タスクカード 103-1〜103-8（`doc/direction-cards.md`）。
- タスクカードを Hancho に保存する仕組み（上の「direction のレコード」の案）→ タスクカード 104-1〜104-6（`doc/task-card-store-cards.md`）。
- `skills/hancho/SKILL.md`、help、`doc/behavior-tree*.md` などをこの用語集に合わせて書き換える → タスクカード 103-8（名前の変更と同時に行う。名前を変える前に文書だけ新しい名前にすると、まだ受け付けない名前を案内してしまうため）。`doc/task-lineage-plan.md` は書き換え済み。
