# 060d — headless 単独運用の確認と OS 依存の分離

Estimate 80 min
Splittable: no
Parent: 060-gui-ipc-client.md

## 前提

- `060-2-gui-ipc-replace.md`（GUI の IPC 化が終わっていること）

## 目的

コア単体の運用を壊さず、GUI を Linux / macOS 向けにも移植できる構成にしておく（`doc/design-v3.md` 2.3, 2.4）。

## やること

- [ ] コア単体（`--headless`）で GUI なしの運用が崩れないことを確認する。
- [ ] GUI を Linux / macOS 向けにも移植可能な構成にするため、OS 依存の描画と IPC 以外の呼び出しを分ける。

## 完了条件

- `headless_smoke` と既存の GUI ビルドが通る。
- OS 依存の呼び出しが層として分かれている（移植のカードから参照できる形）。

## 参照

- `doc/design-v3.md` 2.3, 2.4
- 親カード `ww-data/tasks/060-gui-ipc-client.md`

## Authorised edits

- `src/gui_dashboard.cpp`
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
