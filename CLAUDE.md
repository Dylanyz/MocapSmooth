# MocapSmooth — zero-phase mocap smoothing for Unreal, measured not guessed

A C++ UE 5.8 editor plugin by Dylan Gitalis (Mad Rice). Right-click an AnimSequence → Animation
Modifier(s) → Add → **Mocap Smooth**, set a strength, Apply. It reproduces Rokoko Studio Preview's
smoothing filter exactly (reverse-engineered, then measured to 0.016–0.19° mean error) and never
compounds: every apply reads from a protected copy of the raw take. Open source, Apache-2.0, at
https://github.com/Dylanyz/MocapSmooth.

**This repo *is* the installed plugin.** `Engine\Plugins\Marketplace\MocapSmooth` in the engine
install is a **directory junction to this folder**, not a copy. Docs and `Tools/` edits are live;
only C++ needs a build, and a build needs a restart.

## Hard rules

1. **Never restart, close or relaunch the Unreal editor without asking Dylan and getting a yes.**
   He is usually mid-shot with unsaved work. `.claude/rules/editor-restarts.md`.
2. **Never touch `Saved/` or `Intermediate/`**, here or in any Unreal project. `Saved/MocapSmooth/`
   in a project holds the protected originals; deleting it loses the way back to the raw take.
3. **Ask before deleting anything**, with specifics on what and why.
4. **No Rokoko or Epic code, ever.** The spec here is a clean-room behavioural description.
   `.claude/rules/licensing-and-credits.md`.
5. **`Tools/smooth_core.py` and `MocapSmoothFilter.cpp` are the same filter, line for line.**
   Change one, change the other in the same commit, and re-run `MocapSmooth.SelfTest`.
6. **Never filter in place without a way back**, and never let a capture overwrite a cached
   original. The refusal guard in `MocapSmoothCache` exists for that; do not weaken it.

## Start here (progressive disclosure)

- The one-line answer, filter choice, working rules → *below*.
- The DSP: Butterworth vs Gaussian, zero phase, quaternions, ringing, edge padding → `.claude/refs/filter-math.md`
- How Rokoko Studio Preview is built and what it exposes → `.claude/refs/rokoko-studio-preview.md`
- The experiment that pinned order, phase and cutoff, with numbers → `.claude/refs/rokoko-measurement.md`
- UE's Curve Editor Butterworth (Driscoll) and what UE's FFT filter actually computes → `.claude/refs/driscoll-ue-curve-editor.md`
- UE 5.8 API facts, the modifier design, region masking, non-compounding re-apply → `.claude/refs/ue-implementation.md`
- **Building, installing, every trap hit, what to verify** → `.claude/refs/ue-animation-modifier-build.md`
- Every approach weighed and why each was kept or dropped → `.claude/refs/alternatives-considered.md`
- Can it be iterated without a restart? (Live Coding, engine vs project layout) → `.claude/refs/live-coding.md`
- Keeping these docs true → `.claude/refs/maintenance.md`

Behaviour rules auto-load from `.claude/rules/`: editor restarts, build/install, the update runbook,
licensing. Read them; they are the ones that bite.

## Layout

| Path | What |
|---|---|
| `MocapSmooth.uplugin` | descriptor: one `Editor` module, Win64, `Installed: true`, `EngineVersion 5.8.0` |
| `Source/MocapSmoothEditor/` | the C++: `MocapSmoothFilter` (DSP + `SelfTest`), `MocapSmoothRegions` (bone regions by hierarchy walk), `MocapSmoothCache` (protected original, FBX fingerprint, refusal guard), `MocapSmoothModifier` (the `UAnimationModifier`: properties, tooltips, apply/revert), module + `MocapSmooth.SelfTest` console command |
| `Tools/smooth_core.py` | **the reference filter**, numpy only, self-testing. Runs in UE, Blender, plain Python |
| `Tools/rokoko_probe.js` | drives a local Rokoko install over JSON-RPC to re-verify the spec |
| `Tools/build_mocapsmooth.ps1` | build (safe with the editor up), `-Status`, `-InstallOnly` |
| `Tools/mocap-smoothing.config.template.json` | optional per-project record of takes/cutoffs, for projects that want values outside the modifier's Details panel |
| `Tools/python_fallback/mocap_smooth/` | the same design in editor Python + an Animation Modifier Blueprint, for projects with no C++ toolchain. Slower, two known bugs |
| `Binaries/`, `Intermediate/` | build products, gitignored, never committed; releases carry them as attachments |

## The answer, in one line

Rokoko Studio Preview's smoothing is a **zero-phase forward-backward (`filtfilt`) 2nd-order
Butterworth** applied per curve, with

```
fc_Hz = 10.5 − slider        # slider is Rokoko's 0.5–10 UI value, default 7 → fc 3.5 Hz
```

Applied to quaternion components with signs made continuous, then renormalised. Because it runs
twice, the *overall* −3 dB knee is `0.802 × fc`.

| Slider | Cutoff | Overall −3 dB knee |
|---|---|---|
| 10 (max smoothing) | 0.5 Hz | 0.40 Hz |
| 7 (default) | 3.5 Hz | 2.81 Hz |
| 5 | 5.5 Hz | 4.41 Hz |
| 0.5 (min smoothing) | 10.0 Hz | 8.02 Hz |

A second, independent finding: the **UE Curve Editor Butterworth** in Charlie Driscoll's MetaHuman
tutorial lands in almost the same place — 0.12 at 30 fps is a 3.6 Hz knee.

## State of play

| Piece | Status |
|---|---|
| Rokoko Studio Preview filter spec | ✅ solved and validated to 0.016°–0.19° mean error |
| UE Curve Editor (Driscoll) method | ✅ characterised from engine source |
| Portable filter implementation | ✅ `Tools/smooth_core.py`, numpy-only, validated |
| UE Animation Modifier | ✅ **this plugin.** Real tooltips and slider clamps, one `GetBoneTrackTransforms` call per bone, `MocapSmooth.SelfTest` built in. Verified 2026-09-15 on UE 5.8.2: capture 0.1 s, whole apply 1.2 s on 80 tracks × 3,492 frames (0.9 s of that is UE's own recompression) |
| Python fallback | ⚠️ works, ~10× slower, one key short per track and no refusal guard — kept for toolchain-less projects only |

## Choosing a filter shape

| Shape | Use when | Trade |
|---|---|---|
| **Butterworth + filtfilt** (default) | matching Rokoko, or general body cleanup | sharp knee separates jitter from performance well; rings ±3.35 % on a step |
| **Gaussian** | hands, head, anything with hard stops where ringing shows | strictly positive kernel, **can never overshoot**; gentler knee so it costs more performance per unit of jitter removed |

Never use a **causal** filter (One-Euro, spring, Kalman, AnimBP nodes) for offline cleanup: it
shifts motion later in time and breaks body-to-face sync. `.claude/refs/alternatives-considered.md`.

## Working rules

- **Smooth the source skeleton, not the retarget.** A MetaHuman-retargeted AnimSequence is mostly
  derived rig (twist, corrective, bulge bones). Filter the clean FK source, then retarget.
- **Never filter in place without a way back.** The plugin caches the original in
  `<Project>/Saved/MocapSmooth/<asset>.mocapraw` on first apply and reads from it every time after;
  the source FBX's path/size/mtime is the only thing that replaces it (a reimport). Any capture that
  would overwrite it is refused while the asset still matches the signature of the last write.
  `Smooth On Top Of Previous` is the opt-in for deliberate stacking.
- **Leave the root alone** unless drift demands it, and treat pelvis translation gently — smoothing
  global motion is what causes foot sliding. Rokoko's Lower Body group excludes the hips too.
- **Default to Rokoko's `_body` group, not `_fullbody`** — everything smoothed except the finger
  bones. Fingers carry most of a performance's read and are the first thing smoothing flattens.
  The exemption is fingers only; the hand/wrist bone stays filtered.
- **Check frame 0 before filtering.** Rokoko's FBX export has *Include reference pose* on by
  default, which puts an A-pose on frame 0 and makes a zero-phase filter ring across the first
  ~20 frames. The plugin detects it (frame 0's step ≈100× the median) and filters from frame 1.
- **Quaternion signs must be made continuous** before filtering components, and the result
  renormalised. Filtering Euler channels invites ±180° wrap blowups and gimbal coupling.
- **Watch the key count.** `GetNumberOfKeys() == GetNumberOfFrames() + 1`; anything working in
  frames is one sample short of the track. The plugin takes its length from the data itself.
- Scale: at 30 fps mocap, performance lives below ~3 Hz and jitter above ~5 Hz. A cutoff near
  3 Hz is the usual sweet spot; both Rokoko's default and the Driscoll setting land there.

## Installing into a project that is not on this machine

1. Release zip or clone → `<Project>/Plugins/MocapSmooth/`, or junction it into the engine
   (`.claude/rules/build-and-install.md`). Never both.
2. If the shipped `Binaries/Win64/UnrealEditor.modules` `BuildId` matches the engine, it loads
   as-is, Blueprint-only projects included. Otherwise one compile + restart.
3. Right-click an AnimSequence → Animation Modifier(s) → Add → **Mocap Smooth**.
4. `MocapSmooth.SelfTest` in the console before trusting the build.

## Per-project config

The modifier's Details-panel properties *are* the config. A project that wants the take list and
chosen cutoffs recorded outside the editor copies `Tools/mocap-smoothing.config.template.json` to
its own `.claude/refs/`. Nothing project-specific belongs in this repo.

## If you are extending this

This is meant to grow. Each smoothing system gets its own `.claude/refs/` file documenting what it
is, how it was characterised, and its measured parameters, with every claim tagged **measured**,
**from source**, or **inferred**. Keep this file a map, put detail in refs, and update `README.md`
so the public explanation and the working notes never disagree (`.claude/refs/maintenance.md`).
