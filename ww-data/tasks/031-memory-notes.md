# 031 — L2 ノートと書き込み規律

Estimate 200 min
Splittable: no

## 目的

人・エージェント・Hancho が明示的に書く短い事実を扱う（`doc/design-v3.md` 3.4, 3.10）。

## やること

- [ ] `memory/notes.jsonl` に作成・置換・取消を追記し、再生して現在状態を作る。
- [ ] MCP `memory_note` / `memory_retract` と CLI `memory note` / `retract`。
- [ ] `scope`（project / taskqueue / job / file / engine）、`status`、`supersedes`、`expiresAt`。
- [ ] 伏せ字（`sk-`, `ghp_`, `AKIA`, `Bearer `, `password=`）、env を記録しない。
- [ ] エージェントのノートは 1 試行 5 件まで、同じ scope + 本文ハッシュは重ねない、`sources` の存在検証。

## 完了条件

- ノートの作成・置換・取消が再生で再現できる。
- 秘密情報の伏せ字と濫造防止がテストで固定されている。

## 参照

- `doc/design-v3.md` 3.4, 3.10
- `doc/design-v3-en.md` Section 3

## Authorised edits

- `src/`
- `tests/`
- `CMakeLists.txt`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\taskqueue_smoke.exe
```
