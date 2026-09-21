// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

using System;
using EpicGames.Core;
using Microsoft.Extensions.Logging;
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
				// FKey and EKeys live here, and the feedback form's key is a setting, so a public header names them.
				"InputCore",
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
				// IPluginManager, which finds the plugin's folder and the feedback form's icon in it.
				"Projects",
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

		// The feedback form draws this icon from disk, so a packaged game has to carry the file.
		RuntimeDependencies.Add("$(PluginDir)/Resources/FeedbackFormIcon.png", StagedFileType.UFS);

		WarnAboutAPlaytestInAShippingBuild(Target);
	}

	/**
	 * Playtest builds are Development builds. A Shipping build compiles logging out, so the Flock SDK cannot report
	 * its error lines or failed ensures, and a release build carrying a playtest's version files its players' data
	 * under that playtest. Said while the game builds, because a build is the one step everyone packaging a Shipping
	 * game goes through, and a warning in the game's own log would be compiled out of it. It never stops the build.
	 *
	 * Only a build that reads these rules says it: a clean build, or the first after a build file changes. An unchanged
	 * rebuild reuses UBT's makefile and never runs this (measured 2026-09-18: two Shipping builds in a row, two warnings
	 * then none), so a release should be packaged from a clean build.
	 */
	private void WarnAboutAPlaytestInAShippingBuild(ReadOnlyTargetRules Target)
	{
		if (Target.Configuration != UnrealTargetConfiguration.Shipping || Target.Type == TargetType.Editor || Target.ProjectFile == null)
		{
			return;
		}

		ConfigHierarchy GameConfig = ConfigCache.ReadHierarchy(ConfigHierarchyType.Game, DirectoryReference.FromFile(Target.ProjectFile), Target.Platform);

		bool bPlaytestingEnabled;
		if (GameConfig.GetBool("/Script/FlockPlaytest.FlockPlaytestSettings", "bPlaytestingEnabled", out bPlaytestingEnabled) && bPlaytestingEnabled)
		{
			Logger.LogWarning("Flock Playtest: Enable Playtesting is on in a Shipping build of {0}. Playtest builds should be Development builds: Shipping compiles logging out, so the Flock SDK cannot report error lines or failed ensures from it. Build a Development game for a playtest, or turn Enable Playtesting off in Project Settings > Plugins > Flock Playtest Settings for a release.",
				Target.Name);
		}

		string GameVersion;
		if (GameConfig.GetString("/Script/Flock.FlockConfig", "GameVersion", out GameVersion) && GameVersion != null
			&& GameVersion.StartsWith("pt-", StringComparison.Ordinal))
		{
			Logger.LogWarning("Flock Playtest: this Shipping build of {0} carries a playtest's Game Version ({1}), so everything its players do is filed under that playtest. Point Game Version in Project Settings > Plugins > Flock SDK Settings at a release version before shipping it.",
				Target.Name, GameVersion);
		}
	}
}
