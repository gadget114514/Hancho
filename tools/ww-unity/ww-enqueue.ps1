# ww-enqueue.ps1 — register task cards as hancho-cli taskqueue jobs (card 287).
# Usage:  Tools\ww-unity\ww-enqueue.cmd 285c 284f                  (add both to queue "cards")
#         Tools\ww-unity\ww-enqueue.cmd 285c -After card-287        (first job waits for card-287)
#         Tools\ww-unity\ww-enqueue.cmd 285c 284f -Parallel         (no chaining: jobs run side by side)
#         Tools\ww-unity\ww-enqueue.cmd 285c 284f -DryRun           (write the JSON, do not register)
# Each card becomes one job running "ww-run <id> -QueueLocked". The job's resources keep jobs
# whose write sets overlap apart (one Unity Editor, one writer per authorised path).
# Jobs of one call chain in argument order: each waits for the previous job (on: done).
# -Parallel: no chaining; -After then applies to every job. -Concurrency <n>: hancho-cli --concurrency.
param(
    [Parameter(Mandatory = $true, Position = 0, ValueFromRemainingArguments = $true)][string[]]$Card,
    [string]$After = "",
    [ValidateSet("claude", "agy", "opencode")][string]$Engine = "claude",
    [string]$Queue = "cards",
    [switch]$Parallel,
    [ValidateRange(0, 64)][int]$Concurrency = 0,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "ww-run.lib.ps1")
# Root and data folder as ww-run finds them (WW_ROOT / WW_DATA_DIR, else git top and ww-data).
$paths = Resolve-WwPaths
$root = $paths.Root
$data = $paths.DataDir
# Tool folder as the job command must name it, e.g. Tools\ww-unity.
$toolRel = "Tools\" + (Split-Path -Leaf $PSScriptRoot)
$hanchoCli = if ($env:HANCHO_CLI) { $env:HANCHO_CLI } else { Join-Path $env:LOCALAPPDATA "Programs\Hancho\hancho-cli.exe" }

# The text under a "## <heading>" line, up to the next "## " line. $Heading is a regex.
function Get-CardSection([string]$Text, [string]$Heading) {
    $out = New-Object System.Collections.Generic.List[string]
    $inSection = $false
    foreach ($line in ($Text -split "`r?`n")) {
        if ($line -match "^##\s+($Heading)(\s|\(|$)") { $inSection = $true; continue }
        if ($inSection -and $line -match '^##\s') { break }
        if ($inSection) { $out.Add($line) }
    }
    return ($out -join "`n")
}

# The "Mode:" line of a card ("- Mode: static" / "- Mode: tools"), lower case; '' when there is none.
function Get-CardMode([string]$Text) {
    $m = [regex]::Match($Text, '(?m)^-\s+Mode:\s*(\w+)')
    if ($m.Success) { return $m.Groups[1].Value.ToLowerInvariant() }
    return ''
}

# Backticked repo paths of a section, normalised: a folder or a trailing "/" becomes "<folder>/**".
function Get-CardPaths([string]$Root, [string]$Section) {
    $paths = New-Object System.Collections.Generic.List[string]
    foreach ($m in [regex]::Matches($Section, '`([^`]+)`')) {
        $path = $m.Groups[1].Value.Trim() -replace '\\', '/'
        if ($path -notmatch '^(Assets|Tools|data)/' -or $path -match '\s') { continue }
        if ($path.EndsWith('/')) { $path = $path + '**' }
        elseif (Test-Path -LiteralPath (Join-Path $Root $path) -PathType Container) { $path = "$path/**" }
        if (-not $paths.Contains($path)) { $paths.Add($path) }
    }
    return @($paths)
}

# Job plan for one card: id, command, summary, timeout, report and resources. Pure apart from the
# card file read, so the tests can check the JSON without hancho-cli.
# Resources by "Mode:" line (card 288); the lock only costs time when wrong, so no mode = Unity:
#   static: write = report only; read = the backticked paths under "## Reads". No Unity lock. The
#           scope must list nothing but the report (an error otherwise).
#   tools:  write = scope paths + report. No Unity lock. The scope needs a path and no Assets/ path.
#   other / none: write = @unity-editor + scope paths + report.
function New-CardJob {
    param(
        [Parameter(Mandatory = $true)][string]$CardId,
        [Parameter(Mandatory = $true)][string]$JobId,
        [Parameter(Mandatory = $true)][string]$Engine,
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$TasksDir
    )
    $cardFile = Get-ChildItem -LiteralPath $TasksDir -Filter "$CardId-*.md" -File -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -First 1
    if (-not $cardFile) { throw "[ww-enqueue] task card $CardId-*.md not found in $TasksDir" }
    $text = Get-Content -LiteralPath $cardFile.FullName -Raw -Encoding UTF8

    $estimate = Get-EstimateMinutes -CardPath $cardFile.FullName
    if (-not $estimate) { throw "[ww-enqueue] card $($cardFile.Name) has no 'Estimate N min' line (timeoutSec is required)" }

    $titleMatch = [regex]::Match($text, '(?m)^#\s+(.+?)\s*$')
    $title = if ($titleMatch.Success) { $titleMatch.Groups[1].Value -replace '^Task\s+\S+:\s*', '' } else { $CardId }

    # Report: the first reports/<name>.md named in "Done when"; default <data>/reports/<id>.md.
    $reportMatch = [regex]::Match((Get-CardSection -Text $text -Heading 'Done when'), 'reports/([\w.-]+\.md)')
    $reportName = if ($reportMatch.Success) { $reportMatch.Groups[1].Value } else { "$CardId.md" }
    $report = "$($paths.RelUrl)/reports/$reportName"

    $mode = Get-CardMode -Text $text
    $scope = Get-CardSection -Text $text -Heading 'Allowed change scope|Authori[sz]ed edits'
    $scopePaths = @(Get-CardPaths -Root $Root -Section $scope | Where-Object { $_ -ne $report })
    $write = New-Object System.Collections.Generic.List[string]
    $read = @()
    switch ($mode) {
        'static' {
            if ($scopePaths.Count -gt 0) { throw "[ww-enqueue] card $($cardFile.Name) is Mode: static but its scope lists $($scopePaths -join ', '); a static card writes only its report" }
            $read = @(Get-CardPaths -Root $Root -Section (Get-CardSection -Text $text -Heading 'Reads'))
        }
        'tools' {
            if ($scopePaths.Count -eq 0) { throw "[ww-enqueue] card $($cardFile.Name) is Mode: tools but its scope names no repo path" }
            $assetPaths = @($scopePaths | Where-Object { $_ -like 'Assets/*' })
            if ($assetPaths.Count -gt 0) { throw "[ww-enqueue] card $($cardFile.Name) is Mode: tools but its scope names Assets/ paths ($($assetPaths -join ', ')); use a card without Mode: tools" }
            $write.AddRange([string[]]$scopePaths)
        }
        default {
            # Unity Editor: one shared instance, so every card that drives it takes the same lock.
            $write.Add('@unity-editor')
            $write.AddRange([string[]]$scopePaths)
        }
    }
    if (-not $write.Contains($report)) { $write.Add($report) }

    $job = [ordered]@{
        id         = $JobId
        command    = "cmd /c $toolRel\ww-run.cmd $CardId -Engine $Engine -JsonStdout -QueueLocked"
        cwd        = $Root
        summary    = "ww-run ${CardId}: $title (est $estimate min, $Engine)"
        timeoutSec = $estimate * 60
        maxAttempts = 1
        success    = [ordered]@{ exitCode = 0; filesNewer = @($report) }
        resources  = [ordered]@{ write = @($write); read = @($read) }
    }
    return $job
}

# Job ids are unique across every queue of the dashboard, so the whole list is checked.
function Get-KnownJobIds {
    if (-not (Test-Path -LiteralPath $hanchoCli)) {
        Write-Host "[ww-enqueue] hancho-cli not found at $hanchoCli; job ids are not checked" -ForegroundColor Yellow
        return @()
    }
    # hancho-cli writes UTF-8 (job summaries can hold Japanese); read it as UTF-8 or ConvertFrom-Json fails.
    $prevEncoding = [Console]::OutputEncoding
    [Console]::OutputEncoding = [System.Text.Encoding]::UTF8
    try { $raw = (& $hanchoCli --json taskqueue list 2>&1 | Out-String) }
    finally { [Console]::OutputEncoding = $prevEncoding }
    $obj = $raw | ConvertFrom-Json
    if (-not $obj.ok) { throw "[ww-enqueue] hancho-cli taskqueue list failed: $raw" }
    return @($obj.jobs | ForEach-Object { [string]$_.id })
}

$tasksDir = Join-Path $data "tasks"
$known = @(Get-KnownJobIds)
$planned = New-Object System.Collections.Generic.List[string]
$jobs = New-Object System.Collections.Generic.List[object]
$previous = $After

foreach ($cardId in $Card) {
    $base = "card-$cardId"
    $jobId = $base
    $n = 2
    while ($known -contains $jobId -or $planned.Contains($jobId)) { $jobId = "$base-$n"; $n++ }
    $planned.Add($jobId)

    $job = New-CardJob -CardId $cardId -JobId $jobId -Engine $Engine -Root $root -TasksDir $tasksDir
    # -Parallel: every job waits only for -After (if given); otherwise each waits for the previous job.
    $dependency = if ($Parallel) { $After } else { $previous }
    if ($dependency) { $job['dependsOn'] = @([ordered]@{ id = $dependency; on = "done" }) }
    $jobs.Add($job)
    if (-not $Parallel) { $previous = $jobId }
}

$stamp = Get-Date -Format "yyyyMMdd-HHmmss-fff"
$scratch = Join-Path $root "scratch"
New-Item -ItemType Directory -Force $scratch | Out-Null
$jsonPath = Join-Path $scratch "ww-enqueue-$stamp.json"
$json = [ordered]@{ taskqueue = $Queue; jobs = $jobs.ToArray() } | ConvertTo-Json -Depth 10
# UTF-8 without BOM; PowerShell 5 Set-Content would add one.
[System.IO.File]::WriteAllText($jsonPath, $json, (New-Object System.Text.UTF8Encoding($false)))
Write-Host $json
Write-Host "[ww-enqueue] job file: $jsonPath"

if ($DryRun) {
    Write-Host "[ww-enqueue] DRYRUN: not registered" -ForegroundColor Yellow
    exit 0
}
$addArgs = @('taskqueue', 'add', '--file', $jsonPath, '--taskqueue', $Queue)
if ($Concurrency -gt 0) { $addArgs += @('--concurrency', [string]$Concurrency) }
& $hanchoCli @addArgs
if ($LASTEXITCODE -ne 0) { Write-Host "[ww-enqueue] hancho-cli taskqueue add failed (exit $LASTEXITCODE)" -ForegroundColor Red; exit $LASTEXITCODE }
Write-Host "[ww-enqueue] registered $($jobs.Count) job(s) in queue '$Queue'" -ForegroundColor Green
exit 0
