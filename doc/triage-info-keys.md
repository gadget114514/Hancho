# Triage information keys

Each `info` section is an array of `{ "k": key, "v": value, "text": English }` items.
The catalog should format the value shown in the second column; `text` remains the fallback for
unknown keys and the English sentence sent to language models. User-provided names, commands,
paths, log contents, host names, and other data inside `v` are not translated.

## Job failure — cause

| Key | `v` shape | English template |
|---|---|---|
| `failureClass` | `{class, confidence}` | `failure class: {class} ({confidence} confidence)` |
| `summary` | `{class, summary}` | `{summary}` (classifier-provided; retain the original wording) |
| `exitCode` | `{actual, expected}` | `exit code {actual} (expected {expected})` |
| `checkDetail` | string | `success check failed: {detail}` |
| `timeoutPattern` | `progressing`, `stalled`, `repeated`, or `unknown` | `timeout pattern: {pattern}` |
| `evidence` | `{file, line, startLine, lines[]}` | `evidence {file}:{line} — {lines}` |
| `eventSequence` | integer | `event sequence: {value}` |
| `lastOutput` | `{file, lines[]}` | `last output lines: {lines}` |
| `llmGuess` | source JSON value | `task queue LLM guess (reference only): {value}` |
| `lastActivity` | source `lastActivity` object | `last activity: {activity} ({target}), {sinceSec}s ago` |

## Job failure — background

| Key | `v` shape | English template |
|---|---|---|
| `jobName` | string | `name: {value}` |
| `jobSummary` | string | `{value}` |
| `command` | string | `command: {value}` |
| `queue` | string | `queue: {value}` |
| `behaviorTree` | `{id, node?}` | `BT {id} / node {node}` |
| `attempt` | `{n, maxAttempts}` | `attempt {n}/{maxAttempts}` |
| `origin` | `{label}` or `{client, folder}` | `origin: {label}` or `origin: {client} + {folder}` |
| `attemptHistory` | array of `{n, start, durationSec?, reason?, class?}` | `attempt history: {history}` |
| `blockedDependents` | `{count, ids[]}` | `dependent blocked: {ids}` |

## Job failure — environment

| Key | `v` shape | English template |
|---|---|---|
| `host` | string | `host: {value}` |
| `cwd` | string | `cwd: {value}` |
| `write` | array of resource names | `resources write: {value}` |
| `read` | array of resource names | `resources read: {value}` |
| `resourceHolder` | `{resource, holderId, lockMode, holderState?, holderRunningSec?}` | `resource wait: {resource} ({holderId}, {lockMode} lock)` |
| `concurrencyAtFailure` | `{count, ids[]}` | `concurrent jobs at failure: {count} ({ids})` |

## Job failure — conditions

| Key | `v` shape | English template |
|---|---|---|
| `waitingSince` | `{at, elapsedSec}` | `waiting since {at}` |
| `timeoutSec` | integer seconds | `timeout: {value} seconds` |
| `attemptDurationSec` | integer seconds | `last attempt elapsed: {value} seconds` |
| `estimateSec` | integer seconds | `estimate: {value} seconds` |
| `elapsedEstimateRatio` | number | `elapsed/estimate ratio: {value}` |
| `idle` | `{output_sec, cpu_sec}` | `idle monitoring: output {output_sec}s, CPU {cpu_sec}s` |
| `successExitCode` | integer | `success exit code: {value}` |
| `successFilesNewer` | array | `success filesNewer: {value}` |
| `successOutputRegex` | string | `success outputRegex: {value}` |
| `successTestReport` | string or source JSON value | `success testReport: {value}` |
| `maxAttempts` | integer | `maximum attempts: {value}` |
| `retryDelaySec` | integer seconds | `retry delay: {value} seconds` |

## Job failure — reason

| Key | `v` shape | English template |
|---|---|---|
| `attemptReason` | string | `attempt reason: {value}` |
| `failureRule` | string | `failure rule: {value}` |
| `recommendationRationale` | `{key, params}` | `recommendation: {English rationale}` |

## BT escalation failure

| Section | Key | `v` shape | English template |
|---|---|---|---|
| cause | `escalationFailure` | string | `behavior tree escalation failed: {detail}` |
| background | `treeId` | string | `BT: {value}` |
| background | `treeName` | string | `tree: {value}` |
| background | `treeState` | string | `tree state: {value}` |
| background | `nodeId` | string | `node: {value}` |
| background | `question` | string | `question: {value}` |
| background | `options` | array | `options: {value}` |
| background | `guidance` | string | `guidance: {value}` |
| background | `relatedJobs` | array of job ids | `related jobs: {value}` |
| environment | `queue` | string | `queue: {value}` |
| conditions | `escalationTimeoutSec` | integer seconds | `escalation timeout: {value} seconds` |
| reason | `treeFailureReason` | string | `{value}` |

## Process failure

| Section | Key | `v` shape | English template |
|---|---|---|---|
| cause | `endState` | string | `state: {value}` |
| cause | `exitCode` | integer | `exit code: {value}` |
| cause | `lastOutput` | `{file, lines[]}` | `last log lines: {lines}` |
| cause | `logFile` | path string | `log: {value}` |
| background | `processName` | string | `name: {value}` |
| background | `command` | string | `command: {value}` |
| background | `startedAt` | timestamp | `started at: {value}` |
| environment | `cwd` | path string | `cwd: {value}` |
| environment | `mode` | `pipe`, `pty`, or `console` | `mode: {value}` |
| conditions | `timeoutSec` | integer seconds | `timeout: {value} seconds` |
| conditions | `idleTimeoutSec` | integer seconds | `idle timeout: {value} seconds` |
| conditions | `durationSec` | integer seconds | `elapsed: {value} seconds` |
| reason | `processEndReason` | string | `process ended in state {value}` |

## Timing divergence

| Section | Key | `v` shape | English template |
|---|---|---|---|
| cause | `timingAccuracy` | `diverged_over` or `diverged_under` | `timing accuracy: {value}` |
| background | `jobName` | string | `name: {value}` |
| background | `jobSummary` | string | `{value}` |
| background | `queue` | string | `queue: {value}` |
| environment | `host` | string | `host: {value}` |
| environment | `cwd` | path string | `cwd: {value}` |
| conditions | `actualDurationSec` | integer seconds | `actual duration: {value} seconds` |
| conditions | `estimateSec` | integer seconds | `estimate: {value} seconds` |
| conditions | `ratio` | number | `ratio: {value}` |
| conditions | `divergenceThreshold` | number | `divergence threshold: {value}` |
| conditions | `recalibrateCount` | integer | `unfinished same-name jobs recalibrate would change: {value}` |
| reason | `timingAdvice` | string | `{value}` |

## Resource wait

| Section | Key | `v` shape | English template |
|---|---|---|---|
| cause | `resourceWait` | string | `waiting for resource: {value}` |
| background | `jobName` | string | `name: {value}` |
| background | `jobSummary` | string | `{value}` |
| background | `queue` | string | `queue: {value}` |
| environment | `host` | string | `host: {value}` |
| environment | `cwd` | path string | `cwd: {value}` |
| environment | `resourceHolder` | `{jobId, state, lockMode, resource, runningSec?}` | `holder job {jobId} [{state}] ({lockMode} lock)` |
| conditions | `waitingSince` | `{at, elapsedSec}` | `waiting since {at} (approximately {elapsedSec}s)` |
| reason | `resourceConflict` | `{lockMode, resource}` | `A {lockMode} lock conflicts with another lock.` |

## Recommendation rationale keys

The `recommendation` object keeps its English `rationale` and adds `rationaleKey` plus `rationaleParams`.
The matching `recommendationRationale` info item stores `{key, params}` in `v`.

| `rationaleKey` | Params | English template |
|---|---|---|
| `timeoutProgressing` | `{timeoutSec}` | `The job was still making progress near its timeout; retry with more time.` |
| `timeoutStalled` | `{}` | `The job had stopped making progress well before the timeout; investigate first.` |
| `timeoutRepeated` | `{}` | `Consecutive attempts timed out; gather more information before retrying.` |
| `timeoutUnknown` | `{}` | `Timeout progress is unknown; a longer timeout is a cautious possibility.` |
| `idleStall` | `{}` | `The job stalled without activity; investigate the cause first.` |
| `retryAfter` | `{retryAt}` | `The failure is rate-limited; retry after {retryAt}.` |
| `raiseTimeout` | `{}` | `The failure classifier recommends raising the timeout.` |
| `fixSpecification` | `{}` | `The failure classifier recommends correcting the job specification.` |
| `transientRetry` | `{class}` | `The failure looks transient; an unchanged retry is likely to pass.` |
| `retryCheckCause` | `{class}` | `The failure classifier suggests a retry, but an unchanged rerun may fail the same way; check the cause first.` |
| `noRecommendation` | `{}` | `The failure does not have a safe, deterministic recommendation.` |
| `btEscalationFailure` | `{}` | `A human should inspect the failed decision request.` |
| `processFailure` | `{}` | `Hancho cannot determine whether a plain process should be restarted.` |
| `timingDivergence` | `{}` | `Use the measured duration to recalibrate unfinished jobs of the same kind and queue.` |
| `resourceWait` | `{}` | `Let the resource scheduler continue while recording the wait.` |
