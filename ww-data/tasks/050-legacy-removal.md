# 050 — 旧 `type` 文字列の外部公開の削除

Estimate 80 min
Splittable: no

## 目的

legacy 形式の外部公開をやめ、既定を `hancho.event/1` に固定する（`doc/design-v3.md` 1.8 段階 4、`doc/design-v3-todo.md` 1）。

## やること

- [ ] `worker stream` と `get_worker_stream` から legacy 形式の受け付けを削除する（README で告知済みの v0.11.0 対応）。
- [ ] `--schema legacy` / `schema: "legacy"` を渡されたときの応答を「非対応」に変え、help を更新する。
- [ ] `doc/LLM-jsonstream-format.md` の移行手順を「完了した移行」の記録に書き換える。
- [ ] README / README-ja.md の告知文を削除済みの事実に書き換える。

## 完了条件

- CLI と MCP の外部に出る形式が `hancho.event/1` のみになる。
- `event_schema_smoke` と `event_adapter_smoke` が通る。

## 参照

- `doc/design-v3.md` 1.3, 1.8
- `doc/design-v3-todo.md` 1
- `doc/LLM-jsonstream-format.md`

## Authorised edits

- `src/cli.cpp`
- `src/mcp_server.cpp`
- `src/event_schema.cpp`
- `README.md`
- `README-ja.md`
- `doc/LLM-jsonstream-format.md`
- `tests/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`
- `doc/schema/hancho.event.v1.json`

## 検証

```powershell
cmake --build build --config Release
build\Release\event_schema_smoke.exe
build\Release\event_adapter_smoke.exe
```
