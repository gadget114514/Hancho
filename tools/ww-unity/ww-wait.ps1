# ww-wait.ps1 — block until named hancho-cli task-queue jobs finish (card 334).
# Usage: Tools\ww\ww-wait.cmd ww-332 card-333 [-Queue cards] [-TimeoutSec <sec>]
# Exit codes: 0 all succeeded; 1 a job failed, was cancelled or can never start (or hancho-cli is
# unreachable); 2 timeout; 3 usage error or unknown job id (checked at the start).
# hancho-cli job states: pending, running, succeeded, failed, cancelled. A pending job whose
# dependency failed, was cancelled or is missing can never start, so it counts as finished.
# Get-WwWaitResult is pure: ww-wait.selftest.ps1 dot-sources this file and calls it directly.

param(
    [Parameter(Position = 0, ValueFromRemainingArguments = $true)][string[]]$JobId,
    [string]$Queue = "cards",
    [ValidateRange(0, 86400)][int]$TimeoutSec = 0
)

$ErrorActionPreference = "Stop"
$hanchoCli = if ($env:WW_HANCHO_CLI) { $env:WW_HANCHO_CLI } else { Join-Path $env:LOCALAPPDATA "Programs\Hancho\hancho-cli.exe" }

# True when a pending job can never start: a dependency is missing, failed or cancelled, or
# is itself such a job. $Path holds the ids on the current walk so a cycle cannot loop forever.
function Test-WwNeverStart([string]$Id, [hashtable]$ById, [System.Collections.Generic.HashSet[string]]$Path) {
    $job = $ById[$Id]
    if ($job.state -ne "pending") { return $false }
    [void]$Path.Add($Id)
    $neverStarts = $false
    foreach ($dep in @($job.depends_on)) {
        $depId = [string]$dep.id
        if (-not $ById.ContainsKey($depId) -or $ById[$depId].state -in @("failed", "cancelled")) { $neverStarts = $true; break }
        if (-not $Path.Contains($depId) -and (Test-WwNeverStart $depId $ById $Path)) { $neverStarts = $true; break }
    }
    [void]$Path.Remove($Id)
    return $neverStarts
}

# Classify the watched ids against the known jobs. Rows carry Status: succeeded, failed,
# cancelled, never-start (finished, not ok) or running/pending/other (still waiting).
# Done: every row is finished. Failed: some finished row is not succeeded.
function Get-WwWaitResult([object[]]$Jobs, [string[]]$Ids) {
    $byId = @{}
    foreach ($job in $Jobs) { $byId[[string]$job.id] = $job }
    $missing = @($Ids | Where-Object { -not $byId.ContainsKey($_) })
    $rows = @()
    foreach ($id in ($Ids | Where-Object { $byId.ContainsKey($_) })) {
        $state = [string]$byId[$id].state
        $status = $state
        if ($state -eq "pending" -and (Test-WwNeverStart $id $byId (New-Object System.Collections.Generic.HashSet[string]))) { $status = "never-start" }
        $rows += [pscustomobject]@{ Id = $id; State = $state; Status = $status; Finished = ($status -in @("succeeded", "failed", "cancelled", "never-start")) }
    }
    $finishedRows = @($rows | Where-Object { $_.Finished })
    return [pscustomobject]@{
        Missing = $missing
        Rows    = $rows
        Done    = ($missing.Count -eq 0 -and $finishedRows.Count -eq $rows.Count)
        Failed  = (@($finishedRows | Where-Object { $_.Status -ne "succeeded" }).Count -gt 0)
    }
}

# Test-only guard: dot-sourcing the file (selftest) stops here, before any hancho-cli call.
if ($MyInvocation.InvocationName -eq ".") { return }

# One long-poll of the queue. Without -Since hancho-cli returns at once (snapshot); with it, hancho-cli
# waits up to TimeoutMs and returns the full job list with the current version.
function Get-WwQueueSnapshot([string]$Queue, $Since, [int]$TimeoutMs) {
    $hanchoCliArgs = @("--json", "taskqueue", "wait", $Queue, "--timeout-ms", [string]$TimeoutMs)
    if ($null -ne $Since) { $hanchoCliArgs += @("--since", [string]$Since) }
    # hancho-cli writes UTF-8 (job summaries can hold Japanese); read it as UTF-8 like ww-enqueue.ps1.
    $prevEncoding = [Console]::OutputEncoding
    try {
        [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
        $raw = (& $hanchoCli @hancho-cliArgs 2>&1 | Out-String)
    } catch {
        return [pscustomobject]@{ ok = $false; error = $_.Exception.Message }
    } finally {
        [Console]::OutputEncoding = $prevEncoding
    }
    try { $obj = $raw | ConvertFrom-Json } catch { return [pscustomobject]@{ ok = $false; error = "non-JSON reply: $raw" } }
    if (-not $obj.ok) { return [pscustomobject]@{ ok = $false; error = [string]$obj.error } }
    return $obj
}

if (-not $JobId -or $JobId.Count -eq 0) {
    Write-Host "usage: ww-wait.cmd <jobId> [<jobId> ...] [-Queue cards] [-TimeoutSec <sec>]" -ForegroundColor Red
    exit 3
}
$ids = @($JobId | Select-Object -Unique)

# Every job seen in any snapshot, so a job purged after it finished keeps its last state.
$known = @{}
$since = $null
$lastStatus = @{}
$firstSnapshot = $true
$failures = 0
$maxFailures = 15
$deadline = if ($TimeoutSec -gt 0) { (Get-Date).AddSeconds($TimeoutSec) } else { $null }

while ($true) {
    $snap = Get-WwQueueSnapshot -Queue $Queue -Since $since -TimeoutMs 5000
    if (-not $snap.ok) {
        if ([string]$snap.error -match "not found") {
            Write-Host "[ww-wait] queue '$Queue' not found: $($snap.error)" -ForegroundColor Red
            exit 3
        }
        $failures++
        Write-Host "[ww-wait] hancho-cli call failed ($failures/$maxFailures): $($snap.error)" -ForegroundColor Yellow
        if ($failures -ge $maxFailures) { exit 1 }
        Start-Sleep -Seconds 2
        continue
    }
    $failures = 0
    $since = [int]$snap.version
    foreach ($job in @($snap.jobs)) { $known[[string]$job.id] = $job }

    $result = Get-WwWaitResult -Jobs @($known.Values) -Ids $ids
    if ($firstSnapshot -and $result.Missing.Count -gt 0) {
        Write-Host "[ww-wait] unknown job id in queue '$Queue': $($result.Missing -join ', ')" -ForegroundColor Red
        exit 3
    }
    $firstSnapshot = $false
    foreach ($row in $result.Rows) {
        if (-not $lastStatus.ContainsKey($row.Id) -or $lastStatus[$row.Id] -ne $row.Status) {
            Write-Host "[ww-wait] $($row.Id) $($row.Status)"
            $lastStatus[$row.Id] = $row.Status
        }
    }

    if ($result.Done) {
        foreach ($row in $result.Rows) { Write-Host "$($row.Id) $($row.Status)" }
        if ($result.Failed) { exit 1 }
        exit 0
    }
    if ($null -ne $deadline -and (Get-Date) -ge $deadline) {
        Write-Host "[ww-wait] timeout after $TimeoutSec s"
        foreach ($row in $result.Rows) { Write-Host "$($row.Id) $($row.Status)" }
        exit 2
    }
}
