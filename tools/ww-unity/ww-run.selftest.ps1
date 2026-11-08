# ww-run.selftest.ps1 — tests for the worktree run-planning logic in ww-run.lib.ps1 and the
# -DryRun path of ww-run.ps1.
# Run: powershell -NoProfile -ExecutionPolicy Bypass -File Tools\ww\ww-run.selftest.ps1
# Prints PASS/FAIL per case and exits non-zero when any case fails.
# It never starts opencode and never creates a real worktree: -DryRun is a pure plan, and the
# duplicate rule is exercised with synthetic process records.
#
# Fixtures live under <repo>\scratch\ww-run-selftest (gitignored).

$ErrorActionPreference = "Stop"
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
. (Join-Path $PSScriptRoot "ww-run.lib.ps1")
. (Join-Path $PSScriptRoot "ww-watch.lib.ps1")

$base = Join-Path $repoRoot "scratch\ww-run-selftest"
Remove-Item -Recurse -Force $base -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $base | Out-Null

$script:pass = 0
$script:fail = 0
function Check([bool]$cond, [string]$msg) {
    if ($cond) { Write-Host "PASS  $msg" -ForegroundColor Green; $script:pass++ }
    else       { Write-Host "FAIL  $msg" -ForegroundColor Red;   $script:fail++ }
}
function New-FakeRun([int]$procId, [string]$commandLine) {
    return [pscustomobject]@{ ProcessId = $procId; CommandLine = $commandLine }
}

Write-Host "`n== 1. worktree path and branch naming ==" -ForegroundColor Cyan
$plan = Get-WorktreePlan -Root "C:\repo" -Task "901" -Base "dev"
Check ($plan.Path -eq "C:\repo\.claude\worktrees\ww-901") "worktree path is .claude\worktrees\ww-901"
Check ($plan.Branch -eq "ww/901-wt") "branch is ww/901-wt"
Check ($plan.Base -eq "dev") "base ref is kept"
$defaultPlan = Get-WorktreePlan -Root "C:\repo" -Task "902"
Check ($defaultPlan.Base -eq "main") "base defaults to main"

Write-Host "`n== 2. duplicate detection (per task, main vs worktree) ==" -ForegroundColor Cyan
$main901 = New-FakeRun 11 'opencode.cmd run "Execute C:/repo/ww-data/tasks/901-alpha.md. Read AGENTS.md first."'
$main902 = New-FakeRun 12 'opencode.cmd run "Execute C:/repo/ww-data/tasks/902-beta.md. Read AGENTS.md first."'
$wt901 = New-FakeRun 13 'opencode.cmd run "Execute C:/repo/ww-data/tasks/901-alpha.md. WORKTREE RUN: work only inside C:\repo\.claude\worktrees\ww-901."'
$wt902 = New-FakeRun 14 'opencode.cmd run "Execute C:/repo/ww-data/tasks/902-beta.md. WORKTREE RUN: work only inside C:\repo\.claude\worktrees\ww-902."'
$child901 = New-FakeRun 15 'opencode.cmd run "Execute C:/repo/ww-data/tasks/901-1-child.md. WORKTREE RUN: work only inside C:\repo\.claude\worktrees\ww-901-1."'

Check ((Test-RunConflict -Runs @($main901) -Task "901" -Worktree).Blocked) "same task is blocked for a worktree run"
Check ((Test-RunConflict -Runs @($wt901) -Task "901").Blocked) "same task is blocked for a main run"
Check ((Test-RunConflict -Runs @($wt901) -Task "901" -Worktree).Blocked) "same task is blocked for another worktree run"
Check (-not (Test-RunConflict -Runs @($main902) -Task "901" -Worktree).Blocked) "different task is allowed in a worktree"
Check ((Test-RunConflict -Runs @($main902) -Task "901").Blocked) "a second main run is blocked"
Check (-not (Test-RunConflict -Runs @($wt902) -Task "901").Blocked) "a worktree run does not block the main run"
Check (-not (Test-RunConflict -Runs @($wt902) -Task "901" -Worktree).Blocked) "different worktree task is allowed"
Check (-not (Test-RunConflict -Runs @($child901) -Task "901" -Worktree).Blocked) "child card 901-1 is not the same task as 901"
Check (-not (Test-RunConflict -Runs @() -Task "901").Blocked) "no runs = allowed"

$mainAgy901 = New-FakeRun 21 'agy.exe -p "Execute C:/repo/ww-data/tasks/901-alpha.md. Read AGENTS.md first." --output-format stream-json --model gemini-3.8-flash-medium --dangerously-skip-permissions --print-timeout 180m'
$wtAgy902 = New-FakeRun 22 'agy.exe -p "Execute C:/repo/ww-data/tasks/902-beta.md. WORKTREE RUN: work only inside C:\repo\.claude\worktrees\ww-902." --output-format stream-json --model gemini-3.8-flash-medium --dangerously-skip-permissions --print-timeout 180m'
Check ((Test-RunConflict -Runs @($mainAgy901) -Task "901" -Worktree).Blocked) "same task is blocked for an agy worktree run"
Check ((Test-RunConflict -Runs @($mainAgy901) -Task "902").Blocked) "a second main run is blocked by an active agy main run"
Check (-not (Test-RunConflict -Runs @($wtAgy902) -Task "901").Blocked) "an agy worktree run does not block the main run"

Write-Host "`n== 2b. shared runs: declared paths decide (doc/resource-locks.md) ==" -ForegroundColor Cyan
Check (Test-PathPatternOverlap (ConvertTo-WwPattern 'src/') (ConvertTo-WwPattern 'src/a.cpp')) "directory covers its files"
Check (-not (Test-PathPatternOverlap (ConvertTo-WwPattern 'src/a.cpp') (ConvertTo-WwPattern 'src/b.cpp'))) "different files do not overlap"
Check (-not (Test-PathPatternOverlap (ConvertTo-WwPattern 'src/') (ConvertTo-WwPattern 'src2/a.cpp'))) "sibling with common prefix does not overlap"
Check (Test-PathPatternOverlap (ConvertTo-WwPattern 'src/*.cpp') (ConvertTo-WwPattern 'SRC\A.cpp')) "glob matches; case and slashes normalised"
Check (-not (Test-PathPatternOverlap (ConvertTo-WwPattern 'src/*.cpp') (ConvertTo-WwPattern 'src/a.h'))) "glob does not match other extension"
Check (Test-PathPatternOverlap (ConvertTo-WwPattern 'Foo.cs') (ConvertTo-WwPattern 'Assets/X/Foo.cs')) "bare file name matches anywhere"
Check (Test-PathPatternOverlap (ConvertTo-WwPattern 'src/**') (ConvertTo-WwPattern 'src/x/y/z.cpp')) "** covers nested files"
Check ($null -eq (Find-PathConflict -Reads @('src/a.h') -OtherReads @('src/a.h'))) "read x read does not conflict"
Check ($null -ne (Find-PathConflict -Reads @('src/a.h') -OtherEdits @('src/a.h'))) "read x write conflicts"
Check ($null -ne (Find-PathConflict -Edits @('src/a.h') -OtherReads @('src/a.h'))) "write x read conflicts"

$sh901 = New-FakeRun 31 'claude -p "Execute C:/repo/ww-data/tasks/901-alpha.md. Read AGENTS.md first. SHARED RUN: other runs edit this checkout in parallel."'
$records = @{
    '901' = [pscustomobject]@{ Task = '901'; Shared = $true; Edits = @('src/a.cpp'); Reads = @('src/common.h') }
}
Check (-not (Test-RunConflict -Runs @($sh901) -Task "902" -Shared -Edits @('src/b.cpp') -Records $records).Blocked) "shared runs with disjoint edits run together"
Check ((Test-RunConflict -Runs @($sh901) -Task "902" -Shared -Edits @('src/a.cpp') -Records $records).Blocked) "shared runs editing the same file are blocked"
Check ((Test-RunConflict -Runs @($sh901) -Task "902" -Shared -Edits @('src/common.h') -Records $records).Blocked) "editing a file another run reads is blocked"
Check (-not (Test-RunConflict -Runs @($sh901) -Task "902" -Shared -Edits @('src/b.cpp') -Reads @('src/common.h') -Records $records).Blocked) "two runs reading the same file run together"
Check ((Test-RunConflict -Runs @($sh901) -Task "902" -Edits @('src/b.cpp') -Records $records).Blocked) "a plain main run locks the whole checkout"
Check ((Test-RunConflict -Runs @($sh901) -Task "902" -Shared -Records $records).Blocked) "a shared run without Authorised edits locks the whole checkout"
Check ((Test-RunConflict -Runs @($main902) -Task "901" -Shared -Edits @('src/b.cpp') -Records @{}).Blocked) "a run without a record locks the whole checkout"
Check ((Test-RunConflict -Runs @($sh901) -Task "901" -Shared -Edits @('src/zzz.cpp') -Records $records).Blocked) "same card is still refused in shared mode"

$readCard = Join-Path $base "904-card.md"
@'
# 904

## Authorised edits
`src/b.cpp`

## Read-only
`src/common.h`, `doc/**`
'@ | Set-Content -LiteralPath $readCard -Encoding UTF8
$readPaths = @(Get-CardReadPaths -CardPath $readCard)
Check ($readPaths.Count -eq 2 -and $readPaths[0] -eq 'src/common.h') "Read-only section parsed"
Check (@(Get-AuthorisedEditPaths -CardPath $readCard).Count -eq 1) "Read-only paths are not edits"

$eventsFile = Join-Path $base "violations.events.jsonl"
@(
    '{"type":"assistant","message":{"content":[{"type":"tool_use","name":"Edit","input":{"file_path":"C:\\repo\\src\\b.cpp"}}]}}',
    '{"type":"assistant","message":{"content":[{"type":"tool_use","name":"Write","input":{"file_path":"C:\\repo\\src\\other.cpp"}}]}}',
    '{"type":"assistant","message":{"content":[{"type":"tool_use","name":"Read","input":{"file_path":"C:\\repo\\src\\read.cpp"}}]}}',
    '{"type":"tool_use","part":{"tool":"write","state":{"input":{"filePath":"C:/repo/ww-data/reports/904.md"}}}}',
    '{"type":"item.completed","item":{"type":"file_change","changes":[{"path":"C:\\repo\\README.md"}]}}'
) | Set-Content -LiteralPath $eventsFile -Encoding UTF8
$viol = @(Get-EditViolations -EventsFile $eventsFile -WorkDir 'C:\repo' -Edits @('src/b.cpp'))
Check ($viol.Count -eq 2 -and $viol -contains 'src/other.cpp' -and $viol -contains 'readme.md') "edits outside Authorised edits are reported (reads and ww-data are not)"

Write-Host "`n== 3. authorisation parsing and untracked-path pre-flight ==" -ForegroundColor Cyan
$gitRepo = Join-Path $base "gitrepo"
New-Item -ItemType Directory -Force (Join-Path $gitRepo "Assets\Tracked"), (Join-Path $gitRepo "Assets\Untracked") | Out-Null
Set-Content -LiteralPath (Join-Path $gitRepo ".gitignore") -Value "Assets/Untracked/" -Encoding UTF8
Set-Content -LiteralPath (Join-Path $gitRepo "Assets\Tracked\Foo.cs") -Value "// tracked" -Encoding UTF8
Set-Content -LiteralPath (Join-Path $gitRepo "Assets\Untracked\Bar.cs") -Value "// ignored" -Encoding UTF8
& git -C $gitRepo init -q 2>&1 | Out-Null
& git -C $gitRepo add .gitignore Assets/Tracked/Foo.cs 2>&1 | Out-Null

$card = Join-Path $base "902-card.md"
@'
# 902 pre-flight fixture

## Authorised edits
`Assets/Tracked/Foo.cs`, `Assets/Untracked/Bar.cs`. Anything else -> STOP.

## Done when
`Assets/NotAuthorised.cs` is not part of the pre-flight.
'@ | Set-Content -LiteralPath $card -Encoding UTF8

$authPaths = @(Get-AuthorisedEditPaths -CardPath $card)
Check ($authPaths.Count -eq 2) "two backticked paths parsed from Authorised edits"
Check (-not ($authPaths -match 'NotAuthorised')) "paths outside Authorised edits are ignored"

$untracked = @(Get-UntrackedAuthorisedPaths -CardPath $card -RepoRoot $gitRepo)
Check ($untracked.Count -eq 1 -and $untracked[0] -eq "Assets/Untracked/Bar.cs") "untracked/ignored path is reported"
Check (-not ($untracked -match 'Assets/Tracked/Foo\.cs')) "tracked path is accepted"

Write-Host "`n== 4. -DryRun prints a plan and changes nothing ==" -ForegroundColor Cyan
$fixture = Join-Path $base "dryrun"
$data = Join-Path $fixture "ww-data"
$tasks = Join-Path $data "tasks"
New-Item -ItemType Directory -Force $tasks | Out-Null
Set-Content -LiteralPath (Join-Path $tasks "903-dry.md") -Value "# 903 dry`n`n## Authorised edits`nNew files only. Anything else -> STOP." -Encoding UTF8

$oldRoot = $env:WW_ROOT
$oldData = $env:WW_DATA_DIR
$env:WW_ROOT = $fixture
$env:WW_DATA_DIR = $data
try {
    $runScript = Join-Path $PSScriptRoot "ww-run.ps1"
    $out = & powershell -NoProfile -ExecutionPolicy Bypass -File $runScript -Task "903" -Worktree -DryRun 2>&1 | Out-String
    $code = $LASTEXITCODE
} finally {
    $env:WW_ROOT = $oldRoot
    $env:WW_DATA_DIR = $oldData
}
Check ($code -eq 0) "worktree dry-run exits 0"
Check ($out -match 'DRYRUN plan') "dry-run prints the plan"
Check ($out -match 'engine\s+:\s+agy') "dry-run prints engine agy by default"
Check ($out -match 'model\s+:\s+gemini-3.8-flash-medium') "dry-run prints default model for agy"
Check ($out -match 'ww-903') "dry-run prints the worktree path"
Check ($out -match 'ww/903-wt') "dry-run prints the branch"
Check ($out -match 'WOULD REFUSE|pre-flight  : ok') "dry-run reports the pre-flight result"
Check (-not (Test-Path (Join-Path $fixture ".claude\worktrees\ww-903"))) "dry-run creates no worktree"
Check (-not (Test-Path (Join-Path $data "running\903.json"))) "dry-run writes no run record"

$outOpencode = $null
$oldRoot = $env:WW_ROOT
$oldData = $env:WW_DATA_DIR
$env:WW_ROOT = $fixture
$env:WW_DATA_DIR = $data
try {
    $outOpencode = & powershell -NoProfile -ExecutionPolicy Bypass -File $runScript -Task "903" -Engine opencode -DryRun 2>&1 | Out-String
} finally {
    $env:WW_ROOT = $oldRoot
    $env:WW_DATA_DIR = $oldData
}
Check ($outOpencode -match 'engine\s+:\s+opencode') "dry-run prints engine opencode with -Engine opencode"

Write-Host "`n== 4b. -Help, default shared mode and -Jobs ==" -ForegroundColor Cyan
$helpOut = & powershell -NoProfile -ExecutionPolicy Bypass -File $runScript -Help 2>&1 | Out-String
Check ($LASTEXITCODE -eq 0 -and $helpOut -match '-Shared' -and $helpOut -match 'SUPERVISOR LLM') "-Help prints usage with shared mode and supervisor steps"
$noArgOut = & powershell -NoProfile -ExecutionPolicy Bypass -File $runScript 2>&1 | Out-String
Check ($noArgOut -match 'USAGE') "no arguments prints the help"

Set-Content -LiteralPath (Join-Path $tasks "904-a.md") -Value "# 904`n`n## Authorised edits`n- ``src/a.cpp```n- ``Foo.cs```n`n## Read-only`n- ``src/common.h``" -Encoding UTF8
Set-Content -LiteralPath (Join-Path $tasks "905-b.md") -Value "# 905`n`n## Authorised edits`n- ``src/b.cpp```n`n## Read-only`n- ``src/common.h``" -Encoding UTF8
Set-Content -LiteralPath (Join-Path $tasks "906-c.md") -Value "# 906`n`n## Authorised edits`n- ``src/a.cpp``" -Encoding UTF8
$jobsFile = Join-Path $base "jobs.json"
$oldRoot = $env:WW_ROOT
$oldData = $env:WW_DATA_DIR
$env:WW_ROOT = $fixture
$env:WW_DATA_DIR = $data
$oldEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"   # -Jobs notes go to stderr
try {
    $autoOut = & powershell -NoProfile -ExecutionPolicy Bypass -File $runScript -Task "904" -DryRun 2>&1 | Out-String
    $mainOut = & powershell -NoProfile -ExecutionPolicy Bypass -File $runScript -Task "904" -Main -DryRun 2>&1 | Out-String
    $noEditsOut = & powershell -NoProfile -ExecutionPolicy Bypass -File $runScript -Task "903" -DryRun 2>&1 | Out-String
    $stdoutJson = & powershell -NoProfile -ExecutionPolicy Bypass -File $runScript -Jobs "904,905" -Engine claude 2>$null | Out-String
    $jobsErr = & powershell -NoProfile -ExecutionPolicy Bypass -File $runScript -Jobs "904,905,906,903" -Engine claude -Out $jobsFile -Verify "build.cmd" 2>&1 | Out-String
    $jobsCode = $LASTEXITCODE
    $missingOut = & powershell -NoProfile -ExecutionPolicy Bypass -File $runScript -Jobs "999" 2>&1 | Out-String
    $missingCode = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $oldEap
    $env:WW_ROOT = $oldRoot
    $env:WW_DATA_DIR = $oldData
}
Check ($autoOut -match 'mode\s+:\s+shared') "a card with Authorised edits runs shared by default"
Check ($mainOut -match 'mode\s+:\s+main') "-Main forces an exclusive main run"
Check ($noEditsOut -match 'mode\s+:\s+main') "a card without Authorised edits stays main"
$parsed = $stdoutJson | ConvertFrom-Json
Check (@($parsed).Count -eq 2 -and $parsed[0].id -eq 'ww-904') "-Jobs without -Out prints a JSON array of jobs"
Check ($jobsCode -eq 0 -and (Test-Path $jobsFile)) "-Jobs -Out writes the file"
$jobs = @((Get-Content -LiteralPath $jobsFile -Raw | ConvertFrom-Json) | ForEach-Object { $_ })
$j904 = $jobs | Where-Object { $_.id -eq 'ww-904' }
Check ($j904.command -match 'ww-run\.cmd"? 904 -Shared -Engine claude -Root .* -JsonStdout$') "job command runs the card with -Shared"
Check ((@($j904.resources.write) -join ',') -eq 'src/a.cpp,**/foo.cs') "Authorised edits become resources.write (bare names as **/name)"
Check ((@($j904.resources.read) -join ',') -eq 'src/common.h') "Read-only paths become resources.read"
Check ((@(($jobs | Where-Object { $_.id -eq 'ww-903' }).resources.write) -join ',') -eq '**') "a card without Authorised edits writes **"
$verifyJob = $jobs | Where-Object { $_.id -eq 'ww-verify' }
Check ($verifyJob -and $verifyJob.command -eq 'build.cmd' -and @($verifyJob.dependsOn).Count -eq 4 -and (@($verifyJob.resources.write) -join ',') -eq '**') "-Verify adds a whole-checkout job after all cards"
Check ($jobsErr -match '903-dry\.md has no') "cards without Authorised edits are reported"
Check ($jobsErr -match 'cards 904 and 906 overlap') "overlapping cards are reported"
Check (-not ($jobsErr -match 'cards 904 and 905 overlap')) "read/read on the same file is not reported"
Check ($missingCode -eq 1 -and $missingOut -match '999-\*\.md not found') "-Jobs with a missing card exits 1"
Check (-not (Test-PathPatternOverlap '**/foo.cs' 'src/b.cpp')) "a bare file name does not overlap another file"
Check ((Test-PathPatternOverlap '**/foo.cs' 'src') -and (Test-PathPatternOverlap '**/foo.cs' 'src/x/foo.cs') -and (Test-PathPatternOverlap '**/foo.cs' 'foo.cs')) "a bare file name overlaps its directories and itself"
Check ((Test-PathPatternOverlap 'src/**' 'src/a.cpp') -and (Test-PathPatternOverlap '**' 'x/y.cpp') -and -not (Test-PathPatternOverlap 'doc/**' 'src/a.cpp')) "** under a directory covers only that directory"
Check ((Test-PathPatternOverlap 'src/**/*.cpp' 'src/x/a.cpp') -and -not (Test-PathPatternOverlap 'src/**/*.cpp' 'src/x/a.h')) "**/*.cpp matches only .cpp files"
Check ($WwExitRefused -eq 75) "a refused run exits 75 (EX_TEMPFAIL), not 1"
$runSource = Get-Content -LiteralPath (Join-Path $PSScriptRoot "ww-run.ps1") -Raw
Check (([regex]::Matches($runSource, 'exit \$WwExitRefused')).Count -eq 2) "ww-run exits with WwExitRefused on refusal and on a refusing -DryRun"
Check ((Get-ConflictHint -Reason "task 901 overlaps this run's paths (** x **)") -match '-Shared.*-Worktree') "a refused main run is told to use -Shared or -Worktree"
Check ((Get-ConflictHint -Reason "task 901 overlaps this run's paths (src/a.cpp x **)" -Shared -HasEdits $true) -match 'other run locks the whole checkout') "a shared run blocked by a main run is told why"
Check ((Get-ConflictHint -Reason "task 901 overlaps this run's paths (src/a.cpp x src/a.cpp)" -Shared -HasEdits $true) -match 'overlap') "a shared/shared overlap gets the overlap hint"
Check (-not (Get-ConflictHint -Reason "task 901 is already running")) "a second run of the same card gets no mode hint"
$wtJob = New-HanchoCliJob -Task "907" -Root "C:\my proj" -RunCmd "C:\my proj\tools\ww\ww-run.cmd" -Engine agy -Worktree
Check ($wtJob.command -match '^"C:\\my proj\\tools\\ww\\ww-run\.cmd" 907 -Worktree .*-Root "C:\\my proj"' -and -not $wtJob.PSObject.Properties['resources']) "worktree job quotes paths and declares no resources"

Write-Host "`n== 5. run record lookup (ww-watch.lib) ==" -ForegroundColor Cyan
$recData = Join-Path $base "records"
$recRunDir = Join-Path $recData "running"
New-Item -ItemType Directory -Force $recRunDir | Out-Null
Set-Content -LiteralPath (Join-Path $recRunDir "904.json") -Value '{ "Task": "904", "Log": "l" }' -Encoding UTF8
Set-Content -LiteralPath (Join-Path $recRunDir "904.last.json") -Value '{ "Task": "904" }' -Encoding UTF8
Check ((Get-RunningRecordPath -DataDir $recData -Task "904") -eq (Join-Path $recRunDir "904.json")) "per-task record is found by task"
Check ((Get-RunningRecordPath -DataDir $recData) -eq (Join-Path $recRunDir "904.json")) "sole active record is used when no task is given"
Check ($null -eq (Get-RunningRecordPath -DataDir $recData -Task "905")) "missing per-task record falls back to nothing"
Set-Content -LiteralPath (Join-Path $recData "running.json") -Value '{ "Task": "900" }' -Encoding UTF8
Check ((Get-RunningRecordPath -DataDir $recData -Task "905") -eq (Join-Path $recData "running.json")) "legacy running.json is the fallback"

Write-Host "`n== 6. progress event formatting (sample + synthetic) ==" -ForegroundColor Cyan
$sample = Join-Path $PSScriptRoot "samples\opencode-events.sample.jsonl"
$sampleLines = @(Get-Content -LiteralPath $sample)
$bashEvent = @($sampleLines | Where-Object { $_ -match '"tool":"bash"' })[0]
Check ((Format-ProgressEvent -Line $bashEvent) -match '^\d{2}:\d{2}:\d{2}\s+bash\s+Get-Date$') "sample bash event -> bash + short target"
$textEvent = @($sampleLines | Where-Object { $_ -match '"type":"text"' })[0]
Check ((Format-ProgressEvent -Line $textEvent) -match '^\d{2}:\d{2}:\d{2}\s+text\s+DONE$') "sample text event -> text + message"
$stepEvent = @($sampleLines | Where-Object { $_ -match '"type":"step_start"' })[0]
Check ((Format-ProgressEvent -Line $stepEvent) -match '^\d{2}:\d{2}:\d{2}\s+step\s+start$') "step_start -> step start"

$readEvent = @{ type = 'tool_use'; timestamp = 1792491038470; part = @{ tool = 'read'; state = @{ input = @{ filePath = 'src/Foo.cs' }; title = 'Read' } } } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $readEvent) -match '^\d{2}:\d{2}:\d{2}\s+read\s+src/Foo\.cs$') "read event -> file path"
$editEvent = @{ type = 'tool_use'; timestamp = 1792491038470; part = @{ tool = 'edit'; state = @{ input = @{ filePath = 'Tools/ww/ww-run.ps1' } } } } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $editEvent) -match '^\d{2}:\d{2}:\d{2}\s+edit\s+Tools/ww/ww-run\.ps1$') "edit event -> file path"
$longCommand = 'npm test -- ' + ('x' * 200)
$bashLong = @{ type = 'tool_use'; timestamp = 1792491038470; part = @{ tool = 'bash'; state = @{ input = @{ command = $longCommand } } } } | ConvertTo-Json -Compress -Depth 8
$bashLongLine = Format-ProgressEvent -Line $bashLong
$bashTarget = ($bashLongLine -split '  ', 3)[2]
Check ($bashTarget.Length -le 120) "long bash command is shortened to 120 chars"
$longText = 'a' * 200
$textLong = @{ type = 'text'; timestamp = 1792491038470; part = @{ text = $longText } } | ConvertTo-Json -Compress -Depth 8
$textTarget = ((Format-ProgressEvent -Line $textLong) -split '  ', 3)[2]
Check ($textTarget.Length -eq 160) "long assistant text is shortened to 160 chars"
$errorEvent = @{ type = 'error'; timestamp = 1792491038470; part = @{ message = 'boom' } } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $errorEvent) -match '^\d{2}:\d{2}:\d{2}\s+error\s+boom$') "error event -> error + message"
$unknownEvent = @{ type = 'mystery_event'; timestamp = 1792491038470; part = @{} } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $unknownEvent) -match '^\d{2}:\d{2}:\d{2}\s+mystery_event$') "unknown event type -> type only"
Check ((Format-ProgressEvent -Line 'not json at all') -match '^\d{2}:\d{2}:\d{2}\s+raw\s+not json at all$') "non-JSON line -> raw"
Check ($null -eq (Format-ProgressEvent -Line '')) "empty line is skipped"

$agySample = Join-Path $PSScriptRoot "samples\agy-events.sample.jsonl"
$agySampleLines = @(Get-Content -LiteralPath $agySample)
$agyInitEvent = @($agySampleLines | Where-Object { $_ -match '"event":"init"' })[0]
Check ((Format-ProgressEvent -Line $agyInitEvent) -match '^\d{2}:\d{2}:\d{2}\s+init$') "agy sample init -> init"
$agyToolEvent = @($agySampleLines | Where-Object { $_ -match '"step_type":"tool"' -and $_ -match '"state":"ACTIVE"' })[0]
Check ((Format-ProgressEvent -Line $agyToolEvent) -match '^\d{2}:\d{2}:\d{2}\s+read_file\s+.*hello\.txt$') "agy sample read_file -> read_file + target"
$agyTextEvent = @($agySampleLines | Where-Object { $_ -match '"step_type":"agent_response"' -and $_ -match 'Hello f' })[0]
Check ((Format-ProgressEvent -Line $agyTextEvent) -match '^\d{2}:\d{2}:\d{2}\s+text\s+text Hello f$') "agy sample agent_response text_delta -> text + delta"
$agyResultEvent = @($agySampleLines | Where-Object { $_ -match '"event":"result"' })[0]
Check ((Format-ProgressEvent -Line $agyResultEvent) -match '^\d{2}:\d{2}:\d{2}\s+result\s+SUCCESS$') "agy sample result -> result + status"

# Synthetic agy events
$agyRunCommand = @{ event = 'step_update'; step_update = @{ step_type = 'tool'; state = 'ACTIVE'; tool_name = 'run_command'; tool_info = @{ parameters = @{ CommandLine = 'git status' } } } } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $agyRunCommand) -match '^\d{2}:\d{2}:\d{2}\s+run_command\s+git status$') "agy synthetic run_command -> CommandLine target"
$agyResultError = @{ event = 'result'; result = @{ status = 'ERROR'; error = 'invalid model' } } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $agyResultError) -match '^\d{2}:\d{2}:\d{2}\s+error\s+invalid model$') "agy synthetic result error -> error + message"
$agyToolError = @{ event = 'step_update'; step_update = @{ step_type = 'tool'; error = 'permission denied' } } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $agyToolError) -match '^\d{2}:\d{2}:\d{2}\s+error\s+permission denied$') "agy synthetic tool error -> error + message"
$agyTopError = @{ event = 'error'; error = 'connection failed' } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $agyTopError) -match '^\d{2}:\d{2}:\d{2}\s+error\s+connection failed$') "agy synthetic top error -> error + message"

# Synthetic claude stream-json events
$claudeInit = @{ type = 'system'; subtype = 'init'; session_id = 's1' } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $claudeInit) -match '^\d{2}:\d{2}:\d{2}\s+init$') "claude system init -> init"
$claudeRead = @{ type = 'assistant'; message = @{ content = @(@{ type = 'tool_use'; name = 'Read'; input = @{ file_path = 'src/Foo.cs' } }) } } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $claudeRead) -match '^\d{2}:\d{2}:\d{2}\s+Read\s+src/Foo\.cs$') "claude tool_use Read -> file_path target"
$claudeBash = @{ type = 'assistant'; message = @{ content = @(@{ type = 'tool_use'; name = 'Bash'; input = @{ command = 'git status' } }) } } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $claudeBash) -match '^\d{2}:\d{2}:\d{2}\s+Bash\s+git status$') "claude tool_use Bash -> command target"
$claudeBashDesc = @{ type = 'assistant'; message = @{ content = @(@{ type = 'tool_use'; name = 'Bash'; input = @{ command = 'git status'; description = 'check the working tree' } }) } } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $claudeBashDesc) -match '^\d{2}:\d{2}:\d{2}\s+Bash\s+git status - check the working tree$') "claude tool_use Bash with description -> command + description"
$claudeGrepDesc = @{ type = 'assistant'; message = @{ content = @(@{ type = 'tool_use'; name = 'Grep'; input = @{ pattern = 'foo'; description = 'find usages' } }) } } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $claudeGrepDesc) -match '^\d{2}:\d{2}:\d{2}\s+Grep\s+foo - find usages$') "claude tool_use Grep with description -> pattern + description"
$claudeTaskDesc = @{ type = 'assistant'; message = @{ content = @(@{ type = 'tool_use'; name = 'Task'; input = @{ description = 'survey code'; prompt = 'read everything' } }) } } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $claudeTaskDesc) -match '^\d{2}:\d{2}:\d{2}\s+Task\s+survey code$') "claude Task description is not repeated"
$claudeText = @{ type = 'assistant'; message = @{ content = @(@{ type = 'text'; text = 'DONE' }) } } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $claudeText) -match '^\d{2}:\d{2}:\d{2}\s+text\s+DONE$') "claude text block -> text"
$claudeUser = @{ type = 'user'; message = @{ content = @(@{ type = 'tool_result'; content = 'x' }) } } | ConvertTo-Json -Compress -Depth 8
Check ($null -eq (Format-ProgressEvent -Line $claudeUser)) "claude tool_result is skipped"
$claudeOk = @{ type = 'result'; subtype = 'success'; is_error = $false; result = 'ok' } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $claudeOk) -match '^\d{2}:\d{2}:\d{2}\s+result\s+success$') "claude result success -> result"
$claudeErr = @{ type = 'result'; subtype = 'error_max_turns'; is_error = $true } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $claudeErr) -match '^\d{2}:\d{2}:\d{2}\s+error\s+error_max_turns$') "claude result error -> error"

# Synthetic codex exec --json events
$codexStart = @{ type = 'thread.started'; thread_id = 't1' } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $codexStart) -match '^\d{2}:\d{2}:\d{2}\s+init$') "codex thread.started -> init"
$codexCmd = @{ type = 'item.started'; item = @{ type = 'command_execution'; command = 'git status' } } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $codexCmd) -match '^\d{2}:\d{2}:\d{2}\s+bash\s+git status$') "codex command_execution -> bash + command"
$codexEdit = @{ type = 'item.completed'; item = @{ type = 'file_change'; changes = @(@{ path = 'src/Foo.cs'; kind = 'update' }) } } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $codexEdit) -match '^\d{2}:\d{2}:\d{2}\s+edit\s+src/Foo\.cs$') "codex file_change -> edit + path"
$codexMsg = @{ type = 'item.completed'; item = @{ type = 'agent_message'; text = 'DONE' } } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $codexMsg) -match '^\d{2}:\d{2}:\d{2}\s+text\s+DONE$') "codex agent_message -> text"
$codexFail = @{ type = 'turn.failed'; error = @{ message = 'rate limited' } } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $codexFail) -match '^\d{2}:\d{2}:\d{2}\s+error\s+rate limited$') "codex turn.failed -> error"
$codexDone = @{ type = 'turn.completed'; usage = @{ input_tokens = 1 } } | ConvertTo-Json -Compress -Depth 8
Check ((Format-ProgressEvent -Line $codexDone) -match '^\d{2}:\d{2}:\d{2}\s+result\s+success$') "codex turn.completed -> result"

Write-Host "`n== 6b. worker process detection ==" -ForegroundColor Cyan
function New-Proc($Name, $Cmd) { [pscustomobject]@{ Name = $Name; CommandLine = $Cmd } }
Check (Test-WorkerProcess (New-Proc 'claude.exe' 'claude -p "Execute ww-data/tasks/901-a.md. Read AGENTS.md first." --output-format stream-json')) "claude -p with a ww prompt is a worker"
Check (-not (Test-WorkerProcess (New-Proc 'claude.exe' 'claude'))) "interactive claude session is not a worker"
Check (-not (Test-WorkerProcess (New-Proc 'claude.exe' 'claude -p "summarise this file"'))) "claude -p without a ww prompt is not a worker"
Check (Test-WorkerProcess (New-Proc 'node.exe' 'node C:\npm\node_modules\@anthropic-ai\claude-code\cli.js -p "Execute ww-data/tasks/901-a.md."')) "npm claude (node cli.js) with a ww prompt is a worker"
Check (-not (Test-WorkerProcess (New-Proc 'node.exe' 'node server.js'))) "other node processes are not workers"
Check (Test-WorkerProcess (New-Proc 'codex.exe' 'codex exec --json "Execute ww-data/tasks/901-a.md."')) "codex exec with a ww prompt is a worker"
Check (-not (Test-WorkerProcess (New-Proc 'codex.exe' 'codex'))) "interactive codex is not a worker"
Check ((Get-WorkerEngine (New-Proc 'codex.exe' '')) -eq 'codex' -and (Get-WorkerEngine (New-Proc 'node.exe' '')) -eq 'claude') "engine labels"
Check ((Get-EngineCommand -Engine claude -Model sonnet) -match "--model 'sonnet'") "claude command carries -Model"
Check ((Get-EngineCommand -Engine codex) -notmatch '--model') "codex command has no --model when none is given"
Check ((Get-EngineCommand -Engine agy) -match "gemini-3\.8-flash-medium") "agy keeps its default model"

Write-Host "`n== 7. card estimate parsing ==" -ForegroundColor Cyan
$estCard = Join-Path $base "estimate-card.md"
Set-Content -LiteralPath $estCard -Value "# 907`n`n- Estimate 60 min; stop at 120.`n" -Encoding UTF8
Check ((Get-EstimateMinutes -CardPath $estCard) -eq 60) "first Estimate N min is used (60, not 120)"
$noEstCard = Join-Path $base "no-estimate.md"
Set-Content -LiteralPath $noEstCard -Value "# 908`n`nNo limit here.`n" -Encoding UTF8
Check ($null -eq (Get-EstimateMinutes -CardPath $noEstCard)) "missing estimate returns null"
Check ($null -eq (Get-EstimateMinutes -CardPath (Join-Path $base "does-not-exist.md"))) "missing card returns null"

Write-Host "`n== 8. dead-run-record cleanup ==" -ForegroundColor Cyan
$cleanData = Join-Path $base "clean"
$cleanRun = Join-Path $cleanData "running"
$cleanLogs = Join-Path $cleanData "logs"
$cleanReports = Join-Path $cleanData "reports"
New-Item -ItemType Directory -Force $cleanRun, $cleanLogs, $cleanReports | Out-Null
$started = (Get-Date).AddMinutes(-10).ToString("s")
$deadLog = Join-Path $cleanLogs "905.log"
$liveLog = Join-Path $cleanLogs "906.log"
Set-Content -LiteralPath $deadLog -Value "started" -Encoding UTF8
Set-Content -LiteralPath $liveLog -Value "started" -Encoding UTF8
Set-Content -LiteralPath (Join-Path $cleanReports "905.md") -Value "# 905" -Encoding UTF8

$dead = @{ Task = '905'; Card = '905-x.md'; Log = $deadLog; Started = $started; Host = 999999; Worktree = $null }
$live = @{ Task = '906'; Card = '906-x.md'; Log = $liveLog; Started = $started; Host = $PID; Worktree = $null }
$dead | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $cleanRun "905.json") -Encoding UTF8
$live | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $cleanRun "906.json") -Encoding UTF8
@{ Task = '905' } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $cleanData "running.json") -Encoding UTF8

$cleaned = @(Clear-DeadRunRecords -DataDir $cleanData -ScriptName "selftest" -Runs @())
Check ($cleaned -contains "905") "dead record is reported as cleaned"
Check (-not (Test-Path (Join-Path $cleanRun "905.json"))) "dead record file is removed"
Check (Test-Path (Join-Path $cleanRun "906.json")) "live-host record is kept"
Check (-not (Test-Path (Join-Path $cleanData "running.json"))) "running.json naming the dead task is removed"
$last = Get-Content -LiteralPath (Join-Path $cleanRun "905.last.json") -Raw | ConvertFrom-Json
Check ($last.Dead -eq $true) "cleaned last record has Dead=true"
Check ($null -eq $last.ExitCode) "cleaned last record has ExitCode=null"
Check (@($last.ReportsWritten) -contains "905.md") "cleaned last record lists the report"
Check ((Get-Content -LiteralPath $deadLog -Raw) -match 'ended \(dead record cleaned by selftest at ') "dead record log gets the ended line"
Check (-not ((Get-Content -LiteralPath $liveLog -Raw) -match 'dead record cleaned')) "live record log is untouched"

# running.json naming a live task survives another refresh
@{ Task = '906' } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $cleanData "running.json") -Encoding UTF8
$null = Clear-DeadRunRecords -DataDir $cleanData -ScriptName "selftest" -Runs @()
Check (Test-Path (Join-Path $cleanData "running.json")) "running.json naming a live task is kept"
Check (Test-Path (Join-Path $cleanRun "906.json")) "live record survives a second refresh"

# a dead host is not dead while an opencode run command line names its card
$named = @{ Task = '907'; Card = '907-x.md'; Log = (Join-Path $cleanLogs "907.log"); Started = $started; Host = 999999; Worktree = $null }
$named | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $cleanRun "907.json") -Encoding UTF8
$fakeRun = [pscustomobject]@{ ProcessId = 1234; CommandLine = 'opencode.exe run "Execute ww-data/tasks/907-x.md. Read AGENTS.md first."' }
$null = Clear-DeadRunRecords -DataDir $cleanData -ScriptName "selftest" -Runs @($fakeRun)
Check (Test-Path (Join-Path $cleanRun "907.json")) "record whose card is named by an opencode run is not dead"

# a dead host is not dead while an agy run command line names its card
$namedAgy = @{ Task = '909'; Card = '909-x.md'; Log = (Join-Path $cleanLogs "909.log"); Started = $started; Host = 999999; Worktree = $null }
$namedAgy | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $cleanRun "909.json") -Encoding UTF8
$fakeAgyRun = [pscustomobject]@{ ProcessId = 1235; CommandLine = 'agy.exe -p "Execute ww-data/tasks/909-x.md. Read AGENTS.md first."' }
$null = Clear-DeadRunRecords -DataDir $cleanData -ScriptName "selftest" -Runs @($fakeAgyRun)
Check (Test-Path (Join-Path $cleanRun "909.json")) "record whose card is named by an agy run is not dead"

Write-Host "`n== 9. -JsonStdout vs default progress output ==" -ForegroundColor Cyan
$jsonDir = Join-Path $base "jsonout"
New-Item -ItemType Directory -Force $jsonDir | Out-Null
$testEvents = Join-Path $jsonDir "test.events.jsonl"
$testProgress = Join-Path $jsonDir "test.progress.log"
$loopScriptFile = Join-Path $jsonDir "run-loop.ps1"
$libPath = (Join-Path $PSScriptRoot "ww-run.lib.ps1")

@"
param([switch]`$JsonStdout, [string]`$EventsFile, [string]`$ProgressFile)
. '$libPath'
`$utf8 = New-Object System.Text.UTF8Encoding(`$false)
`$lines = @(
    '{"event":"init","conversation_id":"test-1"}',
    '{"event":"step_update","step_update":{"conversation_id":"test-1","step_index":2,"state":"ACTIVE","step_type":"tool","tool_name":"read_file","tool_info":{"name":"read_file","parameters":{"Path":"Assets/Test.cs"}}}}'
)
foreach (`$line in `$lines) {
    [System.IO.File]::AppendAllText(`$EventsFile, `$line + "`r`n", `$utf8)
    if (`$JsonStdout) {
        [Console]::Out.WriteLine(`$line)
    }
    `$progressLine = Format-ProgressEvent -Line `$line
    if (`$progressLine) {
        [System.IO.File]::AppendAllText(`$ProgressFile, `$progressLine + "`r`n", `$utf8)
        if (-not `$JsonStdout) {
            Write-Host `$progressLine
        }
    }
}
"@ | Set-Content -LiteralPath $loopScriptFile -Encoding UTF8

# Case A: Default (no -JsonStdout)
Remove-Item $testEvents, $testProgress -ErrorAction SilentlyContinue
$outDefault = & powershell -NoProfile -ExecutionPolicy Bypass -File $loopScriptFile -EventsFile $testEvents -ProgressFile $testProgress 2>&1 | Out-String
Check ($outDefault -match 'read_file\s+Assets/Test\.cs') "default loop prints formatted progress line to stdout"
Check (-not ($outDefault -match '\{"event":"init"')) "default loop does not print raw JSON to stdout"
Check (Test-Path $testEvents) "default loop writes events.jsonl"
Check ((Get-Content $testEvents -Raw) -match '\{"event":"init"') "default events.jsonl contains raw JSON"
Check (Test-Path $testProgress) "default loop writes progress.log"
Check ((Get-Content $testProgress -Raw) -match 'read_file\s+Assets/Test\.cs') "default progress.log contains formatted progress"

# Case B: With -JsonStdout
Remove-Item $testEvents, $testProgress -ErrorAction SilentlyContinue
$outJson = & powershell -NoProfile -ExecutionPolicy Bypass -File $loopScriptFile -JsonStdout -EventsFile $testEvents -ProgressFile $testProgress 2>&1 | Out-String
Check ($outJson -match '\{"event":"init"') "-JsonStdout prints raw JSON line to stdout"
Check ($outJson -match '\{"event":"step_update"') "-JsonStdout prints subsequent raw JSON lines to stdout"
Check (-not ($outJson -match 'read_file\s+Assets/Test\.cs')) "-JsonStdout suppresses formatted progress line on stdout"
Check (Test-Path $testEvents) "-JsonStdout still writes events.jsonl"
Check ((Get-Content $testEvents -Raw) -match '\{"event":"init"') "events.jsonl still contains raw JSON"
Check (Test-Path $testProgress) "-JsonStdout still writes progress.log"
Check ((Get-Content $testProgress -Raw) -match 'read_file\s+Assets/Test\.cs') "progress.log still contains formatted progress"

# Case C: ww-run.ps1 param binding with -JsonStdout and WW_JSON_STDOUT env
$dryRunOut = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "ww-run.ps1") -Task "903" -Worktree -DryRun -JsonStdout 2>&1 | Out-String
Check ($dryRunOut -match 'DRYRUN plan') "ww-run.ps1 accepts -JsonStdout switch"

$oldEnvJson = $env:WW_JSON_STDOUT
try {
    $env:WW_JSON_STDOUT = "1"
    $dryRunEnvOut = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot "ww-run.ps1") -Task "903" -Worktree -DryRun 2>&1 | Out-String
    Check ($dryRunEnvOut -match 'DRYRUN plan') "ww-run.ps1 accepts WW_JSON_STDOUT=1 environment variable"
} finally {
    $env:WW_JSON_STDOUT = $oldEnvJson
}

Write-Host "`n== 10. a run ends when the engine exits, even if its child keeps stdout open ==" -ForegroundColor Cyan
# The fake engine prints one event, starts a child that inherits stdout and lives for 300 s, then
# exits 0. Before the fix ww-run waited for EOF and so waited for that child.
$runRepo = Join-Path $base "run-repo"
New-Item -ItemType Directory -Force (Join-Path $runRepo "ww-data\tasks") | Out-Null
& git -C $runRepo init -q
& git -C $runRepo -c user.email=selftest@example.invalid -c user.name=selftest commit -q --allow-empty -m init
Set-Content -Encoding UTF8 (Join-Path $runRepo "ww-data\tasks\901-lingering.md") "# 901 lingering child`n`nSplittable: no`n"
$fakeEngine = Join-Path $base "fake-engine.ps1"
Set-Content -Encoding UTF8 $fakeEngine @'
'{"type":"result","subtype":"success"}'
Start-Process -NoNewWindow -FilePath powershell -ArgumentList '-NoProfile', '-Command', 'Start-Sleep -Seconds 300 #ww-selftest-orphan'
exit 0
'@
$oldEngineCmd = $env:WW_TEST_ENGINE_COMMAND
$watch = [System.Diagnostics.Stopwatch]::StartNew()
$runOut = Join-Path $base "run-out.txt"
try {
    $env:WW_TEST_ENGINE_COMMAND = "& powershell.exe -NoProfile -ExecutionPolicy Bypass -File '$fakeEngine'"
    # Started with redirected output and a deadline, so a regression fails here instead of hanging.
    $runProc = Start-Process powershell -ArgumentList '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Join-Path $PSScriptRoot "ww-run.ps1"), '901', '-Root', $runRepo, '-Engine', 'claude' -PassThru -WindowStyle Hidden -RedirectStandardOutput $runOut
    $finished = $runProc.WaitForExit(90000)
    if (-not $finished) { & taskkill /PID $runProc.Id /T /F | Out-Null }
    # Start-Process leaves ExitCode null on Windows PowerShell 5.1 (see ww-watch.ps1), so read the
    # exit code that ww-run wrote to its last record.
    $runExit = if ($finished) { [int](Get-Content (Join-Path $runRepo "ww-data\running\901.last.json") -Raw | ConvertFrom-Json).ExitCode } else { -1 }
} finally {
    $env:WW_TEST_ENGINE_COMMAND = $oldEngineCmd
}
$watch.Stop()
Check $finished ("run ended without waiting for the child (took {0:N1} s)" -f $watch.Elapsed.TotalSeconds)
Check ($runExit -eq 0) "run exit code is the engine's exit code (0)"
$lastRecord = Join-Path $runRepo "ww-data\running\901.last.json"
Check (Test-Path $lastRecord) "last record is written (finally block reached)"
$orphans = @(Get-CimInstance Win32_Process -Filter "Name='powershell.exe'" | Where-Object { $_.CommandLine -match 'ww-selftest-orphan' })
Check ($orphans.Count -eq 0) "the child left behind by the engine was stopped"
foreach ($o in $orphans) { & taskkill /PID $o.ProcessId /F | Out-Null }

Write-Host "`n== summary ==" -ForegroundColor Cyan
Write-Host "pass=$($script:pass) fail=$($script:fail)"
if ($script:fail -gt 0) { exit 1 } else { exit 0 }
