# 072-2 — RunRecord の生成と rebuild・集計

Estimate 90 min
Splittable: no
Parent: 072-runrecord-plain-process.md

## 前提

- `072-1-plain-run-state.md`（キューを持たない実行の状態保存）

## 目的

保存した状態から L1 の RunRecord を作り、集計と作り直しにも含める（`doc/design-v3.md` §7 決定 2026-11-11、`doc/design-v3-todo.md` 7）。

## やること

- [ ] キューの外の試行から RunRecord を作る。取得できない項目（トークン、filesEdited、失敗分類の一部）は空のままにする。推測で埋めない。
- [ ] 失敗分類の決定的な規則が動く範囲（起動失敗、タイムアウト、終了コード）はキューの試行と同じ判定を使う。
- [ ] `memory rebuild` でこれらの記録も同じ入力から再生できるようにする。
- [ ] `memory search` / `memory stats` がこれらの記録を含めて数えることを確認する。
- [ ] スモークテストを追加する（キューの外の 1 実行 → 記録 → rebuild で再生）。

## 完了条件

- キューの外で終わったプロセスが `memory search --kind run` に出る。
- `memory rebuild` で同じ記録が再生できる。
- 既存のキューのジョブの記録に影響がない。

## 参照

- `doc/design-v3.md` 3.3, 3.7, §7
- `doc/design-v3-todo.md` 7
- 親カード `ww-data/tasks/072-runrecord-plain-process.md`

## Authorised edits

- `src/memory_record.cpp`
- `src/memory_record.h`
- `src/memory_search.cpp`
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
