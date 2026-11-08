# ww-run.lib.ps1 — pure helpers for ww-run.ps1: worktree naming, the per-task duplicate rule
# and the tracked-files pre-flight. Dot-source this file; it defines functions only and has no
# side effects, so ww-run.selftest.ps1 can exercise the decisions without starting a run.

# Checkout root and task-data folder; nothing project specific is hardcoded.
#   root : -Root / WW_ROOT, else the git top level of the current directory, else the current directory.
#   data : -DataDir / WW_DATA_DIR, else <root>\ww-data (tasks\, reports\, logs\, running\ ...).
# RelData is the data folder relative to root (normally ww-data); worktree runs use it to find
# the same folder inside the worktree.
# Watch comes from <data>\config.json, e.g. { "watch": ["src", "Tools"] }: the folders (relative
# to root) whose changes count as run progress. Empty means the whole checkout except the data folder.
function Resolve-WwPaths {
    param([string]$Root = $env:WW_ROOT, [string]$DataDir = $env:WW_DATA_DIR)
    if (-not $Root) {
        $previous = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        $top = & git rev-parse --show-toplevel 2>$null | Select-Object -First 1
        $ok = ($LASTEXITCODE -eq 0 -and $top)
        $ErrorActionPreference = $previous
        $Root = if ($ok) { $top -replace '/', '\' } else { (Get-Location).Path }
    }
    $Root = $Root.TrimEnd('\', '/')
    if (-not $DataDir) { $DataDir = Join-Path $Root "ww-data" }
    $DataDir = $DataDir.TrimEnd('\', '/')
    $rel = if ($DataDir.StartsWith("$Root\", [StringComparison]::OrdinalIgnoreCase)) { $DataDir.Substring($Root.Length + 1) }
           else { "ww-data" }
    $watch = @()
    $configFile = Join-Path $DataDir "config.json"
    if (Test-Path -LiteralPath $configFile) {
        try { $config = Get-Content -LiteralPath $configFile -Raw | ConvertFrom-Json }
        catch { throw "cannot read ${configFile}: $($_.Exception.Message)" }
        $watch = @(@($config.watch) | Where-Object { $_ } | ForEach-Object { ([string]$_ -replace '\\', '/').Trim('/') })
    }
    return [pscustomobject]@{
        Root    = $Root
        DataDir = $DataDir
        RelData = $rel
        RelUrl  = $rel -replace '\\', '/'
        Watch   = $watch
    }
}

# Worker runs (agy or opencode)
# Engines: agy, opencode, claude (Claude Code), codex (Codex CLI).
# claude.exe / codex.exe are also what interactive sessions run as, so for those two only a
# command line that carries a ww prompt ("Execute ...tasks/<id>-...") counts as a worker run.
# Claude Code installed through npm runs as node.exe .../claude-code/cli.js, and may also appear
# under the npm or claude.cmd shims.
function Test-WorkerProcess([object]$Proc) {
    if (-not $Proc) { return $false }
    $name = [string]$Proc.Name
    $cmd = [string]$Proc.CommandLine
    $wwPrompt = $cmd -match 'Execute\s+\S*tasks[\\/]'
    $claudePrint = [bool]($wwPrompt -and $cmd -match '\s(-p|--print)\s')
    switch ($name) {
        'agy.exe'      { return $true }
        'opencode.exe' { return [bool]($cmd -match '\srun\s') }
        'claude.exe'   { return $claudePrint }
        'claude.cmd'   { return $claudePrint }
        'node.exe'     { return [bool]($wwPrompt -and $cmd -match 'claude-code[\\/]cli' -and $cmd -match '\s(-p|--print)\s') }
        'npm'          { return $claudePrint }
        'npm.cmd'      { return $claudePrint }
        'npm.exe'      { return $claudePrint }
        'cmd.exe'      { return [bool]($claudePrint -and $cmd -match 'claude') }
        'codex.exe'    { return [bool]($wwPrompt -and $cmd -match '\sexec\s') }
    }
    return $false
}

# Engine name for a worker process (ww-status display).
function Get-WorkerEngine([object]$Proc) {
    switch ([string]$Proc.Name) {
        'agy.exe'      { return 'agy' }
        'opencode.exe' { return 'opencode' }
        'codex.exe'    { return 'codex' }
    }
    return 'claude'
}

function Get-WorkerRuns {
    Get-CimInstance Win32_Process -Filter "Name='opencode.exe' or Name='agy.exe' or Name='claude.exe' or Name='codex.exe' or Name='node.exe'" -ErrorAction SilentlyContinue |
        Where-Object { Test-WorkerProcess $_ }
}
function Get-OpencodeRuns { Get-WorkerRuns }

# The command ww-run runs (inside a child powershell) for each engine. $Model is passed only when
# set; agy has a default model. The prompt is read from a UTF-8 file into $prompt by the caller.
function Get-EngineCommand {
    param([Parameter(Mandatory = $true)][string]$Engine, [string]$Model = "")
    $m = $Model.Replace("'", "''")
    switch ($Engine) {
        'opencode' { return "& opencode.cmd run --format json$(if ($Model) { " --model '$m'" }) `$prompt" }
        'agy'      { return "& agy.exe -p `$prompt --output-format stream-json --model '$(if ($Model) { $m } else { 'gemini-3.8-flash-medium' })' --dangerously-skip-permissions --print-timeout 180m" }
        'claude'   { return "& claude -p `$prompt --output-format stream-json --verbose --dangerously-skip-permissions$(if ($Model) { " --model '$m'" })" }
        'codex'    { return "& codex exec --json --dangerously-bypass-approvals-and-sandbox$(if ($Model) { " --model '$m'" }) `$prompt" }
    }
    throw "unknown engine '$Engine' (agy, opencode, claude, codex)"
}

# Worktree runs are recognised from the prompt/command line that ww-run puts into opencode or agy.
# Keeping this in one place makes "a worktree run never blocks the main run" testable.
function Test-WorktreeCommandLine([string]$CommandLine) {
    if (-not $CommandLine) { return $false }
    return [bool]($CommandLine -match 'WORKTREE RUN' -or $CommandLine -match '\.claude[\\/]worktrees[\\/]')
}

# Does a command line execute this exact card? The negative lookahead keeps task 901 apart from
# its child cards 901-1-*.md, and the backslash alternative covers the absolute Windows path
# that ww-run writes into the prompt.
function Test-CommandLineHasTask([string]$CommandLine, [string]$Task) {
    if (-not $CommandLine -or -not $Task) { return $false }
    $escaped = [regex]::Escape($Task)
    return [bool]($CommandLine -match "tasks[\\/]$escaped-(?!\d+-)")
}

# Plan for -Worktree: deterministic path/branch naming. No IO here (the caller creates it),
# which is what lets -DryRun print the plan without side effects.
function Get-WorktreePlan {
    param(
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$Task,
        [string]$Base = "main"
    )
    return [pscustomobject]@{
        Task   = $Task
        Path   = Join-Path (Join-Path $Root ".claude\worktrees") "ww-$Task"
        Branch = "ww/$Task-wt"
        Base   = $Base
    }
}

# ---------- declared paths (doc/resource-locks.md) ----------
# A card path as a comparable pattern relative to the checkout: forward slashes, lower case,
# no leading "./" or trailing "/". A bare file name (`Foo.cs`) can live anywhere: **/foo.cs.
function ConvertTo-WwPattern([string]$Path) {
    $p = ([string]$Path -replace '\\', '/').Trim().ToLowerInvariant()
    while ($p.StartsWith('./')) { $p = $p.Substring(2) }
    $bare = $p -notmatch '/'
    $p = $p.TrimEnd('/')
    if ($p -and $bare -and $p -ne '**') { $p = "**/$p" }
    return $p
}

# One path segment against another; '*' and '?' never cross '/'. Two wildcard segments are
# assumed to overlap (conservative).
function Test-SegmentOverlap([string]$A, [string]$B) {
    $globA = $A -match '[*?]'
    $globB = $B -match '[*?]'
    if (-not $globA -and -not $globB) { return $A -eq $B }
    if ($globA -and $globB) { return $true }
    $glob = if ($globA) { $A } else { $B }
    $lit = if ($globA) { $B } else { $A }
    $rx = '^' + ([regex]::Escape($glob) -replace '\\\*', '[^/]*' -replace '\\\?', '[^/]') + '$'
    return [bool]($lit -match $rx)
}

# Do two patterns (ConvertTo-WwPattern) name a common file? A directory covers everything below
# it, a '**' segment matches any number of segments. When unsure the answer is "overlap", with one
# exception so bare file names stay usable: when a '**' has swallowed the rest of the other
# pattern and that pattern ends in a file name (`b.cpp`), it is not taken as a directory, so
# `**/foo.cs` does not overlap `src/b.cpp` (it still overlaps `src` and `src/foo.cs`).
function Test-PathPatternOverlap([string]$A, [string]$B) {
    if (-not $A -or -not $B) { return $false }
    return Test-SegmentsOverlap @($A -split '/') 0 @($B -split '/') 0 $false
}

function Test-SegmentsOverlap($Sa, [int]$I, $Sb, [int]$J, [bool]$Swallowed) {
    if ($I -ge $Sa.Count -and $J -ge $Sb.Count) { return $true }
    if ($I -ge $Sa.Count -or $J -ge $Sb.Count) {
        # One side is used up: it is an ancestor directory of the other, unless it names a file
        # that a '**' ran over, or the other side has only '**' left.
        $rest = if ($I -ge $Sa.Count) { @($Sb[$J..($Sb.Count - 1)]) } else { @($Sa[$I..($Sa.Count - 1)]) }
        if (@($rest | Where-Object { $_ -ne '**' }).Count -eq 0) { return $true }
        $done = if ($I -ge $Sa.Count) { $Sa } else { $Sb }
        if ($Swallowed -and $done.Count -gt 0 -and $done[$done.Count - 1] -match '\.[a-z0-9]+$') { return $false }
        return $true
    }
    if ($Sa[$I] -eq '**') {
        if ($I + 1 -ge $Sa.Count) { return $true }
        for ($k = $J; $k -le $Sb.Count; $k++) {
            if (Test-SegmentsOverlap $Sa ($I + 1) $Sb $k ($Swallowed -or $k -gt $J)) { return $true }
        }
        return $false
    }
    if ($Sb[$J] -eq '**') { return Test-SegmentsOverlap $Sb $J $Sa $I $Swallowed }
    if (-not (Test-SegmentOverlap $Sa[$I] $Sb[$J])) { return $false }
    return Test-SegmentsOverlap $Sa ($I + 1) $Sb ($J + 1) $Swallowed
}

# Write/read sets conflict on write x write, write x read and read x write; read x read is fine.
# Returns the first conflicting pair as "<mine> x <theirs>", or $null.
function Find-PathConflict {
    param($Edits, $Reads, $OtherEdits, $OtherReads)
    $e = @(@($Edits) | Where-Object { $_ } | ForEach-Object { ConvertTo-WwPattern $_ })
    $r = @(@($Reads) | Where-Object { $_ } | ForEach-Object { ConvertTo-WwPattern $_ })
    $oe = @(@($OtherEdits) | Where-Object { $_ } | ForEach-Object { ConvertTo-WwPattern $_ })
    $or = @(@($OtherReads) | Where-Object { $_ } | ForEach-Object { ConvertTo-WwPattern $_ })
    foreach ($pair in @(@($e, ($oe + $or)), @($r, $oe))) {
        foreach ($x in @($pair[0])) {
            foreach ($y in @($pair[1])) {
                if (Test-PathPatternOverlap $x $y) { return "$x x $y" }
            }
        }
    }
    return $null
}

# What a run locks in the checkout. A plain main run, a run without Authorised edits, or a run
# whose record is unknown locks the whole checkout ("**").
function Get-RunLocks {
    param($Record)
    if ($Record -and $Record.Shared -and @($Record.Edits | Where-Object { $_ }).Count -gt 0) {
        return [pscustomobject]@{ Edits = @($Record.Edits); Reads = @($Record.Reads | Where-Object { $_ }) }
    }
    return [pscustomobject]@{ Edits = @('**'); Reads = @() }
}

# Exit code of a run refused by Test-RunConflict (75 = EX_TEMPFAIL: try again later). It differs
# from 1 (setup error) and from engine failures, so a caller such as a hancho-cli task queue can tell
# "another run holds these paths" from "the card failed".
$WwExitRefused = 75

# ---------- Unity Editor ----------
# Unity and uloop drive one Editor instance on this checkout, so a main-checkout run needs that
# Editor open on this project. config.json in the data folder may hold:
#   { "unity": { "editor": "C:\\...\\Unity.exe", "startTimeoutSec": 300 } }
# With "editor" set, a missing Editor is started with -projectPath and the check waits for its
# window; without it, a missing Editor is only reported.
function Get-UnityConfig([string]$DataDir) {
    $file = Join-Path $DataDir "config.json"
    if (-not (Test-Path -LiteralPath $file)) { return $null }
    try { return (Get-Content -LiteralPath $file -Raw | ConvertFrom-Json).unity } catch { return $null }
}

function Get-UnityProjectArg([string]$CommandLine) {
    $m = [regex]::Match([string]$CommandLine, '-projectPath\s+"?([^"\s]+)', 'IgnoreCase')
    # Unity's helper processes (AssetImportWorker) pass the path with forward slashes.
    if ($m.Success) { return ($m.Groups[1].Value.TrimEnd('\', '/') -replace '/', '\') }
    return $null
}

# Returns $true when an Editor is open on this checkout (or was started for it).
function Test-UnityEditor {
    param([Parameter(Mandatory = $true)][string]$Root, [Parameter(Mandatory = $true)][string]$DataDir)
    $rootNorm = ($Root.TrimEnd('\', '/') -replace '/', '\')
    $cfg = Get-UnityConfig $DataDir
    $editors = @(Get-CimInstance Win32_Process -Filter "Name='Unity.exe'" -ErrorAction SilentlyContinue)
    if ($editors.Count -eq 0) {
        if (-not $cfg -or -not $cfg.editor -or -not (Test-Path -LiteralPath $cfg.editor)) {
            Write-Host "[ww-run] WARNING: Unity Editor is not running; uloop compile/tests will fail. Open the project, or set unity.editor in $DataDir\config.json" -ForegroundColor Yellow
            return $false
        }
        Write-Host "[ww-run] starting Unity Editor for $rootNorm" -ForegroundColor Cyan
        Start-Process -FilePath $cfg.editor -ArgumentList @('-projectPath', $rootNorm) | Out-Null
        $timeout = if ($cfg.startTimeoutSec) { [int]$cfg.startTimeoutSec } else { 300 }
        $deadline = (Get-Date).AddSeconds($timeout)
        while ((Get-Date) -lt $deadline) {
            $ready = @(Get-Process -Name Unity -ErrorAction SilentlyContinue | Where-Object { $_.MainWindowHandle -ne 0 })
            if ($ready.Count -gt 0) { Write-Host "[ww-run] Unity Editor is up"; return $true }
            Start-Sleep -Seconds 5
        }
        Write-Host "[ww-run] WARNING: Unity Editor window did not appear within $timeout s" -ForegroundColor Yellow
        return $false
    }
    $others = @($editors | Where-Object {
        $p = Get-UnityProjectArg $_.CommandLine
        $p -and ($p -ine $rootNorm)
    })
    if ($others.Count -gt 0) {
        Write-Host "[ww-run] WARNING: Unity Editor is open on another project: $(($others | ForEach-Object { Get-UnityProjectArg $_.CommandLine }) -join ', ')" -ForegroundColor Yellow
    }
    return $true
}

# Duplicate rule (card 170, extended for shared runs):
#   * the same task card never runs twice, in any mode;
#   * main-checkout runs are compared by their declared paths: a plain main run locks the whole
#     checkout, a -Shared run locks its Authorised edits (write) and Read-only paths (read);
#   * a worktree run never blocks the main run and a main run never blocks a worktree run.
# Records maps task id -> run record (ww-data\running\<id>.json) of the active runs.
# -QueueLocked (a hancho-cli task-queue job whose resources already keep overlapping jobs
# apart) skips the path comparison: only the same task blocks.
function Test-RunConflict {
    param(
        $Runs,
        [Parameter(Mandatory = $true)][string]$Task,
        [switch]$Worktree,
        [switch]$Shared,
        [switch]$QueueLocked,
        $Edits = @(),
        $Reads = @(),
        [hashtable]$Records = @{}
    )
    $active = @(@($Runs) | Where-Object { $_ })
    $sameTask = @($active | Where-Object { Test-CommandLineHasTask -CommandLine $_.CommandLine -Task $Task })
    if ($sameTask.Count -gt 0) {
        return [pscustomobject]@{ Blocked = $true; Reason = "task $Task is already running"; Runs = $sameTask }
    }
    if (-not $Worktree -and -not $QueueLocked) {
        $mine = Get-RunLocks ([pscustomobject]@{ Shared = [bool]$Shared; Edits = @($Edits); Reads = @($Reads) })
        $mainRuns = @($active | Where-Object { -not (Test-WorktreeCommandLine $_.CommandLine) })
        foreach ($run in $mainRuns) {
            $record = $null
            foreach ($t in $Records.Keys) {
                if (Test-CommandLineHasTask -CommandLine $run.CommandLine -Task $t) { $record = $Records[$t]; break }
            }
            $theirs = Get-RunLocks $record
            $hit = Find-PathConflict -Edits $mine.Edits -Reads $mine.Reads -OtherEdits $theirs.Edits -OtherReads $theirs.Reads
            if ($hit) {
                $who = if ($record) { "task $($record.Task)" } else { "another main-checkout run" }
                return [pscustomobject]@{ Blocked = $true; Reason = "$who overlaps this run's paths ($hit)"; Runs = @($run) }
            }
        }
    }
    return [pscustomobject]@{ Blocked = $false; Reason = ""; Runs = @() }
}

# ---------- hancho-cli job definitions (doc/resource-locks.md 3.2) ----------
# One Hancho task-queue job per card: the card's Authorised edits / Read-only paths
# become resources.write / resources.read, so hancho-cli never starts two overlapping cards. A card
# without Authorised edits writes "**" (the whole checkout), the same lock ww-run itself takes.
# Bare file names become **/name as in ConvertTo-WwPattern. Worktree jobs get no resources:
# they work in their own checkout.
function Format-CmdArg([string]$Value) {
    if ($Value -match '[\s&()^|<>]') { return '"' + $Value + '"' }
    return $Value
}

function New-HanchoCliJob {
    param(
        [Parameter(Mandatory = $true)][string]$Task,
        [string]$CardName = "",
        [Parameter(Mandatory = $true)][string]$Root,
        [Parameter(Mandatory = $true)][string]$RunCmd,
        [string]$Engine = "agy",
        [string]$Model = "",
        [int]$TimeoutSec = 7200,
        [switch]$Worktree,
        $Edits = @(),
        $Reads = @()
    )
    $title = if ($CardName) { [System.IO.Path]::GetFileNameWithoutExtension($CardName) } else { $Task }
    $parts = @((Format-CmdArg $RunCmd), $Task, $(if ($Worktree) { "-Worktree" } else { "-Shared" }), "-Engine", $Engine)
    if ($Model) { $parts += @("-Model", (Format-CmdArg $Model)) }
    $parts += @("-Root", (Format-CmdArg $Root), "-JsonStdout")
    $job = [ordered]@{
        id         = "ww-$Task"
        summary    = "ww card $title"
        timeoutSec = $TimeoutSec
        cwd        = $Root
        command    = ($parts -join ' ')
    }
    if (-not $Worktree) {
        $w = @(@($Edits) | Where-Object { $_ } | ForEach-Object { ConvertTo-WwPattern $_ })
        $r = @(@($Reads) | Where-Object { $_ } | ForEach-Object { ConvertTo-WwPattern $_ })
        if ($w.Count -eq 0) { $w = @('**') }
        $job.resources = [pscustomobject][ordered]@{ write = $w; read = $r }
    }
    return [pscustomobject]$job
}

# The final build/test job: locks the whole checkout and waits until every card job is done.
function New-HanchoCliVerifyJob {
    param(
        [Parameter(Mandatory = $true)][string]$Command,
        [Parameter(Mandatory = $true)][string]$Root,
        [string[]]$After = @(),
        [int]$TimeoutSec = 7200
    )
    return [pscustomobject][ordered]@{
        id         = "ww-verify"
        summary    = "build + tests after the ww cards"
        timeoutSec = $TimeoutSec
        cwd        = $Root
        resources  = [pscustomobject][ordered]@{ write = @('**'); read = @() }
        dependsOn  = @($After | ForEach-Object { [pscustomobject][ordered]@{ id = $_; on = "done" } })
        command    = $Command
    }
}

# What to tell the user when Test-RunConflict refuses a run. A second run of the same card gets
# no hint (no mode helps). An exclusive main run on either side is the usual cause of a refusal:
# shared mode (declared Authorised edits) or a worktree lets the runs go in parallel.
function Get-ConflictHint {
    param([string]$Reason, [switch]$Shared, [switch]$Worktree, [bool]$HasEdits = $false)
    if ($Worktree -or $Reason -match 'is already running') { return "" }
    $theyLockAll = $Reason -match 'x \*\*\)$' -or $Reason -match '^another main-checkout run'
    if (-not $Shared -or -not $HasEdits) {
        return "this run locks the whole checkout (main mode). To run in parallel, list the card's files under '## Authorised edits' and run with -Shared, or use -Worktree."
    }
    if ($theyLockAll) {
        return "the other run locks the whole checkout (main mode). Start such cards with -Shared (with '## Authorised edits') or -Worktree so they can run in parallel."
    }
    return "the declared paths overlap. Wait for that run, narrow the card's Authorised edits, or use -Worktree."
}

# Run records (ww-data\running\<id>.json) of active runs, keyed by task id.
function Get-ActiveRunRecords {
    param([Parameter(Mandatory = $true)][string]$DataDir)
    $out = @{}
    $runDir = Join-Path $DataDir "running"
    if (-not (Test-Path -LiteralPath $runDir)) { return $out }
    foreach ($file in @(Get-ChildItem -LiteralPath $runDir -Filter "*.json" -File -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -notlike "*.last.json" })) {
        try { $r = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json } catch { continue }
        if ($r -and $r.Task) { $out[[string]$r.Task] = $r }
    }
    return $out
}

# Files an engine edited, from one event line (claude, opencode, agy, codex formats).
function Get-EventEditPaths {
    param([string]$Line)
    if ([string]::IsNullOrWhiteSpace($Line)) { return @() }
    try { $evt = $Line | ConvertFrom-Json -ErrorAction Stop } catch { return @() }
    if ($null -eq $evt) { return @() }
    $editTools = @('edit', 'write', 'multiedit', 'notebookedit', 'edit_file', 'write_file', 'write_to_file',
        'replace_file_content', 'multi_replace_file_content', 'create_file', 'apply_patch')
    $fromInput = {
        param($tool, $in)
        if (-not $tool -or -not $in -or ([string]$tool).ToLowerInvariant() -notin $editTools) { return }
        foreach ($name in @('file_path', 'filePath', 'notebook_path', 'TargetFile', 'path', 'Path')) {
            if ($in.$name) { [string]$in.$name; return }
        }
    }
    $out = New-Object System.Collections.Generic.List[string]
    if ($evt.type -eq 'assistant') {
        foreach ($b in @($evt.message.content)) {
            if ($b -and $b.type -eq 'tool_use') { & $fromInput $b.name $b.input | ForEach-Object { $out.Add($_) } }
        }
    } elseif ($evt.type -eq 'tool_use' -and $evt.part) {
        $in = if ($evt.part.state -and $evt.part.state.input) { $evt.part.state.input } else { $evt.part.input }
        & $fromInput $evt.part.tool $in | ForEach-Object { $out.Add($_) }
    } elseif ($evt.event -eq 'step_update' -and $evt.step_update) {
        $s = $evt.step_update
        $tool = if ($s.tool_name) { $s.tool_name } elseif ($s.tool_info) { $s.tool_info.name } else { $s.tool }
        $in = if ($s.tool_info -and $s.tool_info.parameters) { $s.tool_info.parameters } elseif ($s.parameters) { $s.parameters } else { $s.input }
        & $fromInput $tool $in | ForEach-Object { $out.Add($_) }
    } elseif ($evt.type -eq 'item.completed' -and $evt.item -and $evt.item.type -eq 'file_change') {
        foreach ($c in @($evt.item.changes)) { if ($c -and $c.path) { $out.Add([string]$c.path) } }
    }
    return @($out)
}

# Edited files outside the declared Authorised edits (paths under the data folder are allowed).
# Edited paths are made relative to WorkDir; those outside it are reported as they are.
function Get-EditViolations {
    param(
        [Parameter(Mandatory = $true)][string]$EventsFile,
        [Parameter(Mandatory = $true)][string]$WorkDir,
        $Edits = @(),
        [string]$RelData = "ww-data"
    )
    if (-not (Test-Path -LiteralPath $EventsFile)) { return @() }
    $allowed = @(@($Edits) | Where-Object { $_ } | ForEach-Object { ConvertTo-WwPattern $_ })
    $allowed += (ConvertTo-WwPattern "$RelData/**")
    $base = (($WorkDir -replace '\\', '/').TrimEnd('/') + '/').ToLowerInvariant()
    $seen = New-Object System.Collections.Generic.List[string]
    foreach ($line in [System.IO.File]::ReadAllLines($EventsFile)) {
        foreach ($p in @(Get-EventEditPaths -Line $line)) {
            $n = ($p -replace '\\', '/').ToLowerInvariant()
            if ($n.StartsWith($base)) { $n = $n.Substring($base.Length) }
            $n = $n.Trim().TrimEnd('/')
            if ($seen.Contains($n)) { continue }
            $ok = $false
            foreach ($a in $allowed) { if (Test-PathPatternOverlap $a $n) { $ok = $true; break } }
            if (-not $ok) { $seen.Add($n) }
        }
    }
    return @($seen)
}

# Paths named (in backticks) in the card's "Authorised edits" section. Single .cs file names
# such as `MicroSplatChunk.cs` are kept too; the caller ignores entries that do not exist
# relative to the checkout, so only fully qualified paths are actually pre-flighted.
function Get-AuthorisedEditPaths {
    param([Parameter(Mandatory = $true)][string]$CardPath)
    return @(Get-CardSectionPaths -CardPath $CardPath -Heading 'Authori[sz]ed edits')
}

# Paths in the card's "Read-only" (or "Reads") section: files the run reads but never edits.
# Shared runs may read the same files at the same time; only an edit of them conflicts.
function Get-CardReadPaths {
    param([Parameter(Mandatory = $true)][string]$CardPath)
    return @(Get-CardSectionPaths -CardPath $CardPath -Heading '(Read-only|Reads)')
}

function Get-CardSectionPaths {
    param([Parameter(Mandatory = $true)][string]$CardPath, [Parameter(Mandatory = $true)][string]$Heading)
    $paths = New-Object System.Collections.Generic.List[string]
    if (-not (Test-Path -LiteralPath $CardPath)) { return @() }
    $inSection = $false
    foreach ($line in Get-Content -LiteralPath $CardPath) {
        if ($line -match "^##\s+$Heading\s*$") { $inSection = $true; continue }
        if ($inSection -and $line -match '^##\s') { break }
        if (-not $inSection) { continue }
        foreach ($m in [regex]::Matches($line, '`([^`]+)`')) {
            $token = $m.Groups[1].Value.Trim()
            if (-not $token -or $token -match '\s' -or $token -match '^https?://') { continue }
            if ($token -notmatch '[\\/]' -and $token -notmatch '\.[A-Za-z0-9]+$') { continue }
            if (-not $paths.Contains($token)) { $paths.Add($token) }
        }
    }
    return @($paths)
}

# ---------- progress visibility (card 190) ----------
# The opencode --format json event stream is one JSON object per line. These helpers turn a line
# into one short readable progress line, and read the estimate / reports / liveness facts that
# ww-run and ww-status share. Everything here is pure or read-only so the self tests can drive it
# with sample events and fixture folders (no opencode).
function Shorten-Text([string]$Text, [int]$Max = 160) {
    if (-not $Text) { return "" }
    $s = ($Text -replace '\s+', ' ').Trim()
    if ($s.Length -gt $Max) { $s = $s.Substring(0, $Max) }
    return $s
}

# Estimate from the card line "Estimate 60 min (hard stop at 60 ...)" -> 60; $null when absent.
function Get-EstimateMinutes {
    param([Parameter(Mandatory = $true)][string]$CardPath)
    if (-not $CardPath -or -not (Test-Path -LiteralPath $CardPath)) { return $null }
    $text = Get-Content -LiteralPath $CardPath -Raw -ErrorAction SilentlyContinue
    if (-not $text) { return $null }
    $m = [regex]::Match([string]$text, 'Estimate\s+(\d+)\s*min', 'IgnoreCase')
    if ($m.Success) { return [int]$m.Groups[1].Value }
    return $null
}

# Short target for a tool call: file path for read/edit/write, command for bash/run_command, then title/input/parameters.
function Get-EventTargetCore([object]$Part) {
    if (-not $Part) { return "" }
    $tool = if ($Part.tool_name) { [string]$Part.tool_name }
            elseif ($Part.tool) { [string]$Part.tool }
            elseif ($Part.tool_info -and $Part.tool_info.name) { [string]$Part.tool_info.name }
            elseif ($Part.name) { [string]$Part.name }
            else { "" }
    $state = $Part.state
    $input = if ($Part.tool_info -and $Part.tool_info.parameters) { $Part.tool_info.parameters }
             elseif ($Part.parameters) { $Part.parameters }
             elseif ($state -and $state.input) { $state.input }
             elseif ($Part.input) { $Part.input }
             else { $null }
    if ($input) {
        if ($tool -in @('read', 'edit', 'write', 'multiedit', 'notebookedit', 'read_file', 'edit_file', 'write_file', 'write_to_file', 'view_file')) {
            $p = if ($input.filePath) { $input.filePath } elseif ($input.file_path) { $input.file_path } elseif ($input.notebook_path) { $input.notebook_path }
                 elseif ($input.Path) { $input.Path } elseif ($input.path) { $input.path } else { $null }
            if ($p) { return (Shorten-Text ([string]$p) 120) }
        }
        if ($tool -in @('bash', 'run_command')) {
            $c = if ($input.command) { $input.command } elseif ($input.CommandLine) { $input.CommandLine } elseif ($input.commandLine) { $input.commandLine } else { $null }
            if ($c) { return (Shorten-Text ([string]$c) 120) }
        }
    }
    if ($state -and $state.title) { return (Shorten-Text ([string]$state.title) 120) }
    if ($input) {
        foreach ($name in @('filePath', 'file_path', 'Path', 'path', 'command', 'CommandLine', 'commandLine', 'pattern', 'description', 'query', 'prompt')) {
            $v = $input.$name
            if ($v) { return (Shorten-Text ([string]$v) 120) }
        }
        try { return (Shorten-Text ($input | ConvertTo-Json -Compress -Depth 6) 120) } catch {}
    }
    return ""
}

# Target plus the tool's "description" (Claude's Bash, Grep, Task ... carry one): "target - description".
function Get-EventTarget([object]$Part) {
    $target = Get-EventTargetCore -Part $Part
    $input = if ($Part -and $Part.tool_info -and $Part.tool_info.parameters) { $Part.tool_info.parameters }
             elseif ($Part -and $Part.parameters) { $Part.parameters }
             elseif ($Part -and $Part.state -and $Part.state.input) { $Part.state.input }
             elseif ($Part -and $Part.input) { $Part.input }
             else { $null }    $d = if ($input -and $input.description) { Shorten-Text ([string]$input.description) 80 } else { "" }
    if ($target -and $d -and $d -ne $target) { return ($target + " - " + $d) }
    return $target
}
# One event line -> "HH:mm:ss  <kind>  <short target>" (or $null for empty/uninteresting text).
# Non-JSON lines and unknown event types never throw; they become a raw/type line instead.
function Format-ProgressEvent {
    param([string]$Line)
    if ([string]::IsNullOrWhiteSpace($Line)) { return $null }
    $clock = (Get-Date).ToString("HH:mm:ss")
    $evt = $null
    try { $evt = $Line | ConvertFrom-Json -ErrorAction Stop }
    catch { return ("{0}  raw  {1}" -f $clock, (Shorten-Text $Line 160)) }
    if ($null -eq $evt) { return ("{0}  raw  {1}" -f $clock, (Shorten-Text $Line 160)) }

    if ($evt.timestamp) {
        try { $clock = [DateTimeOffset]::FromUnixTimeMilliseconds([long]$evt.timestamp).ToLocalTime().ToString("HH:mm:ss") }
        catch {}
    }

    # agy stream-json format (card 241)
    if ($evt.event) {
        $eventKind = [string]$evt.event
        if ($eventKind -eq 'init') {
            return ("{0}  init" -f $clock)
        }
        if ($eventKind -eq 'step_update') {
            $step = $evt.step_update
            if (-not $step) { return $null }
            $stepType = [string]$step.step_type

            if ($step.error) {
                return ("{0}  error  {1}" -f $clock, (Shorten-Text ([string]$step.error) 160))
            }
            if ($stepType -eq 'tool') {
                if ($step.state -eq 'DONE' -and -not $step.error) {
                    return $null
                }
                $tool = if ($step.tool_name) { [string]$step.tool_name }
                        elseif ($step.tool_info -and $step.tool_info.name) { [string]$step.tool_info.name }
                        elseif ($step.tool) { [string]$step.tool }
                        else { "tool" }
                $target = Get-EventTarget -Part $step
                if ($target) { return ("{0}  {1}  {2}" -f $clock, $tool, $target) }
                return ("{0}  {1}" -f $clock, $tool)
            }
            if ($stepType -in @('text', 'agent_response')) {
                $text = if ($step.text) { [string]$step.text }
                        elseif ($step.text_delta) { [string]$step.text_delta }
                        else { "" }
                if ([string]::IsNullOrWhiteSpace($text)) { return $null }
                return ("{0}  text  {1}" -f $clock, (Shorten-Text $text 160))
            }
            if ($stepType -eq 'user_input') { return $null }
            if (-not $stepType) { return $null }
            return ("{0}  {1}" -f $clock, $stepType)
        }
        if ($eventKind -eq 'result') {
            $res = $evt.result
            if ($res) {
                if ($res.status -eq 'ERROR' -or $res.error) {
                    $msg = if ($res.error) { [string]$res.error } else { "result error" }
                    return ("{0}  error  {1}" -f $clock, (Shorten-Text $msg 160))
                }
                $status = if ($res.status) { [string]$res.status } else { "SUCCESS" }
                return ("{0}  result  {1}" -f $clock, $status)
            }
            return ("{0}  result" -f $clock)
        }
        if ($eventKind -match 'error') {
            $msg = if ($evt.error) { [string]$evt.error } elseif ($evt.message) { [string]$evt.message } else { "unknown error" }
            return ("{0}  error  {1}" -f $clock, (Shorten-Text $msg 160))
        }
        return ("{0}  {1}" -f $clock, $eventKind)
    }

    $type = [string]$evt.type

    # claude stream-json format (claude -p --output-format stream-json --verbose): system / assistant
    # (content blocks: text, tool_use, thinking) / user (tool results) / result.
    if ($type -in @('system', 'assistant', 'user') -or ($type -eq 'result' -and $evt.subtype)) {
        if ($type -eq 'system') {
            if ($evt.subtype -eq 'init') { return ("{0}  init" -f $clock) }
            return $null
        }
        if ($type -eq 'user') { return $null }
        if ($type -eq 'result') {
            if ($evt.is_error -or $evt.subtype -ne 'success') {
                $msg = if ($evt.result) { [string]$evt.result } else { [string]$evt.subtype }
                return ("{0}  error  {1}" -f $clock, (Shorten-Text $msg 160))
            }
            return ("{0}  result  success" -f $clock)
        }
        $lines = New-Object System.Collections.Generic.List[string]
        foreach ($block in @($evt.message.content)) {
            if (-not $block) { continue }
            if ($block.type -eq 'tool_use') {
                $target = Get-EventTarget -Part $block
                if ($target) { $lines.Add(("{0}  {1}  {2}" -f $clock, $block.name, $target)) }
                else { $lines.Add(("{0}  {1}" -f $clock, $block.name)) }
            } elseif ($block.type -eq 'text' -and -not [string]::IsNullOrWhiteSpace([string]$block.text)) {
                $lines.Add(("{0}  text  {1}" -f $clock, (Shorten-Text ([string]$block.text) 160)))
            }
        }
        if ($lines.Count -eq 0) { return $null }
        return ($lines -join "`r`n")
    }

    # codex exec --json format: thread.started, turn.started/completed/failed, item.started/
    # updated/completed (item.type: agent_message, reasoning, command_execution, file_change,
    # mcp_tool_call, web_search, todo_list, error), error.
    if ($type -like '*.*') {
        if ($type -eq 'thread.started') { return ("{0}  init" -f $clock) }
        if ($type -eq 'turn.completed') { return ("{0}  result  success" -f $clock) }
        if ($type -eq 'turn.failed') {
            $msg = if ($evt.error -and $evt.error.message) { [string]$evt.error.message } else { "turn failed" }
            return ("{0}  error  {1}" -f $clock, (Shorten-Text $msg 160))
        }
        $item = $evt.item
        if (-not $item) { return $null }
        $itemType = [string]$item.type
        if ($type -eq 'item.started') {
            if ($itemType -eq 'command_execution') { return ("{0}  bash  {1}" -f $clock, (Shorten-Text ([string]$item.command) 120)) }
            if ($itemType -eq 'mcp_tool_call') { return ("{0}  {1}.{2}" -f $clock, $item.server, $item.tool) }
            if ($itemType -eq 'web_search') { return ("{0}  search  {1}" -f $clock, (Shorten-Text ([string]$item.query) 120)) }
            return $null
        }
        if ($type -eq 'item.completed') {
            if ($itemType -eq 'agent_message' -and -not [string]::IsNullOrWhiteSpace([string]$item.text)) {
                return ("{0}  text  {1}" -f $clock, (Shorten-Text ([string]$item.text) 160))
            }
            if ($itemType -eq 'file_change') {
                $files = @(@($item.changes) | Where-Object { $_ } | ForEach-Object { [string]$_.path }) -join ', '
                return ("{0}  edit  {1}" -f $clock, (Shorten-Text $files 120))
            }
            if ($itemType -eq 'command_execution' -and $null -ne $item.exit_code -and [int]$item.exit_code -ne 0) {
                return ("{0}  error  exit {1}: {2}" -f $clock, $item.exit_code, (Shorten-Text ([string]$item.command) 120))
            }
            if ($itemType -eq 'error') { return ("{0}  error  {1}" -f $clock, (Shorten-Text ([string]$item.message) 160)) }
        }
        return $null
    }

    # opencode format
    $part = $evt.part

    if ($type -eq 'tool_use') {
        $tool = if ($part -and $part.tool) { [string]$part.tool } else { "tool" }
        $target = Get-EventTarget -Part $part
        if ($target) { return ("{0}  {1}  {2}" -f $clock, $tool, $target) }
        return ("{0}  {1}" -f $clock, $tool)
    }
    if ($type -eq 'text') {
        $text = if ($part -and $part.text) { [string]$part.text } else { "" }
        if ([string]::IsNullOrWhiteSpace($text)) { return $null }
        return ("{0}  text  {1}" -f $clock, (Shorten-Text $text 160))
    }
    if ($type -match 'error') {
        $msg = ""
        foreach ($cand in @($part.error, $part.message, $part.text, $evt.error, $evt.message)) {
            if ($null -eq $cand) { continue }
            if ($cand -is [string]) { $msg = [string]$cand }
            elseif ($cand.message) { $msg = [string]$cand.message }
            else { try { $msg = ($cand | ConvertTo-Json -Compress -Depth 6) } catch {} }
            if ($msg) { break }
        }
        if (-not $msg) { $msg = "unknown error" }
        return ("{0}  error  {1}" -f $clock, (Shorten-Text $msg 160))
    }
    if ($type -eq 'step_start') { return ("{0}  step  start" -f $clock) }
    if ($type -eq 'step_finish') {
        $reason = if ($part -and $part.reason) { [string]$part.reason } else { "" }
        if ($reason) { return ("{0}  step  finish ({1})" -f $clock, $reason) }
        return ("{0}  step  finish" -f $clock)
    }
    if (-not $type) { $type = "unknown" }
    return ("{0}  {1}" -f $clock, $type)
}

# Last lines of a progress log + how long ago the newest line was written (ww-status display).
function Get-ProgressInfo {
    param([string]$Path, [int]$Tail = 5)
    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) { return $null }
    $item = Get-Item -LiteralPath $Path -ErrorAction SilentlyContinue
    if (-not $item) { return $null }
    $lines = @(Get-Content -LiteralPath $Path -Tail $Tail -ErrorAction SilentlyContinue)
    return [pscustomobject]@{
        Path       = $Path
        Lines      = $lines
        AgeMinutes = [math]::Round(((Get-Date) - $item.LastWriteTime).TotalMinutes, 1)
    }
}

# The "ReportsWritten" rule from ww-run, shared with the dead-record cleanup: reports touched
# since the run started whose name is phase-*.md or <Task>*.md.
function Get-ReportsWritten {
    param(
        [Parameter(Mandatory = $true)][string]$ReportsDir,
        $Started = $null,
        [Parameter(Mandatory = $true)][string]$Task
    )
    if (-not (Test-Path -LiteralPath $ReportsDir)) { return @() }
    $start = $null
    if ($Started -is [datetime]) { $start = $Started }
    elseif ($Started) { try { $start = [datetime]::Parse([string]$Started) } catch {} }
    $files = @(Get-ChildItem -LiteralPath $ReportsDir -File -ErrorAction SilentlyContinue)
    if ($start) { $files = @($files | Where-Object { $_.LastWriteTime -ge $start }) }
    return @($files | Where-Object { $_.Name -like "phase-*.md" -or $_.Name -like "$Task*.md" } | ForEach-Object { $_.Name })
}

function Test-ProcessAlive([int]$Id) {
    if ($Id -le 0) { return $false }
    return [bool](Get-Process -Id $Id -ErrorAction SilentlyContinue)
}

# Dead-record cleanup (card 190): a record is dead when its ww-run Host PID is gone AND no
# opencode.exe ... run command line names its card. A live Host is never touched.
# Called by ww-status on each refresh and by ww-run before its duplicate check. Read-only except
# for the dead record itself, so the self test can drive it with a fixture data folder.
function Clear-DeadRunRecords {
    param(
        [Parameter(Mandatory = $true)][string]$DataDir,
        [string]$ScriptName = "ww",
        $Runs = $null
    )
    $runDir = Join-Path $DataDir "running"
    if (-not (Test-Path -LiteralPath $runDir)) { return @() }
    if ($PSBoundParameters.ContainsKey('Runs')) {
        $active = @(@($Runs) | Where-Object { $_ })
    } else {
        $active = @(Get-WorkerRuns)
    }
    $reportsDir = Join-Path $DataDir "reports"
    $legacy = Join-Path $DataDir "running.json"
    $now = Get-Date
    $cleaned = New-Object System.Collections.Generic.List[string]

    $files = @(Get-ChildItem -LiteralPath $runDir -Filter "*.json" -File -ErrorAction SilentlyContinue |
        Where-Object { $_.Name -notlike "*.last.json" })
    foreach ($file in $files) {
        $record = $null
        try { $record = Get-Content -LiteralPath $file.FullName -Raw | ConvertFrom-Json } catch { continue }
        if (-not $record -or -not $record.Task) { continue }
        $task = [string]$record.Task

        $hostPid = 0
        if ($record.Host) { [void][int]::TryParse([string]$record.Host, [ref]$hostPid) }
        if (Test-ProcessAlive -Id $hostPid) { continue }
        $named = @($active | Where-Object { Test-CommandLineHasTask -CommandLine $_.CommandLine -Task $task })
        if ($named.Count -gt 0) { continue }

        $last = @{}
        foreach ($p in $record.PSObject.Properties) { $last[$p.Name] = $p.Value }
        $last.Ended = $now.ToString("s")
        $last.ExitCode = $null
        $last.Dead = $true
        $last.ReportsWritten = @(Get-ReportsWritten -ReportsDir $reportsDir -Started $record.Started -Task $task)
        $last | ConvertTo-Json -Depth 6 | Set-Content -Encoding UTF8 (Join-Path $runDir "$task.last.json")

        if ($record.Log) {
            try {
                $append = "ended (dead record cleaned by {0} at {1}){2}" -f $ScriptName, $now.ToString("s"), "`r`n"
                [System.IO.File]::AppendAllText($record.Log, $append, (New-Object System.Text.UTF8Encoding($false)))
            } catch {}
        }
        Remove-Item -LiteralPath $file.FullName -ErrorAction SilentlyContinue

        if (Test-Path -LiteralPath $legacy) {
            try {
                $l = Get-Content -LiteralPath $legacy -Raw | ConvertFrom-Json
                if ([string]$l.Task -eq $task) { Remove-Item -LiteralPath $legacy -ErrorAction SilentlyContinue }
            } catch {}
        }
        Write-Host ("[{0}] cleaned dead run record for task {1} (host PID {2} not alive)" -f $ScriptName, $task, $record.Host) -ForegroundColor Yellow
        $cleaned.Add($task)
    }
    return @($cleaned)
}

# Find the worker (agy or opencode) spawned for this run (best effort; the record stores it when known).
function Find-WorkerPid {
    param([Parameter(Mandatory = $true)][string]$Task, [int]$Attempts = 8, [int]$DelayMs = 250)
    for ($i = 0; $i -lt $Attempts; $i++) {
        $cand = @(Get-WorkerRuns |
            Where-Object { Test-CommandLineHasTask -CommandLine $_.CommandLine -Task $Task } |
            Sort-Object CreationDate -Descending)
        if ($cand.Count -gt 0) { return [int]$cand[0].ProcessId }
        Start-Sleep -Milliseconds $DelayMs
    }
    return $null
}

function Find-OpencodePid {
    param([Parameter(Mandatory = $true)][string]$Task, [int]$Attempts = 8, [int]$DelayMs = 250)
    Find-WorkerPid -Task $Task -Attempts $Attempts -DelayMs $DelayMs
}

# Worktree pre-flight (card 170): a card may edit a path that does not exist in a fresh
# worktree because it is untracked/ignored in git (e.g. a gitignored vendor folder before card
# 150). For every listed path that exists in the main checkout, ask git whether its parent
# folder is tracked; if not, the worktree would be missing files the card is allowed to touch.
function Get-UntrackedAuthorisedPaths {
    param(
        [Parameter(Mandatory = $true)][string]$CardPath,
        [Parameter(Mandatory = $true)][string]$RepoRoot
    )
    $bad = New-Object System.Collections.Generic.List[string]
    foreach ($rel in @(Get-AuthorisedEditPaths -CardPath $CardPath)) {
        $full = Join-Path $RepoRoot $rel
        if (-not (Test-Path -LiteralPath $full)) { continue }
        $folder = if (Test-Path -LiteralPath $full -PathType Container) { $full } else { [System.IO.Path]::GetDirectoryName($full) }
        if (-not $folder) { continue }
        # git writes the "unknown path" case to stderr; keep that non-terminating here even
        # when the caller (ww-run) runs with $ErrorActionPreference = Stop.
        $previous = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        & git -C $RepoRoot ls-files --error-unmatch -- $folder 2>&1 | Out-Null
        $tracked = ($LASTEXITCODE -eq 0)
        $ErrorActionPreference = $previous
        if (-not $tracked -and -not $bad.Contains($rel)) { $bad.Add($rel) }
    }
    return @($bad)
}

# ---------- engine job object ----------
# ww-run puts the engine launcher into a Windows job object. Every process the engine starts
# belongs to the job, including children whose parent has already exited, so one call ends
# whatever the engine left running. (A tree walk by parent PID cannot find those children.)
if (-not ('WwJob' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class WwJob {
    [DllImport("kernel32.dll", CharSet = CharSet.Unicode)] public static extern IntPtr CreateJobObject(IntPtr attributes, string name);
    [DllImport("kernel32.dll")] public static extern bool AssignProcessToJobObject(IntPtr job, IntPtr process);
    [DllImport("kernel32.dll")] public static extern bool TerminateJobObject(IntPtr job, uint exitCode);
    [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr handle);
}
'@
}

# Returns the job handle, or IntPtr.Zero when the job could not be set up (the caller then
# ends only the launcher).
function New-EngineJob([System.Diagnostics.Process]$Proc) {
    $job = [WwJob]::CreateJobObject([IntPtr]::Zero, $null)
    if ($job -eq [IntPtr]::Zero) { return $job }
    if (-not [WwJob]::AssignProcessToJobObject($job, $Proc.Handle)) {
        [WwJob]::CloseHandle($job) | Out-Null
        return [IntPtr]::Zero
    }
    return $job
}

# Ends every process still in the job, then releases it.
function Stop-EngineJob([IntPtr]$Job) {
    if ($Job -eq [IntPtr]::Zero) { return }
    [WwJob]::TerminateJobObject($Job, 1) | Out-Null
    [WwJob]::CloseHandle($Job) | Out-Null
}
