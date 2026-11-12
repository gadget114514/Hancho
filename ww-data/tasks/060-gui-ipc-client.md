# 060 — GUI をコアの IPC クライアントにする

Estimate 300 min
Splittable: no

## 目的

Windows の Direct2D GUI をコアの外に出し、`Hancho.exe --headless` と GUI を独立させた状態にする（`doc/design-v3.md` 2.3 段階 A、`doc/design-v3-todo.md` 8 の前提）。

## やること

子カードに分解した。個々の作業は子カードで管理する:

- [ ] `ww-data/tasks/060-1-gui-ipc-requests.md` — GUI が必要な IPC 要求の整備
- [ ] `ww-data/tasks/060-2-gui-ipc-replace.md` — GUI の状態読み取りの IPC 置き換え
- [ ] `ww-data/tasks/060-3-gui-standalone.md` — GUI 単体起動と再接続の確認
- [ ] `ww-data/tasks/060-4-gui-headless-split.md` — headless 単独運用の確認と OS 依存の分離

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
