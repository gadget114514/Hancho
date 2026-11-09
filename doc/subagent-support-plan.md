# Claude サブエージェント対応プラン

Hancho 経由で Claude Code のサブエージェントを追跡・活用できるようにする。

## 確認済みの stream-json の形式（Claude Code 2.1.314）

- サブエージェントの `assistant` / `user` 行: `parent_tool_use_id`, `agent_id`, `subagent_type`, `task_description` が付く。
- `system` / `task_started`: `task_id`, `tool_use_id`, `description`, `subagent_type`, `is_backgrounded`, `spawn_depth`, `task_type`。
- `system` / `task_notification`: `task_id`, `tool_use_id`, `status`, `summary`, `usage` (`total_tokens`, `tool_uses`, `duration_ms`)。
- `task_id` と `agent_id` は同じ値、`tool_use_id` と `parent_tool_use_id` も同じ値。
- 1 回の実行で `init` と `result` が複数回出ることがある。後の `result` は `origin.kind = task-notification`。
- `result.subagent_stats`: `spawned`, `completed`, `failed` など。`total_cost_usd` は累積。

## 段階 1: 見える化（C++）

対象: `src/llm_stream.h`, `src/llm_stream.cpp`, `src/llm_stream_claude.cpp`, `src/gui_dashboard.cpp`, `src/process_manager.*`, `src/cli.cpp`

1. `LlmStreamEvent` に `toolUseId`, `parentToolUseId`, `agentId`, `subagentType`, `taskDescription`, `taskStatus` を追加し、Claude パーサーで埋める。
2. `system` の `task_started` / `task_notification` を新しいイベント種別 `subagent_start` / `subagent_end` にする。`tool_use_id` の `tool_use` と `tool_result` も対応付ける。
3. `LlmSubagentState`（id, 種類, 説明, 状態, 開始・終了時刻, トークン数, ツール回数, 現在の活動）を追加し、`LlmAgentState` に一覧として持たせる。
4. `parentToolUseId` がある行はサブエージェントの状態だけを更新する。親の `status`, `activity`, `currentTool`, `lastMessage`, `lastThought`, `toolCalls` は上書きしない。
5. トークンの基準値リセットを、直前の 1 件との比較から `message.id` ごとの記録に変える。別エージェントの行が挟まっても二重に数えない。`kMaxEvents` と同様に、記録の増え方に上限を設ける。
6. `result` を受けても、未完了のサブエージェントが残っている間は `completed` / `done` にしない。プロセス終了時の扱いは既存の処理に従う。
7. `LlmEventsToJson` に `subagents` 配列と各イベントの親子フィールドを追加し、`hancho-cli agent status` / `stream` で見えるようにする。
8. GUI のモニターペインにサブエージェント一覧を追加する。UI 文字列は既存の `Tr(日本語, 英語, スペイン語)` を使う。
9. `tests/llm_stream_smoke.cpp` を追加し、CMake に登録する。`tools/ww/samples/claude-subagent-events.sample.jsonl`（個人情報を除いた最小のサンプル）を読み、親の状態が上書きされないこと、一覧の開始から完了、二重計上しないこと、未完了での `result` を確認する。

## 段階 1b: モニターペインのツリー表示（段階 1 の後に別途実装）

既存の ListView を流用し、インデントした行で木を表す。列（時刻、トークン数など）と色分けはそのまま使う。TreeView には切り替えない。

### 表示のルール

- 実行中の枝: 末端の活動まで開いて表示する。並列なら複数の枝が同時に開く。
- 完了した枝: 完了から 3 秒後に 1 行の要約（説明、所要時間、トークン数、ツール回数）に閉じる。
- 失敗した枝（`completed` 以外で終わった枝）: 閉じずに開いたままにする。
- 手動で開閉した枝: 自動開閉の対象から外す。
- 親が閉じている枝は、子孫の状態によらず表示しない。ただし実行中か失敗の子孫がある枝は自動では閉じない。
- 枝の中で連続する同じ種類の完了済みツール行は `ファイル読み込み ×6` のようにまとめる（thinking / replying は既存どおり）。
- 要約は `LlmSubagentState` の集計から作る。`kMaxEvents` で古いイベントが消えても残るようにする。イベントが消えた枝も要約行は表示する。
- サブエージェントがない実行の表示は、順序も含めて今と完全に同じにする（新しい行が上）。サブエージェントがある実行は時系列（古い行が上）で並べる。

### 実装

1. GUI に依存しない `src/activity_tree.h` / `src/activity_tree.cpp` を追加する。`BuildActivityTree(events, subagents, nowMs, running, overrides)` が表示行の配列を返す純粋関数。各行は深さ、種類、エージェント id、枝か活動か、開閉状態、対応する `ActivityRow` 相当の値を持つ。
2. `ds->rows` はフラットのまま残す。時間チャート（`gui_dashboard.cpp` の `KindRow` 集計）は `ds->rows` を使い続け、集計が変わらないようにする。リスト用に `ds->treeRows` を別に持つ。
3. リストの再構築（`gui_dashboard.cpp:4092` 付近）を `treeRows` から行う。クリック、選択、コピー（`ActivityRowCopyText`）、詳細表示の行番号の対応付けも合わせて直す。サブエージェントなしの逆順表示を壊さない。
4. 再構築の条件に、表示行の開閉状態の変化を加える。イベントが来なくても 3 秒後に閉じるようにするため。再構築で選択行とスクロール位置を保つ。
5. 活動列の先頭にインデントと `▶` / `▼` を描き、そのクリックで開閉する。キーボードの左右キーでも開閉できるようにする。手動の開閉状態はエージェント id ごとに `LlmStreamDlgState` に持つ。監視プロセスが変わったら消す。
6. 実行中の行を見えるようにスクロールする。ただし利用者がスクロールした後は、最下部へ戻るまで自動スクロールしない。
7. 段階 1 で追加した `SubagentsText` の一覧は、ツリーと重複するので撤去する。
8. `tests/llm_stream_smoke.cpp`（または新しい `tests/activity_tree_smoke.cpp`）で `BuildActivityTree` を試す: 実行中は末端まで開く、完了 3 秒前後で閉じる、失敗は開いたまま、手動の開閉が優先される、入れ子と並列、イベントが消えた枝の要約、同種の行のまとめ、サブエージェントなしは今の並びと同じ。`nowMs` を引数で渡して時刻を固定する。
9. `doc/features.md` / `doc/features-ja.md` に動きを書く。

## イベント保持件数の設定

- `kMaxEvents` の固定値 500 をやめ、既定 2000 の設定にする。範囲は 100〜100000 で、範囲外は丸める。`-1` は無制限で、メモリが枯渇するまで保持する。
- 無制限でメモリを確保できない場合（`std::bad_alloc`）は、プロセス全体を落とさず、古いイベントの半分を捨てて続行し、ログに 1 回記録する。設定欄の説明に、`-1` はメモリを使い切る恐れがあることを書く。
- 設定ダイアログに数値欄を追加し、`restoreUsageGraph` と同じ経路で保存、復元、適用する（`SetUsageHistoryPersistence` と同様の `SetLlmMaxEvents` / `GetLlmMaxEvents`）。実行中のプロセスにも適用し、減らした場合は次のイベントで古い分を捨てる。
- `doc/features.md` / `doc/features-ja.md` に書く。テストで上限と丸めを確認する。

## 段階 2: 委譲（エージェント定義）

1. `tools/agents/hancho-runner.md` を追加する。ビルド、テスト、サーバーなど長時間の処理を MCP の `start_process` と `wait_process` / `get_process_output` で実行し、要約だけを返す Claude Code エージェント定義。
2. `hancho-cli agent install-template [--path <dir>] [--dry-run] [--json]` を追加する。既定で `.claude/agents/hancho-runner.md` に書き、既存ファイルは上書きしない。テンプレートは exe に埋め込む。
3. CLI のヘルプと JSON スキーマ（`src/cli.cpp`）を更新する。

## 段階 3: 並列実行（`ww-run` / タスクキュー）

1. `ww-run` / `ww-watch` に `-Agent <name>`（環境変数 `WW_AGENT`）を追加する。`claude` エンジンの時だけ `--agent` を渡し、他のエンジンではエラーにする。
2. `-Jobs` で生成するジョブにも引き継ぐ（`New-HanchoCliJob`）。
3. `ww-run.selftest.ps1` にテストを追加する。

## ドキュメント

`doc/features.md`, `doc/features-ja.md`, `doc/cli.md`, `doc/cli-ja.md`, `doc/LLM-jsonstream-format.md`, `doc/pros-cons-comparison.md`, `tools/ww/README-ww.md` に反映する。英語版と日本語版の両方を更新する。

## 完了条件

- `Hancho`, `hancho-cli`, `taskqueue_smoke`, `llm_stream_smoke` が Release でビルドできる。
- `llm_stream_smoke`, `taskqueue_smoke`, `tools/ww/ww-run.selftest.ps1`, `tools/ww/tests/ww-watch.tests.ps1` が成功する。
- 既存の非 Claude エージェントの解析結果が変わらない。

## 未確認の項目

- 長時間のサブエージェントで、完了前に親の `result` が出るか。
- MCP で `taskqueue_*` が見えない環境がある原因（ソースには定義がある）。
- 対話（PTY）モードの追跡は対象外。
