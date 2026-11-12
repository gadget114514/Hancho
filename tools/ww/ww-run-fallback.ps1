# ww-run-fallback.ps1 - run one task card with a fallback engine when the first engine hits a limit.
#
# The first engine (agy by default) runs the card. If it fails because of a usage limit
# (rate limit, quota, 429, ...), the card is run again with the fallback engine
# (opencode by default) instead of failing the job. Any other failure is reported as is.
#
# Only one engine's event stream reaches stdout, so the consumer (Hancho) sees one clean
# stream: the primary run's stream is kept and printed when it is used, and the fallback
# run streams live.

param(
    [Parameter(Mandatory = $true)][string]$Task,
    [string]$Engine = "agy",
    [string]$Fallback = "opencode",
    [string]$Model = $env:WW_MODEL,
    [string]$Extra = "",
    [string]$Root = $env:WW_ROOT,
    [switch]$Shared,
    [switch]$Main,
    [switch]$Worktree,
    [switch]$JsonStdout,
    [switch]$DryRun
)

$ErrorActionPreference = "Stop"

# Usage-limit wording seen from the engines. Only these make the wrapper fall back;
# every other failure is the card's or the engine's own problem and is reported as is.
$limitPatterns = @(
    'rate[ _-]?limit',
    'ratelimit',
    'too many requests',
    'quota',
    'insufficient_quota',
    'resource[ _-]?exhausted',
    'usage limit',
    'daily limit',
    'weekly limit',
    'limit reached',
    'limit exceeded',
    'exceeded your current',
    'billing',
    'http[^0-9]{0,12}429',
    '"(code|status)"\s*:\s*429'
)

function Get-LimitHint([string]$text) {
    if ([string]::IsNullOrEmpty($text)) { return $null }
    foreach ($p in $limitPatterns) {
        $m = [regex]::Match($text, $p, [System.Text.RegularExpressions.RegexOptions]::IgnoreCase)
        if ($m.Success) { return $m.Value }
    }
    return $null
}

function Get-RunLogTail([string]$task, [string]$root) {
    # The newest .log of this card in ww-data/logs holds the engine's last words.
    $data = if ($root) { Join-Path $root "ww-data" } else { "ww-data" }
    $logs = Join-Path $data "logs"
    if (-not (Test-Path $logs)) { return "" }
    $files = @(Get-ChildItem $logs -Filter "$task-*.log" -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notlike "*-fallback.log" } |
        Sort-Object LastWriteTime -Descending)
    if ($files.Count -eq 0) { return "" }
    try {
        return (Get-Content $files[0].FullName -Tail 60 -ErrorAction SilentlyContinue) -join "`n"
    } catch { return "" }
}

function Write-FallbackNote([string]$message) {
    $note = "[$(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')] $message"
    Write-Host $note
    $data = if ($Root) { Join-Path $Root "ww-data" } else { "ww-data" }
    $logs = Join-Path $data "logs"
    if (Test-Path $logs) {
        try { Add-Content -Path (Join-Path $logs "$Task-fallback.log") -Value $note -Encoding UTF8 } catch { }
    }
}

$runCmd = Join-Path $PSScriptRoot "ww-run.cmd"
if (-not (Test-Path $runCmd)) {
    Write-Host "[ww-run-fallback] ww-run.cmd not found next to this script" -ForegroundColor Red
    exit 1
}

function Get-EngineArgs([string]$engine) {
    $a = @($Task, "-Engine", $engine)
    if ($Root) { $a += @("-Root", $Root) }
    if ($Shared) { $a += "-Shared" }
    if ($Main) { $a += "-Main" }
    if ($Worktree) { $a += "-Worktree" }
    if ($JsonStdout) { $a += "-JsonStdout" }
    if ($Model) { $a += @("-Model", $Model) }
    if ($Extra) { $a += @("-Extra", $Extra) }
    return $a
}

if ($DryRun) {
    Write-Host "[ww-run-fallback] would run: $runCmd $((Get-EngineArgs $Engine) -join ' ')"
    Write-Host "[ww-run-fallback] fallback:  $runCmd $((Get-EngineArgs $Fallback) -join ' ')"
    exit 0
}

$tempDir = [System.IO.Path]::GetTempPath()
$outFile = Join-Path $tempDir ("wwfb-{0}-{1}.out" -f $Task, [guid]::NewGuid().ToString("N").Substring(0, 8))
$errFile = Join-Path $tempDir ("wwfb-{0}-{1}.err" -f $Task, [guid]::NewGuid().ToString("N").Substring(0, 8))

Write-Host "[ww-run-fallback] $Task with $Engine"
$proc = Start-Process -FilePath $runCmd -ArgumentList (Get-EngineArgs $Engine) -NoNewWindow -Wait -PassThru `
    -RedirectStandardOutput $outFile -RedirectStandardError $errFile
$code = $proc.ExitCode

$primaryOut = ""
if (Test-Path $outFile) { $primaryOut = Get-Content $outFile -Raw -ErrorAction SilentlyContinue }
$primaryErr = ""
if (Test-Path $errFile) { $primaryErr = Get-Content $errFile -Raw -ErrorAction SilentlyContinue }
if ($primaryErr) { [Console]::Error.Write($primaryErr) }

if ($code -eq 0) {
    if ($primaryOut) { [Console]::Out.Write($primaryOut) }
    Remove-Item $outFile, $errFile -ErrorAction SilentlyContinue
    exit 0
}

$hint = Get-LimitHint $primaryOut
if (-not $hint) { $hint = Get-LimitHint $primaryErr }
if (-not $hint) { $hint = Get-LimitHint (Get-RunLogTail $Task $Root) }

if (-not $hint) {
    # Not a limit problem: keep the primary result (its stream and its exit code).
    if ($primaryOut) { [Console]::Out.Write($primaryOut) }
    Remove-Item $outFile, $errFile -ErrorAction SilentlyContinue
    Write-FallbackNote "$Engine failed with exit code $code (no usage limit found); no fallback"
    exit $code
}

Write-FallbackNote "$Engine hit a usage limit ('$hint', exit code $code); retrying $Task with $Fallback"

# The fallback run streams its own events to stdout.
& $runCmd (Get-EngineArgs $Fallback)
$fallbackCode = $LASTEXITCODE
Remove-Item $outFile, $errFile -ErrorAction SilentlyContinue
if ($fallbackCode -eq 0) {
    Write-FallbackNote "$Fallback finished $Task"
} else {
    Write-FallbackNote "$Fallback failed with exit code $fallbackCode"
}
exit $fallbackCode
