# ww-watch.lib.ps1 — progress helpers for ww-watch.ps1, kept separate so the pure logic
# (Get-Snapshot / Test-Progress) can be unit-tested without starting a real run.
# Dot-source this file; it defines functions and has no side effects.
#
# Expects the caller to define $root (the repository root) before Get-Snapshot is called.

# ---------- process enumeration ----------
# Get-WorkerRuns (agy / opencode / claude / codex) and Resolve-WwPaths come from ww-run.lib.ps1.
. (Join-Path $PSScriptRoot "ww-run.lib.ps1")
function Get-Descendants([int[]]$rootIds) {
    $all = Get-CimInstance Win32_Process
    $found = New-Object System.Collections.Generic.HashSet[int]
    $queue = New-Object System.Collections.Generic.Queue[int]
    foreach ($id in $rootIds) { [void]$found.Add($id); $queue.Enqueue($id) }
    while ($queue.Count -gt 0) {
        $cur = $queue.Dequeue()
        foreach ($p in $all | Where-Object { $_.ParentProcessId -eq $cur }) {
            if ($found.Add([int]$p.ProcessId)) { $queue.Enqueue([int]$p.ProcessId) }
        }
    }
    return @($found)
}
function Get-CpuSeconds([int[]]$ids) {   # raw seconds, deliberately NOT rounded
    $sum = 0.0
    foreach ($id in $ids) { $p = Get-Process -Id $id -ErrorAction SilentlyContinue; if ($p) { $sum += $p.CPU } }
    return $sum
}

# ---------- progress ----------
# Select-ProgressStatus keeps only the `git status --porcelain` lines whose path lies under one
# of the -Watch folders (ww-data\config.json "watch"), or, with no watch list, outside the data
# folder (-Exclude). Get-Snapshot already asks git to limit itself to the watch folders, but doing
# the filter again in pure code (card 055) makes the rule explicit and lets ww-watch.selftest.ps1
# prove that an edit under ww-data/tasks (a task card) is never counted as run progress.
# Lines are "XY PATH" (or "XY OLD -> NEW" for renames); a quoted path is unquoted before the test.
function Select-ProgressStatus([string]$Porcelain, [string[]]$Watch = @(), [string]$Exclude = "ww-data") {
    $kept = New-Object System.Collections.Generic.List[string]
    $watchList = @(@($Watch) | Where-Object { $_ })
    foreach ($line in ($Porcelain -split "`r?`n")) {
        if ($line.Length -le 3) { continue }
        foreach ($p in ($line.Substring(3) -split ' -> ')) {
            $p = $p.Trim().Trim('"') -replace '\\', '/'
            $under = { param($dir) $p -like "$($dir.Trim('/'))/*" }
            $match = if ($watchList.Count) { [bool]($watchList | Where-Object { & $under $_ }) }
                     else { -not (& $under $Exclude) }
            if ($match) { $kept.Add($line); break }
        }
    }
    return ($kept -join "`n")
}

# Get-FileListing turns a file or directory into a stable "path|size|mtime" listing. Empty when
# the path is absent, so a run that has not written anything yet hashes the same as before.
function Get-FileListing([string]$Path) {
    if (-not (Test-Path $Path)) { return "" }
    return (@(Get-ChildItem -File -Recurse $Path -ErrorAction SilentlyContinue |
        Sort-Object FullName |
        ForEach-Object { "$($_.FullName)|$($_.Length)|$($_.LastWriteTimeUtc.Ticks)" }) -join "`n")
}

# Get-ProgressHash folds the three progress inputs into one int. Pure and IO-free so the self
# tests can drive it with synthetic strings.
function Get-ProgressHash([string]$GitStatus, [string]$Reports, [string]$Log) {
    return ("$GitStatus`n--reports--`n$Reports`n--log--`n$Log").GetHashCode()
}

# Get-RunningRecordPath picks the run record for the task being watched: the per-task record
# ww-data\running\<Task>.json (card 170), or — when no task is given — the only active
# record in that folder. Falls back to the legacy main-only running.json. Returns $null when
# nothing is running. Pure filesystem lookup, so the self tests can drive it with fixtures.
function Get-RunningRecordPath([string]$DataDir, [string]$Task = $null) {
    $runDir = Join-Path $DataDir "running"
    if ($Task) {
        $perTask = Join-Path $runDir "$Task.json"
        if (Test-Path $perTask) { return $perTask }
    } else {
        $active = @(Get-ChildItem $runDir -Filter "*.json" -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -notlike "*.last.json" })
        if ($active.Count -eq 1) { return $active[0].FullName }
    }
    $legacy = Join-Path $DataDir "running.json"
    if (Test-Path $legacy) { return $legacy }
    return $null
}

# Get-Snapshot captures the raw inputs that can signal progress. Unlike the old Get-Signature
# it does not round the CPU values, so a caller can compare them against a CPU delta threshold
# instead of "did the rounded string change".
#
# The git signal is deliberately NARROW (card 055): `git status --porcelain -- <watch folders>`
# only sees the folders listed in ww-data\config.json. Anything else in the repo (task cards,
# decisions, the user's own edits) changes the working tree but must NOT look like run progress.
# Reports and the current run log are gitignored, so porcelain never shows them even though a
# real run writes them; they are added as explicit file listings (size + mtime) instead. The
# current log comes from the run record in $DataDir (written by ww-run at run start): the
# per-task record when -Task is given, otherwise the sole active per-task record, with
# running.json as the backward-compatible fallback.
function Get-Snapshot([string]$DataDir = $null, [string]$Task = $null) {
    $runs = @(Get-WorkerRuns)
    $ids = if ($runs.Count) { Get-Descendants @($runs.ProcessId) } else { @() }
    $paths = Resolve-WwPaths -Root $root -DataDir $DataDir
    $data = $paths.DataDir
    $porcelain = & git -C $root status --porcelain -- @($paths.Watch) 2>$null | Out-String
    $git = Select-ProgressStatus $porcelain -Watch $paths.Watch -Exclude $paths.RelUrl
    $reports = Get-FileListing (Join-Path $data "reports")
    $log = ""
    $running = Get-RunningRecordPath -DataDir $data -Task $Task
    if ($running) {
        $logPath = $null
        try { $logPath = (Get-Content $running -Raw | ConvertFrom-Json).Log } catch { $logPath = $null }
        if ($logPath) { $log = Get-FileListing $logPath }
    }
    $workerCpu = [double](Get-CpuSeconds $ids)
    return [pscustomobject]@{
        WorkerCpu   = $workerCpu
        OpencodeCpu = $workerCpu
        GitHash     = [int](Get-ProgressHash -GitStatus $git -Reports $reports -Log $log)
    }
}

# Test-Progress is the pure decision: has anything made progress since the baseline snapshot?
# $prev should be the snapshot taken at the LAST observed progress (not the previous poll), so
# a slow CPU trickle accumulates instead of resetting the stall timer.
#
# Signals: the git/report/log hash, and the worker tree's CPU (the agent and everything it
# started, e.g. builds and tests). $minCpuDelta stays small (~2 s): a working agent makes >= 2 s
# of CPU within one poll, while an idle one burns only a fraction of that.
#
# EXPLICIT RULE (card 055-4): a run whose log file is still only its single start line AND whose
# worker-tree CPU grew less than $minCpuDelta over the baseline window is a stall, so the caller's
# stall timer keeps running and the run is flagged.
#
# A CPU DROP means the process tree restarted (worker rebuilt/relaunched, or a descendant
# exited). Treat it as progress and let the caller re-base: comparing a negative delta against
# the threshold would otherwise look like "no progress" and could hide the restart.
function Test-Progress($prev, $now, [double]$minCpuDelta = 2.0) {
    if ($null -eq $prev -or $null -eq $now) { return $true }
    if ($now.GitHash -ne $prev.GitHash) { return $true }
    $prevCpu = if ($null -ne $prev.WorkerCpu) { $prev.WorkerCpu } else { $prev.OpencodeCpu }
    $nowCpu  = if ($null -ne $now.WorkerCpu)  { $now.WorkerCpu }  else { $now.OpencodeCpu }
    if ($nowCpu -lt $prevCpu) { return $true }
    if (($nowCpu - $prevCpu) -ge $minCpuDelta) { return $true }
    return $false
}
