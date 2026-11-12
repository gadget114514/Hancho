# 061a — ブラウザ UI の方式の確定

Estimate 30 min
Splittable: no
Parent: 061-linux-gui.md

## 目的

Linux / macOS 向けのダッシュボードを **ブラウザ UI** とすることを確定させ、ネイティブ GUI（Qt、GTK、Tauri など）は **ペンディング** とする（`doc/design-v3-todo.md` 8、`doc/design-v3.md` §7 決定 2026-11-11）。

## やること

- [ ] サーバーをどこに置くかを決める。コア（`Hancho --headless`）に localhost の HTTP + WebSocket サーバーを埋め込むか、`hancho-cli serve` のような別コマンドにする。
- [ ] localhost の認証方法を決める。起動時に 1 回限りのトークンか、`hosts.json` と同じトークン。他プロセスからのアクセスを防ぐ。決定を文書化する。
- [ ] UI 資産（同梱の静的 HTML/JS）の置き場所と更新方法を決める。
- [ ] ネイティブ GUI をペンディングとする旨と、再開条件（ブラウザ UI で足りなかったもの）を文書に書く。

## 完了条件

- 方式・認証・資産の同梱が文書として確定している。
- ネイティブ GUI のペンディングが文書に記載されている。

## 参照

- `doc/design-v3-todo.md` 8
- `doc/design-v3.md` 2.4, §7
- 親カード `ww-data/tasks/061-linux-gui.md`

## Authorised edits

- `doc/`
- `CMakeLists.txt`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
# 文書のみのタスク。ビルドは不要
git diff -- doc/
```
