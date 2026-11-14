# WinConsoleServer Protocol (WCSP) v1

WCSP is the binary protocol Hancho uses to expose ConPTY terminal jobs over TCP. It was imported from WinConsoleServer and is implemented by the core's terminal server (`src/term_server.cpp`) and by the client in `src/wcsp/` (`RemoteSession`).

This document describes protocol version **1.2**. Version 1.1 adds the retained-session list (`LIST_SESSIONS` / `SESSION_LIST`) and persistent sessions. Version 1.2 adds `FeatureSizeSync`, the `RESIZED` reason `PEER`, and the rule that terminal sizes are exchanged in **cells** only. The reference implementation is `src/wcsp/protocol.h` / `protocol.cpp`.

## 1. Transport and endpoint

- **TCP only.** No UDP, no TLS. The wire format is binary and independent of the TCP segmentation: one TCP connection carries a sequence of frames.
- **Plain text is never assumed to be NUL terminated.** All integers are little-endian and all strings are UTF-8 with explicit byte lengths.
- **Authentication is a shared token** carried in `HELLO`. Hancho requires it for every connection, including loopback connections.
- **Nothing is encrypted.** Use the listener locally, or on a trusted LAN only.

The core's listener is configured in `<data-dir>/config.json`:

```json
"terminal": {
  "local": true,
  "remote": false,
  "host": "0.0.0.0",
  "port": 9139,
  "token": "",
  "history_bytes": 33554432
}
```

| Key | Default | Meaning |
| :--- | :--- | :--- |
| `local` | `true` | Accept local connections from this PC (terminal client / dashboard). Binds `127.0.0.1` on an ephemeral port. |
| `remote` | `false` | Bind `host:port` so another PC can connect. When `host` is `0.0.0.0`, loopback also works. |
| `host` | `0.0.0.0` | Interface used when `remote` is true. |
| `port` | `9139` | Port used when `remote` is true. |
| `token` | `""` | Shared secret. When empty, the core generates 16 random bytes and writes the 32 lowercase hex characters back to the file. |
| `history_bytes` | `32 MiB` | Raw ConPTY output kept per job and the maximum replay sent to a new subscriber. Accepted range is 64 KiB to 64 MiB. |

When both `local` and `remote` are false, the listener is not started. A local IPC `term_endpoint` request returns `{"ok":true,"host":"127.0.0.1","port":<ephemeral>,"token":"<token>"}` while the server is running; otherwise it returns an error. The standalone terminal client (`Hancho.exe terminal`) discovers the local endpoint this way and usually attaches with a target of `attach:<jobId>`.

Settings → **WinConsoleServer** toggles `local` and `remote`.

## 2. Framing

Every frame is an 8-byte header followed by `payload_len` bytes of payload:

| Offset | Size | Field | Notes |
| ---: | ---: | :--- | :--- |
| 0 | 4 | `payload_len` | `uint32` little-endian. |
| 4 | 1 | `type` | Frame type (section 4). |
| 5 | 1 | `flags` | Bit 0 = `PRIORITY` (input only); other bits reserved, send 0. |
| 6 | 2 | `stream_id` | Reserved for multiplexing. Current implementations always send `0` and ignore it. |

Rules:

- A frame with `payload_len > 1,048,576` bytes (1 MiB) is invalid. The reference `FrameReader` stops with a "frame too large" error and the connection is closed.
- Frames may be split arbitrarily across TCP reads and may be coalesced. Readers must parse incrementally.
- A sender must split a payload larger than 1 MiB across multiple `OUTPUT` frames. Output ordering is preserved by TCP.
- Unknown frame types are ignored by the current server and client; a receiver must not treat them as fatal.

Example: an `INPUT` frame carrying the four bytes `abcd`:

```text
00 00 00 04   10 00 00 00   61 62 63 64
└ length=4 ┘  │  │  └ stream_id=0
              │  └── flags=0
              └───── type=0x10 (INPUT)
```

## 3. TLV encoding

Control messages (`HELLO`, `HELLO_ACK`) are a sequence of TLVs:

| Size | Field |
| ---: | :--- |
| 2 | `type` (`uint16` little-endian) |
| 4 | `length` in bytes (`uint32` little-endian) |
| `length` | value bytes |

Strings are UTF-8 without a NUL terminator. `ENV` may appear more than once. Unknown TLV types are ignored. For duplicated scalar TLVs, the first occurrence wins.

| TLV | ID | Value |
| :--- | ---: | :--- |
| `MAGIC` | `0x0000` | Exactly 4 bytes: `WCSP`. Required in `HELLO`. |
| `VERSION` | `0x0001` | `uint16 major`, `uint16 minor`. |
| `NAME` | `0x0002` | UTF-8 client/server name (for display, not authentication). |
| `FEATURES` | `0x0003` | `uint32` feature bitmask. |
| `SIZE` | `0x0004` | `uint16 cols`, `uint16 rows`. |
| `SHELL` | `0x0005` | UTF-8 shell/target name. |
| `TOKEN` | `0x0006` | UTF-8 shared token. |
| `ENV` | `0x0007` | One UTF-8 environment entry; reserved, Hancho does not send or apply it. |
| `CLIENT_ID` | `0x0008` | UTF-8 client identifier; optional, not used by Hancho. |
| `SESSION_ID` | `0x0009` | `uint32`; server-assigned connection/session id in `HELLO_ACK`. |
| `ENCODING` | `0x000A` | UTF-8 encoding name; `HELLO_ACK` sends `UTF-8`. |

## 4. Frame types

| ID | Frame | Direction | Hancho status |
| ---: | :--- | :--- | :--- |
| `0x01` | `HELLO` | client → server | Implemented. |
| `0x02` | `HELLO_ACK` | server → client | Implemented. |
| `0x05` | `AUTH` | client → server | Reserved. Token is carried in `HELLO`. |
| `0x06` | `AUTH_RESULT` | server → client | Reserved. |
| `0x10` | `INPUT` | client → server | Implemented. Raw terminal input. |
| `0x11` | `RESIZE` | client → server | Implemented. |
| `0x12` | `RESIZED` | server → client | Implemented. Reports the applied (possibly clamped) size, or a peer client's size. |
| `0x13` | `CLOSE` | client → server | Implemented. Terminates the session: the underlying ConPTY job is closed. No payload. |
| `0x14` | `PING` | both | Implemented. Payload is a `uint64` nonce. |
| `0x15` | `PONG` | both | Implemented. Echoes the nonce. |
| `0x16` | `WINDOW_UPDATE` | both | Reserved (flow credit is not implemented). |
| `0x20` | `OUTPUT` | server → client | Implemented. Raw ConPTY output. |
| `0x22` | `EXIT` | server → client | Implemented. Sent before the server closes the connection. |
| `0x23` | `ERROR` | server → client | Implemented. Used during the handshake and on request errors. |
| `0x24` | `TITLE` | server → client | Reserved. The window title is carried as terminal escape sequences in `OUTPUT`. |
| `0x30` | `LIST_SESSIONS` | client → server | Implemented. Requests the retained-session list; empty payload. |
| `0x31` | `SESSION_LIST` | server → client | Implemented. Retained sessions (section 6.1). |
| `0x7F` | `NOOP` | both | Reserved. |

## 5. HELLO / HELLO_ACK

### HELLO

`HELLO` must be the first frame. Hancho closes the connection if it is not received within **5 seconds**.

| TLV | Required | Description |
| :--- | :--- | :--- |
| `MAGIC` | yes | `WCSP`. |
| `VERSION` | yes | Must be `1.x`; the server rejects a different major version. |
| `NAME` | no | Display name (for example `Hancho/0.9.15`). |
| `FEATURES` | no | Requested features. Missing means `0`. |
| `SIZE` | yes | Requested `cols` × `rows`. |
| `SHELL` | yes for Hancho | `cmd`, `powershell`, `bash`, or `attach:<id>`. |
| `TOKEN` | yes for Hancho | Shared secret; must be non-empty and match the server token. |
| `CLIENT_ID` | no | Ignored by Hancho. |
| `ENV` | no | Reserved; Hancho does not apply it. |

### HELLO_ACK

The server replies with `HELLO_ACK` after validating the request, resizing the target job to the requested size, and subscribing to its output.

| TLV | Description |
| :--- | :--- |
| `VERSION` | Server version (`1.2`). |
| `NAME` | Server name (Hancho sends `WinConsoleServer`). |
| `FEATURES` | Negotiated features: `hello.features & server_supported_features`. |
| `SIZE` | Accepted `cols` × `rows` after clamping. |
| `SESSION_ID` | Connection id, unique per server process starting at 1. |
| `SHELL` | Resolved display name (`cmd`, `powershell`, `bash`, or the attached job name). |
| `ENCODING` | Always `UTF-8`. |

If the client requested a different size and the server clamped it, the client should update its view to `SIZE`. Hancho's client also emits a local resized notification.

### Features

| Bit | Name | Hancho |
| ---: | :--- | :--- |
| `1 << 0` | `RESIZE` | Implemented. |
| `1 << 1` | `PRIORITY_INPUT` | Implemented (`INPUT` flag). |
| `1 << 2` | `FLOW_CREDIT` | Reserved. `WINDOW_UPDATE` is not used. |
| `1 << 3` | `COMPRESSION` | Reserved. Payloads are never compressed. |
| `1 << 4` | `TLS` | Reserved. The connection is plain TCP. |
| `1 << 5` | `IMAGE_PASSTHROUGH` | Advertised; images travel in `OUTPUT` as terminal escape data. |
| `1 << 6` | `CLIPBOARD_OSC52` | Advertised; OSC 52 sequences travel in `OUTPUT`. |
| `1 << 7` | `EXIT_CODE` | Implemented in `EXIT`. |
| `1 << 8` | `SESSION_LIST` | Implemented. `LIST_SESSIONS` / `SESSION_LIST`. |
| `1 << 9` | `SIZE_SYNC` | Implemented. Multi-client size negotiation (section 7). |

Hancho's default client feature mask is `0x3E3` (resize, priority input, image passthrough, clipboard OSC 52, exit code, session list and size sync). A server never turns on a bit the client did not request. `SIZE_SYNC` is active only when both `HELLO` and `HELLO_ACK` carry it.

### Handshake sequence

```text
client                                   server
  |------------- TCP connect ------------->|
  |------------- HELLO ------------------->|  (must arrive within 5 s)
  |<------------ HELLO_ACK ---------------|  or ERROR + close
  |<------------ OUTPUT (replay) ---------|  optional, last history_bytes
  |<------------ OUTPUT (live) -----------|
  |------------- INPUT ------------------>|
  |------------- RESIZE ----------------->|
  |<------------ RESIZED -----------------|
  |------------- PING ------------------->|
  |<------------ PONG --------------------|
  |------------- CLOSE ------------------>|
  |<------------ EXIT --------------------|
```

Control session (used to reconnect after a detach):

```text
client                                   server
  |------------- HELLO (SHELL=list) ----->|
  |<------------ HELLO_ACK ---------------|
  |------------- LIST_SESSIONS ---------->|
  |<------------ SESSION_LIST ------------|
  |------------- CLOSE ------------------>|
```

Handshake validation failures send `ERROR` and close the connection; transport failures just close it. The server validates in this order: frame type, TLV decode, major version, token presence, token equality, size clamping, shell/target.

## 6. Sessions, shells and attachment

- Without `attach:`, `SHELL` must be `cmd`, `powershell`, `bash`, or the control target `list` / `sessions` (case-insensitive). Anything else returns `ShellNotAllowed`.
- `list` / `sessions` is a control session: it creates no PTY and sends no terminal output. The client can send `LIST_SESSIONS` and receives one `SESSION_LIST`, then closes the connection.
- `cmd` starts `cmd.exe`, `powershell` starts `powershell.exe`, and `bash` uses Git Bash when installed, otherwise `bash.exe`. The working directory is the user profile and stdin is open.
- `attach:<id>` attaches to an existing Hancho process **only if** it is a headless ConPTY job. Non-ConPTY jobs, jobs with no headless PTY, and unknown ids return `ShellNotAllowed`.
- The server resizes the target job to the requested `SIZE` before taking the replay.
- A new subscriber first receives the tail of the raw output (`history_bytes`) as `OUTPUT`, then live output. The replay is raw bytes and may begin in the middle of an escape sequence; feed it to the terminal parser as-is.
- Several clients may attach to the same job. Each has its own `SESSION_ID` and receives its own replay. When clients negotiate `SIZE_SYNC`, a resize from one is forwarded to the others as `RESIZED` with reason `PEER`; otherwise they must not assume the PTY keeps their requested size.
- An **attached** job keeps running when the client detaches or closes the connection without `CLOSE`.
- A **shell started by this session** is now retained too. A lost connection, a client crash or a plain socket close only detaches; the shell keeps running and appears in the session list. Send `CLOSE` to terminate it explicitly.
- Any retained ConPTY job can be reattached with `attach:<id>`. The client lists the retained sessions with `LIST_SESSIONS` and lets the user choose one or more.
- If the attached job has already exited, the server still sends `HELLO_ACK`, the replay, and then `EXIT`.

### 6.1 SESSION_LIST encoding

`SESSION_LIST` is one frame with a little-endian binary payload:

```text
uint32 session_count
repeat session_count times:
    uint32 id
    uint8  state          (Hancho ProcState: 0=running, 1=completed,
                          2=failed, 3=timed out, 4=idle-timed out, 5=killed)
    uint16 name_len
    uint8  name[name_len]       (UTF-8)
    uint16 command_len
    uint8  command[command_len] (UTF-8)
```

The server sends at most 1024 entries, truncating names to 256 bytes and commands to 512 bytes so the frame stays below the 1 MiB limit. Unknown extra bytes after the last entry are ignored. `LIST_SESSIONS` itself has an empty payload.

## 7. Input, output and resize

### INPUT (`0x10`)

Payload is raw keyboard bytes. The server passes it to the ConPTY unchanged.

- `flags` bit 0 (`PRIORITY`) asks the server to use its front-of-queue input path. Hancho's client sets it for `WriteFront` (for example, a paste that must overtake buffered typing).
- An empty payload is ignored.

### OUTPUT (`0x20`)

Payload is raw ConPTY output: UTF-8 text, ANSI/VT sequences, and terminal replies such as OSC 52 or image protocols. The server does not line-buffer or rewrite it, and one frame may contain partial UTF-8 sequences.

### RESIZE (`0x11`) / RESIZED (`0x12`)

```text
RESIZE  payload: uint16 cols, uint16 rows
RESIZED payload: uint16 cols, uint16 rows, uint16 reason
```

- `cols` is clamped to `2..1000`, `rows` to `1..1000`.
- `reason` is `0` (`APPLIED`) when the requested size was accepted, `1` (`CLAMPED`) when the reply contains a different size, or `2` (`PEER`) when another client resized the shared PTY. Unknown reason values are ignored but still decoded.
- After a successful `RESIZE`, the server sends `RESIZED{PEER}` to every *other* connection attached to the same job that negotiated `SIZE_SYNC`. Attaching a new client resizes the job during the handshake and notifies those peers the same way.
- A malformed payload is ignored (the connection stays open).

Sizes are exchanged in **cells** only. DPI, font size, display scaling and window chrome are client-local concerns: a client must compute `cols`/`rows` from the physical pixels of its viewport and its own cell metrics, and must never send pixels or DIPs. Hancho's client creates Direct2D at a fixed 96 DPI and scales glyph sizes by the window DPI so one cell metric is valid in both units.

## 8. CLOSE, EXIT and ERROR

### CLOSE (`0x13`)

Empty payload. The client explicitly **terminates** the session. The server removes the subscription, closes the underlying ConPTY job (whether it was started by WCSP or attached with `attach:<id>`), sends `EXIT` with reason `CLIENT_CLOSE`, and the job disappears from the running-session list.

A plain TCP disconnect (no `CLOSE`) is a **detach**: the server keeps the ConPTY session alive for a later reconnect. This is why closing the terminal client window can leave sessions running; the client asks whether to terminate or detach, and "Detach" simply closes the socket.

### EXIT (`0x22`)

```text
payload: uint16 reason, uint16 reserved (0), uint32 exit_code
```

`exit_code` is the process exit code reinterpreted as a signed 32-bit value.

| Reason | Value | Meaning |
| :--- | ---: | :--- |
| `CHILD_EXITED` | `0` | The ConPTY child exited. |
| `SERVER_SHUTDOWN` | `1` | The terminal server is shutting down. |
| `CLIENT_CLOSE` | `2` | The client closed or disconnected. |
| `KILLED` | `3` | The session or job was killed. |

### ERROR (`0x23`)

```text
payload: uint16 code, uint16 reserved (0), UTF-8 message (rest of payload)
```

| Code | Name | Emitted by Hancho |
| ---: | :--- | :--- |
| `0x0001` | `INVALID_FRAME` | No (malformed frames close the connection). |
| `0x0002` | `FRAME_TOO_LARGE` | No (same). |
| `0x0010` | `INVALID_HELLO` | Yes: wrong first frame or bad `HELLO`. |
| `0x0101` | `VERSION_MISMATCH` | Yes. |
| `0x0102` | `AUTH_REQUIRED` | Yes: empty `TOKEN`. |
| `0x0103` | `AUTH_FAILED` | Yes: token mismatch. |
| `0x0104` | `SHELL_NOT_ALLOWED` | Yes: disallowed shell, unknown attach id, non-ConPTY target, or no headless PTY. |
| `0x0105` | `SESSION_LIMIT` | No (not enforced). |
| `0x0201` | `SHELL_START_FAILED` | Yes: ConPTY shell creation failed. |
| `0x0202` | `CONPTY_UNAVAILABLE` | No. |
| `0x0301` | `INTERNAL` | No. |

After `HELLO_ACK`, a malformed client frame is treated as a protocol error and closes the connection; unknown frame types are ignored.

## 9. Flow control, backpressure and timeouts

WCSP v1 has no credit-based flow control. Senders must not let one connection grow without bound.

Hancho's server:

- Reads a client frame every 200 ms (also to notice a finished job).
- Sets `SO_RCVTIMEO` on the client socket and a 5-second `SO_SNDTIMEO`.
- Keeps at most **8 MiB** of queued `OUTPUT` per connection. A large replay temporarily raises that limit by the replay size so the replay is not mistaken for a slow client; the normal 8 MiB limit returns once the queue drains. If a client still cannot keep up, the session is disconnected (detached, not killed) to avoid blocking the ConPTY reader thread. No `EXIT` frame is guaranteed in this case.
- Sends `PONG` at the front of the queue.
- Does not send periodic `PING`; it only replies.
- Replays at most `history_bytes` per new attachment (section 6).

Hancho's client:

- Sends `PING` with the current millisecond timestamp every **15 seconds** when idle.
- Keeps priority `INPUT` frames ahead of normal input.
- Sends `INPUT`, `RESIZE` and `CLOSE` without flow-control credits.

`TCP_NODELAY` and `SO_KEEPALIVE` are enabled on accepted server sockets.

## 10. Security

- The server is **plain TCP** with a **shared bearer token**. The token is sent in clear text in `HELLO`; anyone who can observe the connection can read it.
- Use `remote` only on a trusted LAN. Disable `local` and `remote` to stop the listener entirely.
- The token is the only authorization. With a valid token, a client can attach to any headless ConPTY job by numeric id.
- New shells are restricted to `cmd`, `powershell`, and `bash`; there is no arbitrary command execution through `SHELL`.
- Incoming bytes are passed to the ConPTY unchanged. Treat a remote client as having the same terminal access as a local user.

## 11. Hancho implementation notes

| Concern | Location |
| :--- | :--- |
| Wire protocol, frame/TLV codecs | `src/wcsp/protocol.h`, `src/wcsp/protocol.cpp` |
| Incremental frame reader | `src/wcsp/frame_reader.h`, `src/wcsp/frame_reader.cpp` |
| Client backend (`ITerminalIo`) | `src/wcsp/remote_session.h`, `src/wcsp/remote_session.cpp` |
| Console client (`wct.exe`) | `src/wct_main.cpp`, `src/wct_client.cpp` |
| Server and job attachment | `src/term_server.h`, `src/term_server.cpp` |

The server implementation is Windows-only. It does not spawn a command shell for `attach:<id>`; it subscribes to the existing headless ConPTY job. The standalone terminal client (`Hancho.exe terminal`, `src/term_client.cpp`) uses `RemoteSession`; the dashboard opens that client window instead of embedding terminal tabs. `wct.exe` is a console client for the same protocol: it runs in the current console, forwards raw input/output, detaches with a single `Ctrl+]`, and sends `CLOSE` when `Ctrl+]` is pressed twice in quick succession.

The server splits an attachment replay into `OUTPUT` frames of at most **256 KiB**, so `history_bytes` (default 32 MiB) can be replayed without exceeding the 1 MiB frame limit. The server retains sessions after a disconnect; only a `CLOSE` frame ends them. The terminal client asks **Terminate / Detach / Cancel** when closing a tab or the window.

## 12. Minimal client checklist

1. Connect over TCP and send `HELLO` with `MAGIC`, `VERSION` `1.0`, `SIZE`, `SHELL`, and `TOKEN`.
2. Wait for `HELLO_ACK`; on `ERROR`, surface `code` and `message` and close. Use `VERSION` `1.2`, `FEATURES` bit 8 for session-list support and bit 9 for multi-client size sync.
3. Parse frames incrementally and enforce the 1 MiB payload limit.
4. Feed `OUTPUT` bytes to a VT parser without assuming UTF-8 boundaries or line boundaries.
5. Send `INPUT` for keystrokes, `RESIZE` when the viewport changes, and `PING` periodically.
6. Handle `RESIZED` (`APPLIED` / `CLAMPED` / `PEER`), `PONG`, `EXIT`, and `ERROR`; ignore unknown frame types. With `SIZE_SYNC`, re-assert your own size on the next focus or keystroke if a `PEER` resize wins.
7. To reconnect, open a control session (`SHELL=list`), send `LIST_SESSIONS`, read `SESSION_LIST`, and attach a chosen id with `attach:<id>`.
8. Close the socket to detach and keep the session. Send `CLOSE` only to terminate it (the underlying ConPTY job is closed).
