# 055a — ランカープラグインの設定とプロトコル

Estimate 80 min
Splittable: no
Parent: 055-ranker-plugin.md

## 目的

データディレクトリの設定で外部ランカープラグインを指定し、stdin/stdout の JSON プロトコルで呼べるようにする（`doc/design-v3.md` 3.8 埋め込み検索）。

## やること

- [ ] データディレクトリの設定でランカープラグインを指定できるようにする（実行可能ファイルのパス）。
- [ ] プラグインのプロトコルを実装する。stdin に問い合わせと候補（id、種別、本文、作成時刻）を JSON で渡し、stdout に候補ごとのスコア配列を受け取る。
- [ ] プラグインはデータディレクトリ内で完結し、外部には何も出さない。

## 完了条件

- 設定でプラグインを指定すると、検索の順位がプラグインのスコアで決まる。

## 参照

- `doc/design-v3.md` 3.8
- 親カード `ww-data/tasks/055-ranker-plugin.md`

## Authorised edits

- `src/memory_search.cpp`
- `src/memory_search.h`
- `src/memory_cli.cpp`
- `tests/`
- `doc/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\memory_search_smoke.exe
```
