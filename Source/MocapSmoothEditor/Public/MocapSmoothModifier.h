// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "CoreMinimal.h"
#include "AnimationModifier.h"
#include "MocapSmoothModifier.generated.h"

UENUM()
enum class EMocapSmoothShape : uint8
{
	/** Matches Rokoko exactly. Sharper separation of jitter from performance, rings ~3.4% on a hard stop. */
	Butterworth UMETA(DisplayName = "Butterworth (matches Rokoko)"),
	/** Can never overshoot, at the cost of a gentler roll-off. */
	Gaussian    UMETA(DisplayName = "Gaussian (never overshoots)"),
};

/**
 * Smooths motion capture with the filter measured out of Rokoko Studio Preview: a zero-phase
 * forward-backward 2nd-order Butterworth, cutoff = 10.5 - strength Hz.
 *
 * Zero phase matters: a causal filter shifts motion later in time and would break body-to-face
 * sync. Every Apply reads from an untouched copy of the raw animation kept in Saved/MocapSmooth,
 * so changing the strength and re-applying lands on that strength instead of stacking.
 */
UCLASS(config = Editor, DisplayName = "Mocap Smooth")
class MOCAPSMOOTHEDITOR_API UMocapSmoothModifier : public UAnimationModifier
{
	GENERATED_BODY()

public:
	UMocapSmoothModifier();

	// ---------------------------------------------------------------------------- 1 Strength

	/**
	 * Master smoothing strength, on Rokoko Studio's 0.5-10 scale. Higher is smoother.
	 *
	 * It sets a cutoff frequency: motion slower than the cutoff is kept, anything faster is
	 * treated as jitter and removed. 5 = 5.5 Hz (gentle), 7 = 3.5 Hz (Rokoko's own default).
	 *
	 * Every region below falls back to this number unless you override it.
	 */
	UPROPERTY(EditAnywhere, Category = "1 Strength", meta = (DisplayName = "Full Body",
		UIMin = "0.5", UIMax = "10.0", ClampMin = "0.5", ClampMax = "10.0"))
	float FullBody;

	// ---------------------------------------------------------------------------- 2 Regions

	/**
	 * Finger bones only - everything below the hand bone.
	 *
	 * -1 = Off (left completely alone), 0 = inherit Hands, then Full Body.
	 *
	 * Off is the default. Fingers carry most of a performance's read and are the first thing
	 * smoothing flattens. Fingers Off with everything else on is exactly Rokoko's "Body" group.
	 */
	UPROPERTY(EditAnywhere, Category = "2 Regions", meta = (DisplayName = "Fingers",
		UIMin = "-1.0", UIMax = "10.0", ClampMin = "-1.0", ClampMax = "10.0"))
	float Fingers;

	/**
	 * The hand/wrist bone itself, not the fingers.
	 *
	 * -1 = Off, 0 = inherit Upper Body, then Full Body.
	 *
	 * Turning this Off is NOT how you protect fingers - it leaves wrist jitter in an otherwise
	 * smooth arm. Use Fingers = -1 for that.
	 */
	UPROPERTY(EditAnywhere, Category = "2 Regions", meta = (DisplayName = "Hands",
		UIMin = "-1.0", UIMax = "10.0", ClampMin = "-1.0", ClampMax = "10.0"))
	float Hands;

	/**
	 * Spine and above, excluding fingers.
	 *
	 * -1 = Off, 0 = inherit Full Body.
	 */
	UPROPERTY(EditAnywhere, Category = "2 Regions", meta = (DisplayName = "Upper Body",
		UIMin = "-1.0", UIMax = "10.0", ClampMin = "-1.0", ClampMax = "10.0"))
	float UpperBody;

	/**
	 * Hips and below, excluding the pelvis itself.
	 *
	 * -1 = Off, 0 = inherit Full Body.
	 *
	 * The pelvis is deliberately excluded, as it is in Rokoko's Lower Body group - smoothing
	 * global motion is what causes foot sliding.
	 */
	UPROPERTY(EditAnywhere, Category = "2 Regions", meta = (DisplayName = "Lower Body",
		UIMin = "-1.0", UIMax = "10.0", ClampMin = "-1.0", ClampMax = "10.0"))
	float LowerBody;

	// ---------------------------------------------------------------------------- 3 Filter

	/**
	 * Butterworth matches Rokoko exactly and separates jitter from performance more sharply, but
	 * overshoots about 3.4% on a hard stop.
	 *
	 * A Gaussian can never overshoot, at the cost of a gentler roll-off - so it removes less
	 * jitter for the same amount of performance lost.
	 */
	UPROPERTY(EditAnywhere, Category = "3 Filter", meta = (DisplayName = "Shape"))
	EMocapSmoothShape Shape;

	/**
	 * Use the non-overshooting Gaussian on hands and fingers only, leaving everything else on the
	 * Shape above.
	 *
	 * Hard finger stops - a fist, a point, a grab - are where Butterworth's ringing actually shows.
	 */
	UPROPERTY(EditAnywhere, Category = "3 Filter", meta = (DisplayName = "Gaussian for Hands and Fingers"))
	bool bGaussianForHandsAndFingers;

	/**
	 * Steepness of the filter's roll-off. 2 is what Rokoko uses; leave it alone to match.
	 *
	 * Higher cuts more sharply between kept and removed motion, and rings more.
	 */
	UPROPERTY(EditAnywhere, Category = "3 Filter", meta = (DisplayName = "Order",
		UIMin = "1", UIMax = "4", ClampMin = "1", ClampMax = "4"))
	int32 FilterOrder;

	// ---------------------------------------------------------------------------- 4 Advanced

	/**
	 * Off (default): always smooth from the untouched copy of the raw animation, so changing the
	 * strength and re-applying lands on that strength instead of stacking.
	 *
	 * On: smooth the result already on the asset, adding a second pass on top.
	 *
	 * The raw copy is kept either way, so Revert still takes you all the way back in one step,
	 * however many passes you stacked.
	 */
	UPROPERTY(EditAnywhere, Category = "4 Advanced", meta = (DisplayName = "Smooth On Top Of Previous"))
	bool bSmoothOnTopOfPrevious;

	/** Also smooth the root bone. Off by default - smoothing the root is what makes feet slide. */
	UPROPERTY(EditAnywhere, Category = "4 Advanced", meta = (DisplayName = "Smooth Root"))
	bool bSmoothRoot;

	/**
	 * Filter the pelvis's position as well as its rotation. Rokoko's Full Body group does.
	 *
	 * Turn it off if the character's global motion starts sliding.
	 */
	UPROPERTY(EditAnywhere, Category = "4 Advanced", meta = (DisplayName = "Smooth Pelvis Translation"))
	bool bSmoothPelvisTranslation;

	/**
	 * Rokoko's FBX export has "Include reference pose" ticked by default, which puts an A-pose on
	 * frame 0. That 15-20 degree jump makes a zero-phase filter ring across the first ~20 frames.
	 *
	 * When one is detected, filtering starts at frame 1 and frame 0 is left untouched. Detection
	 * is by outlier, so a clip that genuinely starts fast is not truncated.
	 */
	UPROPERTY(EditAnywhere, Category = "4 Advanced", meta = (DisplayName = "Skip Leading Reference Pose"))
	bool bSkipLeadingReferencePose;

	//~ Begin UAnimationModifier interface
	virtual void OnApply_Implementation(UAnimSequence* AnimationSequence) override;
	virtual void OnRevert_Implementation(UAnimSequence* AnimationSequence) override;
	//~ End UAnimationModifier interface
};
