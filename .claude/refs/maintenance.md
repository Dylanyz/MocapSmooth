# Maintenance — keeping these docs true

Last updated: 2026-10-06

MocapSmooth is small, but its value is the *documentation*: measured facts about other people's
smoothers and about UE's animation API. Those go stale quietly. After any session that changed the
plugin or learned something about it, run the shared wrap-up checklist in the plugin hub
(`refs/maintenance.md`: README and refs agree, prune, project facts stay out, script `Verified:` lines),
then these MocapSmooth items. Both are mandatory.

## MocapSmooth checklist

1. **New control added?** Add it to `README.md`'s property table with what it is *for*, not just
   its range, and keep the tooltip in `MocapSmoothModifier.h` saying the same thing.
2. **Filter maths changed?** Change `Tools/smooth_core.py` and `Source/.../MocapSmoothFilter.cpp`
   in the same commit, re-vendor `smooth_core.py` into `Tools/python_fallback/mocap_smooth/`, and
   update the expected `SelfTest` numbers in `ue-animation-modifier-build.md`.
3. **New UE API fact or trap** goes in `ue-implementation.md` (API) or
   `ue-animation-modifier-build.md` (build/traps), with the symptom that revealed it; an engine-true
   one also goes to `/ue-docs`. These are the expensive facts, the ones most likely to be
   "simplified" away by a later agent.
4. **A new smoothing system characterised** (Move, Vicon, Xsens, Blender, Maya…) gets its own
   `refs/<vendor>-*.md` with method, numbers and a **measured / from source / inferred** tag on every
   claim, plus a trademark line in `NOTICE` in the same commit.
5. **Which takes a film smoothed at which slider** belongs in that film project's own `.claude/`.

## Watch list

- **Engine version.** Everything here is UE 5.8. Moving to 5.9 needs a new junction, a rebuild,
  and a re-check of `GetNumberOfKeys() == GetNumberOfFrames() + 1` and the
  `IAnimationDataModel::GetBoneTrackTransforms` signature, the two version-fragile facts.
- **Open question from the measurement:** the Rokoko spec was measured on a 60 fps clip; whether
  the cutoff scales with capture rate was not verified (`rokoko-measurement.md`).
- **Python fallback** carries two known bugs (off-by-one key count, unguarded capture path) and is
  kept only for toolchain-less projects. If it is ever fixed, say so in
  `ue-animation-modifier-build.md`; if it is ever dropped, delete `Tools/python_fallback/` and the
  fallback paragraphs, not just the folder.

## Next (for the next agent working here)

- **Visible credit in the plugin (CPAL section 14 / LICENSE Exhibit B), deferred 2026-10-06.** Show
  "Dylan Gitalis · https://youtube.com/@madricetv" with a link to https://github.com/Dylanyz/MocapSmooth
  prominently in the plugin's UI (the Mocap Smooth modifier's Details panel) and log it once at module
  startup. C++ change: build, then install when the editor is free. Details: plugin hub `refs/licensing.md`.

## Log

- 2026-10-06 — **Moved into the plugin hub** (`Desktop\Coding\ueplugins\MocapSmooth`, junction
  re-pointed). The shared build/install, editor-restart and update-runbook rules were removed from
  `.claude/rules/` (now plugin hub `refs/` and `/ue-agent-control` `launch-close.md`); the
  MocapSmooth-only parts moved into `CLAUDE.md` "Iterating". `Tools/build_mocapsmooth.ps1`
  regenerated from the shared template (gains the robocopy exit-code guard and `UnrealEditor-Cmd`
  detection).
- 2026-09-16 — **Repo created; the `/mocap-smoothing` user-scope skill retired.** Everything the
  skill held (SKILL.md → `CLAUDE.md`, `references/` → `.claude/refs/`, `scripts/` and `templates/`
  → `Tools/`, `ue/mocap_smooth/` → `Tools/python_fallback/`, `ue/MocapSmooth/` → the repo root)
  now lives with the code and ships with the open-source repo, at parity with DynamicLens. Engine
  junction replaces the per-project copy in CitySample; `Tools/build_mocapsmooth.ps1` added.
  Descriptor bumped to 1.0.0 with docs, support and author URLs, `EngineVersion` 5.8.0,
  `Installed: true`, `PlatformAllowList: Win64`.
- 2026-09-15 — C++ plugin built and verified on CitySample (UE 5.8.2): SelfTest all pass, 8 Rokoko
  takes migrated from the Python version without reimport. Two Python bugs found and recorded.
- 2026-09-15 — Blueprint modifier route built, then superseded the same day by the C++ port because
  Blueprint variable tooltips and slider ranges cannot be set from Python or any MCP toolset.
- 2026-09-14/15 — Rokoko Studio Preview filter reverse-engineered and measured; UE Curve Editor
  (Driscoll) method characterised from engine source; `smooth_core.py` written and validated.
