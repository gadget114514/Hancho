# 061 — Linux / macOS 向けダッシュボード（ブラウザ UI）

Estimate 230 min
Splittable: no

## 目的

Linux / macOS 向けのダッシュボードを **ブラウザ UI** で作る（`doc/design-v3-todo.md` 8）。ネイティブ GUI（Qt、GTK、Tauri など）は **ペンディング**。設計文書 §2.6 の非目標は取り消し済み。

## 前提

- `058-1-ws-multiplex.md`、`058-2-ws-subscribe.md` — WebSocket 1 本での複数要求と購読（ブラウザの接続経路）

## やること

子カードに分解した。個々の作業は子カードで管理する:

- [ ] `ww-data/tasks/061-1-browser-ui-approach.md` — ブラウザ UI の方式の確定
- [ ] `ww-data/tasks/061-2-browser-ui-skeleton.md` — localhost の HTTP + WebSocket の骨格
- [ ] `ww-data/tasks/061-3-browser-ui-panes.md` — 5 機能の実装
- [ ] `ww-data/tasks/061-4-browser-ui-package.md` — ビルドと配布の文書化

## 完了条件

- Linux と macOS のブラウザで、ジョブの一覧・監視・操作ができる。ブラウザを閉じてもジョブは止まらない。

## 参照

- `doc/design-v3-todo.md` 8
- `doc/design-v3.md` 2.4

## Authorised edits

- `src/`
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
build\Release\headless_smoke.exe
```
