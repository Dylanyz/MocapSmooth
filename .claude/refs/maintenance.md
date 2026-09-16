# Maintenance — keeping these docs true

Last updated: 2026-09-16

MocapSmooth is small, but its value is the *documentation*: measured facts about other people's
smoothers and about UE's animation API. Those go stale quietly. The checklist below is MANDATORY
after any session that changed the plugin or learned something about it.

## Wrap-up checklist

1. **Plugin behaviour changed?** Update the affected file in `.claude/refs/` *and* `README.md`.
   The README is the public explanation; these refs are the working notes. They must not disagree.
2. **New control added?** Add it to `README.md`'s property table with what it is *for*, not just
   its range, and keep the tooltip in `MocapSmoothModifier.h` saying the same thing.
3. **Filter maths changed?** Change `Tools/smooth_core.py` and `Source/.../MocapSmoothFilter.cpp`
   in the same commit, re-vendor `smooth_core.py` into `Tools/python_fallback/mocap_smooth/`, and
   update the expected `SelfTest` numbers in `ue-animation-modifier-build.md`.
4. **New UE API fact or trap** goes in `ue-implementation.md` (API) or
   `ue-animation-modifier-build.md` (build/traps), with the symptom that revealed it. These are the
   expensive facts, and the ones most likely to be "simplified" away by a later agent.
5. **A new smoothing system characterised** (Move, Vicon, Xsens, Blender, Maya…) gets its own
   `refs/<vendor>-*.md` with method, numbers and a **measured / from source / inferred** tag on every
   claim, plus a trademark line in `NOTICE` in the same commit.
6. **A rule Dylan states about how to work** goes in `.claude/rules/`, quoted, with the why.
   Refs describe the plugin; rules constrain the agent.
7. **Build recipe changed or verified on a new engine?** Update the header block and the `Verified:`
   date in `Tools/build_mocapsmooth.ps1`, and the `BuildId` in `../rules/build-and-install.md`.
8. **Prune** what the session disproved. Delete it, do not strike it through, and log it below.
9. **Project-specific facts never live here.** Which takes a film smoothed at which slider belongs
   in that film project's own `.claude/`. This repo documents the plugin only.

## Watch list

- **Engine version.** Everything here is UE 5.8, junction at
  `UE_5.8\Engine\Plugins\Marketplace\MocapSmooth`. Moving to 5.9 needs a new junction, a rebuild,
  and a re-check of `GetNumberOfKeys() == GetNumberOfFrames() + 1` and the
  `IAnimationDataModel::GetBoneTrackTransforms` signature, the two version-fragile facts.
- **Open question from the measurement:** the Rokoko spec was measured on a 60 fps clip; whether
  the cutoff scales with capture rate was not verified (`rokoko-measurement.md`).
- **Python fallback** carries two known bugs (off-by-one key count, unguarded capture path) and is
  kept only for toolchain-less projects. If it is ever fixed, say so in
  `ue-animation-modifier-build.md`; if it is ever dropped, delete `Tools/python_fallback/` and the
  fallback paragraphs, not just the folder.
- **Licence.** Apache-2.0. MPL-2.0 is a one-file swap if Dylan ever wants modifications forced
  back open.
- **Discoverability.** A `CLAUDE.md` in this repo only loads when the working directory is inside
  it. Agents working in a *film* project need a pointer there; CitySample has one. Add one to each
  new film project rather than duplicating any of this content into it.

## Log

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
