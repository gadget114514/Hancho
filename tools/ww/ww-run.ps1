# ww-run.ps1 — run one task card (ww-data\tasks\<id>-*.md) with agy, opencode, claude or codex, safely and visibly.
# The checkout root is detected (see Resolve-WwPaths); pass -Root when the current directory is
# not inside the project.
# Usage:  Tools\ww\ww-run.cmd 012                     (main checkout)
#         Tools\ww\ww-run.cmd 012 -Root C:\path\proj  (run against another checkout)
#         Tools\ww\ww-run.cmd 012 -Engine opencode    (agy is the default; also claude, codex)
#         Tools\ww\ww-run.cmd 012 -Engine claude -Model sonnet
#         Tools\ww\ww-run.cmd 012 -Worktree            (parallel run in .claude\worktrees\ww-012)
#         Tools\ww\ww-run.cmd 012 -Worktree -Base main
#         Tools\ww\ww-run.cmd 012 -Worktree -DryRun    (print the plan, change nothing)
#         Tools\ww\ww-run.cmd 012 -Shared              (main checkout, in parallel with runs whose
#                                                     Authorised edits / Read-only paths do not overlap)
#         Tools\ww\ww-run.cmd -Jobs 012,013 -Out ww-jobs.json -Verify build.cmd
#                                                    (write hancho-cli task-queue jobs; runs nothing)
#         Tools\ww\ww-run.cmd -Help
param(
    [string]$Task = "",
    [string]$Extra = "",
    [switch]$Worktree,
    [switch]$Shared,
    [switch]$Main,
    [string]$Base = "main",
    [ValidateSet("agy", "opencode", "claude", "codex")][string]$Engine = $(if ($env:WW_ENGINE) { $env:WW_ENGINE } else { "agy" }),
    [string]$Model = $env:WW_MODEL,
    [string]$Agent = $env:WW_AGENT,
    [switch]$JsonStdout = $(if ($env:WW_JSON_STDOUT -eq "1") { $true } else { $false }),
    [string]$Root = $env:WW_ROOT,
    [switch]$DryRun,
    [string]$Jobs = "",
    [string]$Out = "",
    [string]$Verify = "",
    [int]$TimeoutSec = 7200,
    [string]$Queue = "",
    [int]$Concurrency = 3,
    [switch]$Help
)

$ErrorActionPreference = "Stop"

if ($Help -or (-not $Task -and -not $Jobs)) {
    @"
ww-run - run one task card (ww-data\tasks\<id>-*.md) with an LLM engine, safely and visibly.

USAGE
  ww-run.cmd <id> [-Shared | -Main | -Worktree [-Base main]] [-Engine agy|opencode|claude|codex]
             [-Model <name>] [-Extra "<text>"] [-Root <checkout>] [-JsonStdout] [-DryRun]
  ww-run.cmd -Jobs <id>,<id>,... [-Queue <name> [-Concurrency <n>]] [-Out <file>]
             [-Verify "<command>"] [-TimeoutSec <sec>] [-Worktree] [-Engine ...] [-Model ...] [-Root ...]
  ww-run.cmd -Help

MODES (one card per run; the same card never runs twice)
  -Shared    main checkout, in parallel with other runs whose declared paths do not overlap:
             '## Authorised edits' paths are written, '## Read-only' (or '## Reads') paths are
             read. Read/read is fine; an edit of a path another run edits or reads is refused.
             A card without Authorised edits locks the whole checkout. The agent is told to
             edit only its Authorised edits, skip compile/tests and not commit. Edits outside
             the declaration are reported (EditViolations in ww-data\running\<id>.last.json).
             Run the build/tests afterwards, on their own.
             This is the DEFAULT when the card has '## Authorised edits'.
  -Main      main checkout, exclusive (locks the whole checkout); the agent may build/test.
             The default when the card has no Authorised edits.
  -Worktree  own worktree .claude\worktrees\ww-<id> on branch ww/<id>-wt; always parallel;
             merge the branch yourself.

OPTIONS
  -Engine      agy (default, or WW_ENGINE), opencode, claude, codex
  -Model       engine model (or WW_MODEL)
-Agent       Claude Code agent (.claude/agents/<name>.md) for -Engine claude only (or WW_AGENT)
  -Extra       text appended to the prompt
  -Root        checkout root (or WW_ROOT); default: git top level of the current directory
  -JsonStdout  print the raw engine events on stdout (or WW_JSON_STDOUT=1)
  -DryRun      print the plan and the conflict check; change nothing

HANCHO-CLI JOBS (-Jobs)
  Writes Hancho task-queue jobs for the given cards and runs nothing. Each card
  becomes job ww-<id> running 'ww-run.cmd <id> -Shared ...' with its Authorised edits as
  resources.write and its Read-only paths as resources.read, so hancho-cli only starts cards
  that do not overlap (ww-run checks the same rule again). With -Worktree the jobs use
  -Worktree and declare no resources.
  Cards without Authorised edits and cards that overlap each other are reported on stderr
  (they will run one after another).
  -Queue       also add the jobs to this hancho-cli task queue (hancho-cli.exe from HANCHO_CLI or PATH);
               the file goes to ww-data\jobs\ unless -Out is given
  -Concurrency queue concurrency for -Queue (default 3)
  -Out         file to write (UTF-8 JSON array); default: print to stdout
  -Verify      add job ww-verify running this command after all cards are done, locking
               the whole checkout (build + tests)
  -TimeoutSec  timeoutSec of every job (default 7200)
  Without -Queue:  hancho-cli.exe taskqueue add --file <file> --taskqueue ww --concurrency 3
  Watch:           hancho-cli.exe taskqueue graph ww   (resource_wait shows who holds what)

SUPERVISOR LLM: run cards in parallel with -Shared
  1. Split the work into cards whose files do not overlap. In each card list every file it
     may edit under '## Authorised edits' and files it only reads under '## Read-only',
     as backticked paths relative to the checkout (`src/a.cpp`, `doc/**`).
  2. ww-run.cmd -Jobs 012,013,014 -Queue ww -Verify "<build and test command>" -Engine claude
  3. hancho-cli.exe taskqueue wait ww   (or taskqueue_wait over MCP); read ww-data\reports\<id>.md
     and check EditViolations in ww-data\running\<id>.last.json.
  4. If ww-verify fails, fix it with one more card (or by hand) and taskqueue retry.
  Do not build or test while card jobs run; ww-verify does that last. Check a card's plan
  with: ww-run.cmd <id> -DryRun

EXIT CODES
  0    the engine finished with 0 (-DryRun: the run would start)
  75   refused: the same card is running, or another run holds overlapping paths. Nothing
       ran; try again later (the reason is in the output and ww-data\logs\refused.log)
  1    setup error: bad options, card not found, worktree pre-flight or creation failed
  else the engine's exit code

STATUS
  ww-status.cmd shows running and last runs; logs are in ww-data\logs.
"@ | Write-Output
    exit 0
}

. (Join-Path $PSScriptRoot "ww-run.lib.ps1")
try { $paths = Resolve-WwPaths -Root $Root }
catch { Write-Host "[ww-run] $($_.Exception.Message)" -ForegroundColor Red; exit 1 }
$root = $paths.Root
$data = $paths.DataDir
$logDir = Join-Path $data "logs"
$runDir = Join-Path $data "running"
$reportsDir = Join-Path $data "reports"
$runningFile = Join-Path $data "running.json"
$lastFile = Join-Path $data "last_run.json"
$taskRunFile = Join-Path $runDir "$Task.json"
$taskLastFile = Join-Path $runDir "$Task.last.json"
Set-Location $root

if (@($Worktree, $Shared, $Main | Where-Object { $_ }).Count -gt 1) {
    Write-Host "[ww-run] -Shared, -Main and -Worktree cannot be combined" -ForegroundColor Red
    exit 1
}
# -Agent (or WW_AGENT) names a Claude Code agent; only the claude engine can use it.
if ($Agent -and $Engine -ne 'claude') {
    Write-Host "[ww-run] -Agent (WW_AGENT) needs -Engine claude, not $Engine" -ForegroundColor Red
    exit 1
}
if ($Agent -and $Agent -notmatch '^[A-Za-z0-9][A-Za-z0-9_-]*$') {
    Write-Host "[ww-run] invalid agent name '$Agent' (letters, digits, '-' and '_')" -ForegroundColor Red
    exit 1
}

# -Jobs: write hancho-cli task-queue jobs for several cards (and optionally add them); run nothing.
# Notes go to stderr so the JSON on stdout stays clean.
if ($Jobs) {
    $ids = @($Jobs -split '[,\s]+' | Where-Object { $_ })
    $runCmd = Join-Path $PSScriptRoot "ww-run.cmd"
    $list = New-Object System.Collections.Generic.List[object]
    $declared = @()
    foreach ($id in $ids) {
        $c = Get-ChildItem (Join-Path $data "tasks") -Filter "$id-*.md" -ErrorAction SilentlyContinue | Select-Object -First 1
        if (-not $c) { [Console]::Error.WriteLine("[ww-run] task card $id-*.md not found"); exit 1 }
        $e = @(Get-AuthorisedEditPaths -CardPath $c.FullName)
        $r = @(Get-CardReadPaths -CardPath $c.FullName)
        if (-not $Worktree -and $e.Count -eq 0) {
            [Console]::Error.WriteLine("[ww-run] note: $($c.Name) has no '## Authorised edits'; it locks the whole checkout and runs alone")
        }
        $list.Add((New-HanchoCliJob -Task $id -CardName $c.Name -Root $root -RunCmd $runCmd -Engine $Engine -Model $Model -Agent $Agent `
            -TimeoutSec $TimeoutSec -Worktree:$Worktree -Edits $e -Reads $r))
        $declared += [pscustomobject]@{ Task = $id; Locks = (Get-RunLocks ([pscustomobject]@{ Shared = $true; Edits = $e; Reads = $r })) }
    }
    if (-not $Worktree) {
        for ($i = 0; $i -lt $declared.Count; $i++) {
            for ($j = $i + 1; $j -lt $declared.Count; $j++) {
                $a = $declared[$i].Locks; $b = $declared[$j].Locks
                $hit = Find-PathConflict -Edits $a.Edits -Reads $a.Reads -OtherEdits $b.Edits -OtherReads $b.Reads
                if ($hit) {
                    [Console]::Error.WriteLine("[ww-run] note: cards $($declared[$i].Task) and $($declared[$j].Task) overlap ($hit); hancho-cli runs them one after another")
                }
            }
        }
    }
    if ($Verify) {
        $list.Add((New-HanchoCliVerifyJob -Command $Verify -Root $root -After @($list | ForEach-Object { $_.id }) -TimeoutSec $TimeoutSec))
    }
    $json = ConvertTo-Json -InputObject $list.ToArray() -Depth 6   # @($list) fails in PowerShell 5.1
    if ($Queue -and -not $Out) {
        $Out = Join-Path (Join-Path $data "jobs") ("ww-jobs-{0}.json" -f (Get-Date -Format "yyyyMMdd-HHmmss"))
    }
    if (-not $Out) { Write-Output $json; exit 0 }
    $outPath = [System.IO.Path]::GetFullPath($(if ([System.IO.Path]::IsPathRooted($Out)) { $Out } else { Join-Path (Get-Location).Path $Out }))
    New-Item -ItemType Directory -Force (Split-Path -Parent $outPath) | Out-Null
    [System.IO.File]::WriteAllText($outPath, $json, (New-Object System.Text.UTF8Encoding($false)))
    Write-Host "[ww-run] wrote $($list.Count) job(s) to $outPath"
    if (-not $Queue) {
        Write-Host "[ww-run] add them: hancho-cli.exe taskqueue add --file `"$outPath`" --taskqueue ww --concurrency $Concurrency"
        exit 0
    }
    $hanchoCli = if ($env:HANCHO_CLI) { $env:HANCHO_CLI } else { (Get-Command hancho-cli.exe -ErrorAction SilentlyContinue | Select-Object -First 1).Source }
    if (-not $hanchoCli) {
        Write-Host "[ww-run] hancho-cli.exe not found (set HANCHO_CLI or add it to PATH); add the file by hand" -ForegroundColor Red
        exit 1
    }
    & $hanchoCli taskqueue add --file $outPath --taskqueue $Queue --concurrency $Concurrency
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
    Write-Host "[ww-run] queued in '$Queue'. Wait: hancho-cli.exe taskqueue wait $Queue   Graph: hancho-cli.exe taskqueue graph $Queue"
    exit 0
}

# 1. Resolve the task card. -DryRun still prints a plan when the card is missing.
$card = Get-ChildItem (Join-Path $data "tasks") -Filter "$Task-*.md" -ErrorAction SilentlyContinue | Select-Object -First 1
if (-not $card -and -not $DryRun) {
    Write-Host "[ww-run] task card $Task-*.md not found" -ForegroundColor Red
    exit 1
}
if (-not $card) {
    Write-Host "[ww-run] DRYRUN warning: task card $Task-*.md not found (a real run would exit 1)" -ForegroundColor Yellow
}
$cardPath = if ($card) { $card.FullName -replace '\\', '/' }
            else { (Join-Path (Join-Path $data "tasks") "$Task-*.md") -replace '\\', '/' }

# 2. Plan the worktree (no side effects yet).
$plan = $null
if ($Worktree) {
    $plan = Get-WorktreePlan -Root $root -Task $Task -Base $Base
    Write-Host ("[ww-run] {0} worktree {1}  branch {2}  base {3}" -f
        $(if ($DryRun) { "DRYRUN" } else { "planned" }), $plan.Path, $plan.Branch, $plan.Base) -ForegroundColor Cyan
}

# Declared paths (doc/resource-locks.md): Authorised edits are written, Read-only paths are read.
$edits = @(if ($card) { Get-AuthorisedEditPaths -CardPath $card.FullName })
$reads = @(if ($card) { Get-CardReadPaths -CardPath $card.FullName })
# Shared is the default whenever the card declares its edits, so runs can go in parallel.
if (-not $Worktree -and -not $Main -and -not $Shared -and $edits.Count -gt 0) {
    $Shared = [switch]$true
    Write-Host "[ww-run] card declares Authorised edits: shared mode (-Main for an exclusive run)" -ForegroundColor Cyan
}
if ($Shared -and $edits.Count -eq 0) {
    Write-Host "[ww-run] -Shared: the card has no Authorised edits paths; locking the whole checkout" -ForegroundColor Yellow
}

# 3. Pre-flight: per-task duplicate check and (for -Worktree) tracked-files check.
$runs = @(Get-WorkerRuns)
# Remove stale per-task records before the duplicate check (a dead run must not block a new one).
# -DryRun stays free of side effects, so it neither cleans nor creates anything.
if (-not $DryRun) { $null = Clear-DeadRunRecords -DataDir $data -ScriptName "ww-run" -Runs $runs }
$conflict = Test-RunConflict -Runs $runs -Task $Task -Worktree:$Worktree -Shared:$Shared -Edits $edits -Reads $reads `
    -Records (Get-ActiveRunRecords -DataDir $data)
if ($conflict.Blocked) {
    Write-Host "[ww-run] not starting: $($conflict.Reason)." -ForegroundColor Red
    $conflict.Runs | ForEach-Object { Write-Host ("  PID {0}: {1}" -f $_.ProcessId, $_.CommandLine) }
    Write-Host "  Check progress: Tools\ww\ww-status.cmd   Stop it: Stop-Process -Id <PID>"
    $hint = Get-ConflictHint -Reason $conflict.Reason -Shared:$Shared -Worktree:$Worktree -HasEdits ($edits.Count -gt 0)
    if ($hint) { Write-Host "  HINT: $hint" -ForegroundColor Yellow }
    if (-not $DryRun) {
        # hancho-cli captures the console output above; refused.log keeps it for runs started elsewhere.
        try {
            New-Item -ItemType Directory -Force $logDir | Out-Null
            $mode = if ($Worktree) { 'worktree' } elseif ($Shared) { 'shared' } else { 'main' }
            $entry = "{0}  task {1} ({2}) refused: {3}.{4}" -f (Get-Date).ToString("s"), $Task, $mode, $conflict.Reason,
                $(if ($hint) { " HINT: $hint" } else { "" })
            Add-Content -Encoding UTF8 -LiteralPath (Join-Path $logDir "refused.log") -Value $entry
        } catch {}
        exit $WwExitRefused
    }
    Write-Host "[ww-run] DRYRUN: a real run would refuse to start." -ForegroundColor Yellow
}

$untracked = @()
if ($Worktree -and $card) {
    $untracked = @(Get-UntrackedAuthorisedPaths -CardPath $card.FullName -RepoRoot $root)
    if ($untracked.Count -gt 0) {
        foreach ($p in $untracked) { Write-Host "[ww-run] worktree pre-flight: $p is not tracked in git" -ForegroundColor Red }
        Write-Host "[ww-run] needs tracked files; run on main or land 150 first" -ForegroundColor Red
        if (-not $DryRun) { exit 1 }
    }
}

# 4. Create the worktree, or reuse it when it already exists on the expected branch.
if ($Worktree -and -not $DryRun) {
    if (Test-Path $plan.Path) {
        $current = (& git -C $plan.Path branch --show-current 2>$null | Select-Object -First 1)
        if ($current -ne $plan.Branch) {
            Write-Host "[ww-run] worktree $($plan.Path) is on branch '$current', expected '$($plan.Branch)'; not touching it." -ForegroundColor Red
            exit 1
        }
        Write-Host "[ww-run] reusing worktree $($plan.Path) on $($plan.Branch)"
    } else {
        New-Item -ItemType Directory -Force (Split-Path -Parent $plan.Path) | Out-Null
        & git -C $root rev-parse --verify --quiet "refs/heads/$($plan.Branch)" | Out-Null
        if ($LASTEXITCODE -eq 0) {
            & git -C $root worktree add $plan.Path $plan.Branch
        } else {
            & git -C $root worktree add $plan.Path -b $plan.Branch $plan.Base
        }
        if ($LASTEXITCODE -ne 0) {
            Write-Host "[ww-run] git worktree add failed (exit $LASTEXITCODE)" -ForegroundColor Red
            exit 1
        }
    }
}

# Worktree bootstrap (card 190): opencode rejects reading files outside its working directory,
# so the card is copied into the worktree and referenced by its relative path. The gitignored
# opencode.json is copied too when missing (never overwritten). The report is written inside the
# worktree and copied back to the main ww-data/reports after the run.
$promptCard = $cardPath
if ($Worktree) {
    if (-not $DryRun) {
        $wtTasks = Join-Path (Join-Path $plan.Path $paths.RelData) "tasks"
        New-Item -ItemType Directory -Force $wtTasks | Out-Null
        if ($card) { Copy-Item -LiteralPath $card.FullName -Destination (Join-Path $wtTasks $card.Name) -Force }
        if ($Engine -eq 'opencode') {
            $wtConfig = Join-Path $plan.Path "opencode.json"
            if (-not (Test-Path -LiteralPath $wtConfig)) {
                $mainConfig = Join-Path $root "opencode.json"
                if (Test-Path -LiteralPath $mainConfig) { Copy-Item -LiteralPath $mainConfig -Destination $wtConfig -Force }
            }
        }
    }
    if ($card) { $promptCard = "$($paths.RelUrl)/tasks/$($card.Name)" }
}
$prompt = "Execute $promptCard. Read AGENTS.md first. $Extra".Trim()
$workDir = $root
if ($Shared) {
    $prompt += " SHARED RUN: other runs edit this checkout in parallel. Edit only the files listed under Authorised edits. Skip compile/tests; list them under 'Verification pending' in the report. Do not commit."
}
if ($Worktree) {
    $workDir = $plan.Path
    $prompt += " WORKTREE RUN: work only inside the worktree $($plan.Path). Skip compile/tests; list them under 'Verification pending' in the report. Write the report to $($paths.RelUrl)/reports/$Task.md inside the worktree and commit it with git add -f; do not merge to main. The copied task card is for reading only; do not commit it."
}

if ($DryRun) {
    $cardDisplay = $promptCard
    if ($Worktree -and $card) { $cardDisplay = "$promptCard (copied into the worktree from $cardPath)" }
    Write-Host "[ww-run] DRYRUN plan:"
    Write-Host "  engine      : $Engine"
    Write-Host "  model       : $(if ($Model) { $Model } elseif ($Engine -eq 'agy') { 'gemini-3.8-flash-medium (default)' } else { '(engine default)' })"
    Write-Host "  command     : $(Get-EngineCommand -Engine $Engine -Model $Model -Agent $Agent)"
    if ($Agent) { Write-Host "  agent       : $Agent (.claude/agents/$Agent.md)" }
    Write-Host "  data dir    : $data"
    Write-Host "  mode        : $(if ($Worktree) { 'worktree' } elseif ($Shared) { 'shared' } else { 'main' })"
    Write-Host "  edits       : $(if ($Worktree) { 'n/a (worktree)' } elseif ($Shared -and $edits.Count) { $edits -join ', ' } else { '** (whole checkout)' })"
    Write-Host "  reads       : $(if ($Shared -and $reads.Count) { $reads -join ', ' } else { '-' })"
    Write-Host "  card        : $cardDisplay"
    Write-Host "  working dir : $workDir"
    Write-Host "  branch      : $(if ($Worktree) { "$($plan.Branch) from $($plan.Base)" } else { 'n/a (main checkout)' })"
    Write-Host "  run record  : $taskRunFile"
    Write-Host "  last record : $taskLastFile$(if ($Worktree) { ' (main last_run.json is not touched)' } else { ", $lastFile" })"
    Write-Host "  log         : $logDir\$Task-<stamp>.log"
    Write-Host "  duplicate   : $(if ($conflict.Blocked) { 'WOULD REFUSE: ' + $conflict.Reason } else { 'ok' })"
    Write-Host "  pre-flight  : $(if ($untracked.Count) { 'WOULD REFUSE: ' + ($untracked -join ', ') } else { 'ok' })"
    if ($conflict.Blocked) { exit $WwExitRefused } elseif ($untracked.Count) { exit 1 } else { exit 0 }
}

# 5. Record what is running (one record per task; running.json/last_run.json stay main-only).
New-Item -ItemType Directory -Force $logDir, $runDir | Out-Null
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$log = Join-Path $logDir "$Task-$stamp.log"
$progress = Join-Path $logDir "$Task-$stamp.progress.log"
$events = Join-Path $logDir "$Task-$stamp.events.jsonl"
$promptFile = Join-Path $logDir "$Task-$stamp.prompt.txt"
$start = Get-Date
$record = @{
    Task        = $Task
    Card        = $card.Name
    Log         = $log
    Progress    = $progress
    Events      = $events
    Started     = $start.ToString("s")
    Host        = $PID
    Worktree    = $(if ($Worktree) { $plan.Path } else { $null })
    Shared      = [bool]$Shared
    Edits       = @($edits)
    Reads       = @($reads)
    Engine      = $Engine
    WorkerPid   = $null
    OpencodePid = $null
}
$record | ConvertTo-Json | Set-Content -Encoding UTF8 $taskRunFile
if (-not $Worktree) { $record | ConvertTo-Json | Set-Content -Encoding UTF8 $runningFile }

Write-Host "[ww-run] task $Task ($($card.Name)) started $($start.ToString('HH:mm:ss'))" -ForegroundColor Cyan
if ($Worktree) { Write-Host "[ww-run] worktree: $($plan.Path)  branch: $($plan.Branch)" -ForegroundColor Cyan }
Write-Host "[ww-run] log: $log"

# 6. Run the engine with stdin closed (step 0: otherwise it waits on stdin and prints nothing) and
# read the stream-json/json event stream line by line as it arrives. Raw events go to the .jsonl
# file; Format-ProgressEvent writes one readable line to the .progress.log and the console.
$exit = 0
$utf8 = New-Object System.Text.UTF8Encoding($false)
[System.IO.File]::WriteAllText($events, "", $utf8)
[System.IO.File]::WriteAllText($progress, ("{0}  start  task {1}" -f $start.ToString("HH:mm:ss"), $Task) + "`r`n", $utf8)
Push-Location $workDir
try {
    "started $($start.ToString('s')) prompt: $prompt" | Set-Content -Encoding UTF8 $log
    [System.IO.File]::WriteAllText($promptFile, $prompt, $utf8)

    # The prompt rides in a UTF-8 file so quotes/backslashes in it cannot break the command line.
    # -EncodedCommand avoids cmd.exe quoting entirely; the child closes stdin immediately (below).
    # stderr is left on the console (raw); JSON errors on stdout still reach the progress log.
    $safePrompt = $promptFile.Replace("'", "''")
    $engineCommand = Get-EngineCommand -Engine $Engine -Model $Model -Agent $Agent
    # HOOK: ww-run.selftest.ps1 replaces the real engine with a fake one (a command line, not a path).
    if ($env:WW_TEST_ENGINE_COMMAND) { $engineCommand = $env:WW_TEST_ENGINE_COMMAND }
    $launch = "`$ProgressPreference='SilentlyContinue'; `$prompt = (Get-Content -LiteralPath '$safePrompt' -Raw -Encoding UTF8).TrimEnd(); $engineCommand; if (`$null -eq `$LASTEXITCODE) { exit 1 } else { exit `$LASTEXITCODE }"
    $encoded = [Convert]::ToBase64String([System.Text.Encoding]::Unicode.GetBytes($launch))

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "powershell.exe"
    $psi.Arguments = "-NoProfile -NonInteractive -ExecutionPolicy Bypass -EncodedCommand $encoded"
    $psi.WorkingDirectory = $workDir
    $psi.UseShellExecute = $false
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $psi.CreateNoWindow = $true
    $proc = [System.Diagnostics.Process]::Start($psi)
    $proc.StandardInput.Close()
# Assign the job before anything else runs: children started before this point are not in it.
$job = New-EngineJob -Proc $proc

    $workerPid = Find-WorkerPid -Task $Task
    if ($workerPid) {
        $record.WorkerPid = $workerPid
        $record.OpencodePid = $workerPid
        $record | ConvertTo-Json | Set-Content -Encoding UTF8 $taskRunFile
    }

    # Read the stream until the run is over, not until stdout reaches EOF. The engine's children
    # (MCP servers, shell tools) inherit stdout and can outlive the engine, so EOF may never come.
    # The run is over when the launcher has exited and its buffered output is drained (3 s grace).
    $reader = $proc.StandardOutput
    $pending = $reader.ReadLineAsync()
    $exitedAt = $null
    while ($true) {
        if ($pending.Wait(500)) {
            $line = $pending.Result
            if ($null -eq $line) { break }
            $exitedAt = $null
            [System.IO.File]::AppendAllText($events, $line + "`r`n", $utf8)
            if ($JsonStdout) {
                [Console]::Out.WriteLine($line)
            }
            $progressLine = Format-ProgressEvent -Line $line
            if ($progressLine) {
                [System.IO.File]::AppendAllText($progress, $progressLine + "`r`n", $utf8)
                if (-not $JsonStdout) {
                    Write-Host $progressLine
                }
            }
            $pending = $reader.ReadLineAsync()
            continue
        }
        if ($proc.HasExited) {
            if (-not $exitedAt) { $exitedAt = Get-Date }
            if (((Get-Date) - $exitedAt).TotalSeconds -ge 3) { break }
        }
    }
    # The launcher's exit code is the engine's. Then end whatever the engine left running (its
    # children may still hold the stdout pipe); they are not part of the run any more.
    if (-not $proc.HasExited) { $proc.WaitForExit(5000) | Out-Null }
    Stop-EngineJob -Job $job
    if (-not $proc.HasExited) { & taskkill /PID $proc.Id /T /F | Out-Null }
    $proc.WaitForExit()
    $exit = $proc.ExitCode
} catch {
    $exit = 1
    $_ | Out-String | Add-Content $log
} finally {
    Pop-Location
    # The agent wrote the report inside the worktree; bring it back for the main checkout.
    if ($Worktree) {
        $wtReport = Join-Path (Join-Path $plan.Path $paths.RelData) "reports\$Task.md"
        if (Test-Path -LiteralPath $wtReport) {
            New-Item -ItemType Directory -Force $reportsDir | Out-Null
            Copy-Item -LiteralPath $wtReport -Destination (Join-Path $reportsDir "$Task.md") -Force
        }
    }
    $end = Get-Date
    $mins = [math]::Round(($end - $start).TotalMinutes, 1)
    $reports = @(Get-ReportsWritten -ReportsDir $reportsDir -Started $start -Task $Task)
    $violations = @()
    if ($edits.Count -gt 0) {
        try { $violations = @(Get-EditViolations -EventsFile $events -WorkDir $workDir -Edits $edits -RelData $paths.RelData) } catch {}
        if ($violations.Count -gt 0) {
            Write-Host "[ww-run] WARNING: edited outside Authorised edits: $($violations -join ', ')" -ForegroundColor Yellow
            "edit violations: $($violations -join ', ')" | Add-Content -Encoding UTF8 $log
        }
    }
    $last = @{
        Task           = $Task
        Card           = $card.Name
        Log            = $log
        Started        = $start.ToString("s")
        Ended          = $end.ToString("s")
        Minutes        = $mins
        ExitCode       = $exit
        ReportsWritten = @($reports)
        EditViolations = @($violations)
        Shared         = [bool]$Shared
        Worktree       = $(if ($Worktree) { $plan.Path } else { $null })
        Engine         = $Engine
    }
    $last | ConvertTo-Json | Set-Content -Encoding UTF8 $taskLastFile
    if (-not $Worktree) { $last | ConvertTo-Json | Set-Content -Encoding UTF8 $lastFile }
    Remove-Item $taskRunFile -ErrorAction SilentlyContinue
    if (-not $Worktree) { Remove-Item $runningFile -ErrorAction SilentlyContinue }

    $msg = if ($reports) { "Task $Task finished in $mins min. Report: $($reports -join ', ')" }
           else { "Task $Task ended in $mins min WITHOUT a phase report (exit $exit). See log." }
    $color = if ($reports -and $exit -eq 0) { "Green" } else { "Yellow" }
    Write-Host "[ww-run] $msg" -ForegroundColor $color
    [console]::beep(880, 300)
}
exit $exit
