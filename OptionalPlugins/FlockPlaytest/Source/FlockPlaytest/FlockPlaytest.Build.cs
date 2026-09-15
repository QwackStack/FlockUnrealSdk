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
		// OnlineSubsystem reads the Steam id from a Steam subsystem that is already running. The plugin never depends
		// on the Steam plugin itself, so a build without Steam needs nothing more.
		// MovieSceneCapture holds the engine's frame grabber, which reads the game viewport back from the GPU. RenderCore,
		// Slate and SlateCore are what reaching that viewport and waiting on the render thread take.
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Json",
				"OnlineSubsystem",
				"MovieSceneCapture",
				"RenderCore",
				"Slate",
				"SlateCore",
			}
			);

		// Video is encoded with the engine's own libvpx, whose installed engines ship a library for 64-bit Windows only
		// (UE 5.5 to 5.8). Every other platform builds without it and records no video; everything else still runs.
		bool bBuildWithVideo = Target.Platform == UnrealTargetPlatform.Win64 && Target.Architecture == UnrealArch.X64;
		if (bBuildWithVideo)
		{
			PrivateDependencyModuleNames.Add("LibVpx");
		}
		PrivateDefinitions.Add("WITH_FLOCK_PLAYTEST_VIDEO=" + (bBuildWithVideo ? "1" : "0"));
	}
}
