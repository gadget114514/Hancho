# Terminal UI (hancho-cli top) Architecture and Design

**[English](tui-design.md) | [日本語](tui-design-ja.md)**

This document defines the architectural decisions, terminal loop, key handling, and rendering framework for **TUI (`hancho-cli top`)**, the interactive terminal dashboard for Linux, macOS, and Windows (`doc/design-v3-todo.md` §8, parent task `ww-data/tasks/062-linux-tui.md`, subtask `062-1-tui-design.md`).

---

## 1. Motivation and Positioning

In the Hancho v3 POSIX roadmap (`doc/design-v3-todo.md` §8), the approach for Linux / macOS interfaces was updated to provide **both a browser GUI and a terminal UI (TUI)**.

While the browser UI ([browser-ui.md](browser-ui.md)) serves desktop and remote browser workflows, `hancho-cli top` serves direct terminal workflows (SSH sessions, server consoles, minimal headless machines). Rather than remaining a naive polling display of processes, `hancho-cli top` is evolved into an interactive terminal dashboard equivalent in primary visibility to the Direct2D desktop dashboard:
- Process List
- Task Queue Overview
- Execution Monitor Stream
- Failure Diagnosis (`why`)
- Memory & Note Overview

```
+-------------------------------------------------------------------+
|               hancho-cli top (Terminal Dashboard)                 |
|  - Tabs: [1: Processes] [2: Queue] [3: Monitor] [4: Why] [5: Memory] |
|  - Key Nav: Tab, 1-5, Up/Down/j/k, r:Refresh, ?:Help, q:Quit      |
+---------------------------------+---------------------------------+
                                  |
              Unix Domain Socket (POSIX) / Named Pipe (Windows)
              IPC JSON Requests ({"action": "list"}, etc.)
                                  |
+---------------------------------v---------------------------------+
|               Hancho Core Process (Hancho --headless)             |
|  - TaskQueue, ProcessManager, Agent Stream, Memory                |
+-------------------------------------------------------------------+
```

---

## 2. Implementation Strategy and Decision

We evaluated three potential approaches for implementing the TUI:

### 2.1 Option Comparison

| Approach | Advantages | Drawbacks / Challenges | Fit for Hancho |
|---|---|---|---|
| **A. ncurses / curses** | - Historical POSIX standard<br>- Pre-built window and line primitives | - No native Windows support (requires PDCurses or Cygwin)<br>- Relies on dynamic host libraries (`libncurses-dev`, `libtinfo`) on Linux, conflicting with Hancho's zero-dependency static binary philosophy<br>- Complicates CMake build matrix | **Rejected** |
| **B. Third-party C++ TUI Frameworks**<br>*(FTXUI, notcurses, etc.)* | - Modern C++ declarative UI<br>- Component abstractions | - Substantial CMake & external dependency footprint<br>- Bloats binary size<br>- Cross-platform compatibility edge cases and maintenance burden | **Rejected** |
| **C. In-house ANSI/VT100 Engine + Platform Abstraction** | - **Zero external dependencies** (standard C++20 + standard OS APIs)<br>- Identical ANSI escape codes on modern Windows (Virtual Terminal Processing) and POSIX (termios / ioctl)<br>- Pure-function rendering decoupled from I/O makes **unit testing simple and robust**<br>- Instant startup and negligible binary size increase | **Selected** |

### 2.2 Decision: In-house ANSI/VT100 Escape Engine

**Decision**: Hancho adopts an **in-house lightweight ANSI/VT100 escape sequence engine and platform terminal abstraction (Option C)** without external curses or framework dependencies.

#### Key Rationales

1. **Zero Runtime Dependencies & Single Static Binary Philosophy**:
   Hancho builds as a self-contained single executable without runtime requirements on external shared libraries. Relying on host ncurses packages would prevent out-of-the-box execution on minimal Linux containers or fresh server images.
2. **Modern Terminal Capabilities**:
   Modern terminal emulators on Linux, macOS (Terminal.app, iTerm2), and Windows 10/11 (Windows Terminal and ConHost with `ENABLE_VIRTUAL_TERMINAL_PROCESSING`) reliably support ANSI VT100 / xterm sequences for cursor positioning, styling, and alternate screen buffers. A terminfo/termcap database is unnecessary.
3. **High Testability via Decoupled Rendering**:
   Separating the view renderer as a pure formatting function (`TopState -> std::string`) allows comprehensive unit and smoke tests in CI environments without requiring a live interactive terminal or PTY harness.
4. **Clean Incremental Evolution**:
   Enables clear progression from the initial skeleton (062-1), to rich multi-pane views (062-2), keyboard operations and remote jobs (062-3), and POSIX verification (062-4).

---

## 3. Architecture and Component Design

Implemented in `src/cli_top.h` (and integrated into `src/cli.cpp`):

```
+-------------------------------------------------------------------+
|                        Top Controller                             |
|  - RunTopLoop / RunTopCommand: Refresh timer and event loop       |
+-------------------+-----------------------------------------------+
                    |
      +-------------+-------------+
      |                           |
+-----v-------------+       +-----v---------------------------------+
|  TerminalSession  |       |              TopState                 |
|  - Raw / cbreak   |       |  - activeTab, selectedRow, showHelp   |
|  - Alt Screen     |       |  - processData, queueData, etc.       |
|  - PollKey(ms)    |       +-------------------+-------------------+
+-------------------+                           |
                                                |
                            +-------------------v-------------------+
                            |              Renderer                 |
                            |  - RenderHeader / RenderTabs          |
                            |  - RenderViewContent (5 Views)        |
                            |  - RenderHelpOverlay / RenderFooter   |
                            |  -> std::string (Pure Buffer)         |
                            +---------------------------------------+
```

### 3.1 Terminal Raw Mode and Alternate Screen (`TerminalSession`)

Managed via RAII:
- **Alternate Screen Buffer**:
  - Enter: `\x1b[?1049h` + `\x1b[?25l` (hide cursor)
  - Exit: `\x1b[?25h` (restore cursor) + `\x1b[?1049l` (restore shell buffer)
- **Raw / Cbreak Mode**:
  - **POSIX**: `tcgetattr`, unsets `ECHO | ICANON | ISIG | IEXTEN`, sets `VMIN=0, VTIME=0`.
  - **Windows**: `SetConsoleMode` with `ENABLE_VIRTUAL_TERMINAL_PROCESSING`, unsets `ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT`.
- **Non-interactive / Once Fallback**:
  - If `--once` is requested or stdout is piped (`!isatty`), raw mode and alternate buffer are omitted, and a single snapshot is written to stdout.

### 3.2 Key Input and Event Model (`TopKey` / `PollKey`)

- Non-blocking poll with configurable timeout (default 1000ms):
  - `q`, `Q`, `Ctrl+C`: Exit cleanly.
  - `r`, `R`: Force immediate refresh and redraw.
  - `Tab`, `Shift+Tab`: Cycle active view tab.
  - `1`, `2`, `3`, `4`, `5`: Switch to specific view tab.
  - `Up`, `Down`, `k`, `j`: Move row selection cursor.
  - `?`, `h`: Toggle help overlay.
  - `Escape`: Close help overlay.

### 3.3 Screen Layout and 5 Panes

Dynamic layout adapting to terminal columns and rows:
1. **Processes**: ID, PID, state, elapsed time, CPU time, last stdout line.
2. **Queue**: Task queue summary counts (running, waiting, pending, succeeded, failed) and job list.
3. **Monitor**: LLM agent stream, thinking log, and tool invocation status.
4. **Why**: Failure diagnosis, classification, root cause summary, and last action.
5. **Memory**: L1/L2/L3 memory notes and retrieval stats.

---

## 4. Subtask Roadmap

- **062-1**: Architecture & decision documentation, `src/cli_top.h` skeleton (update loop, key input, tab navigation), `--once` / `--interval` / `--json` compatibility, unit smoke test.
- **062-2**: Full display implementation of the 5 views (Processes, Queue, Monitor, Why, Memory).
- **062-3**: Interactive keyboard operations (kill, retry, cancel) and remote host jobs.
- **062-4 (Current)**: POSIX verification documentation and smoke test coverage.

---

## 5. POSIX Verification and Operational Procedure

Standard verification checklist for testing `hancho-cli top` interactive navigation, non-interactive batch mode, shell pipeline integration, and terminal restoration upon exit under Linux / macOS environments.

### 5.1 Prerequisites and Environment

- **OS**: Linux (Ubuntu 20.04+, Debian 11+, RHEL/Rocky 8+, Alpine 3.16+) or macOS (11.0 Big Sur+)
- **Terminal Emulator**: VT100 / xterm compatible terminal emulator supporting alternate screen buffers and ANSI escape sequences (GNOME Terminal, xterm, Kitty, Alacritty, iTerm2, macOS Terminal.app, tmux, screen, or remote SSH session).
- **Build**:
  ```sh
  cmake -B build-posix -DCMAKE_BUILD_TYPE=Release
  cmake --build build-posix -j
  ```

### 5.2 Interactive TUI Checklist

1. **Start Core**:
   ```sh
   # Start headless core in separate shell or background (hancho-cli starts it automatically if absent)
   ./build-posix/Hancho --headless --data-dir ./ww-data
   ```
2. **Launch Interactive Top**:
   ```sh
   ./build-posix/hancho-cli --data-dir ./ww-data top
   ```
   - Switches cleanly into alternate screen buffer and hides terminal cursor.
   - Header displays `Hancho Top`, timestamp, `Core: ONLINE`, and `Interval: 1000ms`.
   - Tab bar `[1: Processes]  [2: Queue]  [3: Monitor]  [4: Why]  [5: Memory]` displays with `1: Processes` inverted/highlighted.

3. **Verify Tab Cycling and View Panes**:
   - `Tab`: Navigates `1: Processes` -> `2: Queue` -> `3: Monitor` -> `4: Why` -> `5: Memory` -> `1: Processes`.
   - `Shift+Tab`: Cycles tabs backwards.
   - Keys `1` through `5`: Switches directly to each corresponding pane:
     - **[1: Processes]**: Process table (ID, PID, STATE, ELAPSED, CPU, LAST LINE) and selected process details (command line, CWD).
     - **[2: Queue]**: Queue breakdown (Running, Waiting, Pending, Succeeded, Failed), jobs list, attempt counts, and job details.
     - **[3: Monitor]**: LLM agent status (Type, Model, Goal, Notes) and live event stream (THOUGHT, TOOL_CALL, TOOL_RESULT).
     - **[4: Why]**: Failure diagnosis (class, confidence summary, last activity, suggested remediation, LLM classification).
     - **[5: Memory]**: Memory records (L1/L2/L3 badges, storage quota usage, quota warning banner).

4. **Verify Row Selection Cursor**:
   - `j` or `Down` arrow: Moves selection pointer (`>`) downwards; bottom detail pane updates accordingly.
   - `k` or `Up` arrow: Moves selection pointer upwards.

5. **Verify Keyboard Operations**:
   - `s` (Stop): Dispatches kill or cancel action to selected process or running job; status toast (e.g. `Stopped process #1.`) renders at the bottom.
   - `r` / `e` (Retry): Dispatches `taskqueue_retry` action for selected failed/finished job.
   - `c` / `x` (Cancel): Dispatches `taskqueue_cancel` action for selected pending/running job.
   - Space / `u` / `Ctrl+R`: Forces immediate refresh and redraw.
   - `?`: Toggles help overlay displaying full keybinding reference; `Escape` or `?` closes it.

6. **Verify Exit and Terminal Restoration**:
   - `q` or `Ctrl+C`: Cleanly exits TUI.
   - Restores primary screen buffer (`\x1b[?1049l`), restores cursor visibility (`\x1b[?25h`), and restores original termios settings (echo and canonical mode). Previous shell history remains intact.

### 5.3 Non-Interactive Batch and Pipe Integration

`hancho-cli top` safely supports non-interactive terminal contexts, cron jobs, and shell pipelines:

1. **Single Snapshot (`--once`)**:
   ```sh
   ./build-posix/hancho-cli --data-dir ./ww-data top --once
   ```
   - Emits a single screen rendering without entering raw mode or alternate screen buffers, then exits with code 0.
2. **Pipes and Redirections (`!isatty`)**:
   ```sh
   ./build-posix/hancho-cli --data-dir ./ww-data top --once | grep "Process List"
   ./build-posix/hancho-cli --data-dir ./ww-data top --once | head -n 20
   ```
   - When stdout is not a TTY, ANSI color sequences are omitted automatically for clean text processing.
3. **Structured JSON Output (`--json`)**:
   ```sh
   ./build-posix/hancho-cli --data-dir ./ww-data top --once --json | jq .
   ```
   - Outputs comprehensive JSON state suitable for external scripts and monitoring daemons.
4. **Configurable Refresh Interval (`--interval`)**:
   ```sh
   ./build-posix/hancho-cli --data-dir ./ww-data top --interval 500
   ```

### 5.4 Terminal Resizing and Recovery

- **Window Resizing (`SIGWINCH`)**:
  Resizing the terminal window dynamically recalculates geometry via `ioctl(STDOUT_FILENO, TIOCGWINSZ, ...)`, resizing table borders and line counts cleanly.
- **Terminal Recovery (`stty sane`)**:
  If the process is forcefully terminated (e.g. `SIGKILL`), terminal state can be restored by running `reset` or `stty sane`.

---

## 6. Smoke Test Architecture and Automated Verification

The entire TUI rendering engine, key parsing, remote host aggregation, and job operation request construction is decoupled from terminal I/O, allowing 100% automated headless unit testing.

### 6.1 Test Suites

1. **`tests/cli_top_smoke.cpp`**:
   Comprehensive unit smoke test covering:
   - `TestFormatHelpers`: Byte formatting (B, KB, MB, GB), padding/truncation, ISO timestamp formatting.
   - `TestKeyParsing`: ESC, arrow keys, Shift+Tab, numeric 1-5 keys, operations (s/r/c/q/space/?).
   - `TestExtendedKeyParsing`: Home, End, PageUp, PageDown, Enter, SS3 keypad arrows.
   - `TestStateTabNavigation`: Forward/backward cycling, wrap-around, direct tab indexing.
   - `TestRenderingProcessesView`: Process table, cursor pointer, details pane.
   - `TestRenderingRemoteProcessesView`: Remote host tags `[host]`, attribution details.
   - `TestRenderingQueueView`: Queue summary metrics, jobs table, job details.
   - `TestRenderingRemoteJobsInQueueView`: Remote job attribution, `queue @ host` column.
   - `TestRenderingMonitorView`: Agent type, goal, LLM stream events (THOUGHT, TOOL_CALL).
   - `TestRenderingDiagnosticsWhyView`: Failure classification, root cause summary, suggested action.
   - `TestRenderingRemoteJobsInDiagnosticsView`: Remote failure details.
   - `TestRenderingMemoryView`: Storage quota, health status, L1/L2/L3 badges, preview.
   - `TestRenderingHelpOverlay`: Keyboard shortcut overlay rendering.
   - `TestRenderingOfflineAndStatusMessage`: Offline core state, toast messages.
   - `TestRenderingEmptyLists`: Empty table placeholder and crash-resilience.
   - `TestColorAndNoColorRendering`: ANSI sequence inclusion vs omission.
   - `TestNarrowTerminalHandling`: Narrow/small terminal boundary layout.
   - `TestJobOperationRequests`: Stop, retry, cancel request JSON serialization.
   - `TestRemoteHelpers`: Deduplication and multi-host job aggregation.

2. **`tests/posix_core_smoke.cpp`**:
   Integrated into the standard POSIX core smoke test suite, running headless rendering and parsing verification without live terminal dependencies.

### 6.2 Verification Command

```sh
./build-posix/posix_core_smoke
```
