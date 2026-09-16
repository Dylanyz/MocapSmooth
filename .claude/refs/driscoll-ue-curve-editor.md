# The UE Curve Editor route (the "Driscoll method")

The most widely taught way to smooth mocap in Unreal, from Charlie Driscoll's tutorial *Cinematic
Motion Capture with Move One and MetaHuman Animator* (UE 5.4). Worth documenting properly because it
turns out to be almost the same filter as Rokoko's, and because UE's implementation has quirks that
matter on long takes.

## The workflow as taught

1. In Sequencer, right-click the skeletal animation track, **Bake To Control Rig**, pick the
   MetaHuman control rig.
2. Select a control group in the Sequencer tree (body control, then head, then each arm, then
   clavicles, then legs).
3. Open the Curve Editor, select all keys for that group.
4. **Filter** then **Fourier Transform (FFT)**, response **Butterworth**, type **Low Pass**.
5. Cutoff around **0.12 to 0.13** for the body, roughly double the aggression (so about **0.06**)
   for arms and hands, same for clavicles and legs.
6. Later, a second pass at **0.1** on the body and head controls.

Reported outcomes in the tutorial: visible improvement, acknowledged loss of fine detail, and
**worse foot sliding** after filtering the legs and body control, patched by hand-deleting IK foot
keys between plant and lift.

## Where the filter lives in UE 5.8

The Fourier Transform filter was **not removed** after 5.4, contrary to a common assumption. It
lives in a plugin rather than the core module, which is why searching the CurveEditor module source
misses it:

```
Engine/Plugins/Editor/CurveEditorTools/Source/CurveEditorTools/Private/Filters/CurveEditorFFTFilter.cpp
```

`CurveEditorTools.uplugin` has `"EnabledByDefault": true`, so the same click path works in 5.6, 5.7
and 5.8.

Core CurveEditor ships Bake, Simplify (Reduce), Smart Reduce, Euler, SmartSnap and **Gaussian**.

## What UE's FFT filter actually computes

From `CurveEditorFFTFilter.cpp` and `SignalProcessing/Public/DSP/PassiveFilter.h`:

1. Rebakes the selected keys evenly at an interval of `(range) / (2 * numKeys)`. For a curve with
   one key per frame that is exactly **2x the frame rate**, so the internal Nyquist is the frame
   rate itself, independent of clip length.
2. Centres the values on the midpoint of min and max, then **zero-pads to the next power of two**.
3. Forward real FFT. Multiplies every bin by a **real, non-negative** gain:

   ```
   Butterworth:  1 / sqrt(1 + (f/fc)^(2*Order))      Order default 4
   Chebyshev:    1 / sqrt(1 + T_Order(f/fc)^2)       UE's default Response, has passband ripple
   ```

4. Inverse FFT, evaluate the filtered dense curve at the original key times, restore the original
   key times and tangents with new values.

Notes:

- A real gain means **no phase shift**. This route is zero-phase too, like Rokoko's, but it gets
  there by construction rather than by a second pass.
- It is a Butterworth **magnitude** applied in the frequency domain, not an actual IIR Butterworth.
- `Response` defaults to **Chebyshev**, which ripples in the passband. Switch it to Butterworth,
  as the tutorial does.
- `CutoffFrequency` is normalised so that 1.0 is Nyquist. With the internal 2x resample, at a
  frame rate of `fps` the real cutoff is `cutoff * fps` Hz. So 0.12 at 30 fps is 3.6 Hz.

## Translating between the two systems

| Rokoko slider | Rokoko design fc | UE FFT cutoff @30 fps | UE FFT cutoff @60 fps |
|---|---|---|---|
| 10.0 | 0.5 Hz | 0.013 | 0.007 |
| 8.5 | 2.0 Hz | 0.053 | 0.027 |
| **7.0 (default)** | **3.5 Hz** | **0.094** | 0.047 |
| 5.0 | 5.5 Hz | 0.147 | 0.074 |
| 0.5 | 10.0 Hz | 0.267 | 0.134 |

So the tutorial's 0.12 body setting (3.6 Hz at 30 fps) sits almost exactly on Rokoko's default
(3.5 Hz). Two communities converged on the same number independently, which is a decent signal that
it is the right neighbourhood for body mocap.

The shapes are not identical: UE applies an order-4 magnitude once (rings ±5.7 % on a step), Rokoko
applies an order-2 magnitude twice (rings ±3.35 %). Close, but UE's is slightly sharper and rings
slightly harder.

## Why not just use this route

It works, and for a 60-second clip it is genuinely fine. Four reasons it is a poor fit for long
takes and repeatable pipelines:

1. **Bake To Control Rig is destructive to the animation link.** The Sequencer section stops
   referencing the AnimSequence and becomes raw rig keys. Re-doing the smoothing means re-baking.
2. **It does not scale.** A 13-minute take at 30 fps is 24,331 frames; a MetaHuman control rig has
   hundreds of controls. Selecting and filtering that in the Curve Editor is not practical.
3. **It filters Euler channels and IK controls.** Euler channels gimbal-couple and blow up across a
   ±180° wrap (Epic ships a separate Euler Filter precisely for this). And smoothing an IK foot
   target physically moves the plant, which is exactly the foot sliding the tutorial hits and then
   hand-fixes.
4. **FFT wraps.** Circular convolution plus a zero pad means the end of the clip couples to its
   start and the padding boundary rings. Invisible at 60 seconds, not invisible at 13 minutes.

Filtering bone quaternions offline avoids all four.

## UE's Gaussian curve filter

Worth knowing since it is the no-ringing alternative and it is built in.

`Engine/Source/Editor/CurveEditor/Private/Filters/CurveEditorGaussianFilter.cpp`:

- one parameter, `KernelWidth`, minimum 3, forced odd;
- `sigma = (KernelWidth - 1) / 6`;
- kernel `exp(-k^2 / (2*sigma^2))`, normalised;
- **edge clamped** (indices below zero clamp to the first key, above the end clamp to the last), so
  no wrap and no pad discontinuity;
- operates on **key indices**, not time, so it assumes evenly spaced keys. Fine for baked mocap.

Which makes it exactly the Gaussian described in [filter-math.md](filter-math.md), just exposed
through a width instead of a sigma:

| KernelWidth | sigma | −3 dB @30 fps |
|---|---|---|
| 5 (minimum) | 0.67 | 6.0 Hz |
| 7 | 1.0 | 4.0 Hz |
| 13 | 2.0 | 2.0 Hz |
| 19 | 3.0 | 1.3 Hz |
| 25 | 4.0 | 1.0 Hz |
