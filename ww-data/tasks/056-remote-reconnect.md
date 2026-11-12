# 056 — リモートプロセスへの再接続

Estimate 155 min
Splittable: no

## 目的

手元のダッシュボードを再起動しても、リモート実行中のジョブを追い続ける（`doc/design-v3.md` 2.5、`doc/design-v3-todo.md` 6）。

## やること

子カードに分解した。個々の作業は子カードで管理する:

- [ ] `ww-data/tasks/056-1-remote-reconnect-path.md` — リモート再接続の対応表と接続経路
- [ ] `ww-data/tasks/056-2-remote-reconnect-restore.md` — 再接続後の状態復元と文書・スモーク

## 完了条件

- ダッシュボードの再起動後も、リモートで動き続けたジョブの結果が手元に残る。

## 参照

- `doc/design-v3.md` 2.5
- `doc/design-v3-todo.md` 6
- `doc/remote-hosts.md`

## Authorised edits

- `src/remote_client.cpp`
- `src/remote_hosts.cpp`
- `src/taskqueue.cpp`
- `src/taskqueue.h`
- `src/ipc_server_remote.cpp`
- `tests/`
- `doc/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\taskqueue_smoke.exe
```
