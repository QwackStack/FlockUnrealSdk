# Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

# ASCII only, deliberately. Windows PowerShell 5.1 reads a BOM-less .ps1 as ANSI, so any non-ASCII
# character here becomes a parser error on someone else's machine rather than a nicer-looking banner.

<#
.SYNOPSIS
    Builds the plugin against every installed Unreal Engine at or above the declared floor, and reports
    what it could not cover.

.DESCRIPTION
    Run this before tagging a release.

    The report is the point, not the build. A script that prints "all builds passed" after compiling one
    engine is worse than no script, because it manufactures confidence in a claim nobody verified. This
    one always prints what it covered *and* what it did not, and fails when the floor engine is missing --
    an unverifiable claim is not a passing one.

    Each engine is built twice over. The editor target is what the automation suite runs in, in the editor
    and again with -game -- but both of those passes run the editor binary, where editor-only data such as
    Blueprint metadata exists, so neither compiles the plugin the way a packaged game does. The game target
    is therefore built as well, in every configuration in -GameConfigurations, and an engine counts as
    verified only when those builds succeed too. Two defects shipped exactly that way, with every suite green
    on every engine: a test that read Blueprint metadata without a guard broke every Development package, and
    a log-category call that Shipping compiles out broke every Shipping build.

    It cannot run in GitHub-hosted CI: those runners have no engine. The no-engine half of the checks
    lives in .github/workflows/consistency.yml.

    Exit codes: 0 the claim is verified, 1 the claim is unproven (something in range failed, or an end of
    the range went uncovered), 2 the source tree changed while the sweep was running -- which invalidates
    every result in the run, including the ones that passed.

.PARAMETER Project
    The .uproject used to drive the build. Defaults to the sibling UEBuildEnviroment project.

.PARAMETER Target
    The editor target the automation suite runs in. Defaults to UEBuildEnviromentEditor.

.PARAMETER GameTarget
    The project's game target, built without the editor. Defaults to the project's own name, which is the
    name Unreal gives a project's game target. The sweep refuses to start when it cannot find it.

.PARAMETER GameConfigurations
    The configurations the game target is built in. Defaults to Development and Shipping.
    Development is what a playtest or QA build uses, and it compiles the automation tests without editor-only
    data. Shipping compiles logging out, along with everything under !UE_BUILD_SHIPPING, so it compiles code
    no other build does. Test is not in the default because an installed engine refuses it ("Targets cannot be
    built in the Test configuration with this engine distribution"); it needs an engine built from source.

.PARAMETER SearchRoots
    Directories to sweep for UE_x.y installs. Defaults to 'Program Files\Epic Games' and 'Epic Games' on
    every fixed drive, because an engine moved off C: records itself nowhere the registry or
    LauncherInstalled.dat can see -- and an engine the script cannot see is one the report silently
    omits, which is the failure mode this script exists to prevent.

.EXAMPLE
    # From a Windows PowerShell prompt at the plugin root:
    .\Tooling\Build-AllEngines.ps1

.EXAMPLE
    # If the execution policy blocks the direct call:
    powershell.exe -NoProfile -ExecutionPolicy Bypass -File Tooling\Build-AllEngines.ps1

.NOTES
    Windows PowerShell 5.1 (powershell.exe) is what this is written and verified against, not
    PowerShell 7 (pwsh) -- which is not installed by default on Windows, so `pwsh` fails with
    CommandNotFoundException on a stock machine. Nothing here needs 7.
#>

[CmdletBinding()]
param(
    [string]   $Project,
    [string]   $Target = 'UEBuildEnviromentEditor',
    [string]   $GameTarget,
    [ValidateSet('DebugGame', 'Development', 'Shipping', 'Test')]
    [string[]] $GameConfigurations = @('Development', 'Shipping'),
    [string[]] $SearchRoots
)

$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent $PSScriptRoot

# The default lives here, not in the param block: under [CmdletBinding()] PowerShell binds param
# defaults before the script scope has $PSScriptRoot, so a Join-Path default there fails with an
# empty-string bind error the moment the script is run with -File.
if (-not $Project) {
    $Project = Join-Path $PSScriptRoot '..\..\..\UEBuildEnviroment.uproject'
}
if (-not (Test-Path $Project)) {
    throw "Cannot find the project at $Project -- pass -Project explicitly."
}
$Project = (Resolve-Path $Project).Path
$ProjectDir = Split-Path -Parent $Project

# Refused up front rather than reported per engine: without a game target every engine would fail the same
# way, after an hour of editor builds and test runs.
if (-not $GameTarget) {
    $GameTarget = [System.IO.Path]::GetFileNameWithoutExtension($Project)
}
$GameTargetFile = Join-Path $ProjectDir "Source\$GameTarget.Target.cs"
if (-not (Test-Path $GameTargetFile)) {
    throw "Cannot find the game target at $GameTargetFile -- pass -GameTarget explicitly. Without a game build the sweep cannot see a defect that only a packaged game compiles."
}

# -- The tree the sweep measures must not move underneath it --
#
# Every engine compiles whatever is on disk at that moment, so a commit, a checkout, a pull or a branch
# switch part-way through leaves the engines before it and the engines after it measuring different source
# -- and nothing in the report can tell. Measured on 2026-09-16: a checkout during a sweep left one engine
# compiling a file from before the very change being verified. It reported GAME BUILD FAILED naming a
# symbol that exists nowhere in the tree, read exactly like a defect on that engine, cost the whole
# 44-minute run, and took a reflog and a line-number match to disprove.
function Get-TreeState {
    param([string] $Dir)

    Push-Location $Dir
    try {
        $Head = & git rev-parse HEAD 2>$null
        if ($LASTEXITCODE -ne 0 -or -not $Head) {
            return $null
        }

        # Only the paths a build actually reads. Editing a doc, a plan or this script during a sweep changes
        # nothing the compiler sees, and voiding a 44-minute run over it would only teach people to work
        # around the guard. Each path's tree object covers its committed content, so HEAD moving for a
        # docs-only commit is correctly ignored while a source change is not.
        $Watched = @('Source', 'OptionalPlugins', 'Flock.uplugin')
        $Parts = [System.Collections.Generic.List[string]]::new()
        foreach ($Path in $Watched) {
            $Tree = & git rev-parse "HEAD:$Path" 2>$null
            if ($LASTEXITCODE -eq 0 -and $Tree) {
                $Parts.Add("$Path=" + $Tree.Trim())
            }
        }
        # Uncommitted edits count as much as commits do: the compiler reads the bytes on disk either way.
        $Parts.Add((& git status --porcelain -- $Watched 2>$null) -join "`n")

        $Sha = [System.Security.Cryptography.SHA1]::Create()
        try {
            $Bytes = [System.Text.Encoding]::UTF8.GetBytes(($Parts -join "`n"))
            $Print = [System.BitConverter]::ToString($Sha.ComputeHash($Bytes)).Replace('-', '')
        } finally {
            $Sha.Dispose()
        }
        return [PSCustomObject]@{ Head = $Head.Trim(); Fingerprint = $Print }
    } finally {
        Pop-Location
    }
}

# True while the tree still matches what the sweep started on. A checkout that cannot be read by git at all
# is reported once and the sweep carries on: an unguarded run is still worth having, an unnoticed one is not.
function Test-TreeUnchanged {
    param($Before, [string] $Dir, [string] $When)

    if (-not $Before) { return $true }
    $Now = Get-TreeState $Dir
    if (-not $Now -or $Now.Fingerprint -eq $Before.Fingerprint) { return $true }

    Write-Host ''
    Write-Host 'THE SOURCE TREE CHANGED WHILE THE SWEEP WAS RUNNING.' -ForegroundColor Red
    Write-Host ("  started on: " + $Before.Head) -ForegroundColor Red
    Write-Host ("  $When`: " + $Now.Head) -ForegroundColor Red
    Write-Host '  Engines built either side of that change measured different source, so nothing in this' -ForegroundColor Red
    Write-Host '  report means what it says -- a pass as much as a failure. Leave git alone for the length' -ForegroundColor Red
    Write-Host '  of a sweep, then run it again.' -ForegroundColor Red
    return $false
}

$TreeAtStart = Get-TreeState $PluginRoot
if ($TreeAtStart) {
    Write-Host ("Tree:           " + $TreeAtStart.Head.Substring(0, 12) + " (Source, OptionalPlugins and Flock.uplugin, committed and not)") -ForegroundColor Cyan
} else {
    Write-Host 'Tree:           not a git checkout, so a source change during the sweep cannot be detected.' -ForegroundColor DarkYellow
}

# The project's drive carries every Intermediate and Binaries the sweep writes, and it cleans and rebuilds
# one per engine. Reported up front because a build that dies for want of disk does not say so plainly.
$ProjectDrive = New-Object System.IO.DriveInfo((Split-Path -Qualifier $ProjectDir) + '\')
Write-Host ("Free space:     {0:N1} GB on {1}" -f ($ProjectDrive.AvailableFreeSpace / 1GB), $ProjectDrive.Name) -ForegroundColor Cyan

# Every engine in the sweep builds into this one project's Intermediate/Binaries, and UHT output does
# NOT reliably regenerate when the engine underneath it changes. A later engine's .gen.cpp compiled
# against an earlier engine's headers fails in generated code that names no SDK file at all -- which
# reads as "the SDK is broken on 5.7" when the truth is "5.8 ran first". Cleaning between engines is
# what makes each result mean what it claims to mean.
function Remove-BuildArtifacts {
    param([string] $Dir)
    foreach ($Sub in @('Intermediate', 'Binaries')) {
        $Path = Join-Path $Dir $Sub
        if (Test-Path $Path) {
            Remove-Item $Path -Recurse -Force -ErrorAction SilentlyContinue
        }
    }
}

function Format-Duration {
    param([int] $Seconds)
    return ('{0}m {1:00}s' -f [int][math]::Floor($Seconds / 60), ($Seconds % 60))
}

# The build log goes to the console, not into the returned object: a native command's output inside a
# function is part of what the function returns. Tee-Object keeps a copy for the strain scan while Out-Host
# still consumes the pipeline, so none of it leaks into the return value.
function Invoke-TimedBuild {
    param(
        [string] $BuildBat,
        [string] $TargetName,
        [string] $Configuration,
        [string] $Project
    )

    $Watch = [System.Diagnostics.Stopwatch]::StartNew()
    & $BuildBat $TargetName Win64 $Configuration -project="$Project" -WaitMutex |
        Tee-Object -Variable BuildOutput | Out-Host
    $ExitCode = $LASTEXITCODE
    $Watch.Stop()

    # A machine that runs short of paging file or disk usually still finishes the build -- the build system
    # sleeps and retries -- so it never reaches the report, and whatever fails next looks like a code defect
    # instead of a tired machine. Measured on 2026-09-16: UE 5.7 logged "The paging file is too small for
    # this operation to complete" and still passed. Surfaced rather than failed on, because it did complete.
    $Strain = @($BuildOutput | Where-Object {
        $_ -is [string] -and $_ -match 'paging file is too small|Allocation failed in VirtualAlloc|not enough space on the disk|out of memory'
    })

    return [PSCustomObject]@{
        Succeeded = ($ExitCode -eq 0)
        Seconds   = [int]$Watch.Elapsed.TotalSeconds
        Strain    = $Strain
    }
}

# Compiling is not the claim. "It builds on 5.7" says nothing about whether it behaves the same there,
# and the APIs that move between engines are exactly the ones where a wrong-but-compiling substitution
# is possible. So every engine runs the automation suite, and the report says which of the two happened.
function Invoke-FlockTests {
    param(
        [string] $EngineDir,
        [string] $Project,
        [string] $ProjectDir,
        # Runs the suite with the editor not hosting it. Only tests declaring ClientContext execute here,
        # which is deliberately a subset -- the disk-touching paths (token store, snapshot store, asset
        # cache, command queue) plus the wire layer. Editor-only testing cannot see a defect that depends
        # on the editor being absent, and this SDK has already shipped one of those.
        [switch] $GameContext
    )

    $EditorCmd = Join-Path $EngineDir 'Engine\Binaries\Win64\UnrealEditor-Cmd.exe'
    if (-not (Test-Path $EditorCmd)) {
        return [PSCustomObject]@{ Ran = $false; Total = 0; Failed = 0; Reason = 'no UnrealEditor-Cmd.exe' }
    }

    # The editor names its log for the project, so derive it rather than hardcoding this repo's name.
    $LogName = [System.IO.Path]::GetFileNameWithoutExtension($Project) + '.log'
    $LogPath = Join-Path $ProjectDir (Join-Path 'Saved\Logs' $LogName)
    if (Test-Path $LogPath) {
        Remove-Item $LogPath -Force -ErrorAction SilentlyContinue
    }

    $ConsoleOut = Join-Path ([System.IO.Path]::GetTempPath()) 'flock-automation-console.log'

    # EAP is relaxed only around the native call. Windows PowerShell turns a redirected native command's
    # stderr into ErrorRecords, and under 'Stop' the first harmless line the editor writes there would
    # abort the whole sweep. The exit code and the log are what we actually judge on.
    $PrevEap = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    if ($GameContext) {
        & $EditorCmd $Project -game '-ExecCmds=Automation RunTests Flock.; Quit' -unattended -nullrhi -NoSplash -log *> $ConsoleOut
    } else {
        & $EditorCmd $Project '-ExecCmds=Automation RunTests Flock.; Quit' -unattended -nullrhi -NoSplash -log *> $ConsoleOut
    }
    $ErrorActionPreference = $PrevEap

    if (-not (Test-Path $LogPath)) {
        return [PSCustomObject]@{ Ran = $false; Total = 0; Failed = 0; Reason = 'no editor log was written' }
    }

    $LogText = Get-Content $LogPath -Raw -ErrorAction SilentlyContinue
    if (-not $LogText) {
        return [PSCustomObject]@{ Ran = $false; Total = 0; Failed = 0; Reason = 'editor log was empty' }
    }

    $Total = ([regex]::Matches($LogText, 'Test Completed\. Result=\{')).Count
    $Ok    = ([regex]::Matches($LogText, 'Test Completed\. Result=\{Success\}')).Count

    # Zero tests is a failure, not a pass. An editor that died on startup reports exactly the same "no
    # failures" as a clean run, and treating that as success is how a broken engine gets called verified.
    if ($Total -eq 0) {
        return [PSCustomObject]@{ Ran = $false; Total = 0; Failed = 0; Reason = 'no tests ran' }
    }

    # A failure is named, with the first error it logged, and its log is kept. The next run deletes this log, and a
    # count alone once left a one-off failure on one engine that nothing could identify afterwards (2026-09-18).
    $FailedTests = @()
    if ($Ok -lt $Total) {
        foreach ($Match in [regex]::Matches($LogText, 'Test Completed\. Result=\{Fail\w*\} Name=\{[^}]*\} Path=\{([^}]*)\}')) {
            $Path = $Match.Groups[1].Value
            # The first error between the test's BeginEvents and EndEvents lines; warnings can come before it.
            $Events = [regex]::Match($LogText, '(?s)BeginEvents: ' + [regex]::Escape($Path) + '(.*?)EndEvents: ' + [regex]::Escape($Path))
            $Detail = if ($Events.Success) { [regex]::Match($Events.Groups[1].Value, 'Error: ([^\r\n]*)') } else { $null }
            $FailedTests += if ($Detail -and $Detail.Success) { "$Path -- $($Detail.Groups[1].Value)" } else { $Path }
        }
        $Engine = Split-Path -Leaf $EngineDir
        $Context = if ($GameContext) { 'game' } else { 'editor' }
        $KeptLog = Join-Path (Split-Path -Parent $LogPath) ("FlockSweep-$Engine-$Context-failed.log")
        Copy-Item $LogPath $KeptLog -Force -ErrorAction SilentlyContinue
        $FailedTests += "full log kept at $KeptLog"
    }

    return [PSCustomObject]@{ Ran = $true; Total = $Total; Failed = ($Total - $Ok); Reason = ''; FailedTests = $FailedTests }
}

# -- The declared floor, read from the one place that owns it --

$CompatHeader = Join-Path $PluginRoot 'Source\Flock\Public\Misc\FlockEngineCompat.h'
if (-not (Test-Path $CompatHeader)) {
    throw "Cannot find the compat header at $CompatHeader"
}
$CompatText = Get-Content $CompatHeader -Raw
$FloorMajor = [int]([regex]::Match($CompatText, '#define\s+FLOCK_ENGINE_FLOOR_MAJOR\s+(\d+)').Groups[1].Value)
$FloorMinor = [int]([regex]::Match($CompatText, '#define\s+FLOCK_ENGINE_FLOOR_MINOR\s+(\d+)').Groups[1].Value)
$Floor = [version]"$FloorMajor.$FloorMinor"

$CeilingMajor = [int]([regex]::Match($CompatText, '#define\s+FLOCK_ENGINE_CEILING_MAJOR\s+(\d+)').Groups[1].Value)
$CeilingMinor = [int]([regex]::Match($CompatText, '#define\s+FLOCK_ENGINE_CEILING_MINOR\s+(\d+)').Groups[1].Value)
$Ceiling = [version]"$CeilingMajor.$CeilingMinor"

if ($Ceiling -lt $Floor) {
    throw "The compat header's ceiling (UE $Ceiling) is below its floor (UE $Floor)."
}

Write-Host "Declared range: UE $Floor to UE $Ceiling" -ForegroundColor Cyan
Write-Host ("Game target:    $GameTarget Win64 " + ($GameConfigurations -join ', ')) -ForegroundColor Cyan

# -- Discover installed engines --

$Engines = @{}

# Launcher installs record themselves here; source builds usually do not.
$RegPath = 'HKLM:\SOFTWARE\EpicGames\Unreal Engine'
if (Test-Path $RegPath) {
    foreach ($Key in Get-ChildItem $RegPath) {
        $Dir = (Get-ItemProperty $Key.PSPath -ErrorAction SilentlyContinue).InstalledDirectory
        if ($Dir -and (Test-Path $Dir)) {
            $Engines[(Split-Path $Key.PSPath -Leaf)] = $Dir
        }
    }
}

# Filesystem sweep, for engines the registry missed: source builds, and launcher installs relocated to
# another drive -- the launcher rewrites neither the registry nor LauncherInstalled.dat when that happens,
# so the sweep is the only thing that finds them.
if (-not $SearchRoots) {
    $SearchRoots = foreach ($Drive in [System.IO.DriveInfo]::GetDrives()) {
        if ($Drive.IsReady -and $Drive.DriveType -eq 'Fixed') {
            (Join-Path $Drive.RootDirectory.FullName 'Program Files\Epic Games')
            (Join-Path $Drive.RootDirectory.FullName 'Epic Games')
        }
    }
}
foreach ($Root in $SearchRoots) {
    foreach ($Dir in Get-ChildItem $Root -Directory -ErrorAction SilentlyContinue) {
        if ($Dir.Name -match '^UE_(\d+\.\d+)$') {
            $Engines[$Matches[1]] = $Dir.FullName
        }
    }
}

if ($Engines.Count -eq 0) {
    Write-Host 'No Unreal Engine installations found.' -ForegroundColor Red
    exit 1
}

Write-Host 'Discovered engines:' -ForegroundColor Cyan
foreach ($Version in ($Engines.Keys | Sort-Object { [version]$_ })) {
    Write-Host ("  UE {0,-5} {1}" -f $Version, $Engines[$Version])
}

# -- Build each engine at or above the floor --

$Verified        = [System.Collections.Generic.List[string]]::new()   # both targets built AND tests green
$GameBuilds      = [System.Collections.Generic.List[string]]::new()   # every game configuration built
$BuildFailed     = [System.Collections.Generic.List[string]]::new()
$GameBuildFailed = [System.Collections.Generic.List[string]]::new()
$TestFailed      = [System.Collections.Generic.List[string]]::new()
$Skipped         = [System.Collections.Generic.List[string]]::new()
$Unusable        = [System.Collections.Generic.List[string]]::new()
$AboveCeiling    = [System.Collections.Generic.List[string]]::new()   # informational: outside the claim
$ResourceStrain  = [System.Collections.Generic.List[string]]::new()   # the machine ran short while building

foreach ($Version in ($Engines.Keys | Sort-Object { [version]$_ })) {
    if ([version]$Version -lt $Floor) {
        $Skipped.Add($Version)
        continue
    }

    # Not the same thing as being below the floor: this one should have been covered and could not be.
    $BuildBat = Join-Path $Engines[$Version] 'Engine\Build\BatchFiles\Build.bat'
    if (-not (Test-Path $BuildBat)) {
        Write-Host "UE $Version : no Build.bat, cannot cover" -ForegroundColor DarkYellow
        $Unusable.Add($Version)
        continue
    }

    # Checked per engine, not only at the end: a tree that has moved invalidates everything measured after
    # it, so stopping here costs one engine rather than the rest of the sweep.
    if (-not (Test-TreeUnchanged $TreeAtStart $PluginRoot "before UE $Version")) {
        exit 2
    }

    Write-Host ''
    Write-Host "=== Building against UE $Version ===" -ForegroundColor Cyan

    Write-Host 'cleaning build artifacts from the previous engine...' -ForegroundColor DarkGray
    Remove-BuildArtifacts $ProjectDir
    foreach ($Plugin in (Get-ChildItem (Join-Path $ProjectDir 'Plugins') -Directory -ErrorAction SilentlyContinue)) {
        Remove-BuildArtifacts $Plugin.FullName
    }

    # An engine above the ceiling is still built and tested -- that is how you find out whether the
    # ceiling can be raised -- but its result is informational. It is outside what the SDK claims, so it
    # can never fail a release of a claim that does not include it.
    $InRange = ([version]$Version -le $Ceiling)

    $EditorBuild = Invoke-TimedBuild -BuildBat $BuildBat -TargetName $Target -Configuration 'Development' -Project $Project
    if (-not $EditorBuild.Succeeded) {
        Write-Host "UE $Version : BUILD FAILED" -ForegroundColor Red
        if ($InRange) { $BuildFailed.Add($Version) } else { $AboveCeiling.Add("$Version (build failed)") }
        continue
    }
    Write-Host "UE $Version : editor target built in $(Format-Duration $EditorBuild.Seconds)" -ForegroundColor DarkGray
    foreach ($Line in $EditorBuild.Strain) { $ResourceStrain.Add("$Version editor: " + $Line.Trim()) }

    # The game target, without the editor -- see the description at the top for why the editor build and
    # both test passes cannot stand in for it. The suite still runs when this fails, so the report says
    # whether the engine has a second problem.
    $FailedGameConfigurations = [System.Collections.Generic.List[string]]::new()
    $GameBuildSeconds = 0
    foreach ($Configuration in $GameConfigurations) {
        $GameBuild = Invoke-TimedBuild -BuildBat $BuildBat -TargetName $GameTarget -Configuration $Configuration -Project $Project
        $GameBuildSeconds += $GameBuild.Seconds
        foreach ($Line in $GameBuild.Strain) { $ResourceStrain.Add("$Version $Configuration`: " + $Line.Trim()) }
        if ($GameBuild.Succeeded) {
            Write-Host "UE $Version : game target built ($GameTarget Win64 $Configuration) in $(Format-Duration $GameBuild.Seconds)" -ForegroundColor Green
        } else {
            Write-Host "UE $Version : GAME BUILD FAILED ($GameTarget Win64 $Configuration)" -ForegroundColor Red
            $FailedGameConfigurations.Add($Configuration)
        }
    }
    $GameBuilt = ($FailedGameConfigurations.Count -eq 0)
    if ($GameBuilt) {
        $GameBuilds.Add("$Version ($(Format-Duration $GameBuildSeconds))")
    } elseif ($InRange) {
        $GameBuildFailed.Add("$Version (" + ($FailedGameConfigurations -join ', ') + ")")
    } else {
        $AboveCeiling.Add("$Version (game build failed: " + ($FailedGameConfigurations -join ', ') + ")")
    }

    Write-Host "UE $Version : running the automation suite..." -ForegroundColor DarkGray
    $Tests = Invoke-FlockTests -EngineDir $Engines[$Version] -Project $Project -ProjectDir $ProjectDir

    if (-not $Tests.Ran) {
        Write-Host "UE $Version : TESTS DID NOT RUN ($($Tests.Reason))" -ForegroundColor Red
        if ($InRange) { $TestFailed.Add($Version) } else { $AboveCeiling.Add("$Version (tests did not run)") }
    } elseif ($Tests.Failed -gt 0) {
        Write-Host "UE $Version : $($Tests.Failed) of $($Tests.Total) tests FAILED" -ForegroundColor Red
        foreach ($Line in $Tests.FailedTests) { Write-Host "    $Line" -ForegroundColor Red }
        if ($InRange) { $TestFailed.Add($Version) } else { $AboveCeiling.Add("$Version ($($Tests.Failed) failed)") }
    } else {
        Write-Host "UE $Version : $($Tests.Total)/$($Tests.Total) tests passed (editor)" -ForegroundColor Green

        # Second pass with the editor not hosting the suite. A subset by design -- see Invoke-FlockTests.
        $GameTests = Invoke-FlockTests -EngineDir $Engines[$Version] -Project $Project -ProjectDir $ProjectDir -GameContext
        if (-not $GameTests.Ran) {
            Write-Host "UE $Version : GAME-CONTEXT TESTS DID NOT RUN ($($GameTests.Reason))" -ForegroundColor Red
            if ($InRange) { $TestFailed.Add($Version) } else { $AboveCeiling.Add("$Version (game-context tests did not run)") }
        } elseif ($GameTests.Failed -gt 0) {
            Write-Host "UE $Version : $($GameTests.Failed) of $($GameTests.Total) game-context tests FAILED" -ForegroundColor Red
            foreach ($Line in $GameTests.FailedTests) { Write-Host "    $Line" -ForegroundColor Red }
            if ($InRange) { $TestFailed.Add($Version) } else { $AboveCeiling.Add("$Version ($($GameTests.Failed) game-context failed)") }
        } else {
            Write-Host "UE $Version : $($GameTests.Total)/$($GameTests.Total) tests passed (game context)" -ForegroundColor Green
            # Green tests alone do not verify an engine whose game build failed; that failure is already listed.
            if ($InRange) {
                if ($GameBuilt) { $Verified.Add($Version) }
            } elseif ($GameBuilt) {
                $AboveCeiling.Add("$Version (green)")
            } else {
                $AboveCeiling.Add("$Version (tests green)")
            }
        }
    }
}

# -- Report --

Write-Host ''
Write-Host '-------- Coverage --------' -ForegroundColor Cyan
if ($Verified.Count) {
    Write-Host ("verified:      " + ($Verified -join '  ') + "   (editor and game targets built + tests green)")
} else {
    Write-Host "verified:      (none)"
}
if ($GameBuilds.Count) {
    Write-Host ("game builds:   " + ($GameBuilds -join '  ') + "   ($GameTarget Win64 " + ($GameConfigurations -join ', ') + ")")
} else {
    Write-Host ("game builds:   (none)   ($GameTarget Win64 " + ($GameConfigurations -join ', ') + ")")
}
if ($BuildFailed.Count)     { Write-Host ("BUILD FAILED:  " + ($BuildFailed -join '  ')) -ForegroundColor Red }
if ($GameBuildFailed.Count) { Write-Host ("GAME BUILD FAILED: " + ($GameBuildFailed -join '  ')) -ForegroundColor Red }
if ($TestFailed.Count)      { Write-Host ("TESTS FAILED:  " + ($TestFailed -join '  ')) -ForegroundColor Red }
if ($Unusable.Count)        { Write-Host ("NOT COVERED:   " + ($Unusable -join '  ') + "  (no Build.bat)") -ForegroundColor Yellow }
if ($AboveCeiling.Count)    { Write-Host ("above ceiling: " + ($AboveCeiling -join '  ') + "  (informational)") -ForegroundColor DarkCyan }
if ($Skipped.Count)         { Write-Host ("below floor:   " + ($Skipped -join '  ')) -ForegroundColor DarkGray }
if ($ResourceStrain.Count) {
    # Not a failure: every one of these builds finished. It is here so that the next run which does fail is
    # read against a machine known to be short of memory or disk, rather than as a defect in the SDK.
    Write-Host ("resource strain: " + $ResourceStrain.Count + " line(s) -- the machine ran short while building") -ForegroundColor Yellow
    foreach ($Line in ($ResourceStrain | Select-Object -First 5)) {
        Write-Host ("  " + $Line) -ForegroundColor Yellow
    }
    if ($ResourceStrain.Count -gt 5) {
        Write-Host ("  ... and " + ($ResourceStrain.Count - 5) + " more") -ForegroundColor Yellow
    }
}
Write-Host "claim:         UE $Floor to UE $Ceiling"

# Both ends of the range are the claim, so both ends must be verified -- and so must everything between
# them that is installed. A range whose middle was never run is not a range, it is two points and a hope.
$Unproven = [System.Collections.Generic.List[string]]::new()
foreach ($End in @($Floor, $Ceiling)) {
    $Key = "$($End.Major).$($End.Minor)"
    if ($Verified -notcontains $Key) {
        $Unproven.Add($Key)
    }
}

Write-Host ''

# Checked once more before any verdict is printed: a tree that moved during the last engine would otherwise
# be reported as verified, which is the one outcome worse than reporting it as failed.
if (-not (Test-TreeUnchanged $TreeAtStart $PluginRoot 'at the end')) {
    exit 2
}

if ($Unproven.Count) {
    Write-Host ("UE " + ($Unproven -join ' and UE ') + " bound the declared range and were not verified. The claim is unproven.") -ForegroundColor Red
    exit 1
}
if ($BuildFailed.Count -or $GameBuildFailed.Count -or $TestFailed.Count) {
    Write-Host 'Engines inside the declared range failed. The claim is unproven.' -ForegroundColor Red
    exit 1
}

if ($AboveCeiling.Count) {
    Write-Host 'Engines newer than the ceiling were exercised; see "above ceiling" above before raising it.' -ForegroundColor DarkCyan
}
Write-Host "The declared claim (UE $Floor to UE $Ceiling) is verified." -ForegroundColor Green
exit 0
