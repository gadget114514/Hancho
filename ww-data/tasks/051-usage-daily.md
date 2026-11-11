# 051 — usage の日次ロールアップ（U1）

Estimate 240 min
Splittable: no

## 目的

24 時間で捨てられる生ポイントに代えて、日 × エンジン × モデルの集計を永続させる（`doc/design-v3.md` 3.6.1 U1、`doc/design-v3-todo.md` 2）。

## やること

- [ ] `logs/usage_history.jsonl` の無限成長を止める。追記のまま放置しない（7 日より古い行を落として書き直す、または日次集計への置き換え）。
- [ ] 生ポイントを書くときに、同じローカル日付の `usage/daily/<年-月>.jsonl` へ `{date, tzOffsetMin, engine, model, delta...}` を追記する。
- [ ] 読むときに同じキー（日 × エンジン × モデル）を合計して `hancho.usage.day/1` 形式で返す。
- [ ] `hancho-cli usage stats --group-by day|month|engine|model` と MCP `usage_stats` を追加する（`since` / `until` / `groupBy`）。
- [ ] IPC に U1 用の `get_usage_daily` を追加する（`get_usage_history` は U0 用に残す）。
- [ ] スモークテストを追加する。

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
