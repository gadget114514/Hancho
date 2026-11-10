# Remote hosts

Hancho can run part of a task queue on another machine and still show the job
in the local list. This is step C of `doc/design-v3.md` (2.5).

## Model

- The IPC request/response shape does not change. It is carried over TCP as a
  WebSocket text message: one JSON request, one JSON response, one connection.
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
- `success.filesNewer` is skipped for remote jobs because those paths exist on
  the other machine; use `success.outputRegex` or check the exit code.
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

## Limitations

- There is no cluster scheduler: a host is chosen by the job, not by Hancho.
- If the local dashboard restarts while a remote attempt is running, the local
  attempt is classified as interrupted. The remote process is not reattached;
  kill it on the host if needed.
- `success.filesNewer` is not evaluated for remote jobs (see above).
- Only a single request/response per WebSocket connection is used. Streaming a
  live log over one long-lived socket is left for a later step.

## Security notes

- Only loopback is trusted implicitly, and only when no token is configured.
  Any other peer must present the token, and a configured token is checked from
  every peer.
- Tokens stay in the credential store or the environment. `hosts list --json`
  reports `token_present`, never the token.
- Prefer an SSH tunnel over exposing the port. There is no built-in TLS.
- The remote listener uses the same IPC actions as the local pipe, so only give
  the token to machines that may control this Hancho instance.
