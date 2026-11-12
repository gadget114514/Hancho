# 060c — GUI 単体起動と再接続の確認

Estimate 75 min
Splittable: no
Parent: 060-gui-ipc-client.md

## 前提

- `060-2-gui-ipc-replace.md`（GUI が IPC クライアントになっていること）

## 目的

GUI をコアと別プロセスとして起動・再接続できる形にする（`doc/design-v3.md` 2.3 段階 A）。

## やること

- [ ] GUI 単体で起動してコアにつなぐ経路を作る。コアがなければ起動を促す。
- [ ] GUI を閉じてもコアのジョブは止まらないことを確認する。再接続も確認する。

## 完了条件

- GUI を閉じてもジョブが止まらず、GUI を開き直すと復元される。

## 参照

- `doc/design-v3.md` 2.3
- 親カード `ww-data/tasks/060-gui-ipc-client.md`

## Authorised edits

- `src/gui_dashboard.cpp`
- `src/main.cpp`
- `tests/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\headless_smoke.exe
```
