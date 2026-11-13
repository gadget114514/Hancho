# IP Messenger (IPMsg) 互換 LAN メッセージング

**[English](ipmsg.md) | [日本語 (Japanese)](ipmsg-ja.md)**

Hancho は古典的な IP Messenger (IPMsg) プロトコルを話せるため、同じ LAN 上の既存の IPMsg クライアントから Hancho がホストとして見え、メッセージとファイルをやり取りできます。このチャネルは LLM 同士のメッセージングにも使います。受信メッセージをワーカー LLM に渡し、その回答を通常の IPMsg 返信として送り返せます。

この機能は**既定で無効**で、**信頼できる LAN 専用**です。暗号化も認証もありません。

## 対応範囲

| IPMsg コマンド | 対応 |
| :--- | :--- |
| `BR_ENTRY` / `ANSENTRY` / `BR_ABSENCE` / `BR_EXIT` | 対応。参加通知、参加への応答、ホスト一覧の追跡、終了時の削除。 |
| `SENDMSG` | 対応（本文、ブロードキャスト、マルチキャスト、開封確認、ファイル添付）。 |
| `RECVMSG` / `READMSG` / `ANSREADMSG` | 対応。到達・既読の通知で送信メッセージの状態を更新。 |
| `GETINFO` / `SENDINFO` | 対応。`Hancho <version> (IPMsg compatible)` を返します。 |
| `GETABSENCEINFO` / `SENDABSENCEINFO` | 対応（不在モード自体は使いません）。 |
| `GETFILEDATA` / `RELEASEFILES` | **通常ファイル**に対応。フォルダは未対応として扱い、転送しません。 |
| `DELMSG` | 無視します。 |
| 秘匿モード | 解析はしますが保護はしません。メッセージは暗号化されません。 |

文字コード:

- `IPMSG_UTF8OPT` がある場合は UTF-8、ない場合は CP932 (Shift_JIS) として解釈します。Hancho は `IPMSG_CAPUTF8OPT` を広告し、相手も対応していれば UTF-8 で送信し、そうでなければ CP932 でエンコードします。これにより日本語の既存 IPMsg クライアントともやり取りできます。
- ワイヤ上の CRLF は保存時に LF へ正規化します。

既定ポートは IPMsg の 2425 ではなく **2426** (`kIpmsgDefaultPort`) です。同じ PC でデスクトップ版 IPMsg が動いていても衝突しないようにするためです。Hancho と通信するには、相手クライアントに `192.168.x.y:2426` をホストとして追加するか、IPMsg のポートを 2426 に変更してください。Hancho は `255.255.255.255` と `hosts` の各エントリへ参加通知をブロードキャストします。

## セキュリティ

- **信頼できる LAN 専用。** ループバック、プライベート (RFC 1918)、リンクローカル、CGNAT の IPv4 からのパケットだけを受け付けます。パブリックアドレスは決して受け付けません。`allow_from` でさらに絞れます（`"192.168.1.0/24"`, `"10.0.0.5"`）。
- **暗号化も認証もありません。** 許可されたネットワーク上の誰でも、別ホストを騙ったメッセージやファイルを送れます。インターネットや信頼できない Wi-Fi では使わないでください。
- **受信テキストは信頼できない入力です。** LLM ハンドラのプロンプトはメッセージを明示的な「信頼できないデータ」ブロックで囲み、中の指示に従わないようワーカーに指示します。それでもプロンプトインジェクションは可能です。`handler.instructions` は狭く保ち、ワーカーの `cwd` で何ができるかを確認してください。
- **`skip_permissions` は危険です。** ネットワーク入力で起動されたワーカーに、エンジンの権限バイパス引数を渡します。LAN 上の全ホストを完全に信頼できる場合以外は `false` のままにしてください。

## 設定 (`config.json`)

設定は `<data-dir>/config.json`（既定は `%LOCALAPPDATA%\Programs\Hancho\logs\config.json`）にあります。ブロックがない、または一部だけの場合は `config_get` が以下の既定値を補います。

```json
"ipmsg": {
  "enabled": false,
  "port": 2426,
  "bind": "0.0.0.0",
  "nickname": "",
  "group": "Hancho",
  "hosts": [],
  "allow_from": [],
  "download_dir": "",
  "auto_download": false,
  "max_file_mb": 1024,
  "max_messages": 2000,
  "handler": {
    "enabled": false,
    "engine": "agy",
    "model": "",
    "agent": "",
    "cwd": "",
    "instructions": "",
    "taskqueue": "ipmsg",
    "auto_reply": true,
    "skip_permissions": false,
    "timeout_sec": 600,
    "max_hops": 4,
    "concurrency": 1
  }
}
```

| キー | 既定値 | 意味 |
| :--- | :--- | :--- |
| `enabled` | `false` | UDP/TCP ノードを起動する。変更するとノードを再起動。 |
| `port` | `2426` | 参加通知・メッセージ用 UDP とファイル転送用 TCP のポート。 |
| `bind` | `"0.0.0.0"` | ローカルのバインドアドレス。 |
| `nickname` | `""` | 他クライアントに表示する名前。空 = `Hancho@<host>`。 |
| `group` | `"Hancho"` | IPMsg のグループ名。 |
| `hosts` | `[]` | 追加のブロードキャスト/ユニキャスト先（`"192.168.1.255"`, `"10.0.0.5:2426"`）。 |
| `allow_from` | `[]` | 受け付ける送信元ネットワーク/ホスト。空 = プライベート/ループバック/リンクローカル全て。 |
| `download_dir` | `""` | 受信ファイルの保存先。空 = `<data-dir>/ipmsg/files`。 |
| `auto_download` | `false` | メッセージ受信時に自動でダウンロードする。 |
| `max_file_mb` | `1024` | ダウンロードする 1 ファイルあたりの上限サイズ。 |
| `max_messages` | `2000` | 履歴に保持するメッセージ数（10〜100000）。 |
| `handler.enabled` | `false` | 条件に合う受信メッセージをワーカー LLM に渡す。 |
| `handler.engine` | `"agy"` | ワーカーエンジン名（`hancho-cli engines` 参照）。 |
| `handler.model` | `""` | モデルの上書き。空 = エンジン既定。 |
| `handler.agent` | `""` | このエージェント宛のメッセージも処理する。空 = 宛先なしのみ。設定時は別エージェント宛を除外。 |
| `handler.cwd` | `""` | ワーカーの作業ディレクトリ。空 = データディレクトリ。 |
| `handler.instructions` | `""` | ハンドラのプロンプト先頭に足す追加指示。 |
| `handler.taskqueue` | `"ipmsg"` | ハンドラのジョブを入れるタスクキュー。 |
| `handler.auto_reply` | `true` | ワーカーの回答を自動で返信する。 |
| `handler.skip_permissions` | `false` | エンジンの権限バイパス引数を渡す（セキュリティ参照）。 |
| `handler.timeout_sec` | `600` | 1 メッセージあたりのワーカー制限時間。 |
| `handler.max_hops` | `4` | 返信チェーンがこのホップ数に達したら返信を止める。 |
| `handler.concurrency` | `1` | ハンドラジョブのキュー並列度。 |

`hancho-cli msg handler ...`、ダイアログの設定タブ、`config_set` での変更は `config.json` に保存され、コアを再起動せずにノードとハンドラが再読み込みされます。

## メッセージ JSON

IPC、CLI、MCP、履歴ファイルは同じ形を使います。

```json
{
  "id": 12,
  "time": "2026-11-13 20:31:12",
  "time_ms": 1794569472000,
  "direction": "in",
  "peer": {"key": "192.168.1.20:2426", "addr": "192.168.1.20", "port": 2426,
           "user": "pc-user", "host": "desktop", "nickname": "Alice"},
  "text": "build finished",
  "from_agent": "builder",
  "to_agent": "reviewer",
  "reply_to": 11,
  "status": "received",
  "read": false,
  "broadcast": false,
  "audience": "agent",
  "hops": 1,
  "files": [
    {"file_id": 1, "name": "build.zip", "size": 18234, "mtime": 1794569000,
     "status": "available", "bytes": 0, "path": "", "error": ""}
  ]
}
```

`direction` は `in`（受信）、`out`（ピアへ送信）、`local`（同一ノードのエージェントへ配信）です。`status` は送信では `sending`、`delivered`、`read`、`failed`、受信では `received` です。`audience` は `human`、`agent`、空（未指定）のいずれかです。`hops` は LLM が生成した返信の連鎖数を表します。

エージェント関連フィールド、`audience`、`hops` は本文とファイル一覧の後ろにある Hancho 独自の隠し拡張で運ばれ、既存の IPMsg クライアントは無視します。拡張を持たない本文が `@name ` で始まる場合は、エージェント `name` 宛として扱います。

## CLI: `hancho-cli msg`（別名: `ipmsg`）

```powershell
hancho-cli.exe msg status
hancho-cli.exe msg on | off            # ipmsg.enabled を書き換えてノードを再起動
hancho-cli.exe msg peers
hancho-cli.exe msg refresh             # 参加通知を送り、応答を集める
hancho-cli.exe msg clear               # 履歴を削除（日別ログファイルは残る）
```

本文とファイルの送信:

```powershell
hancho-cli.exe msg send 192.168.1.20:2426 "build finished"
hancho-cli.exe msg send "*" "maintenance at 22:00" --from-agent ops
hancho-cli.exe msg send local "please review" --to-agent reviewer --audience agent
hancho-cli.exe msg send 192.168.1.20 "here is the log" --file C:\tmp\build.log
Get-Content C:\tmp\body.txt | hancho-cli.exe msg send 192.168.1.20 --stdin
```

`<to>` にはピアキー（`ip:port`）、IP、ニックネーム、全ピアを表す `*`、同一ノードのエージェントだけを表す `local`/`self` を指定できます。必要に応じて `--from-agent`、`--to-agent`、`--reply-to`、`--audience human|agent` を付けます。

受信と確認:

```powershell
hancho-cli.exe msg inbox --since 0 --limit 50
hancho-cli.exe msg inbox --agent reviewer --unread --mark-read
hancho-cli.exe msg get 12
hancho-cli.exe msg wait --agent reviewer --since 12 --timeout 120
hancho-cli.exe msg read 12 13
hancho-cli.exe msg read --all
hancho-cli.exe msg download 12 --wait
hancho-cli.exe msg download 12 --file-id 1 --dir C:\incoming
```

`msg wait` は新しい受信メッセージが来るまでロングポーリングします（既定 60 秒）。返された最後の id を次の `--since` に渡してください。

ログとハンドラ:

```powershell
hancho-cli.exe msg log --since 7d --query "build" --limit 50
hancho-cli.exe msg log --event handler_reply
hancho-cli.exe msg handler
hancho-cli.exe msg handler on --engine opencode --model gpt-5.1 --agent hancho-llm
hancho-cli.exe msg handler off
```

生の IPC 応答が必要な場合は `--json`（コマンドの前後どちらでも可）を付けます。

## MCP ツール

MCP サーバ (`Hancho.exe mcp`) は 10 個のツールを公開します。引数は camelCase (`sinceId`, `messageId`, ...) と snake_case の別名を受け付けます。

| ツール | 用途 |
| :--- | :--- |
| `ipmsg_status` | ノード状態、ポート、ニックネーム、ピア、未読数、最後の id、ハンドラ状態。 |
| `ipmsg_peers` | LAN 上で見えたホスト一覧。 |
| `ipmsg_refresh` | 参加通知をブロードキャスト。ピアは 1 秒ほどで応答。 |
| `ipmsg_send` | 本文/ファイル送信（`to`, `text?`, `fromAgent?`, `toAgent?`, `replyTo?`, `audience?`, `files?`, `hops?`）。 |
| `ipmsg_get` | 本文、エージェント情報、audience、ファイルを含む 1 件のメッセージ（`messageId`）。 |
| `ipmsg_inbox` | `sinceId` 以降の受信メッセージ（`agent?`, `includeUnaddressed?`, `unreadOnly?`, `includeOutgoing?`, `limit?`, `markRead?`）。 |
| `ipmsg_wait` | 新着メッセージをロングポーリング（`sinceId?`, `agent?`, `timeoutMs?`）。MCP の読み取りループ外で動作。 |
| `ipmsg_read` | 既読にして開封通知を送る（`ids?` または `all`）。 |
| `ipmsg_download` | ファイルをダウンロード（`messageId`, `fileId?`, `dir?`, `wait?`, `timeoutMs?`）。 |
| `ipmsg_log` | 会話ログを検索（`since?`, `until?`, `query?`, `peer?`, `agent?`, `event?`, `limit?`）。 |

## LLM 同士のメッセージング

エージェントは `from_agent` で名乗り、`to_agent` で相手を指定し、`reply_to` で返信先メッセージを指します。受信ループの例:

```powershell
$since = 0
while ($true) {
  $reply = hancho-cli.exe msg wait --agent reviewer --since $since --timeout 120 --json | ConvertFrom-Json
  if (-not $reply.ok) { continue }
  foreach ($m in $reply.messages) {
    $since = [Math]::Max($since, [int64]$m.id)
    hancho-cli.exe msg send $m.peer.key "ack" --from-agent reviewer --to-agent $m.from_agent `
      --reply-to $m.id --audience agent
  }
}
```

audience の扱い:

- `human`: 人間向けの本文。LLM ハンドラは処理しません。通知や、IPMsg ウィンドウで読むための回答に使います。
- `agent`: 別の LLM/Hancho 向け。ハンドラの条件に合えば処理されます。
- 未指定: 人間が書いたメッセージと同様に扱います（本文がエージェント宛でなければ、ハンドラは処理しません）。

`to_agent` のないメッセージは、`agent` の値にかかわらずどのハンドラでも処理されます。`agent: "reviewer"` のハンドラは `to_agent` が `reviewer` のメッセージも処理し、`agent: ""` のハンドラは特定エージェント宛のメッセージを除外します。隠し拡張を送れない既存クライアントでは `@name ` 接頭辞が使えます。

`hops` は LLM が返信するたびに増えます。`hops` が `handler.max_hops` 以上のメッセージはハンドラが無視するため、ボット同士の無限ループを止められます。

### ハンドラの流れ

1. 受信（またはローカル）メッセージがフィルタを通ります: direction が `in`/`local`、audience が `human` でない、`hops < max_hops`、ハンドラ自身のエージェントからのローカルメッセージでない、`to_agent` が `handler.agent` と整合する。
2. Hancho は `handler.taskqueue`（既定 `ipmsg`）にジョブを作ります。id は `ipmsg-<message-id>`（既に存在する場合は `-rN` を付加）、summary は `IPMsg from <peer>: <先頭行>`、試行回数 1、設定されたエンジン/モデル/制限時間を使います。プロンプトにはメッセージ JSON と、JSON オブジェクト 1 個だけを返す厳格な指示が入ります。
3. ワーカーの最終テキストを `{"audience":"human"|"agent","text":"...","to_agent":"..."}` または `{"audience":"none"}` として解析します。解析できないテキストは `human` とみなし、そのテキストを返信にします。
4. `auto_reply` が真で回答が `none` でなければ、元のピア（または `local`）へ返信します。`from_agent` は `handler.agent` または `hancho-llm`、`to_agent` は回答の `to_agent` または元の送信者、`reply_to` は元メッセージ id、`audience` は回答の値、`hops` は +1 です。
5. 各段階で `handler_*` ログイベントを書きます。

ワーカーは通常の Hancho ジョブなので、タスクキューに現れ、JSON ストリームも保持し、他のジョブと同じように調査できます。

## ログと履歴

- **履歴**: `<data-dir>/ipmsg/messages.json`。`max_messages` 件まで。`msg clear` で削除。
- **会話ログ**: `<data-dir>/ipmsg/log/ipmsg-YYYY-MM-DD.jsonl`。1 行 1 JSON、日ごとのファイル。履歴を消してもログファイルは残ります。
- **受信ファイル**: `download_dir` が空なら `<data-dir>/ipmsg/files`。

ログに書かれるイベント:

| イベント | 意味 |
| :--- | :--- |
| `recv` | メッセージを受信（メッセージ JSON 全体）。 |
| `send` | 送信メッセージを作成。 |
| `local` | ローカルのエージェントへ配信。 |
| `status` | 到達/既読通知で送信メッセージの状態が変化。 |
| `file_sent` / `file_received` | ファイル転送が完了（path、bytes、error）。 |
| `handler_skipped` | ハンドラのフィルタがメッセージを除外（reason: `audience-human`, `max-hops`, `agent-filter`, `self` など）。 |
| `handler_queued` | メッセージに対するワーカージョブをキューに追加。 |
| `handler_reply` | ワーカーの回答を返信。 |
| `handler_no_reply` | 返信しなかった（`audience-none`、空回答、`auto-reply-disabled`）。 |
| `handler_failed` | ジョブの構築/登録、または返信の送信に失敗。 |

`hancho-cli msg log`（または MCP `ipmsg_log`）で `--since`、`--until`、`--query`、`--peer`、`--agent`、`--event`、`--limit` を指定して検索できます。ダイアログの**ログ**タブにも同じフィルタと**フォルダを開く**ボタンがあります。

## 単体ダイアログ

```powershell
Hancho.exe ipmsg [--data-dir <path>] [--tray]
```

- IPMsg ダイアログだけを IPC クライアントとして起動します。メインダッシュボードのウィンドウは作りません。コアが起動していなければ、先にヘッドレスで起動します。
- データディレクトリごとに 1 インスタンス（名前付きミューテックス）。2 回目の起動は既存ウィンドウを前面に出します。
- 独自のトレイアイコンを持ちます。audience が `agent` でないメッセージが届くとバルーン通知を出し、タイトル/ツールチップに未読数を表示します。勝手にウィンドウを開くことはありません。
- 閉じる (`X`) とトレイに格納します。トレイメニューは**開く**と**終了**です。`--tray` を付けると非表示で開始します。
- タブは**チャット**（ホスト、メッセージ、本文、添付、送信・返信）、**ログ**（検索可能な会話ログ）、**設定**（有効化、ポート、ニックネーム、グループ、追加ホスト、ハンドラの on/off、エンジン、モデル、エージェント、自動返信）です。設定は `config_set` 経由で書き込むため、コアがノードとハンドラを再読み込みします。
- メインダッシュボードのツールバーには IPMsg ボタン（`💬`）があり、ダイアログを起動します。ダッシュボード起動時には `ipmsg.enabled` が真なら `Hancho.exe ipmsg --tray` を起動します。

## IPC アクション

ツール開発者向けに、同じ操作を Named Pipe でも提供します: `ipmsg_status`、`ipmsg_enable`、`ipmsg_peers`、`ipmsg_refresh`、`ipmsg_send`、`ipmsg_get`、`ipmsg_inbox`、`ipmsg_wait`、`ipmsg_read`、`ipmsg_download`、`ipmsg_log`、`ipmsg_clear`。フィールドは snake_case で、`ipmsg_wait` は最大 600000 ms ブロックします。`config_get` は既定値を補った `ipmsg` ブロックを返し、`config_set` に `ipmsg` パッチを渡すとヘルパースレッドでノードを再起動します。
