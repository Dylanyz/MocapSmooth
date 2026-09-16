# Measuring Rokoko Studio Preview's filter

The binary tells you it is a Butterworth. It does not tell you the order, whether it runs zero-phase,
or how the strength slider maps to a cutoff. Those constants live in IL. This file records how they
were measured, what the numbers are, and how the result was validated.

Date: 2026-09-15. Studio Preview 1.2.0 / MotionEngineCli 1.2.1.

## Experiment design

The idea is simple: get the same clip out of the engine twice, once unfiltered and once filtered,
and compare.

1. Extract a real `.rkk-scene` (a zip) into a sandbox directory.
2. Copy `scenes.db` beside it and repoint `FileInfos.LocalPath` at the extracted copy, so the
   original scene is never touched.
3. Drive the engine over JSON-RPC:
   `initialize` then `loadScene` then `getSceneManifest` then `setActiveClip` then
   `getEvaluatedAnimationData` for a **baseline**.
4. `setSmoothingSegment` over the whole clip with group `_fullbody` at one strength, then
   `getEvaluatedAnimationData` again.
5. Repeat step 4 across strengths. Never call `saveScene`.

Test clip: a 40.9 s take, 2456 frames, evaluated at 60 fps, 52 animated joints.

Engine strengths probed: 0.5, 2.0, 3.5, 5.5, 10.0 (UI sliders 10, 8.5, 7, 5, 0.5).

A synthetic clip would have been cleaner, but `importGltfClip` silently produces a static pose (see
[rokoko-studio-preview.md](rokoko-studio-preview.md)), so a real capture was used instead. That is
fine: any broadband input works for transfer-function estimation.

## Method 1: transfer function

For each joint and each quaternion component, with Welch averaging over Hann-windowed segments:

```
H(f) = mean(conj(A) * B) / mean(|A|^2)
```

where `A` is the baseline spectrum and `B` the filtered spectrum. Averaging across all 52 joints
times 4 components gives a clean estimate.

### Result: it is zero-phase

Phase of `H` is flat at **0.0 to −0.2 degrees** across the whole passband at every strength. A
causal biquad chain would show steadily growing lag. There is none. The engine runs the filter
**forward and backward** — `filtfilt`.

(Above the knee the measured phase goes wild. That is the noise floor, where the magnitude is
essentially zero and phase is meaningless. Ignore it.)

### Result: magnitude response

Measured dB response of the rotation channels, averaged:

| f (Hz) | engine 0.5 | engine 2.0 | engine 3.5 | engine 5.5 | engine 10.0 |
|---|---|---|---|---|---|
| 0.23 | −0.4 | 0.0 | 0.0 | 0.0 | 0.0 |
| 0.53 | −6.5 | −0.1 | −0.1 | −0.1 | −0.1 |
| 1.00 | −23.8 | −0.6 | −0.2 | −0.1 | −0.1 |
| 2.00 | −40.8 | −6.1 | −1.0 | −0.3 | −0.2 |
| 3.00 | −36.2 | −16.2 | −3.9 | −1.0 | −0.4 |
| 4.00 | −38.5 | −28.0 | −8.8 | −2.0 | −0.3 |
| 5.00 | −40.1 | −33.6 | −15.1 | −4.1 | −0.1 |
| 6.00 | −36.3 | −33.9 | −23.1 | −7.9 | −0.3 |
| 8.00 | −30.3 | −29.9 | −31.0 | −16.7 | −1.3 |

Below about −25 dB the measurement floors out; the clip simply has no energy up there. Do not read
slopes from the floor.

Interpolated −3 dB points: 0.39, 1.62, 2.82, 4.55, 8.72 Hz.

## Method 2: time-domain fit (the decisive one)

Take the baseline, apply our own `filtfilt(butter(n, fc))` to each sign-continuous quaternion
component, renormalise, and minimise mean angular error against Rokoko's actual output. Sweep `n`
and `fc`.

| UI slider | engine strength | best-fit order | best-fit fc | ratio fc/strength | residual (mean angular error) | as % of the change Rokoko made |
|---|---|---|---|---|---|---|
| 10.0 | 0.5 | 2 | 0.52 Hz | 1.04 | 0.19° | 10 % |
| 8.5 | 2.0 | 2 | 2.00 Hz | 1.00 | 0.039° | 9 % |
| **7.0 (default)** | **3.5** | **2** | **3.60 Hz** | 1.03 | 0.022° | 13 % |
| 5.0 | 5.5 | 2 | 5.65 Hz | 1.03 | 0.016° | 24 % |

Order 2 wins at every strength, and the cutoff tracks the strength parameter essentially 1:1.

## The answer

```
filter  = filtfilt( butter(2, fc / (fps/2)) )
fc_Hz   = engine_strength = 10.5 - ui_slider
```

Applied per quaternion component with signs made continuous, then renormalised. Positions are
filtered the same way when the group includes them.

Derived consequences:

- overall −3 dB knee of the finished result is `0.802 × fc` (0.802 is where `1/(1+x^4)` crosses
  1/sqrt(2)). At slider 7 that is 2.81 Hz, matching the 2.82 Hz measured spectrally. The two methods
  agree.
- asymptotic rolloff 24 dB/octave.
- step overshoot ±3.35 %.

| UI slider | design fc | overall −3 dB |
|---|---|---|
| 10.0 | 0.5 Hz | 0.40 Hz |
| 8.5 | 2.0 Hz | 1.60 Hz |
| 7.0 | 3.5 Hz | 2.81 Hz |
| 5.0 | 5.5 Hz | 4.41 Hz |
| 0.5 | 10.0 Hz | 8.02 Hz |

## Validation

Re-implementing the filter in numpy and running it on the same baseline reproduces Rokoko's own
output to within **0.016° to 0.19° mean angular error per joint**. On the main body joints at
maximum smoothing the residual is 2.6 % to 6.3 % of the change Rokoko applied, and the worst
individual frames are the last frame of the clip — an edge-padding difference, not a shape
difference.

Rokoko Studio Preview's smoothing is therefore fully reproducible outside the app.

## A correction worth keeping

An earlier pass of this analysis concluded order 4 per direction and 48 dB/octave. That was wrong.
The cause was arithmetic: the slope between the −6 dB and −12 dB points was computed using a span of
18 dB instead of 6 dB, inflating every slope by 3x.

Corrected, the measured slope in that band is 15 to 19 dB/oct. Theory for `filtfilt` of a 2nd-order
Butterworth in the same band is 15.2 dB/oct; for 4th-order it would be 30.5. The time-domain fit
agrees independently. Order 2.

The lesson generalises: **fit in the time domain when you can.** Spectral slope estimates are easy
to get wrong, are contaminated by the noise floor, and near the knee the slope has not reached its
asymptote anyway.

## Open question

The measurement was made on one clip evaluated at 60 fps. Whether the engine's cutoff is absolute Hz
or scales with the capture's native sample rate was **not tested**. The clean 1:1 mapping to Hz
suggests absolute, but if you work at 100 fps or 120 fps, verify before trusting the numbers: apply
the same strength to clips captured at two different rates and compare the resulting knee.
