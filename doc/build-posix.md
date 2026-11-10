# Building the POSIX headless core

Hancho supports Linux and macOS as a **headless core plus CLI**. There is no GUI
dashboard on these platforms; the only screen is `hancho-cli top`
(`doc/design-v3.md` 2.4).

## Requirements

- CMake 3.20 or newer
- A C++20 compiler (GCC 11+, Clang 14+, Apple Clang 14+)
- POSIX PTY support: `util` on Linux (`libutil`), built into libc on macOS
- pthreads

## Build

```sh
cmake -S . -B build-posix -DCMAKE_BUILD_TYPE=Release
cmake --build build-posix --parallel
```

The build produces:

| Binary | Purpose |
|---|---|
| `Hancho` | Headless core: task queue, behaviour trees, IPC (Unix domain socket) |
| `hancho-cli` | Client: queue/BT/process commands and `top` |
| `posix_core_smoke` | Portability and core smoke test |

## Run

```sh
./build-posix/Hancho --headless --data-dir ./ww-data
./build-posix/hancho-cli --data-dir ./ww-data top
```

`hancho-cli` starts the core automatically when it is not running. Each data
directory gets its own lock (`flock`) and socket, so several projects can run
concurrently.

## Test

```sh
./build-posix/posix_core_smoke
```

The test covers shell quoting, the instance lock, the Unix-socket transport, a
real task queue run through `/bin/sh`, and one behaviour tree job node.

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
  run: ./build-posix/posix_core_smoke
```

## Shell rules

ChainGuard has a separate POSIX rule set (`src/chain_guard_posix.cpp`). It
rejects `&&`, `||`, unquoted `&`, `;`, newlines and polling loops, while a
single `|` remains allowed. Worker commands run through `/bin/sh -c`, and
`{summary}` / `{instructions}` are quoted with POSIX rules.
