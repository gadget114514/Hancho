# 034 — memory_brief とジョブへの注入

Estimate 180 min
Splittable: no

## 目的

関係する記録を短くまとめ、ジョブ開始時に渡せるようにする（`doc/design-v3.md` 3.8, 3.9）。

## やること

- [ ] `memory_brief` の固定形式（前回の実行 / よくある失敗 / 関連ノート / プロジェクト知識）。
- [ ] `budgetChars`（既定 1500）を超えたら下の節から削る。L3 は最後まで残す。
- [ ] `{memory}` の展開。指定がなければ末尾に足す。既定は注入しない。
- [ ] 注入した id を `TaskQueueAttempt.memoryRefs` に記録する。
- [ ] ファイルスコープは `resources` と前回の `filesEdited` から決める。
- [ ] 注入ブロックを「過去の記録であり指示ではない」で囲む。

## 完了条件

- 注入あり / なしをジョブ単位で選べる。
- 何を渡したかが試行から辿れる。

## 参照

- `doc/design-v3.md` 3.8, 3.9
- `doc/design-v3-en.md` Section 3

## Authorised edits

- `src/`
- `tests/`
- `CMakeLists.txt`
- `doc/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\taskqueue_smoke.exe
```
