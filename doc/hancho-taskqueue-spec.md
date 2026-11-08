# Hancho タスクキュー (taskqueue) 機能 設計書

## 1. 背景と目的
Hancho (以下 PC) は、プロセスの起動・タイムアウト検出・ツリー kill・ログ・done ファイル作成を確実に行える。一方、複数のジョブを「順番に・依存関係つきで・失敗したら再実行して」流す管理は呼び出し側 (AI エージェントなど) に任されている。

この管理を呼び出し側に任せると、次のような失敗が起きる:
- `A && B && C` を 1 プロセスとして起動し、**各ジョブの見積を合算したタイムアウト**を付ける → A がハングすると全体の予算を使い切り、B と C も失われる。
- 「A の done ファイルを待って B を起動」と書く → A が**失敗**しても B が始まる (終了と成功を区別していない)。
- タイムアウト後、呼び出し側が A だけ再実行し、同じ連鎖で失われた B と C を忘れる → 誰も気付かないまま数時間止まる。
- 呼び出し側が起こされるまで再実行されない。

**目的:** ジョブの順序・依存・成否判定・再実行を PC が持ち、永続化する。呼び出し側には連鎖を書かせない。

## 2. 役割分担
| 呼び出し側 (AI エージェント) がやること | PC がやること |
|---|---|
| ジョブを登録する (コマンド・ジョブ単位のタイムアウト・成功条件・依存) | 実行順の決定、起動 |
| 結果を読む | タイムアウト / ハング検出、ツリー kill |
| 失敗したジョブを直して `taskqueue_retry` する | 成否判定、自動再実行、後続の開始・ブロック |
| | 状態の永続化、通知 |

呼び出し側にさせないこと:
- **連鎖コマンド (`&&` など) を書くこと。** PC は拒否する (8 章)。
- 複数ジョブのタイムアウトを合算すること。タイムアウトは 1 ジョブ分しか指定できない。
- 「次に何を流すか」を決めること。
- ジョブを成功扱いにすること。

## 3. 設計原則
1. **状態を持つのは PC だけ。** タスクキューはディスクに永続化し、PC が再起動しても続きから進む。
2. **成功は PC が検証して判定する。** exit code だけで判定しない。MCP からは成功に書き換えられない (強制成功は GUI のユーザー操作のみ)。
3. **1 プロセス = 1 ジョブ = 1 タイムアウト。** 連鎖は受け付けない。
4. **依存元が成功するまで依存先は開始しない。** 依存元の失敗が確定したら依存先は `blocked`。依存関係のないジョブは止めない。
5. **再実行は PC が自動で行う** (上限つき)。呼び出し側の起床を待たない。
6. **汎用。** PC はジョブの中身を知らない。再実行時の文脈は環境変数で渡すだけ。

## 3.5 用語: ジョブ・アテンプト・プロセス
| 概念 | 意味 | 寿命 | 例 |
|---|---|---|---|
| **ジョブ (Job)** | 呼び出し側が「やり遂げてほしい 1 つの仕事」。**仕事の内容 (指示書)**、単一のコマンド、それが**終わったと言える条件** (成功条件)・制限時間・依存先を持つ | 登録してから purge されるまで (PC 再起動をまたぐ) | 「タスクカード 1 枚を実行し、レポートファイルを書かせる」 |
| **アテンプト (Attempt)** | ジョブの 1 回の実行。失敗すると PC が次のアテンプトを作る | 起動から終了まで | 1 回目はタイムアウト、2 回目で成功 |
| **プロセス** | アテンプト 1 回で起動する OS プロセスのツリー (既存の managed process) | アテンプトと同じ | `cmd /c run.cmd A` とその子 |
| **タスクキュー (TaskQueue)** | ジョブの集合と、同時実行数などの実行方針 | 永続 | `main` (同時 1 本) |

- ジョブ : アテンプト = 1 : N、アテンプト : プロセス = 1 : 1。
- 「状態」(pending / succeeded / failed …) はジョブが持つ。プロセスが終わってもジョブが終わったとは限らない。
- 1 ジョブ = 1 コマンド。複数のコマンドを順に実行したいときは、ジョブを複数登録して `dependsOn` でつなぐ (連鎖コマンドは書けない)。
- ジョブは**指示書 (instructions) の本文を自分で持つ** (5.4)。元のファイル (例: タスクカード) が後で書き換えられたり消えたりしても、ジョブが何をするはずだったかはジョブを見れば分かる。

## 3.6 依存関係の表し方
- 依存は**ジョブとジョブの間**に張る (`Job.dependsOn`)。タスクキュー同士の依存というものはない。
- ジョブ id は PC 全体で一意。`dependsOn` は**別のタスクキューのジョブも指せる** (例: 同時 1 本の `main` のジョブが、同時 3 本の `worktree` のジョブの完了を待つ)。タスクキューは「同時に何本動かすか」を決めるだけで、順序は決めない。
- 順序はすべて `dependsOn` で表す。登録順は「依存を満たしたジョブが複数あるときの優先順」にしか使わない。

### `A && B && C` の表し方
3 つのジョブを登録し、B は A に、C は B に `on: success` で依存させる:
```json
{ "taskqueue": "main",
  "jobs": [
    { "id": "A", "command": "run A", "timeoutSec": 1800 },
    { "id": "B", "command": "run B", "timeoutSec": 3600, "dependsOn": ["A"] },
    { "id": "C", "command": "run C", "timeoutSec": 900,  "dependsOn": ["B"] }
  ] }
```
シェルの `&&` との違い:
| | シェルの `A && B && C` | タスクキュー |
|---|---|---|
| タイムアウト | 全体で 1 つ (合算) | ジョブごと (1800 / 3600 / 900) |
| A が失敗 | B, C は永久に失われる | A を自動再実行。上限に達したら B, C は `blocked` で残り、A を直して `taskqueue_retry` すれば続きから進む |
| A が「終了したが成功していない」 | exit 0 なら B が始まる | 成功条件 (`success`) を満たさなければ B は始まらない |
| 途中で PC やエージェントが落ちる | 連鎖ごと消える | 永続化した状態から続く |

その他の形:
- `A & B` (A の成否に関係なく、終わったら B): `"dependsOn": [{ "id": "A", "on": "done" }]`
- A と B の両方が成功したら C: `"dependsOn": ["A", "B"]`
- A, B, C に順序なし: `dependsOn` を付けない (並ぶ数はタスクキューの `concurrency` で決まる)
- 別のタスクキューのジョブ W を待つ: `"dependsOn": ["W"]` (W がどのタスクキューにあってもよい)

## 3.7 依存関係の「宣言」と「実行」を分ける
AI エージェントは依存関係を**宣言する**のは得意だが、それに従って**実行する** (順番を守る、失敗したら後続を止める、再実行したら後続を再開する、取りこぼしを拾う) のは苦手。そこで:

| 段階 | 担当 | 仕組み |
|---|---|---|
| 宣言 | 呼び出し側 | `taskqueue_add` の `dependsOn` に書くだけ。登録した時点で呼び出し側の仕事は終わる |
| 次に何を始めるかの決定 | PC | Scheduler (6 章の状態遷移)。依存がすべて成功したジョブだけ開始する |
| 失敗時の後続の扱い | PC | 依存先を自動で `blocked` にする。消さない |
| 再実行 | PC | 上限まで自動再実行 (5 章 `maxAttempts`)。呼び出し側は何もしない |
| 再開 | PC | 呼び出し側が失敗ジョブ 1 つを `taskqueue_retry` すれば、blocked の後続はすべて自動で pending に戻る。後続を個別に思い出す必要はない |
| 取りこぼしの検出 | PC | 実行できるジョブが残っているのに動いていなければ `taskqueueStalled` を通知 (10 章) |

呼び出し側が実行を手で行えないようにする仕組み:
1. **連鎖コマンドを拒否** (8 章)。順序をコマンドラインに書けない。
2. **登録済みジョブの手動起動を拒否:** `start_process` の command が、終わっていない (pending / blocked / failed / running) ジョブの command と同じなら拒否し、「`taskqueue_retry <id>` を使う」と返す。依存を無視して 1 つだけ再実行する、という今回の失敗を防ぐ。
3. **待ち合わせコマンドを拒否** (8.2)。done ファイル待ちで順序を作れない。
4. **成功の書き換え不可** (11 章)。依存元を成功扱いにして後続を無理に進められない。
5. **呼び出し側が覚えておく必要をなくす:** 未完了のジョブ・blocked の理由・次に動くジョブは `taskqueue_list` / `taskqueue_wait` でいつでも見られる。呼び出し側の記憶に頼らない。

## 4. アーキテクチャ
```
 MCP / CLI / GUI
   │  start_process(単一コマンド)   taskqueue_add(jobs[])
   ▼
 ChainGuard ── 連鎖コマンドを検出して拒否 (8 章)
   │
 TaskQueueStore ── TaskQueue / Job / Attempt を保持・永続化 (8 章以外すべて)
   │
 TaskQueueScheduler ── 実行可能ジョブを選んで起動、終了時に成否判定と状態遷移
   │  1 attempt = 1 managed process
   ▼
 ProcessManager (既存) ── 起動・timeout・idle・ツリー kill・ログ・doneFile
   │
 Notifier ── events.jsonl / notifyFile / onEvent / taskqueue_wait
```
- 新規モジュール: `chain_guard`, `taskqueue_store`, `taskqueue_scheduler`, `taskqueue_notifier`。
- ProcessManager の変更は最小にする (idle の CPU 条件、killExclude、jobId の付与のみ)。
- Scheduler は既存の監視ループから呼ぶ: プロセス終了検出 → 成否判定 → 状態遷移 → 次のジョブ開始。

### 4.0 ジョブとプロセス管理のつながり
ジョブ (TaskQueue 層) と managed process (既存の ProcessManager 層) は、**アテンプト**を介して 1 対 1 でつながる。ProcessManager はジョブも依存も知らない。

```
Job ──1:N── Attempt ──1:1── ManagedProcess (既存)
             processId  ◀──▶  jobId, attemptN (逆参照)
```

**起動 (TaskQueue → ProcessManager)**
1. Scheduler が開始できるジョブを選ぶ (依存がすべて成功、空きあり)。
2. 指示書の現在の版を `instructions/<job>.r<rev>.md` に書き出す (5.4)。
3. ジョブから `StartProcessRequest` を作る: commandLine (`{instructions}` を書き出したパスに置換) / workingDir / env (+ `PC_JOB_ID`, `PC_ATTEMPT`, `PC_JOB_INSTRUCTIONS`, `PC_JOB_INSTRUCTIONS_REV`, `PC_PREV_*`) / timeoutSec / idleTimeoutSec / logFile (`logs/taskqueue/<job>.<n>.log`) / killExclude / openStdin=false。
4. `ProcessManager::Start()` を呼び、返った processId と使った指示書の版 (`instructionsRev`) を新しい Attempt に記録。ManagedProcess 側には `jobId` と `attemptN` を付ける。
5. Job.state = running、Attempt.deadline を保存 (永続化)。

**終了 (ProcessManager → TaskQueue)**
1. ProcessManager はプロセスの終了を従来通り検出し (正常終了 / timeout / idle / kill)、`onProcessFinished(processId, exitCode, endKind)` コールバックを呼ぶ。新たに加えるのはこのコールバックだけ。
2. Scheduler が processId から Attempt を引く (jobId がないプロセスは無視 = 従来の単独プロセス)。
3. 成功条件を判定 (exit code / filesNewer / outputRegex。ログはその attempt の logFile を読む)。
4. Attempt.reason を記録 → Job の状態遷移 (6 章) → 依存先の評価 → 次のジョブを起動。

**操作の対応**
| 操作 | TaskQueue 層 | ProcessManager 層 |
|---|---|---|
| `taskqueue_cancel` | Job = cancelled、再実行しない | 実行中の attempt のプロセスをツリー kill |
| `kill_process` (ジョブのプロセスに対して) | Attempt.reason = killed、再実行判定を行う | ツリー kill (従来通り) |
| `list_processes` | — | 従来の一覧に `jobId` / `attemptN` を追加して表示 |
| `get_process_output` | `taskqueue_get` は attempt ごとの processId と logFile を返す | 従来通り processId で出力を読める |
| PC 再起動 | 永続化した Attempt (processId, rootPid, 起動時刻, deadline) から再接続を試みる。PID 再利用を避けるため、PID と起動時刻の両方が一致したときだけ再接続 | 再接続したプロセスを managed process として登録し直す。できなければ Attempt = interrupted |

**責任の境界**
- ProcessManager: 1 プロセスの起動・監視・kill・ログ。ジョブ・依存・再実行を知らない。
- TaskQueue (Store + Scheduler): どのジョブを、いつ、何回起動するか。プロセスを直接触らず、必ず ProcessManager を通す。
- この境界により、タスクキューを使わない従来の `start_process` は今まで通り動く。

### 4.1 タイムアウト値の受け渡し
タイムアウトは**ジョブの属性**で、決める・保存する・適用する場所を分ける。どこでも合算しない。

```
[決める]   呼び出し側がジョブ 1 つ分の値を数値で渡す
              timeoutSec: 1800
              (PC は指示書の中身を読まないので、見積を指示書から読み取ることはしない)
                 │
[検証]     taskqueue_add / taskqueue_update
              ・timeoutSec は必須 (なしはエラー)
              ・≤ TaskQueue.maxTimeoutSec (超えたらエラー。切り詰めはしない)
                 │
[保存]     Job.timeoutSec
           Job.idle {outputSec, cpuSec}
                 │  pending / blocked で待っている時間は数えない
[適用]     Scheduler がアテンプトを起動するとき
              StartProcessRequest.timeoutSec     = Job.timeoutSec
              StartProcessRequest.idleTimeoutSec = Job.idle (CPU 条件つき)
              Attempt.deadline = 起動時刻 + Job.timeoutSec  (永続化)
                 │
[監視]     ProcessManager (既存) が deadline / idle を判定 → ツリー kill
                 │
[結果]     Attempt.reason = timeout | idle、Attempt.effectiveTimeoutSec を記録
           → Scheduler が再実行判定
```

規則:
1. **時計はアテンプトの起動で始まる。** 依存待ち・キュー待ちの時間は含めない。
2. **再実行のたびに満額を与える。** 残り時間を引き継がない (前回の消費で次が短くなる事故を防ぐ)。合計の上限は `timeoutSec × maxAttempts` で自然に決まる。
3. **他のジョブと共有しない。** ジョブ A のタイムアウトがジョブ B の時間に影響することはない。タスクキュー全体の予算というものは持たない (GUI に見込み終了時刻を表示するだけ)。
4. **実行中は変えられない。** `taskqueue_update` は pending / failed / blocked のときだけ timeoutSec を変更でき、次のアテンプトから効く。実行中の延長は GUI のみ。
5. **PC 再起動:** 生きているプロセスに再接続した場合は、保存した `Attempt.deadline` をそのまま使う (リセットしない)。再接続できなければ interrupted として次のアテンプトに満額を与える。
6. **合算させない仕組み:** 1 ジョブ = 1 コマンドで連鎖を拒否する (8 章) ため、呼び出し側が複数ジョブ分の時間を 1 つの値にまとめる場面がない。誤って大きな値を入れても `maxTimeoutSec` で拒否する。

## 5. データモデル
### 5.1 TaskQueue
```json
{ "name": "main", "concurrency": 1, "paused": false, "version": 42,
  "notifyFile": "...", "maxTimeoutSec": 7200, "defaultMaxAttempts": 2 }
```
- `version`: 状態が変わるたびに +1 (ロングポール用)。
- `maxTimeoutSec`: 1 ジョブに指定できるタイムアウトの上限。超える値は登録時にエラー (合算した値を入れられないようにする)。GUI / 設定ファイルでのみ変更できる。

### 5.2 Job
```json
{
  "id": "build-a",
  "taskqueue": "main",
  "name": "Build A",
  "summary": "Build A and write reports/A.md",
  "instructions": {
    "source": "tasks/A.md",
    "rev": 1,
    "sha256": "...",
    "text": "# Task A ...\n- Estimate 30 min ...",
    "capturedAt": "..."
  },
  "command": "tools\\run.cmd {instructions}",
  "cwd": "C:\\work\\proj",
  "env": { "KEY": "VALUE" },
  "dependsOn": [{ "id": "prep", "on": "success" }],
  "timeoutSec": 1800,
  "idle": { "outputSec": 720, "cpuSec": 720 },
  "maxAttempts": 2,
  "retryDelaySec": 0,
  "success": { "exitCode": 0, "filesNewer": ["reports/A.md"], "outputRegex": null },
  "killExclude": ["Unity.exe"],
  "priority": 0,
  "state": "pending",
  "attempts": [],
  "blockedBy": null,
  "createdAt": "...", "updatedAt": "..."
}
```
- `summary`: **必須**。ジョブの内容を表す短い 1 行の文字列 (5.5)。
- `timeoutSec`: **必須**。ジョブ 1 つ分の秒数 (4.1)。
- `dependsOn[].on`: `success` (依存元が succeeded で開始) または `done` (終われば成否に関係なく開始)。文字列だけなら `success`。
- `idle`: **出力が `outputSec` 秒止まり、かつプロセスツリーの CPU が `cpuSec` 秒止まった**ときだけハングと判定する。0 の条件は使わない。重い処理中 (CPU 使用中・出力なし) を誤って kill しないため。既存の `lastOutputTickMs` / `lastCpuActivityTickMs` を利用する。判定間隔は粗くてよい (既存の監視ループの周期で十分)。
- `success`: 条件はすべて AND。`filesNewer` は cwd からの相対パスで、すべて存在し mtime が attempt の開始時刻より新しいこと。`outputRegex` はログ末尾 200 行に一致すること。省略時は `exitCode: 0` だけ。
- `killExclude`: ツリー kill で除外するイメージ名 (共有の常駐プロセスを巻き込まないため)。

### 5.3 Attempt
```json
{ "n": 1, "processId": 146, "start": "...", "end": "...",
  "reason": "timeout", "exitCode": 1, "instructionsRev": 1,
  "lastLinesFile": "logs/taskqueue/build-a.1.tail.txt",
  "checkDetail": "reports/A.md missing" }
```
`reason`: `ok | exit | timeout | idle | killed | checkFailed | interrupted | launchFailed`

### 5.4 指示書 (instructions)
ジョブの「何をするか」の本文。**PC はファイルの内容もフォーマットも関知しない。** 保存・版管理・受け渡しだけを行い、読み取りや解析はしない。

**登録**
- `taskqueue_add` の各ジョブに `instructionsFile` (パス、cwd からの相対可) か `instructionsText` (本文) のどちらかを指定する。省略可 (指示書のないジョブ)。
- PC は登録時に本文を読み取り、ジョブに**複製して保存**する (`text`, `sha256`, `source`, `rev=1`)。読めなければ登録エラー。
- 上限 1 MB、UTF-8。

**実行**
- アテンプトを起動するとき、その時点の版を `<PC データディレクトリ>/taskqueues/<name>/instructions/<job>.r<rev>.md` に書き出す。
- command の `{instructions}` をそのパスに置き換える。環境変数 `PC_JOB_INSTRUCTIONS` (パス) と `PC_JOB_INSTRUCTIONS_REV` も渡す。
- 実行するプログラムは元のファイルではなく、このパスを読む。これで「実際に何を指示されて実行したか」が版として残る。

**更新**
- 元ファイルの変更は自動では取り込まない (実行中や再実行の途中で内容がすり替わらないように)。
- 取り込むのは次の 2 つだけ。どちらも running 中のジョブにはできない:
  - `taskqueue_retry <id>` に `refreshInstructions: true` (既定 true): 元ファイルを読み直して新しい版にしてから pending に戻す。「カードを直して再実行」がこの 1 回の操作で済む。
  - `taskqueue_update <id>` に `instructionsFile` / `instructionsText`。
- 新しい版ができたら `rev+1`、events.jsonl に `instructionsUpdated` (旧/新 sha256) を記録する。古い版は削除せず残す。
- `taskqueue_get` / `taskqueue_list` は `sourceChanged` (元ファイルの現在の sha256 がジョブの版と違う) を返す。元ファイルが消えていれば `sourceMissing`。取り込み忘れに気付けるようにするため。

### 5.5 サマリ (summary)
ジョブの内容を表す短い文字列。指示書の本文とは別に、**コマンドラインで渡す**。
- 登録: `taskqueue_add` の `jobs[].summary` (CLI では `--summary "..."`)。必須、1 行 (改行不可)、200 文字以内。
- 実行: command の `{summary}` を、その文字列を **1 個の引数として**クォートしたものに置き換える (空白・引用符・`&` などは PC がエスケープする。置換後の文字列は ChainGuard の対象外)。環境変数 `PC_JOB_SUMMARY` にも入れる。
- 表示: `taskqueue_list` / `taskqueue_graph` / GUI の一覧 / 通知イベントに出す。人間とエージェントがジョブを一目で見分けるため。
- 変更: `taskqueue_update` で変更できる (running 中は不可)。
- PC はサマリの意味も解釈しない。

例: `"command": "tools\\run.cmd {instructions} {summary}"`、`"summary": "Apply placement rules and bake test stage"`

## 6. 状態遷移
```
pending ──(依存条件をすべて満たし、空きあり、paused でない)──> running
pending ──(success 依存の依存元が failed/blocked/cancelled)──> blocked
running ──(成功条件を満たす)──> succeeded
running ──(それ以外で終了)──> attempt 記録
     attempts < maxAttempts → pending (retryDelaySec 後、元の位置のまま)
     attempts >= maxAttempts → failed
running ──(cancel)──> cancelled (ツリー kill)
(PC 起動時に running だったジョブ) ──> reason=interrupted で attempt 記録 → 上と同じ再実行判定
failed / blocked / cancelled ──(taskqueue_retry)──> pending
```
- `blocked` は依存元が `pending` に戻れば自動で `pending` に戻る。
- スケジューリング: 実行可能なジョブを `priority` 降順 → 登録順で選ぶ。再実行するジョブは後ろに回さない。
- 依存の循環・存在しない id への依存は登録時に拒否する。

## 7. 再実行時の文脈 (環境変数)
| 変数 | 内容 |
|---|---|
| `PC_JOB_ID` | ジョブ id (全 attempt) |
| `PC_ATTEMPT` | 1 始まりの attempt 番号 (全 attempt) |
| `PC_JOB_SUMMARY` | ジョブのサマリ (全 attempt) |
| `PC_JOB_INSTRUCTIONS` | 指示書の版を書き出したファイルのパス (指示書のあるジョブ、全 attempt) |
| `PC_JOB_INSTRUCTIONS_REV` | その版番号 |
| `PC_PREV_REASON` | 前回の reason (2 回目以降) |
| `PC_PREV_EXIT` | 前回の exit code (2 回目以降) |
| `PC_PREV_TAIL` | 前回のログ末尾 50 行を保存したファイルのパス (2 回目以降) |

## 8. 連鎖コマンドの禁止 (ChainGuard)
### 8.1 対象
`start_process` と `taskqueue_add` の `command` すべて。

### 8.2 検出規則
- 外側の `cmd /c "..."` / `cmd.exe /c ...` / `powershell -Command "..."` は外して中身も調べる。
- 引用符 `"..."` の中と `^` でエスケープされた文字は対象外。それ以外 (括弧の中を含む) で次のどれかがあれば**連鎖**とみなす:
  - `&&`, `||`, 単独の `&`, 改行
  - PowerShell の `;` (`powershell` / `pwsh` の `-Command` 引数の中)
- 次の**待ち合わせ**パターンも拒否する: 別プロセスの完了をファイルやプロセスの存在でポーリングする書き方 (例: `while (-not (Test-Path X)) { Start-Sleep N }`、`:loop ... if not exist X goto loop`)。検出は単純なパターン一致でよい。

### 8.3 拒否したときの応答
エラーで返し、プロセスは起動しない。エラーには次を含める:
- どこで連鎖を検出したか (位置と演算子)。
- 分割したコマンドの候補と、それを `taskqueue_add` で登録する JSON の例 (各ジョブに `timeoutSec` を入れる欄、後のジョブに `dependsOn` を付けた形)。
- 「タイムアウトは合算せず、ジョブごとに指定する」という一文。
- 待ち合わせパターンなら「`dependsOn` を使う」。

### 8.4 例外
- `pipe` (`|`) は連鎖ではないので許可する。
- ユーザーが本当に連鎖を 1 プロセスで流したい場合: **GUI と CLI のみ** `--allow-chain` で許可する。MCP には出さない (AI エージェントは回避できない)。
- スクリプトファイル (`.cmd` / `.ps1`) の中身は調べない。連鎖をスクリプトに隠すことは防げないが、運用ルール側で禁止する。

## 9. 永続化
- `<PC データディレクトリ>/taskqueues/<name>.json` に TaskQueue と Job をまとめて保存する。状態が変わるたびに一時ファイルへ書いて rename し、書きかけの壊れたファイルを残さない。
- 指示書の各版: `taskqueues/<name>/instructions/<job>.r<rev>.md`。ジョブの JSON には最新版の本文とメタデータを持つ。
- イベントログ: `logs/taskqueue-<name>.events.jsonl` に 1 行 1 イベントで追記する。
- PC 起動時: `running` だったジョブのプロセスがまだ生きていれば接続し直す。できなければ `interrupted` として扱う。
- 終了したジョブは `taskqueue_purge` するまで履歴として残す。

## 10. 通知
イベント種別: `jobStarted`, `jobAttemptFailed`, `jobSucceeded`, `jobFailed`, `jobBlocked`, `jobCancelled`, `taskqueueDrained` (pending/running なし), `taskqueueStalled` (pending はあるが実行できるものがない)。

各イベントで:
1. events.jsonl に追記し、TaskQueue の `version` を +1。
2. `taskqueueDrained` / `taskqueueStalled` / `jobFailed` のとき `notifyFile` を作成または更新する (呼び出し側はこれを監視して起きる)。
3. 任意設定 `onEvent`: イベント JSON を渡してコマンドを実行する (トースト・チャット通知など)。失敗してもタスクキューは止めない。
4. GUI を更新する。

## 11. MCP ツール
| ツール | 引数 | 戻り値 / 動作 |
|---|---|---|
| `start_process` (変更) | 既存のまま | 連鎖・待ち合わせなら 8.3 のエラー。単一コマンドなら従来通り (タスクキュー外) |
| `taskqueue_add` | `taskqueue`, `jobs[]`, `concurrency?`, `notifyFile?` | 追加したジョブ id。連鎖・循環・重複 id・未知の依存・`summary` / `timeoutSec` なし・上限超過があれば全体を追加しない |
| `taskqueue_list` | `taskqueue?`, `state?` | ジョブ一覧 (状態・attempt 数・最後の reason・blockedBy) |
| `taskqueue_get` | `id`, `includeInstructions?` (既定 true), `rev?` | 全情報と attempt 履歴、指示書の本文 (指定の版)、`sourceChanged` / `sourceMissing` |
| `taskqueue_retry` | `id`, `resetAttempts?` (既定 true), `refreshInstructions?` (既定 true) | 指示書を元ファイルから取り込み直し、failed/blocked/cancelled を pending に戻す |
| `taskqueue_update` | `id`, 変更可能フィールド (summary, instructionsFile, instructionsText, command, timeoutSec, idle, maxAttempts, success, env)。依存は 11.1 で変更する | pending/failed/blocked のときだけ。同じ検証 (連鎖・上限) を行う |
| `taskqueue_cancel` | `id` | running ならツリー kill して cancelled |
| `taskqueue_pause` / `taskqueue_resume` | `taskqueue` | 新しいジョブの開始を止める / 再開する (実行中は止めない) |
| `taskqueue_wait` | `taskqueue`, `sinceVersion`, `timeoutMs` | version が変わるまで待ち、変化したジョブと新しい version を返す |
| `taskqueue_purge` | `taskqueue`, `states[]` | 終了済みジョブを削除 |

### 11.1 依存関係の管理コマンド
依存の操作は次の 4 つに限る。どれも**宣言を変えるだけ**で、ジョブの起動は Scheduler が行う。
| ツール | 引数 | 動作 |
|---|---|---|
| `taskqueue_add` | `jobs[].dependsOn` | ジョブ登録と同時に依存を宣言する (基本はこれだけで足りる) |
| `taskqueue_link` | `job`, `dependsOn` (id), `on?` (`success` 既定 / `done`) | 既存ジョブに依存を 1 本追加する |
| `taskqueue_unlink` | `job`, `dependsOn` (id) | 依存を 1 本外す |
| `taskqueue_graph` | `taskqueue?`, `root?` (job id) | 依存グラフを返す: 各ジョブの状態、依存先と依存元、`ready` (今すぐ開始できる)、`waiting` (何を待っているか)、`blocked` (どの失敗が原因か) |

規則:
- 変更できるのは `pending` / `blocked` / `failed` のジョブだけ。`running` / `succeeded` / `cancelled` のジョブの依存は変更できない (エラー)。
- 循環や存在しない id ができる変更はエラー。何も変わらない。
- 変更したらすぐ状態を評価し直す: 失敗済みのジョブへの依存を足すと `blocked`、原因の依存を外すと `pending` に戻る。結果を戻り値で返す。
- `taskqueue_update` では `dependsOn` を変更できない (依存の変更は上の 2 つに限定し、操作を追跡しやすくする)。変更はすべて events.jsonl に `dependencyLinked` / `dependencyUnlinked` として記録する。
- よく使う確認は `taskqueue_graph`。「次に何が動くか」「なぜ止まっているか」を呼び出し側が推測せずに読める。

- **成功への強制変更、`maxTimeoutSec` の変更、`--allow-chain` は MCP に出さない。**
- `kill_process` / `wait_process` / `list_processes` は従来通り。タスクキューから起動したプロセスには `jobId` を付ける。タスクキューのジョブに対する `kill_process` は、その attempt を `killed` として扱い再実行判定を行う (cancel ではない)。
- `start_process` のツール説明文に「複数のコマンドを順に実行するときは `taskqueue_add` を使う。`&&` は拒否される」と書く (AI エージェントが最初から正しい方を選べるように)。

## 12. CLI
`pc taskqueue add --file jobs.json`、`pc taskqueue list main`、`pc taskqueue retry <id>`、`pc taskqueue cancel <id>`、`pc run "<cmd>" [--allow-chain]`。jobs.json は `taskqueue_add` の引数と同じ形式。

## 13. GUI
既存ダッシュボード (暗色テーマ、上部カウンタ、ツールバー、表) の見た目と操作感に合わせる。描画は既存と同じ Direct2D。

### 13.1 全体構成
```
┌ [プロセス] [タスクキュー]     表示キュー: [☑main ☑worktree]   表示: [表|グラフ] ┐
├ 全体: 待機 2 │ 実行中 2 │ ブロック 1 │ 失敗 1 │ 成功 6                          ┤
├ main (同時 1, 実行中 1/1) 待機1 ブロック1 失敗1 成功4  [一時停止]              ┤
├ worktree (同時 3, 実行中 1/3) 待機1 成功2               [一時停止]              ┤
├ ⚠ job-B が失敗 (2/2, idle)。job-C が待ち続けています。 [job-B を表示]          ┤
├ ジョブ表 または 依存グラフ (全キューを 1 画面に、13.3 / 13.4)                   ┤
├ 詳細パネル (13.5)                                                                ┤
└ ステータスバー: 選択ジョブ / 見込み終了時刻 (キューごと)                         ┘
```
- 上段のタブで既存の「プロセス」画面と「タスクキュー」画面を切り替える。
- **すべてのキューを 1 画面に表示するのが基本。** キューを切り替える操作はない。「表示キュー」のチェックは絞り込み用 (既定ですべてオン)。キューをまたぐ依存もそのまま 1 つの表・グラフで追える。
- キューの帯 (キューごとに 1 行): キュー名、同時実行数と使用中の枠、状態別の件数、一時停止 / 再開ボタン。一時停止中は帯を黄色にする。キューが増えたら帯も増える (多い場合は折りたたみ)。
- 表示切り替え: 「表」と「グラフ」。選択中のジョブは両方で共有する。

### 13.2 カウンタと警告バナー
- カウンタ: 全キュー合計の待機 (pending)・実行中・ブロック・失敗・成功の 5 つ。クリックするとその状態で絞り込む。キューごとの件数は 13.1 のキューの帯に出す (帯の件数クリックで「そのキュー × その状態」に絞り込む)。
- 警告バナー: 次のときだけ赤で表示する。クリックで原因のジョブを選択する。
  - `failed` のジョブがある: 「<id> が失敗 (<試行>/<上限>, <理由>)。<後続 id…> が待ち続けています。」
  - `taskqueueStalled`: 「<キュー> で実行できるジョブがありません。原因: <id>」
- 複数の警告は 1 行ずつ並べる (最大 3 行、それ以上は「ほか n 件」)。
- 一時停止はキューの帯で表示する (バナーは出さない)。

### 13.3 ジョブ表
| 列 | 内容 |
|---|---|
| ID | ジョブ id |
| サマリ | `summary` |
| 状態 | 色付きバッジ (成功=緑、実行中=青、ブロック=黄、失敗=赤、待機/キャンセル=灰) |
| 試行 | `<実行済み>/<maxAttempts>` |
| 経過 / 上限 | 実行中は現在の試行の経過時間と `timeoutSec`。待機中は上限のみ |
| 依存 / 理由 | 依存元 (`← A, B`)。止まっていれば理由 (`idle`、`空き待ち`、`B の失敗` など) |
| キュー | キュー名 (常に表示) |
- 1 つの表に全キューのジョブを並べる。
- 並び順: 依存の順 (トポロジカル順、キューをまたいで計算)。同順位は キュー → priority → 登録順。
- 「キューごとにまとめる」切り替え: オンにするとキュー名の見出し行で区切って表示する (見出し行は折りたたみ可)。キューをまたぐ依存は依存列に `← worktree:W` のようにキュー名つきで出す。
- 列見出しクリックで並べ替え (既存の表と同じ)。終了済みジョブの非表示切り替え。

### 13.4 依存グラフ
- 全キューのジョブを 1 つのグラフに描く。キューごとに横長の帯 (スイムレーン) を作り、ジョブはそのキューの帯の中に置く。帯の左端にキュー名・同時実行数・一時停止状態。
- 左から右へ流れる層状レイアウト。層 = 依存の根からの最長距離 (全キュー共通で計算するので、帯が違っても同じ層は縦にそろう)。同じ帯・同じ層の中は交差が少なくなる順に並べる (単純なヒューリスティックでよい)。
- キューをまたぐ依存は帯をまたぐ線で描く。
- ノード: 角丸の箱に ID・サマリ (省略表示)・試行回数。塗りは 13.3 の状態色。実行中は経過/上限の進捗バーを箱の下端に表示する。
- エッジ: `on: success` は実線、`on: done` は破線。依存元が失敗してブロックしている辺は赤。
- 「表示キュー」で外したキューのジョブは描かない。ただし表示中のジョブの依存元・依存先になっている場合だけ、点線枠の薄いノードとしてキュー名を添えて描く (依存の切れ目が見えなくならないように)。
- 操作:
  - クリック: ジョブを選択し、詳細パネルに表示。
  - 選択したジョブの上流と下流をハイライトする。失敗ジョブを選ぶと、それが原因でブロックしているジョブがすべて強調される。
  - ホイールでズーム、ドラッグでパン、「全体表示」ボタン。
  - 右クリックメニュー: 詳細パネルと同じ操作 (再実行、キャンセルなど)。
- ノード数の上限は設けないが、200 を超える場合は終了済みジョブを畳んで表示する。

### 13.5 詳細パネル
見出し: ID、サマリ、状態。元ファイルの指示書が変わっていれば黄色のバッジ「指示書が更新されています (取り込み待ち)」。元ファイルが消えていれば「元ファイルなし」。

タブ:
| タブ | 内容 |
|---|---|
| 試行履歴 | 各試行の番号・開始/終了・理由・exit code・使った指示書の版・`checkDetail`。プロセス番号をクリックすると「プロセス」タブの該当行を選択してログを開く |
| 指示書 | 本文 (読み取り専用)。版の切り替えと、前の版との差分表示 |
| 依存 | 依存元と依存先の一覧 (状態つき)。それぞれクリックで選択を移す |
| イベント | このジョブの events.jsonl の行 |

ボタン:
| ボタン | 動作 | 備考 |
|---|---|---|
| 再実行 | `taskqueue_retry` (指示書を取り込む) | failed / blocked / cancelled のとき |
| キャンセル | `taskqueue_cancel` | |
| 上限を延長 | 実行中の試行の deadline を延ばす (分を入力) | GUI のみ |
| 強制成功… | succeeded にする。理由の入力と確認ダイアログ必須。イベントに `forcedSuccess` を記録 | GUI のみ |

キューに対する操作: 一時停止 / 再開はキューの帯のボタン。終了済みを消去 (`taskqueue_purge`) はツールバー (表示中のキューすべてが対象、確認ダイアログつき)。

### 13.6 既存の「プロセス」タブへの追加
- 「ジョブ」列 (`jobId #試行`)。タスクキュー外のプロセスは空欄。
- 「ジョブで絞り込み」: 選択中のジョブのプロセスだけを表示する。
- ジョブ列のダブルクリックで「タスクキュー」タブの該当ジョブへ移る。

### 13.7 通知
- トレイのバルーン通知: `jobFailed`、`taskqueueStalled`、`taskqueueDrained`。クリックでダッシュボードを開き、該当ジョブを選択する。
- `jobAttemptFailed` (自動再実行される失敗) は通知しない。バナーと表にだけ出す。

### 13.8 設定画面
`maxTimeoutSec`、`defaultMaxAttempts`、idle の既定値 (`outputSec` / `cpuSec`)、連鎖拒否の有効/無効、`onEvent` コマンド、通知の有効/無効。

## 14. 既存実装との対応
- 1 attempt = 既存の `StartProcessRequest` で起動する managed process 1 件。`timeoutSec` / `idleTimeoutSec` / `doneFile` / ツリー kill を流用する。
- idle 判定は既存の出力 idle に CPU idle (`lastCpuActivityTickMs`) を AND で追加する。タスクキュー外の単独プロセスの `idle_timeout_sec` の動作は変えない。
- `killExclude` はツリー kill の対象からイメージ名で除外する (除外したプロセスの子も残す)。

## 15. 受け入れテスト
偽ジョブ (`cmd /c exit 0`、`cmd /c exit 1`、出力なしで sleep、CPU を使い続けて出力なし、ファイルを書かずに exit 0) で確認する。
1. **依存:** A → B(dependsOn A) と独立ジョブ C。A が exit 1 を上限まで → A failed、B blocked、C succeeded、`taskqueueStalled` か `taskqueueDrained`。
2. **終了 ≠ 成功:** A が exit 0 だが `filesNewer` を書かない → checkFailed → 再実行 → failed。B は開始しない。
3. **再実行:** A が 1 回目 timeout、2 回目成功 → B が始まる。2 回目に `PC_ATTEMPT=2`、`PC_PREV_REASON=timeout`。
4. **ジョブ単位のタイムアウト:** A がハング → A だけ自分の timeoutSec で止まり、B/C はそれぞれ自分の timeoutSec を持つ。
5. **idle の誤判定防止:** CPU を使い続けて出力しないジョブは kill されない。両方止まったジョブは kill される。
6. **killExclude:** 除外イメージ名のプロセスは生き残る。
7. **永続化:** 実行中に PC を終了 → 再起動 → interrupted として再実行され、続きのジョブも進む。
8. **retry:** failed の A を `taskqueue_retry` → A pending、blocked の B も pending。
9. **並列数:** concurrency=1 なら同時に 1 つ、2 なら 2 つ。
10. **通知:** `taskqueueDrained` で notifyFile ができ、`taskqueue_wait` が新しい version を返す。
11. **MCP で回避できない:** 強制成功・`maxTimeoutSec` 変更・`--allow-chain` が MCP にない。
12. **ChainGuard (start_process / taskqueue_add / taskqueue_update 共通):**
    - 拒否: `A && B`、`cmd /c "A && B"`、`A & B`、`A || B`、`( A && B )`、改行区切り、`powershell -Command "a; b"`、`while(-not (Test-Path X)){Start-Sleep 20}`。
    - 許可: `"x && y"` を引数に含む単一コマンド、`A | B`、`.cmd` スクリプト 1 本。
    - 拒否時のエラーに検出位置・分割候補・`taskqueue_add` の JSON 例が含まれる。
18. **依存の操作:** `taskqueue_link` で循環 → エラーで何も変わらない。failed のジョブへ link → 即 blocked。unlink で原因が消える → pending。running のジョブへの link/unlink → エラー。`taskqueue_graph` が ready / waiting / blocked と原因を返す。
17. **手動起動の拒否:** failed のジョブ A と同じ command を `start_process` → エラー (`taskqueue_retry A` を案内)。A を `taskqueue_retry` → A とその blocked の後続がすべて pending に戻る。
16. **タスクキューをまたぐ依存:** `worktree` (concurrency 3) のジョブ W に `main` のジョブ M が依存 → W 成功後に M 開始。W failed なら M blocked。
13. **タイムアウト上限:** `timeoutSec` がない、または `maxTimeoutSec` 超 → `taskqueue_add` がエラー、何も追加されない。
15. **タイムアウトの受け渡し:** 依存待ちの時間は数えない。2 回目のアテンプトも満額。再接続したアテンプトは元の deadline のまま。
14. **CLI `--allow-chain`:** 連鎖が 1 プロセスで起動される (従来動作)。
19. **指示書:**
    - `instructionsFile` で登録 → 本文がジョブに保存され、元ファイルを削除しても実行できる (`sourceMissing` が出る)。
    - 実行時に `{instructions}` と `PC_JOB_INSTRUCTIONS` が版のファイルを指し、中身が登録時の本文と一致する。
    - 実行中に元ファイルを書き換えても、その attempt と自動再実行は旧版のまま。`sourceChanged` が true になる。
    - failed のジョブを `taskqueue_retry` → 新しい版 (rev 2) が取り込まれ、次の attempt の `instructionsRev` が 2。
    - 中身が任意の形式でも、そのまま保存・受け渡しされる (PC は読まない)。
20. **サマリ:**
    - `summary` なし / 改行を含む / 200 文字超 → 登録エラー。
    - `{summary}` が 1 個の引数としてコマンドラインに渡る (空白・引用符・`&` を含んでも分割されず、ChainGuard にも掛からない)。`PC_JOB_SUMMARY` にも同じ値。
    - `taskqueue_list` / GUI / 通知に summary が表示される。

## 16. 範囲外
- 複数マシン間の分散実行。
- cron などの時刻指定実行。
- スクリプトファイルの中身の検査。
- ジョブの出力の意味解析 (成功条件の正規表現以外)。

