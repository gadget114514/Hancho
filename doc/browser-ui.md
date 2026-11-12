# Browser UI Approach and Architecture

**[English](browser-ui.md) | [日本語](browser-ui-ja.md)**

This document defines the architecture, server placement, localhost authentication, asset bundling and delivery, and native GUI suspension/resumption criteria for the **Browser UI** dashboard for Linux / macOS (`doc/design-v3-todo.md` §8, `doc/design-v3.md` §7, task `ww-data/tasks/061-linux-gui.md`).

---

## 1. Server Architecture and Placement

### 1.1 Embedded in Core (`Hancho --headless`)

The backend server for the browser UI on Linux and macOS will **not** be a separate command (e.g. `hancho-cli serve`). Instead, it is **directly embedded into the core process (`Hancho --headless` / POSIX daemon) as a lightweight HTTP + WebSocket server**.

```
+-------------------------------------------------------------------+
|                        Web Browser                                |
|  - SPA (HTML5 / Vanilla JS / Pure CSS)                            |
|  - Localhost Dashboard (Process List / TaskQueue / Monitor etc.)   |
+---------------------------------+---------------------------------+
                                  |
            HTTP GET (Static Assets) / WebSocket (IPC Messages & Subscriptions)
                                  |
+---------------------------------v---------------------------------+
|               Hancho Core Process (Hancho --headless)             |
|                                                                   |
|  +-------------------------------------------------------------+  |
|  | Localhost HTTP & WebSocket Server (Port 9138)               |  |
|  | - Token Auth / Host & Origin Verification                   |  |
|  | - Static File Delivery (Embedded in binary or --webui-dir)  |  |
|  | - IPC WebSocket Handler (Multiplex & Subscriptions)         |  |
|  +------------------------------+------------------------------+  |
|                                 |                                 |
|  +------------------------------v------------------------------+  |
|  | Hancho Engine (TaskQueue / ProcessManager / BT / Memory)    |  |
|  +-------------------------------------------------------------+  |
+-------------------------------------------------------------------+
```

### 1.2 Why Not a Separate Command (`hancho-cli serve`)

1. **Unified Process Lifecycle**:
   - The dashboard is available as long as the core is alive and automatically shuts down when the core terminates. No zombie processes or leaked port listeners.
2. **Single Binary Maintenance**:
   - Users run only a single core process. There is no need to monitor or supervise two daemon processes.
3. **Zero Proxy Overhead and No Dual Failure Points**:
   - A separate serve process creates a dual IPC proxy (`Browser <-> serve <-> core (Named Pipe / UDS)`), adding latency, failure points, and redundant JSON serialization.
   - Embedding directly allows the server to reuse the core's existing WebSocket multiplexing and streaming subscription facilities (`058-1`, `058-2`, `RemoteListener`).

### 1.3 Port and Protocol Multiplexing

- **Default Port**: `9138` (configurable via `config.json` `webui.port`, `--webui-port`, or `HANCHO_WEBUI_PORT`).
- **HTTP / WebSocket Multiplexing on the Same Port**:
  - Regular HTTP requests (`GET /`, `GET /assets/*`) return static UI assets (HTML / JS / CSS / SVG).
  - HTTP requests with an `Upgrade: websocket` header trigger the WebSocket handshake and switch to the Hancho IPC protocol (JSON-RPC multiplexing and streaming subscriptions).
  - Sharing the port avoids port conflicts and halves firewall/tunnel configuration overhead.

### 1.4 Browser Launch Helper

The CLI provides a helper command to open the browser dashboard:

```bash
hancho-cli web
# or
hancho-cli ui
```

This helper reads the active session token and port from the data directory and opens `http://127.0.0.1:<port>/?token=<token>` in the system's default browser (`xdg-open` / `open` / `ShellExecute`).

### 1.5 Exposure Settings (`config.json` -> `webui`)

The server is loopback-only by default. Exposure is controlled by the `webui` object in `<data-dir>/config.json`:

```json
{
  "webui": {
    "enabled": true,
    "scope": "lan",
    "host": "0.0.0.0",
    "port": 9138,
    "allowed_hosts": ["desktop-abc", "desktop-abc.local"],
    "allowed_origins": ["http://desktop-abc:9138"]
  }
}
```

| Field | Values | Meaning |
|---|---|---|
| `enabled` | `true` / `false` | Start the browser UI. |
| `scope` | `loopback` (default), `lan`, `custom` | `loopback` binds `127.0.0.1` and accepts only loopback `Host` / `Origin`. `lan` / `custom` bind `host` and accept IP literals plus `allowed_hosts` / `allowed_origins`. |
| `host` | `0.0.0.0`, `127.0.0.1`, an IP, or a hostname | Bind address for `lan` / `custom`. `0.0.0.0` listens on all interfaces. |
| `port` | `1..65535` | Listen port (default `9138`). |
| `allowed_hosts` | hostnames | Extra names accepted in `Host` / `Origin` (for example a machine name). |
| `allowed_origins` | exact origins | Extra `http(s)://host:port` origins accepted. |

Environment overrides (for headless/test use): `HANCHO_WEBUI_HOST`, `HANCHO_WEBUI_SCOPE`, `HANCHO_WEBUI_ALLOWED_HOSTS`, `HANCHO_WEBUI_ALLOWED_ORIGINS`, `HANCHO_WEBUI_PORT`, `HANCHO_WEBUI_DISABLE`. CLI flags map to the same variables (`--webui-host`, `--webui-scope`, `--webui-allowed-hosts`, `--webui-allowed-origins`, `--webui-port`, `--webui-disable`).

Changing `webui` through `config_set` restarts the listener in the background. On the Windows dashboard, Settings -> Maintenance exposes Enabled, Exposure (Loopback / LAN), and Allowed hosts; `port` and `allowed_origins` stay in `config.json`.

**Security:** `lan` / `custom` intentionally relax the localhost-only restriction. The session token is still required for non-loopback clients, but there is no TLS. Use this only on a trusted LAN or VPN, and open the port explicitly on Windows, for example:

```powershell
New-NetFirewallRule -DisplayName "Hancho WebUI" -Direction Inbound -LocalPort 9138 -Protocol TCP -Action Allow
```

For access over the internet, prefer an SSH tunnel or VPN (6.3.4).

---

## 2. Localhost Authentication and Security

By default the browser UI listens only on localhost (`127.0.0.1`). It still defends against malicious scripts on other browser tabs (DNS Rebinding, CSRF, Cross-Site WebSocket Hijacking) and unprivileged local processes. When `webui.scope` is `lan` or `custom`, the `Host` / `Origin` allowlist below is widened as configured.

### 2.1 Threat Model

| Threat | Description | Countermeasure |
|---|---|---|
| **DNS Rebinding** | Malicious external domain DNS resolves to `127.0.0.1`, executing scripts against local API | Strict verification of the HTTP `Host` header |
| **Cross-Site Request Forgery (CSRF)** | Another browser tab sends requests to `http://localhost:9138/` | Mandatory session token, no permissive CORS headers |
| **Cross-Site WebSocket Hijacking** | External website opens `ws://localhost:9138/` (not restricted by SOP) | Strict `Origin` header verification and token check |
| **Multi-user Interception** | Other local users on a shared machine connect to localhost port | Token file protected with `0600` file permissions |

### 2.2 Session Token Scheme

1. **Token Generation**:
   - On core startup, a cryptographically secure 128-bit random token (32 hex characters) is generated.
   - Example: `a7f9b2c3d4e5f60718293a4b5c6d7e8f`
2. **Token File Storage**:
   - The token is written to `<data-dir>/.webui_token`.
   - File permissions are restricted to owner-only read/write (`chmod 0600` on POSIX).
   - The file is automatically deleted upon clean shutdown of the core.
3. **Fixed Token Support**:
   - If `HANCHO_TOKEN` or a host token in `hosts.json` is configured, it is also accepted as a valid token (for headless automated testing or SSH tunnel setups).

### 2.3 Token Handover and URL Scrubbing

1. **Initial URL Query Handover**:
   - When launching the browser, the token is passed as a query parameter:
     `http://127.0.0.1:9138/?token=<session_token>`
2. **Immediate Removal from URL**:
   - Upon page load, `app.js` extracts the token from `window.location.search`, stores it in memory (or `sessionStorage`), and immediately scrubs it from the URL bar:
     ```javascript
     const params = new URLSearchParams(window.location.search);
     const token = params.get('token');
     if (token) {
       sessionStorage.setItem('hancho_token', token);
       history.replaceState({}, document.title, window.location.pathname);
     }
     ```
   - This prevents token leakage through browser history, screenshots, or the `Referer` header when clicking external links.

### 2.4 WebSocket Authorization

1. **Handshake Verification**:
   - The browser provides the token via the query parameter `ws://127.0.0.1:9138/ws?token=<token>`, protocol header (`Sec-WebSocket-Protocol: hancho-auth, <token>`), or an initial auth frame (`{"action": "auth", "token": "..."}`).
2. **Rejection**:
   - Requests without a valid token receive `401 Unauthorized` (HTTP) or a close frame `4401 Unauthorized` (WebSocket).

### 2.5 Defense-in-Depth

1. **`Host` Header Check**:
   - In the default `loopback` scope, every request must have a `Host` header matching `localhost`, `127.0.0.1`, or `[::1]` (with port).
   - In `lan` / `custom` scope, loopback, IP literals (for example `192.168.1.20:9138`), and the configured `allowed_hosts` are accepted; other hostnames receive `403 Forbidden`.
2. **`Origin` Header Check**:
   - If an `Origin` header is present, it must be loopback, an IP literal, an exact `allowed_origins` entry, or a host in `allowed_hosts`. Any other origin is rejected immediately.

---

## 3. UI Asset Location, Bundling, and Updates

### 3.1 Repository Layout

Static UI assets reside under `src/webui/`:

```
src/webui/
├── index.html        # SPA layout and structure
├── app.js            # Main client logic (WebSocket, state, router)
├── style.css         # Dark theme stylesheet matching Hancho GUI colors
└── assets/           # Static icons (SVG), fonts, etc.
```

### 3.2 Zero External Runtime Dependencies

- **No Framework Lock-in**:
  - Implemented using pure standard HTML5, CSS3, and ES6+ Vanilla JavaScript without mandatory npm/webpack build toolchains.
- **No External CDN Requests**:
  - No remote CDN scripts, fonts, or assets. Runs completely self-contained in air-gapped and offline environments.

### 3.3 Delivery Modes (Embedded vs. External Directory)

```
[Request for Static Asset]
           |
           v
+-----------------------------------------------+
| Is an external directory specified?           |
| 1. --webui-dir <path>                         |
| 2. Environment variable HANCHO_WEBUI_DIR      |
| 3. <data-dir>/webui/                          |
| 4. Installed share/hancho/webui/              |
+----------------------+------------------------+
                       |
        +--------------+--------------+
        | YES                         | NO
        v                             v
+-------------------------------+ +-------------------------------+
| Serve from external directory | | Serve embedded binary resource|
| (development & customization) | | (default single-binary dist)  |
+-------------------------------+ +-------------------------------+
```

1. **Embedded Binary Resource (Default / Distribution)**:
   - At CMake build time, all assets under `src/webui/` are converted into C++ byte arrays and compiled into the executable.
   - Distributing a single binary provides a complete, working web dashboard with zero risk of missing asset files.
2. **External Directory Override (Development & Customization)**:
   - If `--webui-dir <dir>`, `HANCHO_WEBUI_DIR`, `<data-dir>/webui/`, or `${CMAKE_INSTALL_PREFIX}/share/hancho/webui/` is provided, files are served from disk.
   - Frontend developers can modify HTML/CSS/JS and test changes immediately via browser reload without recompiling C++.

### 3.4 Update Policy

- **Standard Updates**: Updating the Hancho binary automatically updates embedded UI assets to the matching version.
- **Custom Overrides**: Users wishing to customize themes or styles can place overriding files in `<data-dir>/webui/` without modifying the binary.

---

## 4. Pending Native GUI and Resumption Criteria

### 4.1 Decision to Suspend Native GUI

Development of desktop-native GUI applications (Qt, GTK, Tauri, wxWidgets, Electron, etc.) for Linux and macOS is **officially suspended (pending)**.

#### Rationale

1. **Portability and Static Linking Principles**:
   - Hancho favors single-binary, minimal-dependency distribution (`/MT` on Windows, minimal dynamic dependencies on POSIX).
   - Qt and GTK introduce heavy dynamic runtime dependencies, desktop environment quirks (GNOME vs. KDE, Wayland vs. X11), and version mismatch problems across distributions.
   - Tauri and Electron introduce Rust/Node.js toolchains and massive WebKit/Chromium dependencies, conflicting with Hancho's clean C++ CMake workflow.
2. **Alignment with Core IPC Separation**:
   - Hancho v3 decouples the GUI into an independent IPC client (`doc/design-v3.md` §2.3, `doc/gui-ipc-state-inventory.md`).
   - Given that the core serves IPC via WebSocket, modern web browsers offer the most consistent and portable client across all non-Windows platforms.
3. **Durability**:
   - Closing a browser tab never affects running jobs. Re-opening the tab reconnects and restores state instantly.

### 4.2 Resumption Criteria (What Browser UI Lacked)

Resuming native GUI development will only be considered if **one of the following proves fundamentally impossible in the browser UI, cannot be accommodated by the TUI (`hancho-cli top`), and becomes a fatal operational blocker**:

1. **Indispensable Native System Tray & Background Notifications**:
   - System tray menu interaction or background dock/tray badge updates cannot be satisfied by Web Notifications or OS notification helpers (`notify-send` / `osascript`).
2. **PTY Performance and Interactive Emulation Limits**:
   - Browser terminal emulation (xterm.js etc.) fails to deliver acceptable performance for high-throughput stream output, specialized terminal keybindings, Sixel graphics, or inline IME integration.
3. **Severe Desktop Integration Constraints**:
   - Browser sandbox restrictions critically impede necessary developer workflows, such as local file drag-and-drop, bi-directional IDE/editor integration, or native file chooser dialogs.
4. **Extreme Low-Resource Environments**:
   - Browser memory overhead is unacceptable on constrained devices or VMs, and the TUI (`hancho-cli top`) is insufficient for visual monitoring needs.

---

## 5. Scope of Major Features

The browser UI implements the **5 primary feature domains** found in the Windows Direct2D GUI (`src/gui_dashboard.cpp`), as outlined in `061-linux-gui.md` and `061-3-browser-ui-panes.md`:

| Feature Domain | Browser UI Implementation | Differences from Windows Direct2D GUI |
|---|---|---|
| **1. Process List** | Process table, statuses (running/completed/failed), PID, CPU/memory stats, process lifecycle controls (kill, kill all, clear, reset IDs, remove), start process, terminal log tailing & stdin streaming | Job Object metrics mapped to POSIX cgroups/process groups. ConPTY specific settings replaced by piped/PTY terminal viewer |
| **2. Task Queue** | KPI cards by state, job table, job details modal, retry, cancel, pause/resume queue, purge finished, resource contention locks | Identical actions and status displays |
| **3. Monitor Pane** | Active agent selection, real-time streaming of normalized events (`hancho.event/1`), tool calls/thoughts/progress, supervisor goal/notes editor, responsive SVG usage graphs | Rendered using responsive HTML/SVG instead of Direct2D |
| **4. Failure Diagnosis** | "Why did it fail" failure category breakdown, diagnosed error cards, evidence inspector, actionable remediation advice | Equivalent badge displays and evidence inspector |
| **5. Memory List** | Hierarchical search (L1 runs, L2 notes, L3 knowledge), record inspector, L2 note retraction, L3 promotion, storage capacity stats, L1 pruning | Integrated into a dedicated tab/pane rather than tray dialogs |

### 5.1 Pane Capabilities Overview

1. **Processes Pane**:
   - Live table of all processes (ID, state, PID, command, CPU %, memory, exit code, started time).
   - Start Process dialog: launch new commands with working dir, execution mode (`pipe` or `pty`), and optional timeout (`start`).
   - Terminal & Log viewer modal: inspect real-time stdout/stderr output tail (`logs`) and deliver input to stdin of running processes (`send`).
   - Process control: kill individual process (`kill`), remove finished process (`remove`), kill all running processes (`kill_all`), clear finished (`clear`), reset process IDs (`reset_ids`).

2. **Task Queue Pane**:
   - KPI summary cards for states: Total, Running, Waiting, Pending, Blocked, Succeeded, Failed, Cancelled (`taskqueue_counts`).
   - Job table with search and filtering by state pills.
   - Global queue actions: pause queue (`taskqueue_pause`), resume queue (`taskqueue_resume`), purge completed/failed jobs (`taskqueue_purge`).
   - Toggle resource contention locks (`taskqueue_resource_locks`).
   - Job details modal with metadata inspector, manual retry (`taskqueue_retry`), and cancellation (`taskqueue_cancel`).

3. **Monitor Pane**:
   - Active agent selector dropdown.
   - Supervisor goal and notes editor with live updates (`update_agent_info`).
   - Normalized event stream (`hancho.event/1`) feed with event filtering (Tool Calls, Thinking, Progress, Errors).
   - Usage and cost analytics: prompt/output/total token counters, estimated cost ($), responsive SVG usage trend chart, daily breakdown table (`get_usage_history`, `get_usage_daily`).

4. **Failure Diagnosis Pane**:
   - Queue-wide classification summary with counts & badges: `exit_nonzero`, `timeout`, `memory_limit`, `signal_terminated`, `cancelled`, `tool_error` (`taskqueue_diagnose` with breakdown).
   - Diagnosed failure cards: failure category badge, error summary, culprit evidence (command, exit status, stderr excerpt), and actionable remediation guidance.

5. **Memory Pane**:
   - Hierarchical storage stats: Total storage used, L1 run records, L2 notes, L3 knowledge, quota bar, quota health warning status (`memory_storage_stats`).
   - Search across layers (`memory_search`) with layer filtering pills (All / L1 / L2 / L3).
   - Memory records table with details inspector modal.
   - Operations on L2 Notes: promote to L3 established knowledge (`memory_promote`), retract invalid/obsolete note with reason (`memory_retract`).
   - Prune L1 execution records with retention days and dry-run calculation (`memory_prune`).

### 5.2 Features Shaved or Adapted from Windows GUI

| Component | Windows Direct2D GUI | Browser UI (Linux / macOS) | Rationale |
|---|---|---|---|
| **Graphics Engine** | Direct2D 1.1 / DirectWrite / DXGI hardware acceleration | Standard HTML5 / CSS3 Grid & Flexbox / Responsive SVG | Relies on standard browser rendering engines to eliminate GPU driver dependencies across diverse Linux desktop environments (Wayland/X11). |
| **Sixel Inline Graphics** | Direct2D bitmap blitting for Sixel images | Standard SVG vector charts and preformatted text logs | SVG provides sharp, responsive charting without requiring a complex ANSI Sixel parser in the browser. |
| **Process Accounting** | Win32 Job Object counters (`JOBOBJECT_BASIC_ACCOUNTING_INFORMATION`) | POSIX process group / `getrusage` / `/proc` metrics | Aligns directly with POSIX process models and cgroups. |
| **System Tray Menus** | Win32 taskbar tray icon (`Shell_NotifyIcon`) and popup menus | First-class top navigation tabs (Processes, Task Queue, Monitor, Diagnosis, Memory) | Promotes secondary features (Memory, Diagnosis) to primary tabs, avoiding desktop environment tray discrepancies (GNOME / KDE / macOS MenuBar). |
| **Modal Dialogs** | Win32 native modal dialogs (`DialogBoxParam`, `GetOpenFileName`) | SPA in-page HTML modal overlays | Keeps all interactions within the browser sandbox, preventing window manager blocking. |
| **ConPTY Configuration** | ConPTY pseudoconsole handle and font configuration | Universal pipe/PTY log streaming and stdin delivery | Simplifies terminal interaction to standard UTF-8 stream delivery across platforms. |

---

## 6. Build and Distribution Packaging

The Hancho Browser UI is engineered to be **completely self-contained within single binaries**, allowing third parties to build, deploy, and operate the dashboard without any external web server, frontend dependencies, or complicated setup.

### 6.1 Build Instructions

#### 6.1.1 Toolchain Prerequisites
- **CMake**: 3.20 or newer
- **C++ Compiler**: Supporting C++20
  - Linux: GCC 11+ or Clang 14+ (`libutil`, `pthreads`)
  - macOS: Apple Clang 14+ (`pthreads`, libc-integrated PTY)
  - Windows: MSVC 2019+ (`/MT` static runtime)
- **Zero Frontend Dependencies**: Node.js, npm, yarn, webpack, or cargo are **never required**. There are no external CDN dependencies, ensuring clean builds in offline and air-gapped environments.

#### 6.1.2 Single-Binary Build (Default & Release)
Static web assets (HTML, CSS, JS, SVG) are stored in `src/webui_assets.cpp` as compiled C++ raw string literals. A standard CMake build produces binaries with the full web dashboard embedded:

- **Linux / macOS**:
  ```bash
  cmake -S . -B build-posix -DCMAKE_BUILD_TYPE=Release
  cmake --build build-posix --parallel
  ```
  Generated binaries:
  - `build-posix/Hancho`: Headless core with integrated HTTP + WebSocket WebUI server
  - `build-posix/hancho-cli`: Command-line client with `hancho-cli web` / `ui` launcher
  - `build-posix/webui_smoke`: Smoke test suite for WebUI HTTP/WS verification

- **Windows**:
  ```cmd
  cmake -S . -B build -A x64
  cmake --build build --config Release
  ```
  Generated binaries:
  - `build\Release\Hancho.exe`: Direct2D GUI and embedded WebUI server
  - `build\Release\hancho-cli.exe`: Command-line client

#### 6.1.3 Frontend Development and Asset Workflow
Developers iterating on frontend assets (HTML/CSS/JS) can test changes in real time without recompiling C++:

1. **Live Disk Loading (Development)**:
   ```bash
   # Point to the webui source directory using --webui-dir
   ./build-posix/Hancho --headless --webui-dir src/webui --data-dir ./ww-data
   # Or set the environment variable
   export HANCHO_WEBUI_DIR=$(pwd)/src/webui
   ```
   Edits to files in `src/webui/` (`index.html`, `app.js`, `style.css`) take effect immediately upon refreshing the browser (F5).

2. **Synchronizing Embedded Assets (`src/webui_assets.cpp`)**:
   Once frontend changes are finalized, copy the updated contents of `src/webui/` into the corresponding string literals in `src/webui_assets.cpp` (`kIndexHtml`, `kStyleCss`, `kAppJs`, `kIconSvg`).

#### 6.1.4 Verification & Testing
Automated tests verify HTTP asset serving, MIME types, token authorization, Host/Origin headers, WebSocket multiplexing, and reconnection restoration:

```bash
# POSIX
./build-posix/webui_smoke

# Windows
build\Release\webui_smoke.exe
```

---

### 6.2 Packaging and Distribution Formats

#### 6.2.1 Standalone Single-Binary Archive (Recommended)
Because assets are compiled into the binary, distribution requires only the core executables and accompanying docs/tools:

- **Linux / macOS Archive Layout (`hancho-vX.Y.Z-<os>-<arch>.tar.gz`)**:
  ```
  hancho-v0.9.5-linux-x64/
  ├── bin/
  │   ├── Hancho                  # Core process (with embedded WebUI)
  │   └── hancho-cli              # CLI client (with web launch helper)
  ├── doc/                        # Topic documentation
  │   ├── browser-ui.md
  │   ├── browser-ui-ja.md
  │   └── ...
  ├── tools/                      # Task-card runners and scripts
  ├── LICENSE
  ├── README.md
  └── README-ja.md
  ```

- **Creating Distribution Tarball (POSIX)**:
  ```bash
  VERSION="0.9.5"
  PKG_DIR="dist/Hancho-v${VERSION}-linux-x64"
  mkdir -p "${PKG_DIR}/bin" "${PKG_DIR}/doc" "${PKG_DIR}/tools"
  cp build-posix/Hancho build-posix/hancho-cli "${PKG_DIR}/bin/"
  cp -r doc/* "${PKG_DIR}/doc/"
  cp -r tools/* "${PKG_DIR}/tools/"
  cp LICENSE README.md README-ja.md "${PKG_DIR}/"
  tar -czf "dist/Hancho-v${VERSION}-linux-x64.tar.gz" -C dist "Hancho-v${VERSION}-linux-x64"
  ```

- **Windows Distribution**:
  - `dist/Hancho-vX.Y.Z-win64.zip`: Standalone zip archive with binaries and docs.
  - `dist/Hancho-vX.Y.Z-win64-setup.exe`: Inno Setup installer.

#### 6.2.2 FHS-Compliant Package Separation (deb / rpm / Homebrew)
For distribution package maintainers adhering to the Linux Filesystem Hierarchy Standard (FHS):

- **Directory Layout**:
  - Binaries: `/usr/bin/` or `/usr/local/bin/`
  - Web UI Assets (Optional override): `/usr/share/hancho/webui/` or `/usr/local/share/hancho/webui/`
  - Documentation: `/usr/share/doc/hancho/`
- **Fallback Resolution**:
  The `Hancho` core resolves web assets in the following order:
  1. CLI parameter `--webui-dir <path>`
  2. Environment variable `HANCHO_WEBUI_DIR`
  3. Data directory `<data-dir>/webui/`
  4. FHS installed directory `${CMAKE_INSTALL_PREFIX}/share/hancho/webui/`
  5. Built-in embedded assets (default fallback)
  Even if external asset files are missing or modified incorrectly, the embedded assets ensure the web dashboard always operates reliably.

---

### 6.3 Third-Party Installation & Setup Guide

Step-by-step instructions for end users and third-party adopters:

#### 6.3.1 Step 1: Install Executables
Extract the distribution archive and place the executables in your `PATH`:

```bash
tar -xzf Hancho-v0.9.5-linux-x64.tar.gz
cd Hancho-v0.9.5-linux-x64

# Install to /usr/local/bin (or ~/.local/bin)
sudo cp bin/Hancho bin/hancho-cli /usr/local/bin/
sudo chmod +x /usr/local/bin/Hancho /usr/local/bin/hancho-cli

# Verify installation
hancho-cli --version
```

#### 6.3.2 Step 2: Start the Core Process
Start the core process for your desired project or data directory:

```bash
# Foreground or background daemon
Hancho --headless --data-dir ./ww-data &
```
*(Note: If the core is not already running, executing any `hancho-cli` command will launch it automatically.)*

- **Custom Web Port**:
  If port `9138` is already in use, pass `--webui-port` or set `HANCHO_WEBUI_PORT`:
  ```bash
  Hancho --headless --webui-port 9200 --data-dir ./ww-data
  ```

#### 6.3.3 Step 3: Open the Dashboard
Launch the dashboard in your system's default browser:

```bash
hancho-cli web
# (alias: hancho-cli ui)
```

- **Headless Servers / URL Display Only**:
  On remote servers without a desktop environment or display server, use `--print-url`:
  ```bash
  hancho-cli web --print-url
  # Output: http://127.0.0.1:9138/?token=4c6e93...
  ```
  Or retrieve as JSON:
  ```bash
  hancho-cli web --json
  ```

#### 6.3.4 Remote Access (LAN, VPN, or SSH Tunneling)

**LAN access.** Put this in `<data-dir>/config.json` and restart Hancho:

```json
{ "webui": { "enabled": true, "scope": "lan", "host": "0.0.0.0", "port": 9138,
             "allowed_hosts": ["desktop-abc"] } }
```

Then open `http://<windows-ip>:9138/?token=<token>` from another device. Add `allowed_hosts` if you use a machine name instead of an IP, and allow the port in Windows Firewall. The token is required for non-loopback clients.

**SSH tunnel (recommended over untrusted networks).** The default `loopback` scope is the safest choice for remote administration:

1. **Establish an SSH Port Forward**:
   ```bash
   ssh -L 9138:127.0.0.1:9138 user@remote-host
   ```
2. **Retrieve the URL on the Remote Host**:
   ```bash
   hancho-cli web --print-url
   ```
3. **Open Locally**:
   Paste the URL (`http://127.0.0.1:9138/?token=...`) into your local browser. Because requests originate as `localhost:9138`, all Host and Origin checks pass securely over the encrypted SSH tunnel.

#### 6.3.5 Service Configuration Examples (systemd / launchd)

- **Linux (systemd user unit: `~/.config/systemd/user/hancho.service`)**:
  ```ini
  [Unit]
  Description=Hancho Headless Core & WebUI Dashboard
  After=network.target

  [Service]
  Type=simple
  ExecStart=/usr/local/bin/Hancho --headless --data-dir %h/.local/share/hancho
  Restart=on-failure
  RestartSec=5s

  [Install]
  WantedBy=default.target
  ```
  Enable and run:
  ```bash
  systemctl --user daemon-reload
  systemctl --user enable --now hancho
  ```

- **macOS (launchd agent: `~/Library/LaunchAgents/com.hancho.core.plist`)**:
  ```xml
  <?xml version="1.0" encoding="UTF-8"?>
  <!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
  <plist version="1.0">
  <dict>
      <key>Label</key>
      <string>com.hancho.core</string>
      <key>ProgramArguments</key>
      <array>
          <string>/usr/local/bin/Hancho</string>
          <string>--headless</string>
          <string>--data-dir</string>
          <string>/Users/username/.local/share/hancho</string>
      </array>
      <key>RunAtLoad</key>
      <true/>
      <key>KeepAlive</key>
      <true/>
  </dict>
  </plist>
  ```

---

## 7. Related Documents and Tasks

- [Hancho v3 Design Document](design-v3.md) (§2.4 Stage B, §7 Decisions)
- [Design v3 Pending Items](design-v3-todo.md) (§8 Linux / macOS GUI and TUI)
- [POSIX Headless Core Build](build-posix.md) (Build guide and execution)
- [Remote Hosts Specification](remote-hosts-ja.md) (WebSocket transport & token authentication)
- [GUI IPC State Inventory](gui-ipc-state-inventory.md) (Decoupled IPC actions)
- Parent Task: `ww-data/tasks/061-linux-gui.md`
- Subtasks:
  - `ww-data/tasks/061-1-browser-ui-approach.md` (HTTP + WebSocket approach & token authentication)
  - `ww-data/tasks/061-2-browser-ui-skeleton.md` (HTTP + WebSocket server skeleton)
  - `ww-data/tasks/061-3-browser-ui-panes.md` (5 feature panes in HTML/JS)
  - `ww-data/tasks/061-4-browser-ui-package.md` (Build & distribution packaging)
