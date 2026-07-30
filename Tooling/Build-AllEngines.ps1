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

.EXAMPLE
    ./Tooling/Build-AllEngines.ps1
#>

[CmdletBinding()]
param(
    [string] $Project = (Join-Path $PSScriptRoot '..\..\..\UEBuildEnviroment.uproject'),
    [string] $Target  = 'UEBuildEnviromentEditor'
)

$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent $PSScriptRoot

# -- The declared floor, read from the one place that owns it --

$CompatHeader = Join-Path $PluginRoot 'Source\Flock\Public\Misc\FlockEngineCompat.h'
if (-not (Test-Path $CompatHeader)) {
    throw "Cannot find the compat header at $CompatHeader"
}
$CompatText = Get-Content $CompatHeader -Raw
$FloorMajor = [int]([regex]::Match($CompatText, '#define\s+FLOCK_ENGINE_FLOOR_MAJOR\s+(\d+)').Groups[1].Value)
$FloorMinor = [int]([regex]::Match($CompatText, '#define\s+FLOCK_ENGINE_FLOOR_MINOR\s+(\d+)').Groups[1].Value)
$Floor = [version]"$FloorMajor.$FloorMinor"

Write-Host "Declared floor: UE $Floor" -ForegroundColor Cyan

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

# Filesystem sweep, for engines the registry missed.
foreach ($Dir in Get-ChildItem 'C:\Program Files\Epic Games' -Directory -ErrorAction SilentlyContinue) {
    if ($Dir.Name -match '^UE_(\d+\.\d+)$') {
        $Engines[$Matches[1]] = $Dir.FullName
    }
}

if ($Engines.Count -eq 0) {
    Write-Host 'No Unreal Engine installations found.' -ForegroundColor Red
    exit 1
}

# -- Build each engine at or above the floor --

$Passed  = [System.Collections.Generic.List[string]]::new()
$Failed  = [System.Collections.Generic.List[string]]::new()
$Skipped = [System.Collections.Generic.List[string]]::new()

foreach ($Version in ($Engines.Keys | Sort-Object { [version]$_ })) {
    if ([version]$Version -lt $Floor) {
        $Skipped.Add($Version)
        continue
    }

    $BuildBat = Join-Path $Engines[$Version] 'Engine\Build\BatchFiles\Build.bat'
    if (-not (Test-Path $BuildBat)) {
        Write-Host "UE $Version : no Build.bat, skipping" -ForegroundColor DarkYellow
        $Skipped.Add($Version)
        continue
    }

    Write-Host ''
    Write-Host "=== Building against UE $Version ===" -ForegroundColor Cyan
    & $BuildBat $Target Win64 Development -project="$Project" -WaitMutex
    if ($LASTEXITCODE -eq 0) {
        $Passed.Add($Version)
    } else {
        $Failed.Add($Version)
    }
}

# -- Report --

Write-Host ''
Write-Host '-------- Coverage --------' -ForegroundColor Cyan
if ($Passed.Count) {
    Write-Host ("covered:      " + ($Passed -join '  '))
} else {
    Write-Host "covered:      (none)"
}
if ($Failed.Count)  { Write-Host ("FAILED:       " + ($Failed -join '  ')) -ForegroundColor Red }
if ($Skipped.Count) { Write-Host ("below floor:  " + ($Skipped -join '  ')) -ForegroundColor DarkGray }
Write-Host "claim:        UE $Floor and newer"

# The floor is the claim, so the floor is what must be verified. Anything above it that fails is a
# warning -- it tells you what would break if you widened, which is exactly the signal you want before
# widening, not a reason to block today's release.
$FloorKey = "$($Floor.Major).$($Floor.Minor)"
if ($Passed -notcontains $FloorKey) {
    Write-Host ''
    Write-Host "UE $FloorKey is the declared floor and was not verified. The claim is unproven." -ForegroundColor Red
    exit 1
}

if ($Failed.Count) {
    Write-Host ''
    Write-Host 'The floor is verified. Newer engines above failed -- do not widen the claim to include them.' -ForegroundColor Yellow
}

Write-Host ''
Write-Host 'The declared claim is verified.' -ForegroundColor Green
exit 0
