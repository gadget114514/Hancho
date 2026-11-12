# 060b — GUI の状態読み取りの IPC 置き換え

Estimate 70 min
Splittable: no
Parent: 060-gui-ipc-client.md

## 前提

- `060-1-gui-ipc-requests.md`（IPC 要求がそろっていること）

## 目的

GUI が直接読んでいるコアの状態を、IPC（Named Pipe 越し）に置き換える（`doc/design-v3.md` 2.3 段階 A）。

## やること

- [ ] 置き換えをプロセス一覧、タスクキュー、監視ペイン、失敗診断、メモリ一覧の順に進める。直接の状態参照を IPC の呼び出しに替える。

## 完了条件

- GUI がコアの IPC クライアントとして、五つの機能を従来どおり出す。

## 参照

- `doc/design-v3.md` 2.3, 2.4
- 親カード `ww-data/tasks/060-gui-ipc-client.md`

## Authorised edits

- `src/gui_dashboard.cpp`
- `src/ipc_client.cpp`
- `tests/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\headless_smoke.exe
```
