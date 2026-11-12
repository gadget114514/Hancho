# 080-2 — スモークテストの更新と通過確認

Estimate 100 min
Splittable: no
Parent: 080-integration-fix.md

## 前提

- `080-1-build-fix.md`（ビルドが通っていること）

## 目的

31 枚のカードが追加したテストと、既存テストを新しいコードに合わせて直し、スモークテストをすべて通す。

## やること

- [ ] `cmake --build build --config Release` で全ターゲットをビルドする（テストの exe も）。
- [ ] 各スモークテストを 1 つずつ実行し、失敗したものを直す:
  - `build\Release\memory_search_smoke.exe`（054 / 055 / 072-2 / 073 が更新）
  - `build\Release\memory_record_smoke.exe`（072-1 / 072-2 が更新）
  - `build\Release\taskqueue_smoke.exe`（053 / 072-1 が更新）
  - `build\Release\llm_stream_smoke.exe`（051-1 / 051-2 が更新）
  - `build\Release\event_schema_smoke.exe`（071 が更新）
  - `build\Release\event_adapter_smoke.exe`
  - `build\Release\headless_smoke.exe`（060-3 が更新）
  - `build\Release\bt_ipc_smoke.exe`（058 / 060 が更新）
  - `build\Release\webui_smoke.exe`（061-3 が新規追加。如果 CMake に登録されていなければ追加する）
  - `build\Release\posix_core_smoke.exe`（062 / 072-1 が更新）
- [ ] 失敗が「実装が仕様を満たしていない」場合は実装側を直す。ただし仕様は `doc/design-v3.md` と該当カードに書くこと。
- [ ] テストが新機能（usage の日次集計、memory prune と容量上限、ランカープラグイン、自動ノート、リモート再接続と filesNewer、WS 購読、ブラウザ UI、TUI、スキーマの型、キューの外プロセスの RunRecord、L3 ワーカー昇格）を実際に検証していることを確認する。検証していない機能があればテストを追加する。
- [ ] 失敗するテストを「進めるためだけの閾値を緩める」で通さない。仕様どおりの期待値に直す。

## 完了条件

- 上記のスモークテストがすべて exit 0 で通る。
- 失敗やスキップを放置していない（テストを削除して通した形は不可）。
- 080-1 で直した API がテストからも使われている。

## 参照

- `doc/design-v3.md` 3, 4
- `ww-data/tasks/`（050〜073）
- 親カード `ww-data/tasks/080-integration-fix.md`

## Authorised edits

- `tests/`
- `CMakeLists.txt`
- `src/`
- `doc/`

## Read-only

- `ww-data/`

## 検証

```powershell
cmake --build build --config Release
build\Release\memory_search_smoke.exe
build\Release\memory_record_smoke.exe
build\Release\taskqueue_smoke.exe
build\Release\llm_stream_smoke.exe
build\Release\event_schema_smoke.exe
build\Release\event_adapter_smoke.exe
build\Release\headless_smoke.exe
build\Release\bt_ipc_smoke.exe
build\Release\webui_smoke.exe
build\Release\posix_core_smoke.exe
```
