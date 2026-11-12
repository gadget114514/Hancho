# Saving Execution State Outside Task Queues (Plain Runs)

**[English](plain-runs.md) | [日本語 (Japanese)](plain-runs-ja.md)**

In Hancho, not only jobs managed by task queues (`TaskQueueJob`), but also standalone and manually invoked processes executed outside task queues have their minimal state saved upon completion. This enables them to be included in L1 run records (`RunRecord`) and reproduced deterministically via `memory rebuild` (decision per `doc/design-v3.md` §7, card `072-1`).

---

## 1. Target Executions

The following executions that do not belong to an active task queue are targeted:

1. **Standalone runs via `hancho-cli run` / `hancho-cli start`**:
   - One-shot command executions without batch queue flags (`--file`, `--exit-on-drain`, `--report`).
2. **Manual processes with `HANCHO_EVENT_PIPE`**:
   - Processes launched with the `HANCHO_EVENT_PIPE` environment variable, or wrapper scripts/CLIs that emit events using `hancho-cli event emit`.
3. **Direct executions via GUI or IPC `action: "start"`**:
   - Managed processes launched directly from the dashboard or via IPC without an associated task queue job ID.

---

## 2. Issuing jobId and attempt

Executions outside a task queue are issued unique IDs that do not conflict with existing queues:

- **jobId**:
  - If a `jobKey` (or `--job-id`) is specified by the caller, that ID is preserved.
  - If omitted, `TaskQueueManager::IssuePlainJobId(processId, name)` generates `j_plain_<name>_<pid>_<seq>` (e.g. `j_plain_build_1234_1`).
- **attempt**:
  - Standalone runs are typically one-shot executions, defaulting to `1`.

---

## 3. Storage Format and Location

The minimal execution state (start, end, exit code, log file, process ID) is persisted using the exact same schema as existing task queue state files (`logs/taskqueues/*.json`):

- **Location**: `logs/taskqueues/plain.json`
- **Queue name**: `"plain"`

### Example `plain.json` Structure

```json
{
  "name": "plain",
  "concurrency": 0,
  "paused": false,
  "version": 1,
  "notify_file": "",
  "max_timeout_sec": 0,
  "default_max_attempts": 1,
  "jobs": [
    {
      "id": "j_plain_deploy_555_1",
      "taskqueue": "plain",
      "name": "deploy",
      "summary": "Deploy production artifacts",
      "command": "deploy.cmd --prod",
      "cwd": "D:/wsgit/app",
      "state": "succeeded",
      "created_at": "2026-11-10 15:00:00",
      "updated_at": "2026-11-10 15:01:00",
      "attempts": [
        {
          "n": 1,
          "process_id": 555,
          "start": "2026-11-10 15:00:00",
          "end": "2026-11-10 15:01:00",
          "reason": "ok",
          "exit_code": 0,
          "log_file": "D:/wsgit/app/logs/proc_555.log",
          "last_lines_file": "D:/wsgit/app/logs/taskqueue/j_plain_deploy_555_1.1.tail.txt",
          "check_detail": ""
        }
      ]
    }
  ]
}
```

---

## 4. Integration with `memory rebuild`

- `memory rebuild` (`RebuildRunRecordsJson()`) scans `logs/taskqueues/*.json`.
- Because `plain.json` resides in that directory, executions outside queues are ingested and converted into L1 `RunRecord` entries (`rr_<jobId>_<attempt>`).
- Even for plain processes without JSON streams (`.events.jsonl`), the command line, start/end timestamps, duration, exit code, and trailing log errors are preserved in the RunRecord.

---

## 5. Failure Classification and Unobtainable Items (072-2)

1. **Deterministic Failure Classification**:
   - For attempts that finish outside a task queue (or lack pre-classified failure diagnostics), deterministic classification rules apply:
     - **`launch_error`**: The process failed to start (e.g. executable not found).
     - **`killed`**: The process was explicitly terminated or cancelled.
     - **`timeout`**: Exceeded `timeoutSec` or ended with timed out status.
     - **`idle_stall`**: Output or CPU activity stalled.
     - **`crash`**: Exited with an NTSTATUS exception code (e.g. `0xC0000005`).
     - **`exit_nonzero`**: Exited with a non-zero exit code.
     - **`success_check_failed`**: Exit code was 0 but check condition detail indicated failure.
2. **Handling Unobtainable Fields**:
   - Items that cannot be determined without an LLM event stream (token counts, `filesEdited`, and complex failure classes such as `tool_error_loop` or `turn_or_context_limit`) are kept empty. Hancho does not fabricate or guess these values.
   - If no events file exists, `filesRead` and `filesEdited` are empty arrays (`[]`), and `usage` is omitted.

---

## 6. Search and Stats Inclusion (072-2)

Executions outside task queues are first-class citizens in L1 memory:

- **`memory search --kind run`**:
  - Plain process records are indexed and returned like any task queue run.
  - Can be filtered specifically with `--taskqueue plain` or `--failure-class <class>`.
- **`memory stats`**:
  - Plain runs contribute to total runs, success rates, duration percentiles (median, p90), failure breakdown counts, and per-job aggregation.

---

## 7. Isolation from Existing Queues

To guarantee that existing queues (such as `main.json` or behavior tree queues) remain completely unaffected:

1. **Scheduler Non-Interference**:
   - Jobs in `plain` are persisted with finished states (`succeeded` or `failed`), so the task queue scheduler (`SchedulerLoop`) never picks them up.
2. **Exclusion from Bulk Retries**:
   - All-queue retries (e.g. `hancho-cli taskqueue retry --failed`) automatically skip the `plain` queue.
3. **ChainGuard / Command Conflict Exclusion**:
   - `FindUnfinishedJobByCommand` ignores the `plain` queue, ensuring previous one-off failures do not block new processes.
4. **Unique ID Namespace**:
   - The `j_plain_` prefix prevents collisions with normal task queue job IDs.
