# 071 — イベントスキーマを `data` の型まで縛る

Estimate 85 min
Splittable: no

## 目的

`doc/schema/hancho.event.v1.json` の `data` を種別ごとに型まで固定する（`doc/design-v3.md` §7 決定 2026-11-11、`doc/design-v3-todo.md` 7）。

## やること

- [ ] 種別ごとの `data` の定義を締める。現状は `additionalProperties` が既定（許可）なので、未知のフィールドを拒否する。`tool.call` の `input` が空スキーマ（`{}`）で何でも通る点と、`plan` の `items[].status` が自由文字列の点を解消する。
- [ ] どうしても自由形式のままにする場所（`input` など）は、残す理由をスキーマの `description` に書く。緩めたくなったときのために、例外の一覧を 1 か所にまとめる。
- [ ] 送信側（`src/event_schema.cpp` と各アダプタ）が生成しうる全部の種別で、締めたスキーマの検証を通すことを確認する。
- [ ] `event_schema_smoke` に、未知のフィールドや型の誤りを含むイベントが落ちることのテストを足す。
- [ ] `doc/schema/` と `doc/LLM-jsonstream-format.md` に、`data` を型まで縛った旨と例外の扱いを書く。

## 完了条件

- 実サンプル（各種別・各アダプタの `.events.jsonl`）が新スキーマで全部通る。
- 未知のフィールドや型の誤りを含むイベントが検証で落ちる。
- 例外（自由形式を残す場所）とその理由が文書にある。

## 参照

- `doc/design-v3.md` 1.3, 1.8, §7
- `doc/schema/hancho.event.v1.json`
- `doc/LLM-jsonstream-format.md`

## Authorised edits

- `doc/schema/`
- `src/event_schema.cpp`
- `src/llm_stream*.cpp`
- `src/event_adapter*`
- `tests/`
- `doc/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\event_schema_smoke.exe
build\Release\event_adapter_smoke.exe
```
