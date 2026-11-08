# ww-watch.tests.ps1 — plain-PowerShell tests for Tools\ww\ww-watch.ps1.
# Run:  powershell -NoProfile -ExecutionPolicy Bypass -File Tools\ww\tests\ww-watch.tests.ps1
#
# The stall scenarios must reach "no change for StallMinutes" while the fake run script sits
# idle. The detector's real signals are worker/descendant CPU and `git status`, and on a live
# machine the test runner itself may be an agent run — so the signature would never stay constant (the watcher's own polling CPU alone keeps changing it).
# To make the fake scenarios deterministic without starting a real run, the harness stubs the
# process-enumeration cmdlets (Get-CimInstance / Get-Process) in the child watcher session so
# the CPU and opencode parts of the signature are constant, and it also keeps `Stop-Runs` from
# killing the parent opencode. The `git status` signal is still read for real, so a file change
# during a test can still mask a stall — keep the working tree still while running.

$ErrorActionPreference = "Stop"
$watch = Join-Path (Split-Path -Parent $PSScriptRoot) "ww-watch.ps1"
$base  = Join-Path $env:TEMP "ww-watch-tests"
Remove-Item -Recurse -Force $base -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $base | Out-Null

$script:pass = 0
$script:fail = 0
function Check([bool]$cond, [string]$msg) {
    if ($cond) { Write-Host "PASS  $msg" -ForegroundColor Green; $script:pass++ }
    else       { Write-Host "FAIL  $msg" -ForegroundColor Red;   $script:fail++ }
}

function New-Fixture([string]$name) {
    $dir = Join-Path $base $name
    $tasks = Join-Path $dir "tasks"
    $data  = Join-Path $dir "data"
    New-Item -ItemType Directory -Force $tasks, $data | Out-Null
    return @{ Dir = $dir; Tasks = $tasks; Data = $data }
}

function Invoke-Watch([string]$Task, [string]$ExtraArgs = "") {
    $env:WW_DATA_DIR  = $script:dataDir
    $env:WW_TASKS_DIR = $script:tasksDir
    if ($script:runScript) { $env:WW_RUN_SCRIPT = $script:runScript }
    else { Remove-Item Env:WW_RUN_SCRIPT -ErrorAction SilentlyContinue }
    # `exit` inside a script invoked with `&` from `-Command` only returns to the caller, it does
    # not set the host's exit code, so re-raise it explicitly at the top level of the command.
    $cmd = "function Get-CimInstance { @() }; function Get-Process { @() }; & '$watch' -Task $Task $ExtraArgs; exit `$LASTEXITCODE"
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -Command $cmd 2>&1 | Out-String
    return @{ Code = $LASTEXITCODE; Output = $out }
}

$sleepScript = @'
param([string]$Task = "", [string]$Extra = "")
Add-Content -Path $env:WW_COUNTER -Value "start"
Start-Sleep -Seconds 120
'@
$successScript = @'
param([string]$Task = "", [string]$Extra = "")
Add-Content -Path $env:WW_COUNTER -Value "start"
exit 0
'@

# ---------- 1. -DryRun expands children in order and reports each leaf's status ----------
Write-Host "`n== 1. -DryRun on an index card ==" -ForegroundColor Cyan
$f = New-Fixture "t1-dryrun"
Set-Content (Join-Path $f.Tasks "045-index.md") "# 045 index" -Encoding UTF8
Set-Content (Join-Path $f.Tasks "045-1-alpha.md") "# alpha" -Encoding UTF8
Set-Content (Join-Path $f.Tasks "045-2-beta.md") "# beta`n`n- Splittable: no" -Encoding UTF8
Set-Content (Join-Path $f.Tasks "045-3-gamma.md") "# gamma" -Encoding UTF8
$script:tasksDir = $f.Tasks; $script:dataDir = $f.Data; $script:runScript = $null
$r = Invoke-Watch "045" "-DryRun"
Check ($r.Code -eq 0) "dryrun exits 0"
Check ($r.Output -match "DRYRUN leaf 045-1 \(045-1-alpha\.md\) splittable=True")  "leaf 045-1 splittable=True"
Check ($r.Output -match "DRYRUN leaf 045-2 \(045-2-beta\.md\) splittable=False") "leaf 045-2 splittable=False"
Check ($r.Output -match "DRYRUN leaf 045-3 \(045-3-gamma\.md\) splittable=True") "leaf 045-3 splittable=True"
$i1 = $r.Output.IndexOf("045-1-alpha"); $i2 = $r.Output.IndexOf("045-2-beta"); $i3 = $r.Output.IndexOf("045-3-gamma")
Check ($i1 -ge 0 -and $i1 -lt $i2 -and $i2 -lt $i3) "children expand in name order"

# ---------- 2. ambiguous card id -> exit 1 ----------
Write-Host "`n== 2. ambiguous card id ==" -ForegroundColor Cyan
$f = New-Fixture "t2-ambiguous"
Set-Content (Join-Path $f.Tasks "046-a.md") "# a" -Encoding UTF8
Set-Content (Join-Path $f.Tasks "046-b.md") "# b" -Encoding UTF8
$script:tasksDir = $f.Tasks; $script:dataDir = $f.Data; $script:runScript = $null
$r = Invoke-Watch "046" "-DryRun"
Check ($r.Code -eq 1) "ambiguous id exits 1"
Check ($r.Output -match "expected exactly 1") "ambiguous id reports the count"

# ---------- 3. stall on a splittable leaf -> exit 4, no re-run ----------
Write-Host "`n== 3. stall on a splittable leaf ==" -ForegroundColor Cyan
$f = New-Fixture "t3-split"
Set-Content (Join-Path $f.Tasks "047-leaf.md") "# leaf`n`nno Splittable marker" -Encoding UTF8
Set-Content (Join-Path $f.Dir "fake-sleep.ps1") $sleepScript -Encoding UTF8
$counter = Join-Path $f.Dir "counter.txt"
$env:WW_COUNTER = $counter
$script:tasksDir = $f.Tasks; $script:dataDir = $f.Data; $script:runScript = (Join-Path $f.Dir "fake-sleep.ps1")
$r = Invoke-Watch "047" "-StallMinutes 0.05 -PollSeconds 1"
Check ($r.Code -eq 4) "splittable leaf stall exits 4"
$report = Join-Path $f.Data "reports\stall-047.md"
Check (Test-Path $report) "stall-047.md written"
if (Test-Path $report) {
    Check ((Get-Content $report -Raw) -match "SPLIT_REQUIRED") "report contains SPLIT_REQUIRED"
}
$lines = @(Get-Content $counter -ErrorAction SilentlyContinue)
Check ($lines.Count -eq 1) "no re-run happened (start counter has 1 line)"

# ---------- 4. stall on an unsplittable leaf -> exit 3 after 2 re-runs ----------
Write-Host "`n== 4. stall on an unsplittable leaf ==" -ForegroundColor Cyan
$f = New-Fixture "t4-human"
Set-Content (Join-Path $f.Tasks "048-leaf.md") "# leaf`n`n- Splittable: no" -Encoding UTF8
Set-Content (Join-Path $f.Dir "fake-sleep.ps1") $sleepScript -Encoding UTF8
$counter = Join-Path $f.Dir "counter.txt"
$env:WW_COUNTER = $counter
$script:tasksDir = $f.Tasks; $script:dataDir = $f.Data; $script:runScript = (Join-Path $f.Dir "fake-sleep.ps1")
$r = Invoke-Watch "048" "-StallMinutes 0.05 -PollSeconds 1"
Check ($r.Code -eq 3) "unsplittable leaf stall exits 3"
$report = Join-Path $f.Data "reports\stall-048.md"
if (Test-Path $report) {
    Check ((Get-Content $report -Raw) -match "HUMAN_DECISION_REQUIRED") "report contains HUMAN_DECISION_REQUIRED"
} else { Check $false "stall-048.md written" }
$lines = @(Get-Content $counter -ErrorAction SilentlyContinue)
Check ($lines.Count -eq 3) "exactly 2 re-runs (start counter has 3 lines)"
$statePath = Join-Path $f.Data "stall-state.json"
if (Test-Path $statePath) {
    $st = Get-Content $statePath -Raw | ConvertFrom-Json
    Check ([int]$st.'048'.retries -eq 2) "state file shows retries = 2"
} else { Check $false "stall-state.json written" }

# ---------- 5. success path -> exit 0 and state entry removed ----------
Write-Host "`n== 5. success path ==" -ForegroundColor Cyan
$f = New-Fixture "t5-success"
Set-Content (Join-Path $f.Tasks "049-leaf.md") "# leaf" -Encoding UTF8
Set-Content (Join-Path $f.Dir "fake-success.ps1") $successScript -Encoding UTF8
$counter = Join-Path $f.Dir "counter.txt"
$env:WW_COUNTER = $counter
'{ "049": { "retries": 1, "stalls": 1 } }' | Set-Content (Join-Path $f.Data "stall-state.json") -Encoding UTF8
$script:tasksDir = $f.Tasks; $script:dataDir = $f.Data; $script:runScript = (Join-Path $f.Dir "fake-success.ps1")
$r = Invoke-Watch "049" "-PollSeconds 1"
Check ($r.Code -eq 0) "success exits 0"
$statePath = Join-Path $f.Data "stall-state.json"
$raw = if (Test-Path $statePath) { Get-Content $statePath -Raw } else { "" }
Check (-not ($raw -match '"049"')) "state entry removed on success"

# ---------- summary ----------
Write-Host "`n== summary ==" -ForegroundColor Cyan
Write-Host "pass=$($script:pass) fail=$($script:fail)"
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
