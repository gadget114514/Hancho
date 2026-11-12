# 073 — L3 昇格を特定のワーカーに許す

Estimate 90 min
Splittable: no

## 目的

設定で列挙した特定のワーカーにも L3 への昇格を許す（`doc/design-v3.md` §7 決定 2026-11-11、3.5、`doc/design-v3-todo.md` 7）。

## やること

- [ ] データディレクトリの設定に、昇格を許すワーカーの名前の一覧を追加する（例: `memory.l3Promoters`）。空なら人のみ（現状どおり）。
- [ ] `memory approve`（CLI）で、呼び出し元が許可一覧に載っているワーカーなら昇格できるようにする。`--by` に `worker:<name>` を受け付ける形か、別フラグかを決めて文書化する。
- [ ] MCP にも `memory_approve` を出すかどうか決める。出すなら呼び出し元のクライアント / セッションが許可一覧に載るワーカーかを確認する。
- [ ] `knowledge.jsonl` の `approvedBy` に種別と名前（例: `worker:codex`）を残す。GUI のメモリ一覧にそのまま出ることを確認する。
- [ ] 許可一覧にない呼び出し元は現状どおり拒否し、エラーに理由を書く。
- [ ] スモークテストを追加する（許可されたワーカーの昇格、未許可の拒否）。

## 完了条件

- 許可されたワーカーが昇格でき、`knowledge.jsonl` に `approvedBy: worker:<name>` が残る。
- 未許可のワーカーは昇格できない。
- 設定が空のときは現状（人のみ）と同じ動きになる。

## 参照

- `doc/design-v3.md` 3.5, 3.10, §7
- `doc/design-v3-todo.md` 7

## Authorised edits

- `src/memory_search.cpp`
- `src/memory_cli.cpp`
- `src/mcp_server.cpp`
- `src/gui_dashboard.cpp`
- `tests/`
- `doc/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\memory_search_smoke.exe
```
