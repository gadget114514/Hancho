# 058b — 購読アクションと seq による再開

Estimate 80 min
Splittable: no
Parent: 058-remote-stream.md

## 前提

- `058-1-ws-multiplex.md`（1 接続で複数要求ができること）

## 目的

リモートのログとイベントの追記を、手元に順序どおり流し続ける（`doc/design-v3.md` 2.5）。

## やること

- [ ] 購読のアクションを追加する。リモートの `.stream.jsonl` / `.events.jsonl` / `.tail.txt` への追記を、手元に順序どおり流す。
- [ ] 購読の切断と再購読（再開位置は seq で指定）を行えるようにする。

## 完了条件

- 追記が欠落と重複なしで手元に届き、再購読で続きから受け取れる。

## 参照

- `doc/design-v3.md` 2.5
- `doc/remote-hosts.md`
- 親カード `ww-data/tasks/058-remote-stream.md`

## Authorised edits

- `src/ipc_server_remote.cpp`
- `src/remote_client.cpp`
- `tests/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\bt_ipc_smoke.exe
```
