# WCSP client sources

These files are copies of the client half of WinConsoleServer (commit `c76d4e7`):

- `protocol.h` / `protocol.cpp`
- `frame_reader.h` / `frame_reader.cpp`
- `iterminal_io.h`
- `remote_session.h` / `remote_session.cpp`

They implement the WCSP v1 wire protocol and the `RemoteSession` terminal backend
used by `TerminalView` in the standalone terminal client window (`src/term_client.cpp`).

Local changes made when importing them into Hancho:

- Fixed includes for the flat `src/wcsp` layout (`../common/frame_reader.h` and
  `../common/protocol.h` became `frame_reader.h` and `protocol.h`).
- Removed the WinConsoleServer `version.h` dependency. `hello.name` is now
  `"Hancho/" PC_VERSION` (or plain `"Hancho"` when `PC_VERSION` is not defined).

The server side lives in `../term_server.h` / `../term_server.cpp`; it attaches
WCSP sessions to `ProcessManager` ConPTY jobs instead of spawning its own shells
(except for the `cmd` / `powershell` / `bash` compatibility modes).

The protocol reference is [`doc/wcsp.md`](../../doc/wcsp.md).
