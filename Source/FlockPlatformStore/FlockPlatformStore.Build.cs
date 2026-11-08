// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

using UnrealBuildTool;

public class FlockPlatformStore : ModuleRules
{
	public FlockPlatformStore(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Not unity-safe, for the reason spelled out in Flock.Build.cs: a test file opens a helper namespace with a file-scope
		// `using namespace`. Remove this only together with the other modules'.
		bUseUnity = false;

		// Flock's own module never names this one: the dependency only points this way, so the core needs none of the online
		// subsystem's headers, and the module hands Flock its store through UFlockStore::SetStoreLibrary (which a game with its
		// own store code may call instead).
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Flock",
			}
			);

		// The engine's online subsystem reaches the platform's store (IOnlinePurchase, IOnlineStoreV2). The Google Play and iOS
		// subsystems are the engine's own plugins, on by default on their platform; this module never depends on them.
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Json",
				"OnlineSubsystem",
			}
			);
	}
}
