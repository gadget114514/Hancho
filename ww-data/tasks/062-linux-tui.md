# 062 — TUI ダッシュボード（hancho-cli top）

Estimate 310 min
Splittable: no

## 目的

`hancho-cli top` を簡易表示のままにせず、ダッシュボードと同等の端末向け画面に育てる（`doc/design-v3-todo.md` 8）。

## やること

子カードに分解した。個々の作業は子カードで管理する:

- [ ] `ww-data/tasks/062-1-tui-design.md` — TUI の実装方法の決定と骨格
- [ ] `ww-data/tasks/062-2-tui-display.md` — TUI の一覧・監視・診断・メモリの表示
- [ ] `ww-data/tasks/062-3-tui-ops.md` — TUI の操作とリモート表示
- [ ] `ww-data/tasks/062-4-tui-verify.md` — TUI の POSIX 確認手順とスモークテスト

## 完了条件

- 端末だけで、ダッシュボードと同じ見通しでジョブを監視・操作できる。

## 参照

- `doc/design-v3-todo.md` 8
- `doc/design-v3.md` 2.4

## Authorised edits

- `src/cli.cpp`
- `src/cli_top*`
- `CMakeLists.txt`
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
