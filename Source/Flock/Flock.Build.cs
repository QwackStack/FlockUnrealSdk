// Copyright 2022, Qwacks. All Rights Reserved.

using UnrealBuildTool;

public class Flock : ModuleRules
{
	public Flock(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Public/Private layout: UBT auto-adds Public/ to public include paths and Private/
		// to private include paths, so no manual include-path wiring is needed.

		// Minimal foundation dependency set. HTTP/TLS/Json modules are intentionally dropped
		// with the old implementation and will be re-added by the HTTP-layer ticket (QWA-978).
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
	}
}
