# 022 — プラットフォーム抽象層

Estimate 360 min
Splittable: no

## 目的

OS 依存を境界の後ろへ移す（`doc/design-v3.md` 2.4）。このカードでは Windows の挙動を変えない。

## やること

- [ ] `IPty`（ConPTY）、`IProcessTree`（Job Object）、`IIpcTransport`（Named Pipe）、`IInstanceLock`（mutex）、`IShellQuote` を定義する。
- [ ] Win32 を直接呼ぶ箇所を洗い出し、Win32 実装を境界の内側へ移す。
- [ ] コアのロジック（キュー、BT、ストリーム解析）は OS 非依存のまま共有する。
- [ ] 境界の単体テストを追加する。

## 完了条件

- Windows の既存テストがすべて通る。
- コアから `#include <windows.h>` を直接使う箇所が減っている（境界の外だけ）。

## 参照

- `doc/design-v3.md` 2.4
- `doc/design-v3-en.md` Section 2.4

## Authorised edits

- `src/`
- `include/`
- `CMakeLists.txt`
- `tests/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
for %t in (taskqueue_smoke llm_stream_smoke bt_ipc_smoke) do build\Release\%t.exe
```
