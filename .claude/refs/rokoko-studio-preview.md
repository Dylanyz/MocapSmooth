# Rokoko Studio Preview — how it is built and how to talk to it

Rokoko documents none of this. Everything below was read out of the shipping app (version **1.2.0**,
investigated 2026-09-15) or obtained by driving its own engine locally. No Rokoko code is reproduced
here; this is a behavioural description for interoperability.

## Why you would care

Studio Preview is, as of 2026, the only Rokoko product with a general body smoothing filter. Studio
2.x's "Filters" panel is something else entirely (see the bottom of this file). If you want to match
"the Rokoko look" without running Preview, this is the thing to match.

## Install layout (Windows)

```
%LOCALAPPDATA%\studio_desktop\
  app-1.2.0\
    Rokoko Studio Preview.exe          Electron shell
    resources\
      app.asar                         the UI (Vite + React + jotai + three.js)
      MotionEngineCli-1.2.1.exe        ~180 MB .NET single-file engine, all the DSP lives here
      .env                             service endpoints
%APPDATA%\Rokoko Studio Preview\
  Scenes\*.rkk-scene                   scene packages (see below)
  Scenes\scenes.db                     SQLite: FileInfos, SyncItems
  logs\engine-log-*.txt                the engine logs every RPC call and its duration
```

Unpack the UI with `npx @electron/asar extract app.asar out/`.

## Architecture

The Electron app contains **no DSP at all**. It manages smoothing segments and sends them to the
native engine over **JSON-RPC** (StreamJsonRpc, LSP-style Content-Length framing) on stdio:

```
MotionEngineCli-1.2.1.exe serve stdio
```

The engine is a .NET single-file bundle with **unobfuscated metadata**, and it returns full
exception stack traces including original source paths
(`C:\dev\motion-engine\src\app\animationeditor\lib\server\...`). That combination makes it unusually
legible.

## What the binary says about the filter

Strings recovered from `MotionEngineCli-1.2.1.exe`:

- `IFCurveFilter`, implemented by **`ButterworthFCurveFilter`**, `SmoothFCurveFilter`,
  `PostIKReachFilter`.
- **`ApplyButterworth`**, **`GetButterWorthParameters`**, `CreateButterWorthTagName`, next to
  `MinCutoff`, `frequencyCutoff`, `cutoff`, `KeySampling` — inside
  `Rokoko.MotionEngine.SimpleAnimXCurves.dll`, alongside `SimpleRotationCurve`,
  `SimplePositionCurve`, `SimpleQuaternionCurve`.
- Implementation primitives: **`SosFilter`** (second-order sections), **`DF2TBiquad`**
  (Direct Form 2 Transposed), `ProcessBiquad`, `_numBiquads`, `BiquadChain`,
  **`PrototypeAnalogLowPass`**, `_preBLTgain`, `_overallGain`.

That last group is a textbook analog-prototype then bilinear-transform then cascaded-biquad
Butterworth. It is an **IIR**, not an FFT multiply and not an FIR kernel. `MathNet.Numerics` is
bundled for general math but is not the filter.

The order, the phase behaviour and the strength-to-cutoff mapping are **not** in the string table —
those constants live in IL. They were obtained by measurement instead:
[rokoko-measurement.md](rokoko-measurement.md).

## What is NOT the smoothing filter

The same binary contains `KalmanFilter3D`, `OneEuroFilter` (Vector2 / Vector3 / Quat),
`LowPassFilterQuat`, `ONE_EURO_FILTER_DEFAULT_MINCUTOFF` and `_BETA`. Their string neighbours are
`IsSmartsuitPro2`, `FingerPoser`, `SmartGlovesOffset`, `KalmanReset`, `ActorProfilePreset` — this is
the **live streaming and sensor-fusion path**, consistent with Rokoko's public statements about a
Kalman-based locomotion engine. Do not confuse it with the timeline smoothing track.

Also checked and empty: Rokoko's open-source Live plugins for Blender, Unity and Unreal contain no
filtering code whatsoever, only UDP transport and retargeting. And the only Rokoko-linked patent,
US10949716B2, covers movement *classification* (FFT plus SVM), not smoothing.

## Parameters on the wire

The UI slider is **0.5 to 10, step 0.5, default 7** (renderer source path
`src/stores/features/smoothing.ts`). Higher slider means smoother.

The UI **inverts it** before sending:

```js
strength: 10.5 - sliderValue
```

So slider 10 becomes engine 0.5, slider 7 becomes engine 3.5, slider 0.5 becomes engine 10. The
engine parameter is the Butterworth cutoff frequency in Hz, floored by `MinCutoff`. Lower engine
value means lower cutoff means more smoothing.

Request shape:

```json
{ "smoothingId": "<guid>", "rangeTicks": [start, end], "strength": 3.5, "smoothingGroup": "_fullbody" }
```

Ticks are **.NET ticks: 1e7 per second**.

## Smoothing groups

Read live from the engine with `getSmoothingGroups`, so these descriptions are Rokoko's own words:

| id | title | description |
|---|---|---|
| `_fullbody` | Full Body | All bones in the hierarchy |
| `_body` | Body | All bones in the hierarchy, excluding the finger bones |
| `_upperbody` | Upper Body | All bones from the spine and up, excluding the finger bones |
| `_lowerbody` | Lower Body | All bones from the hips and down, **excluding the hips bone** |
| `_hands` | Hands | Both left and right hand bones, including all finger bones |

**`_body` is the one to reach for by default.** In Rokoko's own launch tutorial (*NEW Mocap
Smoothing with Rokoko Studio Preview*, youtube.com/watch?v=2vQ4Hef6MjQ) the presenter works only in
Full Body or Body, and moves to Body the moment smoothing reads as stiff fingers — his words:
fingers are very expressive, and smoothing loses that expressiveness. Note the exclusion is the
**finger bones**, not the hand: under `_body` the wrist/hand bone is still filtered. That makes
`_body` ≠ `_fullbody` minus `_hands`.

Two things to note. The groups **overlap**, so any reimplementation needs an explicit precedence
rule. And Lower Body excluding the hips is deliberate: smoothing the hips smooths the character's
global motion and drags planted feet.

Full Body does filter **translation** as well as rotation. The hips position channel shows the same
cutoff as the rotations.

## The JSON-RPC surface

24 methods. The ones that matter:

| Method | Params | Notes |
|---|---|---|
| `initialize` | `["<appDataDir>"]` | **positional string**, not an object. Returns server info |
| `getFileInfos` | `["<userId>"]` | lists known scenes from `scenes.db` |
| `loadScene` | `{parameters:{fileInfoId}}` | expects an **unpacked** scene directory |
| `getSceneManifest` | none | actors, clips, durations |
| `setActiveClip` | `{parameters:{clipId}}` | |
| `getEvaluatedAnimationData` | `{parameters:{clipId, fps}}` | the baked result, per joint, quaternions and positions |
| `setSmoothingSegment` | `{parameters:{smoothingId, rangeTicks, strength, smoothingGroup}}` | creates or updates |
| `deleteSmoothingSegment` | `{parameters:{smoothingId}}` | |
| `getSmoothingGroups` | none | the table above |
| `importGltfClip` | `{parameters:{importFilePath}}` | see the caveat below |
| `saveScene` | | **only this writes to disk** |

Others: `clipSegmentCreate` / `Update` / `Delete`, `exportAnimation`, `exportLoopSegment`,
`getEvaluatedLoopSegment`, `getExportSkeletonOptions`, `getUploadUrl`, `deleteAnimationClip`,
`renameAnimationClip`, `importLocalStudioSceneFromJsonFolder`, `shutdown`.

### Gotchas

- **Sending `"params": null` crashes the engine.** StreamJsonRpc's argument counter throws and the
  process exits. For no-argument methods omit `params` entirely.
- `initialize` takes a **positional** array containing one string. Everything else wraps its
  arguments in a `parameters` object.
- `importGltfClip` returns `isSuccessful: true` for a hand-built glTF, but the resulting clip
  evaluates to a **static rest pose**. Rokoko retargets onto its own 79-joint "Newton" skeleton and
  did not pick up animation from either a plain node hierarchy or a skinned one. A synthetic
  test-signal route therefore does not work; use a real scene.

## Scene package format

A `.rkk-scene` is a **zip archive**, despite looking like a single file:

```
scene-properties.json
model-<guid>.json          the actor and skeleton
clip-<guid>.json           one per take, can be hundreds of MB
```

`loadScene` wants a **directory** at the path named in `scenes.db` table `FileInfos`, column
`LocalPath` — not the zip. To work on a copy safely: extract the zip to a directory, copy
`scenes.db` beside it, and update `LocalPath` to point at the extracted directory. The engine then
loads the copy and the originals are never touched. Nothing is written back unless `saveScene` is
called.

## Export-side gotcha: the leading reference pose

Rokoko Studio's FBX export panel ships **Include reference pose** enabled. It writes the skeleton's
reference pose (A-Pose (UE5) with the Manny preset) as the **first frame of the clip**. Measured on
this project's takes (Manny (UE5) target, UE naming, 30 fps): frame 0 -> frame 1 moves **15-19
degrees mean, up to 61 degrees peak**, where real consecutive frames move 0.07-0.8 degrees.

For smoothing this is a step at the signal boundary, and `filtfilt` rings on it. A/B at slider 7
(3.5 Hz, 30 fps) on a real take: including frame 0 pulls frames 1-5 **2.65 degrees** off the raw
motion versus **1.48 degrees** with it excluded, and frames 1-20 0.87 versus 0.56. The two converge
by frame ~21, which is the filter's settling length at that cutoff.

So **detect and exclude a leading reference-pose frame before filtering**, leaving it untouched in
the output. Do not simply drop frame 0 unconditionally: a clip that genuinely starts on fast motion
would be truncated. Compare frame 0's step against the median of the following steps -- the real
ones are two orders of magnitude smaller, so the test is unambiguous.

## Studio 2.x is a different thing

For completeness, since it is easy to conflate. Studio 2.x's Filters panel offers Locomotion
(Kalman-based foot lock, plus a "smooth frames" moving average applied **only to vertical hip
motion**), Knee Pop Smoothing, Drift Fix, Treadmill, Toe Bend, and a hand-smoothing toggle. None of
these is a general body low-pass. They are non-destructive inside the project, but FBX export bakes
whatever is enabled.
