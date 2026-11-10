# 010 — 正規化イベントを利用側に移行

Estimate 180 min
Splittable: no

## 目的

`hancho.event/1` を、GUI・MCP・BT の読み取り経路でも使う（`doc/design-v3.md` 1.7）。

## やること

- [ ] 監視ペインが `.events.jsonl` の v1 を読む（内部 `LlmStreamEvent` への依存を外す）。
- [ ] MCP `get_worker_stream` の v1 を新しい既定にできるようにする（legacy は併存）。
- [ ] BT の条件が v1 の `kind` / `activity` / `failure` を参照できるようにする。
- [ ] 上記の回帰テストを追加する。

## 完了条件

- 旧 `type` 文字列を読むのは内部実装だけになる。
- `event_schema_smoke`、`bt_integration_smoke`、`bt_ipc_smoke` が通る。

## 参照

- `doc/design-v3.md` 1.3, 1.7
- `doc/schema/hancho.event.v1.json`

## Authorised edits

- `src/gui_dashboard.cpp`
- `src/mcp_server.cpp`
- `src/behavior_tree_cond.cpp`
- `tests/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`
- `src/event_schema.h`

## 検証

```powershell
cmake --build build --config Release
build\Release\event_schema_smoke.exe
build\Release\bt_integration_smoke.exe
build\Release\bt_ipc_smoke.exe
```
