# ww-watch.ps1 — run a task card with stall detection and the split-first retry policy.
# Usage: Tools\ww\ww-watch.cmd 038-2   [-StallMinutes 8] [-PollSeconds 30] [-MaxRetries 2] [-MinCpuDelta 2.0] [-DryRun]
# Exit: 0 ok | 1 pre-flight/ambiguous card | 2 other failure | 3 human decision required | 4 split required
param(
    [Parameter(Mandatory = $true)][string]$Task,
    [double]$StallMinutes = 8,
    [int]$PollSeconds = 30,
    [int]$MaxRetries = 2,
    [double]$MinCpuDelta = 2.0,
    [ValidateSet("agy", "opencode", "claude", "codex")][string]$Engine = $(if ($env:WW_ENGINE) { $env:WW_ENGINE } else { "agy" }),
    [string]$Model = $env:WW_MODEL,
    [string]$Agent = $env:WW_AGENT,
    [string]$Root = $env:WW_ROOT,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "ww-watch.lib.ps1")   # also loads ww-run.lib.ps1
# HOOK: tests redirect ww-data/tasks and the run script (WW_DATA_DIR is honoured by Resolve-WwPaths).
try { $paths = Resolve-WwPaths -Root $Root }
catch { Write-Host "[ww-watch] $($_.Exception.Message)" -ForegroundColor Red; exit 1 }
$root = $paths.Root
$data = $paths.DataDir
$tasks = if ($env:WW_TASKS_DIR) { $env:WW_TASKS_DIR } else { Join-Path $data "tasks" }
$runScript = if ($env:WW_RUN_SCRIPT) { $env:WW_RUN_SCRIPT } else { Join-Path $PSScriptRoot "ww-run.ps1" }
$stateFile = Join-Path $data "stall-state.json"
$reportDir = Join-Path $data "reports"
Set-Location $root

# ---------- state (retry counters survive across invocations) ----------
function Load-State {
    $h = @{}
    if (Test-Path $stateFile) {
        $o = Get-Content $stateFile -Raw | ConvertFrom-Json
        foreach ($p in $o.PSObject.Properties) { $h[$p.Name] = @{ retries = [int]$p.Value.retries; stalls = [int]$p.Value.stalls } }
    }
    return $h
}
function Save-State($h) { $h | ConvertTo-Json -Depth 4 | Set-Content -Encoding UTF8 $stateFile }

# ---------- card helpers ----------
function Get-OwnCards([string]$id) {   # the card itself (not its sub-cards)
    $e = [regex]::Escape($id)
    @(Get-ChildItem $tasks -Filter "$id-*.md" | Where-Object { $_.Name -match "^$e-(?!\d+-)" })
}
function Get-ChildCards([string]$id) { # NNN-<n>-*.md
    $e = [regex]::Escape($id)
    @(Get-ChildItem $tasks -Filter "$id-*.md" | Where-Object { $_.Name -match "^$e-\d+-" } | Sort-Object Name)
}
function Test-Unsplittable($card) {
    return [bool](Select-String -Path $card.FullName -Pattern '^\s*[-*]?\s*Splittable:\s*no\b' -Quiet)
}

# ---------- process / progress helpers ----------
# Get-OpencodeRuns / Get-Descendants / Get-CpuSeconds / Get-Snapshot / Test-Progress live in
# ww-watch.lib.ps1 (dot-sourced above) so the pure progress logic can be unit-tested.
function Stop-Runs {
    foreach ($r in @(Get-WorkerRuns)) { & taskkill /PID $r.ProcessId /T /F | Out-Null }
}
function Write-Report([string]$id, [string]$kind, [string]$body) {
    New-Item -ItemType Directory -Force $reportDir | Out-Null
    $path = Join-Path $reportDir "stall-$id.md"
    "# Stall report ($id): $kind`n`n$body" | Set-Content -Encoding UTF8 $path
    Write-Host "[ww-watch] $kind -> $path" -ForegroundColor Red
}

# ---------- run one leaf card with the retry policy ----------
function Invoke-Leaf([string]$id, $card) {
    if ($DryRun) {
        Write-Host "[ww-watch] DRYRUN leaf $id ($($card.Name)) splittable=$(-not (Test-Unsplittable $card))"
        return 0
    }
    $state = Load-State
    if (-not $state.ContainsKey($id)) { $state[$id] = @{ retries = 0; stalls = 0 } }

    while ($true) {
        $extra = ""
        if ($state[$id].retries -gt 0) {
            $extra = "A previous run of this card stalled and was killed (re-run $($state[$id].retries) of $MaxRetries). Run git status first, keep valid partial work, and continue; do not redo finished steps."
        }
        Write-Host "[ww-watch] card $id start (re-runs used: $($state[$id].retries)/$MaxRetries) $(Get-Date -Format HH:mm:ss)" -ForegroundColor Cyan

        $env:WW_ROOT = $root
        if ($Engine) { $env:WW_ENGINE = $Engine }
        if ($Model) { $env:WW_MODEL = $Model }
        if ($Agent) { $env:WW_AGENT = $Agent }
        $argList = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $runScript, $id)
        if ($extra) { $argList += "`"$extra`"" }
        $proc = Start-Process -FilePath "powershell" -ArgumentList $argList -PassThru -NoNewWindow
        # WHY (deviates from the reference): on Windows PowerShell 5.1, Start-Process -NoNewWindow
        # -PassThru leaves .ExitCode $null even after the child exits, so the success path below
        # would never read 0 and never clear the state entry. Enabling events makes it readable.
        $proc.EnableRaisingEvents = $true

        # Baseline = the snapshot at the LAST observed progress, not the previous poll. When a
        # poll shows no progress we deliberately do NOT move it, so a tiny CPU trickle
        # accumulates; only a real worker-tree CPU delta (>= MinCpuDelta) or a git change counts.
        # Counting the child process's pids lets a restart (worker relaunched) reset the
        # baseline instead of feeding a negative CPU delta into Test-Progress.
        #
        # EXPLICIT RULE (card 055-4): a run that has only written its single start log line and
        # whose worker tree grew less than MinCpuDelta is a stall.
        $base = Get-Snapshot -DataDir $data; $basePids = @(@(Get-WorkerRuns).ProcessId | Sort-Object)
        $lastChange = Get-Date; $stalled = $false
        while (-not $proc.HasExited) {
            Start-Sleep -Seconds $PollSeconds
            $now = Get-Snapshot -DataDir $data
            $nowPids = @(@(Get-WorkerRuns).ProcessId | Sort-Object)
            $restarted = ("$nowPids" -ne "$basePids")
            if ($restarted -or (Test-Progress -prev $base -now $now -minCpuDelta $MinCpuDelta)) {
                $base = $now; $basePids = $nowPids; $lastChange = Get-Date; continue
            }
            if (((Get-Date) - $lastChange).TotalMinutes -ge $StallMinutes) { $stalled = $true; break }
        }

        if (-not $stalled) {
            $code = $proc.ExitCode
            if ($code -eq 0) { $state.Remove($id) | Out-Null; Save-State $state }
            return $code
        }

        # ----- stalled -----
        Stop-Runs
        # Give ww-run time to end by itself: it stops the engine's leftover children and writes its
        # last record in its finally block. taskkill /F skips finally, so it is only the last resort.
        if (-not $proc.HasExited) { $proc.WaitForExit(30000) | Out-Null }
        if (-not $proc.HasExited) { & taskkill /PID $proc.Id /T /F | Out-Null }
        $state[$id].stalls++
        Save-State $state

        if (-not (Test-Unsplittable $card)) {
            # Rule 1: split first. No re-run until the card is decomposed (or marked 'Splittable: no').
            Write-Report $id "SPLIT_REQUIRED" ("worker stalled for >= $StallMinutes min on a card that is not marked unsplittable.`n`n" +
                "Claude: decompose this card into sub-cards ($id-1-*.md, $id-2-*.md, ...) and make this card an index, " +
                "or add the line 'Splittable: no' to the card if it cannot be split further. Then re-run ww-watch.")
            return 4
        }
        if ($state[$id].retries -ge $MaxRetries) {
            # Rule 2: unsplittable cards are re-run at most $MaxRetries times, then a human decides.
            Write-Report $id "HUMAN_DECISION_REQUIRED" ("The card cannot be split further and stalled after $MaxRetries re-runs (total stalls: $($state[$id].stalls)).`n`n" +
                "Check 'git status' for partial work, then decide: rewrite the card, run it by hand, or drop it.")
            return 3
        }
        $state[$id].retries++
        Save-State $state
        Start-Sleep -Seconds 5
    }
}

# ---------- expand index cards into sub-cards, depth first ----------
function Invoke-Card([string]$id) {
    $own = @(Get-OwnCards $id)
    if ($own.Count -ne 1) {
        Write-Host "[ww-watch] card '$id': expected exactly 1 matching file, found $($own.Count): $($own.Name -join ', ')" -ForegroundColor Red
        return 1
    }
    $children = @(Get-ChildCards $id)
    if ($children.Count -gt 0) {
        $seen = @{}
        foreach ($c in $children) {
            if ($c.Name -notmatch "^($([regex]::Escape($id))-\d+)-") { continue }
            $childId = $Matches[1]
            if ($seen.ContainsKey($childId)) { continue }
            $seen[$childId] = $true
            $code = Invoke-Card $childId
            if ($code -ne 0) { return $code }
        }
        return 0
    }
    return (Invoke-Leaf $id $own[0])
}

# ---------- pre-flight ----------
if (-not $DryRun) {
    if (Get-WorkerRuns) { Write-Host "[ww-watch] worker run already active. Not starting." -ForegroundColor Red; exit 1 }
    $resident = Get-CimInstance Win32_Process -Filter "Name='opencode.exe' or Name='agy.exe'" -ErrorAction SilentlyContinue |
        Where-Object { $_.CommandLine -notmatch '\srun\s' -and $_.CommandLine -notmatch '\s-p\s' }
    if ($resident) { Write-Host "[ww-watch] WARNING: resident worker processes: $($resident.ProcessId -join ', ')" -ForegroundColor Yellow }
}

exit (Invoke-Card $Task)
