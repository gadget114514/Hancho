# 061d — ブラウザ UI のビルドと配布の文書化

Estimate 25 min
Splittable: no
Parent: 061-linux-gui.md

## 前提

- `061-3-browser-ui-panes.md`（機能が動くこと）

## 目的

作ったブラウザ UI を第三者が導入できる形に文書でまとめる（`doc/design-v3-todo.md` 8）。

## やること

- [ ] ビルド手順と配布方法（UI 資産の同梱を含むパッケージ形式）を文書化する。

## 完了条件

- 配布手順に従って第三者が導入できる。

## 参照

- `doc/design-v3-todo.md` 8
- 親カード `ww-data/tasks/061-linux-gui.md`

## Authorised edits

- `doc/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
# 文書のみのタスク。ビルドは不要
git diff -- doc/
```
