# 055b — プラグイン失敗時のフォールバックとサンプル

Estimate 85 min
Splittable: no
Parent: 055-ranker-plugin.md

## 前提

- `055-1-ranker-plugin-protocol.md`（プラグインの呼び出しがあること）

## 目的

プラグインが壊れても検索が止まらないようにし、差し替え動作を検証できる形にする（`doc/design-v3.md` 3.8）。

## やること

- [ ] プラグインの起動失敗、タイムアウト、不正な出力のときは組み込みの BM25 風順位に落ちる（検索が止まらない）。
- [ ] サンプルプラグインと、差し替え動作を確かめるスモークテストを追加する。

## 完了条件

- プラグインが壊れても検索が組み込みで動く。
- 設定で組み込みとプラグインを切り替えられる。

## 参照

- `doc/design-v3.md` 3.8
- 親カード `ww-data/tasks/055-ranker-plugin.md`

## Authorised edits

- `src/memory_search.cpp`
- `src/memory_search.h`
- `src/memory_cli.cpp`
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
