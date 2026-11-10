# 033 — メモリ検索・取得・統計

Estimate 240 min
Splittable: no

## 目的

L1 / L2 / L3 を検索し、根拠付きで取り出せるようにする（`doc/design-v3.md` 3.6, 3.8）。

## やること

- [ ] MCP `memory_search` / `memory_get` / `memory_stats`、CLI 同名コマンド。
- [ ] フィルタ: `kinds`, `scope`, `file`, `jobName`, `taskqueue`, `engine`, `model`, `outcome`, `failureClass`, `since`。
- [ ] BM25 相当のキーワード検索。scope / ファイル一致、新しさ、層の重み（L3 > L2 > L1）、失敗質問時の加点。
- [ ] `memory/index/` を派生物として作り、なくても再構築できるようにする。
- [ ] `IMemoryRanker` の境界だけ用意する（埋め込みは同梱しない）。

## 完了条件

- 検索結果から id と出典へ辿れる。
- 索引を消しても `memory rebuild` で戻る。

## 参照

- `doc/design-v3.md` 3.6, 3.8
- `doc/design-v3-en.md` Section 3

## Authorised edits

- `src/`
- `tests/`
- `CMakeLists.txt`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\taskqueue_smoke.exe
```
