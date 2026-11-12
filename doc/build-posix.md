# Building the POSIX headless core

Hancho supports Linux and macOS as a **headless core plus CLI**. In addition to the
terminal screen (`hancho-cli top`), the core embeds a modern **Browser UI dashboard**
(`hancho-cli web`, detailed specification in [browser-ui.md](browser-ui.md)) (`doc/design-v3-todo.md` §8).

## Requirements

- CMake 3.20 or newer
- A C++20 compiler (GCC 11+, Clang 14+, Apple Clang 14+)
- POSIX PTY support: `util` on Linux (`libutil`), built into libc on macOS
- pthreads
- *(Note: Frontend tools like Node.js / npm are not required. UI assets are compiled directly into the binary.)*

## Build

```sh
cmake -S . -B build-posix -DCMAKE_BUILD_TYPE=Release
cmake --build build-posix --parallel
```

The build produces:

| Binary | Purpose |
|---|---|
| `Hancho` | Headless core: task queue, behaviour trees, IPC (Unix domain socket), embedded WebUI server |
| `hancho-cli` | Client: queue/BT/process commands, `top`, and `web` / `ui` browser dashboard launcher |
| `posix_core_smoke` | Portability and core smoke test |
| `webui_smoke` | Browser UI HTTP asset delivery, token auth, and WebSocket IPC smoke test |

## Run

```sh
# Start headless core (WebUI HTTP+WebSocket server starts on localhost:9138)
./build-posix/Hancho --headless --data-dir ./ww-data

# Open Browser UI dashboard in default browser with authenticated session token
./build-posix/hancho-cli --data-dir ./ww-data web

# Open interactive terminal TUI dashboard (tabs, monitoring, why, job operations)
./build-posix/hancho-cli --data-dir ./ww-data top

# Non-interactive snapshot for scripting or piping
./build-posix/hancho-cli --data-dir ./ww-data top --once
```

`hancho-cli` starts the core automatically when it is not running. Each data
directory gets its own lock (`flock`) and socket, so several projects can run
concurrently. See [tui-design.md](tui-design.md) §5 for keyboard shortcuts and
the detailed POSIX verification checklist.

## Test

```sh
# Run POSIX core and TUI output formatting smoke tests
./build-posix/posix_core_smoke

# Run WebUI HTTP/WS smoke tests
./build-posix/webui_smoke
```

The tests cover shell quoting, the instance lock, the Unix-socket transport, a
real task queue run through `/bin/sh`, a behaviour tree job node, WebUI asset delivery and WebSocket IPC, and TUI 5-view rendering and key parsing.

## Platform boundaries

| Boundary | POSIX implementation |
|---|---|
| `IPty` | `forkpty` |
| `IProcessTree` | session/process group via `setsid`; cgroup v2 when writable |
| `IIpcTransport` | Unix domain socket |
| `IInstanceLock` | `flock` |
| `IShellQuote` | POSIX single-quote rules |

Windows keeps its existing ConPTY, Job Object, Named Pipe and mutex code paths.

## CI

```yaml
- name: Build
  run: |
    cmake -S . -B build-posix -DCMAKE_BUILD_TYPE=Release
    cmake --build build-posix --parallel
- name: Test
  run: |
    ./build-posix/posix_core_smoke
    ./build-posix/webui_smoke
```

## Shell rules

ChainGuard has a separate POSIX rule set (`src/chain_guard_posix.cpp`). It
rejects `&&`, `||`, unquoted `&`, `;`, newlines and polling loops, while a
single `|` remains allowed. Worker commands run through `/bin/sh -c`, and
`{summary}` / `{instructions}` are quoted with POSIX rules.
