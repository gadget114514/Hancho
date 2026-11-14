# Audit LLM 設計書

この文書は [audit-llm-ja.md](audit-llm-ja.md)（コンセプト）を実装できる粒度まで具体化した設計書です。
役割分担（Human / Supervisor / Worker / Audit）、5 大監査項目、2 つのフィードバック経路という骨格はコンセプト文書のまま引き継ぎ、ここでは「何がどこで計算され、どの形式で、いつ誰に届くか」を定めます。

- 対象バージョン: v0.9.6 時点の実装を出発点とし、v0.10 以降で段階的に実装する
- 関連文書: [audit-llm-ja.md](audit-llm-ja.md), [design-v3.md](design-v3.md)（4.2 失敗分類 / 4.3 診断）, [resource-locks-ja.md](resource-locks-ja.md), [behavior-tree-ja.md](behavior-tree-ja.md), [mcp-ja.md](mcp-ja.md)

---

## 1. 目的と非目的

### 目的
1. Supervisor LLM のオーケストレーション行動（見積もり、リトライ、タスク分解、ログの読み方、並列化）を、Hancho が持つテレメトリから**客観的に評価**する。
2. 評価結果を **人間（ループ A）** と **Supervisor 自身（ループ B）** の両方に、行動に移せる形（具体的な数値・コマンド）で返す。
3. 監査が無効でも、LLM が使えなくても、Hancho の実行機能には一切影響しない。

### 非目的
- Audit LLM はコードを書かない・ジョブを起動しない・ジョブを止めない。**勧告のみ**を出す。
- Worker の出力内容（コードの正しさ）の品質レビューは扱わない（それは Supervisor かテストの責務）。
- Supervisor の会話ログ本体は読まない。Hancho が観測できるのは MCP / IPC 経由の呼び出しとジョブのテレメトリだけであり、監査もその範囲に限る。

---

## 2. 現状（v0.9.6）の実装と差分

コンセプト文書には未実装の項目も含まれているため、まず現状を整理する。

| 項目 | コンセプト文書の記述 | v0.9.6 の実装 | 状態 |
|---|---|---|---|
| ① 見積もり乖離 | duration / estimate を算出し 2 倍以上・1/3 以下で警告 | `TaskQueueManager::JobToJson` が `timing_feedback`（`duration_sec`, `estimate_sec`, `ratio`, `accuracy`, `advice`）を付与（[taskqueue.cpp](../src/taskqueue.cpp) `TimingFeedbackToJson`）。閾値は 2.0 / **0.35** の固定値 | 部分実装（決定的ルールのみ） |
| ② 無修正リトライ | ワークツリー変更なしの再試行を検知 | なし | 未実装 |
| ③ コマンド設計・粒度 | ChainGuard 拒否履歴、デッドロック監査 | なし | 未実装 |
| ④ コンテキスト衛生 | 生ログ読み込みの監査 | なし | 未実装 |
| ⑤ リソース競合 | `blocked_by` 多発の監査 | ヒント窓が `resource_wait` を見る分岐はあるが、`taskqueue list` の応答に `resource_wait` は含まれない（`taskqueue graph` のみ） | 実質未動作 |
| LLM による分析 | 軽量 LLM がテレメトリを分析 | LLM 呼び出しなし。ヒントはすべてルールベース | 未実装 |
| ループ A（Hint 窓） | 右下ポップアップ、コピー、自動消去 | 実装済み（[gui_dashboard.cpp](../src/gui_dashboard.cpp) `AnalyzeStateForHint` / `EvaluateSupervisorHint`）。Supervisor 起動ボタン付き | 実装済み（不具合あり、2.1 参照） |
| ループ B（`"audit"` 注入） | `taskqueue_wait` / `taskqueue_get` / `bt_wait` に `"audit"` | `timing_feedback` はジョブ JSON に入るので間接的に届く。`"audit"` オブジェクトと `get_audit_feedback` ツールはなし | 部分実装 |
| 設定 | `audit_llm_enabled` ほか | `show_supervisor_hints`, `hint_auto_dismiss_sec`, `supervisor_command` のみ読み書き。`audit_llm_*`, `audit_timing_divergence_threshold` は未使用 | 部分実装 |

### 2.1 調査で見つかった既存実装の不具合

本設計の実装時にあわせて直す。

> **P0 対応状況（2026-11-13、未リリース）**: 1〜6 はすべて修正済み。
> - 4・5: `timing_feedback` は成功試行（`reason == "ok"`）だけを対象にし、5 秒未満は `diverged_under` にしない。閾値は `config.json` の `audit_timing_divergence_threshold`（既定 2.0）と `audit_timing_under_threshold`（既定 0.33）から読む。
> - 3: `taskqueue list` の各ジョブにも `resource_wait` を出す。
> - 2: BT 待ちの判定は escalate ノードの状態で行う。
> - 1: ヒントのコマンドを実在するもの（`taskqueue why` / `retry`、`bt get --id`、`bt resume`）に直した。
> - 6: 表示済みのヒントは飛ばして次を出す。表示中は差し替えず、条件が消えたものは忘れる。
>
> 画面上でのヒント窓の動作はまだ確認していない。

1. **ヒントのコピー用プロンプトに存在しない CLI コマンドが入っている。**
   - `hancho-cli taskqueue inspect <id>` → 実在するのは `taskqueue why <id>`
   - `hancho-cli taskqueue requeue <id>` → 実在するのは `taskqueue retry <id>`
   - `hancho-cli bt get <id>` → 正しくは `bt get --id <id>`
   - `hancho-cli bt answer <id> --node <n> --result ...` → 正しくは `bt resume --id <id> --node <n> --answer <a> --rationale <why>`
2. **BT エスカレーション検出の状態名が実装と一致しない。** ヒント側は `waiting_for_llm` / `waiting_for_decision` / `escalated` を探すが、BT 実装の状態名は `waitingForLlm`（camelCase）。ノード単位の状態名も含めて照合し直す必要がある。
3. **リソース競合ヒントが発火しない。** 上表 ⑤ のとおり、`lastTaskListJson` に `resource_wait` がない。
4. **失敗した試行でも見積もり乖離を判定している。** `TimingFeedbackToJson` は最後に終了した試行を使うため、数秒で落ちた失敗試行が `diverged_under` と判定され、Supervisor に「見積もりを縮めよ」という誤った助言が届く。成功した試行だけを対象にすべき。
5. **閾値の不一致。** コンセプト文書は「1/3 以下」、コードは 0.35、設定キー `audit_timing_divergence_threshold` は読まれていない。
6. **乖離ヒントが 1 件目で止まる。** ヒントは最初に見つかった乖離ジョブで return し、同じシグネチャは再表示しないため、そのジョブが残っている限り 2 件目以降の乖離が表示されない。

---

## 3. 全体アーキテクチャ

監査を **2 段構成** にする。判定そのものは決定的なルールで行い、LLM は「束ねて言語化する」役に限定する。

```
 TaskQueueManager / BtManager / ResourceLock / MCP server
        │  (イベント: job_finished, job_retried, chain_rejected,
        │   lock_waited, tool_called ...)
        ▼
 ┌──────────────────────────────────────────────┐
 │ AuditManager（新設, src/audit.cpp/.h）        │
 │  ┌──────────────────────────┐                │
 │  │ Stage 1: Rule Engine      │ 決定的・即時   │
 │  │  ①〜⑤ の検出ルール        │ LLM 不要       │
 │  └────────────┬─────────────┘                │
 │               │ AuditFinding（構造化）         │
 │  ┌────────────▼─────────────┐                │
 │  │ Finding Store             │ 重複排除・既読 │
 │  │ logs/audit/<queue>.jsonl  │ 永続化         │
 │  └────────────┬─────────────┘                │
 │               │ 未処理 finding が閾値を超えたら │
 │  ┌────────────▼─────────────┐                │
 │  │ Stage 2: Audit LLM（任意）│ 非同期・低頻度 │
 │  │  要約 + Directive 生成     │                │
 │  └────────────┬─────────────┘                │
 └───────────────┼──────────────────────────────┘
        ┌────────┴──────────┬───────────────────┐
        ▼                   ▼                   ▼
  ループ A: GUI Hint 窓   ループ B: MCP 応答の   CLI / WebUI
  / WebUI 通知            "audit" フィールド、   hancho-cli audit ...
                          audit_get ツール
```

### 設計判断
- **判定は Stage 1 で完結させる。** Audit LLM が無効・未インストール・失敗しても、finding はルールだけで出る。LLM の出力が「警告すべきか」を決めることはない。これにより誤検知の再現性・テスト容易性を確保し、LLM のコストを「言語化」にだけ払う。
- **Stage 2 は非同期・バッチ。** ジョブ完了の都度 LLM を呼ぶとコストと待ち時間が見合わない。未処理の finding がたまったとき（または Supervisor が明示的に要求したとき）だけ呼ぶ。
- **AuditManager はコア側（IPC サーバープロセス）に置く。** GUI だけでなく CLI・MCP・WebUI・ヘッドレス実行のどこからでも同じ finding が見えるようにするため。現在 GUI 内にある `AnalyzeStateForHint` のロジックは AuditManager に移し、GUI は finding を表示するだけにする。

---

## 4. データモデル

### 4.1 AuditFinding

```jsonc
{
  "id": "aud-20261112-0007",          // 一意 ID（日付 + 連番）
  "kind": "timing_divergence",          // 下記 kind 一覧
  "severity": "warn",                   // info | warn | critical
  "taskqueue": "build-fix",
  "job_ids": ["build-3"],               // 関係するジョブ（複数可）
  "bt_id": null,                        // BT 由来なら BT ID
  "signature": "timing:build-3:over",   // 重複排除キー
  "created_at": "2026-11-12 15:20:31",
  "evidence": {                         // kind ごとの根拠データ（数値のみ。ログ本文は入れない）
    "duration_sec": 142, "estimate_sec": 30, "ratio": 4.73
  },
  "advice": "Actual duration (142s) exceeded estimate (30s) by 4.7x. ...", // ルールが作る定型文（英語）
  "suggested_actions": [                // そのまま実行できる是正操作
    {"type": "update_estimate", "job_ids": ["build-4", "build-5"], "estimate_sec": 150, "timeout_sec": 300}
  ],
  "directive": null,                    // Stage 2 が書き込む自然文（後述）
  "state": "open",                      // open | delivered | acknowledged | resolved | suppressed
  "delivered_to": ["mcp"]               // 届いた経路（mcp, gui, webui, cli）
}
```

### 4.2 kind 一覧

| kind | 監査項目 | severity の既定 |
|---|---|---|
| `timing_divergence` | ① | warn（比率 4 倍以上で critical） |
| `timeout_risk` | ① | warn |
| `unmodified_retry` | ② | warn（同一 job で 2 回目以降 critical） |
| `retry_storm` | ② | critical |
| `chain_rejected` | ③ | info |
| `granularity_too_fine` | ③ | info |
| `granularity_too_coarse` | ③ | info |
| `dependency_stall` | ③ | warn |
| `raw_log_heavy` | ④ | info |
| `lock_contention` | ⑤ | warn |
| `bt_escalation_pending` | （運用） | warn |

`bt_escalation_pending`・ジョブ失敗・全完了・アイドルといった現在のヒント窓の「状態通知」は監査ではなく運用通知なので、finding とは別系統（GUI のヒント窓が直接表示する運用ヒント）として残す。finding にするのは `bt_escalation_pending` のように Supervisor が放置している状態だけとする（10 分以上応答がない場合など）。

### 4.3 永続化

- `logs/audit/<taskqueue>.jsonl` に 1 行 1 finding で追記する。状態変更は同じ `id` の行を追記し、読み込み時に最後の行を採用する（taskqueue の他のログと同じく追記型で、壊れにくくする）。
- BT 由来でキューに属さないものは `logs/audit/_bt.jsonl`。
- 保持期間は `taskqueue purge --older-than-days` に連動して古い finding も削除する。
- `hancho-cli reset` の削除対象に `logs/audit/` を含める。

### 4.4 試行（attempt）に追加するテレメトリ

② の判定に必要な情報を `TaskQueueAttempt` に追加する。

| フィールド | 型 | 内容 |
|---|---|---|
| `workspace_fp` | string | 試行開始時のワークスペース指紋（5.2 参照）。取得できなければ空 |
| `retry_origin` | string | `auto`（max_attempts による自動再試行）/ `manual`（`taskqueue_retry` 呼び出し）/ `bt`（BT の retry ノード） |

既存の `instructionsRev`、`exitCode`、診断の `failureClass` と組み合わせて使う。

---

## 5. 検出ルール（Stage 1）

すべてのルールは「どのイベントで評価するか」「入力」「条件」「出力」で定義する。閾値は 9 章の設定で変更できる。

### 5.1 ① 見積もり乖離

**評価タイミング**: ジョブが `succeeded` になったとき。

**入力**: 成功した最後の試行の `start` / `end`、`estimate_sec`、`timeout_sec`。

**条件**:
- `estimate_sec > 0` のジョブだけを対象にする（0 は「見積もりなし」）。
- **成功した試行だけ**で比率 `ratio = duration / estimate` を求める（2.1 の 4 を修正）。
- `ratio >= over_threshold`（既定 2.0）→ `timing_divergence`（over）
- `ratio <= under_threshold`（既定 0.33）かつ `duration >= 5 秒` → `timing_divergence`（under）。極端に短いジョブは計測誤差が大きいため除外する。
- `timeout_sec > 0` かつ `duration >= timeout_sec * 0.8` → `timeout_risk`。

**補正値の算出**（`suggested_actions` の中身）:
- 同じキュー内で「同種」のジョブを集め、実測の中央値を求める。同種の判定は、①`command` の先頭トークン（実行ファイル名）が同じ、②`name` から末尾の数字・連番を除いた接頭辞が同じ、のいずれか。
- 推奨 `estimate_sec = ceil(中央値 × 1.1)`、推奨 `timeout_sec = max(現在値, 推奨 estimate × 2)`。
- 対象は同種で、まだ開始していない（`pending` / `waiting`）ジョブ。対象がなければ `suggested_actions` は空にし、`advice` だけを出す（今後の見積もりへの教訓）。

**既存 `timing_feedback` との関係**: ジョブ JSON の `timing_feedback` は互換のため残す（成功試行のみに直したうえで）。finding はそれを集約・補正値付きにしたもの。

### 5.2 ② 無修正リトライ（スラッシング）

**評価タイミング**: ジョブの新しい試行が始まるとき（`retry_origin` が `manual` または `bt` のとき。`auto` は Supervisor の判断ではないので対象外）。

**ワークスペース指紋 `workspace_fp`**:
- ジョブの `cwd` が git 作業ツリー内なら、`git rev-parse HEAD` と `git status --porcelain=v1 -z` の出力、および変更ファイルの mtime/サイズを連結したもののハッシュ（SHA-1 で十分）。
- git でない、または git 実行に失敗した場合は空文字（判定不能 → ルールを適用しない）。誤検知より見逃しを選ぶ。
- 取得は試行開始時に 1 回、タイムアウト 2 秒。大規模リポジトリで遅い場合に備え、`audit_workspace_fingerprint: false` で無効化できる。

**条件**（すべて満たすとき `unmodified_retry`）:
1. 直前の試行が失敗している。
2. 直前の試行と今回の試行で `workspace_fp` が同一（どちらも非空）。
3. `instructionsRev` が変わっていない（Worker LLM への指示を書き換えた場合は「修正あり」とみなす）。
4. ジョブの `command` / `env` が変わっていない。

さらに、直前の失敗の `failureClass` が「一時的」に分類されるもの（ネットワーク、タイムアウトなど design-v3 4.2 の分類で再試行が妥当なもの）であれば severity を `info` に下げる。

**`retry_storm`**: 同じキューで 10 分以内に 5 回以上の `manual` リトライがあり、その間キュー全体で成功が 0 件のとき。

**出力例**:
```
advice: "Job 'test-2' was retried (attempt 3) with no workspace change since attempt 2,
         which failed with class 'compile_error'. Fix the cause before retrying.
         Run `hancho-cli taskqueue why test-2` for the diagnosis."
suggested_actions: [{"type": "inspect", "command": "hancho-cli taskqueue why test-2"}]
```

**ブロックはしない。** リトライを拒否する機能（ChainGuard のような強制）は入れない。誤検知時にユーザーの作業を止めてしまうため。将来必要になれば `audit_block_unmodified_retry` として別途検討する（12 章）。

### 5.3 ③ コマンド設計・タスク粒度

| kind | 評価タイミング | 条件 |
|---|---|---|
| `chain_rejected` | ChainGuard が `taskqueue_add` / `start` を拒否したとき | 同一セッション（同一 MCP 接続）で 3 回目以降の拒否。1〜2 回目はエラー応答そのものが十分なフィードバックなので finding にしない |
| `granularity_too_fine` | キュー内のジョブがすべて終わったとき | ジョブ数 ≥ 20 かつ成功ジョブの実行時間の中央値 < 3 秒 |
| `granularity_too_coarse` | ジョブ完了時 | `duration` ≥ 30 分かつ Worker ジョブ（BT の worker ノード、または engine 指定あり） |
| `dependency_stall` | 定期（1 分ごと） | 実行中ジョブ 0 件、`pending` ジョブあり、かつ全 `pending` の依存先に `failed` / `cancelled` が含まれる状態が 5 分続く |

`dependency_stall` は真のデッドロック（依存の循環）ではなく「失敗した依存のせいで誰も進めない」状態。循環は `taskqueue_add` 時の検証で既に拒否されるため、監査では扱わない。

### 5.4 ④ コンテキスト衛生

Hancho から見えるのは MCP / IPC のツール呼び出しだけなので、**ツール呼び出しパターンから推定**する。

- MCP サーバーでツールごとの応答バイト数を記録する（既に `MCP_TOOL_RESP ... bytes=` を監査ログに出しているので、それを AuditManager にも渡す）。
- 条件: 1 つのジョブの失敗後、Supervisor が `taskqueue_diagnose` / `taskqueue_why` を一度も呼ばずに、そのジョブのプロセス出力取得（`get_process_output` 系）で合計 200 KB 以上を取得した → `raw_log_heavy`（info）。
- `advice` は「`taskqueue why <id>` と末尾行で原因が分かる場合が多い」という定型文。

CLI 経由（`hancho-cli` を Bash で叩く Supervisor）の場合は IPC の `action` で同じ判定をする。判定できない経路は対象外。

### 5.5 ⑤ リソース競合

**評価タイミング**: リソースロックの待機が解除されたとき、および 1 分ごと。

**条件**:
- 1 つのジョブがロック待ちで 60 秒以上待った（`lock_wait_sec >= 60`）、または
- 同じリソースで同時に 3 ジョブ以上が待機している。

**出力**: 待っていたリソース名、保持していたジョブ、待ち時間合計。`suggested_actions` には「依存関係 `depends_on` を張って直列化する」か「ロックを `read` に下げられないか検討する」を入れる。

これには ResourceLock 側で待機の開始・終了時刻を記録する必要がある（現在 `taskqueue graph` が出している `resource_wait` の元データを流用する）。

### 5.6 重複排除と抑制

- `signature` が同じ open/delivered な finding があれば新規作成せず `evidence` を更新するだけにする。
- ユーザーまたは Supervisor が `acknowledged` にした signature は、同じキューでは 1 時間は再発行しない。
- キュー単位で finding は 1 時間あたり最大 20 件（超過分は 1 件の `info` にまとめる）。監査がノイズ源になるのを防ぐ。
- ジョブが後で成功した場合、そのジョブに関する `unmodified_retry` は `resolved` に自動遷移する。

---

## 6. Audit LLM（Stage 2）

### 6.1 役割

Stage 1 の finding を束ねて、Supervisor が 1 回読めば行動できる **Directive（是正指示文）** を書く。具体的には次の 3 つだけを行う。

1. 複数の finding の関連づけ（例: 「見積もり超過」と「タイムアウト接近」と「同種ジョブの未開始」を 1 つの指示にまとめる）。
2. 優先順位付け（どれから直すべきか）。
3. 人間向け（ループ A）と Supervisor 向け（ループ B）の 2 種類の文面生成。

判定・数値の算出はしない。数値は Stage 1 の `evidence` / `suggested_actions` をそのまま引用させる。

### 6.2 呼び出し条件

次のいずれかで 1 回呼ぶ。同時に 2 つは走らせない。

- 同じキューに `warn` 以上の未処理 finding が 3 件以上たまった。
- `critical` の finding が出た。
- Supervisor が `audit_get` を `with_directive: true` で呼んだ。
- ユーザーが GUI / CLI から明示的に要求した（`hancho-cli audit summarize <queue>`）。

さらに、同じキューへの呼び出しは最短 5 分間隔（`audit_llm_min_interval_sec`）。1 日の呼び出し回数上限 `audit_llm_daily_limit`（既定 50）を超えたら Stage 2 を止め、Stage 1 の `advice` だけで配信する。

### 6.3 実行方法

- Worker LLM と同じ仕組み（`engines.json` のエンジン定義と `BtManager` の worker コマンド生成）を使い、`audit_llm_engine` / `audit_llm_model` で指定したエンジンを **1 回限りの非対話プロセス**として起動する。プロセスは taskqueue のジョブとしては登録せず、プロセス一覧に `audit-llm` という名前で表示する（ユーザーが見て止められるように）。
- 入力は一時ファイル（`logs/audit/tmp/<id>.json`）に書いたテレメトリパケットを標準入力で渡す。
- 出力は JSON（6.5）で受け取る。パースできなければ 1 回だけ再試行し、それでも失敗したら Stage 2 を諦める。
- タイムアウト 120 秒。
- Audit LLM プロセスには**ツール実行・ファイル編集の権限を与えない**。エンジンがその種のフラグを持つ場合は最も制限された設定で起動する（例: `claude -p` なら許可ツールなし）。エンジン定義側に `audit_args` を設けて指定する。

### 6.4 入力（テレメトリパケット）

```jsonc
{
  "taskqueue": "build-fix",
  "queue_summary": {"total": 12, "succeeded": 7, "failed": 2, "running": 1, "pending": 2},
  "findings": [ /* open な AuditFinding の配列（evidence / advice / suggested_actions を含む） */ ],
  "jobs": [ /* finding に関係するジョブだけ。id, name, state, estimate_sec, timeout_sec,
               attempts[].{start, end, exitCode, failureClass, retry_origin} のみ */ ],
  "language": "ja"   // GUI の UI 言語。人間向け文面に使う
}
```

**入れないもの**: ジョブの `instructions` 本文、プロセス出力・ログ本文、環境変数の値、ファイル内容。理由は (1) トークンコストの抑制、(2) ログ経由のプロンプトインジェクションの遮断、(3) 秘密情報の外部送信の防止。失敗分類（`failureClass`）と数値だけで Directive は書ける。

### 6.5 出力スキーマ

```jsonc
{
  "directive_for_supervisor": "string, English, <= 600 chars, imperative",
  "summary_for_human": "string, in `language`, <= 200 chars",
  "priority_finding_ids": ["aud-...", "..."],
  "covered_finding_ids": ["aud-...", "..."]
}
```

Hancho 側で検証する。

- `priority_finding_ids` / `covered_finding_ids` が入力にない ID を含んでいたら捨てる。
- 文字数超過は切り詰める。
- `directive_for_supervisor` に含まれる `hancho-cli` コマンドは、既知のサブコマンド一覧に照合し、存在しないものを含む場合は Directive 全体を捨てて Stage 1 の `advice` にフォールバックする（2.1 の 1 のような誤ったコマンドを Supervisor に渡さないため）。

採用した Directive は `covered_finding_ids` の各 finding の `directive` に書き込む。

### 6.6 プロンプト（骨子）

```
You are an auditor of a supervisor LLM that orchestrates jobs through Hancho.
You do not run commands. You only write advice.
The input is telemetry and findings that Hancho already computed with fixed rules.
Do not invent numbers: quote only values that appear in `evidence` or `suggested_actions`.
Only mention hancho-cli commands listed below: <許可コマンド一覧>
Return JSON matching this schema: <6.5 のスキーマ>
```

プロンプトはバイナリに埋め込み、`logs/audit/prompt.txt` があればそちらを優先する（調整用）。

---

## 7. 配信経路

### 7.1 ループ B: Supervisor への注入（MCP / IPC）

**`"audit"` フィールド**: 次のツールの応答に、そのキュー（BT なら BT）の未配信 finding を添付する。

- `taskqueue_wait`, `taskqueue_get`, `taskqueue_list`, `bt_wait`, `bt_get`

```jsonc
"audit": {
  "count": 2,
  "directive": "Jobs build-4 and build-5 share the build step that took 142s vs 30s estimated. ...",
  "findings": [
    {"id": "aud-...", "kind": "timing_divergence", "severity": "warn",
     "job_ids": ["build-3"], "advice": "...", "suggested_actions": [...]}
  ],
  "more": "hancho-cli audit list build-fix"
}
```

- 添付は 1 応答あたり最大 3 件（severity 順）。残りは `count` と `more` で示す。応答肥大化で ④ のコンテキスト衛生を自ら悪化させないため。
- 添付した finding は `delivered`（`delivered_to` に `mcp`）にし、同じ finding は二度添付しない。
- `audit` フィールドは未配信 finding がないときは**付けない**（毎回空オブジェクトを付けるとトークンの無駄）。
- 実装場所は `mcp_server.cpp` の `AttachTaskQueueDiagnosis` と同じ後処理段（`AttachAuditFeedback` を追加）。IPC（CLI）の `taskqueue_wait` 等には `--audit` 指定時のみ付ける（スクリプトから使う CLI の出力形式を変えないため）。

**新規 MCP ツール**:

| ツール | 引数 | 内容 |
|---|---|---|
| `audit_get` | `taskqueue?`, `states?`（既定 open,delivered）, `with_directive?` | finding の一覧。`with_directive: true` なら必要に応じて Stage 2 を同期実行（最大 120 秒） |
| `audit_ack` | `ids[]`, `resolution?`（`acknowledged` / `resolved` / `suppressed`）, `note?` | Supervisor が対応した・無視すると明示する |

コンセプト文書の `get_audit_feedback` は `audit_get` に名称を揃える（`taskqueue_*` / `bt_*` と同じ `名詞_動詞` 形式）。

### 7.2 ループ A: 人間への通知（GUI / WebUI）

- **GUI Hint 窓**: 既存の窓を流用する。表示ソースを「AuditManager の finding（優先）」＋「運用ヒント（BT 待ち・失敗・全完了）」の 2 系統にする。finding 表示時は:
  - カテゴリ: kind に応じたラベル（例「⏱ 見積もり乖離 (Audit)」）
  - 要約: `summary_for_human`（なければ `advice` を UI 言語に合わせた定型文）
  - コピー用プロンプト: `directive_for_supervisor`（なければ `advice` + `suggested_actions` から組み立てた文）
  - ボタン追加: **「既読にする」**（`audit_ack` 相当）。閉じるだけでは既読にしない。
- 未読 finding が複数ある場合は「1 / 3」のようにページ送りできるようにする（2.1 の 6 を解消）。
- **ダッシュボード上の常設表示**: タスク一覧に「Audit」列（未読件数のバッジ）を追加し、Hint 窓を閉じても後から確認できるようにする。
- **WebUI**: 既存の通知領域に finding を出す。WS のイベントとして `audit_finding` を流す。
- **TUI（`hancho-cli top`）**: ステータス行に未読件数を出すだけにする。

### 7.3 CLI

```
hancho-cli audit list [<queue>] [--all] [--json]
hancho-cli audit show <id>
hancho-cli audit ack <id>... [--resolve | --suppress] [--note <text>]
hancho-cli audit summarize <queue>      # Stage 2 を明示実行
hancho-cli audit purge [--older-than-days N]
```

`hancho-cli help --json` のコマンド一覧と、Supervisor 向けガイド（`cli.cpp` の workflow 説明）に `audit` を追記する。

---

## 8. Supervisor 側の想定フロー

```
taskqueue_add (estimate_sec=30 ×5)
   │
taskqueue_wait ──► 応答: build-1 succeeded, "audit": {timing_divergence ...
   │                      suggested_actions: update_estimate build-2..5 → 150 / 300}
   │
taskqueue_update build-2..5 (estimate_sec=150, timeout_sec=300)
audit_ack [aud-...] resolution=resolved
   │
taskqueue_wait ──► 応答: test-2 failed
taskqueue_retry test-2      ← コードを直さずに再試行
taskqueue_wait ──► 応答: test-2 failed, "audit": {unmodified_retry, critical,
                          advice: "...run `hancho-cli taskqueue why test-2`"}
   │
taskqueue_why test-2 → 修正 → taskqueue_retry test-2
```

Supervisor 向けガイド（[mcp-ja.md](mcp-ja.md) と `hancho-cli guide`）に「応答に `audit` があれば読み、対応したら `audit_ack` する」の一文を加える。

---

## 9. 設定

`logs/config.json`（GUI の設定ダイアログからも変更可能）。コンセプト文書のキーを引き継ぎ、追加する。

```jsonc
{
  // 全体
  "audit_enabled": true,                     // Stage 1 を含む監査全体。false なら finding を作らない
  "audit_llm_enabled": false,                // Stage 2。既定は無効（コストが発生するため明示的に有効化）
  "audit_llm_engine": "agy",
  "audit_llm_model": "gemini-3.8-flash-medium",
  "audit_llm_min_interval_sec": 300,
  "audit_llm_daily_limit": 50,

  // ルール閾値
  "audit_timing_divergence_threshold": 2.0,  // over 側（コンセプト文書のキー）
  "audit_timing_under_threshold": 0.33,
  "audit_lock_wait_sec": 60,
  "audit_workspace_fingerprint": true,
  "audit_disabled_kinds": [],                // 例: ["granularity_too_fine"]

  // 配信
  "audit_inject_mcp": true,                  // ループ B
  "audit_inject_max": 3,
  "show_supervisor_hints": true,             // ループ A（既存）
  "hint_auto_dismiss_sec": 7,                // 既存
  "triage_supervisor_engine": ""             // 「解決させる」のバッチ Supervisor のエンジン（旧 supervisor_command は廃止。詳細は triage.md）
}
```

- コンセプト文書の `audit_llm_enabled: true` の例は、既定値ではなく有効化例であることを明記する。
- 環境変数 `HANCHO_AUDIT=0` で一時的に監査全体を無効にできる（CI・テスト用）。
- 設定は IPC サーバー（コア）側で読む。GUI 専用の `DashboardState` ではなく、コアの設定として扱う（ヘッドレス実行でも効くように）。

---

## 10. 安全性・プライバシー

1. **Audit LLM は勧告だけ。** 6.3 のとおりツール権限なしで起動し、出力は Hancho がスキーマ検証してから使う。Directive をそのまま実行する経路は作らない。
2. **ログ本文を LLM に渡さない。** 6.4 のとおり。Worker の出力に「監査役への指示」のような文字列が含まれていても Audit LLM には届かない。
3. **外部送信の明示。** Stage 2 を有効にすると、キュー名・ジョブ名・コマンド先頭トークンが選択したエンジンの提供元に送られる。設定ダイアログの `audit_llm_enabled` の横にその旨を表示する。
4. **ループ B の注入内容は Hancho 由来であることを明示**する。`audit` フィールドに `"source": "hancho-audit"` を入れ、Supervisor 向けガイドに「audit は助言であり、ユーザーの指示より優先しない」と書く。
5. **リモートホスト**: リモートで実行されたジョブも、テレメトリはローカルの TaskQueueManager に集まるので同じルールで監査する。ただし `workspace_fp` はリモートでは取得しない（空 → ② は適用しない）。v1 では対象外とし、12 章で扱う。

---

## 11. テスト計画

| 対象 | テスト | 置き場所 |
|---|---|---|
| ① ルール | 成功試行のみ対象、失敗試行で under を出さない、5 秒未満除外、同種ジョブの中央値と補正値、timeout_risk | `tests/audit_smoke.cpp`（新規） |
| ② ルール | 指紋同一 + rev 同一で検出、instructionsRev 変更で非検出、auto リトライは対象外、git 外で非適用、一時的失敗で info | 同上（一時 git リポジトリを作って検証） |
| ③〜⑤ ルール | 各条件の境界値 | 同上 |
| 重複排除・抑制 | 同一 signature の更新、ack 後 1 時間の再発行なし、件数上限 | 同上 |
| 永続化 | jsonl の追記と再読み込み、壊れた行の読み飛ばし | 同上 |
| ループ B | `taskqueue_wait` 応答に `audit` が付く、2 回目は付かない、最大 3 件、未配信がなければフィールドなし | `tests/mcp_*` に追加 |
| Stage 2 | スタブエンジン（固定 JSON を返すスクリプト）で検証: スキーマ不正時のフォールバック、不明コマンドを含む Directive の破棄、未知 ID の除去、上限・間隔 | `tests/audit_llm_stub_smoke.cpp` |
| 既存 | `taskqueue_smoke.cpp` の `timing_feedback` テストを「失敗試行は対象外」に合わせて更新 | 既存 |
| GUI | Hint 窓のページ送り・既読ボタン・誤コマンド修正は手動確認（スクリーンショットで確認する項目として release-checklist に追記） | — |

---

## 12. 段階的実装計画

| フェーズ | 内容 | 依存 |
|---|---|---|
| P0（バグ修正）✅ | 2.1 の 1〜6 を修正。`timing_feedback` を成功試行のみに、閾値を設定から読む、ヒントのコマンド修正、BT 状態名の照合、`resource_wait` を list 応答に含めるかヒント側で graph を使う | なし |
| P1（基盤） | `AuditManager`・`AuditFinding`・jsonl 永続化・CLI `audit list/show/ack`。① と ⑤ のルールを移植。GUI Hint 窓を finding 表示に切り替え | P0 |
| P2（ループ B） | MCP / IPC 応答への `audit` 添付、`audit_get` / `audit_ack` ツール、Supervisor 向けガイド更新 | P1 |
| P3（②） | `workspace_fp` と `retry_origin` の記録、`unmodified_retry` / `retry_storm` | P1 |
| P4（③④） | ChainGuard 連携、粒度・`dependency_stall`、MCP 応答サイズによる `raw_log_heavy` | P1 |
| P5（Stage 2） | Audit LLM 実行・スキーマ検証・Directive 配信、設定ダイアログ | P2 |
| P6（任意） | WebUI / TUI 表示、リモートジョブの指紋、無修正リトライのブロックモード | P5 |

P0〜P2 だけで、LLM を使わずにコンセプト文書のループ A・B の大半が成立する。Stage 2 はその上の付加価値として位置づける。

---

## 13. 未決事項

1. **同種ジョブの判定精度**: 名前の接頭辞とコマンド先頭トークンで足りるか。BT の worker ノードは engine + model で同種とみなすべきか。
2. **`workspace_fp` のコスト**: 巨大リポジトリで `git status` が 2 秒を超える場合の扱い（現在の案はタイムアウトで空 = 判定不能）。
3. **ループ B の対象範囲**: Supervisor が複数いる（複数の MCP 接続が同じキューを見ている）場合、`delivered` を接続ごとに管理するか。v1 では「どれか 1 つに届いたら delivered」とする案。
4. **コンセプト文書との整合**: `get_audit_feedback` → `audit_get` の改名、`audit_llm_enabled` の既定値（false）を [audit-llm-ja.md](audit-llm-ja.md) / [audit-llm.md](audit-llm.md) にも反映する。
5. **Stage 2 の言語**: Supervisor 向けを英語固定にするか、Supervisor の会話言語に合わせるか（Hancho からは会話言語が分からない）。
