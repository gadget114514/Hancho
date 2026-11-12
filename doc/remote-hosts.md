# Remote hosts

Hancho can run part of a task queue on another machine and still show the job
in the local list. This is step C of `doc/design-v3.md` (2.5).

## Model

- The IPC request/response shape does not change. It is carried over TCP as a
  WebSocket text message. A single persistent WebSocket connection supports
  multiple concurrent requests multiplexed by request ID (`req_id`), as well
  as real-time streaming subscriptions for logs and events (`subscribe` / `unsubscribe`).
- The local dashboard listens on **loopback only** by default. A connection from
  any other address needs a token. TLS is not implemented; use an SSH tunnel
  when the traffic leaves the machine.
- A job with `"host"` runs through that host's `hanchod`/dashboard. Local and
  remote jobs share the task queue list; the queue column shows `main @ buildbox`.
- Logs, normalized events and the `.done` file are fetched over the protocol and
  mirrored under `<data-dir>/taskqueue/`:
  - `<job>.<attempt>.log`
  - `<job>.<attempt>.events.jsonl`
  - `<job>.<attempt>.done`
  - `<job>.<attempt>.tail.txt`

## Hosts file

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

- `address` is `host:port`, `[::1]:port` or `ws://host:port`. `wss://` is
  rejected: terminate TLS in the tunnel instead.
- `token_env` defaults to `HANCHO_TOKEN_<NAME>` (upper case, non-alphanumerics
  replaced with `_`).
- `token_credential` defaults to `Hancho/host/<name>` in the Windows Credential
  Manager.
- The token itself is never written to `hosts.json`.

Token lookup order:

1. `token_env` / `HANCHO_TOKEN_<NAME>`
2. Windows Credential Manager target (`token_credential`)
3. `HANCHO_TOKEN` (one shared token, useful for a single-host tunnel)

## Starting a host listener

The listener is opt-in:

```powershell
# loopback only; any local process may connect
$env:HANCHO_REMOTE_LISTEN = "127.0.0.1:9137"
Hancho.exe gui

# reachable from the network; every non-loopback peer must send the token
$env:HANCHO_REMOTE_LISTEN = "0.0.0.0:9137"
$env:HANCHO_REMOTE_TOKEN  = "<long random string>"
Hancho.exe gui
```

Recommended access over SSH:

```powershell
ssh -L 9137:127.0.0.1:9137 buildbox
hancho-cli hosts add buildbox 127.0.0.1:9137
```

If `HANCHO_REMOTE_LISTEN` is set to a non-loopback address without a token, the
listener still starts but rejects every external connection. When a token **is**
configured it is required from every peer, loopback included, because an SSH
tunnel also arrives as a loopback connection.

## Jobs

```json
{ "id": "linux-test", "name": "linux-test", "host": "buildbox",
  "summary": "Run the Linux test suite", "command": "make test", "timeoutSec": 1800 }
```

- The job is validated locally (chain guard, required fields) and started on the
  remote host with the remote dashboard's own log and done paths.
- Dependency ordering is unchanged. Only the launch is remote.
- `success.filesNewer` is evaluated on the remote host via the `check_files_newer`
  IPC action (or directly from attempt `.done` metadata). Both file modification
  times (`mtime`) and the reference process start time are evaluated in the remote
  host's own clock domain, so clock skew between machines and timezone differences
  do not cause false failures.
- Cancelling a remote job sends `kill` to that host.

## Resource locks

Resource conflicts are evaluated **per host**. The same path, or the same named
lock (`@build`), does not conflict when the jobs run on different hosts.

## CLI

```powershell
hancho-cli hosts list
hancho-cli hosts add buildbox 127.0.0.1:9137 --token-env HANCHO_TOKEN_BUILDBOX
hancho-cli hosts remove buildbox
hancho-cli hosts ping buildbox

# send any IPC request to a host instead of the local dashboard
hancho-cli --host buildbox list
hancho-cli --host buildbox taskqueue list main
```

IPC actions added for hosts:

- `hosts_list` (and `hosts_reload`)
- `hosts_add`, `hosts_remove`, `hosts_ping`
- `remote_taskqueues`, `remote_processes` — aggregate one host's view for a
  local tool; every entry carries `host`, and errors stay per host.
- `get_done` — return the `.done` file content for a process.
- `check_files_newer` — verify paths exist and are newer than process start on the host.

## Success condition: filesNewer on remote hosts

When a remote job specifies `success.filesNewer`:

- **Host-side evaluation**: Paths are evaluated directly on the machine running the job.
  Relative paths resolve against the job's `cwd` on the remote host.
- **Clock domain**: Both the reference start time and the artifact's modification time
  (`mtime`) are obtained from the remote host's operating system clock.
- **Clock skew tolerance**: Differences between the local system clock and the remote host's
  system clock have zero impact on evaluation accuracy because the timestamps are never
  compared across machine boundaries.
- **Timezone**: All timestamps and file modification times are handled as 64-bit UTC filetimes
  (100ns precision). Timezone differences between machines do not cause offsets or false failures.
- **Fallback**: If `check_files_newer` cannot find a live in-memory process start record on the
  host (e.g. after a host daemon restart), it falls back to the process start epoch recorded by
  the host's process manager or the attempt start timestamp sent in the request.

## Streaming & Subscriptions

- A single persistent WebSocket connection supports concurrent request multiplexing using
  request IDs (`req_id` / `request_id`).
- Real-time streaming subscriptions (`action: "subscribe"`) continuously push appended lines from
  remote `.events.jsonl`, `.stream.jsonl`, and `.tail.txt` files to the local dashboard.
- Subscriptions support sequence-based resumption (`seq`), enabling lossless reconnects and catch-up.
- The local dashboard's execution monitor pane automatically streams and displays remote job events and
  status in real time, identically to local jobs.

## Limitations

- There is no cluster scheduler: a host is chosen by the job, not by Hancho.
- When the local dashboard restarts while a remote attempt is running, Hancho
  reconnects to the remote process via the host registered in `hosts.json`. The
  remote attempt's status, full log, normalized events, and `.done` file are
  fetched and restored to the local task queue. If the host is unreachable or
  no longer listed in `hosts.json`, the attempt is classified as interrupted.

## Security notes

- Only loopback is trusted implicitly, and only when no token is configured.
  Any other peer must present the token, and a configured token is checked from
  every peer.
- Tokens stay in the credential store or the environment. `hosts list --json`
  reports `token_present`, never the token.
- Prefer an SSH tunnel over exposing the port. There is no built-in TLS.
- The remote listener uses the same IPC actions as the local pipe, so only give
  the token to machines that may control this Hancho instance.
