<#
    build_mocapsmooth.ps1 - build the MocapSmooth plugin, and optionally install it.

    Shared build script for the Mad Rice UE plugins (one copy per repo, kept in step with the others).
    Only the values in the CONFIG block differ between repos.

    Purpose : RunUAT BuildPlugin into a temp package dir; -Install copies Binaries + Intermediate\Build
              from the package into the plugin folder (the folder the engine junction points at).
    Runs in : plain PowerShell. No Unreal editor needed for the build.
    Safety  : the BUILD is safe while the Unreal editor is running (it packages elsewhere).
              The INSTALL is not, because a running editor holds a lock on the DLL. -Install refuses
              while any UnrealEditor process is alive (UnrealEditor-Cmd included), by design.
              Close the editor only once whoever is using it says it is free.
    Verified: 2026-09-16 on UE 5.8.2 (BuildId 55116800).  (date, engine, BuildId). Re-verify after an engine upgrade.

    Usage:
      Tools\build_mocapsmooth.ps1 -Status       # read-only: what is built / installed / next
      Tools\build_mocapsmooth.ps1               # build only, leaves the package in $PackageDir
      Tools\build_mocapsmooth.ps1 -Install      # build then install (editor must be closed)
      Tools\build_mocapsmooth.ps1 -InstallOnly  # install an existing package without rebuilding
      Tools\build_mocapsmooth.ps1 -Engine "C:\Program Files\Epic Games\UE_5.8"

    Do not pipe the output through Select-Object -First N: it stops the pipeline (and UAT) early.
    Redirect to a log instead:  Tools\build_mocapsmooth.ps1 *> $env:TEMP\msb_build.log
#>
[CmdletBinding()]
param(
    [string] $Repo,
    [string] $Engine,
    [string] $PackageDir = "$env:TEMP\msb",
    [switch] $Status,
    [switch] $Install,
    [switch] $InstallOnly
)

$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------- CONFIG (the only per-repo part)
$PluginName = "MocapSmooth"
$ModuleDll  = "UnrealEditor-MocapSmoothEditor.dll"

# This script lives in <repo>\Tools, so the repo is its parent. No hard-coded user paths.
if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }
# The plugin folder: the repo root itself (DynamicLens, MocapSmooth), or a subfolder when the repo
# root holds data that must stay out of the engine (AssetBrowser: "Plugin\AssetBrowser").
$PluginSubdir = ""
$PluginDir = if ($PluginSubdir) { Join-Path $Repo $PluginSubdir } else { $Repo }
$uplugin   = Join-Path $PluginDir "$PluginName.uplugin"

# Engine: prefer the version the .uplugin targets, else the newest UE_* install found.
# Not needed for -Status, which is read-only and never invokes UAT.
if (-not $Engine -and -not $Status) {
    $want = $null
    $upJson = Get-Content $uplugin -Raw | ConvertFrom-Json
    if ($upJson.EngineVersion -match '^(\d+)\.(\d+)') { $want = "UE_$($Matches[1]).$($Matches[2])" }
    $roots = @("C:\Program Files\Epic Games", "D:\Program Files\Epic Games")
    $cands = @()
    foreach ($r in $roots) {
        if (Test-Path $r) { $cands += Get-ChildItem $r -Directory -Filter "UE_*" -ErrorAction SilentlyContinue }
    }
    $cands = $cands | Sort-Object Name -Descending
    $pick = $cands | Where-Object { $_.Name -eq $want } | Select-Object -First 1
    if (-not $pick) { $pick = $cands | Select-Object -First 1 }
    if (-not $pick) { throw "No Unreal Engine install found. Pass -Engine explicitly." }
    $Engine = $pick.FullName
    Write-Host "Engine: $Engine" -ForegroundColor DarkGray
}

function Test-EditorRunning {
    # UnrealEditor-Cmd (headless renders, commandlets) loads engine plugins too, so it locks the DLL.
    return [bool](Get-Process UnrealEditor, UnrealEditor-Cmd -ErrorAction SilentlyContinue)
}

# ---------------------------------------------------------------- status
# One call that answers "where is this plugin up to, and what is the next action?"
# Read-only. Safe any time, editor running or not.
if ($Status) {
    $instDll = Join-Path $PluginDir "Binaries\Win64\$ModuleDll"
    $pendDll = Join-Path $PackageDir "Binaries\Win64\$ModuleDll"

    $newestSrc = Get-ChildItem (Join-Path $PluginDir "Source") -Recurse -Include *.h,*.cpp,*.cs -ErrorAction SilentlyContinue |
                 Sort-Object LastWriteTime -Descending | Select-Object -First 1

    $inst = if (Test-Path $instDll) { Get-Item $instDll } else { $null }
    $pend = if (Test-Path $pendDll) { Get-Item $pendDll } else { $null }
    $editorUp = Test-EditorRunning

    "$PluginName status"
    "  plugin        $PluginDir"
    "  newest source {0}  {1}" -f $(if ($newestSrc) { $newestSrc.LastWriteTime.ToString('yyyy-MM-dd HH:mm') } else { '(none)' }),
                                 $(if ($newestSrc) { $newestSrc.Name } else { '' })
    "  installed DLL {0}" -f $(if ($inst) { $inst.LastWriteTime.ToString('yyyy-MM-dd HH:mm') } else { 'NOT INSTALLED' })
    "  built package {0}" -f $(if ($pend) { $pend.LastWriteTime.ToString('yyyy-MM-dd HH:mm') + "  ($PackageDir)" } else { 'none' })
    "  editor        {0}" -f $(if ($editorUp) { 'RUNNING - installing is blocked' } else { 'not running - safe to install' })

    $needsBuild   = $newestSrc -and (-not $inst -or $newestSrc.LastWriteTime -gt $inst.LastWriteTime) -and
                    (-not $pend -or $newestSrc.LastWriteTime -gt $pend.LastWriteTime)
    $needsInstall = $pend -and (-not $inst -or $pend.LastWriteTime -gt $inst.LastWriteTime)

    ""
    if ($needsBuild)        { "NEXT: source is newer than any build. Run this script with no switches." }
    elseif ($needsInstall)  { if ($editorUp) { "NEXT: a newer build is waiting. ask whether the editor is free; once it is closed, run -InstallOnly." }
                              else           { "NEXT: a newer build is waiting and the editor is closed. Run -InstallOnly." } }
    else                    { "NEXT: nothing to do. The installed DLL is up to date with the source." }
    return
}

$runUAT = Join-Path $Engine "Engine\Build\BatchFiles\RunUAT.bat"
foreach ($p in @($uplugin, $runUAT)) {
    if (-not (Test-Path $p)) { throw "Not found: $p" }
}

# ---------------------------------------------------------------- build
if (-not $InstallOnly) {

    # A writable UE_SDKS_ROOT quiets UBT's AutoSDK probe for platforms we do not target.
    # NOTE: this does NOT satisfy the .NET Framework SDK requirement. UBT needs a real NetFxSDK to
    # instantiate SwarmInterface, and without it BuildPlugin fails with a RulesError before
    # compiling anything: add the .NET Framework 4.8 SDK component to Visual Studio / Build Tools.
    if (-not $env:UE_SDKS_ROOT -or -not (Test-Path $env:UE_SDKS_ROOT)) {
        $stub = Join-Path $env:LOCALAPPDATA "$($PluginName)Build\AutoSDK"
        New-Item -ItemType Directory -Force -Path (Join-Path $stub "HostWin64") | Out-Null
        $env:UE_SDKS_ROOT = $stub
        Write-Host "UE_SDKS_ROOT -> $stub (stub)" -ForegroundColor DarkGray
    }

    # Build into a staging dir and only replace the package on success. An early version wiped
    # $PackageDir first, so a failed build destroyed the good build waiting to be installed
    # (DynamicLens, 2026-09-15). Never do that.
    $stage = "$PackageDir.new"
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }

    Write-Host "Building $PluginName -> $stage" -ForegroundColor Cyan
    & $runUAT BuildPlugin -Plugin="$uplugin" -Package="$stage" -Rocket -TargetPlatforms=Win64
    $uatExit = $LASTEXITCODE

    $stagedDll = Join-Path $stage "Binaries\Win64\$ModuleDll"
    if ($uatExit -ne 0 -or -not (Test-Path $stagedDll)) {
        Write-Host ""
        if (Test-Path (Join-Path $PackageDir "Binaries\Win64\$ModuleDll")) {
            Write-Host "Build failed. The previous good package at $PackageDir is untouched." -ForegroundColor Yellow
        }
        Write-Host "If the log says `"Could not find NetFxSDK install dir`", add the .NET Framework 4.8 SDK component to Visual Studio / Build Tools." -ForegroundColor Yellow
        throw "BuildPlugin failed with exit code $uatExit"
    }

    # success: swap staging into place
    if (Test-Path $PackageDir) { Remove-Item $PackageDir -Recurse -Force }
    Move-Item $stage $PackageDir

    $dll = Join-Path $PackageDir "Binaries\Win64\$ModuleDll"
    if (-not (Test-Path $dll)) { throw "Build reported success but $dll is missing" }
    Write-Host ("BUILD OK  {0:yyyy-MM-dd HH:mm}  {1:N0} bytes" -f (Get-Item $dll).LastWriteTime, (Get-Item $dll).Length) -ForegroundColor Green

    # BuildId check: a mismatch means the installed engine will prompt to rebuild on launch.
    $engMods = Join-Path $Engine "Engine\Binaries\Win64\UnrealEditor.modules"
    $pkgMods = Join-Path $PackageDir "Binaries\Win64\UnrealEditor.modules"
    if ((Test-Path $engMods) -and (Test-Path $pkgMods)) {
        $engId = (Get-Content $engMods -Raw | ConvertFrom-Json).BuildId
        $pkgId = (Get-Content $pkgMods -Raw | ConvertFrom-Json).BuildId
        if ($engId -eq $pkgId) { Write-Host "BuildId $pkgId matches the engine." -ForegroundColor DarkGray }
        else { Write-Host "WARNING: package BuildId $pkgId != engine BuildId $engId - the editor will ask to rebuild." -ForegroundColor Yellow }
    }
}

# ---------------------------------------------------------------- install
if (-not ($Install -or $InstallOnly)) {
    Write-Host ""
    Write-Host "Not installed. The package is waiting at $PackageDir." -ForegroundColor Yellow
    Write-Host "Installing needs the editor closed - ask first, then re-run with -InstallOnly." -ForegroundColor Yellow
    return
}

if (Test-EditorRunning) {
    throw "An Unreal editor process is running, so the plugin DLL is locked. Ask whether it is free; once it is closed, re-run with -InstallOnly."
}

if (-not (Test-Path (Join-Path $PackageDir "Binaries\Win64"))) {
    throw "No built package at $PackageDir. Run without -InstallOnly first."
}

# Only Binaries\Win64 and Intermediate\Build come across. Copying the whole package would
# overwrite Source, Content, Resources (and any other authored folder) with the packaged copies
# and blow away uncommitted work. A naive early copy also produced a nested Binaries\Binaries.
foreach ($sub in @("Binaries\Win64", "Intermediate\Build")) {
    $src = Join-Path $PackageDir $sub
    $dst = Join-Path $PluginDir $sub
    if (-not (Test-Path $src)) { continue }
    Write-Host "install $sub" -ForegroundColor Cyan
    robocopy $src $dst /E /NFL /NDL /NJH /NJS /NP | Out-Null
    # robocopy uses 0-7 for success (1 = files copied, 3 = copied + extras). Only >= 8 is a
    # real failure. Clear it afterwards so a successful copy does not leave a non-zero
    # $LASTEXITCODE for the script to exit with, which reads as a failed install.
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE) for $sub" }
    $global:LASTEXITCODE = 0
}

$installed = Join-Path $PluginDir "Binaries\Win64\$ModuleDll"
Write-Host ("INSTALLED {0:yyyy-MM-dd HH:mm}  {1}" -f (Get-Item $installed).LastWriteTime, $installed) -ForegroundColor Green
Write-Host ""
Write-Host "Next: relaunch the editor (with the plugin enabled in the project), wait for init, then:" -ForegroundColor Yellow
Write-Host "  MocapSmooth.SelfTest" -ForegroundColor Yellow

exit 0
