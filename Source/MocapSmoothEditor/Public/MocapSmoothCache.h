// SPDX-License-Identifier: Apache-2.0
//
// THE UNTOUCHED ORIGINAL, kept safe outside the asset.
//
// THE RULE THIS FILE ENFORCES
//     Smoothing always reads from a pristine copy of the raw animation, never from whatever is
//     currently on the asset. Filtering an already-filtered curve compounds silently, so without
//     that rule "change the slider from 5 to 6" would quietly mean "5, and then 6 on top".
//
// WHERE IT LIVES
//     <Project>/Saved/MocapSmooth/<asset>.mocapraw    the original bone tracks. Write-once.
//     <Project>/Saved/MocapSmooth/<asset>.json        which FBX it came from, and a signature of
//                                                     what we last wrote.
//     Both are regenerable, and Saved/ is outside version control. Storing the original inside the
//     asset is not an option -- 80 bones x 24,331 frames is tens of MB that every save would
//     rewrite into version control.
//
// WHEN THE ORIGINAL IS REPLACED -- and it is the only time
//     When the source FBX changes. The meta file records the source file's path, size and modified
//     time at capture; if those differ the take has been re-exported and reimported, so the asset
//     holds genuinely new raw data and a new original is taken. Nothing else replaces it. There is
//     no everyday switch that can, which is the point.
//
// THE ONE WAY IT COULD STILL GO WRONG, AND THE GUARD
//     Capturing an "original" from an asset that is secretly already smoothed. So every write
//     records a cheap signature of what was written (64 sampled frames), and a capture that is
//     about to REPLACE an existing original checks it: if the asset still matches what we last
//     wrote, the capture is refused and the log says to reimport the FBX instead.
//
//     The very first capture on a take cannot be guarded -- there is nothing to compare against,
//     and the asset is assumed raw. Capture before you smooth, which is what happens naturally.

#pragma once

#include "CoreMinimal.h"

class UAnimSequence;

/** Local-space bone tracks for a whole AnimSequence. */
struct FMocapTracks
{
	TArray<FName> BoneNames;
	int32 NumFrames = 0;
	/** Per bone, NumFrames entries each. */
	TArray<TArray<FVector3f>> Positions;
	TArray<TArray<FQuat4f>> Rotations;
	TArray<TArray<FVector3f>> Scales;

	int32 NumBones() const { return BoneNames.Num(); }
	bool IsValidFor(const TArray<FName>& Tracks, int32 Frames) const
	{
		return NumFrames == Frames && BoneNames == Tracks;
	}
};

struct FMocapSmoothMeta
{
	/** "<fbx path>|<size>|<unix mtime>", or empty when it cannot be read. */
	FString Source;
	FString Captured;
	FString LastWritten;
	/** Human-readable record of the settings behind the last write, for the log and for you. */
	FString LastSettings;
	int32 Frames = 0;
	int32 Bones = 0;
	/** How many smoothing passes are stacked on the asset right now. 0 == it holds the original. */
	int32 Passes = 0;
	/** Signature of the animation we last wrote. bHasProbe == false means "nothing written yet". */
	uint32 LastWriteProbe = 0;
	bool bHasProbe = false;
};

namespace MocapSmoothCache
{
	MOCAPSMOOTHEDITOR_API FString PayloadPath(const UAnimSequence* Anim);
	MOCAPSMOOTHEDITOR_API FString MetaPath(const UAnimSequence* Anim);
	MOCAPSMOOTHEDITOR_API bool HasOriginal(const UAnimSequence* Anim);

	MOCAPSMOOTHEDITOR_API bool LoadMeta(const UAnimSequence* Anim, FMocapSmoothMeta& OutMeta);
	MOCAPSMOOTHEDITOR_API bool SaveMeta(const UAnimSequence* Anim, const FMocapSmoothMeta& Meta);

	/** Identity of the FBX this AnimSequence was imported from. Empty if it cannot be read.
	    A re-export changes the size or the timestamp; a reimport of the identical file changes
	    neither -- exactly the distinction we want. */
	MOCAPSMOOTHEDITOR_API FString SourceFingerprint(const UAnimSequence* Anim);

	/** Read every track out of the asset as it stands now. One call per bone, not per frame. */
	MOCAPSMOOTHEDITOR_API bool ReadCurrent(const UAnimSequence* Anim, const TArray<FName>& Tracks,
	                                       FMocapTracks& Out);

	/** Cheap signature of the animation currently on the asset. 64 frames spread across the take. */
	MOCAPSMOOTHEDITOR_API uint32 Probe(const UAnimSequence* Anim, const TArray<FName>& Tracks);

	/**
	 * The untouched original. Captured on first use; after that only a changed source FBX
	 * replaces it, and never when the asset still holds our own last write.
	 * bOutCaptured says whether this call did the capturing.
	 */
	MOCAPSMOOTHEDITOR_API bool LoadOrCapture(const UAnimSequence* Anim, const TArray<FName>& Tracks,
	                                         FMocapTracks& Out, bool& bOutCaptured);

	/** Remember what we just wrote, so a later capture can recognise it and refuse. */
	MOCAPSMOOTHEDITOR_API void RecordWrite(const UAnimSequence* Anim, const TArray<FName>& Tracks,
	                                       int32 Passes, const FString& Settings);

	/** Discard the cached original. Backs up to .mocapraw.bak first and still refuses when the
	    asset holds our own smoothing. Reimporting the FBX is the safe route; this is the hatch. */
	MOCAPSMOOTHEDITOR_API bool ForgetOriginal(const UAnimSequence* Anim);
}
