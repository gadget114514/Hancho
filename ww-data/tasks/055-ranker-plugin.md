# 055 — IMemoryRanker の外部プラグイン

Estimate 165 min
Splittable: no

## 目的

検索の順位付けを外部プロセスのプラグインで差し替えられるようにする（`doc/design-v3.md` 3.8、`doc/design-v3-todo.md` 5）。

## やること

子カードに分解した。個々の作業は子カードで管理する:

- [ ] `ww-data/tasks/055-1-ranker-plugin-protocol.md` — ランカープラグインの設定とプロトコル
- [ ] `ww-data/tasks/055-2-ranker-plugin-fallback.md` — プラグイン失敗時のフォールバックとサンプル

## 完了条件

- 設定で組み込みとプラグインを切り替えられる。
- プラグインが壊れても検索が組み込みで動く。

## 参照

- `doc/design-v3.md` 3.8（埋め込み検索）
- `doc/design-v3-todo.md` 5

## Authorised edits

- `src/memory_search.cpp`
- `src/memory_search.h`
- `src/memory_cli.cpp`
- `src/mcp_server.cpp`
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
