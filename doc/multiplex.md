# Multiplexing & Concurrency in Hancho

## Overview
Hancho supports multiple concurrent hancho-cli clients (CLI and MCP) against a
single dashboard (ProcessManager) instance. This document describes the IPC
multiplexing design and its limits.

## Architecture
- **Single dashboard instance per data dir**: `Hancho.exe` acquires a named
  mutex (`GetGuiMutexNameWide()`). A second launch just focuses the existing window.
- **hancho-cli CLI / MCP servers** are separate processes that connect to the dashboard
  via a named pipe: `\\.\pipe\Hancho_IPC_v1[_<tag>]`.
- Each `SendIpcRequest` opens its own pipe instance (message mode).
  `IpcServer::AcceptLoop` creates a new pipe instance per connection and spawns a
  detached `HandleClient` thread. So pipe I/O itself is not head-of-line blocked.

## Thread Safety
- `ProcessManager` and `TaskQueueManager` guard shared state with `std::mutex`. All
  public methods (`StartProcess`, `GetSnapshot`, `GetAgentStream`, task queue ops)
  acquire the lock internally.
- Concurrent requests from multiple hancho-cli (or multiple MCP servers) execute in
  parallel threads and are serialized only at mutex boundaries.

## Known Bottlenecks
1. **`StartProcess` holds `mutex_` during process creation** (CreateJobObject, ConPTY
   setup, hundreds of ms). Concurrent starts serialize; state queries block behind
   them.
2. **`HandleClient::ReadFile` has no timeout** — a slow/idle client can pin a thread
   indefinitely (`PIPE_UNLIMITED_INSTANCES` grows thread count).
3. **No request queue / worker pool** — thread-per-connection; under heavy load,
   thread creation overhead.

## Recommended Improvements
- Split `StartProcess`: register the process under the lock (short), then do blocking
  creation outside the lock.
- Add read timeout to `HandleClient` (e.g. 30s) and disconnect on timeout.
- Move long operations (wait, kill tree) off the IPC thread or make them async.
- Optionally replace thread-per-connection with a bounded worker pool + request queue.

## Typical Load
For Supervisor LLM usage (a few requests/sec), current design is sufficient.
High-parallelism (dozens of concurrent starts) needs the improvements above.
