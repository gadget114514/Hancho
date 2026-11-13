# Chat (IPMsg compatible LAN messaging)

**[English](chat.md) | [日本語 (Japanese)](chat-ja.md)**

Hancho can speak the classic IP Messenger (IPMsg) protocol, so a stock IPMsg client on the same LAN can see Hancho as a host and exchange messages and files with it. The same channel is used for LLM-to-LLM messaging: a received message can be handed to a worker LLM, and its answer is sent back as a normal IPMsg reply.

Chat is **off by default** and is meant for a **trusted LAN only**. Nothing is encrypted or authenticated.

## What works

| IPMsg command | Support |
| :--- | :--- |
| `BR_ENTRY` / `ANSENTRY` / `BR_ABSENCE` / `BR_EXIT` | Yes. Hancho announces itself, answers presence entries, tracks hosts and removes them on exit. |
| `SENDMSG` | Yes (text, broadcast, multicast, read-check, file attachments). |
| `RECVMSG` / `READMSG` / `ANSREADMSG` | Yes. Delivery and read receipts update the outgoing message status. |
| `GETINFO` / `SENDINFO` | Yes. Hancho answers with `Hancho <version> (IPMsg compatible)`. |
| `GETABSENCEINFO` / `SENDABSENCEINFO` | Yes (absense mode itself is not used). |
| `GETFILEDATA` / `RELEASEFILES` | Yes, for **regular files**. Directories/folders are marked unsupported and are not transferred. |
| `DELMSG` | Ignored. |
| Secret mode | Parsed but not protected: messages are not encrypted. |

Text encoding:

- With `IPMSG_UTF8OPT` the text is UTF-8; without it Hancho decodes CP932 (Shift_JIS). Hancho advertises `IPMSG_CAPUTF8OPT` and sends UTF-8 to peers that advertise it too, otherwise it encodes as CP932. This keeps stock Japanese IPMsg clients working.
- CRLF on the wire is normalized to LF in the stored message.

The default port is **2426**, not IPMsg's well-known 2425 (`kIpmsgDefaultPort`). This avoids a collision when a desktop IPMsg is already running on the same PC. To talk to Hancho, either add `192.168.x.y:2426` as a host entry in the other client or change its IPMsg port to 2426. Hancho broadcasts presence to `255.255.255.255` and to every entry in `hosts`.

## Security

- **Trusted LAN only.** Packets are accepted only from loopback, private (RFC 1918), link-local and CGNAT IPv4 addresses. Public addresses are never accepted. `allow_from` can narrow this further (`"192.168.1.0/24"`, `"10.0.0.5"`).
- **No encryption, no authentication.** Anyone on the allowed network can send a message or a file that looks like it came from another host. Do not use this over the Internet or on an untrusted Wi-Fi network.
- **Received text is untrusted input.** The LLM handler prompt wraps the message in an explicit "untrusted data" block and tells the worker not to follow instructions inside it. Prompt injection is still possible; keep `handler.instructions` narrow and review what the worker can do in its `cwd`.
- **`skip_permissions` is dangerous.** It passes the engine's permission-bypass flags to a worker that was triggered by network input. Leave it `false` unless every host on the LAN is fully trusted.

## Configuration (`config.json`)

The block lives in `<data-dir>/config.json` (`%LOCALAPPDATA%\Programs\Hancho\logs\config.json` by default). `config_get` fills in these defaults when the block is missing or partial.

```json
"chat": {
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
    "taskqueue": "chat",
    "auto_reply": true,
    "skip_permissions": false,
    "timeout_sec": 600,
    "max_hops": 4,
    "concurrency": 1
  }
}
```

| Key | Default | Meaning |
| :--- | :--- | :--- |
| `enabled` | `false` | Start the UDP/TCP node. Changing it restarts the node. |
| `port` | `2426` | UDP port for presence/messages and TCP port for file transfer. |
| `bind` | `"0.0.0.0"` | Local bind address. |
| `nickname` | `""` | Shown to other clients. Empty = `Hancho@<host>`. |
| `group` | `"Hancho"` | IPMsg group name. |
| `hosts` | `[]` | Extra broadcast/unicast targets (`"192.168.1.255"`, `"10.0.0.5:2426"`). |
| `allow_from` | `[]` | Accepted source networks/hosts. Empty = every private/loopback/link-local address. |
| `download_dir` | `""` | Where received files go. Empty = `<data-dir>/chat/files`. |
| `auto_download` | `false` | Download files automatically as messages arrive. |
| `max_file_mb` | `1024` | Size limit per file for downloads. |
| `max_messages` | `2000` | Messages kept in the history (10-100000). |
| `handler.enabled` | `false` | Pass matching incoming messages to a worker LLM. |
| `handler.engine` | `"agy"` | Worker engine name (see `hancho-cli engines`). |
| `handler.model` | `""` | Model override; empty = engine default. |
| `handler.agent` | `""` | Also handle messages addressed to this agent. Empty = only unaddressed messages; when set, messages addressed to another agent are skipped. |
| `handler.cwd` | `""` | Worker working directory; empty = the data directory. |
| `handler.instructions` | `""` | Extra instructions prepended to the handler prompt. |
| `handler.taskqueue` | `"chat"` | Task queue that receives the handler jobs. |
| `handler.auto_reply` | `true` | Send the worker's answer back automatically. |
| `handler.skip_permissions` | `false` | Pass the engine's permission-bypass arguments (see Security). |
| `handler.timeout_sec` | `600` | Per-message worker timeout. |
| `handler.max_hops` | `4` | Stop replying when a reply chain reaches this many hops. |
| `handler.concurrency` | `1` | Queue concurrency for handler jobs. |

Changes made with `hancho-cli chat handler ...`, the dialog's Settings tab or `config_set` are written to `config.json`; the node and the handler reload without restarting the core.

## Message JSON

IPC, CLI, MCP and the history file use the same shape:

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

`direction` is `in` (received), `out` (sent to a peer) or `local` (delivered to agents on this node). `status` is `sending`, `delivered`, `read` or `failed` for outgoing messages and `received` for incoming ones. `audience` is `human`, `agent` or empty (unspecified). `hops` counts LLM-generated replies in a chain.

The agent fields, `audience` and `hops` travel in a hidden Hancho extension after the text and file list; stock IPMsg clients ignore it. A plain message whose text starts with `@name ` is addressed to agent `name`.

## CLI: `hancho-cli chat` (alias: `msg`)

```powershell
hancho-cli.exe chat status
hancho-cli.exe chat on | off            # writes chat.enabled and restarts the node
hancho-cli.exe chat peers
hancho-cli.exe chat refresh             # broadcast presence and collect answers
hancho-cli.exe chat clear               # remove history; daily log files stay
```

Send text and/or files:

```powershell
hancho-cli.exe chat send 192.168.1.20:2426 "build finished"
hancho-cli.exe chat send "*" "maintenance at 22:00" --from-agent ops
hancho-cli.exe chat send local "please review" --to-agent reviewer --audience agent
hancho-cli.exe chat send 192.168.1.20 "here is the log" --file C:\tmp\build.log
Get-Content C:\tmp\body.txt | hancho-cli.exe chat send 192.168.1.20 --stdin
```

`<to>` accepts a peer key (`ip:port`), an IP, a nickname, `*` for every peer, or `local`/`self` for agents on this node only. Add `--from-agent`, `--to-agent`, `--reply-to` and `--audience human|agent` as needed.

Receive and inspect:

```powershell
hancho-cli.exe chat inbox --since 0 --limit 50
hancho-cli.exe chat inbox --agent reviewer --unread --mark-read
hancho-cli.exe chat get 12
hancho-cli.exe chat wait --agent reviewer --since 12 --timeout 120
hancho-cli.exe chat read 12 13
hancho-cli.exe chat read --all
hancho-cli.exe chat download 12 --wait
hancho-cli.exe chat download 12 --file-id 1 --dir C:\incoming
```

`chat wait` long-polls until a new incoming message arrives (default 60 s); pass the returned last id as the next `--since`.

Log and handler:

```powershell
hancho-cli.exe chat log --since 7d --query "build" --limit 50
hancho-cli.exe chat log --event handler_reply
hancho-cli.exe chat handler
hancho-cli.exe chat handler on --engine opencode --model gpt-5.1 --agent hancho-llm
hancho-cli.exe chat handler off
```

Add `--json` (before or after the command) for the raw IPC reply.

## MCP tools

The MCP server (`Hancho.exe mcp`) exposes ten tools. They accept camelCase (`sinceId`, `messageId`, ...) and snake_case aliases.

| Tool | Purpose |
| :--- | :--- |
| `chat_status` | Node state, port, nickname, peers, unread count, last id and the handler state. |
| `chat_peers` | Hosts seen on the LAN. |
| `chat_refresh` | Broadcast a presence entry; peers answer within a second. |
| `chat_send` | Send text/files (`to`, `text?`, `fromAgent?`, `toAgent?`, `replyTo?`, `audience?`, `files?`, `hops?`). |
| `chat_get` | One message with text, agent fields, audience and files (`messageId`). |
| `chat_inbox` | Received messages after `sinceId` (`agent?`, `includeUnaddressed?`, `unreadOnly?`, `includeOutgoing?`, `limit?`, `markRead?`). |
| `chat_wait` | Long-poll for a new message (`sinceId?`, `agent?`, `timeoutMs?`). Runs off the MCP read loop. |
| `chat_read` | Mark messages read and send read receipts (`ids?` or `all`). |
| `chat_download` | Download files (`messageId`, `fileId?`, `dir?`, `wait?`, `timeoutMs?`). |
| `chat_log` | Search the conversation log (`since?`, `until?`, `query?`, `peer?`, `agent?`, `event?`, `limit?`). |
| `chat_handler` | Show or change the answering LLM (`enabled?`, `engine?`, `model?`, `agent?`, `instructions?`, `taskqueue?`, `autoReply?`, `skipPermissions?`, `timeoutSec?`, `maxHops?`, `concurrency?`). Only supplied fields change; `chat_status` shows the result. |

## LLM to LLM messaging

Agents identify themselves with `from_agent`, address another agent with `to_agent`, and point at the message they answer with `reply_to`. A simple receive loop:

```powershell
$since = 0
while ($true) {
  $reply = hancho-cli.exe chat wait --agent reviewer --since $since --timeout 120 --json | ConvertFrom-Json
  if (-not $reply.ok) { continue }
  foreach ($m in $reply.messages) {
    $since = [Math]::Max($since, [int64]$m.id)
    hancho-cli.exe chat send $m.peer.key "ack" --from-agent reviewer --to-agent $m.from_agent `
      --reply-to $m.id --audience agent
  }
}
```

Audience rules:

- `human`: the text is for a person. The LLM handler never processes it. Use it for notifications and answers meant to be read in a Chat window.
- `agent`: the text is for another LLM/Hancho. Covered by the handler when its filter matches.
- Unspecified: treated like a human-written message (the handler skips it only if the text is not addressed to its agent).

A message with no `to_agent` is handled by any handler, whatever its `agent` value. A handler with `agent: "reviewer"` additionally handles messages whose `to_agent` is `reviewer`, while a handler with `agent: ""` skips messages addressed to a specific agent. The `@name ` prefix works with stock clients that cannot send the hidden extension.

`hops` is incremented on every LLM-generated reply. A message whose `hops` is already `>= handler.max_hops` is ignored by the handler, which stops runaway bot-to-bot loops.

### Handler flow

1. An incoming (or local) message passes the filter: direction `in`/`local`, audience not `human`, `hops < max_hops`, not a local message from the handler's own agent, and `to_agent` compatible with `handler.agent`.
2. Hancho creates a task queue job in `handler.taskqueue` (default `chat`) with id `chat-<message-id>` (a `-rN` suffix is added if that id already exists), summary `Chat from <peer>: <first line>`, one attempt and the configured engine/model/timeout. The prompt contains the message JSON and strict instructions to return one JSON object.
3. The worker's final text is parsed as `{"audience":"human"|"agent","text":"...","to_agent":"..."}` or `{"audience":"none"}`. Unparseable text is treated as `human` with the text as the reply.
4. If `auto_reply` is true and the answer is not `none`, Hancho sends the reply to the original peer (or `local`), with `from_agent` = `handler.agent` or `hancho-llm`, `to_agent` = the answer's `to_agent` or the original sender, `reply_to` = the original message id, `audience` from the answer, and `hops` + 1.
5. Every step writes a `handler_*` log event.

Workers are normal Hancho jobs, so they appear in the task queue, keep their JSON stream, and can be inspected like any other job.

## Logs and history

- **History**: `<data-dir>/chat/messages.json`, capped at `max_messages`. `chat clear` removes it.
- **Conversation log**: `<data-dir>/chat/log/chat-YYYY-MM-DD.jsonl`, one JSON object per line, one file per day. Log files are kept when the history is cleared.
- **Received files**: `<data-dir>/chat/files` unless `download_dir` is set.

Events written to the log:

| Event | Meaning |
| :--- | :--- |
| `recv` | A message was received (full message JSON). |
| `send` | An outgoing message was created. |
| `local` | A message was delivered locally to agents. |
| `status` | Delivery/read receipt changed an outgoing message. |
| `file_sent` / `file_received` | A file transfer finished (path, bytes, error). |
| `handler_skipped` | The handler filter rejected a message (reason: `audience-human`, `max-hops`, `agent-filter`, `self`, ...). |
| `handler_queued` | A worker job was queued for a message. |
| `handler_reply` | The worker's answer was sent back. |
| `handler_no_reply` | No reply was sent (`audience-none`, empty answer or `auto-reply-disabled`). |
| `handler_failed` | The job could not be built/queued or the reply could not be sent. |

Use `hancho-cli chat log` (or MCP `chat_log`) with `--since`, `--until`, `--query`, `--peer`, `--agent`, `--event` and `--limit`. The dialog's **Log** tab provides the same filters and an **Open folder** button.

## Standalone dialog

```powershell
Hancho.exe chat [--data-dir <path>] [--tray]
```

- Runs only the Chat dialog as an IPC client; it does not create the main dashboard window. If the core is not running, it starts it headlessly first.
- One dialog per data directory (named mutex). A second launch brings the existing window to the front.
- Own tray icon. When a message arrives whose audience is not `agent`, the dialog shows a balloon notification and an unread count in the title/tooltip. It never opens by itself.
- Close (`X`) hides to the tray. Tray menu: **Open**, **Exit**. `--tray` starts hidden.
- Tabs: **Chat** (hosts, messages, message body, attachments, compose and reply, plus an **Answering LLM** bar to change the handler engine/model and enable/disable it), **Log** (searchable conversation log) and **Settings** (enable, port, nickname, group, extra hosts, handler on/off, engine, model, agent, auto reply). Settings are written through `config_set`, so the core reloads the node and handler.
- The window follows the dashboard theme (`theme` in `config.json`): window, lists, headers, edits, buttons and tabs are painted from the same palette, and a theme change is picked up while the window is open.
- The main dashboard toolbar has a Chat button (`💬`) that launches the dialog, and at dashboard start it launches `Hancho.exe chat --tray` when `chat.enabled` is true.

## IPC actions

For tool developers, the same operations are available over the Named Pipe: `chat_status`, `chat_enable`, `chat_peers`, `chat_refresh`, `chat_send`, `chat_get`, `chat_inbox`, `chat_wait`, `chat_read`, `chat_download`, `chat_log`, `chat_handler` and `chat_clear`. Fields are snake_case; `chat_wait` blocks for up to 600000 ms. `config_get` returns the `chat` block with defaults, `config_set` with a `chat` patch restarts the node on a helper thread, and `chat_handler` patches only the answering-LLM fields.
