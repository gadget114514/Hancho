# ww-status.ps1 — is the worker (agy, opencode, claude or codex) working? what changed? what is waiting for me?
# Usage:  Tools\ww\ww-status.cmd          (once)
#         Tools\ww\ww-status.cmd -Watch   (refresh every 10 s, Ctrl+C to stop)
param([switch]$Watch, [string]$Root = $env:WW_ROOT)

. (Join-Path $PSScriptRoot "ww-run.lib.ps1")
try { $paths = Resolve-WwPaths -Root $Root }
catch { Write-Host "[ww-status] $($_.Exception.Message)" -ForegroundColor Red; exit 1 }
$root = $paths.Root
$data = $paths.DataDir
# Folders scanned for "files changed": the config.json watch list plus the data folder.
$scanDirs = @(@($paths.Watch | ForEach-Object { Join-Path $root $_ }) + $data)

function Get-RunRecords {
    $records = New-Object System.Collections.Generic.List[object]
    $runDir = Join-Path $data "running"
    if (Test-Path $runDir) {
        Get-ChildItem $runDir -Filter "*.json" -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -notlike "*.last.json" } |
            ForEach-Object {
                try { $records.Add((Get-Content $_.FullName -Raw | ConvertFrom-Json)) } catch {}
            }
    }
    $legacyFile = Join-Path $data "running.json"
    if (Test-Path $legacyFile) {
        try {
            $legacy = Get-Content $legacyFile -Raw | ConvertFrom-Json
            if (-not (@($records) | Where-Object { $_.Task -eq $legacy.Task })) { $records.Add($legacy) }
        } catch {}
    }
    return $records
}

function Get-RecordCardPath($Record) {
    if (-not $Record -or -not $Record.Card) { return $null }
    if ($Record.Worktree) {
        $copied = Join-Path (Join-Path (Join-Path $Record.Worktree $paths.RelData) "tasks") $Record.Card
        if (Test-Path -LiteralPath $copied) { return $copied }
    }
    return (Join-Path (Join-Path $data "tasks") $Record.Card)
}

# Progress log view (card 190): last 5 readable steps + age of the newest line.
function Show-Progress($Record, [string]$Indent = "           ") {
    if (-not $Record -or -not $Record.Progress) { return $false }
    $info = Get-ProgressInfo -Path $Record.Progress -Tail 5
    if (-not $info) { return $false }
    Write-Host ("{0}progress: {1}" -f $Indent, $info.Path)
    foreach ($line in $info.Lines) { Write-Host ("  {0}" -f $line) }
    Write-Host ("{0}last step {1} min ago" -f $Indent, $info.AgeMinutes)
    return $true
}

function Show-Status {
    Clear-Host
    Write-Host ("=== ww status  {0} ===" -f (Get-Date -Format "HH:mm:ss")) -ForegroundColor Cyan

    # worker processes (agy, opencode, claude or codex). Worktree and -Shared runs are expected to coexist with other
    # runs, so only a second plain main-checkout run is a duplicate warning.
    $procs = @(Get-WorkerRuns)
    # Auto-clean dead run records on every refresh: host gone and no worker names the card.
    $null = Clear-DeadRunRecords -DataDir $data -ScriptName "ww-status" -Runs $procs
    $records = @(Get-RunRecords)

    if (-not $procs) {
        Write-Host "worker   : NOT RUNNING" -ForegroundColor Yellow
    } else {
        $mainProcs = @($procs | Where-Object { -not ($_.CommandLine -match 'WORKTREE RUN|SHARED RUN' -or $_.CommandLine -match '\.claude[\\/]worktrees[\\/]') })
        if ($mainProcs.Count -gt 1) { Write-Host "worker   : WARNING $($mainProcs.Count) main runs at once (duplicate?)" -ForegroundColor Red }
        foreach ($p in $procs) {
            $mode = if ($p.CommandLine -match 'WORKTREE RUN' -or $p.CommandLine -match '\.claude[\\/]worktrees[\\/]') { "worktree" }
                    elseif ($p.CommandLine -match 'SHARED RUN') { "shared" } else { "main" }
            $gp = Get-Process -Id $p.ProcessId -ErrorAction SilentlyContinue
            $elapsed = if ($gp) { [math]::Round(((Get-Date) - $gp.StartTime).TotalMinutes, 1) } else { "?" }
            $cpu = if ($gp) { [math]::Round($gp.CPU, 0) } else { "?" }
            $task = if ($p.CommandLine -match 'tasks[\\/](\d+)') { $Matches[1] } else { "?" }
            $engineLabel = Get-WorkerEngine $p
            Write-Host ("worker ({0}) : RUNNING  PID {1}  task {2}  {3}  {4} min  CPU {5}s" -f $engineLabel, $p.ProcessId, $task, $mode, $elapsed, $cpu) -ForegroundColor Green

            $record = $records | Where-Object { [string]$_.Task -eq [string]$task } | Select-Object -First 1
            if (-not (Show-Progress -Record $record)) {
                # No progress log yet: fall back to the CPU delta / last-file-written guess.
                if ($gp) {
                    $c0 = $gp.CPU; Start-Sleep -Seconds 3; $gp.Refresh(); $busy = [math]::Round($gp.CPU - $c0, 2)
                    $state = if ($busy -gt 0.05) { "working (CPU +$busy s in 3 s)" } else { "waiting (model reply / tests)" }
                    Write-Host "           activity: $state"
                }
                $last = Get-ChildItem -Path $scanDirs -Recurse -File -ErrorAction SilentlyContinue |
                    Where-Object { $_.Name -notlike "frame_*" -and $_.Extension -ne ".meta" -and $_.FullName -notmatch '\\logs\\' } |
                    Sort-Object LastWriteTime -Descending | Select-Object -First 1
                if ($last) {
                    $ago = [math]::Round(((Get-Date) - $last.LastWriteTime).TotalMinutes, 1)
                    $warn = if ($ago -gt 20) { "  <- no file written for a long time; check the worker terminal" } else { "" }
                    Write-Host ("           last file written {0} min ago: {1}{2}" -f $ago, $last.FullName.Substring($root.Length + 1), $warn)
                }
            }
        }
    }

    # Every active run record: ww-data\running\<Task>.json (one per run), with the legacy
    # main-only running.json as a fallback for older runs.
    if ($records.Count -gt 0) {
        foreach ($r in $records) {
            $mode = if ($r.Worktree) { "worktree $($r.Worktree)" }
                    elseif ($r.Shared) { "shared (edits: $(@($r.Edits) -join ', '))" } else { "main" }
            $started = $null
            try { $started = [datetime]::Parse($r.Started) } catch {}
            $mins = if ($started) { [math]::Round(((Get-Date) - $started).TotalMinutes, 1) } else { "?" }
            $estimate = $null
            $cardPath = Get-RecordCardPath $r
            if ($cardPath) { $estimate = Get-EstimateMinutes -CardPath $cardPath }
            $estText = if ($null -ne $estimate) { ", estimate $estimate min" } else { "" }
            Write-Host ("--- run: task {0}  {1}  started {2} ({3} min{4})  log {5}" -f $r.Task, $mode, $r.Started, $mins, $estText, $r.Log)
            if ($null -ne $estimate -and $started -and (((Get-Date) - $started).TotalMinutes -ge $estimate)) {
                Write-Host "           OVER ESTIMATE (stop rule)" -ForegroundColor Red
            }
            if (-not (Show-Progress -Record $r)) {
                if ($r.Log -and (Test-Path $r.Log)) {
                    Get-Content $r.Log -Tail 8 | ForEach-Object { Write-Host "  $_" }
                }
            }
        }
    } elseif (Test-Path (Join-Path $data "last_run.json")) {
        $l = Get-Content (Join-Path $data "last_run.json") -Raw | ConvertFrom-Json
        Write-Host ("last run : task {0} ended {1} ({2} min, exit {3}) reports: {4}" -f $l.Task, $l.Ended, $l.Minutes, $l.ExitCode, ($l.ReportsWritten -join ', '))
    }

    # recently changed files (proof of activity)
    Write-Host "--- files changed in the last 10 min"
    $since = (Get-Date).AddMinutes(-10)
    Get-ChildItem -Path $scanDirs -Recurse -File -ErrorAction SilentlyContinue |
        Where-Object { $_.LastWriteTime -ge $since -and $_.Extension -ne ".meta" -and $_.Name -notlike "frame_*" } |
        Sort-Object LastWriteTime -Descending | Select-Object -First 8 |
        ForEach-Object { Write-Host ("  {0}  {1}" -f $_.LastWriteTime.ToString("HH:mm:ss"), $_.FullName.Substring($root.Length + 1)) }

    # git
    Push-Location $root
    Write-Host ("--- git: branch {0}" -f (git branch --show-current))
    git log --oneline -3 | ForEach-Object { Write-Host "  $_" }
    Pop-Location

    # waiting for the human
    $pending = Get-ChildItem (Join-Path $data "patches\pending") -File -ErrorAction SilentlyContinue | ForEach-Object Name
    $open = Get-ChildItem (Join-Path $data "decisions") -Filter "*.md" -ErrorAction SilentlyContinue | Where-Object { (Select-String -Path $_.FullName -Pattern '^status: open' -Quiet) } | ForEach-Object Name
    Write-Host "--- waiting for you"
    Write-Host ("  patches pending : {0}" -f ($(if ($pending) { $pending -join ', ' } else { 'none' })))
    Write-Host ("  open decisions  : {0}" -f ($(if ($open) { $open -join ', ' } else { 'none' })))
    $latest = Get-ChildItem (Join-Path $data "reports") -Filter "phase-*.md" -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($latest) { Write-Host ("  latest report   : {0} ({1})" -f $latest.Name, $latest.LastWriteTime.ToString("MM-dd HH:mm")) }
}

if ($Watch) { while ($true) { Show-Status; Start-Sleep -Seconds 10 } } else { Show-Status }
