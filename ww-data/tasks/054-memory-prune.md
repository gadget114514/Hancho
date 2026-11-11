# 054 — memory prune と容量上限

Estimate 120 min
Splittable: no

## 目的

メモリの保持期間と容量の上限を設計どおりに効かせる（`doc/design-v3.md` 3.7、`doc/design-v3-todo.md` 4）。

## やること

- [ ] `hancho-cli memory prune` を追加する。既定 90 日より古い月の `memory/records/*.jsonl` を消し、`--dry-run` で対象の一覧だけを出す。
- [ ] `memory/` 全体の容量上限をデータディレクトリの設定に持ち、超えたら古い L1 から消す。
- [ ] L2 と L3 は自動で消さず、上限に近づいたら警告する（CLI 出力と GUI 通知）。
- [ ] `memory stats` に現在の容量と上限を含める。
- [ ] スモークテストを追加する。

## 完了条件

- `prune` の後も `memory rebuild` で残った記録を壊さず再構築できる。
- 上限超過時に L2 / L3 が消えない。

## 参照

- `doc/design-v3.md` 3.7
- `doc/design-v3-todo.md` 4

## Authorised edits

- `src/memory_cli.cpp`
- `src/memory_record.cpp`
- `src/memory_record.h`
- `src/memory_search.cpp`
- `src/gui_dashboard.cpp`
- `tests/`
- `doc/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\memory_record_smoke.exe
build\Release\memory_search_smoke.exe
```
