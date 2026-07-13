// Copyright Epic Games, Inc. All Rights Reserved.

using UnrealBuildTool;

public class Qwack_ue_Sdk : ModuleRules
{
	public Qwack_ue_Sdk(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicIncludePaths.AddRange(
			new string[] {
				// ... add public include paths required here ...
			}
			);

		PrivateIncludePaths.AddRange(
			new string[] {
				// ... add other private include paths required here ...
			}
			);

		// Minimal foundation dependency set. HTTP/TLS/Json modules are intentionally
		// dropped with the old implementation and will be re-added by the HTTP-layer
		// ticket (QWA-978) when that layer is rebuilt.
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"DeveloperSettings",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Projects",
			}
			);

		DynamicallyLoadedModuleNames.AddRange(
			new string[]
			{
				// ... add any modules that your module loads dynamically here ...
			}
			);
	}
}
