# 032 — L3 プロジェクト知識

Estimate 120 min
Splittable: no

## 目的

人が承認した事実だけを、チームで共有できる知識として残す（`doc/design-v3.md` 3.5）。

## やること

- [ ] `memory/knowledge.jsonl` に `approvedBy` / `approvedAt` 付きで保存する。
- [ ] 昇格は人だけ（`hancho-cli memory approve <id>`、GUI）。エージェントには許さない。
- [ ] git にコミットする前提のファイル形式にする。
- [ ] 031 のノートから昇格する経路を作る。

## 完了条件

- 承認済みの知識だけが `knowledge.jsonl` に入る。
- エージェント経由では昇格できない。

## 参照

- `doc/design-v3.md` 3.5
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
