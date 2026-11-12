# 062b — TUI の一覧・監視・診断・メモリの表示

Estimate 75 min
Splittable: no
Parent: 062-linux-tui.md

## 前提

- `062-1-tui-design.md`（骨格が動くこと）

## 目的

ダッシュボードと同じ情報を端末に出す（`doc/design-v3-todo.md` 8）。

## やること

- [ ] プロセス一覧、タスクキュー、実行状態監視ペインを端末に表示する。
- [ ] 失敗診断（`taskqueue why` と同じ内容）とメモリ一覧を見られるようにする。

## 完了条件

- 端末でダッシュボードと同じ情報が見られる。

## 参照

- `doc/design-v3-todo.md` 8
- 親カード `ww-data/tasks/062-linux-tui.md`

## Authorised edits

- `src/cli.cpp`
- `src/cli_top*`
- `tests/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`
- `src/gui_dashboard.cpp`

## 検証

```powershell
# Windows ではビルドと既存テストのみ（POSIX 環境で動作確認する）
cmake --build build --config Release
build\Release\posix_core_smoke.exe
```
