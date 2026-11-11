# 053 — Hancho が自動で書くノート

Estimate 180 min
Splittable: no

## 目的

決定的な規則で L2 ノートを自動記録する（`doc/design-v3.md` 3.4、`doc/design-v3-todo.md` 3）。

## やること

- [ ] 同じジョブ名が同じ失敗分類で 3 回続いたとき、「`build` は直近 3 回 `success_check_failed`（filesNewer）で失敗」のようなノートを書く。
- [ ] 所要時間がタイムアウトの 80% を超える試行が続いたとき、p90 と上限を本文にしたノートを書く。
- [ ] 失敗の後に成功したとき、失敗試行と成功試行の `filesEdited` の差を「直した可能性のあるファイル」としてノートに書く。
- [ ] author は `hancho`、`sources` は根拠の RunRecord id。既存の書き手規律（重複の拒否、5 件上限）に乗る。
- [ ] 同じノートを繰り返し作らない（条件が継続している間は 1 度だけ出す）。
- [ ] スモークテストを追加する。

## 完了条件

- 上記 3 規則が試行の結果から自動で発火する。
- `memory search` / `memory_brief` で自動ノートが返る。

## 参照

- `doc/design-v3.md` 3.4, 3.10
- `doc/design-v3-todo.md` 3

## Authorised edits

- `src/taskqueue.cpp`
- `src/taskqueue.h`
- `src/memory_record.cpp`
- `src/memory_record.h`
- `tests/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`
- `doc/design-v3-todo.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\taskqueue_smoke.exe
build\Release\memory_record_smoke.exe
```
