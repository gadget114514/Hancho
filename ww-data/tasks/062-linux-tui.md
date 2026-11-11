# 062 — TUI ダッシュボード（hancho-cli top）

Estimate 480 min
Splittable: no

## 目的

`hancho-cli top` を簡易表示のままにせず、ダッシュボードと同等の端末向け画面に育てる（`doc/design-v3-todo.md` 8）。

## やること

- [ ] TUI の実装方法を決める（ライブラリを使うか自前で書くか）。決定を文書化する。
- [ ] プロセス一覧、タスクキュー、実行状態監視ペインを端末に表示する。
- [ ] 失敗診断（`taskqueue why` と同じ内容）とメモリ一覧を見られるようにする。
- [ ] ジョブの操作（停止、再実行、取り消し）をキーボードから行えるようにする。
- [ ] リモートのジョブも同じ画面に出す。
- [ ] POSIX 環境での動作確認の手順を文書化する。
- [ ] スモークテストを追加する（出力の成型部分を単体テストできる構成にする）。

## 完了条件

- 端末だけで、ダッシュボードと同じ見通しでジョブを監視・操作できる。

## 参照

- `doc/design-v3-todo.md` 8
- `doc/design-v3.md` 2.4

## Authorised edits

- `src/cli.cpp`
- `src/cli_top*`
- `CMakeLists.txt`
- `doc/`
- `tests/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`
- `src/gui_dashboard.cpp`

## 検証

```powershell
# Windows ではビルドと既存テストのみ（POSIX 環境で動作確認する）
cmake --build build --config Release
build\Release\posix_core_smoke.exe
```
