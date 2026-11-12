# Hancho

**[English](README.md) | [日本語 (Japanese)](README-ja.md)**

**Hancho** は、Claude Code や各種自動化スクリプトから複数のサブプロセス・対話型 CLI エージェント（`opencode`, `claude`, PowerShell, cmd, Git Bash 等）を並列起動・監視・制御し、**状況をひと目で把握できる Native Win32 + Direct2D マルチターミナルダッシュボード GUI** を備えたプロセスマネージャです。

バージョンは `hancho-cli.exe --version` で確認できます（現在: 0.9.1）。

### 対象ユーザー

- **LLM コーディングエージェント（Claude Code、opencode、Codex など）を使う Windows 開発者**で、監督役の LLM から別の CLI エージェントをワーカープロセスとして動かし、作業を任せたい人。
- **長時間・並列・対話型のジョブ**（ビルド、テスト、開発サーバー、TUI エージェント）を扱う人。チャットのセッションより長く動かしたい作業、依存関係やリトライのある決まった計画で進めたい作業、Claude 以外のワーカーが混ざる作業向けです。
- **多数のエージェント端末を 1 つのダッシュボードで見ながら操作したい人**。タイムアウト、リトライ、依存関係を設定でき、ログは再起動後も残ります。

**向いていないケース**: 短い推論やコード修正だけで、LLM のサブエージェントで足りている場合。トークンコストはどちらを使ってもほぼ同じです。Hancho で何が得られるか（耐久性、決定的な制御、可視性）は [サブエージェント vs. Hancho](doc/pros-cons-comparison-ja.md#トークンコストが決め手でなくなった今) を、全体は [Pros/Cons 比較](doc/pros-cons-comparison-ja.md) を参照してください。

![Hancho Dashboard](doc/images/dashboard.jpg)

ダッシュボードのプロセス一覧・タスクキュー一覧の下には **実行状態監視ペイン** があります。選択中のプロセスに追従し、目標、今何をしているか、TODO の進捗、ジョブの成功条件・試行回数・制限時間、経過時間付きの平易な活動履歴を表示します。JSON ストリーミング中のプロセスはプロセス一覧で `{}` が付きます。詳しくは [主な特徴](doc/features-ja.md) を参照してください。

---

## Express

- [Hancho-v0.9.5-win64-setup.exe](https://github.com/gadget114514/Hancho/releases/download/v0.9.5/Hancho-v0.9.5-win64-setup.exe) をダウンロードしてインストール
  （ポータブル版: [Hancho-v0.9.5-win64.zip](https://github.com/gadget114514/Hancho/releases/download/v0.9.5/Hancho-v0.9.5-win64.zip)、全バージョン: [Releases](https://github.com/gadget114514/Hancho/releases)）
- LLM に「PATH から hancho を探して、help を読んで使い方を把握して」と伝える
- LLM に「タスクを作成して、hancho 経由で (opencode|claude|agy|codex) に実行させて」と伝える
- 以上

LLM は `hancho help --json` を実行して使い方を理解します。
手元の LLM を監督役（フロントエンド）として使い、別の LLM をワーカーとして指定できます。

---

## ワーカー LLM に委任する: `run_worker`

MCP ツール `run_worker` は、1つのタスクをワーカーの LLM CLI に任せ、終了まで待って最終結果だけを返します。ワーカーのやり取りは監督役のコンテキストに入らず、生のストリームは `stream_file` に残ります。

```json
{
  "name": "run_worker",
  "arguments": {
    "cmd": "claude -p \"失敗しているテストを要約して\" --output-format stream-json --verbose",
    "name": "test-summary",
    "cwd": "D:/work/myproject",
    "timeout_sec": 1800
  }
}
```

| 引数 | 意味 |
| :--- | :--- |
| `cmd`（必須） | ワーカーのコマンドライン。1コマンドのみ |
| `name`, `cwd` | ダッシュボードの表示名、作業ディレクトリ |
| `agent_type` | ストリーム形式: `claude`, `agy`, `opencode`, `auto`（既定はコマンドから判定） |
| `timeout_sec`, `idle_timeout_sec` | この実行時間を超えたとき、またはこの時間出力がないときに強制終了（0 = 無制限） |
| `supervisor_goal`, `supervisor_notes` | ダッシュボードに表示するテキスト |
| `wait_ms` | 最大待ち時間（ミリ秒。既定の 0 はワーカー終了まで） |

応答には `finished`, `success`, `state`, `exit_code`, `result`, `result_source`, `result_truncated`, `tool_calls`, `tool_errors`, `num_turns`, `permission_denials`, `total_tokens`, `cost_usd`, `log_file`, `stream_file` が含まれます。

- `result` の上限は 64 KB で、切り詰めると `result_truncated` が true になります。ストリームに結果のイベントがないときは、`result` に出力の末尾の行が入り、`result_source` は `output` になります。
- `wait_ms` が切れると、応答は `finished: false` とワーカーの現在の動作を返し、ワーカーは動き続けます。続きは、返された `id` を `wait_worker` に渡して待ちます。
- ハーネスからはネイティブのサブエージェントには見えません。そう見せたいときは、`run_worker` を呼んで結果を返すだけの薄いカスタムエージェント（Claude Code なら `.claude/agents/<name>.md`、安価なモデル、このツールのみ）を定義します。その分のトークンがかかるため、`run_worker` を直接呼ぶのが最も安上がりです。
- 順序・依存・リトライのある複数ステップには、[タスクキュー](doc/taskqueue-ja.md)を使ってください。

---

## ビヘイビアツリー: 次に何をするかの判断を Hancho の中で行う

次の手順が前の結果しだいの作業（ビルド → 修正 → 再ビルド、テスト → 修正 → 再テスト、「このファイルが X なら Y をする」）では、これまで監督役の LLM が付きっきりでした。ジョブの終了を待ち、結果を読み、次を決めてジョブを足す、を繰り返していました。Hancho はビヘイビアツリー (BT) の実行機構を内蔵しました。LLM は作業全体を 1 つの JSON のツリーとして書き、`bt_start` で一度だけ渡します。Hancho がツリーをたどり、各手順を普通のタスクキューのジョブとして実行し、決まりきった判断は自分で行います。

| | これまで（LLM が 1 手順ずつ回す） | ビヘイビアツリー |
| :--- | :--- | :--- |
| 次の手順を決めるのは | 監督役の LLM。手順ごとに 1 ターン | Hancho がツリーに従って決める（分岐、ループ、リトライ、条件の確認） |
| トークンとターン | 手順ごとにターンとコンテキストを使う | `bt_start` 1 回。LLM が呼ばれるのは本当に判断が要るとき (`escalate`) と終了時だけ |
| 監督役の LLM | 起きて見張り続ける必要がある | `bt_wait` で待つ、寝る、コンテキストが圧縮される、のどれでもツリーは進む |
| 終わらないループ | LLM が止めるかどうか次第 | `retry` / `repeat` は `max` が必須。ツリー全体も `maxNodeRuns` と `maxWallSec` で打ち切る |
| 機械的な確認 | LLM がログや終了コードを読む | 組み込みの条件: `exitCode`, `fileExists`, `fileNewer`, `outputMatches`, `jsonValue`, `var` |
| 判断の記録 | 会話履歴に埋もれる | `escalate` が判断に必要な材料（目的、質問、基準、ログ、ファイル）をまとめて渡し、回答と理由を記録する |
| 再起動 | LLM がどこまで進んだかを覚えておく必要がある | ツリーの状態は変化のたびに保存され、Hancho を再起動しても続きから進む |
| ワーカーのエンジン | CLI ごとにコマンドラインを手で書く | `worker` の手順はエンジン名（`claude`, `agy`, `opencode`, `codex`、または `engines.json` の独自定義）で指定。1 つのツリーで混在できる |

変わらない点: 各手順は今まで通りタスクキューのジョブで、タイムアウト、成功条件、リソースロック、ログを持ち、タスク一覧にも普通のジョブとして並びます。ツールバーの **BT** ボタン（Ctrl+Shift+B）で、どのノードがどのタスクを起動したか、各ツリーがどこまで進んだかを表示できます。LLM は `hancho-cli help --json` と下記のビヘイビアツリー関連資料から書き方を知ります。

### ビヘイビアツリー関連資料

- **[ビヘイビアツリーガイド](doc/behavior-tree-ja.md)** ([English](doc/behavior-tree.md)): JSON 定義形式、全ノード種類、変数展開、ワーカーエンジン、`escalate` 判断パケット、完全なサンプル 3 例を含む総合ガイド。
- **[トークン消費削減ガイド](doc/behavior-tree-token-optimization-ja.md)** ([English](doc/behavior-tree-token-optimization.md)): 中間ステップのトークン消費ゼロ化、`compact: true` による応答軽量化、`escalate` の行数・サイズ制限など、トークン効率を最大化する設計とベストプラクティス。
- **[設計・実装計画書](doc/behavior-tree-plan.md)** ([English](doc/behavior-tree-plan-en.md)): ビヘイビアツリーの設計思想、状態マシンとタスクキューの統合、GUI 連携計画をまとめた設計書。

---

## 安定したイベントとアダプター

Hancho は生のストリームの隣に、版番号つきのイベント列を書き出します。ワーカープロセスの `logs/proc_<id>_<name>_stream.jsonl` は CLI の生の行を、`logs/proc_<id>_<name>.events.jsonl` は正規化した `hancho.event/1` イベント（`tool.call`、`tool.result`、`usage`、`run.ended` など）を持ちます。スキーマは [doc/schema/hancho.event.v1.json](doc/schema/hancho.event.v1.json) で配布します。

- `hancho-cli worker stream <id>` は `.events.jsonl` の正規化イベント（`hancho.event/1`）を表示します。
- MCP の `get_worker_stream` は同じ形式（`hancho.event/1`）を返します。
- C++ を書かずに CLI を追加できます。`<data-dir>/adapters/<name>.json` を置く（Tier 1: `match`、`rules`、JSON Pointer の `map`）か、`exec` に変換プロセスを指定します（Tier 3: stdin に生の行、stdout に `hancho.event` 行）。
- `engines.json` に `adapter` と `capabilities`（`jsonStream`、`resume`、`reportsUsage`、`reportsSubagents`、`permissionModes`）を書けます。
- ラッパースクリプトから直接イベントを出せます。pipe モードのワーカーには `HANCHO_EVENT_PIPE` が渡され、`hancho-cli event emit --kind message --text "..."` で書き込めます。
- `hancho-cli adapter test <name>` は `tests/adapters/<name>/` の入力と期待する `{kind, data}` を照合します。

**イベント形式と旧形式の削除。** `hancho-cli worker stream` と MCP `get_worker_stream` は `hancho.event/1` を返します。以前の内部 `type` 文字列は v0.9.3 で非推奨になり、**v0.11.0** で削除されました。`--schema legacy`（CLI）または `schema: "legacy"`（MCP）を指定すると非対応エラーとなります。移行記録と `type` → `kind` の対応表は [doc/LLM-jsonstream-format.md](doc/LLM-jsonstream-format.md#legacy-type-strings-and-migration-to-hanchoevent1) にあります。

## Hancho をやめるとき（卒業）

Hancho は目的ではなく手段です。[Hancho v3 設計 §5.2](doc/design-v3.md) の理由を harness 側が満たしたら、作業を失わずにやめられます。

1. `hancho-cli.exe mcp uninstall` で `.mcp.json` / `opencode.json` から Hancho の項目を外します。
2. `hancho-cli.exe agent uninstall-template` で `.claude\agents\hancho-runner.md` を消します。
3. データディレクトリは既定で残ります。消す場合はどちらかのコマンドに `--delete-data-dir` を付けます。

Hancho のデータはすべて JSON / JSONL（タスクキュー、ビヘイビアツリー、ストリームイベント、意味的メモリ）で、Hancho がなくても読めます。やめる前に [design-v3.md §5.3–5.5](doc/design-v3.md) の機能対応表と置き換え条件を確認してください。この表はリリースごとに[リリースチェックリスト](doc/release-checklist-ja.md)で見直します。

## ドキュメント

| ドキュメント | 内容 |
| :--- | :--- |
| [スタート](doc/start-ja.md) | LLM エージェント向けクイックスタート、ビルド方法、`Hancho.exe` と `hancho-cli.exe` |
| [主な特徴](doc/features-ja.md) | ダッシュボード GUI、シェル起動、履歴、トレイ、ログ、設定、タスクキュー概要 |
| [CLI](doc/cli-ja.md) | `hancho-cli.exe` の使い方、stdin モード、完了通知 |
| [タスクキュー](doc/taskqueue-ja.md) | 順序・依存・ジョブのフィールド・状態・ChainGuard |
| [ビヘイビアツリー](doc/behavior-tree-ja.md) | BT の形式、ノードの種類、変数、ワーカーのエンジン、escalate、例 |
| [BT トークン削減ガイド](doc/behavior-tree-token-optimization-ja.md) | トークン削減アーキテクチャ、`compact: true`、軽量化パケット |
| [BT 設計計画書](doc/behavior-tree-plan.md) | ビヘイビアツリーエンジンのアーキテクチャと実装計画 |
| [MCP サーバー](doc/mcp-ja.md) | MCP 登録と MCP ツール |
| [データディレクトリ](doc/data-directory-ja.md) | プロジェクト内・git 管理の `--data-dir` |
| [Pros/Cons](doc/pros-cons-comparison-ja.md) | サブエージェント等との使い分け |
| [批判的比較](doc/critical-comparison-ja.md) | Hancho の限界と使うべきでないケースを辛口に整理 |
| [競合比較](doc/competitive-comparison-ja.md) | エージェントフレームワークやメモリサービスとの位置づけ |
| [Hancho v3 設計](doc/design-v3.md) | イベントスキーマとアダプター、ヘッドレス/リモート、意味的メモリ、失敗分析、卒業指針 |
| [リリースチェックリスト](doc/release-checklist-ja.md) | リリースごとの卒業用機能対応表の見直しと片付けコマンドの確認 |
| [tools/ww](tools/ww/README-ww.md) | 任意・独立のタスクカード実行テンプレート（Hancho の一部ではない） |

---

（以下の help 本文は英語です）

## リリースノート

### v0.9.5
- **ブラウザダッシュボード**: `hancho-cli web` で、プロセス / キュー / 監視 / 失敗診断 / メモリの各画面を持つ HTTP + WebSocket ダッシュボード（127.0.0.1:9138、トークン認証）を開けます。`doc/browser-ui-ja.md` を参照。
- **対話 TUI**: `hancho-cli top` が 5 タブ、行移動、停止 / 再試行 / 取消、リモートジョブ、`--once --json` を備えた端末ダッシュボードになりました。外部 curses 依存はありません。`doc/tui-design-ja.md` を参照。
- **リモートホストの完成**: 1 本の WebSocket で要求を多重化し、ログ / イベントを seq 再開付きで配信。ローカルダッシュボード再起動後もリモート試行へ再接続し、`success.filesNewer` はリモートホスト側の時計で評価します。
- **長期使用量集計**: 日次 / 月次の `hancho.usage.day/1` ロールアップ、`hancho-cli usage stats|export|compact`、MCP `usage_stats` / `usage_export`、IPC `get_usage_daily`、ダッシュボードの期間選択。エンジン / モデル別の帰属は、JSON ストリームを出し `--model` を受け取るタスクキュー / ビヘイビアツリージョブから記録されます。
- **メモリ運用**: 外部ランカープラグイン（失敗時は BM25 に自動フォールバック）、`memory_max_bytes` 上限付きの `memory prune`、設定したワーカーによる L3 昇格、タスクキュー外の通常実行を L1 記録にする仕組み（`taskqueue record-plain`、`memory rebuild`）。
- **チェックと安全性**: `success.testReport`（JUnit / NUnit）とスコープ付き名前付きリソースロック（`@name:{root}`、`@name:{cwd}`）、`hancho.event/1` の `data` を厳密に型付け、`--schema legacy` を拒否。
- **ドキュメントと help**: `hancho-cli help` に、`start` はプロセスでありタスクではないこと、タスク一覧とモデル別使用量には `taskqueue add` / `run` / `bt start` が必要であることを明記しました。

### v0.9.4
- **意味的メモリ**: L1 実行記録、L2 ノート、L3 承認済み知識を `memory/` に保存。`hancho-cli memory search|get|brief|stats|note|approve|rebuild|prune` と MCP `memory_search` / `memory_get` / `memory_brief` / `memory_stats` / `memory_note` / `memory_retract`。`{memory}` で上限付きの brief を注入し、渡した id を試行に記録します。
- **失敗分析**: 試行ごとに決定的な診断（分類、確信度、要約、証拠、直前の活動、推奨アクション）。`hancho-cli taskqueue why <id>`、MCP のタスク結果、BT の条件から参照でき、再試行方針を分類ごとに変えられます。
- **ヘッドレス / POSIX / リモートの基礎**: `Hancho.exe --headless`、`hancho-cli run --exit-on-drain --report junit.xml`、POSIX ビルドとプラットフォーム境界、`hosts.json` とリモートジョブのミラー。初期段階です（`doc/build-posix.md`、`doc/remote-hosts.md`）。
- **卒業**: キュー / BT / 知識の `hancho-cli export`、`mcp uninstall`、`agent uninstall-template`、リリースチェックリスト。
- **ETA と paused 起動**: ジョブが `estimateSec` を受け取り、ダッシュボードがタイムアウトではなくカード見積を残り時間に表示。`bt start --paused true` はジョブを作らずにツリーを登録します。

### v0.9.3
- **旧ストリーム形式の非推奨化**: `hancho-cli worker stream` と MCP `get_worker_stream` は既定で `hancho.event/1` を返します。旧 `type` 文字列は `--schema legacy` / `schema: "legacy"` で v0.10.x まで利用でき、v0.11.0 で削除します。[移行手順](doc/LLM-jsonstream-format.md#legacy-type-strings-and-migration-to-hanchoevent1)を参照してください。
- **BT 全体のタスク一覧**: `hancho-cli bt tasks --id <id>`、`taskqueue list --bt <id>`、MCP `taskqueue_list` の `bt` で、ビヘイビアツリーの全ジョブを並列分岐のキューをまたいで一覧できます。並列分岐は専用キュー（と専用の並列度）を保ち、ダッシュボードは元々すべてのキューをまとめて表示します。

### v0.9.2
- **安定したイベントスキーマ**: ワーカーの出力を正規化した `hancho.event/1` イベントとして `logs/proc_<id>_<name>.events.jsonl` にも書きます（生の `.stream.jsonl` はそのまま）。`hancho-cli worker stream <id> --schema v1`、または MCP の `get_worker_stream` に `schema: "v1"` を渡して読めます。JSON Schema は `doc/schema/hancho.event.v1.json` に同梱します。
- **C++ なしで CLI に対応**: `<data-dir>/adapters/<name>.json` を置く（Tier 1: `match`、`rules`、JSON Pointer の `map`）か、`exec` に変換プロセスを指定します（Tier 3: stdin に生の行、stdout に `hancho.event` 行）。`engines.json` に `adapter` と `capabilities` を書けます。
- **スクリプトからイベントを送信**: pipe モードのワーカーには `HANCHO_EVENT_PIPE` が渡され、`hancho-cli event emit --kind ... --text ...` で書き込めます。`hancho-cli adapter test <name>` は記録した入力と期待する `{kind, data}` を照合します。
- **ビヘイビアツリーの並列分岐**: `parallel` ノードの子はそれぞれ専用のタスクキューで動くようになり、分岐が実際に同時実行されます。長いキュー名にはハッシュを付けてファイル名の衝突を防ぎます。
- **ドキュメントと help**: README と `hancho-cli help` に、イベントスキーマ、アダプター、`HANCHO_EVENT_PIPE`、`--schema v1`、`event emit`、`adapter test` を追記しました。

### v0.9.1
- **修正: リソース列のヘッダ色**: タスク一覧の「リソース」列のヘッダが、ほかの列と同じテーマ色（オーナードロー）で描画されます。これまではネイティブの既定色でした。
- **修正: `help --json` の MCP 自動登録コマンド**: `setup.mcp.auto_install` が `hancho-cli.exe mcp install` を案内するようになりました。以前は `Hancho.exe mcp install` と表示され、設定を書かずに MCP サーバーが起動していました。

### v0.9.0
- **ProcessCommander 2.9.x から Hancho 0.9.0 へ改名**: 製品名、実行ファイル（`Hancho.exe`）、GUI ウィンドウクラス、Named Pipe / ミューテックス名、既定のデータフォルダ（`.hancho`）、MCP サーバー名（`hancho`）、環境変数 `HANCHO_LOG_DIR` を Hancho に変更しました。コンソールクライアントは `pcder.exe` から `hancho-cli.exe` に改名しました。旧 `.processcommander` のデータや旧パイプ・ミューテックス名は自動移行しません。必要に応じて `--data-dir` で新しく作成してください。

### v2.9.3
- **どのセッションが追加したタスクか分かるように**: タスクキューの各ジョブに発行元（エージェントのセッション id、Claude デスクトップのセッション id、MCP クライアント、作業フォルダ、プロセス id）を記録します。MCP の `taskqueue_add`、`hancho-cli taskqueue add`、ビヘイビアツリーで自動的に入ります。`originLabel`、`--origin-label`、`PC_ORIGIN_LABEL` で作業に名前を付けられます。ダッシュボードの新しい **発行元** 列に表示され（マウスを乗せると全項目）、`taskqueue_list` の `origin: "self"`（`hancho-cli taskqueue list --origin self`）で自分のセッションのジョブだけを一覧できます。詳しくは [doc/taskqueue-ja.md](doc/taskqueue-ja.md) を参照してください。
- **ビヘイビアツリーの応答のトークンを削減**: ツリー全体を返すのは `bt_get` だけになりました。ほかの BT ツールと `bt_wait` の `compact: true` はツリーを省き、長い変数値とプロンプトは先頭部分だけになります。escalate の `include` に `lines` と `maxBytes` を指定できます。ワーカーのノードに `outputSchema` が渡されるようになりました。

### v2.9.1
- **監督役 LLM の `bt_wait` を軽く**: 何も起きずに時間切れになったとき、`bt_wait` はツリー全体ではなく `{state, version, nodeRuns, timedOut}` だけを返すようになりました。終了と escalate のときの応答は変わらず、すぐに返ります。
- **長く待てて取り消せる待機**: MCP の `bt_wait` は既定で最大 10 分待ちます（`timeoutMs` を負にすると無制限）。待っている間もほかの MCP 呼び出しに応答し、`notifications/cancelled` で約 2 秒以内に止まります。`hancho-cli bt wait` の既定は 60 秒のままです。

### v2.9.0
- **ビヘイビアツリー**: 監督役の LLM が、複数手順の作業全体（分岐、ビルド修正ループ、リトライ、条件の確認、ワーカー LLM の手順、判断）を 1 つの JSON のツリーとして `bt_start` で渡せるようになりました。Hancho がツリーを実行し、各手順を普通のタスクキューのジョブとして追加します。LLM が呼ばれるのは `escalate` の判断と終了時だけです。MCP ツール `bt_validate`, `bt_start`, `bt_get`, `bt_list`, `bt_wait`, `bt_resume`, `bt_retry`, `bt_update`, `bt_cancel`, `bt_pause` と `hancho-cli bt ...` コマンドを追加しました。詳しくは [ビヘイビアツリー](#ビヘイビアツリー-次に何をするかの判断を-hancho-の中で行う) と [doc/behavior-tree-ja.md](doc/behavior-tree-ja.md) を参照してください。
- **ワーカーのエンジン**: `worker` の手順は、エンジン（`claude`, `agy`, `opencode`, `codex`、またはデータディレクトリの `engines.json` の独自定義）とモデルで指定します。確認なしで動かすための引数はエンジン定義の側だけに置きます。
- **ビヘイビアツリーの表示**: ツールバーの **BT** ボタン（Ctrl+Shift+B）か、タスクの右クリックメニュー「ビヘイビアツリーで表示」で、各ツリー、どのノードがどのタスクを起動したか、どこまで進んだかを表示します。実行中のツリーが上に来て、24 時間より前に終わったツリーは「すべて表示」をオンにしない限り隠れます。
- **LLM 向けの help**: `Hancho.exe --help`、`hancho-cli help`、`hancho-cli help --json` に、ジョブを手で足す代わりにビヘイビアツリーを使う場面と使い方を書きました。
- **タスクキュー**: ビヘイビアツリーが起動したジョブに MCP や CLI から `taskqueue_retry` / `taskqueue_update` すると拒否します（`bt_retry` / `bt_update` を使います）。ダッシュボードの再実行は、その手順からツリーを再実行します。

### v2.7.2
- **実行されないジョブを残り時間から除外**: 合計残り時間（タスク残り時間の KPI とツールチップ）に、ブロックされたジョブや、上流が満たされない形で終わった保留ジョブを含めなくなりました。件数の表示は変わりません。
- **保留 / ブロック KPI のアニメーション**: 件数が 1 以上のとき、「保留 / ブロック」カードがアニメーションします。
- **OpenCode の使用量の分類**: モデル別の使用量表示で、OpenCode のトークン使用量が agy として数えられていた問題を直しました。OpenCode として記録されます。

### v2.7.1
- **リストの文字サイズ**: プロセス一覧とタスクキューの一覧で、それぞれ文字サイズ（10〜24 px）を変えられます。設定の「表示」タブで指定します。
- **行の色は「色」タブへ**: 状態別の行背景色を、設定の「表示」タブから新しい「色」タブに移しました。
- **ウィンドウタイトル**: `--data-dir` 付きで起動すると、タイトルにデータディレクトリを表示します。複数のダッシュボードを見分けられます。

### v2.7.0
- **タスクキューの時間絞り込み**: `hancho-cli taskqueue list --since 6h` で、指定期間内に更新されたジョブだけを表示します（MCP の `taskqueue_list` にも `since` を追加）。単位は m・h・d です。
- **失敗ジョブの一括再実行**: `hancho-cli taskqueue retry --failed [<キュー名>] [--since 6h]` で、失敗したジョブをまとめて再実行します。キュー名と期間で絞り込めます。MCP の `taskqueue_retry` も `id` を省いて `failed: true` を渡すと同じ一括再実行になります。
- **プロセスの後始末**: プロセスが終了したとき、残っていた子プロセス（python、cmd など）も終了させます。ジョブに子プロセスが取り残されなくなります。
- **監視ペイン**: アクティビティツリーには、サブエージェントだけを表示するようにしました。ルートエージェント自身の手順は表示しません。

### v2.5.2
- **トレイからの復帰**: Explorer を再起動した後もトレイアイコンを登録し直します。トレイアイコンを追加できない環境では、格納せず最小化します（ウィンドウに戻れなくなるのを防ぎます）。
- **クラッシュ記録**: ダッシュボード・IPC・プロセス監視・出力読み取りで発生した例外を、黙ってプログラムを終了させず監査ログ（`CRASH_GUARD`）に記録します。捕捉されなかった例外は `CRASH` として記録します。
- **終了時の点滅**: プロセス・タスクの行が終了すると数秒間点滅します。設定画面で無効にしたり、点滅時間（1〜60 秒）を変えたりできます。
- **失敗時の音**: 失敗・タイムアウト・強制終了したプロセスは、完了時とは別のシステム音を鳴らします。完了音の設定に従います。

### v2.5.1
- **行の背景色**: プロセス・タスク一覧の行を、設定画面で状態ごと（実行中・アイドル・完了・タイムアウト・失敗）に色分けできるようにしました。ダークテーマとライトテーマで別々に設定でき、文字色は背景に合わせて白か黒に自動で切り替わります。
- **`hancho-cli reset [--yes]`**: データディレクトリ内のプロジェクトデータをすべて削除します。`--yes` を付けないと削除対象を一覧表示するだけです。Hancho の実行中は実行できません。
- **`hancho-cli taskqueue purge --older-than-days N`**: N 日以上更新されていない終了済みジョブ（succeeded・failed・cancelled）を削除します。

### v2.5.0
- **ダッシュボードの更新間隔**: 既定を 2000 ms にし、設定画面で ms 単位に変更できるようにしました（100〜60000 ms）。実行中のプロセスやキューのジョブがないときは間隔を 2 倍にします。
- **ダッシュボードの再描画**: プロセス・タスク一覧と KPI カードは内容が変わったときだけ描き直すようにし、アイドル時の更新で描画する量を減らしました。
- **タスク監視**: ID 列の幅を 1.5 倍、キュー列の幅を 3/4 にしました。

### v2.3.7
- **タスク監視**: 目標欄の高さをペインの高さの半分にしました（以前の 150px の上限はなくなりました）。

### v2.3.6
- **セキュリティ**: ダッシュボード自動起動のコマンドインジェクションを修正しました。`‘` や `’` を含むパスやデータディレクトリによって、実行中のユーザーの権限で PowerShell コマンドが実行されるおそれがありました。

### v2.3.5
- **サブエージェント**: Claude のサブエージェントを個別に追跡し、タスク監視ペインにツリーで表示します。`hancho-cli worker status` と IPC の状態にも一覧が出ます。
- **ストリーム**: Claude の部分メッセージ（`--include-partial-messages`）と `codex exec --json` のイベントを解析します。
- **ツールバー**: 時計の左に更新アイコンボタンを追加しました。
- **設定**: プロセスごとに保持するストリームイベント数を変更できます（既定 2000、-1 でメモリが尽きるまで保持）。
- **ツール**: `hancho-cli worker install-template`（hancho-runner エージェント）、ww-run と ww-watch の `-Agent` / `WW_AGENT`、`tools/ww-unity` を追加しました。
- **リスト**: スクロールバーを押さえている間は更新を止め、タスクリストは更新してもスクロール位置を保ちます。
- **トークン**: Claude の使用量をメッセージごとに1回だけ数え、result 行ではメッセージに出なかった分だけを足します。

### v2.3.4
- **タスク監視**: 目標欄とプレビュー欄のスクロールバーをテーマの色に合わせました。

### v2.3.3
- **テーマ**: 濃い紫、薄い紫、濃い黄、薄い黄を追加しました。
- **リストヘッダ**: 背景にテーマのアクセント色をわずかに混ぜました。
- **スクロールバー**: プロセスリストとタスクリストのスクロールバーをダブルバッファにして、ちらつきを減らしました。
- **設定**: サウンドの項目が上のボタンと重なっていたのを直しました。

### v2.3.2
- **テーマ**: 背景が淡色のパステルテーマを5種類追加しました（パステル青、橙、桃、赤、緑）。
- **KPI カード**: 色の帯の高さを2倍にし、文字を太字にして、上下の余白を詰めました。
- **CLI**: `hancho-cli agent` を `hancho-cli worker` に改名しました。旧名も使えます。
- **MCP**: `get_agent_stream` / `get_agent_state` / `update_agent_info` を `get_worker_stream` / `get_worker_state` / `update_worker_info` に改名しました。旧名も使えますが、ツール一覧には表示されません。

### v2.3.1
- **MCP**: `run_agent` / `wait_agent` を `run_worker` / `wait_worker` に改名しました。ハーネス標準のサブエージェント機能との誤解を避けるためです。旧名も使えますが、ツール一覧には表示されません。

### v2.3.0
- **MCP**: 1つのタスクをワーカー LLM に任せ、最終結果だけを返すツールを追加しました。
- **モニターペイン**: ツール呼び出しの説明を専用の列に表示し、サブタスクリストの幅を3分の2に広げました。
- **タスク一覧**: ホバーで「リソース」の全文を表示でき、セルをコピーできます。
- **ww-run**: stdout が閉じた時点ではなく、エンジンの終了時に完了します。

それ以前のバージョン: [Releases](https://github.com/gadget114514/Hancho/releases)

## 付録: `Hancho.exe --help` の出力

```text
Hancho 0.9.3 - Native Win32 Multi-LLM Process & ConPTY Manager + Live GUI Dashboard

USAGE:
  Hancho.exe [gui] [--data-dir <path>]
      Launch the live Win32 Dashboard GUI and Named Pipe server.

  Hancho.exe quit
      Quit the running GUI, including one hidden in the system tray (its tray icon is removed).

  Hancho.exe mcp [--data-dir <path>]
      Run as a Model Context Protocol (MCP) stdio server for Claude.
      Automatically connects to (or launches) the Win32 GUI Dashboard.

  Hancho.exe --help
      Show this help.

  Hancho.exe --version
      Show the version.

OPTIONS (per launch):
  --data-dir <path>   Store logs, state, task queues and config in <path>.
                      Use a project-local directory to git-manage them. Each
                      data dir gets its own dashboard instance (separate pipe).
                      Equivalent environment variable: HANCHO_LOG_DIR.
                      Consistency: pass the same --data-dir on every call, or embed
                      it once with `hancho-cli.exe mcp install --data-dir <path>`.
                      Different data dirs are isolated dashboard instances.

WORKING MODEL (LLM orchestration):
  Hancho is the execution layer for a supervising LLM.

    Supervisor LLM (Claude Code, opencode, ...)
      |  start_process   -> one long-running worker command
      |  taskqueue_add   -> ordered/dependent worker jobs
      |  bt_start        -> a behavior tree: steps, branches, retries, checks
      v
    Hancho
      |  owns processes, timeouts, logs, retries, state
      v
    Worker LLM / build command
      -> one job command; writes artifacts and logs

  Assumed structure:
    1. The supervisor plans and declares jobs. It does not run the work itself,
       does not chain commands, and does not fabricate success.
    2. Each unit of work is one job with a one-line summary, its own timeoutSec,
       and an optional success check (filesNewer / outputRegex). Steps or
       dependencies become separate jobs linked with dependsOn.
    3. Workers are ordinary processes and may themselves be LLM CLIs. Pass
       detailed instructions via instructionsFile/instructionsText; workers read
       {instructions} / PC_JOB_INSTRUCTIONS and write artifacts.
       Role clarity: when supervisor and worker share the same engine
       (Claude/Claude, opencode/opencode), roles can blur; prefer a different
       worker engine or a clearly distinct worker role/prompt.
    4. The supervisor observes with taskqueue_wait / taskqueue_list /
       taskqueue_graph and reads logs; it never polls .done files.
    5. On failure, fix the cause and call taskqueue_retry. Blocked dependents
       resume automatically.
    6. Keep the supervisor context small: read artifacts, not raw transcripts.
    7. When the next step depends on results (build -> fix -> rebuild loops,
       test -> fix -> retest, "if this file says X then ..."), do not watch and
       add jobs by hand: write the whole plan as a behavior tree (BT) and submit
       it once with bt_start. Hancho runs it, adding each step as an
       ordinary job, and returns to you only when a decision is needed
       (escalate) or the tree finishes. See BEHAVIOR TREES below.

CONSOLE CLIENT / CLI SETUP:
  Use hancho-cli.exe for CLI commands (start, list, logs, send, wait, kill, kill-all, clear).
  It auto-starts the GUI dashboard and locates Hancho.exe in its folder.
  Run `hancho-cli.exe help` for human-readable usage, or `hancho-cli.exe help --json` for
  machine-readable commands and setup instructions for LLMs.

COMPLETION NOTIFICATION / BACKGROUND TASKS:
  `hancho-cli.exe start ... --wait --wait-ms 0 --json` keeps hancho-cli.exe alive until the
  managed process exits. `--wait-ms 0` means unlimited wait. Exit code is 0 on
  completion, the child exit code on failure, and 124 if the wait times out.
  For an existing process: `hancho-cli.exe wait --id <id> --wait-ms 0 --json`.
  Hancho also writes a .done JSON file on exit.

MCP SERVER SETUP:
  Register this executable in .mcp.json (Claude Code) or claude_desktop_config.json:
    {
      "mcpServers": {
        "hancho": {
          "command": "<absolute path to Hancho.exe>",
          "args": ["mcp"]
        }
      }
    }
  Auto-register (recommended for LLM agents):
    hancho-cli.exe mcp install                    (writes .mcp.json in the current directory)
    hancho-cli.exe mcp install --client opencode  (writes/updates opencode.json)

  PRECONDITIONS (LLM agents):
    - Windows 10/11 x64, same user session as the dashboard; no admin required.
    - The dashboard auto-starts on first use and owns the Named Pipe server.
    - Logs, state and task queues live under logs/ (override: HANCHO_LOG_DIR).

  MCP tools (34):
    Process (6):
      start_process, list_processes, get_process_output, send_process_input,
      wait_process, kill_process
    Worker (5):
      run_worker, wait_worker, get_worker_stream, get_worker_state, update_worker_info
    Task queue (13):
      taskqueue_add, taskqueue_list, taskqueue_get, taskqueue_retry,
      taskqueue_update, taskqueue_cancel, taskqueue_pause, taskqueue_resume,
      taskqueue_wait, taskqueue_purge, taskqueue_link, taskqueue_unlink,
      taskqueue_graph
    Behavior tree (10):
      bt_validate, bt_start, bt_get, bt_list, bt_wait, bt_resume, bt_retry,
      bt_update, bt_cancel, bt_pause

  HOW TO USE (decision guide):
    - One command, e.g. a build script, CLI worker or shell -> start_process
    - One task for a worker LLM, final result only         -> run_worker
      Harnesses do not see this as a native sub-agent; a thin custom agent that
      calls run_worker makes it one, at the cost of the wrapper's tokens.
    - Multiple steps, fixed order or dependencies         -> taskqueue_add + dependsOn
    - Steps whose next step depends on results: branches,
      fix loops, checks, worker LLM steps, your decisions -> bt_start + bt_wait
    - Never write &&, ||, a single &, PowerShell ';', newline chains or polling
      loops. ChainGuard rejects them: one job = one command with its own timeoutSec.
    - Required taskqueue job fields: id, command, summary, timeoutSec.
      Optional: cwd, env, dependsOn, idle, maxAttempts, retryDelaySec, success,
      killExclude, resources {write,read}, priority, instructionsFile / instructionsText.
    - resources: jobs whose write/read sets overlap never run together (read/read is
      fine). Paths/globs resolve against cwd; "@name" is a named lock.
    - Success is verified: exit code AND optional filesNewer / outputRegex.
    - States: waiting = queued and ready; pending = dependencies not satisfied;
      blocked = a dependency failed; running; succeeded; failed; cancelled.
    - Observe with taskqueue_wait / taskqueue_list / taskqueue_graph; after fixing
      the cause use taskqueue_retry (blocked dependents resume automatically).

  BEHAVIOR TREES (bt_*):
    A behavior tree is one JSON document describing the whole workflow. Leaves run
    work (job = one command, worker = one task for a worker LLM); composites decide
    what runs next (sequence, fallback, parallel, if, retry, repeat, timeout);
    conditions check results (fileExists, fileNewer, outputMatches, exitCode,
    jsonValue, var, nodeState); escalate hands a decision back to you.
    Flow:
      1. bt_validate (optional), then bt_start {bt: {...}} or {file: "plan.json"}.
      2. Loop on bt_wait {id, sinceVersion, timeoutMs}. It returns when the tree
         finishes or waits for you (state "waitingForLlm" + a decision packet).
      3. Answer with bt_resume {id, node, answer, rationale}. The packet holds the
         question, guidance, allowed answers and the logs/files/vars you asked to
         include, so you can decide even after your context was compacted.
      4. On failure read bt_get {id, compact: true}; fix the cause (bt_update for
         a non-running node's definition) and call bt_retry {id, from}.
    Rules:
      - retry / repeat need "max". Jobs need command, summary, timeoutSec; workers
        need prompt, summary, timeoutSec and optionally engine / model (claude,
        agy, opencode, codex or ones defined in <data dir>\engines.json).
      - Write "intent" (the goal) and, on escalate, "question" + "guidance" for
        your future self. ${name} expands variables; in command it is quoted as
        one argument. Do not retry BT jobs with taskqueue_retry; use bt_retry.
    Minimal example (build, and on failure let a worker fix it, up to 3 times):
      {"id": "fix-build", "summary": "Build and fix", "intent": "Release build passes",
       "cwd": "D:/repo",
       "root": {"type": "retry", "max": 3, "child": {"type": "fallback", "children": [
         {"type": "job", "id": "build", "summary": "Build", "timeoutSec": 1800,
          "command": "cmake --build build --config Release"},
         {"type": "sequence", "children": [
           {"type": "worker", "id": "fix", "engine": "claude", "summary": "Fix build",
            "timeoutSec": 3600, "prompt": "Fix the build errors in ${build.logFile}."},
           {"type": "fail"}]}]}}}
    Full format, every node type and three complete examples:
      doc\behavior-tree.md (English), doc\behavior-tree-ja.md (Japanese), in the
      Hancho installation folder. `hancho-cli.exe bt --help` lists the CLI.

  For the full machine-readable guide (agent.quick_start, decision, states):
    hancho-cli.exe help --json
```

---

## 付録: `hancho-cli.exe help` の出力

```text
hancho-cli 0.9.3 - Hancho console client

USAGE:
  hancho-cli [--json] <command> [options]

GLOBAL OPTIONS:
  --json, -j               Print machine-readable JSON instead of human-friendly text.
  -h, --help               Show this help.
  -V, --version            Show the version (also: hancho-cli version).

DATA DIRECTORY (per invocation):
  --data-dir <path>        Store logs, state, task queues and config in <path> for
                           THIS hancho-cli call. May be placed before or after the command.
                           Point it at a project folder to git-manage the data:
                             hancho-cli.exe --data-dir .hancho taskqueue add --file jobs.json
                             hancho-cli.exe taskqueue list main --data-dir .hancho
                           Each data dir gets its own dashboard instance, so different
                           projects can run at the same time.

  CONSISTENCY (one data dir per project):
    --data-dir is per invocation, not a global setting. To keep a project's
    logs/state/task queues consistent:
      1. Choose one project-local directory, e.g. .hancho in the project root.
      2. Register the MCP server once with it:
           hancho-cli.exe mcp install --data-dir .hancho
         This writes "--data-dir .hancho" into the project's MCP config,
         so every MCP tool call for the project uses the same directory.
      3. Pass the same --data-dir on every direct hancho-cli call.
      4. Relative paths resolve against the caller's current directory. MCP clients
         normally start the server in the project root; if the cwd is uncertain,
         put an absolute path in the MCP config.
      5. Different data dirs are isolated (own dashboard instance, Named Pipe and
         mutex); they never share processes or queues.
      6. Verify: hancho-cli.exe --data-dir <path> help --json prints
         "effective_data_dir", and the dashboard status bar shows the same path.

HANCHO-CLI CONCEPTS:
  Process IDs : Numeric IDs assigned by the dashboard (#1, #2, ...). Use them with
                list --id, logs --id, send --id, wait --id and kill --id.
  Modes       : pipe    = anonymous pipes (default; non-interactive CLI workers)
                pty     = Win32 ConPTY (interactive TUI: opencode, claude)
                console = separate visible console window
  stdin       : pipe mode keeps stdin writable (hancho-cli send) unless --close-stdin;
                close it for programs that wait for EOF.
  Logs        : logs/proc_<id>_<name>_<timestamp>.log (or --log-file).
  Streams     : logs/proc_<id>_<name>_stream.jsonl (raw) and
                logs/proc_<id>_<name>.events.jsonl (normalized hancho.event/1).
  Adapters    : <data dir>/adapters/<name>.json (Tier 1 declarative or Tier 3 exec).
  Done file   : atomic JSON on exit (logs/proc_<id>.done or --done-file) with
                state, exit_code, reason, log_file and done_file.
  cwd / env   : --cwd defaults to the caller's directory; PWD and INIT_CWD are
                set for the child and OLDPWD is removed.
  JSON output : --json / -j before or after the command prints one JSON object.
  Lifecycle   : start returns immediately unless --wait; use wait --id or the
                .done file to detect completion.

EXIT CODES:
  0    success / completed
  1    CLI or configuration error (also: process failure without --wait)
  124  wait timed out while the process is still running
       start --wait / wait return the managed process's exit code

ENVIRONMENT:
  HANCHO_LOG_DIR     Override the logs directory (state, logs, queues).
  HANCHO_EVENT_PIPE  Set by Hancho for pipe-mode workers; hancho-cli event emit
                     writes hancho.event/1 lines to this pipe.

EXAMPLES:
  hancho-cli.exe list --json
  hancho-cli.exe start --name build --cmd "cmd /c build.cmd" --timeout 1800 --wait --wait-ms 0 --json
  hancho-cli.exe start --name tui --pty --cmd "opencode"
  hancho-cli.exe logs --id 1 --tail 80
  hancho-cli.exe send --id 1 --input "status"
  hancho-cli.exe taskqueue add --file jobs.json --taskqueue main
  hancho-cli.exe mcp install --client opencode

COMMANDS:
  list [--id <id>]
      List all managed processes and their live metrics.

  logs --id <id> [--tail <lines>] [--wait-ms <ms>] [--raw]
      Fetch process output logs (ANSI codes stripped by default unless --raw).

  start [options] -- <command...>
      Start a managed process (e.g. claude, opencode) under a Win32 Job Object.
      Options:
        --cmd "<command>"        Command line to execute
        --name <label>           Display name on dashboard (e.g. "opencode-worker")
        --mode <pty|pipe|console>
                                 pty     = Win32 ConPTY interactive session
                                 pipe    = Anonymous pipes (default; no ConPTY)
                                 console = Separate visible console window
        --pty                    Shortcut for --mode pty (enable ConPTY).
        --open-stdin             Pipe mode only (default): keep stdin writable for hancho-cli send.
        --close-stdin            Pipe mode only: close stdin after launch so non-interactive
                                 programs (e.g. opencode run) receive EOF and can finish.
        --cwd <dir>              Working directory (default: the caller's current directory)
        --log-file <path>        Custom path for live output log
        --done-file <path>       Custom completion marker file created on exit
        --timeout <sec>          Max runtime timeout in seconds (0 = unlimited)
        --idle-timeout <sec>     Hang timeout if no output for N seconds (0 = unlimited)
        --eta <time>             Estimated end time shown in gray while running
                                 (e.g. "18:30" or "2026-10-22 18:30:00").
                                 If omitted, --timeout shows "remaining N seconds".
         --estimate-end <time>    Alias for --eta
         --agent-type <claude|agy|antigravity|opencode|codex|aider|ollama|adapter>
                                  Hint worker type for JSON stream parsing (auto-detected if omitted).
                                  A name with <data-dir>/adapters/<name>.json uses that Tier 1/3 adapter.
         --json-stream            Enable JSONL stream parsing; writes <log>.stream.jsonl and the
                                  normalized <log>.events.jsonl (hancho.event/1) for LLM workers.
         --wait                   Block until process exits (unlimited unless --wait-ms is set)
        --wait-ms <ms>           Max wait when --wait is set; initial output wait otherwise
                                 (default: 500; 0 = unlimited with --wait)

  send --id <id> --input "<text>" [--no-newline] [--wait-ms <ms>]
      Send input text to a running process's stdin or ConPTY session.

  wait --id <id> [--wait-ms <ms>]
      Wait until the specified process exits (or wait-ms elapses).

  kill --id <id>
      Forcefully kill the entire process tree (Job Object) for <id>.

  kill-all
      Forcefully kill all running managed process trees.

  clear
      Remove finished processes from the dashboard list.

  event emit --kind <kind> [--text <text>] [--tool <name>] [--activity <a>] [--target <t>]
             [--status <s>] [--data <json>] [--pipe <name>]
      Write one hancho.event/1 line to HANCHO_EVENT_PIPE (set for pipe-mode workers).

  adapter test <name> [--file <adapter.json>] [--dir <test-dir>]
      Run the Tier 1 declarative adapter against tests/adapters/<name>/*.input.jsonl and
      *.expected.jsonl (each expected line is {kind, data}).

  worker status <id>
      Show current worker status, active tool, thoughts, message, and supervisor context.

  worker stream <id> [--tail <n>] [--type <kind>] [--since <seq>] [--summary] [--schema <v1>]
      View captured JSON streaming events for claude, agy, or opencode workers.
      --schema v1 (default) returns the stable hancho.event/1 shape from <log>.events.jsonl.
      --schema legacy is not supported (removed in v0.11.0).
      --type matches the kind (tool.call, message, ...).

  worker info <id> [--notes "<notes>"] [--goal "<goal>"]
      Update supervisor notes or goal for an ongoing worker.

  agent install-template [--path <dir>] [--dry-run]
      Write the hancho-runner subagent to <dir>\.claude\agents\hancho-runner.md (default: the current
      directory). It runs long commands through Hancho and returns a short summary.
      An existing file is not overwritten.

  agent uninstall-template [--path <dir>] [--dry-run] [--delete-data-dir]
      Remove the hancho-runner subagent written by install-template (the inverse command).
      Empty .claude\agents and .claude directories are removed too. Add --delete-data-dir
      to also delete the data directory (kept by default).

  mcp install [--client claude|opencode] [--path <file>] [--name <name>] [--data-dir <path>] [--dry-run]
      Register Hancho as an MCP server automatically. Writes or updates
      .mcp.json (Claude Code, default) or opencode.json (--client opencode).
      --data-dir embeds "--data-dir <path>" in the generated MCP server args so the
      project keeps its logs/state/task queues inside that directory.
      --json prints the machine-readable result; --dry-run previews without writing.

  mcp uninstall [--client claude|opencode] [--path <file>] [--name <name>] [--data-dir <path>]
                [--delete-data-dir] [--dry-run]
      Remove the Hancho entry from the MCP config (the inverse of mcp install).
      The data directory is kept by default; add --delete-data-dir to delete it too.
      The data directory is the one embedded by mcp install, --data-dir, or the default.

LEAVING HANCHO (graduation):
  Stop using Hancho without losing your work:
    1. hancho-cli.exe mcp uninstall                  # remove MCP registration (keep data)
    2. hancho-cli.exe agent uninstall-template       # remove hancho-runner.md
    3. Delete the data directory only if you want to:
         hancho-cli.exe mcp uninstall --delete-data-dir
  While you keep it, all Hancho data stays in open formats: queues, behavior trees,
  events and memory are JSON / JSONL and readable without Hancho.
  When a harness covers your use, compare it with doc\design-v3.md sections 5.3-5.5
  (feature map and conditions) before leaving. doc\release-checklist.md records the
  per-release review of that table.

TASK QUEUE:
  taskqueue add --file <jobs.json> [--taskqueue <name>] [--concurrency <n>] [--notify-file <path>]
                [--origin-label <text>]
      Register jobs (array, or an object with "jobs"/"taskqueue"/"concurrency").
      Each job records its origin: the caller's session id (CLAUDE_CODE_SESSION_ID
      or PC_ORIGIN_SESSION), working folder and pid, plus --origin-label (or
      PC_ORIGIN_LABEL). The dashboard shows it in the Origin column.
      Each job needs id, command, summary, timeoutSec; dependsOn replaces '&&'.
      Optional: cwd, env, dependsOn, idle, maxAttempts, retryDelaySec, success,
      killExclude, resources {write,read}, defer, priority, instructionsFile/instructionsText.
      resources: jobs whose write/read sets overlap (write/write, write/read) never
      run together, across all queues; read/read may. Paths/globs resolve against
      cwd; a bare file name (Foo.cs) means **/Foo.cs; "@name" is a named lock.
      Disable with Settings / config.json resource_locks.
      defer {exitCodes,delaySec,limit} (default [75], 60, 60): an attempt that exits
      with one of exitCodes ("nothing ran, try again later", e.g. ww-run refusing an
      overlapping card) goes back to pending after delaySec without using up
      maxAttempts; after limit deferrals (0 = no limit) it is an ordinary failure.

  taskqueue list [<name>] [--state <state>] [--since <period>] [--origin <self|label|session>] [--bt <id>]
      List jobs with state, queue, summary and blockedBy. --since keeps only
      jobs updated within the period (30m, 6h, 2d), e.g. --since 6h.
      --bt <id> lists every job of a behavior tree across its parallel branch queues.
      --origin self keeps only the jobs this session added.

  taskqueue get <id> [--rev <n>] [--no-instructions]
  taskqueue retry <id>
  taskqueue retry --failed [<name>] [--since <period>]
      Retry every failed job at once, optionally only one queue and only jobs
      that failed within the period, e.g. `taskqueue retry --failed --since 6h`
      retries the failures of the last 6 hours. Blocked dependents resume.
  taskqueue cancel <id>
  taskqueue pause|resume <name>
  taskqueue graph [<name>] [--root <id>]     (ready, waiting_for, resource_wait, retry_in_sec, blocked)
  taskqueue wait <name> [--since <version>] [--timeout-ms <ms>]
  taskqueue purge <name> [--states succeeded,failed,cancelled]
  taskqueue purge [<name>] --older-than-days <N>   (finished jobs not updated for N days; all queues if <name> omitted)
  reset [--yes]                                    (delete ALL project data in the data directory; lists targets without --yes; refused while running)

BEHAVIOR TREES (for LLM agents; next step depends on results):
  bt validate|start --file <bt.json>    Check / register and run a behavior tree
  bt wait --id <id> [--since-version <n>] [--timeout-ms <ms>]
      Long-poll until the tree finishes or waits for your decision (escalate).
  bt resume --id <id> --node <node> --answer <a> --rationale <why> [--vars <json>]
  bt get --id <id> [--compact]  |  bt list
  bt retry --id <id> [--from <node>] [--reset-counters]
  bt update --id <id> [--node <node>] (--file <path> | --bt <json> | --patch <json>)
  bt cancel --id <id>  |  bt pause --id <id> [--paused true|false]
      Leaves: job (one command), worker (one task for a worker LLM; engine/model);
      composites: sequence, fallback, parallel, if, retry/repeat (max required),
      timeout; conditions: fileExists, fileNewer, outputMatches, exitCode,
      jsonValue, var, nodeState; escalate returns a decision packet to you.
      Each leaf runs as an ordinary task-queue job; retry BT jobs with bt retry,
      not taskqueue retry. Full format and examples: doc\behavior-tree.md in the
      installation folder; `hancho-cli bt --help` for all options.

  start/run ... --allow-chain
      Allow a chained command (&&, ||, &, ';') as a single process. Task-queue
      jobs never allow chains; use dependsOn instead.

COMPLETION / BACKGROUND TASKS:
  Use --wait to keep hancho-cli.exe alive until the managed process exits. Add
  --wait-ms 0 for an unlimited wait (or a positive value for a maximum).
  The exit code is 0 on completion, the child exit code on failure, and 124
  if the wait times out.

    hancho-cli.exe start --name task --wait --wait-ms 0 --json --cmd "opencode run ..."
    hancho-cli.exe wait --id <id> --wait-ms 0 --json

  Hancho also writes a .done JSON file on exit. Without --wait,
  start returns immediately; poll the .done file or call wait.

SETUP FOR LLM AGENTS:
  CLI:
    Use hancho-cli.exe (recommended) or the legacy "Hancho.exe <command>" form.
    Run "hancho-cli.exe help --json" to retrieve this help as machine-readable JSON.
    hancho-cli.exe auto-starts the GUI dashboard on first use if it is not running.

  MCP (stdio):
    Register Hancho.exe in .mcp.json (Claude Code) or
    claude_desktop_config.json (Claude Desktop):
      {
        "mcpServers": {
          "hancho": {
            "command": "D:\\wsgit\\Hancho\\build\\Release\\Hancho.exe",
            "args": ["mcp"]
          }
        }
      }

    AUTO-REGISTER (for LLM agents; no manual JSON editing):
      hancho-cli.exe mcp install                    # .mcp.json (Claude Code)
      hancho-cli.exe mcp install --client opencode  # opencode.json
      hancho-cli.exe mcp install --path <file> --json --dry-run

    ORCHESTRATION MODEL (how an LLM should use this):
      A supervising LLM uses Hancho as its execution layer to run
      child-process worker LLMs/CLIs as managed jobs.

        Supervisor LLM  ->  Hancho  ->  Worker LLM / build command
        plans + declares    timeouts, logs,        one job command;
        jobs                retries, state         writes artifacts

      - The supervisor keeps its context small: it plans and declares jobs; it
        does not run the work itself, chain commands, or fabricate success.
      - One job = one command. Use start_process for a single worker; use
        taskqueue_add with dependsOn for multiple steps or dependencies.
      - When the next step depends on results (build -> fix -> rebuild,
        test -> fix -> retest, branch on a file's content), do not watch and
        add jobs by hand: write the plan as a behavior tree and submit it once
        with bt_start, then loop on bt_wait and answer escalations with
        bt_resume (answer + rationale). See BEHAVIOR TREES above and
        doc\behavior-tree.md.
      - Each job needs a one-line summary and its own timeoutSec. Pass detailed
        instructions via instructionsFile/instructionsText; the worker reads
        {instructions} / PC_JOB_INSTRUCTIONS and writes artifacts.
      - Workers may be LLM CLIs (opencode run, claude -p, ...) or build scripts.
      - Role clarity: when the supervisor and worker use the same engine
        (e.g. Claude supervising `claude -p`, or opencode supervising
        `opencode run`), roles can blur and cause confusion; prefer a
        different worker engine for workers, or give workers a clearly
        distinct role/prompt and avoid nested supervision.
      - Verify success with exit code + filesNewer/outputRegex; observe with
        taskqueue_wait / taskqueue_list / taskqueue_graph and logs; retry with
        taskqueue_retry. Blocked dependents resume automatically.
      - Never poll .done files; use taskqueue_wait or wait_process.
      - Delegating to a worker LLM: run_worker blocks until the worker exits and
        returns only its final result; the raw stream stays in stream_file.
      - Your harness does not see Hancho as a native sub-agent. To have
        it shown as one, define a thin custom agent (Claude Code:
        .claude/agents/<name>.md, cheap model, this tool only) that calls run_worker
        and returns the result. The wrapper costs extra tokens; calling run_worker
        directly is cheapest.

    MCP tools (34):
      Process:    start_process, list_processes, get_process_output,
                  send_process_input, wait_process, kill_process
      Worker:     run_worker, wait_worker, get_worker_stream, get_worker_state,
                  update_worker_info
      Task queue: taskqueue_add, taskqueue_list, taskqueue_get, taskqueue_retry,
                  taskqueue_update, taskqueue_cancel, taskqueue_pause, taskqueue_resume,
                  taskqueue_wait, taskqueue_purge, taskqueue_link, taskqueue_unlink,
                  taskqueue_graph
      Behavior tree: bt_validate, bt_start, bt_get, bt_list, bt_wait, bt_resume,
                  bt_retry, bt_update, bt_cancel, bt_pause

NOTE:
  The GUI dashboard is provided by Hancho.exe, which is auto-started
  on first use if it is not already running.
```
