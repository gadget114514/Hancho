# 035 — メモリ GUI と効果測定

Estimate 180 min
Splittable: no

## 目的

メモリを人が見て判断できる画面と、注入の効果を測る集計を追加する（`doc/design-v3.md` 3.11, 3.12）。

## やること

- [ ] タスク詳細の「過去の実行」タブ（RunRecord 一覧、集計、失敗分類の推移、関連ノート）。
- [ ] メモリ一覧ダイアログ（検索、ノートの取消、L3 昇格、作者の種類で絞り込み）。
- [ ] 実行状態監視ペインに注入したメモリ id を出す。
- [ ] `memoryRefs` と L1 から、注入あり / なしの再試行成功率・再発率・増えたトークンを集計する。

## 完了条件

- GUI からノートの取消と L3 昇格ができる。
- 注入の効果を比較できる（効果が見えるまで既定は無効）。

## 参照

- `doc/design-v3.md` 3.11, 3.12
- `doc/design-v3-en.md` Section 3

## Authorised edits

- `src/gui_dashboard.cpp`
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
