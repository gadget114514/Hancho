# 058 — WebSocket 1 本でのログの常時ストリーミング

Estimate 235 min
Splittable: no

## 目的

リモートのログとイベントを、要求ごとに取り寄せるのではなく常時流し続ける（`doc/design-v3.md` 2.5、`doc/design-v3-todo.md` 6）。

## やること

子カードに分解した。個々の作業は子カードで管理する:

- [ ] `ww-data/tasks/058-1-ws-multiplex.md` — WebSocket 1 本での複数要求
- [ ] `ww-data/tasks/058-2-ws-subscribe.md` — 購読アクションと seq による再開
- [ ] `ww-data/tasks/058-3-ws-pane-doc.md` — 監視ペインへの反映と文書・スモーク

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
