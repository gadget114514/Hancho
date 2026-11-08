# プロジェクト内データディレクトリ (git 管理)

**[English](data-directory.md) | [日本語 (Japanese)](data-directory-ja.md)**

ログ・プロセス状態・タスクキュー・GUI 設定は 1 つの**データディレクトリ**に保存されます。`--data-dir <path>` で**呼び出しごと**に指定できます:

```powershell
hancho-cli.exe --data-dir .hancho taskqueue add --file jobs.json
hancho-cli.exe taskqueue list main --data-dir .hancho
hancho-cli.exe mcp install --data-dir .hancho   # プロジェクトの MCP 設定に埋め込む
```

一貫性の保証方法:

1. プロジェクトごとに 1 つのディレクトリ（例: `.hancho/`）を決める。
2. `hancho-cli.exe mcp install --data-dir .hancho` を一度実行すると、`.mcp.json` / `opencode.json` に `--data-dir .hancho` が書き込まれ、そのプロジェクトの MCP 呼び出しはすべて同じディレクトリを使う。
3. 直接 `hancho-cli.exe` を呼ぶときは**毎回同じ `--data-dir`** を渡す（グローバル設定ではない）。
4. 相対パスは呼び出し時の cwd 基準で解決される。MCP 側の cwd が不明なら MCP 設定に絶対パスを書く。
5. データディレクトリごとにダッシュボードインスタンス・Named Pipe・ミューテックスが分離され、複数プロジェクトを同時に運用してもプロセス/キューを共有しない。
6. 確認: `hancho-cli.exe --data-dir <path> help --json` の `effective_data_dir`、またはダッシュボードのステータスバー。

環境変数 `HANCHO_LOG_DIR` も同等の指定方法です。
