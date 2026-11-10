# 020 — ヘッドレス起動と GUI の IPC クライアント化

Estimate 240 min
Splittable: no

## 目的

画面なしでコアを動かし、GUI を外してもジョブが止まらないようにする（`doc/design-v3.md` 2.3 段階 A）。

## やること

- [ ] `Hancho.exe --headless`（ウィンドウ・トレイなしでコアと IPC を起動）。
- [ ] GUI はコアの IPC クライアントにする。
- [ ] GUI を閉じてもジョブを止めず、後から再接続できるようにする。
- [ ] ヘッドレス起動と再接続のテストを追加する。

## 完了条件

- `--headless` でウィンドウなしに起動し、`hancho-cli` から操作できる。
- GUI を閉じても実行中のジョブが継続する。

## 参照

- `doc/design-v3.md` 2.3
- `doc/design-v3-en.md` Section 2.3

## Authorised edits

- `src/main.cpp`
- `src/process_manager.cpp`
- `src/process_manager.h`
- `src/ipc_server.cpp`
- `src/ipc_server.h`
- `src/gui_dashboard.cpp`
- `tests/`
- `CMakeLists.txt`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\bt_ipc_smoke.exe
build\Release\taskqueue_smoke.exe
```
