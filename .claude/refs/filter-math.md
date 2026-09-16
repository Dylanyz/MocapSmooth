# Filter math for motion capture smoothing

Everything here is either textbook DSP or measured on this bench. Claims are tagged
**[measured]**, **[from source]** (read out of a shipping binary or engine source) or
**[derived]** (arithmetic from the other two).

## The problem

Mocap noise and mocap performance overlap in amplitude but not much in frequency. At 30–60 fps
body capture:

| Band | Contents |
|---|---|
| 0–3 Hz | the performance: weight shifts, gestures, strides, head turns |
| 3–5 Hz | transition zone; fast hits and snaps have energy here |
| 5 Hz and up | sensor jitter, solver shimmer, finger/wrist noise |

So smoothing is a low-pass problem, and the only interesting question is what the filter does at
the boundary between those bands.

## Rule 1: zero phase, always

A causal filter delays what it smooths. The delay is not constant across frequency either, so fast
moves shift more than slow ones. For offline cleanup that is pure loss:

- it desynchronises body from face and audio, which on a performance-capture shoot were aligned
  frame by frame;
- it makes scrubbing and rendering disagree, because the filter's state depends on playback history.

The fix is to run the filter **forwards and then backwards** (`filtfilt`). The backward pass cancels
the forward pass's phase exactly, leaving a real, symmetric response with **zero phase shift**. The
magnitude is squared as a side effect, so a 2nd-order design becomes a 4th-order effective response.

Both systems documented in this skill are zero-phase. Rokoko's is `filtfilt`; UE's Curve Editor
filter gets there differently, by multiplying an FFT by a real magnitude, which is also zero-phase.

## Rule 2: rotations are quaternions, not three numbers

Filtering Euler channels independently is wrong in two ways: a ±180° wrap produces a huge false
transient, and the three axes are not independent so filtering them separately distorts the
rotation. UE ships a whole separate "Euler Filter" to paper over the first problem.

Instead:

1. Walk the quaternion track and flip sign where `dot(q[i], q[i-1]) < 0`, so the path is continuous
   on the 4-sphere rather than jumping between `q` and `−q`.
2. Filter each of the four components independently.
3. Renormalise each result.

This is a small-angle approximation of proper rotation filtering, and it is accurate for the window
sizes smoothing actually uses. If you ever need a very wide window (σ beyond roughly 6 frames),
switch to filtering in rotation-vector space around a moving reference instead.

## Butterworth

Magnitude response of an order-`n` low-pass:

```
|H(f)| = 1 / sqrt(1 + (f/fc)^(2n))
```

Run through `filtfilt` it becomes the square of that:

```
|H_total(f)| = 1 / (1 + (f/fc)^(2n))
```

Consequences worth remembering **[derived]**:

- the overall −3 dB knee is **not** `fc`. For `filtfilt` of order 2 it is `0.802 × fc`; for order 4
  it is `0.896 × fc`.
- asymptotic rolloff is `12 × n` dB/octave for the two-pass result (24 dB/oct at order 2).
- in the −6 dB to −12 dB band the measured slope is much gentler than asymptotic: 15.2 dB/oct for
  `filtfilt` order 2, 30.5 dB/oct for order 4. This is a useful fingerprint for identifying an
  unknown filter's order from a measured response.

Butterworth is "maximally flat": it disturbs the passband as little as possible for a given
sharpness. That is exactly what you want when the performance and the jitter are close together in
frequency.

The cost is **ringing**. A sharp knee means the equivalent time-domain kernel has negative side
lobes, so a step gets a small pre-tremor and an overshoot **[derived]**:

| Filter | Step overshoot |
|---|---|
| `filtfilt` Butterworth order 2 (Rokoko's) | ±3.35 % |
| single-pass Butterworth order 4 magnitude (UE's FFT filter) | ±5.69 % |
| Gaussian, any width | 0 % |

On organic motion this is rarely noticed. On a hard stop, a punch, or a finger tap it can read as a
tiny bounce.

## Gaussian

Kernel `g[k] ∝ exp(−k² / 2σ²)`, normalised, convolved with edge clamping. Strictly positive, so it
**cannot overshoot** and cannot invent motion that was not captured. Its frequency response is also
Gaussian, so the rolloff is gentle.

−3 dB point **[derived]**:

```
f_3dB ≈ 0.1325 / σ   cycles per sample   =   0.1325 × fps / σ   Hz
```

At 30 fps: σ=1 → 3.98 Hz, σ=2 → 1.99 Hz, σ=3 → 1.33 Hz.

Epic's own Curve Editor Gaussian filter parameterises this as `KernelWidth` with
`σ = (KernelWidth − 1) / 6` **[from source]**, so KernelWidth 7 → σ=1, 13 → σ=2, 19 → σ=3.

## Butterworth vs Gaussian, quantified

Measured at 30 fps, comparing what each keeps of performance (2 Hz) against what each removes of
jitter (6 Hz) **[derived]**:

| Filter | knee | keeps at 2 Hz | keeps at 6 Hz | overshoot |
|---|---|---|---|---|
| Butterworth ord 4, cutoff 3.6 Hz | 3.6 Hz | 99.5 % | 12.9 % | ±5.7 % |
| Butterworth ord 4, cutoff 3.0 Hz | 3.0 Hz | 98.1 % | 6.2 % | ±5.7 % |
| Butterworth ord 4, cutoff 1.8 Hz | 1.8 Hz | 54.9 % | 0.8 % | ±5.7 % |
| Gaussian σ 1.1 | 3.6 Hz | 89.9 % | 38.5 % | 0 % |
| Gaussian σ 2 | 2.0 Hz | 70.4 % | 4.2 % | 0 % |
| Gaussian σ 3 | 1.3 Hz | 45.4 % | 0.1 % | 0 % |

Read the middle two columns together. At the same knee the Butterworth removes three times as much
jitter. To match its jitter rejection a Gaussian has to smooth hard enough to eat roughly 20 % of
the real performance.

**So: Butterworth by default, Gaussian where ringing is visible.** Hands and head on hard stops are
the usual candidates.

## Edge handling

This matters more than people expect on long takes.

- **`filtfilt` with odd reflection** (the standard) is well behaved at any length. Residual error
  against a reference implementation is concentrated in the final frame or two **[measured]**.
- **FFT-based filtering wraps.** Multiplying a spectrum is circular convolution, so the end of the
  clip bleeds into the start. UE's FFT filter zero-pads to the next power of two, which limits but
  does not remove the problem: the signal steps to the padding value at the boundary and rings.
  On a 60-second clip this is invisible; on a 13-minute take it is not **[derived]**.
- **Gaussian convolution with edge clamping** has no wrap and no pad discontinuity at any length.

Prefer reflection or clamping. If you implement an FFT route, pad by reflection rather than zeros.

## Cascading

Applying a filter twice multiplies the magnitude responses. Two Gaussians combine cleanly as
`σ_total = sqrt(σ1² + σ2²)`, so a second pass is always equivalent to one wider pass. Butterworth
passes do not combine into another Butterworth; you get a steeper, differently shaped filter.

Practical consequence: **re-applying a smoothing pass is not the same as changing its strength.**
Any tool that lets a user adjust a smoothing amount must filter from the original every time, never
from the previous result.

## Positions

Only a few bones have meaningful translation: the root and the pelvis. Everything else is a fixed
offset from its parent. Filter those two only, and be conservative:

- smoothing global translation is what produces foot sliding, because a planted foot is held in
  place by the hips being where they were;
- Rokoko reached the same conclusion independently: its Lower Body group is documented as
  "all bones from the hips and down, **excluding the hips bone**" **[from source]**.

Leave the root untouched unless drift is the actual complaint.
