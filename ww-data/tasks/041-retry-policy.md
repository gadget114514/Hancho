# 041 — 分類に応じた再試行

Estimate 100 min
Splittable: no

## 目的

失敗の理由に応じて再試行の仕方を変える（`doc/design-v3.md` 4.5）。

## やること

- [ ] ジョブの `retry.on` / `retry.never` / `waitForRateLimit`。
- [ ] `rate_limited` は `TaskQueueDefer` と同じ扱いで、リセット時刻まで待ち `maxAttempts` を消費しない。
- [ ] `retry` 未指定なら従来どおり理由を問わず再試行する。
- [ ] 040 の分類と組み合わせる。

## 完了条件

- `timeout` / `rate_limited` / `auth_error` の再試行の違いがテストで固定されている。

## 参照

- `doc/design-v3.md` 4.5
- `doc/design-v3-en.md` Section 4

## Authorised edits

- `src/taskqueue.cpp`
- `src/taskqueue.h`
- `tests/`

## Read-only

- `doc/design-v3.md`
- `doc/design-v3-en.md`

## 検証

```powershell
cmake --build build --config Release
build\Release\taskqueue_smoke.exe
```
