# ルーチン — 名前を付けて繰り返す BT ワークフロー

**ルーチン**は、ビヘイビアツリーの定義を名前を付けて保存したものです。同じ計画を BT を書き直さずに何度でも実行できます。ルーチンは手で書くほかに、worker LLM に作らせることもできます（`routine ask`）。どちらの場合も承認済みのツリーはジョブと `foreach` ノードだけで構成され、実行ごとに新しい ID が割り当てられるためジョブ ID が衝突しません。LLM が作った定義は常に**提案**として扱われ、人が承認するまで実行されません。

「全ターゲットをビルドして全 `*_smoke` を走らせる」「生成物を作り直す」「リリース確認を流す」のような、リポジトリに固定された作業に向いています。リポジトリ内の例は `.hancho/routines/smoke-all.json` です。

## 定義の置き場所

| スコープ | 定義ファイル | 備考 |
| :--- | :--- | :--- |
| `project` | `<cwd>/.hancho/routines/<name>.json` | リポジトリで管理する。`.hancho/` 全体を無視していても `.hancho/routines/` だけは Git に含める。保存時の cwd（プロジェクトルート）が定義に記録される。 |
| `data` | `<data-dir>/routines/<name>.json` | マシンローカル。個人的な定型作業向け。 |

同じ名前が両方のスコープにある場合は **project が優先**されます。実行記録は常に `<data-dir>/routines/<name>.runs.json`（直近 50 件）に保存され、実行 ID、状態、入力差分、ノード別結果が残ります。

定義ファイルには `name`、`summary`、`cwd`、`bt`（通常の BT JSON）、`inputs`（鮮度チェック用 glob）、`inputsSnapshot`（保存時点のパス → SHA-256）、`foreachSnapshot`、`notes`、`history`（`{at, by, reason}`）が入ります。`bt` は `bt validate` とまったく同じバリデータで検証されます。

## CLI

```powershell
hancho-cli.exe routine list
hancho-cli.exe routine show smoke-all
hancho-cli.exe routine check smoke-all
hancho-cli.exe routine run smoke-all --wait --timeout-ms 3600000
hancho-cli.exe routine save --name smoke-all --file plan.json --inputs CMakeLists.txt --inputs "tests/*.cpp" --summary "Build and smoke" --project --reason "initial"
```

| コマンド | 内容 |
| :--- | :--- |
| `list` | 名前、summary、スコープ、最終実行、鮮度状態（`[STALE]` と差分件数）。 |
| `show <name>` | 定義全体、最終実行のノード別結果（状態・時間・テスト結果集計）、前回比の `newlyFailed` / `fixed`、入力差分。 |
| `run <name> [--wait] [--timeout-ms <n>] [--task <id>]` | 保存済み BT を新しい実行 ID とタスクキューで起動。実行は `--task` の下に 1 つの direction として載り、省略時は `Routine: <name>` というタスクを自動生成してぶら下げる。`--wait` は完了まで待ち、失敗時は非ゼロ終了。 |
| `save --name ... --file ... [--inputs <glob> ...] [--summary ...] [--project\|--data] --reason ...` | 登録／更新。BT を検証し、両スナップショットを更新して `history` に 1 行追加する。必ず `--reason` を付ける。 |
| `check <name>` | 現在の入力ファイルと保存時スナップショットを比較し、`state: ok` / `stale` と `added` / `removed` / `changed` / `absorbed` を返す。 |
| `ask "<やりたいこと>" [--name <n>] [--cwd <dir>] [--wait] [--scope ...]` | worker LLM にルーチンの作成（`--name` があれば修正）を依頼する。検証後、提案として保存される。 |
| `approve <name> --user-said "<ユーザーの言葉>"` | 提案を承認する（`approvedVia: chat`）。空の `--user-said` は拒否扱い。 |
| `reject <name> --reason "<理由>"` | 提案を破棄し、理由を履歴に残す。 |
| `save --name <n> --from-bt <btId> [--project\|--data]` | 過去の BT（`<data-dir>/behaviortrees/<btId>.json`）の定義を承認済みルーチンとして登録する。 |

スクリプトから使う場合は `--json` を付けます。全オプションは `routine --help` で確認できます。

## `routine ask`：LLM による提案

`routine ask` はタスクキュー `routine-ask` の worker ジョブとして実行されます。ダッシュボードに表示され、失敗は通常の診断・triage の対象です。worker は承認済み・有効な `worker` LLM リストを rank 順に選び、故障中（091-6）のエンジンは飛ばします。リストが空の場合は既定エンジンを使い、結果に警告を付けます。

プロンプトには、依頼文、`cwd`、ルーチンの形式、`bt template test-suite` の手本、既存ルーチン一覧が入ります。修正時はさらに現在の定義、`routine check` の差分、最終実行のノード表、失敗ノードのログ末尾が渡されます。worker は `{name, summary, bt, inputs, notes, rationale}` を `PC_BT_RESULT_FILE` に書き込み、`rationale` で承認者に理由を説明します。Hancho 側で次の検証を行います。

- `bt validate --strict` 相当の厳格検証
- `foreach` の glob を実際に展開してジョブ一覧を作るドライラン（`dryRun.count` とコマンド）
- ChainGuard（コマンド連結の禁止）と `worker` / `escalate` ノードの拒否

検証に失敗した場合はエラーを付けて worker に**1 回だけ**直させます。それでも不合格ならジョブは失敗し、理由を結果に出します。

合格した定義は `<dir>/.proposals/<name>.json` に `status: proposed` で保存されます。更新の場合は承認済みの `<name>.json` を変更しないため、提案が保留中の間も `routine run` は旧版を使い続けます。`routine show` には提案、ドライランのジョブ一覧、`rationale` が表示されます。提案しかないルーチンは `routine run` が拒否します。ダッシュボードの承認ボタン（`approvedVia: dashboard`）または `routine approve --user-said`（`approvedVia: chat`）で承認すると承認済み定義に昇格し、`routine reject` で破棄できます。履歴には `by`（クライアント、engine/model）と `reason` が残ります。

人が `routine save`（CLI／ダッシュボード）で保存したものは従来どおり承認済みです。MCP（LLM）からの `routine_save` は提案になります。

## MCP

同じ操作が MCP ツール `routine_list`、`routine_show`、`routine_run`、`routine_save`、`routine_check`、`routine_ask`、`routine_approve`、`routine_reject` として公開されています。supervisor LLM はシェルを使わずにワークフローの実行やルーチンの作成・承認／却下ができます。`routine_run` の `task` は CLI の `--task` と同じ意味で、応答に `taskId` が入ります。

## 実行 ID とジョブ

`routine run` ごとに `<name>-<yyyymmdd-hhmmss>`（衝突時は連番付き）を生成し、タスクキュー `routine-<name>` を使います。この ID が BT の `id` に書き込まれるため、ツリーが作るジョブはすべてこの接頭辞を持ちます。実行は 1 つの direction で、`--task` / `task` で指定したタスク、または省略時に自動生成されたタスクの下に載ります。`routine show` とダッシュボードはこの ID で結果をまとめ、直近 50 件を保持します。

## `stale` と `absorbed`

保存時に `inputs` の glob を展開してハッシュを記録します。`check` / `list` / `run` / `show` で現在のファイルと比較します。

- `added` / `removed` / `changed` があればルーチンは **stale**（依存物が変わった状態）です。
- `foreach` ノードの `glob`（`exclude` を考慮）の下で増減したファイルは、stale ではなく **absorbed** として別表示になります。ツリーが実行時に展開するためです。たとえば `tests/*_smoke.cpp` が 1 つ増えても `smokes` の子が増えるだけです。

stale でも実行は止まりません。成功した場合はスナップショットを更新し、`by: routine-run` の履歴を追加します。stale のまま失敗した場合は、triage に永続的な `routine_stale_failure` カードを作り、推奨アクションを **`update_routine`** とします。これを選ぶとそのルーチンに対して `routine ask`（直す）が起動し、失敗ノードとログ末尾がプロンプトに渡されます。結果は承認待ちの提案となり、カードに `routineAsk` / `proposalPending` として記録されます。手で直す場合は `routine show` と差分を見て `routine save --reason "..."` を使います。stale でない失敗は通常のジョブ失敗カードで扱います。

## ダッシュボード

Web UI に **Routines** タブがあります。一覧（鮮度・最終実行状態）、`Run` ボタン（`routine_run` IPC 経由）、最終実行のノード別結果、テスト結果集計、`newlyFailed` / `fixed`、stale 差分を表示します。**LLM に頼む** 入力欄で新規作成を依頼し、各行の **LLM に直させる** ボタンで修正を依頼できます。提案中は `[提案中]` と表示され、詳細欄に依頼文・rationale・ドライランのジョブ一覧・Approve/Reject ボタンが並びます。ネイティブの Win32 ダッシュボードに専用ページはなく、ルーチン操作は Web UI 側で行います。

## 関連ドキュメント

- [ビヘイビアツリー](behavior-tree-ja.md) — BT の JSON 形式、`foreach`、`${foreach:...}`、変数、ノードの種類。
- [タスクキュー](taskqueue-ja.md) — ルーチンが起動したジョブのキューイングとリトライ。
- [トリアージ](triage.md) — 失敗カードと判断（`update_routine` を含む）。
