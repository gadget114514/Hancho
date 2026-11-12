# 051 — usage の日次ロールアップ（U1）

Estimate 230 min
Splittable: no

## 目的

24 時間で捨てられる生ポイントに代えて、日 × エンジン × モデルの集計を永続させる（`doc/design-v3.md` 3.6.1 U1、`doc/design-v3-todo.md` 2）。

## やること

子カードに分解した。個々の作業は子カードで管理する:

- [ ] `ww-data/tasks/051-1-usage-day-write.md` — usage の日次ファイルへの書き込みと成長止め
- [ ] `ww-data/tasks/051-2-usage-day-read-cli.md` — 日次集計の読み出しと usage stats / usage_stats
- [ ] `ww-data/tasks/051-3-usage-ipc-smoke.md` — IPC get_usage_daily とスモークテスト

## 完了条件

- 当日の内訳と月内の合計が CLI と MCP から取れる。
- 追記は途中で落ちても壊れない（再生で合計が復元できる）。

## 参照

- `doc/design-v3.md` 3.6.1
- `doc/design-v3-todo.md` 2

## Authorised edits

- `src/llm_stream.cpp`
- `src/llm_stream.h`
- `src/cli.cpp`
- `src/mcp_server.cpp`
- `src/ipc_server.cpp`
- `src/ipc_client.cpp`
- `tests/`
- `doc/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\llm_stream_smoke.exe
```
