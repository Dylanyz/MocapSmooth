// Copyright (c) 2026 Dylan Gitalis. Source-available under CPAL-1.0 with the Commons Clause; see LICENSE.
// SPDX-License-Identifier: CPAL-1.0 AND LicenseRef-Commons-Clause-1.0

using UnrealBuildTool;

public class MocapSmoothEditor : ModuleRules
{
	public MocapSmoothEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"AnimationModifiers",
				// AnimationModifier.h itself includes AnimationBlueprintLibrary.h, so anything
				// that includes our public header needs it too -- hence Public, not Private.
				"AnimationBlueprintLibrary",
			}
		);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"UnrealEd",
				"Json",
			}
		);
	}
}
