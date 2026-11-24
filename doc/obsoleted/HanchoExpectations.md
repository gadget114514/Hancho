# Hancho に期待すること（運用契約）

作成日: 2026-11-19

## 1. 背景

カード718を supervisor セッションから `ww-run.cmd 718 -Main -Engine opencode` で**直接**起動したところ、opencode.exe は起動した（`running/718.last.json` の `OpencodePid 44976`）が 60 分間 `.events.jsonl` = 0 バイト、`.stderr.log` = 0 バイト、`.progress.log` = `start` の 1 行だけで、コード編集もレポートもブランチも作成されなかった。親プロセスは `ww-run.ps1:429` の `ReadLine()` でブロックしたまま、呼び出し側シェルのタイムアウトでプロセスツリーごと kill された。

同じ `-Engine opencode -Main` のカード717は開始 11 秒後に最初のイベントを出しており、ww-run の起動手順自体は正常だった。したがってハングは opencode の初期化／最初のモデル呼び出しの段階と推定される。

**問題はハングそのものより、それが Hancho のジョブとして登録されていなかったため、チャット以外のどこにも記録されなかったこと。** Hancho 側には同じ失敗を構造化して残す仕組みがあるのに使っていなかった。

## 2. Hancho が既に持っている機能（確認済み）

| 機能 | コマンド／根拠 | 期待できること |
|---|---|---|
| 失敗の診断 | `taskqueue_get` / `taskqueue why <id>` | 失敗ジョブに `failure class` / `summary` / `lastActivity` / `evidence` / `suggestedAction` が付く |
| ジョブへの記録 | `taskqueue_update --id <id> --patch <json>` | 事後的にメモ／状態をジョブへ書き戻せる |
| 判断の理由 | `bt_resume`（`answer` + `rationale`） | エスカレーションの回答に理由を残せる |
| 再試行 | `taskqueue_retry` / `bt_retry` | 同じジョブを再投入できる |
| 見積りの学習 | `estimateSec` + `timing_feedback` | 成功後に `diverged_over` / `diverged_under` / `accurate` が付き、後続ジョブの見積りを補正できる |
| 永続メモ | `memory_note` / `memory_search` | 再発防止の教訓をセッションを越えて残せる |
| チャット | `chat_send` / `chat_log` | セッション間で知見を共有できる |
| 状態可視化 | `taskqueue_list` / `taskqueue_graph` | ダッシュボードで進行／ブロック／失敗が見える |

Hancho CLI のヘルプには明示的に「Only taskqueue / behavior-tree jobs appear in the task list and count toward per-engine/model LLM usage」とある。**`ww-run` を直接起動しただけでは Hancho の対象外**で、上記のどれも働かない。

## 3. 期待する運用（必須）

### 3.1 カード実行は必ずジョブとして登録する

- `ww-run.cmd` を `run_in_background` やフォアグラウンドで直接起動しない。カードは taskqueue（キュー `cards`）に登録して Hancho に実行させる（`CLAUDE.md` 2 章のとおり）。
- これにより、タイムアウト・ハング・編集違反が自動的に failed job + diagnosis として残る。
- 1 枚だけならJob JSON を `taskqueue add` する。複数枚／結果で次が変わるなら Behavior Tree（`data/weapons/bt/<id>.json`）にして `bt_start` → `bt_wait` → `bt_resume` で回す。

### 3.2 ジョブ定義の必須項目

| 項目 | 期待値 |
|---|---|
| `id` | `card-NNN` |
| `taskqueue` | `cards` |
| `command` | `Tools\ww\ww-run.cmd <NNN> [-Main] -Engine opencode -JsonStdout -QueueLocked` |
| `timeoutSec` | カードの見積時間（推定）。Unity カードは余裕を見て設定 |
| `estimateSec` | 同上。成功後の `timing_feedback` で補正する |
| `resources.write` | Unity を使うカードは `@unity-editor`（同時 1 枚） |
| 成功条件 | 終了コード + `filesNewer: data/weapons/reports/NNN.md` + レポートの `contentFile` / `contentRegex` |
| `instructionsFile` / `instructionsText` | カードへの詳細指示（1 行 `summary` も必須） |
| モデル | opencode ワーカーには `--model` を渡す（下記 3.3） |
| データディレクトリ | 既定の共有 `Hancho\logs` を使う。`--data-dir` は付けない（CLAUDE.md 2 章） |

### 3.3 opencode ワーカーには `--model` を渡す

Hancho ヘルプ:「Workers must emit a JSON stream and receive `--model` (opencode: `opencode run --format json --model <provider/model>`)」。

しかし現状の `ww-run.lib.ps1:92` の `Get-EngineCommand` は

```
& opencode.cmd run --format json$(if ($Model) { " --model '$m'" }) `$prompt
```

で、`-Model` を渡さなければ `--model` が付かない（カード718の `-DryRun` 出力も `command : & opencode.cmd run --format json $prompt` だった）。`opencode.json` の既定モデルに頼らず、**ジョブ側で明示的に `-Model` を渡す**か、ww-run 側を直す必要がある。

### 3.4 失敗・ハングが起きたときの手順

7.5 D3 / D4 に合わせた手順（「自分でやる」は禁止ではなく、記録付きの最後の選択肢）。

1. タイムアウト／failed を待つ（自分のシェルのタイムアウトで kill しない）。初回イベント監視（`idle.firstEventSec`、worker 既定 180 s）と idle 監視に任せる。起動直後の無応答は `startup_timed_out` として止まり、診断では `startup_hang` になる。kill 前のプロセストリースナップショットは `detail.processTree` に残る。
2. `taskqueue why <id>`（または `taskqueue get --json` の `failure`）で `class` / `likelyCauses` / `nextSteps` / `supervisorNote` を読む。必要なら `.events.jsonl` のサイズ、`.stderr.log`、`.progress.log` の最終行、`OpencodePid` も証拠として添える（`taskqueue_update --id <id> --patch <json>` でメモ可）。
3. `nextSteps` の順に試す。1 回目は同じ設定で `taskqueue retry` → 2 回目は `change` を付けて設定を変える（モデルの変更は `taskqueue update --approve-model-change --reason "..."`、エンジン変更も承認付き）→ 3 回目は escalate。BT ジョブの再実行は `bt_retry`。
4. 自分で実装するのは**記録付きの最後の選択肢**。単発ジョブは `taskqueue takeover --id <id> --reason "..."`、BT は `bt resume --answer takeover --rationale "..."` で判断を残してから実装し、終わったら `taskqueue complete --id <id> --reason "..." [--report <path>]` で結果を戻す（`--result succeeded` 既定で後続の `dependsOn` が進む）。
5. 教訓（例: `--model` 未指定、同じエンジンのネスト）は `memory_note` または `data/weapons/decisions/NNN.md` に昇格させる。
6. チャットへの報告は**補足**。一次記録は Hancho ジョブに残す。黙って委任をやめない。

### 3.5 Behavior Tree を使う場合

- 各カードは `fallback[job, sequence[escalate(expect continue/stop), var <escalate>.answer == continue]]` の形にする（CLAUDE.md 2 章）。
- `bt_validate` → `bt_start` → `bt_get` で `version` を読み、`bt_wait` は必ず `sinceVersion` を付ける（付けないと途中の変化で即戻る）。
- `waitingForLlm` ならレポートを読んで `bt_resume(node, answer, rationale)`。**rationale を省略しない**。
- BT ジョブの再実行は `bt_retry`（`taskqueue_retry` は不可）。

### 3.6 役割の明確化

Hancho ヘルプは「supervisor と worker が同じエンジンのときは役割が曖昧になり得る。別エンジンにするか、明確に異なるロール／プロンプトを与える」と助言している。今回は opencode セッションが opencode ワーカーを起動する形だったため、この助言に反していた。可能なら worker を `agy` / `claude` にするか、`You ARE the ww worker` のカード冒頭と `-Engine opencode` の組み合わせを維持する。

## 4. 今回のケースで Hancho に期待した動作（ギャップ）

| 期待 | 実際 |
|---|---|
| カード718が `card-718` ジョブとして `cards` キューに載る | 載っていない（`ww-run` を直接起動したため） |
| 3600 s で `failed` になり、diagnosis に `failure class=timeout/hang`、`evidence=events.jsonl 0 bytes, stderr 0 bytes`、`suggestedAction=retry with --model / different engine` が入る | ジョブが無いため diagnosis なし |
| `taskqueue why card-718` で原因を確認できる | できない |
| 教訓が `memory_note` に残り、次回のカード投入時に参照される | 残っていない（チャットにしかない） |

カード710〜717は `fx-setup-710-717` という BT のジョブとして Hancho に登録されており、717 は `succeeded` と記録されている。718 だけがこの契約から外れていた。

## 5. 改善候補（未実施）

1.  supervisor がカードを起動するときは必ず Hancho の `taskqueue_add` を通すラッパーを用意する（`ww-run.cmd` を直接呼ばない）。
2.  `ww-enqueue.cmd` は旧 ProcessCommander (`pcder`) 前提なので、Hancho 版の enqueue を用意する。Job JSON は `data/weapons/jobs/` に置く。
3.  ww-run の opencode コマンドに既定で `--model` を渡す（`WW_MODEL` 未設定時に `opencode.json` のモデルを解決して渡す）。
4.  ハング検知の triage を Hang 用に拡張し、`events = 0` かつ `elapsed > estimateSec` のときは自動で `taskqueue_update` に incident を書く。**090 で実装済み**: 090-1 が `startup_hang` / `retry_with_change` と `nextSteps` に基づく triage 推奨を追加し、090-2 が 0 イベントの起動ハングを既定 180 s で検知してジョブの attempt に診断（`class` / `evidence` / `likelyCauses` / `nextSteps`）として自動記録するようにした。なお `taskqueue_update` で別途 incident メモを自動生成する部分は 090-1〜3 には入っていない。

## 6. 今回の実装への影響

カード718／719 の実装自体はこのセッションで直接行い、compile 0 エラー／EditMode 399/399 パス／レポート作成まで完了している（各レポート参照）。Hancho への登録・コミット・マージは行っていない。上記の運用契約は、今後のカード実行と incident 記録の改善のためのもの。

## 7. 対策案: ハングを「次の一手」につなげ、supervisor が Hancho を捨てないようにする

### 7.1 何が起きたか（問題の構造）

今回の失敗は 2 段になっている。

1. **ハングが「ハングした」以上の情報にならなかった。** 60 分待って分かったのは「止まった」ことだけで、原因の候補も次に試すことも出てこなかった。
2. **supervisor が委任をやめて自分で実装した。** 次の一手が示されない以上、LLM にとって一番手軽な道は「自分で書く」だった。しかもその判断はどこにも記録されず、Hancho から見ると「カードが消えた」状態になった。

2 は 1 の結果でもある。対策は「早く・具体的に・選択肢付きで失敗を返す」ことと、「引き取るなら記録付きの判断にする」ことの両方が要る。

現行実装で確認したギャップ:

| 箇所 | 現状 | 問題 |
|---|---|---|
| `src/taskqueue.cpp` の診断ルール 3（`timeout`） | イベント数に関係なく `suggestedAction = "raise_timeout"` | イベント 0 件のまま 60 分経ったジョブに「タイムアウトを延ばせ」と勧めてしまう。完全に逆の助言 |
| `src/taskqueue.cpp` の診断ルール 4（`idle_stall`） | `suggestedAction = "retry"` だけ | 同条件で再実行してもまた止まる可能性が高い。何を変えるかがない |
| `src/triage.cpp` の `stalled` / `idle_stall` | 推奨候補が `investigate`（「まず調べろ」） | LLM にとっては行き止まり。何を調べ、調べた後どうするかがない |
| `idle` の既定値 | `output_sec = 0`, `cpu_sec = 0`（無効） | worker ジョブでも無出力検知が働かず、`timeoutSec` まで丸ごと待つ |
| 起動直後の無応答 | 専用の検知なし | 「最初のイベントが来ない」は最も多く、最も早く分かるハングなのに区別されない |

### 7.2 対策 A: 早く検知する（Hancho 本体）

**A1. 初回イベント待ちの監視（`idle.firstEventSec`）を追加する。**（実装済み: 090-2）
- JSON ストリームの worker ジョブでは、最初の有効イベント（`events.jsonl` に 1 行目が出るまで）を別の時計で監視する。
- ラッパー（`ww-run.cmd` など）が出す非 JSON の行は数えない。今回は `.progress.log` に `start` が出ていたが、これを「活動」と見なすと検知できない。
- 既定値は 180 s 程度。カード717は 11 s で最初のイベントを出しているので、桁違いの余裕がある。
- 超えたら `endKind = "startup_timed_out"` で止める。

**A2. worker / LLM ジョブでは `idle` を既定で有効にする。**（実装済み: 090-2）
- `engine` 付き・`worker` ノード由来のジョブは、未指定なら `idle.output_sec = 600` 程度を既定にする。明示的に `0` を書いたときだけ無効にする。
- 監視対象は「パース済みイベント」の時刻にする（生の stdout ではなく）。

**A3. 実行中に「止まりかけ」を通知する。**（未実装）
- `firstEventSec` や `idle.output_sec` の半分を過ぎたら、ジョブに `stallWarning` を付け、`bt_wait` / `taskqueue wait` をその時点で一度返す（新しい `version` を立てる）。
- supervisor は 60 分ではなく数分で状況を知り、待つか止めるかを判断できる。

### 7.3 対策 B: 失敗を「原因の候補＋次の一手」として返す（診断・triage）

**B1. `startup_hang` を新しい失敗クラスとして分ける。**（実装済み: 090-1。kill 前のプロセスツリーは 090-2）

条件: イベント 0 件（または `run.started` 以降に何も無い）で `timeout` / `idle_timed_out` / `startup_timed_out` になった。

診断に入れる内容:

```json
{
  "class": "startup_hang",
  "summary": "The worker started (pid 44976) but emitted no events in 3600 s.",
  "evidence": {
    "eventsBytes": 0, "stderrBytes": 0,
    "lastProgressLine": "start",
    "processTree": [{"name": "opencode.exe", "pid": 44976, "cpuSec": 0.4, "state": "waiting"}],
    "commandHasModel": false,
    "supervisorEngine": "opencode", "workerEngine": "opencode"
  },
  "likelyCauses": [
    {"id": "no_model", "why": "opencode was launched without --model"},
    {"id": "same_engine_nesting", "why": "supervisor and worker are both opencode"},
    {"id": "waiting_for_input", "why": "the process tree used almost no CPU; it may be waiting for a prompt or stdin"}
  ],
  "suggestedAction": "retry_with_change",
  "nextSteps": [
    {"action": "retry", "change": {"model": "<explicit model>"}, "needsApproval": true},
    {"action": "retry", "change": {"engine": "claude"}, "needsApproval": true},
    {"action": "escalate", "question": "Worker hung at startup twice. Retry with another engine, or take over?"}
  ]
}
```

- `likelyCauses` は機械的に判定できるものだけ出す: コマンドに `--model` が無い、supervisor と worker のエンジンが同じ、プロセスツリーの CPU がほぼ 0（入力待ちの疑い）、stderr に何かある、など。
- `processTree` はタイムアウトで kill する**前**に取る。kill 後では何が止まっていたか分からない。

**B2. `timeout` の診断をイベントの有無で分ける。**（実装済み: 090-1）
- イベント 0 件 → `startup_hang`（上記）。`raise_timeout` は勧めない。
- 最後のイベントがタイムアウト直前 → 今どおり `raise_timeout`。
- 途中で止まった → `idle_stall` 扱い。最後のイベント（どのツール呼び出しで止まったか）を `evidence` に入れる。

**B3. triage の推奨を `investigate` で終わらせない。**（実装済み: 090-1）
- `stalled` / `idle_stall` / `startup_hang` の推奨は、上の `nextSteps` の先頭（1 回目は「同じ設定で 1 回だけ再試行」、2 回目以降は「設定を変えて再試行」、3 回目は「エスカレーション」）にする。
- `investigate` を出す場合も、「`taskqueue why <id>` の `likelyCauses` を確認し、`nextSteps` から選ぶ」と具体的に書く。

**B4. incident を自動で記録する（5 章 4 の具体化）。**（一部実装: 090-1 / 090-2。診断は attempt の失敗レコードに自動記録される。`taskqueue_update` による別途 incident メモと自動 `memory_note` は未実装）
- `startup_hang` / `idle_stall` になったら、診断と `evidence` をジョブに incident として残す（`taskqueue_update` を人や LLM に頼らない）。
- 同じ `engine + model + likelyCause` の組み合わせで 2 回目が起きたら、`memory_note` を自動で作る（例: 「opencode を `--model` なしで起動すると初期化で止まる」）。次回 `taskqueue add` で同じ組み合わせが来たら警告に出す。

### 7.4 対策 C: 投入前に止める（lint / 事前チェック）

**C1. `taskqueue add` / `bt_validate` で警告を出す。**（未実装。091-4 で予定）
- opencode worker のコマンドに `--model` が無い。
- supervisor（MCP クライアント）と worker のエンジンが同じで、ロール指示（`instructionsText` など）が無い。
- worker ジョブなのに `idle` も `firstEventSec` も無効。

**C2. エンジンの健康診断（`worker probe`）。**（未実装。091-3 で予定）
- 大きなカードを流す前に、`engine + model` の組で「1 行返すだけ」の最小プロンプトを 60 s 制限で走らせ、結果を短時間キャッシュする。
- probe が失敗したら、本番ジョブは起動せず `startup_hang` 相当の診断付きで `blocked` にする。60 分を無駄にしない。

### 7.5 対策 D: supervisor が黙って引き取らないようにする（契約とツール出力）

LLM は自分のルールよりツールの返り値に強く従う。なので、指示は SKILL.md だけでなく**失敗時のツール出力そのもの**に入れる。

**D1. 失敗時の返り値に「委任を続ける」旨を入れる。**（一部実装: 090-1。`taskqueue why` / `taskqueue get` / triage に `supervisorNote` と `nextSteps` を表示。`bt_wait` への一文は未実装）
- `taskqueue_get` / `bt_wait` が `startup_hang` / `idle_stall` を返すときは、`nextSteps` に加えて次の 1 文を付ける:
  「このタスクを supervisor が直接実装する前に、`escalate` で判断を記録すること（`answer: takeover` と `rationale`）」。

**D2. 「引き取り（takeover）」を正式な選択肢にする。**（実装済み: 090-3。実装では BT の escalate ノードは `succeeded` のまま `takenOver: true` と `takeover` オブジェクトを記録し、ノード状態を `takenOver` にしていない。ジョブ側は `takenOver` 状態）
- 自分でやる判断自体は悪くない（今回も 718/719 は完成している）。問題は記録が残らないこと。
- BT の escalate の `expect` に `takeover` を標準で加える。`bt_resume(answer: "takeover", rationale: ...)` されたら、ノードを `succeeded(takenOver)` ではなく **`takenOver` 状態**にし、ダッシュボードと `taskqueue list` に表示する。
- 単発ジョブ用に `taskqueue takeover --id <id> --reason "..."` を用意する。ジョブは `cancelled` ではなく `takenOver` になり、誰が・なぜ引き取ったかが残る。
- 引き取った作業の終わりに `taskqueue complete --id <id> --report <path>` で結果（レポート、テスト結果）を戻せるようにし、後続の `dependsOn` もそのまま進むようにする。

**D3. SKILL.md に「ワーカーが失敗・ハングしたとき」の手順を書く。**（実装済み: 090-4）

```
1. 自分のシェルのタイムアウトで kill しない。Hancho の startup/idle 監視に任せる。
2. taskqueue why <id> を読み、likelyCauses と nextSteps を確認する。
3. nextSteps の順に試す: 同じ設定で 1 回 → 設定を変えて（モデル/エンジン変更は承認付き）→ escalate。
4. 自分で実装するなら、先に takeover（または escalate の answer: takeover）で理由を記録する。
5. 黙って委任をやめない。チャットでの報告は補足で、一次記録はジョブに残す。
```

**D4. 3.4 の手順をこれに合わせて更新する（実装済み: 090-4）。** 「自分でやる」は禁止ではなく、記録付きの最後の選択肢として位置づける。

### 7.6 優先順位

| 順 | 対策 | 効果 | 規模 | 状況 |
|---|---|---|---|---|
| 1 | B2: イベント 0 件のタイムアウトで `raise_timeout` を勧めない | 誤った助言を止める | 小（診断ルール 1 か所） | 実装済み（090-1） |
| 2 | B3 + D1: triage / 返り値に `nextSteps` と「黙って引き取らない」一文 | supervisor が行き止まらない | 小〜中 | 一部実装（090-1。D1 の `bt_wait` は未実装） |
| 3 | D3: SKILL.md に手順を書く | 次のセッションから効く | 小（文書のみ） | 実装済み（090-4） |
| 4 | A1 + A2: 初回イベント監視と worker の idle 既定値 | 60 分 → 3 分で検知 | 中 | 実装済み（090-2） |
| 5 | B1: `startup_hang` と `likelyCauses` / kill 前のプロセスツリー | 原因まで分かる | 中 | 実装済み（090-1 / 090-2） |
| 6 | D2: `takeover` 状態 | 引き取りが記録に残る | 中（状態の追加、UI） | 実装済み（090-3） |
| 7 | C1 / C2 / B4 / A3: lint、probe、自動 memory、途中警告 | 再発を減らす | 中〜大 | 未実装（B4 は診断の自動記録のみ 090-1 / 090-2） |

1〜6 は 090-1〜4 で実装済み（2 は一部実装）。7 は未実装で、C1 は 091-4、C2 は 091-3 で予定している。当初は「1〜3 は今の構造のまま入れられるので先にやる。4 以降は `TaskQueueIdle` と失敗クラスの拡張が要る」としていた。

## 8. 方針: supervisor が BT にハング対策を「事前に」書けるようにする

7 章は「ハングした後に supervisor が正しく動く」ための対策だった。基本方針はもう一歩進めて、**supervisor LLM が BT を書く時点で、ハングしたらどうするかをプログラムしておく**ことにする。そうすれば、実行中に supervisor が起きて判断する必要がなく、委任をやめる隙も生まれない。

そのために BT（とその周辺の API）に足りないものは次のとおり。

| 足りないもの | 今の BT でできること | 問題 |
|---|---|---|
| 失敗の**理由**で分岐する | `exitCode` / `nodeState`（成功か失敗か）だけ | ハング、認証エラー、レート制限、テスト失敗を区別できない。全部同じ fallback に落ちる |
| 事前承認したエンジン／モデルの切り替え | 不可（モデル変更には人の承認が要る） | 「opencode が止まったら agy で」を計画に書けない |
| 本番前の健康診断 | 不可 | 壊れたエンジンに 60 分のカードを投げてしまう（今回の opencode の件） |
| 計画のハング対策の抜けを指摘する | `bt validate` は構文だけ | worker に初回イベント監視が無い、`--model` が無い、などを書いた時点で教えてくれない |
| 書き方の手本 | 文書の例だけ | LLM が毎回ゼロから書くと抜けが出る |

### 8.1 失敗情報を BT から読めるようにする（実装済み: 091-1）

job / worker ノードが失敗したら、診断（090-1）を変数に入れる:

| 変数 | 値 |
|---|---|
| `${<node>.failureClass}` | `startup_hang` / `idle_stall` / `timeout` / `rate_limited` / `auth_error` / `exit_nonzero` など |
| `${<node>.failureSummary}` | 診断の summary |
| `${<node>.likelyCauses}` | 原因 id のカンマ区切り（例: `no_model,waiting_for_input`） |
| `${<node>.suggestedAction}` | `retry` / `retry_with_change` / `raise_timeout` など |
| `${<node>.retryAt}` | rate limit の再試行可能時刻 |
| `${<node>.engine}` / `${<node>.model}` | 実際に使ったエンジン／モデル |

新しい条件ノード `failure`:

```json
{ "type": "failure", "node": "impl", "class": ["startup_hang", "idle_stall"] }
{ "type": "failure", "node": "impl", "cause": "no_model" }
```

`escalate` の判断パケットには、`include` で指定したノードの診断（`failureClass` / `likelyCauses` / `nextSteps`）を自動で入れる。

### 8.2 失敗の種類ごとの処理を書く `catch` ノード（実装済み: 091-2）

```json
{ "type": "catch", "id": "impl_guard",
  "child": { "type": "worker", "id": "impl", "...": "..." },
  "handlers": [
    { "class": ["startup_hang", "idle_stall"], "do": { "...": "別エンジンで再実行など" } },
    { "class": ["rate_limited"], "do": { "type": "wait", "sec": 600 } , "thenRetry": true },
    { "class": ["auth_error"], "do": { "type": "escalate", "question": "..." } }
  ],
  "otherwise": "fail" }
```

- `child` が成功すれば `catch` も成功。失敗したら最初に一致した handler の `do` を実行する。`thenRetry: true` なら `do` の後に `child` をもう一度実行する（`maxRetries` で上限）。
- どれにも一致しなければ `otherwise`（`fail` 既定 / `escalate`）。
- `fallback` + `if` + `failure` 条件でも同じことは書けるが、LLM が書き間違えない短い形を用意する。

BT 全体の既定値 `defaults` も足す: `{"worker": {"idle": {"firstEventSec": 180, "outputSec": 600}, "maxAttempts": 1}}`。各ノードに毎回書かなくても、ハング監視が必ず付く。

### 8.3 事前承認したエンジンの切り替えと健康診断（実装済み: 091-3 / 091-6。権限連携は 091-7）

実装状況: `engines` / `switchEngineOn` / `probe` / LLM リストと承認経路 / `{engine}` / `{model}` / `engineSwitched` は 091-3。3 リストの実行時配線（consult の代理回答・supervisor の選択）は 091-6。LLM リスト未承認候補の警告と `needs` の権限申請は 091-7。ダッシュボードの LLM ページは Win32 設定ダイアログの 10 個目のタブ（ブラウザ WebUI ではない）。

worker ノードに **エンジン候補の列** を書けるようにする:

```json
{ "type": "worker", "id": "impl", "prompt": "...", "timeoutSec": 5400,
  "engines": [
    { "engine": "opencode", "model": "opencode-go/deepseek-v4.1-flash" },
    { "engine": "agy", "model": "gemini-3.8-flash-high" }
  ],
  "switchEngineOn": ["startup_hang", "auth_error", "probe_failed"] }
```

- 先頭から使い、`switchEngineOn` の失敗クラスで次の候補に進む。それ以外の失敗（テスト失敗など）では切り替えない。
- **使える LLM のリストを Hancho の設定に持つ（LLM リスト）。役割ごとに別々のリストにする。** リストに入っていてユーザーが承認した provider / モデルの間の切り替えは、1 回ごと・BT ごとの承認なしに Hancho が行ってよい。
  - リストは 3 つで、互いに独立して設定する（同じモデルを複数のリストに入れてもよい。並び順・有効/無効・承認もリストごと）:
    | リスト | 用途 |
    |---|---|
    | `worker` | 実作業に使う LLM（BT `worker` ノード、ww カード、`run_worker`） |
    | `consult` | 相談（判断）に使う LLM（escalate の代理回答 `fallback`、triage の `advise` など）。supervisor が応答しない間に BT の判断を代わりに出す |
    | `supervisor` | Hancho が起動する監督役（triage のバッチ supervisor、`triage handoff`、チャットの応答ハンドラなど）。落ちたら同じリストの次の候補で起動し直す |
  - 設定は `<data-dir>/llms.json`: `{"worker": [...], "consult": [...], "supervisor": [...], "history": [...]}`。各行は `{engine, provider, model, rank, enabled, note, status, approvedVia, approvedAt, userSaid}`。`engine` は起動方法（`claude` / `opencode` / `codex` / `agy` / `engines.json` の追加分）、`provider` はモデルの提供元（例: `opencode-go`、`anthropic`、`google`）、`rank` は切り替えの優先順、`enabled: false` で一時的に外す。
  - **LLM も設定できる**: CLI `hancho-cli llm list [--list <l>]` / `llm add --list <l> --engine <e> --model <m> [--provider <p>] [--rank <n>] --reason "..."` / `llm remove` / `llm enable|disable`、MCP `llm_list` / `llm_add` / `llm_remove` / `llm_set_enabled`。`--list`（`worker` / `consult` / `supervisor`）は必須。ダッシュボードの設定画面（LLM ページ、リストごとに分けて表示）でも編集できる。変更はすべて `history` に誰が（`human` / MCP クライアント名 / `cli`）・どのリストを・いつ・なぜを記録する。
  - **ユーザーの事前承認**: 各行は `status: proposed | approved` を持つ。LLM が `llm add` した行は `proposed` で、使わない。ユーザーが承認した行だけが `approved` になり、Hancho はその範囲でだけ自動で切り替える。承認の経路は 2 つで、どちらで承認したかを `approvedVia` に残す:
    - ダッシュボードの LLM ページで、提案中の行を確認して承認ボタンを押す（`approvedVia: dashboard`）。
    - supervisor がチャットでユーザーに確認し、ユーザーの返答を引用して `llm approve --list <l> --engine <e> --model <m> --user-said "<ユーザーの言葉>"` する（`approvedVia: chat`、引用を記録）。
  - 承認済みの行の `rank` 変更や無効化は承認なしでよい（安全側の変更）。モデルや provider を変える編集は `proposed` に戻る。
  - 提案中の行があることは、ダッシュボードの通知と `llm list` の出力で分かるようにする。
  - BT worker ノードの `engines` は `worker` リスト、escalate の `fallback` は `consult` リスト、Hancho が起動する supervisor は `supervisor` リストの、承認済みで有効な行だけを使う。該当リストに無い候補は `bt validate` でエラー（`llm add --list <l>` と承認を案内する）。`engines` を書かないジョブでも、ジョブのエンジン・モデルが `worker` リストに承認済みで入っていれば、Hancho は故障時（8.6）にリストの次の候補へ切り替えてよい（ジョブ側で `"engineSwitch": false` を書けば止められる）。
  - 既存の「モデルを勝手に変えない」ルール（`taskqueue update --approve-model-change`）は、`worker` リストに無いモデルへの変更に対してだけ残す。
  - 切り替えるには、ジョブのコマンドのどこがエンジン／モデルかを Hancho が知っている必要がある。BT `worker` ノードと `engine` 付きジョブはエンジンレジストリで組み立てるのでそのまま切り替えられる。ww-run のような任意コマンドは `{engine}` / `{model}` のプレースホルダで書く（例: `ww-run.cmd 090-1 -Engine {engine} -Model {model} ...` と `"engine": "opencode", "model": "..."`）。プレースホルダの無い任意コマンドは切り替えない。
  - 切り替えのたびに「どのリストのどの行（と、その承認）に基づいたか」を記録する。
- 切り替えたら `engineSwitched` イベントを出し、ダッシュボードとノードの状態に「どのエンジンからどれへ、なぜ」を残す。

新しい葉ノード `probe`:

```json
{ "type": "probe", "engine": "opencode", "model": "opencode-go/deepseek-v4.1-flash", "timeoutSec": 60 }
```

- 最小のプロンプトを送り、時間内に応答（最初のイベント＋正常終了）があれば成功。結果は `<data-dir>` に短時間（既定 10 分）キャッシュする。
- worker ノードに `"probe": true` を書くと、起動前に自動で probe する。失敗したら本番を起動せず、失敗クラス `probe_failed` で終わる（`switchEngineOn` に入れておけば次のエンジンへ）。
- CLI / MCP からも単体で使える: `hancho-cli worker probe --engine opencode --model ... [--json]`。

### 8.4 書いた時点で抜けを指摘する（実装済み: 091-4。permit 警告は 091-7）

実装状況: 8 種類の警告（`no_hang_watch` / `opencode_without_model` / `unguarded_worker` / `engines_without_switch` / `switch_without_engines` / `engine_not_permitted` / `unswitchable_command` / `escalate_without_takeover`）と `--strict`（MCP は `strict: true`）、`bt template` / `bt_template` は 091-4。設計では「エラー」と書かれている項目も、実装では警告（`warnings[]`）として返し、`--strict` でのみエラーになる。権限の `permit_not_approved` / `permit_invalid` は 091-7。テンプレートの `permission_denied` ハンドラと cwd 注意書きも 091-7 で追加済み。

`bt validate` に警告（エラーではない）を足す。`--strict` では警告もエラーにする。

- worker / LLM を起動する job に、初回イベント監視も idle 監視も無い（`defaults` も無い）。
- opencode を起動するのに `--model`（`model`）が無い。
- worker の失敗を受ける `catch` / `fallback` / `retry` が無い（ルートまで素通し）。
- `engines` が 1 つで `switchEngineOn` が書かれている、または `engines` が 2 つ以上なのに `switchEngineOn` が無い。
- `escalate` の `expect` に `takeover` が無い（7.5 D2）。

`bt template <name>` で手本を出す（`--json`）。最低限 `resilient-worker`（`catch` + `engines` + `probe` + `defaults` + escalate）と `card-sequence`（ww カードを順に流し、各カードをハング対策付きで包む）。MCP にも `bt_template` を出す。

### 8.6 エンジンの故障状態を共有し、Hancho の中だけで回復する（実装済み: 091-6）

2026-11-19 の opencode のハングは、LLM の**トークン（クォータ）切れ**だった。provider はエラーを返さず黙って待たせるので、Hancho から見えるのは「イベント 0 件」だけで、ネットワーク障害と区別できない。区別できなくても、次の仕組みがあれば supervisor を起こさずに回復できる。

| 段階 | 仕組み | 状態 |
|---|---|---|
| 検知 | 初回イベント監視で 3 分以内に `startup_hang` | 090-2 |
| 代わりのエンジンで続ける | `engines` + `switchEngineOn`（事前承認） | 091-3 |
| **他のジョブを巻き込まない** | エンジン（`engine + model`、可能なら provider 単位）を「故障中」にし、同じエンジンを使う待機中ジョブを起動せず保留する（失敗にしない） | 091-6 |
| **回復を確かめる** | 故障中のエンジンを間隔を空けて probe する（既定 5 分 → 10 分 → 20 分 … 上限 60 分）。トークンがリセットされれば probe が通る | 091-6 |
| **自動で再開する** | probe が通ったら「正常」に戻し、保留していたジョブを起動し、`switchEngineOn` で退避していた BT ノードは次の実行から元の候補に戻す | 091-6 |

- 代わりのエンジンが無い（または承認が無い）場合でも、「保留 → probe → 再開」だけで、トークンのリセット後に Hancho が自分で続きを流せる。supervisor の判断は要らない。
- クォータ切れを明示するエラー（`rate_limited` / `quota` / `429` / `insufficient` など）が出たときは、`retryAt` があればそれまで probe しない。
- 故障中のエンジンはダッシュボードと `hancho-cli worker health` に出し、`engineDown` / `engineRecovered` イベントを残す。手動で `worker health --reset <engine>` もできる。
- **supervisor も同じ制約を受ける。** supervisor LLM にも権限とトークンの上限があり、肝心なときに起きられないことがある。回復を「supervisor が判断してくれる」ことに頼らない。escalate の `fallback`（代理回答）は `consult` リスト（8.3）の承認済みの候補を順に使い、故障中のエンジンは飛ばす。Hancho が起動する supervisor も `supervisor` リストで同じように扱う。supervisor が応答しない間も、事前に BT に書いた方針（`catch` / `engines` / 保留と再開）だけで進めるのが原則。
- 使用量の記録から「このエンジンの直近の成功」と「故障を検知した時刻」を診断に入れ、`likelyCauses` に `quota_suspected`（直近まで正常に使えていて、突然すべてのジョブが 0 イベントになった）を出す。

### 8.8 ワーカーの権限: 許可の要る仕事を黙って渡さない／ユーザーが事前に OK を出せる（実装済み: 091-7。実プロセスでの live 確認・画面目視は未実施）

2026-11-19、opencode（`opencode run`）のワーカーが 2 回、プロジェクトの外のファイル（`%TEMP%` のテスト用ディレクトリ、`~/.config/opencode/AGENTS.md`）を読もうとして拒否され、その場でセッションが終わった。終了コードは 0 で、Hancho には「レポートが無い」とだけ見え、診断は `rate_limited` と誤判定された。

非対話のワーカーには「許可しますか？」に答える人がいない。**原則は「権限が足りないことでジョブを失敗させない」**。拒否は作業の途中で起きる小さな行き違いで、ジョブ全体を捨てる理由にはならない。対策は 3 つ入れる。

**(0) 拒否されてもワーカーを止めない・続きから再開する。**
- ワーカーの起動設定で、未承認の操作を「ask（確認待ち→非対話では拒否してセッション終了）」ではなく「deny（ツールのエラーとしてモデルに返し、モデルは別の方法で続ける）」にする。opencode なら起動時に渡す設定の `permission` で、Hancho が既定で入れる（ユーザーの全体設定は触らない）。`--auto` のような全許可は使わない。
- それでもセッションが拒否で終わったら、Hancho は失敗にせず、**同じセッションを続きから再開する**（opencode: `run --session <id>`、claude: `--resume <id>`）。再開時のプロンプトに「`<対象>` へのアクセスは許可されていない。プロジェクト内の方法で続けること（必要なら `needs` に書いて申請）」を入れる。上限（既定 2 回）までは自動、超えたら (2) の保留に回す。
- `success` 判定が失敗でも、拒否で終わったジョブは `failed` にしない（`permission_denied` → 再開 → 保留）。

**(1) 権限の拒否を正しく検知して記録する。**
- ストリームの tool エラー（opencode: `"The user rejected permission to use this specific tool call."`、claude: permission denied 系）を拾い、新しい失敗クラス `permission_denied` にする。`evidence` に拒否された tool・対象（パス、コマンド）・エンジンを入れる。`rate_limited` などのログ文字列ルールより優先する。
- `nextSteps` は「(a) 指示を直してプロジェクト内で済ませる」「(b) 権限を申請する（下記）」「(c) escalate」。

**(2) 権限リスト（permits）を持ち、ユーザーの事前承認で広げる。** 8.3 の LLM リストと同じ形にする。
- `<data-dir>/permits.json`: `{"permits": [...], "history": [...]}`。各行は `{kind, target, engines, scope, status, approvedVia, approvedAt, userSaid, note}`。
  - `kind`: `read_path` / `write_path` / `command` / `network` など。`target`: パスの glob（例: `%TEMP%/pc_*/**`、`~/.config/opencode/AGENTS.md`）やコマンドのパターン。`engines`: 対象エンジン（省略で全部）。`scope`: `global` / `taskqueue:<名前>` / `bt:<id>`。
- **LLM も申請できる**（`hancho-cli permit request --kind ... --target ... --reason "..."`、MCP `permit_request`）。申請は `proposed` で、使わない。ユーザーがダッシュボードの承認ボタンか、チャットで確認したうえでの `permit approve --user-said "<ユーザーの言葉>"` で `approved` にする。`permit list` / `permit revoke` も用意し、変更はすべて `history` に残す。
- **起動時に承認済みの権限をエンジンの設定に変換して渡す。** Hancho が持つのはエンジン共通の表現で、エンジンごとの変換はエンジンレジストリ（`engines.json`）に置く（例: opencode は起動時の環境変数で渡す設定の `permission`、claude は `--add-dir` / `--allowedTools`）。ユーザーの全体設定（`~/.config/opencode/opencode.jsonc` など）は書き換えない。
- **ジョブ側で必要な権限を宣言できる**（`"needs": [{"kind": "read_path", "target": "..."}]`）。`taskqueue add` / `bt validate` で、宣言された権限が承認済みでなければ警告（`--strict` でエラー）し、`permit request` を案内する。
- **実行中に拒否されたら、失敗にせず保留にできる。** `permission_denied` で終わったジョブは、拒否された対象を `permit request` として自動で申請し、ジョブを `waitingForPermit` で止める（BT なら `catch` の `permission_denied` handler で書ける）。ユーザーが承認したら同じジョブを自動で再実行する。却下されたら `failed` にし、`nextSteps` の (a) を勧める。
- **既定は狭く。** worker の既定権限は「`cwd`（ジョブの作業ディレクトリ）の中だけ」。カードの指示にも「プロジェクトの外を触らない」を標準で入れる（ww-run のプロンプト、`bt template`）。

### 8.7 文書（このカード 091-5 で実施済み）

`doc/obsoleted/behavior-tree.md` / `-ja.md` に上の機能、`skills/hancho/SKILL.md` に「BT を書くときはハング対策を最初から入れる」節と `resilient-worker` の例を書く。あわせて `doc/obsoleted/cli-help.md` / `-ja.md`、`doc/obsoleted/mcp.md` / `-ja.md` に CLI / MCP の対応コマンド・引数を追記し、`ww-data/reports/091-5.md` に結果を記録する。

## 9. 8 章の実装状況まとめ

| 項目 | カード | 状況 |
|---|---|---|
| 8.1 失敗変数・`failure` 条件・escalate 診断 | 091-1 | 実装済み |
| 8.2 `catch` / `defaults` | 091-2 | 実装済み |
| 8.3 `engines` / `switchEngineOn` / `probe` / LLM リスト | 091-3 | 実装済み |
| 8.3 consult / supervisor の実行時配線 | 091-6 | 実装済み（画面操作の目視確認は未実施） |
| 8.4 `bt validate` 警告 / `--strict` / `bt template` | 091-4 | 実装済み |
| 8.4 `permit_not_approved` / `permit_invalid` 警告 | 091-7 | 実装済み |
| 8.6 エンジン故障状態の共有・probe・自動回復 | 091-6 | 実装済み |
| 8.8 `permission_denied` / permits / `needs` / `waitingForPermit` | 091-7 | 実装済み（live 確認・画面目視は未実施） |
| 8.7 文書化 | 091-5 | 実施済み |
