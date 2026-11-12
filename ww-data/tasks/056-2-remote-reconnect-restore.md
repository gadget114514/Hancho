# 056b — 再接続後の状態復元と文書・スモーク

Estimate 80 min
Splittable: no
Parent: 056-remote-reconnect.md

## 前提

- `056-1-remote-reconnect-path.md`（再接続の経路があること）

## 目的

再接続した試行の結果を手元に復元させ、制限の解消を文書に反映する（`doc/design-v3.md` 2.5）。

## やること

- [ ] 再接続時にリモートの試行の状態・ログ・イベントを取り寄せ、手元の一覧に復元する。
- [ ] `doc/remote-hosts.md` の「再接続しない」の制限を書き換える。
- [ ] スモークテストを追加する（再起動を模した手元コアの再起動でリモートの状態を復元する）。

## 完了条件

- ダッシュボードの再起動後も、リモートで動き続けたジョブの結果が手元に残る。

## 参照

- `doc/design-v3.md` 2.5
- `doc/remote-hosts.md`
- 親カード `ww-data/tasks/056-remote-reconnect.md`

## Authorised edits

- `src/remote_client.cpp`
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
