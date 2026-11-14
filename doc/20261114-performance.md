# アイドル時の CPU 使用量レビューと対応（2026-11-14）

## 目的

Hancho がジョブを実行していないとき（アイドル時）に、CPU とディスク I/O を
使い続けている箇所を洗い出し、ポーリング間隔の見直しと設定 UI の追加を行う。

## 調査結果

明確なビジーループ（`Sleep(0)` や待機なしの無限ループ）は見つからなかった。
問題は「固定間隔のポーリング」と「毎回の全件再構築」の組み合わせで、次の箇所が
アイドル時も動き続けていた。

### 1. 使用量パネル（1 秒間隔）

- `src/gui_dashboard.cpp` の `UsagePanelWndProc` が 1 秒ごとに全体を再描画。
- `DrawUsagePanel` は Gantt の全ジョブ再パースと使用量グラフの再構築を行う。
- 24h 範囲でメモリ上の履歴が空の場合、`ReadDailyUsageSummary("24h")` を毎秒実行。
  7d/30d/12m 範囲では `ReadDailyUsageSummary` を無条件に毎秒実行していた。
- `ReadDailyUsageSummary` は日次 JSONL をディレクトリ走査して全行パースする。

### 2. 監視（ストリーム）ペイン（1 秒間隔）

- `src/gui_dashboard.cpp` の `LlmStreamWndProc` が 1 秒タイマーで
  `ReloadStreamDialogData` を実行。
- 選択中プロセスがあると毎秒 `FetchProcessSnapshotIpc`、`GetAgentStateIpc`、
  `LoadMonitorEvents` → `ReadHanchoEventsFile` を実行。
- `IsWindowVisible` のみを見ており、最小化（`IsIconic`）中も動き続けていた。

### 3. ダッシュボード更新時の IPC ファンアウト

- 1 回の更新で `taskqueue_list` を 2 回取得し、`triage_list` が内部でさらに
  `TaskQueueList` と全プロセススナップショットを再構築。
- `KpiSignature` と描画で `taskqueue_counts` を別々に取得していた。

### 4. プロセスメトリクス（200 ms 間隔）

- `ProcessManager::MonitorThreadFunc` が 200 ms ごとに `UpdateJobMetrics` を実行。
- `platform::IProcessTree::Query` はジョブ内の全 PID に対して `OpenProcess` と
  `GetProcessMemoryInfo` を呼ぶため、長時間実行中のプロセスやエージェントが多いと
  アイドル時でもシステムコールが増える。

### 5. タスクキューの O(n²) 走査

- `IsReadyLocked` が依存ごとに `FindJobConst` を呼び、`FindJobConst` は全ジョブを
  線形探索していた。
- `ProcessSchedulerTick` は 300 ms ごと、`TaskQueueList` は取得のたびに
  この走査を行うため、ジョブ履歴が増えるとアイドル時の CPU が増加する。

### その他の残課題

- `TermConnection::Run` は 200 ms 受信タイムアウトごとに `GetSnapshot` を実行する。
- `RemoteSession::SenderLoop` は 100 ms ごとに起床する。
- `remote_client` のイベント購読は 30 ms ごとにファイルを確認する。
- `TriageManager::Loop` は 5 秒ごとに全ジョブ・BT・プロセスを再構築する。
- `TerminalView` のカーソル点滅は 530 ms ごとにターミナル全体を再描画する。
- Chat ダイアログは 1 秒ごとに `chat_status` を取得する。
- WebUI は 3 秒ごとに自動更新し、monitor タブで日次使用量を再取得する。

## 対応内容

### 1. 使用量パネルを 5 秒間隔に変更

- 既定の再描画間隔を 5000 ms に変更。
- 非表示のときは再描画しない。
- 設定ダイアログの「パフォーマンス」タブから変更可能。

### 2. 監視ペインを 5 秒間隔に変更

- 既定のポーリング間隔を 5000 ms に変更。
- 最小化中（`IsIconic`）はポーリングしない。
- 設定ダイアログの「パフォーマンス」タブから変更可能。

### 3. ダッシュボード IPC の重複を解消

- 同じ更新ティック内で取得済みの `taskqueue_list` JSON を再利用し、
  トリアージ後の再描画で IPC を再度呼ばないようにした。
- `taskqueue_counts` を短時間キャッシュし、KPI シグネチャと描画で共有する。

### 4. プロセスメトリクスを 5 秒間隔に変更

- `ProcessManager` に `SetMetricsIntervalMs` / `GetMetricsIntervalMs` を追加。
- 既定は 5000 ms。プロセスごとに最終取得時刻を保持して間隔を制御する。
- プロセス終了検出とタイムアウト監視は従来どおり 200 ms 間隔のまま
  （応答性を維持しつつ、重いメトリクス取得だけを間引く）。
- `config.json` の `process_metrics_interval_ms` で設定可能。
  `config_set` で実行中のコアにも即時反映する。

### 5. タスクキューのジョブ ID インデックス

- `TaskQueueManager` に `id -> (queue, index)` のインデックスを追加。
- ジョブの追加・削除時に dirty フラグを立て、次回参照時に O(n) で再構築。
- `FindJob` / `FindJobConst` は O(1) 参照になり、依存解決・一覧取得・
  件数集計の O(n²) 走査を解消。

### 6. 設定ダイアログに「パフォーマンス」タブを追加

次の 4 項目を手動で設定できる。

| 設定 | 既定値 | 範囲 | config.json キー |
| --- | --- | --- | --- |
| ダッシュボード更新間隔 | 2000 ms | 100〜60000 ms | `refresh_interval_ms` |
| 使用量グラフ更新間隔 | 5000 ms | 1000〜60000 ms | `usage_refresh_interval_ms` |
| 監視ペイン更新間隔 | 5000 ms | 1000〜60000 ms | `stream_refresh_interval_ms` |
| プロセス CPU/メモリ取得間隔 | 5000 ms | 1000〜60000 ms | `process_metrics_interval_ms` |

値を大きくすると CPU とディスク I/O を抑えられる。実行中のジョブが多いときは
既定値のままを推奨する。

## 検証

- `cmake --build build --config Release --target Hancho hancho-cli` が成功すること。
- 設定値を変更すると、次回起動を待たずにタイマーへ反映されること。
- プロセスメトリクス間隔は `config_set` 経由でコアに反映されること。

## 未対応（今後の候補）

- ターミナルセッションの 200 ms ポーリングと、リモート送信スレッドの 100 ms 起床。
- リモートイベント購読の 30 ms ファイル確認。
- Triage の 5 秒フル再構築（バージョン差分でのスキップ）。
- ターミナルのカーソル点滅による全画面再描画（部分描画化）。
- Chat ダイアログと WebUI のポーリング間隔の設定 UI 化。
