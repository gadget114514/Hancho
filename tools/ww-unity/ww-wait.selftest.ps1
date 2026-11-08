# ww-wait.selftest.ps1 — unit tests for the pure job-state logic in ww-wait.ps1 (card 334).
# Run: powershell -NoProfile -ExecutionPolicy Bypass -File Tools\ww\ww-wait.selftest.ps1
# Prints PASS/FAIL per case and exits non-zero when any case fails.
# It dot-sources ww-wait.ps1 only: the script stops before any hancho-cli call, so no queue is touched.

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "ww-wait.ps1")

$script:pass = 0
$script:fail = 0
function Check([bool]$cond, [string]$msg) {
    if ($cond) { Write-Host "PASS  $msg" -ForegroundColor Green; $script:pass++ }
    else       { Write-Host "FAIL  $msg" -ForegroundColor Red;   $script:fail++ }
}
function Job([string]$id, [string]$state, [string[]]$dependsOn = @()) {
    return [pscustomobject]@{ id = $id; state = $state; depends_on = @($dependsOn | ForEach-Object { [pscustomobject]@{ id = $_; on = "done" } }) }
}
function StatusOf($result, [string]$id) {
    return ($result.Rows | Where-Object { $_.Id -eq $id }).Status
}

Write-Host "`n== Get-WwWaitResult ==" -ForegroundColor Cyan

$r = Get-WwWaitResult -Jobs @((Job "a" "succeeded"), (Job "b" "succeeded")) -Ids @("a", "b")
Check ($r.Done -and -not $r.Failed) "all succeeded: done, not failed"

$r = Get-WwWaitResult -Jobs @((Job "a" "succeeded"), (Job "b" "failed")) -Ids @("a", "b")
Check ($r.Done -and $r.Failed) "one failed: done and failed"

$r = Get-WwWaitResult -Jobs @((Job "a" "cancelled")) -Ids @("a")
Check ($r.Done -and $r.Failed) "cancelled counts as finished and failed"

$r = Get-WwWaitResult -Jobs @((Job "a" "succeeded"), (Job "b" "running")) -Ids @("a", "b")
Check (-not $r.Done) "still running: not done"
Check ((StatusOf $r "b") -eq "running") "still running: status kept as running"

$r = Get-WwWaitResult -Jobs @((Job "a" "running"), (Job "b" "pending" @("a"))) -Ids @("b")
Check (-not $r.Done -and -not $r.Failed) "pending behind running dependency: waits, not never-start"

$r = Get-WwWaitResult -Jobs @((Job "a" "succeeded"), (Job "b" "pending" @("a"))) -Ids @("b")
Check (-not $r.Done) "pending behind succeeded dependency: still waits (queue starts it)"

$r = Get-WwWaitResult -Jobs @((Job "a" "failed"), (Job "b" "pending" @("a"))) -Ids @("b")
Check ($r.Done -and $r.Failed) "pending behind failed dependency: never-start, done and failed"
Check ((StatusOf $r "b") -eq "never-start") "pending behind failed dependency: status never-start"

$r = Get-WwWaitResult -Jobs @((Job "a" "cancelled"), (Job "b" "pending" @("a")), (Job "c" "pending" @("b"))) -Ids @("c")
Check ((StatusOf $r "c") -eq "never-start") "never-start propagates down a pending chain"

$r = Get-WwWaitResult -Jobs @((Job "b" "pending" @("gone"))) -Ids @("b")
Check ((StatusOf $r "b") -eq "never-start" -and $r.Done) "dependency missing from queue: never-start"

$r = Get-WwWaitResult -Jobs @((Job "a" "succeeded")) -Ids @("a", "typo-1")
Check (@($r.Missing) -contains "typo-1") "unknown id is reported in Missing"
Check (-not $r.Done) "unknown id is not done (caller exits 3 at start)"

$r = Get-WwWaitResult -Jobs @((Job "x" "pending" @("y")), (Job "y" "pending" @("x"))) -Ids @("x")
Check ((StatusOf $r "x") -eq "pending") "dependency cycle terminates and stays pending"

Write-Host "`n$script:pass passed, $script:fail failed"
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
