# MocapSmooth (UE 5.8 plugin)

**Zero-phase smoothing for motion capture, as an Unreal Animation Modifier — and the measured
documentation of what the good mocap smoothers actually do.**

Right-click an AnimSequence → **Animation Modifier(s) → Add → Mocap Smooth**, set a strength,
Apply. It reproduces Rokoko Studio Preview's smoothing filter exactly, never compounds (every
apply reads from a protected copy of the raw take), and never shifts motion in time, so body stays
locked to face and audio.

Smoothing motion capture is a solved problem that almost nobody writes down. Tool vendors ship a
slider with no units and no documentation, tutorials pass around magic numbers, and everyone ends
up tuning by eye. This repo is the missing documentation, with a plugin on top.

## The headline result

**Rokoko Studio Preview's smoothing slider is a zero-phase Butterworth filter, and the mapping is
trivial once you know it:**

```
fc_Hz = 10.5 - slider        # slider is the 0.5 .. 10 UI value, default 7
filter = filtfilt(butter(order=2, fc))
```

Applied per quaternion component, signs made continuous, then renormalised.

That was derived by reading the shipping app's behaviour and then measured end to end.
Reimplementing it reproduces Rokoko's own output to within **0.016 to 0.19 degrees** mean angular
error per joint. So you can get the exact same smoothing without the app, in any DCC, in a batch
script — or in Unreal, with this plugin.

| Slider | Cutoff | Overall −3 dB knee |
|---|---|---|
| 10 (max smoothing) | 0.5 Hz | 0.40 Hz |
| 7 (default) | 3.5 Hz | 2.81 Hz |
| 5 | 5.5 Hz | 4.41 Hz |
| 0.5 (min smoothing) | 10.0 Hz | 8.02 Hz |

A second, independent finding: the **Unreal Curve Editor Butterworth workflow** taught in Charlie
Driscoll's MetaHuman tutorial lands in almost exactly the same place. Its 0.12 setting at 30 fps is
a 3.6 Hz knee; Rokoko's default is 3.5 Hz. Two communities converged on the same number without
knowing it.

## Install

**Requires UE 5.8**, Windows. Editor-only module; nothing ships in a packaged game.

### Option 1 - download a release (no compiler needed)

Take the latest zip from [Releases](https://github.com/Dylanyz/MocapSmooth/releases) and unzip it
into your project's `Plugins` folder, so you end up with
`MyProject/Plugins/MocapSmooth/MocapSmooth.uplugin`. Launch the project and enable **Mocap Smooth**
in Settings > Plugins if it is not already on. The release carries prebuilt Win64 editor binaries,
so nothing compiles on your machine and a Blueprint-only project stays Blueprint-only.

### Option 2 - build from source

Clone into your project's `Plugins` folder:

```
git clone https://github.com/Dylanyz/MocapSmooth.git MyProject/Plugins/MocapSmooth
```

Binaries are not tracked, so the first launch offers to build the missing module. Say yes. You need
a C++ toolchain: Visual Studio with the C++ workload **and the .NET Framework 4.8 SDK component**,
without which Unreal Build Tool fails with `Could not find NetFxSDK install dir` before compiling
anything.

### Option 3 - engine-wide (what the author does)

Installing into the engine lets every 5.8 project see the plugin with no per-project copy. It needs
prebuilt binaries, so build the package first (see *Rebuild*), then junction the engine at this repo:

```powershell
New-Item -ItemType Junction `
  -Path   "C:\Program Files\Epic Games\UE_5.8\Engine\Plugins\Marketplace\MocapSmooth" `
  -Target "<path to this repo>"
```

An engine plugin is never compiled by the editor, so every C++ change means running the build
script and restarting. Do not also keep a copy in a project's `Plugins` — a plugin found at both
paths fails to load.

### Verify

In the editor console:

```
MocapSmooth.SelfTest
```

Six checks against the reference implementation: slider mapping, −3 dB knee, zero-phase symmetry,
Gaussian overshoot (must be 0), Butterworth overshoot (~3.5 %), quaternion renormalisation. All
must print `ok`.

## Using it

1. Right-click one or more AnimSequences → **Animation Modifier(s) → Add → Mocap Smooth**.
2. Set **Full Body**. 7 is Rokoko's default (3.5 Hz); 4–5 is gentler.
3. **Apply**. The log names each region and its cutoff, and the asset saves normally.
4. Changed your mind? Set a different strength and Apply again — you get *that* strength, not the
   old one plus the new one. **Revert** takes you back to the untouched raw in one step.

**Smooth the source skeleton, then retarget.** A MetaHuman-retargeted sequence is mostly derived
rig (twist, corrective, bulge bones); filter the clean FK source and let the retarget re-derive.

### Properties

| Group | Property | What it is for |
|---|---|---|
| 1 Strength | **Full Body** | Master strength on Rokoko's 0.5–10 scale, higher is smoother. Cutoff = 10.5 − value Hz. Every region below inherits this unless overridden. |
| 2 Regions | **Fingers** | Finger bones only. **−1 = Off (default)**: fingers carry most of a performance's read and are the first thing smoothing flattens. Off with everything else on is exactly Rokoko's *Body* group. 0 = inherit Hands, then Full Body. |
| | **Hands** | The hand/wrist bone itself. −1 = Off, 0 = inherit Upper Body. Turning this off is *not* how you protect fingers — it leaves wrist jitter in an otherwise smooth arm. |
| | **Upper Body** | Spine and above, excluding fingers. −1 = Off, 0 = inherit. |
| | **Lower Body** | Hips and below, excluding the pelvis itself (as Rokoko does — smoothing global motion causes foot sliding). |
| 3 Filter | **Shape** | *Butterworth* matches Rokoko exactly and separates jitter from performance sharply, but overshoots ~3.4 % on a hard stop. *Gaussian* can never overshoot, at the cost of a gentler roll-off. |
| | **Gaussian for Hands and Fingers** | Non-overshooting Gaussian on hands and fingers only. A fist, a point, a grab are where Butterworth's ringing actually shows. |
| | **Order** | Roll-off steepness. 2 is what Rokoko uses; leave it to match. |
| 4 Advanced | **Smooth On Top Of Previous** | Off (default): always smooth from the raw copy. On: add a second pass on top of what is on the asset. The raw copy is kept either way, so Revert still undoes every pass. |
| | **Smooth Root** | Off by default — smoothing the root is what makes feet slide. |
| | **Smooth Pelvis Translation** | Filter the pelvis position as well as rotation (Rokoko's Full Body group does). Turn off if global motion starts sliding. |
| | **Skip Leading Reference Pose** | Rokoko's FBX export puts an A-pose on frame 0 by default; that 15–20° jump makes a zero-phase filter ring across the first ~20 frames. When detected (by outlier, so a clip that genuinely starts fast is not truncated), filtering starts at frame 1. |

### The original is protected, by design

Every apply reads from a pristine copy of the raw animation in
`<Project>/Saved/MocapSmooth/<asset>.mocapraw`, captured on the first apply. Beside it, a `.json`
records the source FBX's path, size and mtime — the **only** thing that replaces the original is
that changing, i.e. a reimport, detected on its own. Every write also records a 64-frame signature;
any capture that would overwrite an existing original, or that runs because the cache is missing,
is **refused** while the asset still matches that signature. That is the one path by which
smoothing could be baked in as the raw, and it is closed. Keep `Saved/` out of version control;
the cache is regenerable from the source FBX.

### Regions

Regions come from a hierarchy walk of the skeleton, not a name list: hand bones are the shallowest
bones named `*hand*` (catches `hand_l` and `LeftHand`, rejects `LeftHandIndex1` and `ik_*`),
fingers are everything below them, the pelvis is found by name or as the deepest branching bone,
lower body is its subtree, upper body is the rest. On an 80-track Manny/Rokoko skeleton that is
fingers 38, hands 2, upper 22, lower 16, pelvis 1, root 1 — identical to the independent Python walk.

## Performance

Measured 2026-09-15, UE 5.8.2, 80 tracks × 3,492 frames: capture **0.1 s**, whole apply **1.2 s**,
of which 0.9 s is Unreal's own animation recompression. A 65,520-frame take applies in 3 s. Reading
is one `GetBoneTrackTransforms` call per bone — an API with no `UFUNCTION`, which is exactly why a
Python implementation has to evaluate a pose per frame and lands at ~13 s for the same take.

## Rebuild

```powershell
Tools\build_mocapsmooth.ps1            # RunUAT BuildPlugin into %TEMP%\msb; safe with the editor open
Tools\build_mocapsmooth.ps1 -Status    # what is built, what is installed, what is next
Tools\build_mocapsmooth.ps1 -InstallOnly   # copy Binaries into this repo; editor must be closed
```

`Binaries/` and `Intermediate/` are gitignored. The build needs the .NET Framework 4.8 SDK (see
Option 2).

## What is in here

| | |
|---|---|
| [CLAUDE.md](CLAUDE.md) | Entry point for humans and agents alike. The one-line recipe, when to pick which filter, working rules, layout |
| [.claude/refs/filter-math.md](.claude/refs/filter-math.md) | The DSP. Why zero phase, why quaternions, Butterworth vs Gaussian with numbers, ringing, edge handling |
| [.claude/refs/rokoko-studio-preview.md](.claude/refs/rokoko-studio-preview.md) | How Studio Preview is built, its JSON-RPC surface, smoothing groups, scene format |
| [.claude/refs/rokoko-measurement.md](.claude/refs/rokoko-measurement.md) | The experiment. Method, raw numbers, validation, and a correction worth reading |
| [.claude/refs/driscoll-ue-curve-editor.md](.claude/refs/driscoll-ue-curve-editor.md) | What Unreal's FFT filter actually computes, and why it struggles on long takes |
| [.claude/refs/ue-implementation.md](.claude/refs/ue-implementation.md) | Unreal 5.8 API facts and the Animation Modifier design |
| [.claude/refs/ue-animation-modifier-build.md](.claude/refs/ue-animation-modifier-build.md) | Building and installing, every trap hit on the way, what to verify, the Python fallback |
| [.claude/refs/alternatives-considered.md](.claude/refs/alternatives-considered.md) | Every approach weighed, and why each was kept or dropped |
| [.claude/refs/live-coding.md](.claude/refs/live-coding.md) | Can you iterate without restarting the editor? Measured answer |
| [Tools/smooth_core.py](Tools/smooth_core.py) | The filter. numpy only, no scipy. Self-testing. Runs unchanged inside Unreal's editor Python, Blender, or standalone |
| [Tools/rokoko_probe.js](Tools/rokoko_probe.js) | Drives a local Rokoko engine over JSON-RPC to re-verify the spec |
| [Tools/python_fallback/](Tools/python_fallback/) | The same modifier in editor Python + Blueprint, for a project with no C++ toolchain. Slower, two known bugs |
| `.claude/rules/` | How the author's agents are expected to behave in this repo (never restart the editor unasked, build/install cycle, licensing) |

## Quick start, outside Unreal

```bash
python Tools/smooth_core.py          # runs the self-test
```

```python
from smooth_core import smooth_quaternions, rokoko_slider_to_fc

fc = rokoko_slider_to_fc(7.0)                      # 3.5 Hz, Rokoko's default
smoothed = smooth_quaternions(quats, fc, fps=60)   # quats is (N, 4)
```

## Things worth knowing even if you never run the code

- **Never use a causal filter for offline cleanup.** One-Euro, springs, Kalman and AnimBP nodes all
  delay what they smooth, and the delay varies with speed. On a performance-capture shoot that
  breaks body-to-face sync. Filter forwards and backwards instead; the phase cancels exactly.
- **Filter quaternions, not Euler angles.** Euler channels blow up across a ±180° wrap and the axes
  are not independent. Make the quaternion signs continuous first, then filter components, then
  renormalise.
- **Leave the root and hips alone.** Smoothing global translation is what causes foot sliding.
  Rokoko reached the same conclusion: its Lower Body group is documented as "all bones from the hips
  and down, excluding the hips bone".
- **Butterworth rings, Gaussian does not.** A sharp knee separates jitter from performance much
  better, at the cost of about 3 % overshoot on a hard stop. Use Butterworth by default and Gaussian
  where the ringing shows, usually hands and head.
- **Re-applying a smoothing pass is not the same as changing its strength.** Filters compound. Any
  adjustable control must filter from the original every time.
- **Frames are intervals, keys are samples.** In UE, `GetNumberOfKeys() == GetNumberOfFrames() + 1`.
  Anything that works in frames is one sample short of every track.

## Scope and honesty

Every claim in the references is tagged as **measured**, **from source**, or **inferred**. Where
something was not tested it says so. One known open question: the measurement was made on a 60 fps
clip and whether Rokoko's cutoff scales with capture rate was not verified.

This repository contains no Rokoko or Epic code. See [NOTICE](NOTICE) for the details.

## Contributing

The structure is designed to grow. Each additional smoothing system gets its own reference file
under `.claude/refs/` covering what it is, how it was characterised, and its measured parameters.
Keep `CLAUDE.md` a map, put detail in refs, and tag every claim with how you know it. If you change
the filter, change `Tools/smooth_core.py` and `MocapSmoothFilter.cpp` together and keep
`MocapSmooth.SelfTest` green.

## License

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).
