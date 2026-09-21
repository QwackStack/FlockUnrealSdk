// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

using UnrealBuildTool;

public class FlockPlaytestEditor : ModuleRules
{
	public FlockPlaytestEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Editor only: says what in this project's settings would stop a playtest before anyone presses Play. It reads
		// the Flock SDK's settings as well as the playtest plugin's, because a playtest session starts from a Flock one.
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Flock",
				"FlockPlaytest",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"UnrealEd",      // FEditorDelegates, to speak up as Play starts
				"MessageLog",    // the Play message log, where each finding appears with its fix
				"Settings",      // ISettingsModule, which opens the settings page a fix is made on
				"DeveloperSettings",
			}
			);
	}
}
