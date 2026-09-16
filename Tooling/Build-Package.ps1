# Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

# ASCII only, deliberately. Windows PowerShell 5.1 reads a BOM-less .ps1 as ANSI, so any non-ASCII
# character here becomes a parser error on someone else's machine rather than a nicer-looking banner.

<#
.SYNOPSIS
    Cooks and packages the harness project, then boots what it produced.

.DESCRIPTION
    Run this before tagging a release, alongside Build-AllEngines.ps1. They answer different questions and
    neither substitutes for the other.

    Build-AllEngines.ps1 *compiles* the game target. This *cooks* it: content is cooked for the target
    platform, shaders are compiled, assets are staged into a .pak, and the result is a package a studio
    could actually ship. Everything between "the C++ compiles" and "the game runs" lives here -- a missing
    map, an asset referencing an editor-only class, a shader that fails for the cook's target, a plugin
    whose Content/ never staged. None of that can fail a compile, and none of it can fail an automation
    suite hosted by the editor.

    Packaging is then only half the claim. A package that builds and cannot start is not a package, so each
    configuration is booted after it is produced, and the report says which of the two happened.

    Development and Shipping are both cooked because they verify different things. Development still carries
    logging and honours -ExecCmds, so it is told to quit and its boot proven by the SDK's own line in the
    staged log. Shipping compiles logging out and ignores -ExecCmds, so it neither writes a line to read nor
    quits when asked: it is run for a bounded window instead, and surviving that window is the pass. That
    asymmetry is not a shortcut; it is what those two builds actually are.

.PARAMETER Project
    The .uproject to cook. Defaults to the sibling UEBuildEnviroment project.

.PARAMETER EngineDir
    The engine to cook with. Defaults to the newest install at or above the compat header's floor, because
    a package is verified against one engine -- unlike the compile sweep, which must cover the whole range.

.PARAMETER Configurations
    Which client configurations to cook. Defaults to Development and Shipping.

.PARAMETER ArchiveDirectory
    Where the staged package is written. Defaults to <project>\Packaged.

.PARAMETER SkipBoot
    Cook and stage only. The report then says the boot was not attempted rather than that it passed.

.NOTES
    Exit codes: 0 every configuration cooked and booted, 1 something failed or could not be covered.

    A cook needs far more disk than a compile. The script refuses to start below MinimumFreeGb rather than
    dying half-way through with an error that names a shader.
#>

[CmdletBinding()]
param(
    [string]   $Project,
    [string]   $EngineDir,
    [ValidateSet('Development', 'Shipping', 'DebugGame')]
    [string[]] $Configurations = @('Development', 'Shipping'),
    [string]   $ArchiveDirectory,
    [int]      $MinimumFreeGb = 25,
    [int]      $BootTimeoutSeconds = 180,
    [int]      $ShippingStayUpSeconds = 30,
    [switch]   $SkipBoot
)

$ErrorActionPreference = 'Stop'
$PluginRoot = Split-Path -Parent $PSScriptRoot

if (-not $Project) {
    $Project = Join-Path $PSScriptRoot '..\..\..\UEBuildEnviroment.uproject'
}
if (-not (Test-Path $Project)) {
    throw "Cannot find the project at $Project -- pass -Project explicitly."
}
$Project = (Resolve-Path $Project).Path
$ProjectDir = Split-Path -Parent $Project
$ProjectName = [System.IO.Path]::GetFileNameWithoutExtension($Project)

if (-not $ArchiveDirectory) {
    $ArchiveDirectory = Join-Path $ProjectDir 'Packaged'
}

# -- The engine to cook with --

if (-not $EngineDir) {
    $CompatHeader = Join-Path $PluginRoot 'Source\Flock\Public\Misc\FlockEngineCompat.h'
    if (-not (Test-Path $CompatHeader)) {
        throw "Cannot find the compat header at $CompatHeader"
    }
    $CompatText = Get-Content $CompatHeader -Raw
    $FloorMajor = [int]([regex]::Match($CompatText, '#define\s+FLOCK_ENGINE_FLOOR_MAJOR\s+(\d+)').Groups[1].Value)
    $FloorMinor = [int]([regex]::Match($CompatText, '#define\s+FLOCK_ENGINE_FLOOR_MINOR\s+(\d+)').Groups[1].Value)
    $Floor = [version]"$FloorMajor.$FloorMinor"

    # Same reasoning as the compile sweep: an engine moved off C: is in neither the registry nor
    # LauncherInstalled.dat, so every fixed drive is swept rather than trusted to announce itself.
    $Found = @{}
    foreach ($Drive in [System.IO.DriveInfo]::GetDrives()) {
        if (-not ($Drive.IsReady -and $Drive.DriveType -eq 'Fixed')) { continue }
        foreach ($Root in @('Program Files\Epic Games', 'Epic Games')) {
            $Path = Join-Path $Drive.RootDirectory.FullName $Root
            foreach ($Dir in Get-ChildItem $Path -Directory -ErrorAction SilentlyContinue) {
                if ($Dir.Name -match '^UE_(\d+\.\d+)$' -and [version]$Matches[1] -ge $Floor) {
                    $Found[$Matches[1]] = $Dir.FullName
                }
            }
        }
    }
    if ($Found.Count -eq 0) {
        throw "No Unreal Engine at or above the declared floor (UE $Floor) was found."
    }
    $Newest = ($Found.Keys | Sort-Object { [version]$_ } | Select-Object -Last 1)
    $EngineDir = $Found[$Newest]
}

$RunUAT = Join-Path $EngineDir 'Engine\Build\BatchFiles\RunUAT.bat'
if (-not (Test-Path $RunUAT)) {
    throw "Cannot find RunUAT.bat under $EngineDir"
}

# -- Refuse rather than die half-way --

$Drive = New-Object System.IO.DriveInfo((Split-Path -Qualifier $ProjectDir) + '\')
$FreeGb = [math]::Round($Drive.AvailableFreeSpace / 1GB, 1)

Write-Host "Project:        $Project" -ForegroundColor Cyan
Write-Host "Engine:         $EngineDir" -ForegroundColor Cyan
Write-Host "Archive:        $ArchiveDirectory" -ForegroundColor Cyan
Write-Host ("Configurations: " + ($Configurations -join ', ')) -ForegroundColor Cyan
Write-Host ("Free space:     $FreeGb GB on " + $Drive.Name) -ForegroundColor Cyan

if ($FreeGb -lt $MinimumFreeGb) {
    Write-Host ''
    Write-Host "Not enough disk to cook: $FreeGb GB free, $MinimumFreeGb GB wanted." -ForegroundColor Red
    Write-Host 'A cook that runs out of space fails deep inside shader compilation, naming a shader rather' -ForegroundColor Red
    Write-Host 'than the disk -- which is a long way to travel to learn this. Free some space and re-run.' -ForegroundColor Red
    exit 1
}

function Format-Duration {
    param([int] $Seconds)
    return ('{0}m {1:00}s' -f [int][math]::Floor($Seconds / 60), ($Seconds % 60))
}

# What UAT stages at the root is a small bootstrap that launches the real binary under
# <Project>\Binaries\Win64 and waits on it. Killing the bootstrap alone orphans the game: measured 2026-09-16,
# a timed-out boot check left a Shipping process burning CPU for twenty minutes after this script had already
# reported and exited. Only the whole tree ends it, so every kill here goes through taskkill /T.
function Stop-PackageProcessTree {
    param([int] $ProcessId)

    & taskkill /PID $ProcessId /T /F 2>&1 | Out-Null
}

# Boots the staged executable and reports whether it started.
#
# The two configurations are judged differently because they are genuinely different programs, not to be
# lenient with one of them:
#
#   Development keeps logging and honours -ExecCmds, so it can be told to quit and its start-up proven from
#   the SDK's own line in the staged log. A build that runs but never names the SDK is a failure, not a pass.
#
#   Shipping compiles logging out, so there is no line to read, AND it ignores -ExecCmds entirely
#   (UE_ALLOW_EXEC_COMMANDS is off in Shipping), so it never quits when asked and waiting for it to exit can
#   only ever time out -- measured 2026-09-16, which is what this comment is here to stop someone rediscovering.
#   What is left is the question that matters: does it survive start-up? The defect this check exists to catch
#   is a Shipping-only crash on boot -- the log-category call that broke every Shipping build from 1.0.0 to
#   1.11.0 was exactly that -- and a crash on boot exits immediately. So Shipping is run for a bounded window:
#   exiting early with a bad code fails, and still being alive at the end of it passes.
function Test-PackageBoots {
    param(
        [string] $ExePath,
        [string] $Configuration,
        [int]    $TimeoutSeconds,
        [int]    $StayUpSeconds
    )

    if (-not (Test-Path $ExePath)) {
        return [PSCustomObject]@{ Booted = $false; Reason = "no executable at $ExePath" }
    }

    $StagedLog = Join-Path (Split-Path -Parent $ExePath) "$ProjectName\Saved\Logs\$ProjectName.log"
    if (Test-Path $StagedLog) {
        Remove-Item $StagedLog -Force -ErrorAction SilentlyContinue
    }

    # -unattended so nothing waits for a person. The Quit is honoured by Development and ignored by Shipping.
    $Arguments = @('-unattended', '-nullrhi', '-NoSplash', '-log', '-ExecCmds=Quit')
    $Process = Start-Process -FilePath $ExePath -ArgumentList $Arguments -PassThru -WindowStyle Hidden

    if ($Configuration -eq 'Shipping') {
        if ($Process.WaitForExit($StayUpSeconds * 1000)) {
            if ($Process.ExitCode -ne 0) {
                return [PSCustomObject]@{ Booted = $false; Reason = "exited $($Process.ExitCode) during start-up" }
            }
            return [PSCustomObject]@{ Booted = $true; Reason = 'started and exited cleanly on its own' }
        }
        Stop-PackageProcessTree $Process.Id
        return [PSCustomObject]@{ Booted = $true; Reason = "started and stayed up ${StayUpSeconds}s (no log, and Shipping ignores -ExecCmds)" }
    }

    if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
        Stop-PackageProcessTree $Process.Id
        return [PSCustomObject]@{ Booted = $false; Reason = "did not exit within ${TimeoutSeconds}s" }
    }
    $ExitCode = $Process.ExitCode

    if (-not (Test-Path $StagedLog)) {
        return [PSCustomObject]@{ Booted = $false; Reason = 'the package wrote no log at all' }
    }
    $LogText = Get-Content $StagedLog -Raw -ErrorAction SilentlyContinue
    if ($LogText -match 'LogFlock') {
        return [PSCustomObject]@{ Booted = $true; Reason = 'started and the SDK reported itself in the log' }
    }
    if ($ExitCode -ne 0) {
        return [PSCustomObject]@{ Booted = $false; Reason = "exited $ExitCode and the log names no SDK line" }
    }
    # A package that runs but never mentions the SDK is not a passing result: the plugin may not have loaded
    # at all, which is exactly the class of defect a cooked build exists to catch.
    return [PSCustomObject]@{ Booted = $false; Reason = 'ran, but the log names no SDK line -- the plugin may not have loaded' }
}

# -- Cook each configuration --

$Packaged = [System.Collections.Generic.List[string]]::new()
$Failed   = [System.Collections.Generic.List[string]]::new()
$BootOk   = [System.Collections.Generic.List[string]]::new()
$BootBad  = [System.Collections.Generic.List[string]]::new()

foreach ($Configuration in $Configurations) {
    Write-Host ''
    Write-Host "=== Cooking $ProjectName Win64 $Configuration ===" -ForegroundColor Cyan

    $ArchiveFor = Join-Path $ArchiveDirectory $Configuration
    $Watch = [System.Diagnostics.Stopwatch]::StartNew()

    # Built as an array and splatted rather than written as bare tokens. PowerShell does not expand a
    # variable inside an unquoted argument that starts with a dash and contains '=': -clientconfig=$Configuration
    # reaches the process as that literal text, and UAT answers "Invalid configuration '$Configuration'" --
    # measured 2026-09-16, at the cost of a packaging run. Every element below is a quoted string, so the
    # expansion is explicit and the whole class of mistake is gone.
    $UatArguments = @(
        'BuildCookRun',
        "-project=$Project",
        '-noP4',
        '-platform=Win64',
        "-clientconfig=$Configuration",
        '-build', '-cook', '-stage', '-pak',
        '-archive',
        "-archivedirectory=$ArchiveFor",
        '-utf8output'
    )
    & $RunUAT @UatArguments | Out-Host
    $ExitCode = $LASTEXITCODE
    $Watch.Stop()
    $Elapsed = [int]$Watch.Elapsed.TotalSeconds

    if ($ExitCode -ne 0) {
        Write-Host "$Configuration : COOK FAILED (exit $ExitCode) after $(Format-Duration $Elapsed)" -ForegroundColor Red
        $Failed.Add("$Configuration (cook)")
        continue
    }

    Write-Host "$Configuration : packaged in $(Format-Duration $Elapsed)" -ForegroundColor Green
    $Packaged.Add("$Configuration ($(Format-Duration $Elapsed))")

    if ($SkipBoot) {
        continue
    }

    $Exe = Join-Path $ArchiveFor "Windows\$ProjectName.exe"
    $Boot = Test-PackageBoots -ExePath $Exe -Configuration $Configuration -TimeoutSeconds $BootTimeoutSeconds -StayUpSeconds $ShippingStayUpSeconds
    if ($Boot.Booted) {
        Write-Host "$Configuration : booted -- $($Boot.Reason)" -ForegroundColor Green
        $BootOk.Add($Configuration)
    } else {
        Write-Host "$Configuration : DID NOT BOOT -- $($Boot.Reason)" -ForegroundColor Red
        $BootBad.Add("$Configuration ($($Boot.Reason))")
    }
}

# -- Report --

Write-Host ''
Write-Host '-------- Packaging --------' -ForegroundColor Cyan
if ($Packaged.Count) { Write-Host ("packaged:      " + ($Packaged -join '  ')) } else { Write-Host 'packaged:      (none)' }
if ($SkipBoot) {
    Write-Host 'boot:          not attempted (-SkipBoot)' -ForegroundColor DarkYellow
} else {
    if ($BootOk.Count)  { Write-Host ("booted:        " + ($BootOk -join '  ')) }
    if ($BootBad.Count) { Write-Host ("DID NOT BOOT:  " + ($BootBad -join '  ')) -ForegroundColor Red }
}
if ($Failed.Count) { Write-Host ("FAILED:        " + ($Failed -join '  ')) -ForegroundColor Red }
Write-Host ("engine:        $EngineDir")

Write-Host ''
if ($Failed.Count -or $BootBad.Count) {
    Write-Host 'The packaged build is unproven.' -ForegroundColor Red
    exit 1
}
if ($SkipBoot) {
    Write-Host 'Cooked and staged. The boot was not attempted, so this is not a verified package.' -ForegroundColor DarkYellow
    exit 0
}
Write-Host ("Packaged and booted: " + ($Configurations -join ', ')) -ForegroundColor Green
exit 0
