# 058a — WebSocket 1 本での複数要求

Estimate 75 min
Splittable: no
Parent: 058-remote-stream.md

## 目的

リモートとの WebSocket 1 接続で、複数の要求を同時に運べるようにする（`doc/design-v3.md` 2.5）。

## やること

- [ ] WebSocket 1 接続で複数の要求を運べるようにする（要求 id の照合）。

## 完了条件

- 1 接続で同時に複数の要求を出し、応答を要求 id で突き合わせられる。

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
