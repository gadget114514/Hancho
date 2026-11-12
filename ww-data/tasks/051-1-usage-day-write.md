# 051a — usage の日次ファイルへの書き込みと成長止め

Estimate 75 min
Splittable: no
Parent: 051-usage-daily.md

## 目的

`logs/usage_history.jsonl` の無限成長を止め、生ポイントを日次ファイルに永続化する（`doc/design-v3.md` 3.6.1 U1）。

## やること

- [ ] `logs/usage_history.jsonl` の無限成長を止める。追記のまま放置しない（7 日より古い行を落として書き直す、または日次集計への置き換え）。
- [ ] 生ポイントを書くときに、同じローカル日付の `usage/daily/<年-月>.jsonl` へ `{date, tzOffsetMin, engine, model, delta...}` を追記する。

## 完了条件

- 24 時間を過ぎてもデータが消えず、日次ファイルに残る。
- 追記は途中で落ちても壊れない（再生で合計が復元できる）。

## 参照

- `doc/design-v3.md` 3.6.1
- 親カード `ww-data/tasks/051-usage-daily.md`

## Authorised edits

- `src/llm_stream.cpp`
- `src/llm_stream.h`
- `tests/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\llm_stream_smoke.exe
```
