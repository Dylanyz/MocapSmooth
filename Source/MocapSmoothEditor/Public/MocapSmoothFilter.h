// SPDX-License-Identifier: Apache-2.0
//
// Zero-phase smoothing, ported 1:1 from the measurement-validated reference implementation
// (~/.claude/skills/mocap-smoothing/scripts/smooth_core.py, validated against Rokoko Studio
// Preview 1.2.0 to 0.016-0.19 degrees mean angular error per joint).
//
// The filter Rokoko Studio Preview uses is a zero-phase forward-backward ("filtfilt")
// 2nd-order Butterworth applied per curve, with
//
//     fc_Hz = 10.5 - slider          (slider is Rokoko's 0.5..10 UI value; 7 -> 3.5 Hz)
//
// Because it runs in both directions the OVERALL -3 dB knee sits at 0.802 * fc.
//
// Zero phase is the whole point: a causal filter shifts motion later in time, which would break
// body-to-face sync. Never substitute One-Euro, a spring, or an AnimBP node for offline cleanup.

#pragma once

#include "CoreMinimal.h"

enum class EMocapSmoothShapeInternal : uint8
{
	Butterworth,
	Gaussian,
};

namespace MocapSmoothFilter
{
	/** Rokoko's UI slider (0.5..10, higher is smoother) -> design cutoff in Hz. */
	MOCAPSMOOTHEDITOR_API double SliderToCutoffHz(double Slider);

	/**
	 * Digital Butterworth low-pass via analog prototype + bilinear transform.
	 * Fills B and A with Order+1 coefficients each, normalised so A[0] == 1.
	 * Returns false if fc is not strictly between 0 and Nyquist.
	 */
	MOCAPSMOOTHEDITOR_API bool DesignButterworth(int32 Order, double CutoffHz, double SampleRateHz,
	                                             TArray<double>& OutB, TArray<double>& OutA);

	/**
	 * Zero-phase filtering: forward then backward, with odd reflection padding at both ends.
	 * Values is NumSamples x NumChannels, channel-interleaved, filtered in place.
	 * Sequences shorter than 4 samples are left alone.
	 */
	MOCAPSMOOTHEDITOR_API void FiltFilt(const TArray<double>& B, const TArray<double>& A,
	                                    TArray<double>& Values, int32 NumChannels);

	/** sigma in samples giving a -3 dB point at CutoffHz. f_3dB ~= 0.1325 * fs / sigma. */
	MOCAPSMOOTHEDITOR_API double GaussianSigmaForCutoff(double CutoffHz, double SampleRateHz);

	/**
	 * Gaussian convolution with edge clamping, in place. Strictly positive kernel, so it can
	 * never overshoot -- which is why it is the option for hands and fingers, where a hard stop
	 * makes the Butterworth's ~3.4% ringing visible.
	 */
	MOCAPSMOOTHEDITOR_API void GaussianFilter(TArray<double>& Values, int32 NumChannels, double Sigma);

	/**
	 * Smooth a quaternion track in place. q and -q are the same rotation but are far apart
	 * numerically, so signs are made continuous first and the result is renormalised. Filtering
	 * Euler channels instead would invite +/-180 wrap blowups and gimbal coupling.
	 */
	MOCAPSMOOTHEDITOR_API void SmoothQuaternions(TArrayView<FQuat4f> Rotations, double CutoffHz,
	                                             double SampleRateHz, int32 Order,
	                                             EMocapSmoothShapeInternal Shape);

	/** Smooth a translation track in place. Only root and pelvis normally carry real translation. */
	MOCAPSMOOTHEDITOR_API void SmoothPositions(TArrayView<FVector3f> Positions, double CutoffHz,
	                                           double SampleRateHz, int32 Order,
	                                           EMocapSmoothShapeInternal Shape);

	/** Self-test mirroring smooth_core.py's. Logs each check; returns true if all pass. */
	MOCAPSMOOTHEDITOR_API bool SelfTest();
}
