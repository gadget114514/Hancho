# リモートホスト

別マシンのワーカーを手元の一覧から動かす仕組みです（`doc/design-v3.md` 2.5 段階 C）。

## 仕組み

- IPC の要求・応答の形は変えません。TCP 上の WebSocket テキストメッセージとして、
  1 接続につき 1 要求・1 応答を運びます。
- 既定では localhost だけで待ち受けます。それ以外のアドレスからの接続には
  トークンが必要です。TLS は実装せず、SSH トンネルに任せます。
- `"host"` を持つジョブは、そのホストの `hanchod` / ダッシュボードで動きます。
  ローカルとリモートのジョブは同じ一覧に出て、キュー列は `main @ buildbox` に
  なります。
- ログ、正規化イベント、`.done` はプロトコル経由で取り寄せ、次の場所に写します:
  - `<data-dir>/taskqueue/<job>.<attempt>.log`
  - `<job>.<attempt>.events.jsonl`
  - `<job>.<attempt>.done`
  - `<job>.<attempt>.tail.txt`

## hosts.json

`<data-dir>/hosts.json`:

```json
{
  "schema": "hancho.hosts/1",
  "hosts": [
    {
      "name": "buildbox",
      "address": "127.0.0.1:9137",
      "token_env": "HANCHO_TOKEN_BUILDBOX",
      "token_credential": "Hancho/host/buildbox"
    }
  ]
}
```

- `address` は `host:port`、`[::1]:port`、`ws://host:port` のいずれかです。
  `wss://` は拒否します。TLS はトンネル側で終端してください。
- `token_env` の既定値は `HANCHO_TOKEN_<NAME>`（大文字化し、英数字以外は `_`）。
- `token_credential` の既定値は Windows 資格情報マネージャーの
  `Hancho/host/<name>` です。
- トークン自体は `hosts.json` に書きません。

トークンの参照順:

1. `token_env` / `HANCHO_TOKEN_<NAME>`
2. Windows 資格情報マネージャーの `token_credential`
3. `HANCHO_TOKEN`（1 ホストだけをトンネルするときの共通トークン）

## 待ち受けの開始

待ち受けは明示したときだけ有効です:

```powershell
# localhost のみ。同じマシンのプロセスはトークンなしで接続できる
$env:HANCHO_REMOTE_LISTEN = "127.0.0.1:9137"
Hancho.exe gui

# ネットワークから到達可能。localhost 以外の接続はトークン必須
$env:HANCHO_REMOTE_LISTEN = "0.0.0.0:9137"
$env:HANCHO_REMOTE_TOKEN  = "<十分に長いランダム文字列>"
Hancho.exe gui
```

SSH トンネル経由の利用を推奨します:

```powershell
ssh -L 9137:127.0.0.1:9137 buildbox
hancho-cli hosts add buildbox 127.0.0.1:9137
```

トークンなしで外部アドレスを指定した場合も待ち受けは起動しますが、localhost
以外の接続はすべて拒否します。トークンを設定した場合は、SSH トンネル経由の
接続も localhost として届くため、localhost を含む全接続でトークンを要求します。

## ジョブ

```json
{ "id": "linux-test", "name": "linux-test", "host": "buildbox",
  "summary": "Linux のテストを通す", "command": "make test", "timeoutSec": 1800 }
```

- ジョブは手元で検証し（チェーンガード、必須項目）、起動だけをリモートの
  ダッシュボードに依頼します。ログと `.done` のパスはリモート側で決まります。
- 依存関係の順序は今までどおりです。
- リモートでは `success.filesNewer` を評価しません（そのパスは別マシンにあるため）。
  `success.outputRegex` か終了コードを使ってください。
- キャンセルすると、そのホストに `kill` を送ります。

## リソースロック

リソースの衝突は**ホストごと**に判定します。同じパスや同じ名前付きロック
（`@build`）でも、別ホストのジョブとは衝突しません。

## CLI

```powershell
hancho-cli hosts list
hancho-cli hosts add buildbox 127.0.0.1:9137 --token-env HANCHO_TOKEN_BUILDBOX
hancho-cli hosts remove buildbox
hancho-cli hosts ping buildbox

# IPC 要求をローカルの代わりに指定ホストへ送る
hancho-cli --host buildbox list
hancho-cli --host buildbox taskqueue list main
```

ホスト用に追加した IPC アクション:

- `hosts_list`（`hosts_reload` も同じ）
- `hosts_add`、`hosts_remove`、`hosts_ping`
- `remote_taskqueues`、`remote_processes` — ホストの一覧をまとめて返します。
  各項目に `host` が付き、エラーはホストごとに残ります。
- `get_done` — プロセスの `.done` の内容を返します。

## 制限

- クラスタスケジューラはありません。ホストはジョブ側で指定します。
- リモート実行中に手元のダッシュボードを再起動すると、手元の試行は interrupted
  として扱われます。リモートのプロセスには再接続しません。必要ならホスト側で
  終了してください。
- リモートでは `success.filesNewer` を評価しません（上記）。
- WebSocket 1 接続につき 1 要求・1 応答です。1 本のソケットでログを流し続ける
  形は今後の段階に回します。

## セキュリティ

- localhost は、トークンを設定していないときだけ暗黙に信頼します。トークンを
  設定した場合は、localhost を含むすべての接続でトークンを確認します。
- トークンは資格情報ストアか環境変数に置きます。`hosts list --json` は
  `token_present` だけを返し、トークンは返しません。
- ポートを直接公開せず、SSH トンネルを使ってください。TLS はありません。
- リモートの待ち受けはローカルのパイプと同じ IPC アクションを持つので、
  トークンはこの Hancho を操作してよいマシンにだけ渡してください。
