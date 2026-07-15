// Copyright 2022, Qwacks. All Rights Reserved.

using UnrealBuildTool;

public class Flock : ModuleRules
{
	public Flock(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// Public/Private layout: UBT auto-adds Public/ to public include paths and Private/
		// to private include paths, so no manual include-path wiring is needed.

		// Json/JsonUtilities are Public: response deserialization happens at the templated call site
		// (FFlockHttpClient::Get<T>), so any module that includes the client header needs them.
		PublicDependencyModuleNames.AddRange(
			new string[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"DeveloperSettings",
				"Json",
				"JsonUtilities",
			}
			);

		// HTTP stays Private: only FFlockHttpModuleAdapter.cpp touches the engine HTTP module, and it
		// is hidden behind FFlockHttpClient::CreateDefault + the IFlockHttpAdapter seam, so no public
		// header leaks an engine-HTTP type and dependent modules (FlockEditor) don't need HTTP.
		PrivateDependencyModuleNames.AddRange(
			new string[]
			{
				"Projects",
				"HTTP",
			}
			);
	}
}
