# 058c — 監視ペインへの反映と文書・スモーク

Estimate 80 min
Splittable: no
Parent: 058-remote-stream.md

## 前提

- `058-2-ws-subscribe.md`（購読の受信があること）

## 目的

流れてきたデータをダッシュボードの監視ペインに反映させ、常時ストリーミングの完成まで行う（`doc/design-v3.md` 2.5）。

## やること

- [ ] 手元のダッシュボードのリモートジョブの監視ペインを、流れてきたデータで更新する。
- [ ] `doc/remote-hosts.md` の「1 接続 1 要求」の制限を書き換える。
- [ ] スモークテストを追加する。

## 完了条件

- リモートジョブの監視ペインが、ローカルのジョブと同じように動き続ける。

## 参照

- `doc/design-v3.md` 2.5
- `doc/remote-hosts.md`
- 親カード `ww-data/tasks/058-remote-stream.md`

## Authorised edits

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
