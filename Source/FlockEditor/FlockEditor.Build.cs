// Copyright 2022, Qwacks. All Rights Reserved.

using UnrealBuildTool;

public class FlockEditor : ModuleRules
{
	public FlockEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Editor-only module: version resolve/bake, the play-mode setup guard, and the build
		// validation guard. Depends on the runtime Flock module for UFlockConfig / UFlockSubsystem
		// and the shared ApiVersion constant.
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"Flock",
				"DeveloperSettings",
			}
			);

		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"UnrealEd",       // FEditorDelegates, editor lifecycle
				"Slate",
				"SlateCore",      // toast notifications
				"ToolMenus",      // Tools > Flock menu entry
				"MessageLog",     // FMessageLog (play-mode guard)
				"AssetRegistry",  // FAssetData (validator signatures)
				"DataValidation", // UEditorValidatorBase (build/validate guard)
			}
			);
	}
}
