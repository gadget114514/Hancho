# 056a — リモート再接続の対応表と接続経路

Estimate 75 min
Splittable: no
Parent: 056-remote-reconnect.md

## 目的

ダッシュボード再起動後に、リモートの試行へ再接続する経路を作る（`doc/design-v3.md` 2.5）。

## やること

- [ ] 手元の試行の状態にリモートの jobId / attempt の対応を保存し、再起動後に `hosts.json` から辿って再接続する。
- [ ] 再接続できない（ホストに到達しない）ときは、現状どおり `interrupted` に戻す。

## 完了条件

- 再起動後、到達できるホストのリモート試行に再接続できる。
- 到達できないときは現状と同じ `interrupted` になる。

## 参照

- `doc/design-v3.md` 2.5
- `doc/remote-hosts.md`
- 親カード `ww-data/tasks/056-remote-reconnect.md`

## Authorised edits

- `src/remote_client.cpp`
- `src/remote_hosts.cpp`
- `src/taskqueue.cpp`
- `src/taskqueue.h`
- `tests/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\taskqueue_smoke.exe
```
