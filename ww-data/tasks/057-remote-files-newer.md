# 057 — リモート実行での success.filesNewer の評価

Estimate 120 min
Splittable: no

## 目的

リモートのジョブでも成功条件 `success.filesNewer` を正しく評価する（`doc/design-v3-todo.md` 6）。

## やること

- [ ] `success.filesNewer` の評価対象を、ジョブの `host` 側のファイル時刻で判定する（手元の時刻で判定しない）。
- [ ] ホスト側で評価する仕組み（評価を hanchod に依頼する、または必要なパスの時刻をプロトコルで取り寄せる）を選び、実装する。
- [ ] 時刻の基準（タイムゾーン、時計のずれ）を決めて文書化する。
- [ ] `doc/remote-hosts.md` の「評価しない」の制限を書き換える。
- [ ] スモークテストを追加する。

## 完了条件

- リモートのジョブが、ホスト側の成果物の時刻で成功 / 失敗を判定する。

## 参照

- `doc/design-v3.md` 2.5
- `doc/design-v3-todo.md` 6
- `doc/remote-hosts.md`

## Authorised edits

- `src/remote_client.cpp`
- `src/ipc_server_remote.cpp`
- `src/taskqueue.cpp`
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
