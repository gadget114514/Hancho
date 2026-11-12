# 050 — export コマンド

Estimate 160 min
Splittable: no

## 目的

Hancho をやめるときに、データを Hancho なしで読める形へ書き出す（`doc/design-v3.md` 5.4）。

## やること

- [ ] `hancho-cli export` を追加する。
- [ ] キュー → 依存順のスクリプト、または CI ワークフローの雛形。
- [ ] BT → 手順書（Markdown）。
- [ ] L3 の知識 → Markdown（`CLAUDE.md` などに貼れる形）。L3 が未実装ならこの項目は後回しにする。
- [ ] 出力は開いた形式（JSON / JSONL / Markdown）にする。

## 完了条件

- エクスポートしたキュー / BT が Hancho なしで読める。

## 参照

- `doc/design-v3.md` 5.4
- `doc/design-v3-en.md` Section 5

## Authorised edits

- `src/cli.cpp`
- `src/taskqueue.cpp`
- `src/taskqueue.h`
- `src/behavior_tree.cpp`
- `src/behavior_tree.h`
- `tests/`
- `doc/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\taskqueue_smoke.exe
build\Release\bt_ipc_smoke.exe
```
