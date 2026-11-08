# ww-watch.selftest.ps1 — unit tests for the pure progress logic in ww-watch.lib.ps1.
# Run: powershell -NoProfile -ExecutionPolicy Bypass -File Tools\ww\ww-watch.selftest.ps1
# Prints PASS/FAIL per case and exits non-zero when any case fails.
# It dot-sources the library only (no real run, no process/git access): Get-Snapshot is
# defined but never called, so the cases exercise Test-Progress directly and deterministically.

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "ww-watch.lib.ps1")

$script:pass = 0
$script:fail = 0
function Check([bool]$cond, [string]$msg) {
    if ($cond) { Write-Host "PASS  $msg" -ForegroundColor Green; $script:pass++ }
    else       { Write-Host "FAIL  $msg" -ForegroundColor Red;   $script:fail++ }
}
function Snap([double]$workerCpu, [int]$gitHash) {
    return [pscustomobject]@{ WorkerCpu = $workerCpu; OpencodeCpu = $workerCpu; GitHash = $gitHash }
}

$base = Snap 10.0 111

Write-Host "`n== Select-ProgressStatus / Get-ProgressHash ==" -ForegroundColor Cyan

# The git signal must ignore everything outside the watch folders (config.json "watch"; card
# 055-1): a task-card edit is the user's own work, not run progress.
$watch = @("src", "Tools")
$porcelain = " M ww-data/tasks/055-foo.md`n?? ww-data/decisions/024.md`n M src/Foo.cs`n?? Tools/ww/ww-watch.ps1`n?? scratch/tmp.txt"
$kept = Select-ProgressStatus $porcelain -Watch $watch
Check ($kept -notmatch 'tasks|decisions|scratch') "task/decision/scratch edits are filtered out"
Check ($kept -match 'src/Foo\.cs') "watched src edit is kept"
Check ($kept -match 'Tools/ww/ww-watch\.ps1') "watched Tools edit is kept"
Check ([string]::IsNullOrEmpty((Select-ProgressStatus " M ww-data/tasks/055-foo.md" -Watch $watch))) "tasks-only status filters to empty"

# No watch list: everything except the data folder counts.
$keptAll = Select-ProgressStatus $porcelain
Check ($keptAll -notmatch 'ww-data/') "no watch list: data folder edits are filtered out"
Check ($keptAll -match 'scratch/tmp\.txt' -and $keptAll -match 'src/Foo\.cs') "no watch list: other edits are kept"

$gitSame = Select-ProgressStatus " M src/Foo.cs" -Watch $watch
$gitTaskEdit = Select-ProgressStatus " M src/Foo.cs`n M ww-data/tasks/055-foo.md" -Watch $watch
$hashSame = Get-ProgressHash -GitStatus $gitSame -Reports "r|1|1" -Log ""
$hashTask = Get-ProgressHash -GitStatus $gitTaskEdit -Reports "r|1|1" -Log ""
Check ($hashSame -eq $hashTask) "a task-card edit does not change the progress hash (no progress)"

$hashAssets = Get-ProgressHash -GitStatus (Select-ProgressStatus " M src/Foo.cs`n M src/Bar.cs" -Watch $watch) -Reports "r|1|1" -Log ""
Check ($hashAssets -ne $hashSame) "a watched-folder edit changes the progress hash"
$hashReport = Get-ProgressHash -GitStatus $gitSame -Reports "r|1|2" -Log ""
Check ($hashReport -ne $hashSame) "a report file change changes the progress hash"

Write-Host "`n== Test-Progress ==" -ForegroundColor Cyan

# 1. git hash change = progress
Check (Test-Progress -prev $base -now (Snap 10.0 222)) "git hash change = progress"

# 1b. a task-card-only edit does not change the hash, so it is not progress
Check (-not (Test-Progress -prev (Snap 10.0 $hashSame) -now (Snap 10.0 $hashTask))) "task-card edit = no progress"
Check (Test-Progress -prev (Snap 10.0 $hashSame) -now (Snap 10.0 $hashAssets)) "watched-folder edit = progress"
Check (Test-Progress -prev (Snap 10.0 $hashSame) -now (Snap 10.0 $hashReport)) "report file change = progress"

# 2. a sub-threshold CPU increase = no progress
Check (-not (Test-Progress -prev $base -now (Snap 10.1 111))) "+0.1s worker CPU = no progress"

# 3. a full threshold increase = progress
Check (Test-Progress -prev $base -now (Snap 12.0 111)) "+2.0s worker CPU = progress"

# 3b. an explicit rule case: start-only log and worker CPU below delta -> no progress (stall).
Check (-not (Test-Progress -prev $base -now (Snap 10.5 111))) "start-only log + worker +0.5s = no progress"

# 4. accumulated trickle against the same baseline: 0.1s/poll, progress exactly on the 20th
$belowThreshold = $true
for ($i = 1; $i -le 19; $i++) {
    if (Test-Progress -prev $base -now (Snap (10.0 + 0.1 * $i) 111)) { $belowThreshold = $false }
}
Check $belowThreshold "accumulated trickle is not progress for polls 1..19"
Check (Test-Progress -prev $base -now (Snap (10.0 + 0.1 * 20) 111)) "accumulated trickle = progress exactly at the 20th poll"

# 5. a CPU drop (restart) = progress, never a negative delta treated as no-progress
Check (Test-Progress -prev (Snap 20.0 111) -now (Snap 2.0 111)) "negative delta (restart) = progress"

# 5b. old snapshots that only carry OpencodeCpu still compare
Check (Test-Progress -prev ([pscustomobject]@{ OpencodeCpu = 10.0; GitHash = 111 }) -now ([pscustomobject]@{ OpencodeCpu = 12.0; GitHash = 111 })) "OpencodeCpu fallback still works"

# 6. unchanged = stall (no progress)
Check (-not (Test-Progress -prev $base -now (Snap 10.0 111))) "unchanged snapshot = no progress"

Write-Host "`n== summary ==" -ForegroundColor Cyan
Write-Host "pass=$($script:pass) fail=$($script:fail)"
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
