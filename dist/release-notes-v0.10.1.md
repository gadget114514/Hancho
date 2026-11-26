## Hancho v0.10.1

### Changes
- **Task list refresh**: when the row count is unchanged, the 5-second refresh now updates only the changed cells instead of recreating every item, and no longer resets column widths. Duplicate task numbers or ids no longer make the list clear and redraw on each tick.
- **Routine JSON viewer**: the Routine Management dialog is now wider and has a read-only, monospace BT JSON viewer on the right. The BT definition preview is no longer mixed into the details pane.
- **Installer finish page**: the installer no longer launches Hancho after installation. The finish page now says "Ask your agent, find hancho and read its help to start".
- **CLI help completeness**: all `bt` subcommands (`tasks`, `template`, `remove` / `rm`) are now listed in the plain help, `bt --help`, and the JSON usage output.
- **Task-first guidance**: the Hancho skills (English and Japanese) now require creating a task before submitting work and attaching that task id to every routine, BT and job submission. A job submitted without a task becomes an implicit task with no task record or direction, and the dashboard shows it as a job.

### 変更（日本語）
- **タスク一覧の更新**: 行数が変わらない場合、5 秒ごとの更新は変化したセルだけを書き換え、全行を作り直さなくなりました。カラム幅も更新のたびにリセットされません。タスク番号や ID が重複していても一覧が消えて描き直される現象は起きません。
- **ルーチンの JSON ビューワ**: ルーチン管理ダイアログを広げ、右側に読み取り専用の BT JSON ビューワ（Consolas、スクロール付き）を追加しました。BT 定義プレビューは詳細欄から分離しています。
- **インストーラーの完了画面**: インストール後に Hancho を起動しなくなりました。完了画面に「Ask your agent, find hancho and read its help to start」を表示します。
- **CLI help の整備**: 全 `bt` サブコマンド（`tasks`、`template`、`remove` / `rm`）を通常 help、`bt --help`、JSON 形式の help のすべてに載せました。
- **タスク優先の指示**: Hancho skill（英日）に、作業を投入する前にタスクを作り、routine / BT / ジョブのすべてにその id を付けるルールを追加しました。task なしの投入は暗黙タスクになり、タスクレコードも direction も作られず、一覧では「ジョブ」と表示されます。
