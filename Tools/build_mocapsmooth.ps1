<#
    build_mocapsmooth.ps1 — build the MocapSmooth plugin, and optionally install it.

    Purpose : RunUAT BuildPlugin into a temp package dir; -Install copies the result over this repo.
    Runs in : plain PowerShell. No Unreal editor needed for the build.
    Safety  : the BUILD is safe while the Unreal editor is running (it packages elsewhere).
              The INSTALL is not, because the editor holds a lock on the DLL. -Install refuses
              while UnrealEditor.exe is alive, by design. Never close the editor for someone:
              ask, wait for a yes, let them close it. See .claude/rules/editor-restarts.md.
    Verified: 2026-09-16 on UE 5.8.2 (BuildId 55116800). Re-verify after an engine upgrade.

    Usage:
      Tools\build_mocapsmooth.ps1                 # build only, leaves the package in $PackageDir
      Tools\build_mocapsmooth.ps1 -Status         # read-only: what is built / installed / next
      Tools\build_mocapsmooth.ps1 -Install        # build then install (editor must be closed)
      Tools\build_mocapsmooth.ps1 -InstallOnly    # install an existing package without rebuilding
      Tools\build_mocapsmooth.ps1 -Engine "C:\Program Files\Epic Games\UE_5.8"
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

$PluginName = "MocapSmooth"
$ModuleDll  = "UnrealEditor-MocapSmoothEditor.dll"

# This script lives in <repo>\Tools, so the repo is its parent. No hard-coded user paths.
if (-not $Repo) { $Repo = Split-Path -Parent $PSScriptRoot }

# Engine: prefer the version the .uplugin targets, else the newest UE_* install found.
# Not needed for -Status, which is read-only and never invokes UAT.
if (-not $Engine -and -not $Status) {
    $want = $null
    $upJson = Get-Content (Join-Path $Repo "$PluginName.uplugin") -Raw | ConvertFrom-Json
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
    return [bool](Get-Process UnrealEditor -ErrorAction SilentlyContinue)
}

# ---------------------------------------------------------------- status
# One call that answers "where is this plugin up to, and what is the next action?"
# Read-only. Safe any time, editor running or not.
if ($Status) {
    $instDll = Join-Path $Repo "Binaries\Win64\$ModuleDll"
    $pendDll = Join-Path $PackageDir "Binaries\Win64\$ModuleDll"

    $newestSrc = Get-ChildItem (Join-Path $Repo "Source") -Recurse -Include *.h,*.cpp,*.cs -ErrorAction SilentlyContinue |
                 Sort-Object LastWriteTime -Descending | Select-Object -First 1

    $inst = if (Test-Path $instDll) { Get-Item $instDll } else { $null }
    $pend = if (Test-Path $pendDll) { Get-Item $pendDll } else { $null }
    $editorUp = Test-EditorRunning

    "$PluginName status"
    "  repo          $Repo"
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
    elseif ($needsInstall)  { if ($editorUp) { "NEXT: a newer build is waiting. ASK DYLAN to close the editor, then -InstallOnly." }
                              else           { "NEXT: a newer build is waiting and the editor is closed. Run -InstallOnly." } }
    else                    { "NEXT: nothing to do. The installed DLL is up to date with the source." }
    return
}

$uplugin = Join-Path $Repo "$PluginName.uplugin"
$runUAT  = Join-Path $Engine "Engine\Build\BatchFiles\RunUAT.bat"
foreach ($p in @($uplugin, $runUAT)) {
    if (-not (Test-Path $p)) { throw "Not found: $p" }
}


# ---------------------------------------------------------------- build
if (-not $InstallOnly) {

    # A writable UE_SDKS_ROOT quiets UBT's AutoSDK probe for platforms we do not target.
    # NOTE: this does NOT satisfy the .NET Framework SDK requirement. UBT needs a real NetFxSDK to
    # instantiate SwarmInterface, and without it BuildPlugin fails with a RulesError before
    # compiling anything. See .claude/rules/build-and-install.md.
    if (-not $env:UE_SDKS_ROOT -or -not (Test-Path $env:UE_SDKS_ROOT)) {
        $stub = Join-Path $env:LOCALAPPDATA "MocapSmoothBuild\AutoSDK"
        New-Item -ItemType Directory -Force -Path (Join-Path $stub "HostWin64") | Out-Null
        $env:UE_SDKS_ROOT = $stub
        Write-Host "UE_SDKS_ROOT -> $stub (stub)" -ForegroundColor DarkGray
    }

    # Build into a staging dir and only replace the package on success, so a failed build can
    # never destroy a good build that is waiting to be installed.
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
        Write-Host "If the log says `"Could not find NetFxSDK install dir`", see .claude/rules/build-and-install.md." -ForegroundColor Yellow
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
    Write-Host "Installing needs the editor closed - ASK FIRST, then re-run with -InstallOnly." -ForegroundColor Yellow
    return
}

if (Test-EditorRunning) {
    throw "UnrealEditor.exe is running, so the plugin DLL is locked. Ask Dylan to close the editor, then re-run with -InstallOnly. Do not close it for him."
}

if (-not (Test-Path (Join-Path $PackageDir "Binaries\Win64"))) {
    throw "No built package at $PackageDir. Run without -InstallOnly first."
}

# Only Binaries\Win64 and Intermediate\Build come across. Copying the whole package would
# overwrite Source with the packaged copy and blow away uncommitted work.
foreach ($sub in @("Binaries\Win64", "Intermediate\Build")) {
    $src = Join-Path $PackageDir $sub
    $dst = Join-Path $Repo $sub
    if (-not (Test-Path $src)) { continue }
    Write-Host "install $sub" -ForegroundColor Cyan
    robocopy $src $dst /E /NFL /NDL /NJH /NJS /NP | Out-Null
    if ($LASTEXITCODE -ge 8) { throw "robocopy failed ($LASTEXITCODE) for $sub" }
}

$installed = Join-Path $Repo "Binaries\Win64\$ModuleDll"
Write-Host ("INSTALLED {0:yyyy-MM-dd HH:mm}  {1}" -f (Get-Item $installed).LastWriteTime, $installed) -ForegroundColor Green
Write-Host ""
Write-Host "Next: relaunch the editor, then run  MocapSmooth.SelfTest  in the console." -ForegroundColor Yellow
