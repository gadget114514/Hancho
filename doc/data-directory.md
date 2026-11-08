# Project-Local Data Directory (git-managed)

**[English](data-directory.md) | [日本語 (Japanese)](data-directory-ja.md)**

Logs, process state, task queues and GUI config live in one **data directory**. Select it per invocation with `--data-dir <path>`:

```powershell
hancho-cli.exe --data-dir .hancho taskqueue add --file jobs.json
hancho-cli.exe taskqueue list main --data-dir .hancho
hancho-cli.exe mcp install --data-dir .hancho   # embed it in the project's MCP config
```

How consistency is guaranteed:

1. Pick one project-local directory, e.g. `.hancho/`.
2. `hancho-cli.exe mcp install --data-dir .hancho` writes `--data-dir .hancho` into `.mcp.json` / `opencode.json`, so every MCP tool call for that project uses the same directory.
3. Pass the same `--data-dir` on every direct `hancho-cli.exe` call. It is **per invocation**, not a global setting.
4. Relative paths resolve against the caller's current directory; use an absolute path in the MCP config if the client's cwd is uncertain.
5. Different data directories are fully isolated: each gets its own dashboard instance, Named Pipe and mutex, so multiple projects can run at once without sharing processes or queues.
6. Verify with `hancho-cli.exe --data-dir <path> help --json` (`effective_data_dir`) or the dashboard status bar, which shows the active data directory.

The environment variable `HANCHO_LOG_DIR` is an equivalent fallback.
