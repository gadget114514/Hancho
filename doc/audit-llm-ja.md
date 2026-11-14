# Audit LLM (監査LLM): Supervisor 監視とフィードバック循環モデル

**[English](audit-llm.md) | [日本語 (Japanese)](audit-llm-ja.md)**

Hancho は、AI コーディングエージェントの自律的なオーケストレーションを支える実行・監視基盤です。
従来の「人間 → Supervisor LLM → Worker LLM / コマンド」という一方通行の指示モデルに対し、Hancho は **Audit LLM（監査LLM）** を導入することで、Supervisor LLM の行動を客観的に評価し、見積もり精度やタスク設計を自律改善する**閉ループ・フィードバック（Closed-loop Feedback）循環**を実現します。

---

## 1. 3層マルチエージェント構造における「Audit LLM」の役割

Hancho における役割分担は以下の 3 層＋1 監査レイヤーで構成されます。

```
        ┌──────────────────────────────────────────────────────────┐
        │               Human User (最終決定者 / 操縦士)            │
        └────────────────────────────┬─────────────────────────────┘
                                     │ 高レベル目標 / 承認
                                     ▼
   ┌─────────────────────────────────────────────────────────────────────────────┐
   │                    Supervisor LLM (作業指揮役 / 監督)                       │
   │  - 計画立案 (`taskqueue_add`, `bt_start`)                                   │
   │  - 実行待機 (`taskqueue_wait`)                                              │
   │  - エラー復旧 (`taskqueue_retry`)                                           │
   └──────────────────┬──────────────────────────────────────▲───────────────────┘
                      │ 実行指示 (1ジョブ=1コマンド)          │
                      │                                      │ 監査フィードバック
                      ▼                                      │ (Timing, Thrashing,
   ┌──────────────────────────────────────┐                  │  Directive)
   │         Worker LLMs / Scripts        │                  │
   │  (`claude -p`, `agy`, ビルド, テスト)│                  │
   └──────────────────┬───────────────────┘                  │
                      │ 標準出力 / exit_code / duration      │
                      ▼                                      │
   ┌─────────────────────────────────────────────────────────┴───────────────────┐
   │                       Hancho コア管理基盤                                   │
   │  - ProcessManager, TaskQueueManager, BtManager                              │
   │  - ResourceLock, Token/Stream Tracker                                       │
   │  - 計測データ: 実実行時間, 見積もり, リトライ回数, 失敗診断                 │
   └──────────────────────────────────┬──────────────────────────────────────────┘
                                      │ 構造化テレメトリ JSON
                                      ▼
   ┌─────────────────────────────────────────────────────────────────────────────┐
   │                     Audit LLM (客観的監査役 / 評価エンジン)                 │
   │  - 見積もり乖離の監査 (Duration vs Estimate)                                │
   │  - スラッシング検知 (コード未修正の無意味な再試行)                          │
   │  - 計画妥当性の監査 (粒度が粗すぎる/細かすぎる、デッドロック)               │
   └──────────────────────────────────┬──────────────────────────────────────────┘
                                      │ 監査ディレクティブ生成
                                      ▼
         ┌────────────────────────────┴────────────────────────────┐
         │                                                         │
   [Route A: 人間へのヒント]                         [Route B: Supervisor への直接注入]
   Hancho Hint ウインドウに表示                       `taskqueue_wait` / `taskqueue_get` の
   「💡 [Audit 勧告] Supervisor が                   レスポンス JSON 内の `"audit"` フィールド、
     同じエラーで 3 回リトライしています」             または `get_audit_feedback` MCP ツール
```

### 各ロールの責務
- **Supervisor LLM (指揮役)**:
  - Claude Code や OpenCode、Antigravity などのメインセッション。
  - ゴール達成のためのタスク分解、Behavior Tree (BT) の生成、Worker の起動、エラー時の修正方針の決定。
- **Worker LLM / Scripts (現場作業員)**:
  - 1つの独立した処理（ファイル修正、ビルド実行、テスト実行、特定調査）をこなすプロセス。
- **Audit LLM (監査役)**:
  - 自身ではコード修正を行わず、**「Supervisor LLM のオーケストレーション行動」を客観的に監査**する第三者エンジン。
  - 軽量・高速な LLM（Gemini Flash, Claude Haiku 等）を使用し、Hancho が収集したテレメトリを分析して Supervisor または人間に是正勧告を発行。

---

## 2. 5大監査項目 (Audit Metrics)

Audit LLM は、Hancho が記録する構造化ログ・メトリクスから以下の 5 項目を重点的に監査します。

### ① 時間見積もり乖離 (Timing Divergence Audit)
- **問題**: Supervisor LLM はハードウェア性能やリポジトリ規模を理解できないため、時間見積もり（`estimate_sec`）やタイムアウト（`timeout_sec`）を過小/過大に設定しがちです。
- **監査内容**:
  - `durationSec`（実実行時間）と `estimateSec`（事前見積もり）の乖離率を算出。
  - 2倍以上超過、または 1/3 以下で完了した場合にアラートを発行。
  - 「過去の同種タスクの中央値は 140秒 です。残りのジョブの見積もりを 150秒、timeout_sec を 300秒 に上方修正せよ」という具体的補正値を Supervisor にフィードバック。

### ② スラッシング・無修正リトライ (Thrashing & Loop Audit)
- **問題**: ジョブが失敗した際、原因を解消しないまま安易に `taskqueue_retry` を繰り返し、トークンと時間を浪費する（Flapping/Thrashing）。
- **監査内容**:
  - 直前の失敗以降、ワークツリー内の関連ファイル（ソースコード）に変更が加えられたかを監査。
  - コード変更がないまま再試行が要求された場合、「同一エラーによる無意味な再試行」として検知・警告。

### ③ コマンド設計・タスク粒度 (ChainGuard & Task Granularity Audit)
- **問題**: `&&` や `;` で複数のシェル処理を 1 つに詰め込もうとしたり、逆に 1 行で済むコマンドを過剰に細分化してタスクキューを氾濫させる。
- **監査内容**:
  - ChainGuard による拒絶履歴や、並列度・依存関係のデッドロックリスクを監査。

### ④ コンテキスト衛生 & トークン効率 (Token Hygiene Audit)
- **問題**: 失敗時に何千行もの生ログを Supervisor のコンテキストに読み込ませ、記憶容量（コンテキストウィンドウ）を枯渇させる。
- **監査内容**:
  - Supervisor が生ログではなく、Hancho の `taskqueue_diagnose`（要約と末尾行）や Artifact を適切に参照しているかを監査。

### ⑤ リソース競合 & 排他ロック (Resource Contention Audit)
- **問題**: 同一ファイルを編集する複数ジョブを同時に走らせようとし、リソースロックで待機状態（`blocked_by`）が多発している状況を監査。

---

## 3. フィードバックの循環方式 (Two Feedback Loops)

Audit LLM が生成した評価結果・指示文は、2 つの経路で伝達されます。

### ループ A: 【人間協調型】トリアージ
トリアージは、対応が必要な対象ごとにカードを作ります。Hancho が原因・背景・環境・条件・理由に分けた情報、処置候補、決定論的な推奨を用意します。流れは **情報 → 助言 → 候補 → 選択** です。Hancho の推奨は決定論で決まり、任意の LLM 助言は候補から選び理由を説明するだけで、新しい処置を作ったり実行したりしません。

`triage_mode` で判断方法を指定します。
- `manual`（手動）: すべて人が選びます。LLM 助言は `triage_llm_mode` に従います。
- `semi`（半自動、既定）: ガードを通過した高確信度かつ安全な Hancho 推奨は自動実行し、それ以外は人の判断を待ちます。LLM 助言は `triage_llm_mode` に従います。
- `auto`（自動）: 判断待ちカードはすべて LLM に相談します。高 / 中確信度の安全な候補でガードを通過した場合だけ Hancho が実行し、それ以外は人の判断を待ちます。

手動 / 半自動で使う `triage_llm_mode` は `uncertain`（既定。推奨の確信度が低い、タイムアウトの見立てが繰り返し / 不明、または前回試行と同じ失敗分類の場合）、`manual`（明示的に相談したときのみ）、`always`（毎回相談）です。決定論的な歯止めにより、カード上の候補だけを許可し、安全でない処置の自動実行を禁止します。同じジョブ・失敗分類への自動再試行は 1 回までで、タイムアウト延長は元の 4 倍とキューの `maxTimeoutSec` を超えません。`needsHuman`、低確信度の助言、助言エラーでは自動処置しません。条件が自然に消えた判断待ちカードは `resolved` になります。判断はカードに記録され、対象ジョブの `notes` にも追記されるため、Supervisor は `taskqueue_get` で確認できます。

LLM 助言は `_triage` タスクキューの非表示ジョブとして実行し、同時実行は 1 件に制限します。カードの情報、候補、Hancho の推奨を助言依頼に含めます。カード項目・候補・設定・動作の詳細は[トリアージ設計](triage.md)を参照してください。

### ループ B: 【自律型】MCP レスポンス自動注入 (Direct Machine Injection)
Supervisor LLM が `taskqueue_wait`、`taskqueue_get`、`bt_wait` を呼んだ際、レスポンス JSON 内に `"audit"` オブジェクトが自動で包含されます。
Supervisor LLM はツールの戻り値を読み取るだけで、外部からの客観的な指示（Directive）として次回タスクのパラメータや修正方針に反映します。

---

## 4. 設定と構成 (Configuration)

Audit LLM の動作は [設定] ダイアログ（および `logs/config.json`）で調整可能です。

```json
{
  "audit_llm_enabled": true,
  "audit_llm_engine": "agy",
  "audit_llm_model": "gemini-3.8-flash-medium",
  "audit_timing_divergence_threshold": 2.0,
  "show_supervisor_hints": true,
  "hint_auto_dismiss_sec": 7
}
```

- **`audit_llm_enabled`**: Audit LLM 機構の有効/無効。
- **`audit_llm_engine`**: 監査に使用する軽量 CLI エンジン（`agy`, `claude`, `opencode`, `codex`）。
- **`show_supervisor_hints`**: GUI ダッシュボードのポップアップ Hint ウインドウの表示/非表示。
- **`hint_auto_dismiss_sec`**: ポップアップが自動消去されるまでの秒数（既定 7）。-1 で手動で閉じるまで表示、0 で表示しない。
