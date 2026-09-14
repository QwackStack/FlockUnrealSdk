// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

using UnrealBuildTool;

public class FlockPlaytest : ModuleRules
{
	public FlockPlaytest(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Flock is Public: the subsystem's public header takes a UFlockSubsystem, so a module that includes
		// it needs Flock's headers too. The dependency only ever points this way. Flock never names this
		// module, because a project can install Flock without it.
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"DeveloperSettings",
				"Flock",
			}
			);
	}
}
