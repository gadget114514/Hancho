# Using Hancho in CI (Jenkins, GitHub Actions, etc.)

All CI flags and environment variables are **opt-in**. Without them, Hancho keeps its default interactive behavior (WMI GUI autostart, queue-wide `run --exit-on-drain`, minimal JUnit XML, and keeping jobs running if `hancho-cli` exits).

## Two Modes for CI

| Mode | When to use | How it works |
|---|---|---|
| **Ephemeral core (`--ephemeral`)** *(recommended)* | Most CI builds (isolated, clean per build) | `hancho-cli run --ephemeral` starts `HanchoCore.exe --headless` as a child inside a Win32 Job Object (`KILL_ON_JOB_CLOSE`), uses `<cwd>\.hancho-ci\<pid>` by default, disables triage (`triage_mode: "off"`), and shuts the core down (cancelling any remaining jobs) when done. If CI aborts `hancho-cli`, the OS kills the core and all child jobs immediately. |
| **Shared resident core** | When a long-lived core runs as a Windows Service or Scheduled Task | Start `HanchoCore.exe --headless --data-dir <dir>` separately. Set `HANCHO_AUTOSTART=off` (or `--no-autostart`) in CI so builds fail fast if the service is down, and pass `--wait-scope direction --cancel-on-exit` so each build waits only for its own direction and cancels it if CI aborts `hancho-cli`. |

## Quick Reference: CI Flags & Environment Variables

- `run --file <queue.json>` / `run --bt <plan.json>`: Register a queue or start a behavior tree, wait for completion, and exit `0` (succeeded), `1` (failed/blocked), or `2` (cancelled/interrupted/timed out).
- `--ephemeral`: Launch a disposable per-build core inside a Job Object.
  - `--seed-from <dir>`: Copy `config.json`, `engines.json`, LLM lists, `failure-rules.json`, and approvals from `<dir>`.
  - `--triage`: Keep triage enabled (by default `--ephemeral` sets `triage_mode: "off"` and `triage_llm_mode: "manual"` so failed jobs do not invoke an LLM).
- `--wait-scope <queue|direction>`: Default `queue` (waits for the entire queue). Pass `direction` to wait only for the direction registered by this `run` invocation.
- `--cancel-on-exit`: On a shared core, watch `hancho-cli`'s PID and cancel this direction (reason: `client exited`) if `hancho-cli` is killed.
- `--follow`: Stream job start/finish lines and the last 20 lines of failed job logs to stdout (or NDJSON with `--json`).
- `--logs-dir <dir>`: Copy finished job logs to `<dir>\<jobId>.log` (or `<dir>\<btId>\<jobId>.log` for `--bt`).
- `--report <junit.xml>`: Write a JUnit XML report (`--bt` includes each job plus 1 testcase for the BT result).
- `--report-output` / `--report-output-bytes <n>`: Include the tail of each job's log (default 64 KB) in `<system-out>` and failure class/exit code in `<failure>`.
- `--timeout <sec>`: Cancel the direction and exit `2` if the run exceeds `<sec>` seconds.
- `--task <id>`: Attach the direction to an existing task record.
- `--no-autostart` / `HANCHO_AUTOSTART=headless|off|gui`: Control core autostart when no core is running (default: `gui`).
- `HANCHO_NO_DIALOGS=1`: Suppress `MessageBoxW` dialogs in headless/Session 0 environments and write to stderr/log instead (also implied when `HANCHO_AUTOSTART` is set).

## Jenkins Example (Windows Agent)

```groovy
pipeline {
    agent { label 'windows' }
    stages {
        stage('Build & Test') {
            steps {
                bat '''
                    hancho-cli.exe run --ephemeral --file ci/jobs.json ^
                        --follow --logs-dir build/hancho-logs ^
                        --report build/junit.xml --report-output ^
                        --timeout 1800
                '''
            }
        }
    }
    post {
        always {
            junit allowEmptyResults: true, testResults: 'build/junit.xml'
            archiveArtifacts artifacts: 'build/hancho-logs/**', allowEmptyArchive: true
        }
    }
}
```

For a behavior tree plan (`ci/plan.json`), replace `--file ci/jobs.json` with `--bt ci/plan.json`.

## GitHub Actions Example (Self-Hosted Windows Runner)

```yaml
jobs:
  build:
    runs-on: self-hosted
    steps:
      - uses: actions/checkout@v4
      - name: Run Hancho CI queue
        shell: cmd
        run: |
          hancho-cli.exe run --ephemeral --file ci/jobs.json --follow --logs-dir artifacts\logs --report artifacts\junit.xml --report-output --timeout 1800
      - name: Upload logs
        if: always()
        uses: actions/upload-artifact@v4
        with:
          name: hancho-logs
          path: artifacts/logs
```

## Running a Shared Core as a Service / Scheduled Task

1. Register `HanchoCore.exe --headless --data-dir C:\hancho-ci-data` as a Windows Service (e.g., via `sc.exe` or NSSM) or a Scheduled Task running at startup.
2. In CI jobs, set:
   ```bat
   set HANCHO_AUTOSTART=off
   set HANCHO_NO_DIALOGS=1
   hancho-cli.exe --data-dir C:\hancho-ci-data run --file ci/jobs.json --wait-scope direction --cancel-on-exit --follow --logs-dir build\hancho-logs --report build\junit.xml --report-output
   ```
3. **Session 0 considerations**: Services run in Session 0 without a desktop. Never use GUI autostart in Session 0; use `--ephemeral`, `HANCHO_AUTOSTART=headless`, or a pre-started `HanchoCore.exe --headless`, and set `HANCHO_NO_DIALOGS=1` so startup errors go to stderr/logs instead of blocking on a hidden `MessageBoxW`.
4. **LLM engine authentication**: If your CI jobs or BTs run `agent` nodes (`claude`, `opencode`, `codex`, `agy`), log in under the **same Windows account** that runs the CI agent or service (e.g., `claude` credentials live in that user's profile), or pass API keys via environment variables.
5. **Why triage is off by default in `--ephemeral`**: Interactive triage invokes an LLM to classify and propose fixes for failed jobs. In CI, deterministic exit codes and JUnit reports are desired without unexpected LLM calls or costs. Pass `--triage` only if you explicitly want LLM triage in an ephemeral run.
