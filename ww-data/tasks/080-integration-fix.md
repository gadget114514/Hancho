# 080 — 統合修正（31 枚のカードの境界の不整合を解消する）

Estimate 195 min
Splittable: no

## 目的

`ww` キューの 31 枚（050〜073）がそれぞれ独立に作ったコードは、カード間で前提が合わず **Release ビルドが通らない**。このカード群で境界を 1 箇所に集約し、ビルドとテストを通す（`doc/design-v3-todo.md` 7 の実装 Mystic 行Cleanup）。

## やること

子カードに分解した。個々の作業は子カードで管理する:

- [ ] `ww-data/tasks/080-1-build-fix.md` — コンパイルエラーの解消（API 定義の統一）
- [ ] `ww-data/tasks/080-2-smoke-tests.md` — スモークテストの更新と通過確認

## 完了条件

- `cmake --build build --config Release --target Hancho hancho-cli` が通る。
- 既存のスモークテストがすべて通る。
- 変更は 2 枚の子のカードの範囲に収まっている。

## 参照

- `ww-data/tasks/`（050〜073）
- `doc/design-v3.md`

## Authorised edits

- `src/`
- `tests/`
- `CMakeLists.txt`
- `doc/`

## Read-only

- `ww-data/`

## 検証

```powershell
cmake --build build --config Release --target Hancho hancho-cli
```
