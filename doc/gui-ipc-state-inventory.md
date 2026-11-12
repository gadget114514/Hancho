# GUI が直接参照するコア状態の目録と IPC 要求一覧

本ドキュメントは、Hancho v3（`doc/design-v3.md` §2.3 段階 A、親カード `ww-data/tasks/060-gui-ipc-client.md`）において、Windows Direct2D GUI（`src/gui_dashboard.cpp`）をコア（`Hancho.exe --headless`）から完全に分離し、独立した IPC クライアントとして動作させるために必要なコア状態の直接参照の目録と、対応する IPC 要求の一覧です。

---

## 1. 状態の目録と IPC マッピング

GUI が従来直接参照していたコアの状態は、以下の 6 つのカテゴリに分類されます。それぞれの直接参照箇所と、対応する IPC 要求（既存および新設）を定義します。

### 1.1 プロセス一覧 (Process List)

GUI の上部ペイン（プロセス一覧）、プロセス詳細ダイアログ、およびプロセス制御機能が参照する状態です。

| GUI の直接参照箇所 (`gui_dashboard.cpp`) | 操作内容 | 従来の直接参照 | 対応する IPC 要求 | 状態 |
|---|---|---|---|---|
| `RefreshDashboardData` (L4303, L4345) | 全プロセス一覧・状態取得 | `pm->GetAllSnapshots()` | `{"action": "list"}` | 既存 |
| `ShowProcessDetailsDialog` (L5280, L7965) | 単一プロセスの詳細取得 | `pm->GetSnapshot(id, snap)` | `{"action": "list", "id": <id>}` | 既存 |
| `LaunchTerminalProcess` (L8418) | 端末プロセスの起動 | `pm->StartProcess(req, err)` | `{"action": "start", ...}` | 既存 |
| `KillProcessWithConfirm` (L8107, L11133) | プロセスツリーの強制終了 | `pm->KillProcess(id, err)` | `{"action": "kill", "id": <id>}` | 既存 |
| `IDC_BTN_KILL_ALL` (L11141) | 全稼働中プロセスの停止 | `pm->KillAllRunning()` | `{"action": "kill_all"}` | 既存 |
| `IDC_BTN_CLEAR` (L8114, L11148) | 終了済みプロセスの消去 | `pm->ClearFinished()` | `{"action": "clear"}` | 既存 |
| `IDM_PROC_REMOVE` (L8111) | プロセス行の削除 | `pm->RemoveProcess(id)` | `{"action": "remove", "id": <id>}`<br>(別名: `process_remove`) | **新設** |
| `ResetProcessIdsWithConfirm` (L8820) | プロセス ID の振り直し | `pm->ResetProcessIds(err)` | `{"action": "reset_ids"}`<br>(別名: `process_reset_ids`) | **新設** |
| `ShowProcessDetailsDialog` (L5308) | プロセス標準出力の取得 | `pm->GetOutput(id, ...)` | `{"action": "logs", "id": <id>, "tail": 200}` | 既存 |
| 端末入力送信 | 入力文字の標準入力への流し込み | `pm->SendInput(id, input)` | `{"action": "send", "id": <id>, "input": "..."}` | 既存 |

---

### 1.2 タスクキュー (Task Queue)

GUI の下部ペイン（タスクキュー一覧）、キュー要約 KPI カード、およびタスク操作メニューが参照する状態です。

| GUI の直接参照箇所 (`gui_dashboard.cpp`) | 操作内容 | 従来の直接参照 | 対応する IPC 要求 | 状態 |
|---|---|---|---|---|
| `RefreshDashboardData` (L1592, L4289) | キュー状態別件数の集計 (running/waiting/pending/blocked/succeeded/failed/cancelled/total) | `tqm->GetJobCounts()` | `{"action": "taskqueue_counts"}` | **新設** |
| `ApplyShellConfig` (L8222) / 設定ダイアログ (L8924) | リソース競合ロックの有効/無効設定・参照 | `tqm->SetResourceLocksEnabled()`<br>`tqm->GetResourceLocksEnabled()` | `{"action": "taskqueue_resource_locks", "enabled": <bool>}` | **新設** |
| `RefreshDashboardData` (L3911) | キュー内のジョブ一覧取得 | `tqm->TaskQueueList("", "", 0, err)` | `{"action": "taskqueue_list", "taskqueue": "", "state": ""}` | 既存 |
| `ShowTaskDetails` (L3255, L3834) | ジョブ詳細・指示文取得 | `tqm->TaskQueueGet(jobId, ...)` | `{"action": "taskqueue_get", "id": "<jobId>"}` | 既存 |
| `IDM_TASK_CANCEL` (L3874, L3879) | ジョブの取り消し | `tqm->TaskQueueCancel(jobId, err)` | `{"action": "taskqueue_cancel", "id": "<jobId>"}` | 既存 |
| `IDM_TASK_RETRY` (L3877) | ジョブの手動再試行 | `tqm->TaskQueueRetry(jobId, ...)` | `{"action": "taskqueue_retry", "id": "<jobId>"}` | 既存 |
| `IDM_TASK_PAUSE` / `RESUME` (L3883) | キュー全体の一時停止 / 再開 | `tqm->TaskQueuePause(tq, paused, err)` | `{"action": "taskqueue_pause", "taskqueue": "<name>"}`<br>`{"action": "taskqueue_resume", ...}` | 既存 |
| `IDM_TASK_PURGE` (L3893) | 終了済みジョブのパージ | `tqm->TaskQueuePurge(tq, states, err)` | `{"action": "taskqueue_purge", "taskqueue": "<name>", "states": [...]}` | 既存 |

---

### 1.3 監視ペイン (Monitor Pane / Agent Stream)

GUI 下部の監視ペイン（`LlmStreamDlgState`）が参照する、LLM のツール呼び出し・思考・進捗・使用量ストリームです。

| GUI の直接参照箇所 (`gui_dashboard.cpp`) | 操作内容 | 従来の直接参照 | 対応する IPC 要求 | 状態 |
|---|---|---|---|---|
| `ReloadStreamDialogData` (L7234) | 正規化イベントストリーム (`hancho.event/1`) 取得 | `pm->GetAgentStream(id, ...)` / イベントファイル直接読み込み | `{"action": "get_agent_stream", "id": <id>, "schema": "v1"}` | 既存 |
| `ReloadStreamDialogData` (L7234) | エージェント状態 (`LlmAgentState`) 取得 | `pm->GetAgentStream(id, ...)` / `ProcessSnapshot` | `{"action": "get_agent_state", "id": <id>}` | 既存 |
| スーパーバイザー目標・メモ更新 | メモと目標の記録 | `pm->UpdateAgentInfo(id, ...)` | `{"action": "update_agent_info", "id": <id>, "notes": "...", "goal": "..."}` | 既存 |
| 使用量推移・日次ロールアップ | トークンと費用の集計グラフ | `QueryUsageStats(...)` / `GetUsageHistory(...)` | `{"action": "get_usage_history", "since_sec": 86400}`<br>`{"action": "get_usage_daily", ...}` | 既存 |

---

### 1.4 失敗診断 (Failure Diagnosis)

ジョブやプロセスの停止理由「なぜ失敗したか（Why did it fail）」の診断・証拠・分類の表示機能です。

| GUI の直接参照箇所 (`gui_dashboard.cpp`) | 操作内容 | 従来の直接参照 | 対応する IPC 要求 | 状態 |
|---|---|---|---|---|
| `FailureForProcess` (L5254) | プロセス ID に紐づく試行の失敗診断の取得 | 全タスクキューのジョブ一覧を走査し `attempt.process_id == procId` を探索 | `{"action": "taskqueue_diagnose", "process_id": <procId>}`<br>(別名: `failure_diagnose`) | **新設** |
| `WhyLoad` (L3007) | ジョブの最新試行の失敗診断の取得 | `tqm->TaskQueueGet` の `attempts[].failure` を抽出 | `{"action": "taskqueue_diagnose", "id": "<jobId>"}` | **新設** |
| `QueueFailureBreakdown` (L2469) | キュー内の失敗分類集計（`exit_nonzero ×2` 等） | `tqm->TaskQueueList(queue, "failed")` を全件解析 | `{"action": "taskqueue_diagnose", "taskqueue": "<name>", "breakdown": true}` | **新設** |

---

### 1.5 メモリ一覧 (Memory List / Memory Dialogs)

タスクトレイ「メモリ一覧」ダイアログおよびジョブ詳細「過去の実行」タブが参照する、L1 実行記録・L2 ノート・L3 知識・容量制限・注入効果比較です。

| GUI の直接参照箇所 (`gui_dashboard.cpp`) | 操作内容 | 従来の直接参照 | 対応する IPC 要求 | 状態 |
|---|---|---|---|---|
| `MemoryListReload` (L3428, L3432) | メモリレイヤー別検索 (L1/L2/L3) | `memory::SearchJson(query)` | `{"action": "memory_search", "text": "...", "kinds": [...]}` | **新設** |
| `MemoryListWndProc` (L3646) | 個別レコードのドキュメント取得 | `memory::GetJson(id, kind)` | `{"action": "memory_get", "id": "<id>", "kind": "<kind>"}` | **新設** |
| `PastRunsLayout` / ジョブ統計 (L2922) | ジョブ別の実行・失敗統計 | `memory::StatsJson(query)` | `{"action": "memory_stats", "job_name": "..."}` | **新設** |
| `MemoryListReload` (L3448, L4322) | メモリ容量統計・上限警告判定 | `pc::memory::GetMemoryStorageStats()` | `{"action": "memory_storage_stats"}` | **新設** |
| `RetractMemoryNoteById` (L2738, L3743) | L2 ノートの取り消し | `memory/notes.jsonl` へ `op: retract` を追記 | `{"action": "memory_retract", "id": "<id>", "reason": "..."}` | **新設** |
| `PromoteMemoryNoteToL3` (L2752, L3743) | L2 ノートの L3 知識への昇格 | `knowledge.jsonl` 追記および `notes.jsonl` 更新 | `{"action": "memory_promote", "id": "<id>", "approved_by": "gui:human"}` | **新設** |
| `MemoryInjectCompare` (L3679) | メモリ注入の有無による成功率・費用差分比較 | `MemoryInjectCompareJson("", "", "")` | `{"action": "memory_compare", "job": "...", "taskqueue": "..."}`<br>(別名: `memory_inject_compare`) | **新設** |
| メモリ古年月 L1 削除 | 古い L1 ファイルの整理 | `memory::PruneRunRecordsJson(...)` | `{"action": "memory_prune", "dry_run": <bool>, "retention_days": 90}` | **新設** |

---

### 1.6 テーマ以外の設定 (Non-theme Settings)

GUI の設定ダイアログ「動作」「保守」タブ、およびシェル起動設定が参照・変更する、コアの実行時設定です。

| GUI の直接参照箇所 (`gui_dashboard.cpp`) | 設定項目 | 従来の直接参照 | 対応する IPC 要求 | 状態 |
|---|---|---|---|---|
| `LoadShellConfig` (L8173) | 設定全般の読み込み | `<data-dir>/logs/config.json` を直接ファイル読み出し | `{"action": "config_get"}` | **新設** |
| `SaveShellConfig` (L8295) | 設定全般の保存 | `<data-dir>/logs/config.json` を直接ファイル書き込み | `{"action": "config_set", "config": {...}}`<br>(または `"patch": {...}`) | **新設** |
| `ApplyShellConfig` (L8220) | イベント保持上限 (`llm_max_events`) | `SetLlmMaxEvents(val)` | `config_set` で自動反映 | **新設** |
| `ApplyShellConfig` (L8222) | リソース競合ロック (`resource_locks`) | `tqm->SetResourceLocksEnabled(val)` | `config_set` で自動反映 | **新設** |
| `ApplyShellConfig` (L8218) | 使用量履歴の永続化 (`restore_usage_graph`) | `SetUsageHistoryPersistence(val)` | `config_set` で自動反映 | **新設** |
| `ShellConfigDlgState` (L8454-8458) | 既定シェルとコマンド (`default_shell`, `cmd_command`, etc.) | `config.json` の各フィールド | `config_get` / `config_set` | **新設** |

---

## 2. 完了条件の確認 (直接参照する状態の目録の空化)

上記の通り、GUI が直接コアの C++ ポインタ（`st->pm`, `st->tqm`, `btm`）やデータディレクトリ内のファイル（`config.json`, `notes.jsonl`, `knowledge.jsonl` 等）を読み書きしていた箇所は、**すべて Named Pipe 越しの IPC 要求に置き換えることが可能**になりました。

これにより、次段階（親カード 060 の GUI IPC クライアント化）において、GUI の `DashboardState` から `ProcessManager* pm` および `TaskQueueManager* tqm` の直接参照を安全に削除し、すべて `SendIpcRequest` 経由の非同期/同期通信に移行できる前提が整いました。
