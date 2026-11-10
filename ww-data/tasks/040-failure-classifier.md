# 040 — 失敗分類器と診断レコード

Estimate 240 min
Splittable: no

## 目的

「なぜ失敗したか」を分類し、証拠付きの診断を残す（`doc/design-v3.md` 4.2〜4.4）。

## やること

- [ ] 4.2 の分類を実装する。
- [ ] 試行ごとに診断レコード（`class`, `confidence`, `summary`, `detail`, `evidence`, `lastActivity`, `suggestedAction`, `retryAt`）。
- [ ] 規則は順序付き・決定的。上から評価し、最初に当たったものを採る。
- [ ] 入力は試行情報、1 章の正規化イベント、ログ末尾。
- [ ] `evidence` は元のイベント seq かログ行を必ず指す。
- [ ] 組み込み規則を用意する。

## 完了条件

- 同じ入力から同じ診断が出る。
- 診断から元データへ辿れる。

## 参照

- `doc/design-v3.md` 4.2〜4.4
- `doc/design-v3-en.md` Section 4

## Authorised edits

- `src/taskqueue.cpp`
- `src/taskqueue.h`
- `src/llm_stream.cpp`
- `src/llm_stream.h`
- `tests/`
- `CMakeLists.txt`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`
- `doc/schema/hancho.event.v1.json`

## 検証

```powershell
cmake --build build --config Release
build\Release\taskqueue_smoke.exe
```
