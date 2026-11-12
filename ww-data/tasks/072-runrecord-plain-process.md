# 072 — キューの外のプロセスにも RunRecord を作る

Estimate 175 min
Splittable: no

## 目的

JSON ストリームの有無や起動経路に関係なく、終わったプロセスを L1 の RunRecord に残す（`doc/design-v3.md` §7 決定 2026-11-11、`doc/design-v3-todo.md` 7）。

## やること

子カードに分解した。個々の作業は子カードで管理する:

- [ ] `ww-data/tasks/072-1-plain-run-state.md` — キューの外の実行の状態保存
- [ ] `ww-data/tasks/072-2-plain-run-record.md` — RunRecord の生成と rebuild・集計

## 完了条件

- キューの外で終わったプロセスが `memory search --kind run` に出る。
- `memory rebuild` で同じ記録が再生できる。
- キューを使う既存のジョブの記録に影響がない。

## 参照

- `doc/design-v3.md` 3.3, 3.7, §7
- `doc/design-v3-todo.md` 7
- `src/memory_record.h`

## Authorised edits

- `src/cli.cpp`
- `src/taskqueue.cpp`
- `src/taskqueue.h`
- `src/memory_record.cpp`
- `src/memory_record.h`
- `tests/`
- `doc/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\memory_record_smoke.exe
build\Release\taskqueue_smoke.exe
```
