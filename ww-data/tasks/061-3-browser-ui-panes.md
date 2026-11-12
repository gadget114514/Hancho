# 061c — ブラウザ UI の 5 機能の実装

Estimate 80 min
Splittable: no
Parent: 061-linux-gui.md

## 前提

- `061-2-browser-ui-skeleton.md`（接続と 1 画面の表示が動くこと）

## 目的

Windows 版ダッシュボードの主要機能をブラウザ UI に実装する（`doc/design-v3-todo.md` 8）。

## やること

- [ ] プロセス一覧、タスクキュー、実行状態監視ペイン、失敗診断、メモリ一覧を HTML/JS で実装する。
- [ ] 機能の範囲は Windows 版を基準に、ブラウザ UI の実情に合わせて削る（削ったものは文書に書く）。

## 完了条件

- Linux と macOS のブラウザで、ジョブの一覧・監視・操作ができる。

## 参照

- `doc/design-v3-todo.md` 8
- 親カード `ww-data/tasks/061-linux-gui.md`

## Authorised edits

- `src/webui*/`
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
build\Release\headless_smoke.exe
```
