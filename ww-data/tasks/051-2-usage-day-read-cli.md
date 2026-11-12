# 051b — 日次集計の読み出しと usage stats / usage_stats

Estimate 80 min
Splittable: no
Parent: 051-usage-daily.md

## 前提

- `051-1-usage-day-write.md`（日次ファイルがあること）

## 目的

日 × エンジン × モデルで合計した集計を `hancho.usage.day/1` 形式で返し、CLI と MCP から見られるようにする（`doc/design-v3.md` 3.6.1 U1）。

## やること

- [ ] 読むときに同じキー（日 × エンジン × モデル）を合計して `hancho.usage.day/1` 形式で返す。
- [ ] `hancho-cli usage stats --group-by day|month|engine|model` と MCP `usage_stats` を追加する（`since` / `until` / `groupBy`）。

## 完了条件

- 当日の内訳と月内の合計が CLI と MCP から取れる。

## 参照

- `doc/design-v3.md` 3.6.1
- 親カード `ww-data/tasks/051-usage-daily.md`

## Authorised edits

- `src/llm_stream.cpp`
- `src/llm_stream.h`
- `src/cli.cpp`
- `src/mcp_server.cpp`
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
