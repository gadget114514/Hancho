# 062a — TUI の実装方法の決定と骨格

Estimate 80 min
Splittable: no
Parent: 062-linux-tui.md

## 目的

`hancho-cli top` の育て方を決め、画面の骨格を作る（`doc/design-v3-todo.md` 8）。

## やること

- [ ] TUI の実装方法を決める（ライブラリを使うか自前で書くか）。決定を文書化する。
- [ ] 画面の更新ループとキー入力の骨格を作る。

## 完了条件

- 端末で骨格が動く。決定の理由が文書に残る。

## 参照

- `doc/design-v3-todo.md` 8
- 親カード `ww-data/tasks/062-linux-tui.md`

## Authorised edits

- `src/cli.cpp`
- `src/cli_top*`
- `doc/`
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
