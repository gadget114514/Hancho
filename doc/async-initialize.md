# 起動時のタスク読み込み非同期化（設計メモ）

## 目的

タスク（ジョブ）が多い状態でダッシュボードを起動するとウィンドウが固まる（表示されない・応答なしになる）問題を、初期読み込みの非同期化によって解消する。

## 調査結果

- `src/gui_dashboard.cpp:20804`（`DashboardWndProc` の `WM_CREATE`）で `RefreshDashboardData(st)` を**同期的に**呼んでいる。
- `CreateWindowExW` の最中に `WM_CREATE` が同期実行されるため、全データの読み込みが完了するまで `CreateWindowExW` が戻らず、`ShowWindow` より前に UI スレッドがブロックされ、ウィンドウが表示されない／固まって見える。
- 重い箇所は `RefreshTaskQueueData`（`:6789`）→ `TaskOwnerUsageForJob`（`:6696`）。
  - 各ジョブ・attempt ごとに `ListDirectoryWide` でイベントファイルを探索し、`TaskUsageAccumulateFile` / `TaskUsageAccumulateStreamFile` でファイル全体を読み込む。
  - タスク数に比例してディスク I/O と JSON パースが急増する。
  - 1 回の refresh tick 内で `RefreshTaskQueueData` が 2 回呼ばれる（`:7688` で初回、`:7689` の `UpdateTriageFromCore` 後にキャッシュ利用 `force=true, useCachedList=true` で `:7690`）。

## 対応方針

### 1. 初回リフレッシュをメッセージ経由にする

- `WM_CREATE` では `RefreshDashboardData(st)` を同期呼び出しせず、`st->initialTaskUsagePending = true` を設定した上で `PostMessageW(hwnd, WM_PC_INITIAL_REFRESH, 0, 0)` を投げて即座にリターンする。
- これにより `CreateWindowExW` が速やかに完了し、`ShowWindow` / `UpdateWindow` によるウィンドウ表示が先に行われる。

### 2. 重い usage 読み込みをワーカースレッドへ分離（2段階読み込み）

1. **関数の UI 状態非依存化**:
   - `TaskUsageAccumulateFile`（`:6541`）と `TaskUsageAccumulateStreamFile`（`:6632`）の第 1 引数を `DashboardState*` から `std::map<std::wstring, TaskUsageFileCache>& usageCache` に変更する。
   - `TaskOwnerUsageForJob`（`:6696`）が `st` から参照しているのは `taskUsageFileCache` と `taskAttemptEventsPath` の 2 つのみであるため、引数を以下のように変更する：
     ```cpp
     static TaskOwnerUsage TaskOwnerUsageForJob(
         std::map<std::wstring, TaskUsageFileCache>& usageCache,
         std::map<std::wstring, std::wstring>& eventsPathCache,
         const std::string& jobId, const std::string& jobJson,
         const std::string& command, bool deferUsage = false);
     ```
   - `deferUsage == true` の場合は、`out.engine = DetectAgentType(command, "");` のみを設定して即座に `return out;` し、ファイル探索・読み込みを一切行わない。

2. **第 1 段階（UI スレッド・即時表示）**:
   - `WM_PC_INITIAL_REFRESH` ハンドラで、`st->initialTaskUsagePending = true` の状態で `RefreshDashboardData(st)` を実行する。
   - `RefreshTaskQueueData` 内の `TaskOwnerUsageForJob` に `deferUsage = st->initialTaskUsagePending` を渡すことで、イベントファイル走査をスキップし、タスクの基本情報と engine 名のみでリストビューを即時描画する。
   - 描画後、`st->lastTaskListJson` が空（`ListDisplayMode::ProcessOnly` 時など）またはジョブが 0 件の場合は、ワーカースレッドを起動せず即座に `st->initialTaskUsagePending = false` に戻す。ジョブが存在する場合はワーカースレッドを起動する。

3. **第 2 段階（ワーカースレッド・バックグラウンド集計）**:
   - スレッドには `st->lastTaskListJson` のコピー文字列を値渡しし、UI スレッド側のマップには直接触れない。
   - スレッド内のローカルマップ（`localUsageCache`, `localEventsPath`）に対して各ジョブの `TaskOwnerUsageForJob(..., /*deferUsage=*/false)` を実行する。
   - 集計完了後、`st->initialUsageMutex` でロックして `st->pendingUsageFileCache` / `st->pendingAttemptEventsPath` に結果をムーブ格納し、`PostMessageW(hwnd, WM_PC_INITIAL_TASK_USAGE, 0, 0)` で完了シグナルを送る。

4. **第 3 段階（UI スレッド・データマージと再描画）**:
   - `WM_PC_INITIAL_TASK_USAGE` ハンドラで `st->initialUsageMutex` をロックし、`pendingUsageFileCache` / `pendingAttemptEventsPath` を `st->taskUsageFileCache` / `st->taskAttemptEventsPath` にマージ（`insert` / 上書き）する。
   - `st->initialTaskUsagePending = false` に戻し、`RefreshTaskQueueData(st, /*force=*/true, /*useCachedList=*/!st->lastTaskListJson.empty())`（または `RefreshDashboardData(st)`）を呼び出して usage / コスト列を埋める。
   - 既にキャッシュにファイル末尾の `offset` が記録されているため、UI スレッド側の再計算ではファイル I/O や JSON パースは発生せず、メモリ上のキャッシュ参照のみで即座に完了する。

### 3. スレッドのライフサイクル管理とメモリ安全性

- **なぜ `LPARAM` で生ポインタを渡さないか（リーク防止）**:
  - `PostMessageW` が成功してメッセージキューに積まれた直後、そのメッセージがディスパッチされる前にウィンドウが破棄（`WM_DESTROY`）されると、未処理メッセージの `LPARAM` に渡したヒープポインタは解放されずメモリリークする。
  - これを防ぐため、集計結果は `DashboardState` のメンバ変数（`pendingUsageFileCache` 等）に保持し、`PostMessageW` は引数 `(0, 0)` の完了通知としてのみ使用する。これにより `DashboardState` の破棄に伴って確実にメモリが解放される。
- **スレッドの保持と終了同期**:
  - `DashboardState` に `std::thread initialUsageThread` と中断フラグ `std::atomic<bool> cancelInitialUsage{false}` を持たせる。
  - `WM_DESTROY` ハンドラで `st->cancelInitialUsage = true` を設定し、`if (st->initialUsageThread.joinable()) st->initialUsageThread.join();` で確実にスレッド終了を待機してから破棄処理を進める。
  - スレッド内のジョブループでは 1 件ごとに `cancelInitialUsage.load()` を確認し、`true` なら直ちに中断して終了する。

### 4. 競合防止とフェイルセーフ

- **タイマーリフレッシュとの競合抑止**:
  - `st->initialTaskUsagePending` が `true` の間は、通常の定期 refresh tick（2 秒）や IPC イベント起因のリフレッシュでも `deferUsage = true` となり、UI スレッドでの二重読み込みを防止する。
- **例外保護とタイムアウト（フェイルセーフ）**:
  - ワーカースレッド全体を `try ... catch (...)` で囲み、予期せぬ例外が発生した場合でも `PostMessageW(hwnd, WM_PC_INITIAL_TASK_USAGE, 0, 0)` を送って `initialTaskUsagePending` が解除されるようにする。
  - さらに UI 側の `RefreshDashboardData` において、スレッド起動時刻（`initialUsageStartTickMs`）から一定時間（例: 15 秒）経過しても `initialTaskUsagePending` が `true` の場合は強制的に `false` にリセットする。

## 変更対象

| 箇所 | 内容 |
|---|---|
| メッセージ定数（`:73` 付近） | `WM_PC_INITIAL_REFRESH = WM_APP + 105`<br>`WM_PC_INITIAL_TASK_USAGE = WM_APP + 106` を追加 |
| `DashboardState`（`:978` 付近） | `bool initialTaskUsagePending = false;`<br>`uint64_t initialUsageStartTickMs = 0;`<br>`std::atomic<bool> cancelInitialUsage{false};`<br>`std::thread initialUsageThread;`<br>`std::mutex initialUsageMutex;`<br>`std::map<std::wstring, TaskUsageFileCache> pendingUsageFileCache;`<br>`std::map<std::wstring, std::wstring> pendingAttemptEventsPath;` を追加 |
| `:6541`, `:6632` | `TaskUsageAccumulateFile` / `TaskUsageAccumulateStreamFile` の第 1 引数を `std::map<std::wstring, TaskUsageFileCache>&` に変更 |
| `:6696` | `TaskOwnerUsageForJob` の引数をローカルキャッシュ参照＋`bool deferUsage = false` に変更 |
| `:7023` (`RefreshTaskQueueData`) | `TaskOwnerUsageForJob` 呼び出しに `st->taskUsageFileCache, st->taskAttemptEventsPath, ..., st->initialTaskUsagePending` を渡す |
| `:7398` (`RefreshDashboardData`) | `initialTaskUsagePending` のタイムアウト判定（例: 15 秒超過で強制解除）を追加 |
| `:20804` (`WM_CREATE`) | `RefreshDashboardData(st)` を `st->initialTaskUsagePending = true; PostMessageW(hwnd, WM_PC_INITIAL_REFRESH, 0, 0);` に置き換え |
| `DashboardWndProc` | `WM_PC_INITIAL_REFRESH`：初回描画とワーカースレッド起動（`ProcessOnly` 等でリストが空なら即 `initialTaskUsagePending = false`）<br>`WM_PC_INITIAL_TASK_USAGE`：キャッシュのマージ、`initialTaskUsagePending = false`、タスクリスト再更新<br>`WM_DESTROY`（`:21764`）：`cancelInitialUsage = true` と `initialUsageThread.join()` を追加 |

## 検証項目

- **ビルド確認**:
  - `cmake --build build --config Release --target Hancho` がエラーなく通ること。
- **基本機能確認**:
  - タスクを多数登録した状態で `Hancho.exe gui` を起動し、ウィンドウが即座に表示され、後から usage / コスト列が埋まること。
  - 通常の refresh tick（2 秒）でタスク一覧が引き続き正常に更新されること。
- **エッジケース・異常系確認**:
  - **即時終了テスト**: 起動直後（usage 列が埋まる前）にウィンドウを閉じても、クラッシュ（`std::terminate`）やメモリリーク、ハングが発生しないこと。
  - **表示モード別テスト**: `ListDisplayMode::ProcessOnly`（プロセスのみ表示）の状態で起動し、後からタスク表示に切り替えた際に usage 列が正常に表示されること。
  - **タスク 0 件テスト**: ジョブが 0 件の状態で起動しても `initialTaskUsagePending` が正しく解除されること。
  - **実行中更新テスト**: ワーカースレッド実行中に新しいタスクが追加された場合でも、スレッド完了後に最新のリストが正しく表示されること。
