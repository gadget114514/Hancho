# Hancho v3 設計: 未実装項目

**[日本語 (Japanese)](design-v3-todo.md)**

[設計文書](design-v3.md)（[English](design-v3-en.md)）のうち、まだ実装していない項目の一覧です。v0.9.4 時点のコードと README に対照して作成しました。ロードマップ（設計文書 §6）の大部分、すなわち 1 の段階 1〜3、2 の段階 A/B、3 の M1〜M3、4 の全段階、5 の文書と片付けコマンドは v0.9.2〜v0.9.4 で前倒し実装済みです。

[設計文書](design-v3.md) に戻る。

---

## 1. 旧イベント形式の廃止（§1.8 段階 4）

- `hancho-cli worker stream` と MCP `get_worker_stream` の旧 `type` 文字列形式をやめる。
- 現状: `--schema legacy` / `schema: "legacy"` が v0.10.x まで有効で、README では **v0.11.0 で削除**すると告知済み。削除時は [移行手順](LLM-jsonstream-format.md#legacy-type-strings-and-migration-to-hanchoevent1) も整理する。

## 2. usage 長期集計（§3.6.1 U1〜U3）

- U1 日次ロールアップ（`hancho.usage.day/1`）、U2 月次ロールアップ、`usage stats` / `usage export` コマンド、MCP `usage_stats` / `usage_export`、IPC `get_usage_daily`、圧縮（`usage compact`）、GUI の範囲選択の 7d / 30d / 12m 拡張。
- 現状: 未実装。`llm_stream.cpp` は 24 時間保持のまま（`kUsageRetentionSec`）。`usage_history.jsonl` は追記だけで縮まないため、U1 を足す前にこの無限成長を止める必要がある。
- 設計文書 §6 では U1 を v0.11 で先行させる予定だった。

## 3. Hancho が自動で書くノート（§3.4）

- 決定的な規則による L2 ノートの自動記録:
  - 同じジョブ名が同じ失敗分類で 3 回続いた。
  - 所要時間がタイムアウトの 80% を超える試行が続いた。
  - 失敗の後に成功した（失敗試行と成功試行の `filesEdited` 差分）。
- 現状: 未実装。`taskqueue.cpp` に author が `hancho` のノートを書く経路がない。

## 4. memory prune と容量上限（§3.7）

- `hancho-cli memory prune`: 古い月の L1 ファイルを消す（既定 90 日、`--dry-run` 対応）。
- `memory/` 全体の容量上限の設定（`config.json` の `memory_max_bytes`）と、超えたら古い L1 から削除、L2/L3 は自動で消さず上限接近を警告する（CLI および GUI 通知）。
- 現状: 実装済み（カード 054）。`memory stats` にも容量と上限を含める。

## 5. 埋め込み検索プラグイン（§3.13 M4）

- `IMemoryRanker` を外部プロセスのプラグインで差し替える仕組み。プラグインは stdin で問い合わせと候補を受け取り、stdout にスコアを返す。
- 現状: 実装済み（カード 055-1、055-2）。データディレクトリの `config.json`（または CLI `--ranker`）で外部プラグインを指定可能。プロセスの起動失敗・異常終了・タイムアウト・不正な出力時は組み込み BM25 へ自動フォールバックし、検索を止めない。サンプルプラグインおよびスモークテスト追加済み。

## 6. リモートの残り（§2.5 段階 C）

基本（`hosts.json`、トークン認証、WebSocket 越しの IPC、ジョブの `host` 指定、ログ・イベント・`.done` の取り寄せ）は実装済み。残るもの:

- 手元のダッシュボードを再起動したときのリモートプロセスへの再接続。現状は手元の試行が `interrupted` になり、再接続しない（[remote-hosts.md](remote-hosts-ja.md) の制限）。
- リモート実行での `success.filesNewer` の評価: 実装済み（カード 057）。ホスト側で `check_files_newer` IPC アクション（または試行の `.done` メタデータ）により評価。ホスト側の時刻系で判定し、時計のずれに影響されない。
- WebSocket 1 接続 1 要求・1 応答を越えた、ログやイベントの常時ストリーミング: 実装済み（カード 058）。WebSocket 多重化、購読アクション（.events.jsonl / .stream.jsonl / .tail.txt）、seq 再開、監視ペインへの常時ストリーミング反映をサポート。

## 7. 未決事項（§7）

決めたもの（2026-11-11。理由とカードは `doc/design-v3.md` §7 にある）:

- イベントスキーマの JSON Schema: **`data` の型まで縛る**。カード `ww-data/tasks/071-schema-data-types.md`。
- JSON ストリームのない普通のプロセスにも RunRecord を作る。カード `ww-data/tasks/072-runrecord-plain-process.md`。
- L3 の昇格: **人に加えて、設定で列挙した特定のワーカーにも許す**。実装済み（カード `ww-data/tasks/073-l3-worker-promotion.md`）。設定 `memory.l3Promoters`、CLI `--by worker:<name>` / `--worker <name>`、MCP `memory_approve` 対応。
- リモートの認証: **トークンのまま。広げない**（実装なし）。
- `incomplete` と `success_check_failed` の境目: **証拠ベース**（計画イベントに未完了 TODO が残る積極的な証拠があるときだけ `incomplete`）。`src/taskqueue.cpp` で実装済み（カードなし）。
- Linux / macOS の画面: **GUI と TUI の両方を作る**（既述）。下記 8 に移動。

---

## 8. Linux / macOS の GUI と TUI（設計変更）

POSIX 対応（設計文書 §2.4 段階 B）の進め方を変更します。設計文書は Linux / macOS 向けの画面を「端末向けの簡易表示（`hancho-cli top`）だけ」、GUI ダッシュボードを非目標としていましたが、**両方作る方針に変えます**。

- **GUI**: Linux / macOS 向けのダッシュボードを新しく作る。**ブラウザ UI とする**: コアが localhost の HTTP + WebSocket サーバーを立て、同梱の静的 HTML/JS をブラウザに開かせる。ネイティブ GUI（Qt、GTK、Tauri など）は **ペンディング**（ブラウザ UI で足りなかったものが再開条件）。方式・認証・資産同梱・主要 5 機能の実装・ビルドと配布パッケージングの文書化は [browser-ui-ja.md](browser-ui-ja.md)（[English](browser-ui.md) / カード 061-1 〜 061-4）で完了済み。
- **TUI**: `hancho-cli top` を簡易表示のままにせず、ダッシュボードと同じ機能の端末向け版として育てる。外部ライブラリを追加せず、自前の ANSI/VT100 エスケープシーケンスとプラットフォーム端末制御による単一バイナリ方式を採用（[tui-design-ja.md](tui-design-ja.md) / [English](tui-design.md)、カード 062-1 〜 062-4）。骨格（更新ループ・キー入力・タブ切り替え）、主要 5 画面（Processes, Queue, Monitor, Why, Memory）、ジョブ操作（停止・再試行・取消）、リモートジョブ表示、POSIX 動作確認手順、および出力成型の自動スモークテストを整備完了。
- 機能の範囲は、Windows 版ダッシュボードの主要機能（プロセス一覧、タスクキュー、実行状態監視ペイン、失敗診断、メモリ一覧）を基準に、Linux / macOS の実情に合わせて削る。

---

## 優先度の目安

| 項目 | 目安 |
|---|---|
| 1. 旧形式の廃止 | v0.11.0 の告知済み期限あり。先に済ませる |
| 2. usage 長期集計 | 設計文書が v0.11 先行を意図。`usage_history.jsonl` の成長止めが前提 |
| 3. 自動ノート、4. prune | メモリの運用品質。小さいので先行実装しやすい |
| 5. 埋め込みプラグイン | 境界だけあり、需要次第 |
| 6. リモートの残り | 使い込みで判明した不足。需要に応じて |
| 8. Linux / macOS の GUI と TUI | 方針・方式・認証・主要 5 機能・ビルド配布文書化完了（[browser-ui-ja.md](browser-ui-ja.md)）。GUI はブラウザ UI（ネイティブはペンディング）。TUI は自前 ANSI 方式に決定し、主要 5 画面・ジョブ操作・リモート表示・POSIX 確認手順・自動スモークテストまで完了（[tui-design-ja.md](tui-design-ja.md) / カード 062-1 〜 062-4） |
