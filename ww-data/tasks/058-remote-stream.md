# 058 — WebSocket 1 本でのログの常時ストリーミング

Estimate 300 min
Splittable: no

## 目的

リモートのログとイベントを、要求ごとに取り寄せるのではなく常時流し続ける（`doc/design-v3.md` 2.5、`doc/design-v3-todo.md` 6）。

## やること

- [ ] WebSocket 1 接続で複数の要求を運べるようにする（要求 id の照合）。
- [ ] 購読のアクションを追加する。リモートの `.stream.jsonl` / `.events.jsonl` / `.tail.txt` への追記を、手元に順序どおり流す。
- [ ] 購読の切断と再購読（再開位置は seq で指定）を行えるようにする。
- [ ] 手元のダッシュボードのリモートジョブの監視ペインを、流れてきたデータで更新する。
- [ ] `doc/remote-hosts.md` の「1 接続 1 要求」の制限を書き換える。
- [ ] スモークテストを追加する。

## 完了条件

- リモートジョブの監視ペインが、ローカルのジョブと同じように動き続ける。

## 参照

- `doc/design-v3.md` 2.5
- `doc/design-v3-todo.md` 6
- `doc/remote-hosts.md`

## Authorised edits

- `src/ipc_server_remote.cpp`
- `src/remote_client.cpp`
- `src/gui_dashboard.cpp`
- `tests/`
- `doc/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\bt_ipc_smoke.exe
```
