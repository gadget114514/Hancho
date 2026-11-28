# デモモード（決定的実行モード）

本質は回帰テスト用の、決まった時刻と決まったデータで Hancho を動かす仕組み。同じ入力から毎回同じ出力を得るため、回帰テスト（[regression-test-cli-mcp-core-plan.md](regression-test-cli-mcp-core-plan.md)）の基盤になる。スクリーンショットを撮るときにも使えるが、それは副次的な用途。

リリースのバイナリにも入っているが、オプションを付けたときだけ動く。既定の動作は変わらない。

> 状態: 方針は承認済み（[regression-test-cli-mcp-core-plan.md](regression-test-cli-mcp-core-plan.md) §4）。このページの「計画」と書いた部分は未実装。

## 何ができるか

- 時刻を固定する（既定は `2026-11-21 15:00:00`）。画面の時刻・経過時間・使用量のグラフが毎回同じになる。
- データディレクトリにデモ用のデータを作る:
  - 使用量の履歴（直近 24 時間、`usage_history.jsonl`）と日ごとの集計（`usage/daily/<年-月>.jsonl`、既定 30 日分）。
  - キュー `demo`（`taskqueues/demo.json`）: 終わったジョブ 3 件と、依存つきで待っているジョブ 3 件。待っているジョブは LLM の代わりに、ダミーの LLM を動かす。
- ダミーの LLM:
  - C++ 版 `hancho-dummy.exe`（`src/dummy_llm.cpp`）: `demo_smoke` が使う。
  - Python 版 `tests/regression/dummy_llm.cmd`（計画）: 回帰テストが使う。core には `--dummy-llm <path>` で渡す。
  - どちらも Claude の stream-json の形で、決まった出力（トークン使用量、サブエージェント）を出す。`--scenario usage|subagents|mixed`、`--seed <n>` で中身が決まり、同じ引数なら毎回同じ出力になる。

## ビルド

リリースビルド（`build\`）にデモ機能が含まれる。CMake のオプション `HANCHO_BUILD_DEMO` は、テスト用のバイナリ（`hancho-dummy.exe`、`demo_smoke.exe`）を作るかどうかだけを決める（既定 `OFF`）。

```
cmake --build build --config Release --target Hancho hancho-cli     # デモ機能を含むリリースバイナリ
build_demo.cmd            # HANCHO_BUILD_DEMO=ON で build-demo\ を作り、demo_smoke を実行する（Release）
build_demo.cmd Debug
build_demo.cmd Release --no-test
```

- `build_demo.cmd` は C++ 版のダミーと `demo_smoke` のためのもの。配布用のバイナリは作らない。
- 配布用の zip とインストーラーは、従来どおり `build\` から作る。

## 使い方

**試験用のデータディレクトリを指定する。** デモの準備は、データディレクトリの `usage_history.jsonl`、`usage/daily/`、`taskqueues/<queue>.json` を上書きする。いつものデータディレクトリで動かすと、使用量の記録とキューが消える。

```
mkdir %TEMP%\hancho-demo
copy tests\data\demo\demo.json %TEMP%\hancho-demo\demo.json
build\Release\Hancho.exe --data-dir %TEMP%\hancho-demo --demo
```

- `--demo`: デモモードで起動し、データを作る。`--demo-now "2026-11-21 15:00:00"` で固定する時刻を変えられる（`demo.json` の `now` より優先）。
- 環境変数 `HANCHO_DEMO=1`（と `HANCHO_DEMO_NOW`）でも同じ。`--demo` を付けた Hancho が起動するプロセスにはこれが引き継がれる。
- `--dummy-llm <path>`（計画）: デモのキューのダミージョブに使うコマンドを指定する。既定は `hancho-dummy.exe`（`Hancho.exe` と同じフォルダから探す。`GetDummyLlmExecutableUtf8`）。`engines.json` のコマンドでは `{dummy}` でその場所を書ける。
- CLI:
  ```
  build\Release\hancho-cli.exe --data-dir %TEMP%\hancho-demo --demo demo prepare   # データを作り直す
  build\Release\hancho-cli.exe --data-dir %TEMP%\hancho-demo --demo demo status    # デモモードかどうかと今の時刻
  build\Release\hancho-cli.exe demo output mixed 1                                 # ダミーの LLM の出力を表示
  ```

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

- `demo_smoke.exe`（C++ 版ダミー）: 一時フォルダにデータを作り、時刻が固定されること、24 時間の履歴が 96 点あること、1h / 24h / 30d の使用量の集計が空でないこと、キューに依存つきのジョブがあること、ダミーの LLM の出力が同じ引数で同じになり、トークン使用量とサブエージェントが読めることを確かめる。通常のテストの一覧（リリースの手順）には入れない。
- 回帰テスト（計画）: `tests/regression/` の Python ランナーが、同じ固定時刻とデータで core・CLI・MCP の出力をゴールデンと比べる。詳細は [regression-test-cli-mcp-core-plan.md](regression-test-cli-mcp-core-plan.md)。

## ファイル

| ファイル | 中身 |
|---|---|
| `src/demo.h` / `src/demo.cpp` | `demo.json` の読み込み、データの生成、ダミーの LLM の出力 |
| `src/dummy_llm.cpp` | `hancho-dummy.exe` の `main`（テスト用） |
| `tests/regression/dummy_llm.py`、`dummy_llm.cmd`（計画） | Python 版のダミー LLM（回帰テスト用） |
| `src/common.cpp` | 時刻の固定（`IsDemoMode`、`SetDemoMode`、`EnableDemoModeFromArgs`）、`GetDummyLlmExecutableUtf8` |
| `src/main.cpp`、`src/cli.cpp` | `--demo` / `--demo-now`、`hancho-cli demo` |
| `tests/demo_smoke.cpp` | 上のテスト |
| `tests/data/demo/demo.json` | `demo.json` の例 |
| `build_demo.cmd`（リポジトリのルート。`tools\` はリリースの zip に入るので置かない） | `build-demo\` でのビルドとテスト |
