# Load test (card 102-9)

Reproduces the 403-job dashboard benchmark from `doc/task-list-redesign-cards.md`
("測り方"). The script creates its own isolated data directory, disables triage,
queues the jobs, waits for the three long-running jobs, and reports the CPU time
of `HanchoDashboard.exe` over a 60-second window.

## Run

Build the Release binaries first, then:

```powershell
powershell -ExecutionPolicy Bypass -File tools\loadtest\run-loadtest.ps1
```

Options:

| Option | Default | Meaning |
| --- | --- | --- |
| `-HanchoExe` | `build\Release\Hancho.exe` | GUI launcher to measure (or set `HANCHO_EXE`) |
| `-CliExe` | `build\Release\hancho-cli.exe` | CLI used to enqueue/poll (or set `HANCHO_CLI`) |
| `-Root` | checkout root | Where the default `build\Release` paths are found |
| `-FastJobs` | 400 | Number of short jobs |
| `-FailEvery` | 13 | Every N-th short job exits 1; 0 means none |
| `-LongJobs` | 3 | Number of `ping -n 1800 127.0.0.1` jobs |
| `-Concurrency` | 16 | `taskqueue add --concurrency` |
| `-SettleSec` | 20 | Wait after the running count reaches 3 |
| `-DurationSec` | 60 | CPU measurement window |
| `-ReadyTimeoutSec` | 60 | Startup / drain polling timeout |
| `-DataDir` | fresh temp dir | Empty data directory to use instead |
| `-KeepDataDir` | off | Print a reminder to keep the data dir |
| `-Help` | | Show built-in usage |

Use `-KeepDataDir` when you want to keep the generated `config.json`,
`loadtest-jobs.json` and `dashboard-perf.csv` for inspection. The script never
deletes the data directory; it only stops the processes it started.

## What the script does

1. Creates a fresh temp data directory and writes `config.json` with
   `"refresh_interval_ms": 2000`, `"triage_mode": "off"` and
   `"triage_llm_mode": "manual"`. **Both triage keys matter:** without them a
   failed job calls a real LLM (138 calls in the 2026-11-24 run).
2. Starts `Hancho.exe --data-dir <dir>` with `HANCHO_PERF_OUT` set only for that
   process tree. The launcher brings up `HanchoDashboard.exe` and
   `HanchoCore.exe` on the same data directory.
3. Generates the full 403-job file (`jobs.sample.json` shows the format) and
   runs `hancho-cli.exe --data-dir <dir> taskqueue add --file <file>
   --taskqueue loadtest --concurrency 16`.
   - 400 jobs: `cmd /c exit 0`, except every 13th: `cmd /c exit 1`.
   - 3 jobs: `ping -n 1800 127.0.0.1` (30 minutes), `timeoutSec: 3600`.
4. Polls `taskqueue list loadtest --state running --json` until exactly three
   jobs are running, waits 20 seconds, then reads the dashboard's
   `TotalProcessorTime` before and after a 60-second sleep. The report shows
   CPU seconds, percent of one core and percent of all cores.
5. Stops only the `Hancho.exe` tree it launched and the matching `ping.exe`
   processes created after the start. It never terminates an existing
   dashboard; if a dashboard is already open on another data directory, it is
   left alone.

## `HANCHO_PERF_OUT`

`src/gui_dashboard.cpp` checks `HANCHO_PERF_OUT` once at startup. When it is
unset, no clock is read and nothing is written (zero instrumentation overhead).
When it points to a file, the dashboard appends CSV rows:

```
start_ms,kind,detail,duration_ms
12345,tick,refresh,161
12346,ipc,taskqueue_counts,3
```

- `tick,refresh` — one `TIMER_REFRESH_ID` message (clock update, data refresh,
  next timer scheduling).
- `ipc,<action>` — one `GuiIpcCall` (`list`, `taskqueue_counts`,
  `taskqueue_list`, `chat_status`, ...), including failed calls.

`set HANCHO_PERF_OUT=C:\path\to\perf.csv` before launching `Hancho.exe` to
collect a log outside a load test. The dashboard holds the file open while it
runs, so point it at a scratch file (the script uses the temp data directory).

## Reference numbers

From `doc/task-list-redesign-cards.md`, same conditions (2026-11-24):

| Version | CPU | Refresh |
| --- | --- | --- |
| Before the fix | 63.6% | 1.32 s |
| With card 102-1 | 5.9% | ~0.16 s |
