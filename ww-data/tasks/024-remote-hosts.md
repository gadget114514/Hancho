# 024 — リモート IPC とホスト登録

Estimate 300 min
Splittable: no

## 目的

別マシンのワーカーを手元から見て操作できるようにする（`doc/design-v3.md` 2.5 段階 C）。

## やること

- [ ] IPC の要求・応答を TCP（WebSocket）に載せる。既定は localhost のみ。
- [ ] 外部利用はトークン必須、SSH トンネル推奨。TLS はトンネルに任せる。
- [ ] ジョブに `host` を追加し、`<data-dir>/hosts.json` でホストを登録する。
- [ ] トークンは資格情報マネージャー / 環境変数から読む（ファイルに書かない）。
- [ ] リソースロックをホストごとに分ける。
- [ ] ログ・イベント・`.done` をプロトコル経由で取り寄せ、手元の GUI に同じ一覧で出す。

## 完了条件

- リモートのジョブが手元の一覧・ログ・イベントに出る。
- 認証なしの外部接続が拒否される。

## 参照

- `doc/design-v3.md` 2.5
- `doc/design-v3-en.md` Section 2.5

## Authorised edits

- `src/`
- `include/`
- `tests/`
- `CMakeLists.txt`
- `doc/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\bt_ipc_smoke.exe
build\Release\taskqueue_smoke.exe
```
