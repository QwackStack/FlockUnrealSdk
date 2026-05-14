// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Qwack_ue_Sdk : ModuleRules
{
	public Qwack_ue_Sdk(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicIncludePaths.AddRange(new string[]
		{
			ModuleDirectory + "/Public",
			ModuleDirectory + "/Public/Qwack_ue_Sdk",
			ModuleDirectory + "/Public/Qwack_ue_Sdk/Analytics",
			ModuleDirectory + "/Public/Qwack_ue_Sdk/Auth",
			ModuleDirectory + "/Public/Qwack_ue_Sdk/Cache",
			ModuleDirectory + "/Public/Qwack_ue_Sdk/Config",
			ModuleDirectory + "/Public/Qwack_ue_Sdk/Context",
			ModuleDirectory + "/Public/Qwack_ue_Sdk/Endpoints",
			ModuleDirectory + "/Public/Qwack_ue_Sdk/GameAPI",
			ModuleDirectory + "/Public/Qwack_ue_Sdk/HTTPClient",
			ModuleDirectory + "/Public/Qwack_ue_Sdk/LogEvent",
			ModuleDirectory + "/Public/Qwack_ue_Sdk/Session",
			ModuleDirectory + "/Public/Qwack_ue_Sdk/Utils",
		});

		PrivateIncludePaths.AddRange(new string[]
		{
			ModuleDirectory + "/Private",
			ModuleDirectory + "/Private/Analytics",
			ModuleDirectory + "/Private/Auth",
			ModuleDirectory + "/Private/Cache",
			ModuleDirectory + "/Private/Config",
			ModuleDirectory + "/Private/Context",
			ModuleDirectory + "/Private/Endpoints",
			ModuleDirectory + "/Private/GameAPI",
			ModuleDirectory + "/Private/HTTPClient",
			ModuleDirectory + "/Private/LogEvent",
			ModuleDirectory + "/Private/Session",
		});

		PublicDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"DeveloperSettings",
		});

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"HTTP",
			"Json",
			"JsonUtilities",
		});
	}
}
