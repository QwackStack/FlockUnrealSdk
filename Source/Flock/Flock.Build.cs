// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

using UnrealBuildTool;

public class Flock : ModuleRules
{
	public Flock(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		// This module is not unity-safe. Its tests put their helpers in a namespace per file and then
		// open that namespace with a file-scope `using namespace`. A unity build pastes those files into
		// one translation unit, so every one of those using-directives is in scope at once and the names
		// they share go ambiguous -- FFixture, Cleanup, Env, TempRoot, NoRetry and about fifteen more.
		// Measured on UE 5.4, 2026-09-22: 535 errors across Flock, FlockEditor and ProtokitePlaytest.
		//
		// This is easy to miss locally, which is why it survived to here: UBT's adaptive unity build
		// reads `git status` to pick its working set, so while the plugin is untracked or modified in a
		// consumer's checkout, every file compiles on its own anyway and nothing collides. It only bites
		// once the files are committed -- which is what a studio's clean checkout looks like.
		//
		// Compiling each file separately is the same stopgap the previous SDK used for the same reason.
		// The real fix is to drop the file-scope using-directives and qualify the helpers, or to give
		// each one a file-specific name; until that happens this line has to stay.
		bUseUnity = false;

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
