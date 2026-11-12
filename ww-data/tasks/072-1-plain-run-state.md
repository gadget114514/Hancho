# 072-1 — キューの外の実行の状態保存

Estimate 85 min
Splittable: no
Parent: 072-runrecord-plain-process.md

## 目的

JSON ストリームの有無や起動経路に関係なく、終わったプロセスを L1 の RunRecord に残すため、まずキューを持たない実行の状態を保存できるようにする（`doc/design-v3.md` §7 決定 2026-11-11、`doc/design-v3-todo.md` 7）。

## やること

- [ ] 対象を決めて文書化する。`hancho-cli run` でキューを使わず起動した 1 回きりの実行、`HANCHO_EVENT_PIPE` を付けて手で起動したプロセスなど。
- [ ] キューを持たない試行に jobId / attempt を発行する経路を作る。試行の最小の状態（開始・終了・終了コード）を `logs/taskqueues/*.json` と同じ形で残すか、そこに登録する。

## 完了条件

- キューの外の実行が、`memory rebuild` の入力として残る。
- キューを使う既存のジョブの記録に影響がない。

## 参照

- `doc/design-v3.md` 3.3, 3.7, §7
- `doc/design-v3-todo.md` 7
- `src/memory_record.h`
- 親カード `ww-data/tasks/072-runrecord-plain-process.md`

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
