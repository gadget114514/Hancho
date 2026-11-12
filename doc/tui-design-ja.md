# TUI（hancho-cli top）方式設計

**[English](tui-design.md) | [日本語](tui-design-ja.md)**

Linux / macOS / Windows 向けの端末ダッシュボード（`doc/design-v3-todo.md` §8、親カード `ww-data/tasks/062-linux-tui.md`、子カード `062-1-tui-design.md`）として提供する **TUI（`hancho-cli top`）** の実装方式の決定、アーキテクチャ、更新ループとキー入力の骨格、画面設計、およびテスト容易性の設計方針を定めます。

---

## 1. 目的と位置づけ

Hancho v3 の Linux / macOS 対応方針（`doc/design-v3-todo.md` §8）では、GUI（ブラウザ UI: [browser-ui-ja.md](browser-ui-ja.md)）と TUI（`hancho-cli top`）の**両方を提供する方針**を採用しました。

`hancho-cli top` は、初期の単純なプロセス一覧のポーリング表示から発展させ、Windows 版 Direct2D ダッシュボードと同等の主要情報（プロセス一覧、タスクキュー、監視ストリーム、失敗診断、メモリ一覧）を端末から直接閲覧・操作できる対話型 TUI ダッシュボードとして育てます。

```
+-------------------------------------------------------------------+
|               hancho-cli top (Terminal Dashboard)                 |
|  - Tabs: [1: Processes] [2: Queue] [3: Monitor] [4: Why] [5: Memory] |
|  - Key Nav: Tab, 1-5, Up/Down/j/k, r:Refresh, ?:Help, q:Quit      |
+---------------------------------+---------------------------------+
                                  |
              Unix Domain Socket (POSIX) / Named Pipe (Windows)
              IPC JSON Requests ({"action": "list"}, etc.)
                                  |
+---------------------------------v---------------------------------+
|               Hancho Core Process (Hancho --headless)             |
|  - TaskQueue, ProcessManager, Agent Stream, Memory                |
+-------------------------------------------------------------------+
```

---

## 2. 実装方法の選定と決定

TUI の実装方法について、以下の 3 つのアプローチを比較検討しました。

### 2.1 候補の比較

| 方式 | 利点 | 課題 / 欠点 | Hancho への適合性 |
|---|---|---|---|
| **A. ncurses / curses** | - POSIX 環境における歴史的標準<br>- ウィンドウや行管理の既存機能が豊富 | - Windows でネイティブに動かない（PDCurses や Cygwin 等の別移植が必要）<br>- Linux でも `libncurses-dev` / `libtinfo` 等の外部動的ライブラリに依存し、Hancho の「単一バイナリ・外部依存なし」の配布方針に反する<br>- MSVC スタティックリンク構成が極めて煩雑 | **不採用** |
| **B. 外部 C++ TUI ライブラリ**<br>*(FTXUI, notcurses 等)* | - モダンな C++ インターフェース<br>- コンポーネント指向・宣言型 UI | - CMake 構成や外部依存（サードパーティ・サブモジュール）が増大する<br>- バイナリサイズが肥大化する<br>- 枯れていないライブラリの場合、プラットフォームごとの非互換やビルド破損リスクがある | **不採用** |
| **C. 自前 ANSI/VT100 エスケープシーケンス + プラットフォーム抽象化** | - **外部依存ゼロ**（標準 C++20 + OS 標準 API のみ）<br>- Windows（Virtual Terminal Processing）と POSIX（termios / ioctl）で共通の ANSI コードが動作<br>- 出力成型（Renderer）が純粋関数・文字列バッファとして分離でき、**単体テストが極めて容易**<br>- 起動が極めて高速でバイナリサイズ増加が最小限 | **採用** |

### 2.2 決定: 自前 ANSI/VT100 エスケープ方式の採用

**決定**: Hancho の `hancho-cli top` は、外部ライブラリを追加せず、**標準 ANSI/VT100 エスケープシーケンスとプラットフォーム端末制御を自前で実装（C 方式）** します。

#### 決定理由

1. **単一実行可能ファイルの配布哲学（Zero Dependencies）**:
   Hancho は、Windows 上で MSVC ランタイムを静的リンクし、Linux/macOS 上でも外部ライブラリを極限まで排除して libc / Threads::Threads のみで動作するポータブルな単一バイナリを目指しています。ncurses や外部フレームワークを導入すると、パッケージマネージャ（apt / brew）での前提パッケージ導入が必要となり、CLI の手軽さが失われます。
2. **現代の端末の VT100 / xterm 互換性**:
   現代の Linux 端末エミュレータ、macOS Terminal/iTerm2、および Windows 10/11（Windows Terminal、ConHost の `ENABLE_VIRTUAL_TERMINAL_PROCESSING`）はすべて ANSI VT100 / xterm エスケープシーケンスを完全サポートしています。 curses のような端末特性データベース（terminfo/termcap）の抽象化層はもはや必須ではありません。
3. **描画ロジックの高いテスト容易性（Decoupled Rendering）**:
   描画エンジンを「状態（`TopState`）を受け取り、画面文字列（ANSI エスケープを含む `std::string`）を返す純粋関数」として設計することで、対話端末がない CI やスモークテスト（`tests/`）環境でも、レイアウト成型、タブ切り替え、プロセス・キューのフォーマットを精密に単体テストできます（親カード 062d の要求「出力の成型部分を単体テストできる構成にする」を満たす）。
4. **段階的・堅牢な拡張性**:
   カード 062-1（設計・骨格）から、062-2（一覧・監視・診断・メモリ表示）、062-3（操作・リモート）、062-4（検証・スモークテスト）へと、コードベースの肥大化なしに確実に積み上げることができます。

---

## 3. アーキテクチャとコンポーネント構成

`src/cli_top.h` を中心に、以下のレイヤーで構成します。

```
+-------------------------------------------------------------------+
|                        Top Controller                             |
|  - RunTopLoop / RunTopCommand: 更新タイマーとイベントループ管理    |
+-------------------+-----------------------------------------------+
                    |
      +-------------+-------------+
      |                           |
+-----v-------------+       +-----v---------------------------------+
|  TerminalSession  |       |              TopState                 |
|  - Raw / cbreak   |       |  - activeTab, selectedRow, showHelp   |
|  - Alt Screen     |       |  - processData, queueData, etc.       |
|  - PollKey(ms)    |       +-------------------+-------------------+
+-------------------+                           |
                                                |
                            +-------------------v-------------------+
                            |              Renderer                 |
                            |  - RenderHeader / RenderTabs          |
                            |  - RenderViewContent (5 Views)        |
                            |  - RenderHelpOverlay / RenderFooter   |
                            |  -> std::string (Pure Buffer)         |
                            +---------------------------------------+
```

### 3.1 端末生モードと代替画面バッファ (`TerminalSession` / RAII Guard)

端末の対話モードでは、RAII クラス `TerminalSession`（または `TerminalRawGuard`）が以下の制御を担います:

- **代替画面バッファの利用**:
  - 開始時: `\x1b[?1049h`（代替バッファへ切替） + `\x1b[?25l`（カーソル非表示）
  - 終了時: `\x1b[?25h`（カーソル再表示） + `\x1b[?1049l`（通常バッファへ復帰）
  - これにより、`top` 終了後も直前のシェル履歴が汚されず復元されます。
- **生モード（Raw / cbreak mode）の切替**:
  - **POSIX**: `tcgetattr` で元の設定を退避し、`ECHO | ICANON | ISIG | IEXTEN` を無効化、`VMIN=0, VTIME=0` で非ブロッキング読み出し可能にします。終了時およびシグナル受信時に `tcsetattr` で元に戻します。
  - **Windows**: `GetConsoleMode` / `SetConsoleMode` を使用し、出力では `ENABLE_VIRTUAL_TERMINAL_PROCESSING` を有効化、入力では `ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT` を解除します。
- **非対話モードとの両立**:
  - `--once` や標準出力がパイプにリダイレクトされている場合（`!isatty`）は、生モードや代替バッファへの切り替えを行わず、標準出力に 1 回だけ整形文字列を出力して終了します。

### 3.2 キー入力とイベントモデル (`TopKey` / `PollKey`)

キー入力は、指定タイムアウト（例: 既定 1000ms）付きの非ブロッキング ポーリングで読み取ります。

- **POSIX**: `poll(STDIN_FILENO, POLLIN, timeoutMs)`。エスケープシーケンス（`\x1b[A` 等の矢印キー、`\x1b[Z` の Shift+Tab 等）をステートマシンで解析。
- **Windows**: 短いスライス待機ループ（`Sleep`）と `_kbhit()` / `_getch()`（2 バイトキーコード）による解析。
- **キー割り当て**:
  - `q`, `Q`, `Ctrl+C`: 終了
  - `r`, `R`: 即座に強制再描画・IPC 再取得
  - `Tab`, `Shift+Tab`: タブ（画面）の巡回切り替え
  - `1`, `2`, `3`, `4`, `5`: 各タブへの直接切り替え
  - `Up`, `Down`, `k`, `j`: 一覧行の選択カーソル移動
  - `?`, `h`: ヘルプオーバーレイの表示 / 非表示
  - `Escape`: ヘルプが開いている場合はヘルプを閉じる

### 3.3 画面構成と 5 つのペイン (Views)

画面レイアウトは端末サイズ（Windows: `GetConsoleScreenBufferInfo`、POSIX: `ioctl(TIOCGWINSZ)`）に応じて動的に適応します。

```
Hancho Top  |  2026-11-11 19:10:00  |  Core: ONLINE (pid 12345)  |  Interval: 1000ms
 [1: Processes]   [2: Queue]   [3: Monitor]   [4: Why]   [5: Memory] 
------------------------------------------------------------------------------------
 ID   PID    STATE             ELAPSED     CPU       LAST LINE
------------------------------------------------------------------------------------
>  1  12350  running           00:01:23    2.4s      cargo build --release
   2  12351  idle              00:05:10    0.8s      waiting for input
------------------------------------------------------------------------------------
[Tab] Switch Tab   [1-5] View   [↑↓/jk] Select   [r] Refresh   [?] Help   [q] Quit
```

1. **Processes (プロセス一覧)**: 稼働中および過去プロセスの PID、状態、経過時間、CPU、直近出力行を表示。
2. **Queue (タスクキュー)**: キューの状態別サマリー（Running, Waiting, Pending, Succeeded, Failed）およびジョブ一覧。
3. **Monitor (監視ストリーム)**: LLM エージェントストリーム、思考プロセス、ツール呼び出しログ。
4. **Why (失敗診断)**: `taskqueue why` と同様の失敗分類、原因要約、直前のアクション。
5. **Memory (メモリ一覧)**: L1/L2/L3 ノートおよび検索統計。

---

## 4. 開発ロードマップ (062 サブタスクとの対応)

- **062-1**: 実装方式の決定文書化、`src/cli_top.h` による画面更新ループ・キー入力の骨格、`--once` / `--interval` / `--json` 互換維持、単体スモークテスト。
- **062-2**: プロセス一覧、タスクキュー、監視ストリーム、失敗診断、メモリ一覧の 5 画面の本実装。
- **062-3**: 端末からのジョブ操作（停止、再試行、取り消し）とリモートホストのジョブ表示。
- **062-4 (本カード)**: POSIX 環境での確認手順文書化と成型スモークテストの網羅。

---

## 5. POSIX 環境での動作確認手順 (Verification Procedure)

Linux / macOS の端末環境において、`hancho-cli top` の対話操作、非対話モード、パイプ連携、およびシグナル終了時の復帰を検証するための標準確認手順です。

### 5.1 前提条件と動作確認環境

- **OS**: Linux (Ubuntu 20.04+, Debian 11+, RHEL/Rocky 8+, Alpine 3.16+ 等) または macOS (11.0 Big Sur 以降)
- **端末エミュレータ**: ANSI / VT100 エスケープシーケンスおよび代替画面バッファをサポートする端末（GNOME Terminal, xterm, Kitty, Alacritty, iTerm2, macOS Terminal.app, tmux, screen, または SSH セッション）
- **ビルド**:
  ```sh
  cmake -B build-posix -DCMAKE_BUILD_TYPE=Release
  cmake --build build-posix -j
  ```

### 5.2 対話モード動作確認チェックリスト

1. **コアの起動**:
   ```sh
   # 別端末またはバックグラウンドでコアを起動（未起動時は hancho-cli が自動起動）
   ./build-posix/Hancho --headless --data-dir ./ww-data
   ```
2. **対話型 TUI の起動**:
   ```sh
   ./build-posix/hancho-cli --data-dir ./ww-data top
   ```
   - 代替画面バッファへ切り替わり、カーソルが非表示になること。
   - 画面最上部に `Hancho Top`、現在時刻、`Core: ONLINE`、`Interval: 1000ms` が表示されること。
   - タブバー `[1: Processes]  [2: Queue]  [3: Monitor]  [4: Why]  [5: Memory]` が表示され、`1: Processes` が反転ハイライトされていること。

3. **タブ巡回と各ペイン表示の確認**:
   - `Tab`: タブが `1: Processes` → `2: Queue` → `3: Monitor` → `4: Why` → `5: Memory` → `1: Processes` と順繰りに切り替わること。
   - `Shift+Tab`: 逆順に巡回すること。
   - `1` 〜 `5` キー: 各ペイン（Processes, Queue, Monitor, Why, Memory）へ一発で直接切り替わること。
   - 各画面の内容確認:
     - **[1: Processes]**: プロセス一覧（ID, PID, STATE, ELAPSED, CPU, LAST LINE）および選択行の詳細（コマンドライン、CWD 等）が表示されること。
     - **[2: Queue]**: キュー集計（Running, Waiting, Pending, Succeeded, Failed）およびジョブ一覧、試行回数、詳細が表示されること。
     - **[3: Monitor]**: LLM エージェント監視情報（Type, Model, Goal, Notes）およびイベントストリーム（THOUGHT, TOOL_CALL, TOOL_RESULT 等）がスクロール表示されること。
     - **[4: Why]**: 失敗ジョブ診断（失敗分類, 根拠, 直前アクティビティ, 推奨アクション, LLM 推定）が表示されること。
     - **[5: Memory]**: メモリ記録一覧（L1/L2/L3 バッジ, クォータ使用量, 容量上限警告）が表示されること。

4. **行選択ナビゲーションの確認**:
   - `j` または `Down`（下矢印）: テーブルのカーソルポインタ（`>`）が下に移動し、下部詳細ペインの内容が連動して切り替わること。
   - `k` または `Up`（上矢印）: カーソルポインタが上に移動すること。

5. **キーボード操作の確認**:
   - `s` (Stop): 選択中のプロセスまたは稼働中ジョブに対して停止リクエスト（kill または cancel）が送信され、下部に成否トースト（例: `Stopped process #1.`）が表示されること。
   - `r` / `e` (Retry): 選択中の失敗/完了ジョブに対して `taskqueue_retry` が送信され、再実行が投入されること。
   - `c` / `x` (Cancel): 選択中の待機中/稼働中ジョブに対して `taskqueue_cancel` が送信されること。
   - スペース / `u` / `Ctrl+R`: 即座に強制再取得が行われ、画面がリフレッシュされること。
   - `?`: ヘルプオーバーレイ画面（キー一覧）がポップアップ表示されること。`Escape` または `?` でオーバーレイが閉じ、元の画面に戻ること。

6. **終了処理と端末復帰の確認**:
   - `q` または `Ctrl+C`: TUI が正常終了し、代替バッファから元のシェル画面へ復帰すること。
   - カーソルが再表示され、直前のシェル履歴やプロンプトが正常に残っていること。
   - シェルのキー入力（echo および canonical mode）が元通り機能すること。

### 5.3 非対話モード・パイプ・スクリプト連携の確認

`hancho-cli top` は、端末（TTY）のない環境やスクリプト連携でも安全に利用できます:

1. **単発スナップショット (`--once`)**:
   ```sh
   ./build-posix/hancho-cli --data-dir ./ww-data top --once
   ```
   - 生モードや代替バッファに移行せず、1 画面分だけ整形出力して終了コード 0 で終了すること。
2. **パイプ・リダイレクト連携 (`!isatty`)**:
   ```sh
   # パイプ経由でテキスト処理ツールに渡す
   ./build-posix/hancho-cli --data-dir ./ww-data top --once | grep "Process List"
   ./build-posix/hancho-cli --data-dir ./ww-data top --once | head -n 20
   ```
   - 標準出力がパイプに接続されている場合、ANSI エスケープシーケンス（カラーコード等）が自動的に無効化され、プレーンテキストとして流れること。
3. **JSON 出力 (`--json`)**:
   ```sh
   ./build-posix/hancho-cli --data-dir ./ww-data top --once --json | jq .
   ```
   - プロセス・タスクキュー・状態の完全な JSON オブジェクトが出力されること。
4. **更新間隔の変更 (`--interval`)**:
   ```sh
   ./build-posix/hancho-cli --data-dir ./ww-data top --interval 500
   ```
   - ヘッダーの `Interval:` が 500ms となり、更新頻度が反映されること。

### 5.4 端末リサイズとシグナル耐性の確認

- **ウィンドウリサイズ (`SIGWINCH`)**:
  端末ウィンドウの幅・高さを変更した際、`ioctl(STDOUT_FILENO, TIOCGWINSZ, ...)` により端末サイズが再取得され、区切り線や行数が追従して正しく再描画されること。
- **異常終了時の復旧 (`stty sane`)**:
  万一 `kill -9` 等で強制終了され端末のエコーが消えた場合は、シェル上で `stty sane` または `reset` を実行することで端末状態を復旧可能。

---

## 6. スモークテスト構成と自動検証 (Smoke Tests & Automated Verification)

TUI の画面成型（Renderer）、キー入力解析、リモート集約、および操作リクエスト構築は、すべて対話端末のない自動テスト環境（CI / スモークテスト）で 100% 単体テスト可能です。

### 6.1 テスト構成

1. **`tests/cli_top_smoke.cpp`**:
   `hancho-cli top` の全機能を網羅する単体スモークテスト。
   - `TestFormatHelpers`: バイト数表記（B / KB / MB / GB）、文字列パディング/トランケート、時刻文字列成型。
   - `TestKeyParsing`: ESC、矢印キー（ANSI / VT100 SS3）、Shift+Tab、数字 1-5、操作キー（s/r/c/q/space/?）。
   - `TestExtendedKeyParsing`: Home, End, PageUp, PageDown, Enter (\r, \n), SS3 矢印。
   - `TestStateTabNavigation`: タブ順送り、逆送り、巡回ラップアラウンド、直接指定。
   - `TestRenderingProcessesView`: プロセス一覧、カーソルポインタ、稼働行、詳細ペイン。
   - `TestRenderingRemoteProcessesView`: リモートホストタグ `[host]`、ホスト別詳細。
   - `TestRenderingQueueView`: キュー集計カウント、ジョブ一覧、ジョブ詳細。
   - `TestRenderingRemoteJobsInQueueView`: リモートジョブ ID、`main @ host` 表記、Remote PID。
   - `TestRenderingMonitorView`: エージェント種別、モデル、思考イベント、ツール呼び出しログ。
   - `TestRenderingDiagnosticsWhyView`: 失敗分類、原因要約、直前アクティビティ、推奨アクション、LLM 推定。
   - `TestRenderingRemoteJobsInDiagnosticsView`: リモート失敗ジョブ、`queue @ host` 表記。
   - `TestRenderingMemoryView`: クォータ使用率、健全性、L1/L2/L3 バッジ、レコードプレビュー。
   - `TestRenderingHelpOverlay`: ヘルプオーバーレイのキー一覧成型。
   - `TestRenderingOfflineAndStatusMessage`: コア未接続時（OFFLINE）表示、ステータストースト表示。
   - `TestRenderingEmptyLists`: 全ペインの空データ時プレースホルダー表示（クラッシュ耐性）。
   - `TestColorAndNoColorRendering`: カラー時 ANSI 出力と非カラー時 ANSI 除去。
   - `TestNarrowTerminalHandling`: 狭小端末（40x12 等）での境界・折り返し耐性。
   - `TestJobOperationRequests`: プロセス停止、稼働中ジョブ停止、ジョブ再実行、ジョブ取消、リモートジョブ操作リクエスト生成。
   - `TestRemoteHelpers`: リモートジョブ・リモートプロセスの重複排除とホスト属性マージ。

2. **`tests/posix_core_smoke.cpp`**:
   POSIX コア統合スモークテスト内に組み込み。ヘッドレスコア・タスクキュー・BT の検証と同時に、TUI 出力成型エンジン（`RenderScreen` による 5 画面の純粋関数レンダリング、キー解析、リクエスト構築）を自動検証します。

### 6.2 実行方法

```sh
# POSIX 環境での自動検証
./build-posix/posix_core_smoke
```
