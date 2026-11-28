# ww-review.ps1 — LLM review of a finished card's report (run by the review job that ww-enqueue adds
# after every card job). It checks whether the report's claims are really backed by the card's
# "Done when" items, and writes a review file that ends with a marker line:
#   <!-- hancho:review-complete: CORRECT -->    or    <!-- hancho:review-complete: INCORRECT -->
# The review job's success check (contentFile + contentRegex) matches only CORRECT, so an INCORRECT
# verdict or a missing verdict fails the review job and the failure shows up in Hancho's triage.
# Usage:  Tools\ww-unity\ww-review.cmd 285c -Report ww-data\reports\285c.md -Engine claude
#         Tools\ww-unity\ww-review.cmd 285c -Report ... -DryRun   (write the prompt, run no engine)
param(
    [Parameter(Mandatory = $true, Position = 0)][string]$CardId,
    [Parameter(Mandatory = $true)][string]$Report,
    [ValidateSet("claude", "agy", "opencode", "codex")][string]$Engine = "claude",
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "ww-run.lib.ps1")
$paths = Resolve-WwPaths
$root = $paths.Root
$data = $paths.DataDir
$utf8 = New-Object System.Text.UTF8Encoding($false)
[Console]::OutputEncoding = [System.Text.Encoding]::UTF8

$tasksDir = Join-Path $data "tasks"
$cardFile = Get-ChildItem -LiteralPath $tasksDir -Filter "$CardId-*.md" -File -ErrorAction SilentlyContinue | Sort-Object Name | Select-Object -First 1
if (-not $cardFile) { throw "[ww-review] task card $CardId-*.md not found in $tasksDir" }
$cardText = Get-Content -LiteralPath $cardFile.FullName -Raw -Encoding UTF8

$reportPath = if ([System.IO.Path]::IsPathRooted($Report)) { $Report } else { Join-Path $root $Report }
if (-not (Test-Path -LiteralPath $reportPath)) { throw "[ww-review] report not found: $reportPath" }
$reportText = Get-Content -LiteralPath $reportPath -Raw -Encoding UTF8

$reviewDir = Join-Path $data "reviews"
New-Item -ItemType Directory -Force $reviewDir | Out-Null
$reviewName = [System.IO.Path]::GetFileNameWithoutExtension($reportPath) + ".review.md"
$reviewPath = Join-Path $reviewDir $reviewName

$prompt = @"
You are reviewing the result of a finished task. Do not edit any files and do not run commands.
Decide whether the report honestly shows that the task is done: every "Done when" item of the
task card is backed by concrete evidence in the report, and the report does not claim success for
work it did not do (for example "tests pass" without test output, or a headings-only report).

Answer with a short justification (at most 15 lines), then a final line that is exactly one of:
VERDICT: CORRECT
VERDICT: INCORRECT

=== TASK CARD ($($cardFile.Name)) ===
$cardText

=== REPORT ($Report) ===
$reportText
"@

$promptPath = Join-Path $reviewDir ($reviewName + ".prompt.txt")
[System.IO.File]::WriteAllText($promptPath, $prompt, $utf8)

if ($DryRun) {
    Write-Host "[ww-review] DRYRUN: prompt written to $promptPath (engine $Engine not run)" -ForegroundColor Yellow
    exit 0
}

# Plain-text output from the engine; the reviewer is asked not to touch files.
$output = switch ($Engine) {
    'claude'   { & claude -p $prompt }
    'agy'      { & agy.exe -p $prompt }
    'opencode' { & opencode.cmd run $prompt }
    'codex'    { & codex exec $prompt }
}
$engineExit = $LASTEXITCODE
$outputText = ($output | Out-String).Trim()
if ($engineExit -ne 0) { throw "[ww-review] engine $Engine exited $engineExit; no verdict written" }

# Fail closed: CORRECT only when the verdict line says so and no INCORRECT line is present.
$verdict = if ($outputText -match '(?m)^\s*VERDICT:\s*INCORRECT\s*$') { 'INCORRECT' }
           elseif ($outputText -match '(?m)^\s*VERDICT:\s*CORRECT\s*$') { 'CORRECT' }
           else { 'INCORRECT' }
$note = if ($verdict -eq 'INCORRECT' -and $outputText -notmatch '(?m)^\s*VERDICT:') { "(no VERDICT line from the reviewer; treated as INCORRECT)`n" } else { '' }

$body = "# Review of $Report`n`nEngine: $Engine   Verdict: $verdict`n`n$note$outputText`n`n<!-- hancho:review-complete: $verdict -->`n"
[System.IO.File]::WriteAllText($reviewPath, $body, $utf8)
Write-Host "[ww-review] $CardId verdict: $verdict -> $reviewPath"
exit 0
