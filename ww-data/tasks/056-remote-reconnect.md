# 056 — リモートプロセスへの再接続

Estimate 240 min
Splittable: no

## 目的

手元のダッシュボードを再起動しても、リモート実行中のジョブを追い続ける（`doc/design-v3.md` 2.5、`doc/design-v3-todo.md` 6）。

## やること

- [ ] 手元の試行の状態にリモートの jobId / attempt の対応を保存し、再起動後に `hosts.json` から辿って再接続する。
- [ ] 再接続時にリモートの試行の状態・ログ・イベントを取り寄せ、手元の一覧に復元する。
- [ ] 再接続できない（ホストに到達しない）ときは、現状どおり `interrupted` に戻す。
- [ ] `doc/remote-hosts.md` の「再接続しない」の制限を書き換える。
- [ ] スモークテストを追加する（再起動を模した手元コアの再起動でリモートの状態を復元する）。

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
