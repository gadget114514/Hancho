# 051c — IPC get_usage_daily とスモークテスト

Estimate 75 min
Splittable: no
Parent: 051-usage-daily.md

## 前提

- `051-2-usage-day-read-cli.md`（集計の読み出しがあること）

## 目的

GUI などの IPC クライアントから日次集計を見られるようにし、U1 全体を検証する（`doc/design-v3.md` 3.6.1 U1）。

## やること

- [ ] IPC に U1 用の `get_usage_daily` を追加する（`get_usage_history` は U0 用に残す）。
- [ ] スモークテストを追加する（051a〜051c の全体を 1 回で通す）。

## 完了条件

- IPC クライアントから当日の内訳と月内の合計が取れる。
- スモークテストが通る。

## 参照

- `doc/design-v3.md` 3.6.1
- 親カード `ww-data/tasks/051-usage-daily.md`

## Authorised edits

- `src/ipc_server.cpp`
- `src/ipc_client.cpp`
- `tests/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\llm_stream_smoke.exe
```
