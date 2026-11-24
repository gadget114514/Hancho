# デモモード

スクリーンショットや説明用に、決まった時刻と決まったデータで Hancho を動かすためのビルド。配布物には入れない。

## 何ができるか

- 時刻を固定する（既定は `2026-11-21 15:00:00`）。画面の時刻・経過時間・使用量のグラフが毎回同じになる。
- データディレクトリにデモ用のデータを作る:
  - 使用量の履歴（直近 24 時間、`usage_history.jsonl`）と日ごとの集計（`usage/daily/<年-月>.jsonl`、既定 30 日分）。
  - キュー `demo`（`taskqueues/demo.json`）: 終わったジョブ 3 件と、依存つきで待っているジョブ 3 件。待っているジョブはダミーの LLM（`hancho-dummy.exe`）を動かす。
- ダミーの LLM `hancho-dummy.exe`: Claude の stream-json の形で、決まった出力（トークン使用量、サブエージェント）を出す。`--scenario usage|subagents|mixed`、`--seed <n>` で中身が決まり、同じ引数なら毎回同じ出力になる。

## ビルド

通常のビルド（`build\`）では作らない。CMake のオプション `HANCHO_BUILD_DEMO`（既定 `OFF`）を `ON` にした別のビルドディレクトリ `build-demo\` で作る。

```
build_demo.cmd            # Release。最後に demo_smoke を実行する
build_demo.cmd Debug
build_demo.cmd Release --no-test
```

手で行うなら:

```
cmake -S . -B build-demo -G "Visual Studio 17 2022" -A x64 -DHANCHO_BUILD_DEMO=ON
cmake --build build-demo --config Release --target Hancho hancho-cli hancho-dummy demo_smoke
build-demo\Release\demo_smoke.exe
```

- `HANCHO_BUILD_DEMO=ON` にすると、全体に `HANCHO_DEMO_BUILD` が定義され、`Hancho.exe` / `hancho-cli.exe` に `src/demo.cpp` と `--demo` のオプションが入る。ほかに `hancho-dummy.exe`（`src/dummy_llm.cpp`）と `demo_smoke.exe`（`tests/demo_smoke.cpp`）ができる。
- `build-demo\` の exe はリリースに使わない。リリースの手順（`CLAUDE.md`）は `build\` だけを使う。

## 使い方

**必ず試験用のデータディレクトリを指定する。** デモの準備は、データディレクトリの `usage_history.jsonl`、`usage/daily/`、`taskqueues/<queue>.json` を上書きする。いつものデータディレクトリで動かすと、使用量の記録とキューが消える。

```
mkdir %TEMP%\hancho-demo
copy tests\data\demo\demo.json %TEMP%\hancho-demo\demo.json
build-demo\Release\Hancho.exe --data-dir %TEMP%\hancho-demo --demo
```

- `--demo`: デモモードで起動し、データを作る。`--demo-now "2026-11-21 15:00:00"` で固定する時刻を変えられる（`demo.json` の `now` より優先）。
- 環境変数 `HANCHO_DEMO=1`（と `HANCHO_DEMO_NOW`）でも同じ。`--demo` を付けた Hancho が起動するプロセスにはこれが引き継がれる。
- CLI:
  ```
  build-demo\Release\hancho-cli.exe --data-dir %TEMP%\hancho-demo --demo demo prepare   # データを作り直す
  build-demo\Release\hancho-cli.exe --data-dir %TEMP%\hancho-demo --demo demo status    # デモモードかどうかと今の時刻
  build-demo\Release\hancho-cli.exe demo output mixed 1                                 # ダミーの LLM の出力を表示
  ```
- `hancho-dummy.exe` は `Hancho.exe` と同じフォルダから探す（`GetDummyLlmExecutableUtf8`）。`engines.json` のコマンドには `{dummy}` でその場所を書ける。

## `demo.json`

データディレクトリの `demo.json` で中身を変える。なければ既定の値を使う。例は `tests/data/demo/demo.json`。

| キー | 既定 | 意味 |
|---|---|---|
| `now` | `2026-11-21 15:00:00` | 固定する時刻（ローカル時刻） |
| `days` | 30 | 日ごとの集計を何日分作るか（1〜365） |
| `points_per_hour` | 4 | 24 時間の履歴の 1 時間あたりの点の数（1〜60） |
| `tokens_per_day` | 180000 | 1 日のトークン数の目安 |
| `cost_per_day` | 5.0 | 1 日の費用（USD）の目安 |
| `queue` | `demo` | 作るキューの名前 |
| `models` | `claude` / `claude-sonnet-4`（重み 3）、`opencode` / `grok-code-fast-1`（重み 2） | 使用量を割り振るエンジンとモデル。`weight` の比で分ける |

## テスト

`demo_smoke.exe` は、一時フォルダにデータを作り、時刻が固定されること、24 時間の履歴が 96 点あること、1h / 24h / 30d の使用量の集計が空でないこと、キューに依存つきのジョブがあること、ダミーの LLM の出力が同じ引数で同じになり、トークン使用量とサブエージェントが読めることを確かめる。通常のテストの一覧（リリースの手順）には入れない。

## ファイル

| ファイル | 中身 |
|---|---|
| `src/demo.h` / `src/demo.cpp` | `demo.json` の読み込み、データの生成、ダミーの LLM の出力 |
| `src/dummy_llm.cpp` | `hancho-dummy.exe` の `main` |
| `src/common.cpp` | 時刻の固定（`IsDemoMode`、`SetDemoMode`、`EnableDemoModeFromArgs`）、`GetDummyLlmExecutableUtf8` |
| `src/main.cpp`、`src/cli.cpp` | `--demo` / `--demo-now`、`hancho-cli demo` |
| `tests/demo_smoke.cpp` | 上のテスト |
| `tests/data/demo/demo.json` | `demo.json` の例 |
| `build_demo.cmd`（リポジトリのルート。`tools\` はリリースの zip に入るので置かない） | `build-demo\` でのビルドとテスト |
