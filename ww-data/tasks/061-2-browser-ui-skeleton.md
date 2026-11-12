# 061b — ブラウザ UI の骨格（localhost の HTTP + WebSocket）

Estimate 95 min
Splittable: no
Parent: 061-linux-gui.md

## 前提

- `061-1-browser-ui-approach.md`（方式と認証の決定）
- `058-1-ws-multiplex.md`、`058-2-ws-subscribe.md`（WebSocket 1 本での複数要求と購読）

## 目的

コアが localhost の HTTP + WebSocket サーバーとして動き、ブラウザが接続して状態を表示できる骨格を作る（`doc/design-v3-todo.md` 8）。

## やること

- [ ] サーバーの起動、トークンによる認証、UI 資産の配信を実装する。
- [ ] ブラウザのタブを閉じてもジョブが止まらないことを確認する（コアは別プロセスで動き続ける）。タブを開き直すと再接続して復元されることも確認する。
- [ ] 接続後に基本の状態（ジョブ一覧の 1 画面）を表示できるところまで進める。

## 完了条件

- Linux と macOS のブラウザで開き、コアに接続してジョブの一覧が見える。

## 参照

- `doc/design-v3-todo.md` 8
- `doc/design-v3.md` 2.4
- 親カード `ww-data/tasks/061-linux-gui.md`

## Authorised edits

- `src/`（サーバー側）
- `src/webui*/`（UI 資産）
- `CMakeLists.txt`
- `tests/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`
- `src/gui_dashboard.cpp`

## 検証

```powershell
# Windows ではビルドと既存テストのみ（POSIX 環境で動作確認する）
cmake --build build --config Release
build\Release\headless_smoke.exe
```
