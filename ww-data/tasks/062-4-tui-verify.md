# 062d — TUI の POSIX 確認手順とスモークテスト

Estimate 80 min
Splittable: no
Parent: 062-linux-tui.md

## 前提

- `062-3-tui-ops.md`（機能がそろっていること）

## 目的

POSIX 環境での確認と、TUI の自動検証を用意する（`doc/design-v3-todo.md` 8）。

## やること

- [ ] POSIX 環境での動作確認の手順を文書化する。
- [ ] スモークテストを追加する（出力の成型部分を単体テストできる構成にする）。

## 完了条件

- 出力の成型部分が単体テストで検証される。

## 参照

- `doc/design-v3-todo.md` 8
- 親カード `ww-data/tasks/062-linux-tui.md`

## Authorised edits

- `src/cli_top*`
- `tests/`
- `doc/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\posix_core_smoke.exe
```
