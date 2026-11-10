# 021 — CI 一発実行と JUnit レポート

Estimate 150 min
Splittable: no

## 目的

CI からキューを流し、結果を機械可読に受け取れるようにする（`doc/design-v3.md` 2.3）。

## やること

- [ ] `hancho-cli run --file queue.json --exit-on-drain --report junit.xml`。
- [ ] 終了コード: 全成功 0、失敗あり 1、取り消し・中断 2。
- [ ] JUnit のジョブごとの `<failure message>` に失敗理由（`reason` / `exitCode` / 診断があれば分類）を入れる。
- [ ] 空キュー・依存待ち・中断のテストを追加する。

## 完了条件

- キューが空になったら終了し、CI がそのまま結果を読める。
- 終了コードの 3 値がテストで固定されている。

## 参照

- `doc/design-v3.md` 2.3
- `doc/taskqueue.md`

## Authorised edits

- `src/cli.cpp`
- `src/taskqueue.cpp`
- `src/taskqueue.h`
- `tests/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\taskqueue_smoke.exe
```
