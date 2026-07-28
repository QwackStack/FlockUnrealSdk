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

		// The editor tests reuse the runtime module's automation fakes (FFlockFakeTransport), so the
		// schema fetcher can be driven end to end without a backend. Test-support headers only — nothing
		// here includes runtime private implementation.
		PrivateIncludePaths.Add(System.IO.Path.Combine(ModuleDirectory, "..", "Flock", "Private"));

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
				// FlockHttpVersionLookup.cpp instantiates FFlockHttpClient::Get<T>, which pulls the
				// Json/JsonUtilities template code into THIS module's objects — so they must be linked
				// here directly, not just inherited transitively through Flock.
				"Json",
				"JsonUtilities",
				// The CI commandlet has no engine loop, so it ticks FHttpManager itself — otherwise a
				// request issued inside it never completes.
				"HTTP",
				// FEdGraphPinType / UEdGraphSchema_K2 — the pin types a generated Blueprint struct's
				// members are declared with (codegen struct spike).
				"BlueprintGraph",
				// FProjectDescriptor — the C++ codegen target registers its generated module in the
				// .uproject, and the engine's own descriptor keeps that file canonical.
				"Projects",
			}
			);
	}
}
