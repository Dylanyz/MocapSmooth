// SPDX-License-Identifier: Apache-2.0

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
