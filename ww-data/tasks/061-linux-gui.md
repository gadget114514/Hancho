# 061 — Linux / macOS 向け GUI ダッシュボード

Estimate 480 min
Splittable: no

## 目的

Linux / macOS 向けのダッシュボード GUI を作る（`doc/design-v3-todo.md` 8）。設計文書 §2.6 の非目標は取り消し済み。

## やること

- [ ] GUI フレームワークを選定する。候補は Qt、GTK、Tauri など。IPC クライアントとしての実装、配布のしやすさ、依存の重さで比べ、決定を文書化する。
- [ ] 選定したフレームワークで、プロセス一覧、タスクキュー、実行状態監視ペイン、失敗診断、メモリ一覧を実装する。
- [ ] コア（`Hancho --headless`）の IPC クライアントとして動かし、GUI を閉じてもジョブが止まらない構成にする。
- [ ] ビルド手順と配布方法（パッケージ形式）を文書化する。
- [ ] 機能の範囲は Windows 版を基準に、Linux / macOS の実情に合わせて削る（削ったものは文書に書く）。

## 完了条件

- Linux と macOS で GUI が起動し、ジョブの一覧・監視・操作ができる。

## 参照

- `doc/design-v3-todo.md` 8
- `doc/design-v3.md` 2.4

## Authorised edits

- `src/gui_<選定したフレームワーク>*/`
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
