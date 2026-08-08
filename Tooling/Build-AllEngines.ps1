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

    It cannot run in GitHub-hosted CI: those runners have no engine. The no-engine half of the checks
    lives in .github/workflows/consistency.yml.

.PARAMETER Project
    The .uproject used to drive the build. Defaults to the sibling UEBuildEnviroment project.

.PARAMETER SearchRoots
    Directories to sweep for UE_x.y installs. Defaults to 'Program Files\Epic Games' and 'Epic Games' on
    every fixed drive, because an engine moved off C: records itself nowhere the registry or
    LauncherInstalled.dat can see -- and an engine the script cannot see is one the report silently
    omits, which is the failure mode this script exists to prevent.

.EXAMPLE
    ./Tooling/Build-AllEngines.ps1
#>

[CmdletBinding()]
param(
    [string]   $Project,
    [string]   $Target = 'UEBuildEnviromentEditor',
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

    return [PSCustomObject]@{ Ran = $true; Total = $Total; Failed = ($Total - $Ok); Reason = '' }
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

$Verified    = [System.Collections.Generic.List[string]]::new()   # built AND tests green
$BuildFailed = [System.Collections.Generic.List[string]]::new()
$TestFailed  = [System.Collections.Generic.List[string]]::new()
$Skipped     = [System.Collections.Generic.List[string]]::new()
$Unusable    = [System.Collections.Generic.List[string]]::new()
$AboveCeiling = [System.Collections.Generic.List[string]]::new() # informational: outside the claim

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

    & $BuildBat $Target Win64 Development -project="$Project" -WaitMutex
    if ($LASTEXITCODE -ne 0) {
        Write-Host "UE $Version : BUILD FAILED" -ForegroundColor Red
        if ($InRange) { $BuildFailed.Add($Version) } else { $AboveCeiling.Add("$Version (build failed)") }
        continue
    }

    Write-Host "UE $Version : built. Running the automation suite..." -ForegroundColor DarkGray
    $Tests = Invoke-FlockTests -EngineDir $Engines[$Version] -Project $Project -ProjectDir $ProjectDir

    if (-not $Tests.Ran) {
        Write-Host "UE $Version : TESTS DID NOT RUN ($($Tests.Reason))" -ForegroundColor Red
        if ($InRange) { $TestFailed.Add($Version) } else { $AboveCeiling.Add("$Version (tests did not run)") }
    } elseif ($Tests.Failed -gt 0) {
        Write-Host "UE $Version : $($Tests.Failed) of $($Tests.Total) tests FAILED" -ForegroundColor Red
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
            if ($InRange) { $TestFailed.Add($Version) } else { $AboveCeiling.Add("$Version ($($GameTests.Failed) game-context failed)") }
        } else {
            Write-Host "UE $Version : $($GameTests.Total)/$($GameTests.Total) tests passed (game context)" -ForegroundColor Green
            if ($InRange) { $Verified.Add($Version) } else { $AboveCeiling.Add("$Version (green)") }
        }
    }
}

# -- Report --

Write-Host ''
Write-Host '-------- Coverage --------' -ForegroundColor Cyan
if ($Verified.Count) {
    Write-Host ("verified:      " + ($Verified -join '  ') + "   (built + tests green)")
} else {
    Write-Host "verified:      (none)"
}
if ($BuildFailed.Count)  { Write-Host ("BUILD FAILED:  " + ($BuildFailed -join '  ')) -ForegroundColor Red }
if ($TestFailed.Count)   { Write-Host ("TESTS FAILED:  " + ($TestFailed -join '  ')) -ForegroundColor Red }
if ($Unusable.Count)     { Write-Host ("NOT COVERED:   " + ($Unusable -join '  ') + "  (no Build.bat)") -ForegroundColor Yellow }
if ($AboveCeiling.Count) { Write-Host ("above ceiling: " + ($AboveCeiling -join '  ') + "  (informational)") -ForegroundColor DarkCyan }
if ($Skipped.Count)      { Write-Host ("below floor:   " + ($Skipped -join '  ')) -ForegroundColor DarkGray }
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
if ($Unproven.Count) {
    Write-Host ("UE " + ($Unproven -join ' and UE ') + " bound the declared range and were not verified. The claim is unproven.") -ForegroundColor Red
    exit 1
}
if ($BuildFailed.Count -or $TestFailed.Count) {
    Write-Host 'Engines inside the declared range failed. The claim is unproven.' -ForegroundColor Red
    exit 1
}

if ($AboveCeiling.Count) {
    Write-Host 'Engines newer than the ceiling were exercised; see "above ceiling" above before raising it.' -ForegroundColor DarkCyan
}
Write-Host "The declared claim (UE $Floor to UE $Ceiling) is verified." -ForegroundColor Green
exit 0
