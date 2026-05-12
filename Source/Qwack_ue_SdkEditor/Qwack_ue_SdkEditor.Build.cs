// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Qwack_ue_SdkEditor : ModuleRules
{
	public Qwack_ue_SdkEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicIncludePaths.AddRange(new string[]
		{
			ModuleDirectory + "/Public",
			ModuleDirectory + "/Public/Qwack_ue_SdkEditor",
		});

		PrivateIncludePaths.AddRange(new string[]
		{
			ModuleDirectory + "/Private",
		});

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"CoreUObject",
			"Engine",
			"Slate",
			"SlateCore",
			"UnrealEd",
			"InputCore",
			"PropertyEditor",
			"EditorStyle",
			"Projects",
			"Settings",
			"DeveloperSettings",
			"HTTP",
			"Json",
			"Qwack_ue_Sdk",
		});
	}
}
