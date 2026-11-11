# 060 — GUI をコアの IPC クライアントにする

Estimate 480 min
Splittable: no

## 目的

Windows の Direct2D GUI をコアの外に出し、`Hancho.exe --headless` と GUI を独立させた状態にする（`doc/design-v3.md` 2.3 段階 A、`doc/design-v3-todo.md` 8 の前提）。

## やること

- [ ] GUI が直接読んでいるコアの状態を、IPC（Named Pipe 越し）に置き換える。IPC にない要求は足す。
- [ ] GUI 単体で起動してコアにつなぐ経路を作る。コアがなければ起動を促す。
- [ ] GUI を閉じてもコアのジョブは止まらないことを確認する。再接続も確認する。
- [ ] コア単体（`--headless`）で GUI なしの運用が崩れないことを確認する。
- [ ] GUI を Linux / macOS 向けにも移植可能な構成にするため、OS 依存の描画と IPC 以外の呼び出しを分ける。

## 完了条件

- GUI がコアの IPC クライアントとして動き、プロセス一覧・タスクキュー・監視ペイン・失敗診断・メモリ一覧が従来どおり出る。
- `headless_smoke` と既存の GUI ビルドが通る。

## 参照

- `doc/design-v3.md` 2.3, 2.4
- `doc/design-v3-todo.md` 8

## Authorised edits

- `src/gui_dashboard.cpp`
- `src/main.cpp`
- `src/ipc_server.cpp`
- `src/ipc_client.cpp`
- `CMakeLists.txt`
- `tests/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\headless_smoke.exe
```
