## Hancho v0.10.2

### Changes
- **Task list tree stays intact**: an explicit direction record (not an implicit `job:` direction) now always gets a heading, even after its behavior tree has finished and it has only one job. The task → direction → job shape stays visible; the "Running" column continues to show only work that is currently running.
- **Task list refresh fix**: changed cells are no longer blanked out by the in-place refresh that replaced the full rebuild.
- **Installer copyright**: the setup executable carries `Copyright (c) 2026 gadget114514, 2nek`.

### 変更（日本語）
- **タスク一覧のツリー形状を維持**: 明示的な direction レコード（`implicit` の `job:` 行ではないもの）は、BT が終了してジョブが1本だけになっても見出しを残します。タスク → direction → ジョブの階層が見え続け、「実行中」列は今実行中のものだけを表示します。
- **タスク一覧の更新修正**: 全行の作り直しをやめた差分更新で、変化したセルが空になる問題を修正しました。
- **インストーラーの copyright**: `Copyright (c) 2026 gadget114514, 2nek` を記載しています。
