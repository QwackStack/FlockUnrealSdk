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

		// Json is linked here as well as through Flock: the playtest config reads Protokite's JSON itself, and
		// a module that calls into another module's exports has to name it.
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Json",
			}
			);
	}
}
