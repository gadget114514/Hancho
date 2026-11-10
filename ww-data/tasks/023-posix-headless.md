# 023 — Linux / macOS のヘッドレスコア

Estimate 480 min
Splittable: no

## 目的

POSIX 環境でコアと CLI を動かす（`doc/design-v3.md` 2.4 段階 B）。画面は `hancho-cli top` だけ。

## やること

- [ ] 022 の境界に POSIX 実装を入れる（forkpty、setsid / cgroup v2、Unix ドメインソケット、flock、POSIX クォート）。
- [ ] Linux でヘッドレスのコアと CLI をビルド・起動できるようにする。
- [ ] `hancho-cli top`（端末向けの簡易表示）を追加する。
- [ ] ChainGuard / ww 系の POSIX シェル規則を別に持つ。
- [ ] CI 用のビルド手順を追加する。

## 完了条件

- Linux でキューと BT が動く。
- コアの単体テストが Linux で通る。

## 参照

- `doc/design-v3.md` 2.4
- `doc/design-v3-en.md` Section 2.4

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
# Windows ではビルドと既存テストのみ
cmake --build build --config Release
build\Release\taskqueue_smoke.exe
```
