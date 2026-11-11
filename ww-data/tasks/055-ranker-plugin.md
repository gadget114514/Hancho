# 055 — IMemoryRanker の外部プラグイン

Estimate 240 min
Splittable: no

## 目的

検索の順位付けを外部プロセスのプラグインで差し替えられるようにする（`doc/design-v3.md` 3.8、`doc/design-v3-todo.md` 5）。

## やること

- [ ] データディレクトリの設定でランカープラグインを指定できるようにする（実行可能ファイルのパス）。
- [ ] プラグインのプロトコルを実装する。stdin に問い合わせと候補（id、種別、本文、作成時刻）を JSON で渡し、stdout に候補ごとのスコア配列を受け取る。
- [ ] プラグインの起動失敗、タイムアウト、不正な出力のときは組み込みの BM25 風順位に落ちる（検索が止まらない）。
- [ ] プラグインはデータディレクトリ内で完結し、外部には何も出さない。
- [ ] サンプルプラグインと、差し替え動作を確かめるスモークテストを追加する。

## 完了条件

- 設定で組み込みとプラグインを切り替えられる。
- プラグインが壊れても検索が組み込みで動く。

## 参照

- `doc/design-v3.md` 3.8（埋め込み検索）
- `doc/design-v3-todo.md` 5

## Authorised edits

- `src/memory_search.cpp`
- `src/memory_search.h`
- `src/memory_cli.cpp`
- `src/mcp_server.cpp`
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
