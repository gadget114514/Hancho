# 060a — GUI が必要な IPC 要求の整備

Estimate 75 min
Splittable: no
Parent: 060-gui-ipc-client.md

## 目的

GUI が直接読んでいるコアの状態を一覧化し、足りない IPC 要求を出そろえる（`doc/design-v3.md` 2.3 段階 A）。

## やること

- [ ] GUI が直接読んでいるコアの状態を一覧化する（プロセス一覧、タスクキュー、監視ペイン、失敗診断、メモリ一覧、テーマ以外の設定）。
- [ ] IPC にない要求を足す。Named Pipe 越しの既存 IPC に、欠けている要求を追加する。

## 完了条件

- GUI が必要な情報が全部 IPC で取れる。直接参照する状態の目録が空になる。

## 参照

- `doc/design-v3.md` 2.3, 2.4
- 親カード `ww-data/tasks/060-gui-ipc-client.md`

## Authorised edits

- `src/ipc_server.cpp`
- `src/ipc_client.cpp`
- `tests/`
- `doc/`

## Read-only

- `src/gui_dashboard.cpp`
- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\headless_smoke.exe
```
