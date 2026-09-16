// SPDX-License-Identifier: Apache-2.0

#include "MocapSmoothRegions.h"

#include "ReferenceSkeleton.h"

namespace
{
	const TCHAR* GPelvisNames[] = { TEXT("pelvis"), TEXT("hips"), TEXT("hip") };
	const TCHAR* GLowerHints[] = { TEXT("thigh"), TEXT("upleg"), TEXT("leg"), TEXT("femur"),
	                               TEXT("foot"), TEXT("ankle"), TEXT("toe"), TEXT("ball") };
	const TCHAR* GUpperHints[] = { TEXT("spine"), TEXT("neck"), TEXT("head"), TEXT("clavicle"),
	                               TEXT("shoulder"), TEXT("chest"), TEXT("torso"), TEXT("arm") };

	bool ContainsAny(const FString& Lower, const TCHAR* const* Hints, int32 Count)
	{
		for (int32 i = 0; i < Count; ++i)
		{
			if (Lower.Contains(Hints[i]))
			{
				return true;
			}
		}
		return false;
	}
}

namespace MocapSmoothRegions
{

const TCHAR* ToString(EMocapRegion Region)
{
	switch (Region)
	{
	case EMocapRegion::Root:    return TEXT("root");
	case EMocapRegion::Pelvis:  return TEXT("pelvis");
	case EMocapRegion::Lower:   return TEXT("lower");
	case EMocapRegion::Upper:   return TEXT("upper");
	case EMocapRegion::Hands:   return TEXT("hands");
	case EMocapRegion::Fingers: return TEXT("fingers");
	default:                    return TEXT("?");
	}
}

void Classify(const FReferenceSkeleton& RefSkeleton, const TArray<FName>& TrackNames,
              TArray<EMocapRegion>& OutRegions)
{
	const int32 NumBones = RefSkeleton.GetNum();
	OutRegions.Init(EMocapRegion::Upper, TrackNames.Num());
	if (NumBones == 0)
	{
		return;
	}

	TArray<FString> LowerNames;
	LowerNames.SetNum(NumBones);
	for (int32 i = 0; i < NumBones; ++i)
	{
		LowerNames[i] = RefSkeleton.GetBoneName(i).ToString().ToLower();
	}

	// --- hand bones: the shallowest bones whose name mentions "hand". Catches hand_l / hand_r (UE)
	// and LeftHand / RightHand (Rokoko Newton) while rejecting LeftHandIndex1, which has a hand
	// candidate as an ancestor. ik_* rigs are not real hands.
	TSet<int32> Candidates;
	for (int32 i = 0; i < NumBones; ++i)
	{
		if (LowerNames[i].Contains(TEXT("hand")) && !LowerNames[i].StartsWith(TEXT("ik_")))
		{
			Candidates.Add(i);
		}
	}
	TSet<int32> Hands;
	for (int32 i : Candidates)
	{
		bool bHasHandAncestor = false;
		for (int32 P = RefSkeleton.GetParentIndex(i); P != INDEX_NONE; P = RefSkeleton.GetParentIndex(P))
		{
			if (Candidates.Contains(P))
			{
				bHasHandAncestor = true;
				break;
			}
		}
		if (!bHasHandAncestor)
		{
			Hands.Add(i);
		}
	}

	// --- pelvis: by name if we can, otherwise the deepest-subtree branching bone.
	int32 Pelvis = INDEX_NONE;
	for (int32 i = 0; i < NumBones && Pelvis == INDEX_NONE; ++i)
	{
		for (const TCHAR* Name : GPelvisNames)
		{
			if (LowerNames[i] == Name)
			{
				Pelvis = i;
				break;
			}
		}
	}
	TArray<int32> ChildCount;
	ChildCount.Init(0, NumBones);
	TArray<int32> SubtreeSize;
	SubtreeSize.Init(1, NumBones);
	for (int32 i = NumBones - 1; i >= 0; --i)
	{
		const int32 P = RefSkeleton.GetParentIndex(i);
		if (P != INDEX_NONE)
		{
			++ChildCount[P];
			SubtreeSize[P] += SubtreeSize[i];   // children always follow parents in a ref skeleton
		}
	}
	if (Pelvis == INDEX_NONE)
	{
		int32 Best = INDEX_NONE;
		for (int32 i = 0; i < NumBones; ++i)
		{
			if (ChildCount[i] >= 2 && (Best == INDEX_NONE || SubtreeSize[i] > SubtreeSize[Best]))
			{
				Best = i;
			}
		}
		Pelvis = Best;
	}

	// --- which side of the body each pelvis child subtree belongs to
	TMap<int32, EMocapRegion> Side;
	if (Pelvis != INDEX_NONE)
	{
		for (int32 i = 0; i < NumBones; ++i)
		{
			if (RefSkeleton.GetParentIndex(i) != Pelvis)
			{
				continue;
			}
			bool bSubtreeHasHand = false;
			for (int32 H : Hands)
			{
				for (int32 P = H; P != INDEX_NONE; P = RefSkeleton.GetParentIndex(P))
				{
					if (P == i)
					{
						bSubtreeHasHand = true;
						break;
					}
				}
				if (bSubtreeHasHand)
				{
					break;
				}
			}
			if (bSubtreeHasHand || ContainsAny(LowerNames[i], GUpperHints, UE_ARRAY_COUNT(GUpperHints)))
			{
				Side.Add(i, EMocapRegion::Upper);
			}
			else if (ContainsAny(LowerNames[i], GLowerHints, UE_ARRAY_COUNT(GLowerHints)))
			{
				Side.Add(i, EMocapRegion::Lower);
			}
			else
			{
				Side.Add(i, EMocapRegion::Upper);
			}
		}
	}

	for (int32 t = 0; t < TrackNames.Num(); ++t)
	{
		const int32 Index = RefSkeleton.FindBoneIndex(TrackNames[t]);
		if (Index == INDEX_NONE)
		{
			OutRegions[t] = EMocapRegion::Upper;
			continue;
		}

		bool bHandAncestor = false;
		bool bUnderPelvis = false;
		int32 PelvisChildOnPath = Index;
		for (int32 P = RefSkeleton.GetParentIndex(Index); P != INDEX_NONE;
		     P = RefSkeleton.GetParentIndex(P))
		{
			if (Hands.Contains(P))
			{
				bHandAncestor = true;
				break;
			}
			if (P == Pelvis)
			{
				bUnderPelvis = true;
				break;
			}
			PelvisChildOnPath = P;
		}

		if (bHandAncestor)
		{
			OutRegions[t] = EMocapRegion::Fingers;
		}
		else if (Hands.Contains(Index))
		{
			OutRegions[t] = EMocapRegion::Hands;
		}
		else if (Index == Pelvis)
		{
			OutRegions[t] = EMocapRegion::Pelvis;
		}
		else if (bUnderPelvis)
		{
			const EMocapRegion* Found = Side.Find(PelvisChildOnPath);
			OutRegions[t] = Found ? *Found : EMocapRegion::Upper;
		}
		else
		{
			OutRegions[t] = EMocapRegion::Root;
		}
	}
}

FString Summarise(const TArray<EMocapRegion>& Regions)
{
	int32 Counts[static_cast<int32>(EMocapRegion::Count)] = { 0 };
	for (EMocapRegion R : Regions)
	{
		++Counts[static_cast<int32>(R)];
	}
	static const EMocapRegion Order[] = { EMocapRegion::Fingers, EMocapRegion::Hands,
	                                      EMocapRegion::Upper, EMocapRegion::Lower,
	                                      EMocapRegion::Pelvis, EMocapRegion::Root };
	TArray<FString> Parts;
	for (EMocapRegion R : Order)
	{
		const int32 C = Counts[static_cast<int32>(R)];
		if (C > 0)
		{
			Parts.Add(FString::Printf(TEXT("%s=%d"), ToString(R), C));
		}
	}
	return FString::Join(Parts, TEXT(", "));
}

} // namespace MocapSmoothRegions
