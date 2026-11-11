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

- `hancho-cli memory prune`: 古い月の L1 ファイルを消す。
- `memory/` 全体の容量上限の設定と、超えたら古い L1 から削除、L2/L3 は自動で消さず上限接近を警告する。
- 現状: 未実装。`memory_cli.cpp` の op は `search` / `get` / `brief` / `stats` / `approve` / `note` / `retract` / `rebuild` のみ。

## 5. 埋め込み検索プラグイン（§3.13 M4）

- `IMemoryRanker` を外部プロセスのプラグインで差し替える仕組み。プラグインは stdin で問い合わせと候補を受け取り、stdout にスコアを返す。
- 現状: 部分的。境界（`src/memory_search.h` の `IMemoryRanker`）はあるが、プラグインを起動して組み込む経路がない。

## 6. リモートの残り（§2.5 段階 C）

基本（`hosts.json`、トークン認証、WebSocket 越しの IPC、ジョブの `host` 指定、ログ・イベント・`.done` の取り寄せ）は実装済み。残るもの:

- 手元のダッシュボードを再起動したときのリモートプロセスへの再接続。現状は手元の試行が `interrupted` になり、再接続しない（[remote-hosts.md](remote-hosts-ja.md) の制限）。
- リモート実行での `success.filesNewer` の評価。現状は評価しない。
- WebSocket 1 接続 1 要求・1 応答を越えた、ログやイベントの常時ストリーミング。

## 7. 未決事項（§7）

未決のまま:

- イベントスキーマの JSON Schema を `data` の型まで縛るか。
- JSON ストリームのない普通のプロセス（キューの外で起動したもの）にも RunRecord を作るか。
- L3 の昇格を人以外（特定のワーカー）にも許すか。
- リモートの認証をトークン以外（SSH 鍵の流用など）にも広げるか。
- `incomplete` と `success_check_failed` の境目をどう決めるか。

決めたもの:

- Linux / macOS の画面: **GUI と TUI の両方を作る**（「`hancho-cli top` だけにするか」を決定）。設計文書 §2.6 の「Linux / macOS の GUI ダッシュボード」の非目標を取り消し、下記 8 に移動。

---

## 8. Linux / macOS の GUI と TUI（設計変更）

POSIX 対応（設計文書 §2.4 段階 B）の進め方を変更します。設計文書は Linux / macOS 向けの画面を「端末向けの簡易表示（`hancho-cli top`）だけ」、GUI ダッシュボードを非目標としていましたが、**両方作る方針に変えます**。

- **GUI**: Linux / macOS 向けのダッシュボードを新しく作る。Windows の Direct2D 版は持ち込まず、コアの IPC クライアントとして GUI を書き直す（設計文書 §2.3 段階 A の「GUI はコアの IPC クライアントにする」を全プラットフォームに広げる）。対応候補は Qt、GTK、Tauri など。実装方法は別途検討する。
- **TUI**: `hancho-cli top` を簡易表示のままにせず、ダッシュボードと同じ機能の端末向け版として育てる。実装方法は別途検討する。
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
| 8. Linux / macOS の GUI と TUI | 方針は決定済み。GUI は IPC クライアント化が前提 |
