// SPDX-License-Identifier: Apache-2.0

#include "MocapSmoothModifier.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/Skeleton.h"
#include "MocapSmoothCache.h"
#include "MocapSmoothFilter.h"
#include "MocapSmoothRegions.h"
#include "Misc/ScopedSlowTask.h"

#define LOCTEXT_NAMESPACE "MocapSmooth"

DEFINE_LOG_CATEGORY_STATIC(LogMocapSmooth, Log, All);

namespace
{
	constexpr float kOff = -1.0f;
	constexpr float kInherit = 0.0f;

	/** First explicit value down the inheritance chain. Returns false for "do not filter". */
	bool Resolve(std::initializer_list<float> Values, float& OutSlider)
	{
		for (float V : Values)
		{
			if (V < -0.001f)
			{
				return false;
			}
			if (V > 0.001f)
			{
				OutSlider = V;
				return true;
			}
		}
		return false;
	}

	/**
	 * True if frame 0 looks like an embedded reference pose rather than captured motion.
	 * Measured on this pipeline's takes: frame 0 -> 1 moves 15-19 degrees mean while real
	 * consecutive frames move 0.07-0.8. Detect the outlier rather than assume it.
	 */
	bool HasLeadingReferencePose(const FMocapTracks& Tracks)
	{
		const int32 N = Tracks.NumFrames;
		const int32 B = Tracks.NumBones();
		if (N < 8 || B == 0)
		{
			return false;
		}
		const int32 K = FMath::Min(90, N - 1);
		TArray<double> Step;
		Step.SetNumZeroed(K);
		for (int32 b = 0; b < B; ++b)
		{
			const TArray<FQuat4f>& R = Tracks.Rotations[b];
			for (int32 i = 0; i < K; ++i)
			{
				const double Dot = FMath::Clamp<double>(FMath::Abs(
					double(R[i].X) * R[i + 1].X + double(R[i].Y) * R[i + 1].Y +
					double(R[i].Z) * R[i + 1].Z + double(R[i].W) * R[i + 1].W), 0.0, 1.0);
				Step[i] += FMath::RadiansToDegrees(2.0 * FMath::Acos(Dot));
			}
		}
		for (double& V : Step)
		{
			V /= B;
		}
		TArray<double> Rest(Step.GetData() + 1, K - 1);
		Rest.Sort();
		const double Median = Rest.Num() > 0 ? Rest[Rest.Num() / 2] : 0.0;
		return Step[0] > 2.0 && Step[0] > 10.0 * FMath::Max(Median, 1e-4);
	}
}

UMocapSmoothModifier::UMocapSmoothModifier()
	: FullBody(5.0f)
	, Fingers(kOff)
	, Hands(kInherit)
	, UpperBody(kInherit)
	, LowerBody(kInherit)
	, Shape(EMocapSmoothShape::Butterworth)
	, bGaussianForHandsAndFingers(false)
	, FilterOrder(2)
	, bSmoothOnTopOfPrevious(false)
	, bSmoothRoot(false)
	, bSmoothPelvisTranslation(true)
	, bSkipLeadingReferencePose(true)
{
	// Reimporting the source FBX rebuilds the data, so re-run and the FBX stays ground truth.
	bReapplyPostOwnerChange = true;
}

void UMocapSmoothModifier::OnApply_Implementation(UAnimSequence* AnimationSequence)
{
	if (!AnimationSequence)
	{
		return;
	}
	const IAnimationDataModel* Model = AnimationSequence->GetDataModel();
	if (!Model)
	{
		UE_LOG(LogMocapSmooth, Error, TEXT("[MocapSmooth] %s has no animation data model."),
		       *AnimationSequence->GetName());
		return;
	}

	TArray<FName> Tracks;
	Model->GetBoneTrackNames(Tracks);
	if (Tracks.Num() == 0)
	{
		UE_LOG(LogMocapSmooth, Warning, TEXT("[MocapSmooth] %s has no bone tracks."),
		       *AnimationSequence->GetName());
		return;
	}

	const USkeleton* Skeleton = AnimationSequence->GetSkeleton();
	if (!Skeleton)
	{
		UE_LOG(LogMocapSmooth, Error, TEXT("[MocapSmooth] %s has no skeleton. Nothing changed."),
		       *AnimationSequence->GetName());
		return;
	}
	TArray<EMocapRegion> Regions;
	MocapSmoothRegions::Classify(Skeleton->GetReferenceSkeleton(), Tracks, Regions);

	const FFrameRate FrameRate = Model->GetFrameRate();
	const double SampleRate = FrameRate.AsDecimal();
	if (SampleRate <= 0.0)
	{
		UE_LOG(LogMocapSmooth, Error, TEXT("[MocapSmooth] %s has an invalid frame rate."),
		       *AnimationSequence->GetName());
		return;
	}

	FScopedSlowTask SlowTask(3.0f,
		FText::Format(LOCTEXT("Smoothing", "Mocap smoothing {0}"),
		              FText::FromString(AnimationSequence->GetName())));
	SlowTask.MakeDialog();

	// Always resolve the original first, even for "on top": it is what makes Revert work, and on a
	// take that has never been smoothed it is captured here while the asset is still raw.
	SlowTask.EnterProgressFrame(1.0f, LOCTEXT("Reading", "Reading the untouched original"));
	FMocapTracks Source;
	bool bCaptured = false;
	if (!MocapSmoothCache::LoadOrCapture(AnimationSequence, Tracks, Source, bCaptured))
	{
		UE_LOG(LogMocapSmooth, Error,
		       TEXT("[MocapSmooth] %s: could not read the original. Nothing changed."),
		       *AnimationSequence->GetName());
		return;
	}

	FMocapSmoothMeta Meta;
	MocapSmoothCache::LoadMeta(AnimationSequence, Meta);
	const int32 PriorPasses = Meta.Passes;

	if (bSmoothOnTopOfPrevious)
	{
		if (PriorPasses <= 0)
		{
			UE_LOG(LogMocapSmooth, Warning,
			       TEXT("[MocapSmooth] %s: 'Smooth On Top Of Previous' is on, but nothing has been ")
			       TEXT("smoothed yet -- this is just a normal first pass."),
			       *AnimationSequence->GetName());
		}
		else if (!MocapSmoothCache::ReadCurrent(AnimationSequence, Tracks, Source))
		{
			UE_LOG(LogMocapSmooth, Error,
			       TEXT("[MocapSmooth] %s: could not read the current animation. Nothing changed."),
			       *AnimationSequence->GetName());
			return;
		}
	}

	const int32 NumFrames = Source.NumFrames;
	const int32 Start = (bSkipLeadingReferencePose && HasLeadingReferencePose(Source)) ? 1 : 0;
	const int32 Order = FMath::Clamp(FilterOrder, 1, 4);

	// Filter into a copy, so a region left Off is restored from the source rather than keeping
	// whatever a previous apply left on the asset. That is what makes re-running with different
	// settings land on those settings exactly.
	SlowTask.EnterProgressFrame(1.0f, LOCTEXT("Filtering", "Filtering"));
	FMocapTracks Out = Source;

	int32 TouchedPerRegion[static_cast<int32>(EMocapRegion::Count)] = { 0 };
	double CutoffPerRegion[static_cast<int32>(EMocapRegion::Count)] = { 0.0 };
	int32 SkippedMask = 0;
	bool bAnyTouched = false;

	for (int32 b = 0; b < Tracks.Num(); ++b)
	{
		const EMocapRegion Region = Regions[b];
		float Slider = 0.0f;
		bool bFilter = false;
		switch (Region)
		{
		case EMocapRegion::Fingers: bFilter = Resolve({ Fingers, Hands, FullBody }, Slider); break;
		case EMocapRegion::Hands:   bFilter = Resolve({ Hands, UpperBody, FullBody }, Slider); break;
		case EMocapRegion::Upper:   bFilter = Resolve({ UpperBody, FullBody }, Slider); break;
		case EMocapRegion::Lower:   bFilter = Resolve({ LowerBody, FullBody }, Slider); break;
		// Rokoko's Lower Body deliberately excludes the hips, so the pelvis follows Full Body only.
		case EMocapRegion::Pelvis:  bFilter = Resolve({ FullBody }, Slider); break;
		case EMocapRegion::Root:    bFilter = bSmoothRoot && Resolve({ FullBody }, Slider); break;
		default:                    bFilter = Resolve({ FullBody }, Slider); break;
		}

		const double Cutoff = bFilter ? MocapSmoothFilter::SliderToCutoffHz(Slider) : 0.0;
		if (!bFilter || Cutoff >= SampleRate * 0.5)
		{
			SkippedMask |= 1 << static_cast<int32>(Region);
			continue;
		}

		const bool bUseGaussian =
			(bGaussianForHandsAndFingers && (Region == EMocapRegion::Hands || Region == EMocapRegion::Fingers))
			|| Shape == EMocapSmoothShape::Gaussian;
		const EMocapSmoothShapeInternal InternalShape = bUseGaussian
			? EMocapSmoothShapeInternal::Gaussian
			: EMocapSmoothShapeInternal::Butterworth;

		const int32 Count = NumFrames - Start;
		MocapSmoothFilter::SmoothQuaternions(
			TArrayView<FQuat4f>(Out.Rotations[b].GetData() + Start, Count),
			Cutoff, SampleRate, Order, InternalShape);

		const bool bSkipTranslation =
			(Region == EMocapRegion::Root || Region == EMocapRegion::Pelvis) && !bSmoothPelvisTranslation;
		if (!bSkipTranslation)
		{
			MocapSmoothFilter::SmoothPositions(
				TArrayView<FVector3f>(Out.Positions[b].GetData() + Start, Count),
				Cutoff, SampleRate, Order, InternalShape);
		}

		++TouchedPerRegion[static_cast<int32>(Region)];
		CutoffPerRegion[static_cast<int32>(Region)] = Cutoff;
		bAnyTouched = true;
	}

	if (!bAnyTouched)
	{
		UE_LOG(LogMocapSmooth, Warning,
		       TEXT("[MocapSmooth] %s: every region is set to Off, so there is nothing to do."),
		       *AnimationSequence->GetName());
		return;
	}

	SlowTask.EnterProgressFrame(1.0f, LOCTEXT("Writing", "Writing tracks back"));
	IAnimationDataController& Controller = AnimationSequence->GetController();
	{
		IAnimationDataController::FScopedBracket Bracket(
			Controller, LOCTEXT("MocapSmoothBracket", "Mocap smooth"));
		for (int32 b = 0; b < Tracks.Num(); ++b)
		{
			if (!Controller.SetBoneTrackKeys(Tracks[b], Out.Positions[b], Out.Rotations[b], Out.Scales[b]))
			{
				UE_LOG(LogMocapSmooth, Warning, TEXT("[MocapSmooth] write failed on bone %s"),
				       *Tracks[b].ToString());
			}
		}
	}

	const int32 Passes = (bSmoothOnTopOfPrevious && PriorPasses > 0) ? PriorPasses + 1 : 1;
	const FString Settings = FString::Printf(
		TEXT("full=%.2f fingers=%.2f hands=%.2f upper=%.2f lower=%.2f shape=%s order=%d root=%d pelvisT=%d"),
		FullBody, Fingers, Hands, UpperBody, LowerBody,
		Shape == EMocapSmoothShape::Gaussian ? TEXT("gaussian") : TEXT("butter"),
		Order, bSmoothRoot ? 1 : 0, bSmoothPelvisTranslation ? 1 : 0);
	MocapSmoothCache::RecordWrite(AnimationSequence, Tracks, Passes, Settings);

	TArray<FString> Bands;
	TArray<FString> Left;
	for (int32 r = 0; r < static_cast<int32>(EMocapRegion::Count); ++r)
	{
		if (TouchedPerRegion[r] > 0)
		{
			Bands.Add(FString::Printf(TEXT("%s %.2f Hz on %d bones"),
			                          MocapSmoothRegions::ToString(static_cast<EMocapRegion>(r)),
			                          CutoffPerRegion[r], TouchedPerRegion[r]));
		}
		else if (SkippedMask & (1 << r))
		{
			Left.Add(MocapSmoothRegions::ToString(static_cast<EMocapRegion>(r)));
		}
	}

	UE_LOG(LogMocapSmooth, Display,
	       TEXT("[MocapSmooth] %s: smoothed from %s. %d frames at %g fps. %s%s%s%s"),
	       *AnimationSequence->GetName(),
	       Passes > 1 ? *FString::Printf(TEXT("the animation already on the asset (pass %d)"), Passes)
	                  : TEXT("the untouched original"),
	       NumFrames, SampleRate, *FString::Join(Bands, TEXT(", ")),
	       Left.Num() > 0 ? *FString::Printf(TEXT(". Left alone: %s"), *FString::Join(Left, TEXT(", "))) : TEXT(""),
	       Start > 0 ? TEXT(". Frame 0 reference pose skipped") : TEXT(""),
	       bCaptured ? TEXT(". Original captured just now") : TEXT(""));
}

void UMocapSmoothModifier::OnRevert_Implementation(UAnimSequence* AnimationSequence)
{
	if (!AnimationSequence)
	{
		return;
	}
	if (!MocapSmoothCache::HasOriginal(AnimationSequence))
	{
		UE_LOG(LogMocapSmooth, Error,
		       TEXT("[MocapSmooth] %s: there is no original cached at %s, so there is nothing to ")
		       TEXT("revert to. Reimport the source FBX."),
		       *AnimationSequence->GetName(), *MocapSmoothCache::PayloadPath(AnimationSequence));
		return;
	}
	const IAnimationDataModel* Model = AnimationSequence->GetDataModel();
	if (!Model)
	{
		return;
	}
	TArray<FName> Tracks;
	Model->GetBoneTrackNames(Tracks);

	FMocapTracks Original;
	bool bCaptured = false;
	if (!MocapSmoothCache::LoadOrCapture(AnimationSequence, Tracks, Original, bCaptured) || bCaptured)
	{
		UE_LOG(LogMocapSmooth, Error,
		       TEXT("[MocapSmooth] %s: could not restore the original."), *AnimationSequence->GetName());
		return;
	}

	IAnimationDataController& Controller = AnimationSequence->GetController();
	{
		IAnimationDataController::FScopedBracket Bracket(
			Controller, LOCTEXT("MocapSmoothRevertBracket", "Mocap smooth revert"));
		for (int32 b = 0; b < Tracks.Num(); ++b)
		{
			Controller.SetBoneTrackKeys(Tracks[b], Original.Positions[b], Original.Rotations[b],
			                            Original.Scales[b]);
		}
	}
	MocapSmoothCache::RecordWrite(AnimationSequence, Tracks, 0, FString());

	UE_LOG(LogMocapSmooth, Display, TEXT("[MocapSmooth] %s: back to the untouched original."),
	       *AnimationSequence->GetName());
}

#undef LOCTEXT_NAMESPACE
