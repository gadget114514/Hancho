# run-loadtest.ps1 - reproduce the 403-job dashboard load benchmark (card 102-9).
# Creates a fresh data directory with triage disabled, starts Hancho.exe on it,
# queues 400 fast jobs (every 13th fails) plus 3 thirty-minute ping jobs, lets
# the queue settle, then measures HanchoDashboard.exe CPU time for 60 seconds.
# HANCHO_PERF_OUT is set for the Hancho process tree only; the dashboard writes
# per-tick and per-IPC timing lines to that file. Only processes started by this
# script are stopped; an existing dashboard is never touched.
#
# Usage (from the checkout root):
#   powershell -ExecutionPolicy Bypass -File tools\loadtest\run-loadtest.ps1
#   tools\loadtest\run-loadtest.ps1 -HanchoExe C:\path\Hancho.exe -CliExe C:\path\hancho-cli.exe
#   tools\loadtest\run-loadtest.ps1 -DurationSec 60 -SettleSec 20 -KeepDataDir
param(
    [string]$HanchoExe = $env:HANCHO_EXE,
    [string]$CliExe = $env:HANCHO_CLI,
    [string]$Root = "",
    [int]$FastJobs = 400,
    [int]$FailEvery = 13,
    [int]$LongJobs = 3,
    [int]$Concurrency = 16,
    [int]$SettleSec = 20,
    [int]$DurationSec = 60,
    [int]$ReadyTimeoutSec = 60,
    [string]$DataDir = "",
    [switch]$KeepDataDir,
    [switch]$Help
)

$ErrorActionPreference = "Stop"

if ($Help) {
    @"
run-loadtest - 403-job dashboard load benchmark (card 102-9).

USAGE
  run-loadtest.ps1 [-HanchoExe <path>] [-CliExe <path>] [-Root <checkout>]
                   [-FastJobs 400] [-FailEvery 13] [-LongJobs 3] [-Concurrency 16]
                   [-SettleSec 20] [-DurationSec 60] [-DataDir <dir>] [-KeepDataDir]

By default the Release build in <checkout>\build\Release is used. A fresh temp
data directory is created unless -DataDir is given (it must then be empty).
config.json is seeded with refresh_interval_ms=2000, triage_mode=off and
triage_llm_mode=manual, so no real LLM is called for the planned failures.
HANCHO_PERF_OUT points at <data-dir>\dashboard-perf.csv. Only the processes this
script starts are stopped at the end; -KeepDataDir keeps the data for inspection.
"@ | Write-Output
    exit 0
}

if (-not $Root) { $Root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path }
if (-not $HanchoExe) { $HanchoExe = Join-Path $Root "build\Release\Hancho.exe" }
if (-not $CliExe) { $CliExe = Join-Path $Root "build\Release\hancho-cli.exe" }
foreach ($exe in @($HanchoExe, $CliExe)) {
    if (-not (Test-Path -LiteralPath $exe -PathType Leaf)) {
        throw "Executable not found: $exe (build the Release configuration first, or pass -HanchoExe / -CliExe)"
    }
}

if (-not $DataDir) {
    $DataDir = Join-Path ([System.IO.Path]::GetTempPath()) ("hancho-loadtest-" + [guid]::NewGuid().ToString("N"))
}
if (-not (Test-Path -LiteralPath $DataDir)) {
    New-Item -ItemType Directory -Path $DataDir -Force | Out-Null
}
if ((Get-ChildItem -LiteralPath $DataDir -Force | Measure-Object).Count -gt 0) {
    throw "Data directory is not empty: $DataDir"
}

$utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$config = [ordered]@{
    refresh_interval_ms = 2000
    triage_mode = "off"
    triage_llm_mode = "manual"
} | ConvertTo-Json
[System.IO.File]::WriteAllText((Join-Path $DataDir "config.json"), $config, $utf8NoBom)

$perfPath = Join-Path $DataDir "dashboard-perf.csv"
$jobsPath = Join-Path $DataDir "loadtest-jobs.json"
$jobs = New-Object 'System.Collections.Generic.List[object]'
for ($i = 1; $i -le $FastJobs; $i++) {
    $exitCode = if ($FailEvery -gt 0 -and ($i % $FailEvery) -eq 0) { 1 } else { 0 }
    $jobs.Add([ordered]@{
        id = ("lt-fast-{0:d4}" -f $i)
        command = "cmd /c exit $exitCode"
        summary = ("loadtest fast {0:d4} (exit {1})" -f $i, $exitCode)
        timeoutSec = 30
    })
}
for ($i = 1; $i -le $LongJobs; $i++) {
    $jobs.Add([ordered]@{
        id = ("lt-long-{0}" -f $i)
        command = "ping -n 1800 127.0.0.1"
        summary = ("loadtest long {0}" -f $i)
        timeoutSec = 3600
    })
}
$jobsFile = [ordered]@{ taskqueue = "loadtest"; concurrency = $Concurrency; jobs = $jobs.ToArray() } | ConvertTo-Json -Depth 6
[System.IO.File]::WriteAllText($jobsPath, $jobsFile, $utf8NoBom)
$expected = $FastJobs + $LongJobs
Write-Host "[loadtest] data dir: $DataDir"
Write-Host "[loadtest] jobs: $expected ($FastJobs fast, every $FailEvery-th exits 1; $LongJobs long)"
Write-Host "[loadtest] perf output: $perfPath"

function Get-ManagedProcesses {
    $result = @()
    $all = Get-CimInstance Win32_Process -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -match '^(Hancho|HanchoDashboard|HanchoCore|ping)\.exe$' }
    foreach ($p in $all) {
        $cl = [string]$p.CommandLine
        if ($p.Name -ieq 'ping.exe') {
            if ($cl -like '*-n 1800 127.0.0.1*' -and $p.CreationDate -ge $script:startedAt) { $result += $p }
        } elseif ($p.ProcessId -eq $script:guiPid -or $cl -like "*$DataDir*") {
            $result += $p
        }
    }
    return $result
}

$script:startedAt = Get-Date
$script:guiPid = 0
$guiProc = $null
$previousPerf = $env:HANCHO_PERF_OUT
try {
    $env:HANCHO_PERF_OUT = $perfPath
    $argLine = '--data-dir "' + $DataDir + '"'
    $guiProc = Start-Process -FilePath $HanchoExe -ArgumentList $argLine -PassThru
    $script:guiPid = $guiProc.Id
    Write-Host "[loadtest] started Hancho.exe pid $($script:guiPid)"
} finally {
    if ($null -eq $previousPerf) { Remove-Item Env:HANCHO_PERF_OUT -ErrorAction SilentlyContinue }
    else { $env:HANCHO_PERF_OUT = $previousPerf }
}

try {
    # Wait for the dashboard/core to come up on this data directory.
    $deadline = (Get-Date).AddSeconds($ReadyTimeoutSec)
    $dashPid = 0
    while ((Get-Date) -lt $deadline) {
        foreach ($p in (Get-ManagedProcesses)) {
            if ($p.Name -ieq 'HanchoDashboard.exe') { $dashPid = [int]$p.ProcessId; break }
        }
        if ($dashPid -gt 0) { break }
        Start-Sleep -Milliseconds 500
    }
    if ($dashPid -le 0) { throw "HanchoDashboard.exe for $DataDir did not start" }

    $reply = & $CliExe --data-dir $DataDir taskqueue add --file $jobsPath --taskqueue loadtest --concurrency $Concurrency --json 2>&1
    $replyText = ($reply | Out-String)
    if ($LASTEXITCODE -ne 0 -or $replyText -notmatch '"ok"\s*:\s*true') {
        throw "taskqueue add failed: $replyText"
    }
    Write-Host "[loadtest] queued $expected jobs"

    # The three pings are the last jobs left running once the fast jobs drain.
    $deadline = (Get-Date).AddSeconds($ReadyTimeoutSec)
    $running = -1
    while ((Get-Date) -lt $deadline) {
        $list = & $CliExe --data-dir $DataDir taskqueue list loadtest --state running --json 2>$null
        if ($LASTEXITCODE -eq 0) {
            $parsed = ($list | Out-String) | ConvertFrom-Json -ErrorAction SilentlyContinue
            if ($parsed -and $parsed.jobs) { $running = @($parsed.jobs).Count }
        }
        if ($running -eq $LongJobs) { break }
        Start-Sleep -Seconds 1
    }
    if ($running -ne $LongJobs) {
        Write-Warning "[loadtest] running count is $running (expected $LongJobs); measuring anyway"
    } else {
        Write-Host "[loadtest] running count reached $LongJobs"
    }

    if ($SettleSec -gt 0) { Start-Sleep -Seconds $SettleSec }

    $dash = Get-Process -Id $dashPid -ErrorAction Stop
    $cpuStart = $dash.TotalProcessorTime.TotalSeconds
    $watch = [System.Diagnostics.Stopwatch]::StartNew()
    Start-Sleep -Seconds $DurationSec
    $watch.Stop()
    $dash.Refresh()
    $cpuSeconds = $dash.TotalProcessorTime.TotalSeconds - $cpuStart
    $wallSeconds = $watch.Elapsed.TotalSeconds
    $cpuPercentOneCore = if ($wallSeconds -gt 0) { 100.0 * $cpuSeconds / $wallSeconds } else { 0 }
    $cpuPercentAllCores = $cpuPercentOneCore / [Environment]::ProcessorCount

    Write-Host ""
    Write-Host "RESULT"
    Write-Host ("  jobs: {0} (fast {1}, long {2}, concurrency {3})" -f $expected, $FastJobs, $LongJobs, $Concurrency)
    Write-Host ("  window: {0:N1} s (settle {1} s)" -f $wallSeconds, $SettleSec)
    Write-Host ("  dashboard CPU time: {0:N3} s" -f $cpuSeconds)
    Write-Host ("  dashboard CPU: {0:N1}% of one core, {1:N1}% of {2} cores" -f $cpuPercentOneCore, $cpuPercentAllCores, [Environment]::ProcessorCount)
    Write-Host "  perf log: $perfPath"
    Write-Host "  data dir: $DataDir"
    Write-Host "  baseline (2026-11-24): 63.6% before the fix, 5.9% after (refresh 1.32 s -> 0.16 s)"
    if (-not (Test-Path -LiteralPath $perfPath)) {
        Write-Warning "[loadtest] no perf log: HANCHO_PERF_OUT was not picked up by the dashboard"
    }
} finally {
    # Stop only processes this script started (the Hancho tree and the three pings).
    $managed = Get-ManagedProcesses
    foreach ($p in $managed) {
        Write-Host "[loadtest] stopping $($p.Name) pid $($p.ProcessId)"
        & taskkill.exe /PID $p.ProcessId /T /F 2>&1 | Out-Null
    }
    if ($KeepDataDir) {
        Write-Host "[loadtest] -KeepDataDir: data dir left at $DataDir"
    } else {
        Write-Host "[loadtest] data dir left at $DataDir (remove it by hand)"
    }
}
