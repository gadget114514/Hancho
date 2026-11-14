# 概要

**[English](overview.md) | [日本語](overview-ja.md)**

Hancho が supervisor LLM と worker プロセスの間で何をするか、主な機能の案内です。

## Express

- [Releases](https://github.com/gadget114514/Hancho/releases) ページを開き、最新の `Hancho-vx.y.z-win64-setup.exe` をダウンロードしてインストール（ポータブル版: `Hancho-vx.y.z-win64.zip`）
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
- 順序・依存・リトライのある複数ステップには、[タスクキュー](taskqueue-ja.md)を使ってください。

---

## 指定タスクの終了後に PC をサスペンドする

夜間のビルドや長時間のエージェント実行が終わったあと、PC を自動でスリープさせられます。対象ジョブが終了し、**他の全ジョブと Hancho 管理プロセスもアイドル**になってから猶予時間の後にサスペンドします。

```powershell
hancho-cli.exe power suspend --after-job final-build --delay-sec 30
hancho-cli.exe power status    # 他の実行数と残り秒数
hancho-cli.exe power cancel    # 解除
```

ジョブ登録時に `"suspendWhenDone": true, "suspendDelaySec": 30` を付けることもできます。MCP の `power_suspend` / `power_cancel` / `power_status` も同じ機能を提供します。`--hibernate` で休止、`--dry-run` で実行前確認ができます。

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

- **[ビヘイビアツリーガイド](behavior-tree-ja.md)** ([English](behavior-tree.md)): JSON 定義形式、全ノード種類、変数展開、ワーカーエンジン、`escalate` 判断パケット、完全なサンプル 3 例を含む総合ガイド。
- **[トークン消費削減ガイド](behavior-tree-token-optimization-ja.md)** ([English](behavior-tree-token-optimization.md)): 中間ステップのトークン消費ゼロ化、`compact: true` による応答軽量化、`escalate` の行数・サイズ制限など、トークン効率を最大化する設計とベストプラクティス。
- **[設計・実装計画書](behavior-tree-plan.md)** ([English](behavior-tree-plan-en.md)): ビヘイビアツリーの設計思想、状態マシンとタスクキューの統合、GUI 連携計画をまとめた設計書。

---

## 安定したイベントとアダプター

Hancho は生のストリームの隣に、版番号つきのイベント列を書き出します。ワーカープロセスの `logs/proc_<id>_<name>_stream.jsonl` は CLI の生の行を、`logs/proc_<id>_<name>.events.jsonl` は正規化した `hancho.event/1` イベント（`tool.call`、`tool.result`、`usage`、`run.ended` など）を持ちます。スキーマは [schema/hancho.event.v1.json](schema/hancho.event.v1.json) で配布します。

- `hancho-cli worker stream <id>` は `.events.jsonl` の正規化イベント（`hancho.event/1`）を表示します。
- MCP の `get_worker_stream` は同じ形式（`hancho.event/1`）を返します。
- C++ を書かずに CLI を追加できます。`<data-dir>/adapters/<name>.json` を置く（Tier 1: `match`、`rules`、JSON Pointer の `map`）か、`exec` に変換プロセスを指定します（Tier 3: stdin に生の行、stdout に `hancho.event` 行）。
- `engines.json` に `adapter` と `capabilities`（`jsonStream`、`resume`、`reportsUsage`、`reportsSubagents`、`permissionModes`）を書けます。
- ラッパースクリプトから直接イベントを出せます。pipe モードのワーカーには `HANCHO_EVENT_PIPE` が渡され、`hancho-cli event emit --kind message --text "..."` で書き込めます。
- `hancho-cli adapter test <name>` は `tests/adapters/<name>/` の入力と期待する `{kind, data}` を照合します。

**イベント形式と旧形式の削除。** `hancho-cli worker stream` と MCP `get_worker_stream` は `hancho.event/1` を返します。以前の内部 `type` 文字列は v0.9.3 で非推奨になり、**v0.11.0** で削除されました。`--schema legacy`（CLI）または `schema: "legacy"`（MCP）を指定すると非対応エラーとなります。移行記録と `type` → `kind` の対応表は [LLM-jsonstream-format.md](LLM-jsonstream-format.md#legacy-type-strings-and-migration-to-hanchoevent1) にあります。

## Hancho をやめるとき（卒業）

Hancho は目的ではなく手段です。[Hancho v3 設計 §5.2](design-v3.md) の理由を harness 側が満たしたら、作業を失わずにやめられます。

1. `hancho-cli.exe mcp uninstall` で `.mcp.json` / `opencode.json` から Hancho の項目を外します。
2. `hancho-cli.exe agent uninstall-template` で `.claude\agents\hancho-runner.md` を消します。
3. データディレクトリは既定で残ります。消す場合はどちらかのコマンドに `--delete-data-dir` を付けます。

Hancho のデータはすべて JSON / JSONL（タスクキュー、ビヘイビアツリー、ストリームイベント、意味的メモリ）で、Hancho がなくても読めます。やめる前に [design-v3.md §5.3–5.5](design-v3.md) の機能対応表と置き換え条件を確認してください。この表はリリースごとに[リリースチェックリスト](release-checklist-ja.md)で見直します。

