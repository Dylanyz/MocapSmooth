// SPDX-License-Identifier: Apache-2.0
//
// Bone region classification, mirroring Rokoko Studio Preview's smoothing groups.
//
// Regions are resolved by WALKING THE SKELETON HIERARCHY, never by string prefixes -- names like
// `upperarm_correctiveroot_l`, `pinky_03_bulge_l` and `wrist_inner_l` do not partition cleanly on
// a MetaHuman skeleton.
//
// Rokoko's groups, for reference:
//     _fullbody   all bones
//     _body       all bones EXCEPT the finger bones      <- the practical default
//     _upperbody  spine and up, excluding fingers
//     _lowerbody  hips and down, EXCLUDING the hips bone
//     _hands      hand bones including all finger bones
//
// `_body` is not a region of its own: it is FullBody with Fingers switched off. That is why
// Fingers exists separately from Hands -- the exemption stops at the hand bone's children, so
// under Body the wrist is still filtered.

#pragma once

#include "CoreMinimal.h"

struct FReferenceSkeleton;

enum class EMocapRegion : uint8
{
	Root,
	Pelvis,
	Lower,
	Upper,
	Hands,
	Fingers,
	Count
};

namespace MocapSmoothRegions
{
	/** Human-readable name, for logging. */
	MOCAPSMOOTHEDITOR_API const TCHAR* ToString(EMocapRegion Region);

	/**
	 * Region per track name. Unlike the Python route (which needed a SkeletalMesh because
	 * `unreal.Skeleton` exposes no hierarchy), C++ reads parents straight off the reference
	 * skeleton, so nothing but the Skeleton asset is required.
	 *
	 * Track names not present in the reference skeleton fall back to Upper.
	 */
	MOCAPSMOOTHEDITOR_API void Classify(const FReferenceSkeleton& RefSkeleton,
	                                    const TArray<FName>& TrackNames,
	                                    TArray<EMocapRegion>& OutRegions);

	/** "fingers=38, hands=2, upper=22, lower=16, pelvis=1, root=1" */
	MOCAPSMOOTHEDITOR_API FString Summarise(const TArray<EMocapRegion>& Regions);
}
