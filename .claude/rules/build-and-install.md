# Build and install — when X, do Y

The full recipe and the failure table live here; the ask-first gate is `editor-restarts.md`.

## Two layouts, one rule each

| Layout | Who | Build with | Restart? |
|---|---|---|---|
| **Engine plugin** (what Dylan does): this repo junctioned at `UE_5.8\Engine\Plugins\Marketplace\MocapSmooth` | every 5.8 project on the machine, no per-project copy, no toolchain in the projects | `Tools\build_mocapsmooth.ps1` (RunUAT BuildPlugin, packages elsewhere, safe with the editor up) then `-InstallOnly` with the editor closed | yes, every C++ change |
| **Project plugin**: a clone or release zip at `<Project>/Plugins/MocapSmooth` | collaborators, CI, anyone without engine write access | first launch offers to compile; or `Build.bat <Project>Editor Win64 Development -Project=...` with the editor closed; or Live Coding for body-only edits | only for reflected changes, see `../refs/live-coding.md` |

**Never both at once.** A project that sees the plugin at an engine path *and* a project path
fails to load it. Delete the project copy before junctioning the engine.

## The engine-layout cycle

```powershell
Tools\build_mocapsmooth.ps1 -Status       # read-only: where is this up to, what is next
Tools\build_mocapsmooth.ps1               # build only, packages to %TEMP%\msb, safe with the editor up
Tools\build_mocapsmooth.ps1 -InstallOnly  # install, only after Dylan has closed the editor
```

Build first, always. Then ask. Then install. Then he relaunches. Then `MocapSmooth.SelfTest`.

## What the script does, and why each part exists

- **Packages to a temp dir** rather than building in place, so a running editor never blocks a build.
  This is the one real advantage of the engine layout: a project build (`Build.bat`) is refused
  while Live Coding is active in a running editor, `BuildPlugin` is not.
- **Derives the repo from its own location** (`<repo>\Tools`) and picks the engine from the
  `EngineVersion` in `MocapSmooth.uplugin`, falling back to the newest `UE_*` install. No
  machine-specific paths, because this repo is public.
- **Sets `UE_SDKS_ROOT` to a writable stub** when unset, which quiets UBT's AutoSDK probe for
  platforms we do not target. It does **not** substitute for the .NET Framework SDK (below).
- **Builds into `<package>.new` and only swaps it into place on success**, so a failed build cannot
  destroy a good package waiting to be installed.
- **Installs only `Binaries\Win64` and `Intermediate\Build`** by robocopy, never `Source/`, so an
  uncommitted edit cannot be overwritten by the packaged copy.
- **Checks `UnrealEditor.modules`.** A matching `BuildId` on both sides means the installed engine
  will load the DLL without prompting to rebuild. UE 5.8.2 Launcher build: `BuildId 55116800`.

## Prerequisite: the .NET Framework SDK

`BuildPlugin` (and any `Build.bat`) fails before compiling anything when no .NET Framework SDK is
present:

```
Unable to instantiate module 'SwarmInterface': Could not find NetFxSDK install dir;
Install a version of .NET Framework SDK at 4.6.0 or higher.
```

SwarmInterface is an editor-target dependency, nothing to do with this plugin. On a fresh machine
add the **.NET Framework 4.8 SDK** component to Visual Studio / Build Tools
(`Microsoft.Net.Component.4.8.SDK` and `.TargetingPack`). Verify: `NETFXSDK.8` under
`HKLM\SOFTWARE\WOW6432Node\Microsoft\Microsoft SDKs`. Already installed on Dylan's machine,
2026-09-15 (the exact installer command is in the DynamicLens repo's build rule).

## After installing

Relaunch, then in the console:

```
MocapSmooth.SelfTest
```

All six checks must print `ok` (expected numbers in `../refs/ue-animation-modifier-build.md`).
Then apply to a scratch duplicate of a real take and confirm re-apply is non-compounding and
Revert lands on 0.0000°.

## Failure modes

| Symptom | Cause | Fix |
|---|---|---|
| `Could not find NetFxSDK install dir`, RulesError | no .NET Framework SDK | the prerequisite above |
| `Unable to build while Live Coding is active` | you ran `Build.bat` against a project while its editor is open | use `Tools\build_mocapsmooth.ps1` (engine layout) or Ctrl+Alt+F11 (project layout) |
| Link error, cannot write the DLL | editor running, `-InstallOnly` bypassed | close it, asking first |
| Editor prompts "missing modules, rebuild?" on launch | `BuildId` mismatch (engine hotfix, or the DLL was built on another engine) | rebuild and reinstall |
| Plugin silently absent from the Add Modifier menu | loaded at both an engine path and a project path | delete the project copy |
| `Mocap Smooth` applies but the asset will not save | the *Python* modifier class is attached, not this one | right-click → Remove Modifier(s), answer **No** to revert; see `../refs/ue-animation-modifier-build.md` |
| UHT: *Found string constant when expecting ',' or ')'* | multi-line string literal in `meta=(ToolTip=...)` | write the tooltip as a `/** */` doc comment |
