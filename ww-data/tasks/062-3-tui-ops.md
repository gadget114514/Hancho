# 062c — TUI の操作とリモート表示

Estimate 75 min
Splittable: no
Parent: 062-linux-tui.md

## 前提

- `062-2-tui-display.md`（表示が動くこと）

## 目的

端末からジョブを操作でき、リモートのジョブも同じ画面で扱えるようにする（`doc/design-v3-todo.md` 8）。

## やること

- [ ] ジョブの操作（停止、再実行、取り消し）をキーボードから行えるようにする。
- [ ] リモートのジョブも同じ画面に出す。

## 完了条件

- 端末だけでジョブの監視と操作ができる。

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
