# 080-1 — コンパイルエラーの解消（API 定義の統一）

Estimate 95 min
Splittable: no
Parent: 080-integration-fix.md

## 目的

31 枚のカードが独立に作ったコードの境界の不整合を解消し、Release ビルドを通す。各カードが自分のファイルだけを直したため、**片側が呼ぶ API をもう片側が定義していない**、というミスマッチ集中在 1 枚にまとめる。

## やること

ビルドエラーは次のとおり（2026-11-11 時点の Release ビルド）。すべて直したらビルドが通ること。

- [ ] `src/gui_dashboard.cpp` 1339-1371: `pc::memory::StorageStats`、`pc::memory::GetMemoryStorageStats()`、`pc::memory::Query::tags` が未定義。054（memory prune）が GUI 側から呼び、core 側に定義が無い。core 側（`src/memory_search.h` / `src/memory_search.cpp` / `src/memory_record.*`）に定義を足すか、GUI 側の呼び方を定義済み API に合わせるかを決める（054 の設計意図は L1/L2/L3 別の容量と上限接近の警告）。
- [ ] `src/gui_dashboard.cpp` 1007: `pc::IpcServer::DispatchJson` が無い。060（GUI を IPC クライアント化）が GUI 側から呼ぶ API。`src/ipc_server.h` / `src/ipc_server.cpp` に定義があるか、名前が違うかを合わせる。
- [ ] `src/gui_dashboard.cpp` 10593, 10650: `LocalTimeFromSec` が無い。`src/common.h` / `src/common.cpp` にヘルパーがあるか、名前が違うかを合わせる。
- [ ] `src/gui_dashboard.cpp` 7758: `Ended` が無い（構造体のメンバー名不一致）。どの型を指しているか確認して、名前を揃える。
- [ ] `src/ipc_server.cpp` 16: `error C1075: '{'`（構文エラー）。直前の宣言か include の不整合。該当箇所を特定して構文を直す。
- [ ] `src/taskqueue.cpp` 1918: `Trim` が無い。057 / 072-1 が使った整形 helper が `src/common.h` に無い。
- [ ] `src/taskqueue.cpp` 4165: `GetAttemptDonePathWide` が無い。done ファイルのパス helper の定義場所（`src/taskqueue.h` / `src/ipc_client.h`）に追加するか、既存の関数名に合わせる。
- [ ] `src/memory_record.cpp` 663: `ClassifyDeterministicFailure` のオーバーロードを解決できない。053 と 072-2 が同名の別実装を追加している。1 つにまとめる（引数の型を揃える）。
- [ ] `src/cli.cpp` 3974: `ShellExecuteW` が無い。Windows 側の include（`<shellapi.h>`）が欠けている。
- [ ] `src/webui_assets.cpp` 371, 1554, 2139, 2537, 2999: `error C2026: 文字列が大きすぎます`。061-3 が埋め込んだアセット文字列が MSVC の 1 文字列長上限（65535 バイト）を超えている。文字列リテラルを分割するか、生の文字列リテラルや `std::string` の連結に置き換える。
- [ ] `filesystem(206,13) error C2338: invalid value_type`: `std::filesystem::path` に `char` と `char16_t` / `wchar_t` が混在している箇所を特定して型を揃える。
- [ ] 上記を直したあと、`cmake --build build --config Release --target Hancho hancho-cli` が通るまで詰める。新たなエラーが出たら、それも直してよい。

## 完了条件

- `cmake --build build --config Release --target Hancho hancho-cli` がエラー 0 で通る（警告は可）。
- 直した API は 1 箇所に定義されている（同名実装が 2 箇所に残っていない）。
- 既存コードの意味を変えていない（memory のスコープ既定、brief のヘッダ、L1 の形式は 080-1 では変えない）。

## 参照

- `doc/design-v3.md` 2.3, 3.7, 4.2
- `ww-data/tasks/054-memory-prune.md`
- `ww-data/tasks/060-gui-ipc-client.md`
- `ww-data/tasks/061-linux-gui.md`
- `ww-data/tasks/072-runrecord-plain-process.md`
- 親カード `ww-data/tasks/080-integration-fix.md`

## Authorised edits

- `src/`
- `CMakeLists.txt`

## Read-only

- `tests/`
- `doc/`
- `ww-data/`

## 検証

```powershell
cmake --build build --config Release --target Hancho hancho-cli
```
