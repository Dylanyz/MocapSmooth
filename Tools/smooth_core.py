"""
smooth_core.py — portable zero-phase smoothing for motion capture.

numpy only. No scipy, so this runs unchanged inside Unreal's editor Python, Blender,
or a bare interpreter.

Implements the filter measured out of Rokoko Studio Preview:
    zero-phase (forward-backward) Butterworth, order 2 per direction,
    cutoff fc_Hz = 10.5 - rokoko_slider
plus a Gaussian alternative for cases where the Butterworth's ringing is visible.

Validated 2026-09-15 against Rokoko Studio Preview 1.2.0's own output:
0.016 to 0.19 degrees mean angular error per joint across four strength settings.
See ../.claude/refs/rokoko-measurement.md.

Run this file directly to execute the self-test.

SPDX-License-Identifier: Apache-2.0
"""

from __future__ import annotations

import numpy as np

__all__ = [
    "rokoko_slider_to_fc",
    "butter_lowpass",
    "filtfilt",
    "gaussian_kernel",
    "gaussian_filter",
    "make_quaternions_continuous",
    "smooth_quaternions",
    "smooth_positions",
]


# --------------------------------------------------------------------------------------
# parameter mapping
# --------------------------------------------------------------------------------------

def rokoko_slider_to_fc(slider: float) -> float:
    """Rokoko Studio Preview's UI slider (0.5..10, default 7) -> design cutoff in Hz.

    Higher slider means smoother. The app inverts the value before sending it to its
    engine as `10.5 - slider`, and that engine value is the cutoff in Hz, 1:1.
    """
    if not 0.5 <= slider <= 10.0:
        raise ValueError("slider must be in [0.5, 10.0]")
    return 10.5 - slider


# --------------------------------------------------------------------------------------
# Butterworth design (analog prototype -> bilinear transform -> polynomial coefficients)
# --------------------------------------------------------------------------------------

def butter_lowpass(order: int, fc: float, fs: float):
    """Digital Butterworth low-pass. Returns (b, a), normalised so a[0] == 1.

    order : per-direction order. Use 2 to match Rokoko.
    fc    : cutoff in Hz. Must be below Nyquist.
    fs    : sample rate in Hz.
    """
    if order < 1:
        raise ValueError("order must be >= 1")
    nyq = fs / 2.0
    if not 0.0 < fc < nyq:
        raise ValueError(f"fc must be in (0, {nyq}); got {fc}")

    k = np.arange(order)
    poles = np.exp(1j * np.pi * (2 * k + order + 1) / (2 * order))  # unit-cutoff prototype
    wa = 2.0 * fs * np.tan(np.pi * fc / fs)                          # prewarp
    poles = wa * poles
    gain = wa ** order

    fs2 = 2.0 * fs
    poles_z = (fs2 + poles) / (fs2 - poles)
    zeros_z = -np.ones(order)
    gain_z = gain * np.real(1.0 / np.prod(fs2 - poles))

    b = gain_z * np.real(np.poly(zeros_z))
    a = np.real(np.poly(poles_z))
    return b / a[0], a / a[0]


def _lfilter_zi(b, a):
    """Steady-state initial conditions for a step input, so the filter starts settled."""
    n = len(a)
    A = np.zeros((n - 1, n - 1))
    A[:, 0] = -a[1:]
    if n > 2:
        A[:-1, 1:] += np.eye(n - 2)
    B = b[1:] - a[1:] * b[0]
    return np.linalg.solve(np.eye(n - 1) - A, B)


def _lfilter(b, a, x, zi):
    """Direct Form 2 Transposed IIR. x may be (N,) or (N, C); filters each column."""
    n = len(b)
    single = x.ndim == 1
    X = x[:, None] if single else x
    Y = np.empty_like(X)
    d = zi.copy()
    for i in range(X.shape[0]):
        xi = X[i]
        yi = b[0] * xi + d[0]
        for j in range(n - 2):
            d[j] = b[j + 1] * xi + d[j + 1] - a[j + 1] * yi
        d[n - 2] = b[n - 1] * xi - a[n - 1] * yi
        Y[i] = yi
    return Y[:, 0] if single else Y


def filtfilt(b, a, x):
    """Zero-phase filtering: forward then backward, with odd reflection padding.

    x may be (N,) or (N, C). Magnitude response is the square of the one-pass response,
    and the phase is exactly zero.
    """
    x = np.asarray(x, dtype=np.float64)
    single = x.ndim == 1
    X = x[:, None] if single else x
    if X.shape[0] < 4:
        return x.copy()

    ntaps = max(len(a), len(b))
    edge = min(3 * ntaps, X.shape[0] - 1)

    left = 2 * X[0] - X[edge:0:-1]
    right = 2 * X[-1] - X[-2:-edge - 2:-1]
    ext = np.concatenate([left, X, right])

    zi = _lfilter_zi(b, a)
    Y = _lfilter(b, a, ext, np.outer(zi, ext[0]))
    Y = _lfilter(b, a, Y[::-1], np.outer(zi, Y[-1]))[::-1]

    out = Y[len(left):len(left) + X.shape[0]]
    return out[:, 0] if single else out


# --------------------------------------------------------------------------------------
# Gaussian (strictly positive kernel, so it can never overshoot)
# --------------------------------------------------------------------------------------

def gaussian_kernel(sigma: float, truncate: float = 4.0) -> np.ndarray:
    if sigma <= 0:
        raise ValueError("sigma must be > 0")
    radius = max(1, int(np.ceil(truncate * sigma)))
    k = np.exp(-(np.arange(-radius, radius + 1) ** 2) / (2.0 * sigma * sigma))
    return k / k.sum()


def gaussian_filter(x, sigma: float):
    """Gaussian convolution with edge clamping. x may be (N,) or (N, C)."""
    x = np.asarray(x, dtype=np.float64)
    single = x.ndim == 1
    X = x[:, None] if single else x
    k = gaussian_kernel(sigma)
    r = len(k) // 2
    pad = np.concatenate([np.repeat(X[:1], r, axis=0), X, np.repeat(X[-1:], r, axis=0)])
    out = np.empty_like(X)
    for c in range(X.shape[1]):
        out[:, c] = np.convolve(pad[:, c], k, mode="valid")
    return out[:, 0] if single else out


def gaussian_sigma_for_cutoff(fc: float, fs: float) -> float:
    """sigma in samples giving a -3 dB point at fc Hz. f_3dB ~= 0.1325 * fs / sigma."""
    return 0.1325 * fs / fc


# --------------------------------------------------------------------------------------
# rotations
# --------------------------------------------------------------------------------------

def make_quaternions_continuous(q: np.ndarray) -> np.ndarray:
    """Flip signs so the quaternion path does not jump between q and -q.

    Required before filtering components, since q and -q are the same rotation but are
    far apart numerically. Input and output are (N, 4); component order does not matter
    as long as it is consistent.
    """
    q = np.array(q, dtype=np.float64, copy=True)
    for i in range(1, len(q)):
        if np.dot(q[i], q[i - 1]) < 0.0:
            q[i] = -q[i]
    return q


def smooth_quaternions(q, fc: float, fs: float, order: int = 2, shape: str = "butter"):
    """Smooth a quaternion track. q is (N, 4). Returns (N, 4), renormalised.

    shape: "butter" (Rokoko-identical, rings slightly) or "gaussian" (never overshoots).
    """
    q = make_quaternions_continuous(np.asarray(q, dtype=np.float64))
    if shape == "butter":
        b, a = butter_lowpass(order, fc, fs)
        out = filtfilt(b, a, q)
    elif shape == "gaussian":
        out = gaussian_filter(q, gaussian_sigma_for_cutoff(fc, fs))
    else:
        raise ValueError("shape must be 'butter' or 'gaussian'")
    norms = np.linalg.norm(out, axis=1, keepdims=True)
    norms[norms == 0.0] = 1.0
    return out / norms


def smooth_positions(p, fc: float, fs: float, order: int = 2, shape: str = "butter"):
    """Smooth a translation track. p is (N, 3).

    Only the root and pelvis normally have meaningful translation. Smoothing global
    motion is what causes foot sliding, so be conservative here.
    """
    p = np.asarray(p, dtype=np.float64)
    if shape == "butter":
        b, a = butter_lowpass(order, fc, fs)
        return filtfilt(b, a, p)
    if shape == "gaussian":
        return gaussian_filter(p, gaussian_sigma_for_cutoff(fc, fs))
    raise ValueError("shape must be 'butter' or 'gaussian'")


# --------------------------------------------------------------------------------------
# self-test
# --------------------------------------------------------------------------------------

def _gain_at(b, a, f, fs, n=16384):
    """Empirical amplitude gain of filtfilt(b, a) at frequency f."""
    t = np.arange(n) / fs
    x = np.sin(2 * np.pi * f * t)
    y = filtfilt(b, a, x)
    s = slice(n // 4, 3 * n // 4)
    return np.sqrt(np.mean(y[s] ** 2)) / np.sqrt(np.mean(x[s] ** 2))


def _self_test():
    fs = 60.0
    ok = True

    print("1. slider mapping")
    for slider, want in ((7.0, 3.5), (10.0, 0.5), (0.5, 10.0)):
        got = rokoko_slider_to_fc(slider)
        good = abs(got - want) < 1e-9
        ok &= good
        print(f"   slider {slider:4.1f} -> fc {got:4.1f} Hz  {'ok' if good else 'FAIL'}")

    print("2. overall -3 dB knee should be 0.802 * fc for filtfilt order 2")
    fc = 3.5
    b, a = butter_lowpass(2, fc, fs)
    expected = 0.802 * fc
    ff = np.arange(expected - 0.6, expected + 0.6, 0.02)
    mags = np.array([_gain_at(b, a, f, fs) for f in ff])
    measured = ff[np.argmin(np.abs(mags - 1 / np.sqrt(2)))]
    good = abs(measured - expected) / expected < 0.02
    ok &= good
    print(f"   measured {measured:.3f} Hz, expected {expected:.3f} Hz  {'ok' if good else 'FAIL'}")

    print("3. zero phase: a symmetric input must stay symmetric")
    n = 513
    x = np.zeros(n)
    x[n // 2] = 1.0
    y = filtfilt(b, a, x)
    asym = np.max(np.abs(y - y[::-1]))
    good = asym < 1e-9
    ok &= good
    print(f"   max asymmetry {asym:.2e}  {'ok' if good else 'FAIL'}")

    print("4. Gaussian must never overshoot")
    step = np.concatenate([np.zeros(200), np.ones(200)])
    g = gaussian_filter(step, 2.0)
    over = g.max() - 1.0
    good = over < 1e-12
    ok &= good
    print(f"   overshoot {over:.2e}  {'ok' if good else 'FAIL'}")

    print("5. Butterworth is expected to overshoot about 3.35 percent")
    bo = filtfilt(*butter_lowpass(2, 3.6, fs), step)
    over = (bo.max() - 1.0) * 100
    good = 2.0 < over < 5.0
    ok &= good
    print(f"   overshoot {over:.2f} percent  {'ok' if good else 'FAIL'}")

    print("6. quaternion smoothing keeps unit norm through a sign flip")
    ang = np.linspace(0, np.pi * 1.5, 300)
    q = np.stack([np.sin(ang / 2), np.zeros(300), np.zeros(300), np.cos(ang / 2)], axis=1)
    q[150:] *= -1
    sq = smooth_quaternions(q, 3.5, fs)
    err = np.max(np.abs(np.linalg.norm(sq, axis=1) - 1.0))
    good = err < 1e-12
    ok &= good
    print(f"   max norm error {err:.2e}  {'ok' if good else 'FAIL'}")

    print("\n" + ("ALL PASS" if ok else "FAILURES ABOVE"))
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(_self_test())
