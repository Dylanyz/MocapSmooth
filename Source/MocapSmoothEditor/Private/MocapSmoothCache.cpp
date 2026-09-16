// SPDX-License-Identifier: Apache-2.0

#include "MocapSmoothCache.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Dom/JsonObject.h"
#include "EditorFramework/AssetImportData.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogMocapSmooth, Log, All);

namespace
{
	constexpr uint32 kPayloadMagic = 0x304D534D;   // "MSM0"
	constexpr uint32 kPayloadVersion = 1;
	constexpr int32 kProbeFrames = 64;

	FString BasePath(const UAnimSequence* Anim)
	{
		FString Safe = Anim->GetPathName();
		int32 Dot;
		if (Safe.FindChar(TEXT('.'), Dot))
		{
			Safe.LeftInline(Dot);
		}
		Safe.RemoveFromStart(TEXT("/"));
		for (TCHAR& C : Safe.GetCharArray())
		{
			if (C != 0 && !FChar::IsAlnum(C) && C != TEXT('_') && C != TEXT('-') && C != TEXT('.'))
			{
				C = TEXT('_');
			}
		}
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("MocapSmooth"), Safe);
	}
}

namespace MocapSmoothCache
{

FString PayloadPath(const UAnimSequence* Anim) { return BasePath(Anim) + TEXT(".mocapraw"); }
FString MetaPath(const UAnimSequence* Anim)    { return BasePath(Anim) + TEXT(".json"); }

bool HasOriginal(const UAnimSequence* Anim)
{
	return IFileManager::Get().FileExists(*PayloadPath(Anim));
}

FString SourceFingerprint(const UAnimSequence* Anim)
{
	const UAssetImportData* ImportData = Anim->AssetImportData;
	if (!ImportData)
	{
		return FString();
	}
	TArray<FString> Files;
	ImportData->ExtractFilenames(Files);
	if (Files.Num() == 0)
	{
		return FString();
	}
	const FString& File = Files[0];
	const FFileStatData Stat = IFileManager::Get().GetStatData(*File);
	if (!Stat.bIsValid)
	{
		// The FBX has moved, or this is a different machine. Fall back to the path alone rather
		// than declaring the original stale and capturing over it.
		return FString::Printf(TEXT("path:%s"), *File);
	}
	return FString::Printf(TEXT("%s|%lld|%lld"), *File, Stat.FileSize,
	                       Stat.ModificationTime.ToUnixTimestamp());
}

bool LoadMeta(const UAnimSequence* Anim, FMocapSmoothMeta& OutMeta)
{
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *MetaPath(Anim)))
	{
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
	if (!FJsonSerializer::Deserialize(Reader, Root) || !Root.IsValid())
	{
		return false;
	}
	OutMeta.Source = Root->GetStringField(TEXT("source"));
	OutMeta.Captured = Root->GetStringField(TEXT("captured"));
	OutMeta.LastWritten = Root->GetStringField(TEXT("lastWritten"));
	OutMeta.LastSettings = Root->GetStringField(TEXT("lastSettings"));
	OutMeta.Frames = Root->GetIntegerField(TEXT("frames"));
	OutMeta.Bones = Root->GetIntegerField(TEXT("bones"));
	OutMeta.Passes = Root->GetIntegerField(TEXT("passes"));
	double ProbeValue = 0.0;
	OutMeta.bHasProbe = Root->TryGetNumberField(TEXT("lastWriteProbe"), ProbeValue);
	OutMeta.LastWriteProbe = static_cast<uint32>(ProbeValue);
	return true;
}

bool SaveMeta(const UAnimSequence* Anim, const FMocapSmoothMeta& Meta)
{
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("source"), Meta.Source);
	Root->SetStringField(TEXT("captured"), Meta.Captured);
	Root->SetStringField(TEXT("lastWritten"), Meta.LastWritten);
	Root->SetStringField(TEXT("lastSettings"), Meta.LastSettings);
	Root->SetNumberField(TEXT("frames"), Meta.Frames);
	Root->SetNumberField(TEXT("bones"), Meta.Bones);
	Root->SetNumberField(TEXT("passes"), Meta.Passes);
	if (Meta.bHasProbe)
	{
		Root->SetNumberField(TEXT("lastWriteProbe"), Meta.LastWriteProbe);
	}

	FString Text;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Text);
	FJsonSerializer::Serialize(Root, Writer);
	return FFileHelper::SaveStringToFile(Text, *MetaPath(Anim));
}

bool ReadCurrent(const UAnimSequence* Anim, const TArray<FName>& Tracks, FMocapTracks& Out)
{
	const IAnimationDataModel* Model = Anim->GetDataModel();
	if (!Model)
	{
		return false;
	}
	if (Model->GetNumberOfFrames() <= 0 || Tracks.Num() == 0)
	{
		return false;
	}

	Out.BoneNames = Tracks;
	Out.Positions.SetNum(Tracks.Num());
	Out.Rotations.SetNum(Tracks.Num());
	Out.Scales.SetNum(Tracks.Num());

	// The sample count comes from the data itself, not from GetNumberOfFrames() vs
	// GetNumberOfKeys() -- those differ by one (frames are intervals, keys are samples) and
	// picking the wrong one silently truncates every track by a frame. Whatever the model hands
	// back for the first track is the length we read, filter and write back, so the round-trip is
	// self-consistent by construction.
	int32 NumFrames = 0;

	// One call per BONE, not per frame. This is the API the Python route could not reach at all
	// (GetBoneTrackTransforms carries no UFUNCTION), which is why the Python version had to
	// evaluate the whole pose per frame -- and Epic's helper for that re-evaluated the entire
	// skeleton once per bone, which is where the old 8 ms/frame came from.
	TArray<FTransform> Transforms;
	for (int32 b = 0; b < Tracks.Num(); ++b)
	{
		Transforms.Reset();
		Model->GetBoneTrackTransforms(Tracks[b], Transforms);
		if (b == 0)
		{
			NumFrames = Transforms.Num();
			Out.NumFrames = NumFrames;
			if (NumFrames <= 0)
			{
				UE_LOG(LogMocapSmooth, Warning, TEXT("[MocapSmooth] %s: track %s has no keys"),
				       *Anim->GetName(), *Tracks[b].ToString());
				return false;
			}
		}
		else if (Transforms.Num() != NumFrames)
		{
			// Tracks of differing length are not something to guess about.
			UE_LOG(LogMocapSmooth, Warning,
			       TEXT("[MocapSmooth] %s: track %s has %d keys but %s has %d, skipping read"),
			       *Anim->GetName(), *Tracks[b].ToString(), Transforms.Num(),
			       *Tracks[0].ToString(), NumFrames);
			return false;
		}
		TArray<FVector3f>& P = Out.Positions[b];
		TArray<FQuat4f>& R = Out.Rotations[b];
		TArray<FVector3f>& S = Out.Scales[b];
		P.SetNumUninitialized(NumFrames);
		R.SetNumUninitialized(NumFrames);
		S.SetNumUninitialized(NumFrames);
		for (int32 f = 0; f < NumFrames; ++f)
		{
			P[f] = FVector3f(Transforms[f].GetTranslation());
			R[f] = FQuat4f(Transforms[f].GetRotation());
			S[f] = FVector3f(Transforms[f].GetScale3D());
		}
	}
	return true;
}

uint32 Probe(const UAnimSequence* Anim, const TArray<FName>& Tracks)
{
	const IAnimationDataModel* Model = Anim->GetDataModel();
	if (!Model)
	{
		return 0;
	}
	const int32 NumFrames = Model->GetNumberOfFrames();
	if (NumFrames <= 0 || Tracks.Num() == 0)
	{
		return 0;
	}

	TArray<FFrameNumber> Frames;
	const int32 Count = FMath::Min(kProbeFrames, NumFrames);
	Frames.Reserve(Count);
	for (int32 i = 0; i < Count; ++i)
	{
		const int32 F = (Count == 1) ? 0 : FMath::RoundToInt(double(i) * (NumFrames - 1) / (Count - 1));
		Frames.Add(FFrameNumber(F));
	}

	uint32 Hash = 2166136261u;
	TArray<FTransform> Transforms;
	for (const FName& Track : Tracks)
	{
		Transforms.Reset();
		Model->GetBoneTrackTransforms(Track, Frames, Transforms);
		for (const FTransform& T : Transforms)
		{
			const FQuat Q = T.GetRotation();
			// Quantised before hashing: the asset quantises keys, so an exact bit compare
			// would be noise rather than signal.
			const int64 Comps[4] = {
				static_cast<int64>(FMath::RoundToDouble(Q.X * 100000.0)),
				static_cast<int64>(FMath::RoundToDouble(Q.Y * 100000.0)),
				static_cast<int64>(FMath::RoundToDouble(Q.Z * 100000.0)),
				static_cast<int64>(FMath::RoundToDouble(Q.W * 100000.0)),
			};
			for (int64 C : Comps)
			{
				Hash = HashCombine(Hash, GetTypeHash(C));
			}
		}
	}
	// 0 is the "no probe" sentinel, so never return it for real data.
	return Hash == 0 ? 1u : Hash;
}

namespace
{
	bool SavePayload(const UAnimSequence* Anim, const FMocapTracks& Tracks)
	{
		const FString Path = MocapSmoothCache::PayloadPath(Anim);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), /*Tree=*/true);
		TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileWriter(*Path));
		if (!Ar)
		{
			UE_LOG(LogMocapSmooth, Error, TEXT("[MocapSmooth] could not write %s"), *Path);
			return false;
		}
		uint32 Magic = kPayloadMagic, Version = kPayloadVersion;
		int32 NumBones = Tracks.NumBones(), NumFrames = Tracks.NumFrames;
		*Ar << Magic << Version << NumBones << NumFrames;
		for (int32 b = 0; b < NumBones; ++b)
		{
			FString Name = Tracks.BoneNames[b].ToString();
			*Ar << Name;
		}
		for (int32 b = 0; b < NumBones; ++b)
		{
			Ar->Serialize(const_cast<FVector3f*>(Tracks.Positions[b].GetData()), sizeof(FVector3f) * NumFrames);
			Ar->Serialize(const_cast<FQuat4f*>(Tracks.Rotations[b].GetData()), sizeof(FQuat4f) * NumFrames);
			Ar->Serialize(const_cast<FVector3f*>(Tracks.Scales[b].GetData()), sizeof(FVector3f) * NumFrames);
		}
		return !Ar->IsError();
	}

	bool LoadPayload(const UAnimSequence* Anim, FMocapTracks& Out)
	{
		const FString Path = MocapSmoothCache::PayloadPath(Anim);
		TUniquePtr<FArchive> Ar(IFileManager::Get().CreateFileReader(*Path));
		if (!Ar)
		{
			return false;
		}
		uint32 Magic = 0, Version = 0;
		int32 NumBones = 0, NumFrames = 0;
		*Ar << Magic << Version << NumBones << NumFrames;
		if (Magic != kPayloadMagic || Version != kPayloadVersion || NumBones <= 0 || NumFrames <= 0)
		{
			UE_LOG(LogMocapSmooth, Warning, TEXT("[MocapSmooth] %s is not a readable original"), *Path);
			return false;
		}
		Out.BoneNames.SetNumUninitialized(NumBones);
		for (int32 b = 0; b < NumBones; ++b)
		{
			FString Name;
			*Ar << Name;
			Out.BoneNames[b] = FName(*Name);
		}
		Out.NumFrames = NumFrames;
		Out.Positions.SetNum(NumBones);
		Out.Rotations.SetNum(NumBones);
		Out.Scales.SetNum(NumBones);
		for (int32 b = 0; b < NumBones; ++b)
		{
			Out.Positions[b].SetNumUninitialized(NumFrames);
			Out.Rotations[b].SetNumUninitialized(NumFrames);
			Out.Scales[b].SetNumUninitialized(NumFrames);
			Ar->Serialize(Out.Positions[b].GetData(), sizeof(FVector3f) * NumFrames);
			Ar->Serialize(Out.Rotations[b].GetData(), sizeof(FQuat4f) * NumFrames);
			Ar->Serialize(Out.Scales[b].GetData(), sizeof(FVector3f) * NumFrames);
		}
		return !Ar->IsError();
	}

	void BackupPayload(const UAnimSequence* Anim)
	{
		const FString Path = MocapSmoothCache::PayloadPath(Anim);
		const FString Bak = Path + TEXT(".bak");
		IFileManager& FM = IFileManager::Get();
		FM.Delete(*Bak, /*RequireExists=*/false, /*EvenReadOnly=*/true, /*Quiet=*/true);
		if (FM.Move(*Bak, *Path, /*Replace=*/true, /*EvenIfReadOnly=*/true))
		{
			UE_LOG(LogMocapSmooth, Warning, TEXT("[MocapSmooth] previous original kept at %s"), *Bak);
		}
	}

	/** True if the asset still holds OUR last write, so capturing now would bake it in. */
	bool RefuseBecauseOurs(const UAnimSequence* Anim, const TArray<FName>& Tracks,
	                       const FMocapSmoothMeta& Meta, const TCHAR* Context)
	{
		if (!Meta.bHasProbe)
		{
			return false;
		}
		if (MocapSmoothCache::Probe(Anim, Tracks) != Meta.LastWriteProbe)
		{
			return false;
		}
		UE_LOG(LogMocapSmooth, Error,
		       TEXT("[MocapSmooth] %s: refusing to take a new original -- %s, but the animation on ")
		       TEXT("the asset is still the smoothing applied on %s. Capturing now would make that ")
		       TEXT("smoothed result the new 'original' and the raw would be gone. Reimport the FBX ")
		       TEXT("(which resets the asset to raw), then smooth again."),
		       *Anim->GetName(), Context,
		       Meta.LastWritten.IsEmpty() ? TEXT("an earlier run") : *Meta.LastWritten);
		return true;
	}

	bool Capture(const UAnimSequence* Anim, const TArray<FName>& Tracks, FMocapTracks& Out,
	             const TCHAR* Reason)
	{
		UE_LOG(LogMocapSmooth, Display,
		       TEXT("[MocapSmooth] capturing the untouched original for %s -- %s"),
		       *Anim->GetName(), Reason);
		const double Start = FPlatformTime::Seconds();
		if (!MocapSmoothCache::ReadCurrent(Anim, Tracks, Out))
		{
			return false;
		}
		if (!SavePayload(Anim, Out))
		{
			return false;
		}
		FMocapSmoothMeta Meta;
		MocapSmoothCache::LoadMeta(Anim, Meta);
		Meta.Source = MocapSmoothCache::SourceFingerprint(Anim);
		Meta.Captured = FDateTime::Now().ToString();
		Meta.Frames = Out.NumFrames;
		Meta.Bones = Out.NumBones();
		Meta.Passes = 0;
		Meta.bHasProbe = false;
		Meta.LastWriteProbe = 0;
		Meta.LastSettings.Reset();
		MocapSmoothCache::SaveMeta(Anim, Meta);
		UE_LOG(LogMocapSmooth, Display, TEXT("[MocapSmooth] captured %d frames x %d bones in %.1f s"),
		       Out.NumFrames, Out.NumBones(), FPlatformTime::Seconds() - Start);
		return true;
	}
}

bool LoadOrCapture(const UAnimSequence* Anim, const TArray<FName>& Tracks, FMocapTracks& Out,
                   bool& bOutCaptured)
{
	bOutCaptured = false;
	const IAnimationDataModel* Model = Anim->GetDataModel();
	if (!Model)
	{
		return false;
	}
	// GetNumberOfKeys, not GetNumberOfFrames: keys are samples, frames are the intervals between
	// them, and GetBoneTrackTransforms hands back keys.
	const int32 NumKeys = Model->GetNumberOfKeys();

	FMocapSmoothMeta Meta;
	const bool bHasMeta = LoadMeta(Anim, Meta);

	FMocapTracks Cached;
	const bool bCachedOk = LoadPayload(Anim, Cached) && Cached.IsValidFor(Tracks, NumKeys);
	if (HasOriginal(Anim) && !bCachedOk)
	{
		UE_LOG(LogMocapSmooth, Warning,
		       TEXT("[MocapSmooth] the cached original for %s no longer matches the asset ")
		       TEXT("(bone list or length changed)"), *Anim->GetName());
	}

	if (!bCachedOk)
	{
		// A missing or mismatched original would otherwise be captured from whatever is on the
		// asset right now -- including a smoothed result, if this is a re-run after the cache was
		// lost. The same guard as the reimport path applies.
		if (RefuseBecauseOurs(Anim, Tracks, Meta,
		                      TEXT("there is no usable original cached for this take")))
		{
			return false;
		}
		if (HasOriginal(Anim))
		{
			BackupPayload(Anim);
		}
		bOutCaptured = Capture(Anim, Tracks, Out, TEXT("first time this take has been smoothed"));
		return bOutCaptured;
	}

	const FString Current = SourceFingerprint(Anim);
	if (bHasMeta && !Meta.Source.IsEmpty() && !Current.IsEmpty() && Meta.Source != Current)
	{
		// The FBX behind this asset changed, so the asset holds new raw data. This is the one and
		// only situation in which an existing original is replaced.
		if (RefuseBecauseOurs(Anim, Tracks, Meta, TEXT("the source FBX looks different")))
		{
			Out = MoveTemp(Cached);
			return true;
		}
		BackupPayload(Anim);
		bOutCaptured = Capture(Anim, Tracks, Out,
		                       TEXT("the source FBX changed, so this take was reimported"));
		return bOutCaptured;
	}

	Out = MoveTemp(Cached);
	return true;
}

void RecordWrite(const UAnimSequence* Anim, const TArray<FName>& Tracks, int32 Passes,
                 const FString& Settings)
{
	FMocapSmoothMeta Meta;
	LoadMeta(Anim, Meta);
	Meta.LastWriteProbe = Probe(Anim, Tracks);
	Meta.bHasProbe = Meta.LastWriteProbe != 0;
	Meta.LastWritten = FDateTime::Now().ToString();
	Meta.Passes = Passes;
	Meta.LastSettings = Settings;
	if (Meta.Source.IsEmpty())
	{
		Meta.Source = SourceFingerprint(Anim);
	}
	SaveMeta(Anim, Meta);
}

bool ForgetOriginal(const UAnimSequence* Anim)
{
	const IAnimationDataModel* Model = Anim->GetDataModel();
	if (!Model)
	{
		return false;
	}
	TArray<FName> Tracks;
	Model->GetBoneTrackNames(Tracks);

	FMocapSmoothMeta Meta;
	LoadMeta(Anim, Meta);
	if (RefuseBecauseOurs(Anim, Tracks, Meta, TEXT("you asked to forget the original")))
	{
		return false;
	}
	if (HasOriginal(Anim))
	{
		BackupPayload(Anim);
	}
	Meta.bHasProbe = false;
	Meta.LastWriteProbe = 0;
	SaveMeta(Anim, Meta);
	UE_LOG(LogMocapSmooth, Warning,
	       TEXT("[MocapSmooth] %s: original discarded. The next smooth captures a new one."),
	       *Anim->GetName());
	return true;
}

} // namespace MocapSmoothCache
