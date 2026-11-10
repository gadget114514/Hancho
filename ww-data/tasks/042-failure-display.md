# 042 — 診断の表示（GUI / CLI / MCP / BT）

Estimate 160 min
Splittable: no

## 目的

診断を利用者が見て次の手を選べる形で出す（`doc/design-v3.md` 4.6）。

## やること

- [ ] GUI: タスク詳細・プロセス詳細に「なぜ失敗したか」、`summary`、`lastActivity`、証拠へのリンク。一覧の状態列に分類。
- [ ] CLI: `hancho-cli taskqueue why <jobId>`。
- [ ] MCP: `taskqueue_get` / `taskqueue_wait` の結果に診断を含める。
- [ ] BT: 条件で `failure.class` / `failure.suggestedAction` を参照できるようにする。
- [ ] 失敗分類の集計を出す。

## 完了条件

- ログを読まずに失敗理由と次の手が分かる。
- 証拠リンクから元のイベント・ログ行へ移れる。

## 参照

- `doc/design-v3.md` 4.6
- `doc/design-v3-en.md` Section 4

## Authorised edits

- `src/cli.cpp`
- `src/gui_dashboard.cpp`
- `src/mcp_server.cpp`
- `src/behavior_tree_cond.cpp`
- `tests/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\taskqueue_smoke.exe
build\Release\bt_integration_smoke.exe
```
