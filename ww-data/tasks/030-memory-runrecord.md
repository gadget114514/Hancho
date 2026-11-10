# 030 — L1 実行記録（RunRecord）

Estimate 200 min
Splittable: no

## 目的

試行 1 回につき 1 件の実行記録を、イベントから決定的に作る（`doc/design-v3.md` 3.3）。

## やること

- [ ] `hancho.memory.run/1` を `memory/records/YYYY-MM.jsonl` に追記する。
- [ ] フィールドを 3.3 の表どおりに埋める（goal, outcome, filesRead/Edited, commands, errors, todos, usage, sources など）。
- [ ] JSON ストリームのない普通のコマンドでも作れる範囲を作る。
- [ ] `memory rebuild` で `.events.jsonl` と `taskqueues/*.json` から作り直せるようにする。

## 完了条件

- 同じ入力から同じ L1 ができる。
- `sources.eventsFile` / `seqRange` から元イベントへ辿れる。

## 参照

- `doc/design-v3.md` 3.3, 3.7
- `doc/design-v3-en.md` Section 3

## Authorised edits

- `src/`
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
build\Release\event_schema_smoke.exe
```
