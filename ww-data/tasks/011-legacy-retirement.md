# 011 — 旧形式の公開停止と移行文書

Estimate 120 min
Splittable: no

## 目的

`type` 文字列の外部公開をいつ・どう止めるかを決め、利用者に伝える（`doc/design-v3.md` 1.8 段階 4）。

## やること

- [ ] `worker stream` と `get_worker_stream` の legacy を非推奨として help に明記する。
- [ ] 新旧併存を少なくとも 1 リリース続ける期限と移行手順を README / `doc/LLM-jsonstream-format.md` に書く。
- [ ] legacy と v1 が同じ内容を返す互換テストを追加する。
- [ ] 既定 schema をどちらにするか決めて文書化する。

## 完了条件

- 利用者が legacy から v1 へ移行できる手順がある。
- 廃止時期がリリースノートと help から辿れる。

## 参照

- `doc/design-v3.md` 1.3, 1.8
- `doc/design-v3-en.md` Section 1

## Authorised edits

- `src/cli.cpp`
- `src/mcp_server.cpp`
- `README.md`
- `README-ja.md`
- `doc/LLM-jsonstream-format.md`
- `tests/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`
- `doc/schema/hancho.event.v1.json`

## 検証

```powershell
cmake --build build --config Release
build\Release\event_schema_smoke.exe
build\Release\event_adapter_smoke.exe
```
