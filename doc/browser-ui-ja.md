# ブラウザ UI 方式設計

**[English](browser-ui.md) | [日本語](browser-ui-ja.md)**

Linux / macOS 向け Hancho ダッシュボード（`doc/design-v3-todo.md` §8、`doc/design-v3.md` §7、タスク `ww-data/tasks/061-linux-gui.md`）として提供する **ブラウザ UI** のアーキテクチャ、サーバー配置、localhost 認証、UI 資産の同梱と配信方式、およびネイティブ GUI のペンディング（保留）方針と再開条件を定めます。

---

## 1. サーバー配置とアーキテクチャ

### 1.1 コア組み込み（埋め込み）方式

Linux / macOS におけるブラウザ UI のバックエンドサーバーは、独立した別コマンド（例: `hancho-cli serve`）ではなく、**コア（`Hancho --headless` / POSIX デーモン）に HTTP + WebSocket サーバーを直接組み込む（埋め込み）** 方式を採用します。

```
+-------------------------------------------------------------------+
|                        Web Browser                                |
|  - SPA (HTML5 / Vanilla JS / Pure CSS)                            |
|  - Localhost Dashboard (Process List / TaskQueue / Monitor etc.)   |
+---------------------------------+---------------------------------+
                                  |
            HTTP GET (Static Assets) / WebSocket (IPC Messages & Subscriptions)
                                  |
+---------------------------------v---------------------------------+
|               Hancho Core Process (Hancho --headless)             |
|                                                                   |
|  +-------------------------------------------------------------+  |
|  | Localhost HTTP & WebSocket Server (Port 9138)               |  |
|  | - Token Auth / Host & Origin Verification                   |  |
|  | - Static File Delivery (Embedded in binary or --webui-dir)  |  |
|  | - IPC WebSocket Handler (Multiplex & Subscriptions)         |  |
|  +------------------------------+------------------------------+  |
|                                 |                                 |
|  +------------------------------v------------------------------+  |
|  | Hancho Engine (TaskQueue / ProcessManager / BT / Memory)    |  |
|  +-------------------------------------------------------------+  |
+-------------------------------------------------------------------+
```

### 1.2 独立コマンド（`hancho-cli serve` 等）を採用しない理由

1. **プロセスのライフサイクル統合**:
   - コアが動いている間だけダッシュボードが利用可能になり、コア終了時にサーバーも同時に終了します。ゾンビプロセスや取り残されたポートリスナーが発生しません。
2. **単一バイナリ運用の維持**:
   - ユーザーが起動するプロセスはコア 1 つだけで済みます。バックグラウンドデーモンを 2 重に管理・監視する必要がありません。
3. **通信オーバーヘッドと二重障害点の排除**:
   - 独立サーバーを置く場合、「ブラウザ ↔ serve プロセス ↔ コア（Named Pipe / UDS）」という 2 重の IPC プロキシ構成になり、遅延の増加、障害箇所の増加、シリアライズ/デシリアライズの重複が発生します。
   - コアに直接組み込むことで、コアが持つ既存の WebSocket 多重化・購読機構（`058-1`、`058-2`、`RemoteListener`）をそのまま活用できます。

### 1.3 ポートとプロトコルの多重化

- **既定ポート**: `9138`（`config.json` の `webui_port` または `--webui-port` で変更可能）。
- **HTTP / WebSocket 同一ポート多重化**:
  - `GET /` や `GET /assets/*` 等の通常 HTTP リクエストに対しては、静的 UI 資産（HTML / JS / CSS / SVG）を返します。
  - `Upgrade: websocket` ヘッダーを含む HTTP 要求に対しては、WebSocket ハンドシェイクを行い、既存の Hancho IPC プロトコル（JSON RPC 多重化およびログ・イベントのストリーミング購読）に切り替えます。
  - ポートを 1 つにまとめることで、ポート競合リスクやファイアウォール設定の負担を半減させます。

### 1.4 ブラウザ起動ヘルパー

CLI にブラウザを起動してダッシュボードを開くヘルパーコマンドを用意します。

```bash
hancho-cli web
# または
hancho-cli ui
```

このコマンドは、稼働中のコアのデータディレクトリからトークンとポートを読み取り、OS 既定のブラウザ（`xdg-open` / `open` / Windows の場合は `ShellExecute`）で `http://127.0.0.1:<port>/?token=<token>` を開きます。

---

## 2. localhost の認証とセキュリティ設計

ブラウザ UI はローカルホスト（`127.0.0.1`）でのみ待ち受けますが、同一マシン上で動く別ユーザーのプロセスや、悪意ある Web サイトからの攻撃（DNS Rebinding、CSRF、WebSocket ハイジャック）を確実に防ぐ必要があります。

### 2.1 脅威モデル

| 脅威 | 攻撃内容 | 対策 |
|---|---|---|
| **DNS Rebinding** | 悪意ある外部ドメインの DNS レコードを `127.0.0.1` に書き換え、外部サイト上のスクリプトからローカル API を叩く | HTTP リクエストの `Host` ヘッダーを厳密検証 |
| **Cross-Site Request Forgery (CSRF)** | ユーザーが別タブで開いた悪意あるサイトが、`fetch('http://localhost:9138/...')` を送る | トークン認証の必須化、CORS 不許可 |
| **Cross-Site WebSocket Hijacking** | 外部サイトの JavaScript から `new WebSocket('ws://localhost:9138/')` を開く（SOP が適用されない） | `Origin` ヘッダーの検証とトークン認証 |
| **マルチユーザー環境での傍受** | 同一 Linux マシン上の他ユーザーが localhost ポートにアクセスしてジョブを盗み見・操作する | パーミッション `0600` で保護されたトークンファイル |

### 2.2 セッショントークン方式

1. **トークンの生成**:
   - コア起動時に、暗号論的擬似乱数生成器を用いて 128-bit（32 文字の 16 進文字列）のワンタイムセッショントークンを生成します。
   - 例: `a7f9b2c3d4e5f60718293a4b5c6d7e8f`
2. **トークンファイルの保存**:
   - 生成したトークンは、データディレクトリ内の安全な一時ファイル `<data-dir>/.webui_token` に書き込みます。
   - ファイルのアクセス権限は所有者のみ読み書き可能（POSIX では `chmod 0600`）に制限します。
   - コアの正常終了時には本ファイルを自動削除します。
3. **固定トークンの許容**:
   - 環境変数 `HANCHO_TOKEN` または `hosts.json` のリモート接続用トークンが設定されている場合は、そのトークンによる認証も受け付けます（ヘッドレス自動テストや SSH トンネル運用との互換性のため）。

### 2.3 トークンのブラウザへの引き渡しと消去

1. **URL クエリでの初期引き渡し**:
   - ブラウザ起動時、URL のクエリパラメータとしてトークンを付与します:
     `http://127.0.0.1:9138/?token=<session_token>`
2. **URL からの即時消去**:
   - ブラウザで読み込まれた `app.js` は、初期化の最初期ステップで `window.location.search` からトークンを抽出します。
   - 抽出したトークンはメモリ内変数（または `sessionStorage`）に保存し、直ちに以下のコードで URL バーからトークンを消去します:
     ```javascript
     const params = new URLSearchParams(window.location.search);
     const token = params.get('token');
     if (token) {
       sessionStorage.setItem('hancho_token', token);
       history.replaceState({}, document.title, window.location.pathname);
     }
     ```
   - これにより、URL の履歴保存、スクリーンショット、外部リンククリック時の `Referer` ヘッダーからのトークン漏洩を防止します。

### 2.4 WebSocket 接続の認可

1. **ハンドシェイク時の認証**:
   - ブラウザから WebSocket を開く際、以下のいずれか（優先順）でトークンを渡します:
     - WebSocket URL のクエリ: `ws://127.0.0.1:9138/ws?token=<token>`
     - または WebSocket プロトコルヘッダー（`Sec-WebSocket-Protocol: hancho-auth, <token>`）
     - または初期接続メッセージ: 接続直後にブラウザから `{"action": "auth", "token": "..."}` を送信し、コアが承認するまで他の IPC 要求を受け付けない。
2. **不一致時の挙動**:
   - トークンが無効または未指定の場合、HTTP は即座に `401 Unauthorized` を返し、WebSocket は Close フレーム（コード `4401 Unauthorized`）を送出して切断します。

### 2.5 防御機構の詳細

1. **`Host` ヘッダー検証**:
   - すべての HTTP / WebSocket リクエストにおいて、`Host` ヘッダーが `localhost`、`127.0.0.1`、`[::1]`（およびポート番号）であるかを検査します。
   - 不一致の場合は `403 Forbidden` を返して遮断します（DNS Rebinding 攻撃の無効化）。
2. **`Origin` ヘッダー検証**:
   - `Origin` ヘッダーが存在する場合、オリジンが `http://localhost:*` または `http://127.0.0.1:*` であるかを検査します。
   - 外部ドメイン（例: `http://evil.com`）からの接続は直ちに拒否します。

---

## 3. UI 資産の配置と配布・更新方法

### 3.1 リポジトリ内の資産構造

UI 資産はリポジトリの `src/webui/` 配下に配置します。

```
src/webui/
├── index.html        # SPA メイン画面構造
├── app.js            # メインロジック（WebSocket クライアント、状態管理、ルーティング）
├── style.css         # ダークテーマスタイル（Hancho Windows GUI に準拠した配色）
└── assets/           # アイコン SVG 等の静的リソース
```

### 3.2 外部依存ゼロ（Zero External Runtime Dependencies）

- **フレームワーク非依存**:
  - React, Vue, Angular などの重厚な依存や、ビルドステップが必須となる node_modules 環境を排除します。
  - 標準の HTML5, CSS3 (CSS Variables, Flexbox/Grid), ES6+ Vanilla JavaScript で実装します。
- **外部 CDN 参照の禁止**:
  - Google Fonts や CDN 由来の CSS/JS スクリプトタグを一切含めません。
  - エアギャップ環境やオフラインの閉域開発ネットワークでも完全に自己完結して動作します。

### 3.3 配信モード（組み込みと外部読み込み）

Hancho は 2 つの資産配信モードをサポートします。

```
[UI 資産配信のリクエスト]
       |
       v
+-----------------------------------------------+
| 外部ディレクトリの指定はあるか？              |
| 1. --webui-dir <path>                         |
| 2. 環境変数 HANCHO_WEBUI_DIR                  |
| 3. <data-dir>/webui/                          |
| 4. インストール先 share/hancho/webui/         |
+----------------------+------------------------+
                       |
        +--------------+--------------+
        | YES                         | NO
        v                             v
+-------------------------------+ +-------------------------------+
| 外部ディレクトリから配信      | | バイナリ埋め込みリソース      |
| (開発時・カスタマイズ時)      | | (単一バイナリ配布時・既定)    |
+-------------------------------+ +-------------------------------+
```

1. **バイナリ組み込みリソース（既定・配布用）**:
   - CMake ビルド時に、`src/webui/` の全ファイルを C++ ヘッダー（バイト配列）に変換してバイナリに直接コンパイルします。
   - これにより、単一の実行可能バイナリ（`Hancho` / `hancho-cli`）をコピーするだけで、HTML ファイルの配置ミスやパス切れを起こさずに WebUI を完全動作させることができます。
2. **外部ディレクトリ読み込み（開発・カスタマイズ用）**:
   - 以下のいずれかが指定されている場合、バイナリ組み込みリソースより優先して外部ディレクトリからファイルを配信します:
     - 起動オプション `--webui-dir <dir>`
     - 環境変数 `HANCHO_WEBUI_DIR`
     - `<data-dir>/webui/`
     - POSIX 標準インストールディレクトリ `${CMAKE_INSTALL_PREFIX}/share/hancho/webui/`
   - これにより、フロントエンドの開発・調整時に C++ バイナリを再コンパイルすることなく、ブラウザのリロード（F5）だけで変更を即座に確認できます。

### 3.4 資産の更新ポリシー

- **通常アップデート**: Hancho のバージョンアップ（バイナリ更新）に伴い、組み込み UI 資産も自動的に一貫して最新化されます。
- **カスタムテーマ / ローカル拡張**: ユーザーが独自の CSS や JS を適用したい場合は、`<data-dir>/webui/` に配置することでバイナリを汚さずに上書き可能です。

---

## 4. ネイティブ GUI のペンディング方針と再開条件

### 4.1 ネイティブ GUI をペンディングとする決定

Linux / macOS 向けのデスクトップネイティブ GUI（Qt, GTK, Tauri, wxWidgets, Electron 等）の開発は、**正式にペンディング（保留）** とします。

#### 決定の根拠

1. **ポータビリティと依存関係の最小化**:
   - Hancho は Windows において `/MT` 静的リンクを採用し、VC++ 再頒布可能パッケージすら不要なシングルバイナリを実現しています。
   - Linux / macOS においても同様に、glibc 以外の余計な動的ライブラリ依存（Qt6, GTK4, WebKitGTK など）を極力排除することが設計原則です。
   - Qt や GTK はディストリビューションごとのバージョン差異やデスクトップ環境（GNOME, KDE, Wayland, X11）の挙動差が大きく、保守負担が膨大になります。
   - Tauri や Electron は Node.js や Rust のビルドチェーンを必要とし、C++ 単独の Hancho のビルドパイプラインを過剰に複雑化させます。
2. **IPC 分離アーキテクチャとの適合**:
   - Hancho v3 では、Windows の Direct2D GUI もコアから IPC 経由に分離します（`doc/design-v3.md` §2.3、`doc/gui-ipc-state-inventory.md`）。
   - コアが HTTP + WebSocket で IPC サーバーとして動作する以上、モダンブラウザを UI クライアントとして使用するのが最も自然であり、OS 間の機能格差を最小限に抑えられます。
3. **耐久性（Durability）**:
   - ブラウザ UI では、ブラウザタブを閉じてもコアプロセスはバックグラウンドで動き続け、実行中のジョブは一切中断されません。後からブラウザを開き直せば状態が即座に復元されます。

### 4.2 再開条件（ブラウザ UI で足りなかったもの）

ネイティブ GUI の開発を再開する検討条件は、**以下のいずれかがブラウザ UI では原理的に解決できず、かつ端末 TUI（`hancho-cli top`）でも代替できず、実運用上の致命的な障害となった場合** に限られます:

1. **OS ネイティブのトレイ常駐およびシステム通知の不可欠化**:
   - システムトレイ（AppIndicator / MenuBar）への常駐、トレイメニューからの直接操作、バックグラウンドでの未読ステータス表示が必須要件となり、Web Notifications API や OS ネイティブ通知ヘルパー（`notify-send` / `osascript`）では運用要件を満たせない場合。
2. **対話型仮想端末（PTY）の性能およびエミュレーション限界**:
   - ブラウザ上のターミナルコンポーネント（xterm.js 等）において、大量の連続出力時のレンダリング性能、特殊キーバインド、Sixel グラフィックス、IME インライン入力連携がネイティブ端末に比べて著しく劣り、実用に耐えないと判断された場合。
3. **ローカルデスクトップ統合の重大な欠落**:
   - ブラウザのセキュリティサンドボックスに起因して、ローカルファイルパスの直接ドラッグ＆ドロップ、外部 IDE / エディタとの双方向連携、ネイティブファイルダイアログのシームレスな操作が妨げられ、開発ワークフローが著しく阻害された場合。
4. **極端な低リソース環境でのブラウザオーバーヘッド**:
   - ブラウザプロセスのメモリフットプリントが許容されない低スペック・組み込み環境において、TUI（`hancho-cli top`）の表現力（視覚的なグラフや複数ペインの同時監視など）ではどうしても業務要件を満たせない場合。

---

## 5. ブラウザ UI の機能範囲と Windows 版との対比

ブラウザ UI では、Windows 版 Direct2D GUI（`src/gui_dashboard.cpp`）の **主要 5 機能** を過不足なく網羅します（親カード `061-linux-gui.md`、子カード `061-3-browser-ui-panes.md`）。

| 機能領域 | ブラウザ UI での提供内容 | Windows 版 Direct2D GUI との差異 |
|---|---|---|
| **1. プロセス一覧** | 全プロセス一覧、状態（running/completed/failed）、CPU/メモリ、停止（kill/clear/reset_ids）、プロセス起動、ログ閲覧・Stdin送信 | Windows Job Object 固有の集計表示を POSIX cgroup/process group に対応。ConPTY 低レベル設定は汎用ターミナル/パイプ表示に統合 |
| **2. タスクキュー** | キュー状態別件数 KPI、ジョブ一覧、詳細表示、取り消し（cancel）、手動再試行（retry）、一時停止/再開（pause/resume）、パージ、リソースロック | 操作系と状態表示は完全互換 |
| **3. 監視ペイン** | エージェントの正規化イベント（`hancho.event/1`）常時ストリーミング、ツール呼び出し/思考/進捗、スーパーバイザー目標・メモ編集、使用量グラフ | Direct2D 描画の代わりに HTML/SVG によるレスポンシブグラフ描画 |
| **4. 失敗診断** | ジョブおよびプロセスの停止理由「なぜ失敗したか（Why did it fail）」の分類、証拠、詳細表示、改善アドバイス | 同等の診断ビューと分類バッジ表示 |
| **5. メモリ一覧** | L1 実行記録・L2 ノート・L3 知識の検索、閲覧、L2 ノート取り消し（retract）、L3 昇格（promote）、容量統計、L1 Prune | タスクトレイダイアログを専用のタブ・ペインとして統合 |

### 5.1 ペイン別の機能詳細

1. **プロセス一覧 (Processes Pane)**:
   - 全プロセスの状態（ID、ステータス、PID、コマンドライン、CPU 使用率、メモリ使用量、終了コード、起動時刻）のテーブル表示。
   - プロセス起動ダイアログ: コマンドライン、引数、作業ディレクトリ、実行モード（pipe/pty）、タイムアウトを指定して起動 (`{"action": "start"}`)。
   - 個別プロセスのログ閲覧モーダル: 標準出力/標準エラー出力のリアルタイム追尾表示と、対話型プロセスへの標準入力流し込み (`{"action": "send"}`)。
   - プロセス停止・管理操作: 個別プロセスの強制終了 (`kill`)、非稼働プロセスの行削除 (`remove`)、全稼働中プロセスの停止 (`kill_all`)、終了済みプロセスの消去 (`clear`)、プロセス ID の振り直し (`reset_ids`)。

2. **タスクキュー (Task Queue Pane)**:
   - キュー要約 KPI カード: Total, Running, Waiting, Pending, Blocked, Succeeded, Failed, Cancelled の件数をリアルタイム集計表示 (`taskqueue_counts`)。
   - ジョブ一覧テーブル: 状態バッジ、Job Key / ID、ジョブ名、所属キュー、コマンド、終了コード、所要時間、更新時刻。
   - キュー全体操作: キューの一時停止 (`taskqueue_pause`)、再開 (`taskqueue_resume`)、終了済み/失敗/取り消しジョブのパージ (`taskqueue_purge`)。
   - リソース競合ロック (`taskqueue_resource_locks`) の有効/無効トグル。
   - ジョブ詳細ダイアログ: 完全なメタデータ表示、ジョブの手動再試行 (`taskqueue_retry`)、取り消し (`taskqueue_cancel`)。

3. **監視ペイン (Monitor Pane)**:
   - 稼働中および最近のエージェントプロセスの選択ドロップダウン。
   - スーパーバイザー目標とメモのインライン閲覧・編集・保存機能 (`update_agent_info`)。
   - 正規化イベントストリーム (`hancho.event/1`) フィード: ツール呼び出し (`tool_call`)、思考 (`thought`)、進捗 (`progress`)、エラーのリアルタイム表示と種別フィルタリング。
   - 使用量と費用のアナリティクス: プロンプト/出力/合計トークン数、推定コスト ($)、時間推移の SVG レスポンシブ棒グラフ、日次集計ロールアップテーブル (`get_usage_history`, `get_usage_daily`)。

4. **失敗診断 (Failure Diagnosis Pane)**:
   - キュー全体の失敗分類集計カード: `exit_nonzero`、`timeout`、`memory_limit`、`signal_terminated`、`cancelled`、`tool_error` 等のカテゴリ別バッジと発生件数の可視化 (`taskqueue_diagnose` breakdown)。
   - 失敗したジョブおよびプロセスの詳細カードリスト: エラー分類バッジ、エラーサマリー、証拠（実行コマンド、終了コード、標準エラー出力の抜粋）、改善アドバイス（タイムアウト設定の見直し、依存関係の修復など）の提示。

5. **メモリ一覧 (Memory Pane)**:
   - 階層メモリ容量統計バナー: 合計使用量、L1 実行記録容量、L2 ノート容量、L3 知識容量、容量クォータプログレスバー、上限警告状態表示 (`memory_storage_stats`)。
   - メモリレイヤー別フィルタ（All / L1 / L2 / L3）とインクリメンタル検索ボックス (`memory_search`)。
   - メモリレコード一覧テーブル: レイヤーバッジ、レコード ID、所属ジョブ、成否（outcome）、要約/知見プレビュー。
   - L2 ノートの管理操作: L3 知識への昇格承認 (`memory_promote`)、理由を指定した取り消し (`memory_retract`)。
   - メモリ整理機能: 保持期間（日数）を指定した L1 古い実行記録の prune モーダル (`memory_prune`)。

### 5.2 Windows 版から削った・改変した機能（差分と理由）

| 項目 | Windows 版 Direct2D GUI | ブラウザ UI (Linux / macOS) | 削った・改変した理由 |
|---|---|---|---|
| **グラフィックス描画エンジン** | Direct2D 1.1 / DirectWrite / DXGI ハードウェアアクセラレーション | 標準 HTML5 / CSS3 Grid & Flexbox / レスポンシブ SVG | ブラウザ標準の DOM レンダリングエンジンに委託することで、クロスプラットフォームでのポータビリティを確保し GPU ドライバ依存を排除。 |
| **Sixel インライングラフィックス** | Direct2D ビットマップ変換による六色・画像直接描画 | テキストログおよび SVG グラフ描画（Sixel デコードはペンディング） | ブラウザ標準の SVG ベクター描画でグラフ表示を満たし、複雑な ANSI Sixel デコーダのブラウザ実装を保留。 |
| **プロセスカウンタ** | Win32 Job Object の特定カウンタ (`JOBOBJECT_BASIC_ACCOUNTING_INFORMATION`) | POSIX プロセスグループ / `getrusage` / `/proc` 準拠の CPU/メモリ集計 | POSIX 環境の標準プロセスモデルに適合させ、Windows 固有の Job Object API 依存を排除。 |
| **システムトレイ常駐** | Win32 タスクトレイ（`Shell_NotifyIcon`）ポップアップメニュー | 専用のタブナビゲーション（Processes, Task Queue, Monitor, Diagnosis, Memory） | ブラウザのタブ UI として第一級の画面ペインに昇格させ、OS 固有のトレイ環境（GNOME, KDE, macOS メニューバー）の差異を吸収。 |
| **モーダルダイアログ** | Win32 ネイティブダイアログ (`DialogBoxParam`, `GetOpenFileName`) | SPA 内蔵 HTML モーダルオーバーレイ | ブラウザサンドボックス内ですべて完結させ、OS ウィンドウマネージャのブロッキングを防ぐ。 |
| **ConPTY 固有設定** | ConPTY 疑似コンソールハンドル・フォント設定 | 汎用パイプ/PTY ログストリーム表示と標準入力送信 | POSIX 端末エミュレーションとログ配信に統一。 |

---

## 6. ブラウザ UI のビルドと配布パッケージング

Hancho ブラウザ UI は、第三者が追加の依存関係や複雑な環境構築なしに利用・導入できるよう、**「完全単一バイナリによる自己完結動作」** を基本原則として設計されています。

### 6.1 ビルド手順

#### 6.1.1 必要なツールと環境
- **CMake**: 3.20 以上
- **C++ コンパイラ**: C++20 対応コンパイラ
  - Linux: GCC 11+ または Clang 14+（`libutil`、`pthreads`）
  - macOS: Apple Clang 14+（`pthreads`、libc 組み込み PTY）
  - Windows: MSVC 2019+（`/MT` 静的ランタイム）
- **フロントエンド開発ツールは不要**: Node.js, npm, yarn, webpack 等のビルドツールは**一切不要**です。外部 CDN 参照もないため、オフライン・エアギャップ環境でもビルド・実行が完結します。

#### 6.1.2 単一バイナリのビルド（標準・既定）
UI 資産（HTML/CSS/JS/SVG）は `src/webui_assets.cpp` にコンパイル時埋め込み文字列として保持されています。通常の CMake ビルドを実行するだけで、外部ファイル不要な自己完結バイナリが生成されます。

- **Linux / macOS**:
  ```bash
  cmake -S . -B build-posix -DCMAKE_BUILD_TYPE=Release
  cmake --build build-posix --parallel
  ```
  生成されるバイナリ:
  - `build-posix/Hancho`: ヘッドレスコア（埋め込み WebUI HTTP + WebSocket サーバー内蔵）
  - `build-posix/hancho-cli`: CLI クライアント（`hancho-cli web` / `ui` ブラウザ起動コマンド内蔵）
  - `build-posix/webui_smoke`: WebUI 単体・統合スモークテストバイナリ

- **Windows**:
  ```cmd
  cmake -S . -B build -A x64
  cmake --build build --config Release
  ```
  生成されるバイナリ:
  - `build\Release\Hancho.exe`: Direct2D GUI + 組み込み WebUI サーバー
  - `build\Release\hancho-cli.exe`: CLI クライアント

#### 6.1.3 UI 資産の開発と更新フロー
フロントエンド（HTML/CSS/JS）の調整や機能追加を行う開発者は、C++ の再コンパイルを挟まずに即時確認できます。

1. **ディスクからのライブ読み込み（開発時）**:
   ```bash
   # --webui-dir オプションでソースディレクトリを指定
   ./build-posix/Hancho --headless --webui-dir src/webui --data-dir ./ww-data
   # または環境変数で指定
   export HANCHO_WEBUI_DIR=$(pwd)/src/webui
   ```
   この状態で `src/webui/` 配下の `index.html` や `app.js`、`style.css` を編集すると、ブラウザのリロード（F5）だけで変更が即時反映されます。

2. **埋め込みソース (`src/webui_assets.cpp`) の同期**:
   開発が完了した資産を単一バイナリ用の埋め込みソースに反映します。`src/webui/` 内の各ファイル（`index.html`、`style.css`、`app.js`、`assets/icon.svg`）の内容を `src/webui_assets.cpp` の対応する C++ 生文字列リテラル（`kIndexHtml`、`kStyleCss`、`kAppJs`、`kIconSvg`）に転記・更新します。

#### 6.1.4 テストと検証
ブラウザ UI の HTTP 配信、MIME タイプ解決、トークン認証、DNS Rebinding / Origin 検証、WebSocket IPC 多重化、再接続復元は自動テストで検証可能です。

```bash
# POSIX
./build-posix/webui_smoke

# Windows
build\Release\webui_smoke.exe
```

---

### 6.2 パッケージ形式と配布方法

#### 6.2.1 単一バイナリアーカイブ配布（推奨・標準形式）
UI 資産がコアバイナリに内蔵されているため、外部の HTML/JS ディレクトリを同梱することなく、実行ファイル単体または tar.gz アーカイブとして配布できます。

- **Linux / macOS 配布アーカイブ構成 (`hancho-vX.Y.Z-<os>-<arch>.tar.gz`)**:
  ```
  hancho-v0.9.5-linux-x64/
  ├── bin/
  │   ├── Hancho                  # ヘッドレスコア本体（WebUI サーバー内蔵）
  │   └── hancho-cli              # CLI クライアント（web コマンド内蔵）
  ├── doc/                        # ドキュメント一式（Markdown）
  │   ├── browser-ui-ja.md
  │   ├── browser-ui.md
  │   └── ...
  ├── tools/                      # 補助ツール群（ww タスクランナー等）
  ├── LICENSE                     # ライセンス表記
  ├── README.md                   # 英語 README
  └── README-ja.md                # 日本語 README
  ```

- **アーカイブの作成手順 (POSIX)**:
  ```bash
  VERSION="0.9.5"
  PKG_DIR="dist/Hancho-v${VERSION}-linux-x64"
  mkdir -p "${PKG_DIR}/bin" "${PKG_DIR}/doc" "${PKG_DIR}/tools"
  cp build-posix/Hancho build-posix/hancho-cli "${PKG_DIR}/bin/"
  cp -r doc/* "${PKG_DIR}/doc/"
  cp -r tools/* "${PKG_DIR}/tools/"
  cp LICENSE README.md README-ja.md "${PKG_DIR}/"
  tar -czf "dist/Hancho-v${VERSION}-linux-x64.tar.gz" -C dist "Hancho-v${VERSION}-linux-x64"
  ```

- **Windows 配布形式**:
  - `dist/Hancho-vX.Y.Z-win64.zip`: 単一バイナリおよび依存 dll、ドキュメントの zip
  - `dist/Hancho-vX.Y.Z-win64-setup.exe`: Inno Setup インストーラー（PATH 自動追加、スタートメニュー登録）

#### 6.2.2 FHS 準拠のシステム分離配布（deb / rpm / Homebrew 等）
ディストリビューションパッケージャーやシステム管理者が、Linux の FHS（Filesystem Hierarchy Standard）に則って資産を分離配置する場合も完全に対応しています。

- **配置先規約**:
  - バイナリ: `/usr/bin/` または `/usr/local/bin/`
  - 静的 UI 資産（カスタマイズ用・分離配布時）: `/usr/share/hancho/webui/` または `/usr/local/share/hancho/webui/`
  - ドキュメント: `/usr/share/doc/hancho/`
- **フォールバック解決ルール**:
  コアプロセス（`Hancho`）は、起動時に以下の優先順序で UI 資産を探索します:
  1. コマンドライン引数 `--webui-dir <path>`
  2. 環境変数 `HANCHO_WEBUI_DIR`
  3. データディレクトリ直下の `<data-dir>/webui/`
  4. FHS 標準ディレクトリ `${CMAKE_INSTALL_PREFIX}/share/hancho/webui/`
  5. 組み込み静的資産（フォールバック）
  これにより、分離パッケージとして静的ファイルが配置されていればそれが使用され、ファイルが欠落または破損していても内蔵アセットにより常に確実に動作します。

---

### 6.3 第三者による導入・セットアップ手順

エンドユーザーや第三者開発者が Hancho ブラウザ UI を導入して利用を開始する手順です。

#### 6.3.1 ステップ 1: インストール
配布アーカイブを展開し、実行バイナリを PATH の通ったディレクトリに配置します。

```bash
# アーカイブを展開
tar -xzf Hancho-v0.9.5-linux-x64.tar.gz
cd Hancho-v0.9.5-linux-x64

# /usr/local/bin 等に配置（または ~/.local/bin）
sudo cp bin/Hancho bin/hancho-cli /usr/local/bin/
sudo chmod +x /usr/local/bin/Hancho /usr/local/bin/hancho-cli

# 動作確認
hancho-cli --version
```

#### 6.3.2 ステップ 2: コアプロセスの起動
任意の作業プロジェクトまたはデータディレクトリを指定してコアを起動します。

```bash
# フォアグラウンドまたはバックグラウンドでコアを起動
Hancho --headless --data-dir ./ww-data &
```
※ コアが停止している状態で `hancho-cli` コマンドを実行した場合でも、自動的にデーモンコアが起動されます。

- **ポートの指定（任意）**:
  既定ポート `9138` 以外を使用したい場合は、`--webui-port` 引数または環境変数 `HANCHO_WEBUI_PORT` を指定します:
  ```bash
  Hancho --headless --webui-port 9200 --data-dir ./ww-data
  ```

#### 6.3.3 ステップ 3: ブラウザ UI のオープン
CLI から以下のコマンドを実行するだけで、安全な認証セッショントークンが付与された状態で既定のブラウザが開きます。

```bash
# ブラウザを自動起動してダッシュボードを開く
hancho-cli web
# （エイリアス: hancho-cli ui）
```

- **ヘッドレス環境・URL 表示のみ**:
  X11 / Wayland 等のデスクトップ環境のないサーバーやコンテナでは、`--print-url` オプションを使用します:
  ```bash
  hancho-cli web --print-url
  # 出力例: http://127.0.0.1:9138/?token=4c6e93...
  ```
  JSON 形式で取得したい場合は `--json` を指定できます:
  ```bash
  hancho-cli web --json
  # {"ok": true, "port": 9138, "token": "...", "url": "http://127.0.0.1:9138/?token=..."}
  ```

#### 6.3.4 リモートサーバー・クラウド VM での利用（SSH トンネリング）
ブラウザ UI はセキュリティを最優先し、外部ネットワークにはポートを直接公開せずローカルホスト（`127.0.0.1`）のみで待ち受けます。リモートサーバー上の Hancho ダッシュボードを手元の PC から安全に操作するには、SSH ポートフォワードを利用します。

1. **手元の PC から SSH トンネルを開設**:
   ```bash
   ssh -L 9138:127.0.0.1:9138 user@remote-host
   ```
2. **リモートでトークン付き URL を確認**:
   ```bash
   hancho-cli web --print-url
   ```
3. **手元の PC のブラウザで開く**:
   手元のブラウザのアドレスバーに取得した URL（例: `http://127.0.0.1:9138/?token=...`）を入力します。ブラウザから見ると `localhost:9138` へのアクセスとなるため、Host/Origin 検証を通過し、暗号化された SSH トンネル経由で安全に操作できます。

#### 6.3.5 常駐サービス設定例（systemd / launchd）

- **Linux (systemd ユーザーサービス: `~/.config/systemd/user/hancho.service`)**:
  ```ini
  [Unit]
  Description=Hancho Headless Core & WebUI Dashboard
  After=network.target

  [Service]
  Type=simple
  ExecStart=/usr/local/bin/Hancho --headless --data-dir %h/.local/share/hancho
  Restart=on-failure
  RestartSec=5s

  [Install]
  WantedBy=default.target
  ```
  登録と起動:
  ```bash
  systemctl --user daemon-reload
  systemctl --user enable --now hancho
  ```

- **macOS (launchd エージェント: `~/Library/LaunchAgents/com.hancho.core.plist`)**:
  ```xml
  <?xml version="1.0" encoding="UTF-8"?>
  <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
  <plist version="1.0">
  <dict>
      <key>Label</key>
      <string>com.hancho.core</string>
      <key>ProgramArguments</key>
      <array>
          <string>/usr/local/bin/Hancho</string>
          <string>--headless</string>
          <string>--data-dir</string>
          <string>/Users/username/.local/share/hancho</string>
      </array>
      <key>RunAtLoad</key>
      <true/>
      <key>KeepAlive</key>
      <true/>
  </dict>
  </plist>
  ```

---

## 7. 関連ドキュメントとタスク依存

- [設計文書: Hancho v3](design-v3.md) (§2.4 段階 B、§7 未決事項)
- [設計文書 未実装項目](design-v3-todo.md) (§8 Linux / macOS の GUI と TUI)
- [POSIX ヘッドレスコアのビルド](build-posix-ja.md) (ビルド手順と実行)
- [リモートホスト仕様](remote-hosts-ja.md) (WebSocket 接続およびトークン認証仕様)
- [GUI IPC 状態目録](gui-ipc-state-inventory.md) (Direct2D GUI から切り離された IPC アクション一覧)
- 親タスク: `ww-data/tasks/061-linux-gui.md`
- 関連タスク:
  - `ww-data/tasks/061-1-browser-ui-approach.md` (HTTP + WebSocket 方式と認証設計)
  - `ww-data/tasks/061-2-browser-ui-skeleton.md` (HTTP + WebSocket サーバー骨格実装)
  - `ww-data/tasks/061-3-browser-ui-panes.md` (5 機能の HTML/JS 実装)
  - `ww-data/tasks/061-4-browser-ui-package.md` (ビルドと配布の文書化)
