# Hancho 0.10 機能・実装レビューレポート

- **レビュー日**: 2026-11-25
- **対象ブランチ / バージョン**: `main` (`CMakeLists.txt`: `0.10.0`, HEAD: `54002d9`)
- **レビュアー**: Gemini 4 Argon (Medium)

---

## 1. エグゼクティブサマリー

Hancho 0.10 系は、従来の「単発プロセス・フラットなジョブキュー・BT（ビヘイビアツリー）」という構成から、**`task` → `direction`（＝タスクカード／トリアージカード）→ `job` → `attempt`** という明確な4階層アーキテクチャへと進化させるメジャーアップデートです。

設計ドキュメント（`doc/terminology-ja.md` およびカード 100-x〜106-x）と現在の `main` ブランチのソースコード・テストを照合した結果、以下の到達度と課題が確認されました。

### 全体評価
- **データ層・CLI/MCP 層・CI 基盤・国際化（i18n）基盤の完成度は非常に高い**:
  - `TaskStore` / `DirectionStore` / `TranslationStore` の各ストアはロック順序（`TaskQueueManager::mutex_` → `DirectionStore::mutex_` → `TaskStore::mutex_`）や状態非保存の原則（ジョブからの動的算出により不整合を防止）が徹底されている。
  - CI 対応（`run --ephemeral` 等）は Win32 Job Object (`JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE`) とクライアント PID 監視を組み合わせ、すべてオプトインで既存挙動を壊さない堅実な実装になっている。
- **未完了のカードおよびブランチ間の取りこぼしが一部残存**:
  - **102-x（タスク一覧の刷新・トリアージの direction 単位化）** の後半（`102-5` 仮想リスト、`102-7` 判断でのカードクローズ、`102-8` トリアージ差分更新、`102-9` 負荷試験スクリプト、`102-10` トリアージの direction 単位化）が `main` で未着手。
  - `stable` ブランチ（v0.9.45 / v0.9.46）に先行投入された **102-1（Overlapped Pipe I/O・ListView 差分描画最適化）** が、`CLAUDE.md` の cherry-pick 停止方針（`e87a7ac`）の影響で `main` に取り込まれておらず、`main` の `src/ipc_client.cpp` に `Sleep(10)` ポーリングが残っている。

---

## 2. 機能領域別 実装状況マトリクス

| シリーズ | テーマ | カード範囲 | 実装状況 | 評価・備考 |
| :--- | :--- | :--- | :---: | :--- |
| **100-x** | タスクと系譜（Task & Lineage） | 100-1 〜 100-10 | **完了** | `TaskStore`, `supersedes`/`supersededBy`, `taskqueue supersede`, WebUI/TUI/Dashboard 統合済み |
| **101-x** | 修正失敗時のエンジン切り替え | 101-1 〜 101-6 | **完了** | `switchEngineOn` (`after`), `blame`, `fix_no_progress`/`fix_oscillating`, `handoff`, `switchLimits` 実装済み |
| **102-x (A)** | LLM エラーの構造化分類 | 102-1 〜 102-4 | **完了** | `ClassifyLlmErrorStructured` (401/403/429/5xx・error code), `ww-run` 構造化判定対応済み |
| **102-x (B)** | タスク一覧の刷新・direction トリアージ | 102-1 〜 102-10 | **一部完了** | **完了**: 102-2, 102-3, 102-4, 102-6<br>**未実装 (main)**: 102-1 (stableのみ), 102-5, 102-7, 102-8, 102-9, 102-10 |
| **103-x** | direction レコードと 0.10.0 用語統一 | 103-1 〜 103-8 | **完了** | `DirectionStore`, `direction:<id>`, `exec`/`agent`, `--list agent`, `peer`/`host`/`flow`, `run_agent`（後方互換エイリアス維持） |
| **104-x** | タスクカードの Hancho 本体保存 | 104-1 〜 104-6 | **ほぼ完了** | `card add/list/show/edit/remove/run/redo/import/export/translate` 実装済み。104-6 の「0.10 自身のカードを Hancho に投入して運用」のみ残課題 |
| **105-x** | CI 対応（Jenkins / GitHub Actions） | 105-1 〜 105-6 | **完了** | `--ephemeral`, `HANCHO_AUTOSTART`, `HANCHO_NO_DIALOGS`, `--wait-scope direction`, `--cancel-on-exit`, `--follow`, `--logs-dir`, `--report-output` |
| **106-x** | 英語データ保存とオンデマンド翻訳 | 106-1 〜 106-6 | **完了** | `TranslationStore`, `NormalizeEnglishField`, `DisplayTextCache`, `card import/export/translate` 多言語対応、`3df3357` で既定 `off` 化 |

---

## 3. 各機能の技術レビュー詳細

### 3.1 タスクと系譜（100-x）および Direction・用語刷新（103-x）
- **設計の妥当性**:
  - `task`（依頼・意図・経過ログ）→ `direction`（キューへの1回の登録＝BT 1本または `taskqueue add` 1回分）→ `job`（1プロセス）→ `attempt`（1試行）の階層化により、これまで「タスク」「ジョブ」「ワーカー」が複数の意味で使われていた混乱（`doc/terminology-ja.md`）が解消された。
  - `src/task_store.cpp` と `src/direction_store.cpp` が状態をディスクに保存せず、常に配下の `job` / `BT` から `TaskState()` / `DirectionState()` を計算する設計は、クラッシュ時や手動操作時の状態不整合を根絶しており優れている。
  - ジョブが `purge` されても、削除直前に `removedJobs` へ最小限の系譜・終端状態を退避するため、過去のタスク結果が壊れない。
- **後方互換性**:
  - BT の `"type": "exec"` / `"type": "agent"`（旧 `job` / `worker`）、CLI/MCP の `run_agent`（旧 `run_worker`）、`--list agent`（旧 `--list worker`）、Chat の `peer` / `host` / `flow` のすべてで 0.9.x の旧名がエイリアスとして維持されており、既存のルーチンやスクリプトを壊さない配慮が行き届いている。

### 3.2 修正失敗時のエンジン切り替え（101-x）と構造化エラー分類（102-x A）
- **設計の妥当性**:
  - テスト実行ノード（`exec`）に `"blame": "<agent id>"` を指定させることで、新しい複合ノード型を増やさずに「agent の修正が原因でテストが落ちた」という因果関係を BT エンジンに認識させた点が非常にクリーンである。
  - 失敗したテスト集合や `failLineRegex` から失敗署名（`failSignature`）を生成し、直近3回と比較して `fix_no_progress`（同一テスト失敗）・`fix_oscillating`（A→B→A の先祖返り）・`fix_failed`（前進ありの失敗）を自動分類するロジックにより、LLM が同じ修正ループでトークンを浪費する問題を防いでいる。
  - さらに `PC_BT_RESULT_FILE` の `handoff` (`{suspected, tried, ruledOut}`) と `HANCHO-RESULT: root_cause_not_found` を次のモデルへのプロンプト末尾（最大 6,000 バイト）に自動注入する仕組みは、マルチモデル運用の実用性を大きく高めている。
  - `fbdb0cc`（v0.9.47 相当）により、試行失敗後にエンジンが `down` 判定された場合、`switchEngineOn` に該当クラスが列挙されていなくても次の正常な承認済み候補へ即座に切り替わるよう改善された点も重要。
  - `src/llm_stream.cpp` の `ClassifyLlmErrorStructured` により、ログ文字列の正規表現に頼らず HTTP ステータス（401/403/429/500/502/503/504/529）とエラーコード（`authentication_error`, `insufficient_quota`, `overloaded_error` 等）から高確度で分類できるようになった。

### 3.3 タスクカード管理と Markdown 入出力（104-x）
- **設計の妥当性**:
  - 「タスクカード ＝ `planned` 状態の `direction`」として `DirectionStore` に統合したことで、データモデルを増やさずに「未実行の計画 (`planned`) → 実行中 (`running`) → 失敗＝トリアージカード (`failed`) → 救済 (`card redo` による `-r1` 系譜)」を一貫して表現できている。
  - `src/card_markdown.cpp` のパーサは、日本語・英語・スペイン語の見出し・前提テーブル・矢印チェーン（`102-2 → 102-3 → 102-6`）・範囲表記（`103-1〜103-7`, `103-1 to 103-7` 等）および「共通の約束」セクションの自動付与に対応しており、`doc/*-cards.md` をそのまま `hancho-cli card import` で取り込める。
  - `src/bt_templates.h` に追加された `card` テンプレート（`impl` agent → `build` exec → `test` exec + `catch` ループ）とタスクの `--card-defaults` により、supervisor が毎回長大な BT JSON を手書きする必要がなくなった。

### 3.4 CI サポート（105-x）
- **設計の妥当性**:
  - `src/ephemeral_core.cpp` の `EphemeralCoreGuard` は、`CREATE_SUSPENDED` で起動した `HanchoCore.exe --headless` を `JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE` 付きの Win32 Job Object に割り当ててから `ResumeThread` している。これにより、Jenkins や GitHub Actions がビルド中止で `hancho-cli` を `TerminateProcess` しても、OS カーネルが確実にコアと全子プロセスを即座に回収する（`tests/ephemeral_smoke.cpp` で検証済み）。
  - 共有コア方式でも `--wait-scope direction` と `--cancel-on-exit`（`WatchDirectionClient` によるクライアント PID 監視）が実装されており、複数パイプラインが同一コアを共有しても互いに干渉しない。
  - `--ephemeral` 時は既定で `triage_mode: "off"`, `triage_llm_mode: "manual"` を書き込むため、CI 上でのテスト失敗時に意図せずトリアージ LLM が大量呼び出しされる事故を防いでいる。

### 3.5 データ英語化とオンデマンド翻訳（106-x）
- **設計の妥当性**:
  - `src/translation_store.cpp` は SHA-256 ハッシュ＋原文一致確認付きのキャッシュ（`<data-dir>/translations/<lang>.json`、LRU 20,000 件）、最大 50 件 / 8,000 文字のバッチ翻訳ワーカー、3回失敗時の1時間バックオフを備える。
  - ダッシュボード側（`pc::DisplayTextCache`）は描画中に IPC を呼ばず、1回のリスト構築で発生したキャッシュミスだけを `DashFlushTranslations` でまとめて非同期要求し、`translationRevision` が進んだときだけ再描画する設計となっており、UI をブロックしない。
  - コミット `3df3357` において、`translate_mode` と `translate_cards` の既定値が `"off"` に変更され、設定画面に「オンデマンド翻訳を有効にする (LLM)」の明示的なトグル（`IDC_SET_TRANSLATE_MODE`）が追加された。意図しない LLM トークン消費を防ぐ安全なデフォルト設定として適切である。

---

## 4. 発見された課題・未実装項目・バグ（重要度順）

### 【高】4.1 タスク一覧のパフォーマンス対策とトリアージの Direction 単位化（102-x 後半）の未完了
`doc/task-list-redesign-cards.md` で計画されたカードのうち、以下が `main` ブランチで未実装または途中となっています。

1. **`102-1` の Overlapped IPC と描画キャッシュが `main` に未マージ**:
   - `stable` ブランチのコミット `47c72d0`（v0.9.46）で実装された `src/ipc_client.cpp` の `FILE_FLAG_OVERLAPPED` + イベント待機（`Sleep(10)` ポーリング廃止）および `src/gui_dashboard.cpp` の行キャッシュ・ListView 差分更新が、`main` ブランチには入っていません。
   - **現状の `main`**: `src/ipc_client.cpp:364` の `WaitForPipeData` が依然として `Sleep(10)` ループになっており、タイムアウト付き IPC 呼び出しのたびに最低 10〜16ms の遅延が発生します。
2. **`102-5`（ダッシュボードの仮想リスト `LVS_OWNERDATA`）が未実装**:
   - `src/gui_dashboard.cpp` に `LVS_OWNERDATA` / `LVN_GETDISPINFO` / `ListView_SetItemCountEx` がまだ導入されていません。
3. **`102-6` の一部（ダッシュボードの KPI タスク基準集計）が無効化されたまま**:
   - `src/gui_dashboard.cpp:3051` に `const bool hasTaskCounts = false;` がハードコードされており、下段 KPI カードがタスク基準（主）＋ジョブ基準（副）ではなく、常に `tqCounts`（ジョブ基準）のみを表示しています。
4. **`102-7`・`102-8`・`102-10`（トリアージの direction 単位化・差分更新・カードクローズ連動）が未実装**:
   - `src/triage_manager.cpp:305` の `TriageManager::Refresh()` は依然として毎回 `tqm_.TaskQueueList("", "", 0, err, "", "", true)` で全ジョブを取得しており、`102-2` の `rev` を使った差分更新（`102-8`）になっていません。
   - `src/triage.cpp:227` の `BuildJobFailureCard` はジョブ単位（`key = "job:" + jobId + "#" + attemptCount`、`target.jobId`）のまま動作しており、BT の fix ループ途中の失敗や 1 direction 内の複数ジョブ失敗を 1 枚の direction トリアージカードにまとめる処理（`102-10`）および判断時の `direction close`（`102-7`）が未実装です。
5. **`102-9`（負荷試験スクリプト `tools/loadtest/` と `HANCHO_PERF_OUT`）が未追加**:
   - 403 ジョブ負荷試験を再現する `tools/loadtest/` がリポジトリに存在しません。

### 【中】4.2 `llm_stream_smoke` における時刻境界（05:00 ラウンド）依存のテスト失敗
`build/Release/llm_stream_smoke.exe` を実行すると、全テスト中 1 件だけ以下の失敗が発生します。
- **失敗箇所**: `tests/llm_stream_smoke.cpp:358`
  ```
  [FAIL] point within 7 days was kept in usage_history.jsonl
  ```
- **原因**:
  - `src/llm_stream.cpp:270` の `PruneUsageHistory()` は、直近の丸め境界時刻（`LastRoundBoundary(now, g_usageRoundMinutes)`、既定はローカル時刻 05:00）より古いレコードの `epochSec` を、その日のローカル 00:00（`LocalDayStart(epoch)`）に丸めて合算します。
  - 一方、`tests/llm_stream_smoke.cpp:329` は `int64_t oneDayAgo = nowSec - 1 * 86400;` で記録したあと、356 行目で `if (ep == oneDayAgo) hasRecent = true;` と**丸め前の正確な秒単位のタイムスタンプ一致**で検査しています。
  - テスト実行時刻が 05:00 以降の場合、`oneDayAgo` は本日の 05:00（`roundCutoff`）より前になるため、`PruneUsageHistory` によって前日の 00:00（`LocalDayStart(oneDayAgo)`）に丸められ、`ep == oneDayAgo` が常に false になります。
- **修正案**:
  - `tests/llm_stream_smoke.cpp:356` で `ep == oneDayAgo || ep == LocalDayStart(oneDayAgo)`（または `ep >= cutoff`）を検査するように修正する。

### 【低】4.3 ドキュメント・バージョン表記・未コミット変更の整理
1. **作業ツリーに未コミットの変更が存在**:
   - `src/gui_dashboard.cpp` に設定ダイアログの LLM タブの入力欄ラベル（`lblLlmEngine`, `lblLlmModel`, `lblLlmProvider`, `lblLlmRank`, `lblLlmNote`）を追加する未ステージの変更（+40行）があります。3言語 `Tr()` に対応した有用な UI 改善であり、問題なければコミットに含めるべきです。
2. **バージョン表記とリリースノートの更新漏れ**:
   - `README.md:9` / `README-ja.md:9` のバージョン表記例が `current: 0.9.26` のままになっています。
   - `README.md` / `README-ja.md` のリリースノートに `v0.9.45` と `v0.9.46`（`stable` でリリース済み）および `v0.10.0` の項目がまだ記載されていません（`dist/release-notes-draft-v0.10.0.md` も未作成）。
3. **`doc/` 配下のカード文書のステータス表記と英語化ルール（104-6 / 106-6）**:
   - `doc/direction-cards.md`, `doc/task-card-store-cards.md`, `doc/ci-cards.md`, `doc/i18n-cards.md` の冒頭「状態:」がすべて「タスクカードまで書いた（2026-11-24）。未着手」のままですが、実際には大半が実装・テスト済みです。実装状況を反映するか、104-6 の方針どおり Hancho のタスクカードへ取り込んでステータス行を更新する必要があります。
   - `skills/hancho/SKILL.md:72,141` や `332` 行目の例に一部旧名（`run_worker`, `"type": "job"`, `"type": "worker"`）が残っています（同ファイル後半では `run_agent`, `exec`, `agent` に更新済みのため、前半のサンプルも統一するとより一貫します）。

---

## 5. 0.10.0 リリースに向けた推奨アクションプラン

1. **`stable` からの `102-1` バックポート取り込み**:
   - `47c72d0` のうち `src/ipc_client.cpp` の Overlapped I/O 化（`WaitForPipeData` の `Sleep(10)` 廃止）を `main` に取り込む（`102-4` のバックグラウンドスレッドと組み合わせることで IPC レイテンシが最小化される）。
2. **`102-x` 残カードの扱い決定**:
   - **案A（0.10.0 に含める）**: `102-10`（トリアージの direction 単位化）→ `102-7`（判断で direction を閉じる）→ `102-8`（トリアージ差分更新）→ `102-5`（`LVS_OWNERDATA` 仮想リスト）→ `102-9`（負荷試験）を順に実装する。特に `doc/terminology-ja.md` で「トリアージのカード＝失敗した direction そのもの」と定義しているため、`102-10` と `102-7` は 0.10.0 で入れておくと概念と実装が完全に一致する。
   - **案B（0.10.1 へ分割）**: 既に完成している 100-x / 101-x / 103-x / 104-x / 105-x / 106-x と `102-2,3,4,6` で 0.10.0 を出し、`LVS_OWNERDATA` とトリアージの direction 単位化を 0.10.1 とする（その場合は `doc/terminology.md` のトリアージ節に「0.10.1 で移行予定」と注記する）。
3. **テストと未コミット変更のクリーンアップ**:
   - `tests/llm_stream_smoke.cpp:356` の時刻丸め判定を修正し、全スモークテストをグリーンにする。
   - `src/gui_dashboard.cpp` の LLM リストラベル追加をコミットし、ビルドして `Hancho.exe` / `hancho-cli.exe` のバイナリ世代を `0.10.0` に揃える。
