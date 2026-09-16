# Iterating without restarting: what is actually possible

The honest answer to "can I work on MocapSmooth (or DynamicLens) without restarting the editor?",
with what was tested on 2026-09-15 rather than assumed. The DynamicLens repo holds the same
analysis with its own measurements; the constraints are identical because both are C++ plugins on a
Launcher-installed UE 5.8.

## What Live Coding does and does not do

Unreal's Live Coding (Ctrl+Alt+F11) recompiles changed C++ and patches it into the running editor.

**It handles:** changes inside existing function bodies. Filter maths, thresholds, region rules,
the cache's signature logic, log text. For this plugin that is a large share of the tuning work,
and it patches in seconds.

**It cannot handle:** adding, removing or retyping a `UPROPERTY` or `UFUNCTION`, changing a class's
layout, or adding new reflected types. Unreal fixes reflection data and object layout when a module
loads, and nothing rewrites them in a live process. A new slider on the modifier is a new
`UPROPERTY`, so it needs a restart in any layout.

**It never touches an engine plugin on an installed engine.** Live Coding only compiles modules it
considers part of the *project* (game modules and project plugins). A plugin under
`Engine/Plugins/Marketplace` is engine territory, and a Launcher engine is not rebuildable from
source, so Ctrl+Alt+F11 ignores it entirely.

## The two layouts, measured

| | Engine plugin (junction, what Dylan does) | Project plugin (`<Project>/Plugins/MocapSmooth`) |
|---|---|---|
| Build while the editor is open | **yes** — `Tools\build_mocapsmooth.ps1` uses `RunUAT BuildPlugin`, which builds against its own host project | no — `Build.bat` fails with *"Unable to build while Live Coding is active"*; you use Ctrl+Alt+F11 instead |
| Body-only change without restarting | no | **yes**, via Live Coding |
| New `UPROPERTY` / new class | restart | restart |
| Projects needing a C++ toolchain | none, built once centrally | every project carrying the plugin becomes a code project |
| Two projects on different plugin versions | not possible | possible |
| Same plugin at both paths | **fails to load** — never do it | |

## Why the engine layout stays for Dylan

21 of the 22 Unreal projects on his machine are Blueprint-only; only CitySample is C++. A project
plugin would turn each of those into a code project that prompts to compile on first launch and
after every engine hotfix, and refuses to open without a working toolchain. Against that, the gain
is hot-patching for the subset of changes that are body-only.

Measured on the 9950X3D: a full clean `BuildPlugin` of this one module is well under two minutes,
and the compile itself is seconds. Build time is not the friction. The restart is, and the restart
is unavoidable for the changes that add controls.

**Decision, 2026-09-16: keep the engine-wide junction, same as DynamicLens.** Revisit only if the
work shifts toward tuning existing maths rather than adding controls. If that day comes, the
restart-free workflow is: delete the junction, clone this repo into CitySample's `Plugins/`, edit,
Ctrl+Alt+F11, and re-junction when done. Do not half-do it.

## What softens the restart in practice

- **`Tools/smooth_core.py` is the filter, line-for-line.** Tune the maths there first, in plain
  Python with numpy, against the `.npz` raw of a real take — no editor involved. Port to C++ once it
  reads right, build, and take the one restart. `MocapSmooth.SelfTest` then confirms the port.
- **The Python fallback** (`Tools/python_fallback/mocap_smooth/`) runs in the live editor with no
  build at all. It is slower and carries two known bugs, but for a "does this cutoff look right on
  this take" question it answers without a restart.
- **The build is safe while he works.** Build early, let the package wait in `%TEMP%\msb`, and
  install on whatever restart he was going to do anyway. That is the whole point of
  `../rules/updating-the-plugin.md`.

## Do not reach for the DLL-rename trick

A new DLL *can* be swapped in under a running editor by renaming the loaded one aside. It is not
used here because Live Coding is part of Dylan's workflow on this machine and would load the new
DLL into a process running the old one. Same reasoning as DynamicLens's `hot-swap.md`.
