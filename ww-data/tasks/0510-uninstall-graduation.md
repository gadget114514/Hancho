# 051 — uninstall と卒業チェックリスト

Estimate 100 min
Splittable: no

## 目的

Hancho をやめるときの片付けと、判断基準の見直し手順を用意する（`doc/design-v3.md` 5.3〜5.5）。

## やること

- [ ] `hancho-cli mcp uninstall`（`mcp install` の逆）。
- [ ] `hancho-cli agent uninstall-template`（`agent install-template` の逆）。
- [ ] データディレクトリを残す / 消すを利用者が選べるようにする。
- [ ] 5.3 の機能対応表をリリースごとに見直す手順をリリースチェックリストへ入れる。
- [ ] README と `hancho-cli help` に「やめ方」を短く書く。データは JSON / JSONL のまま読めることを明記する。

## 完了条件

- uninstall 系コマンドが設定とテンプレートを元に戻す。
- 卒業の判断基準が README から辿れる。

## 参照

- `doc/design-v3.md` 5.3〜5.5
- `doc/design-v3-en.md` Section 5
- `doc/critical-comparison.md`

## Authorised edits

- `src/cli.cpp`
- `src/mcp_server.cpp`
- `README.md`
- `README-ja.md`
- `doc/`
- `tests/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\bt_ipc_smoke.exe
```
