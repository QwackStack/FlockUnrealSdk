// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Codegen/FlockCppModuleEmitter.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

/**
 * The C++ target's skeleton is the one piece of codegen whose failure mode is "the project no longer
 * builds" rather than "one feature is missing", so it is verified before any type emitter exists.
 *
 * `EmitsACompilableModule` writes into a **stable, findable** directory rather than a GUID one, on
 * purpose: the emitted bytes are meant to be copied into a real project and compiled, and that check is
 * worth being able to run by hand. Structural assertions cannot tell you a .Build.cs is valid C#.
 */
namespace FlockCppModuleEmitterTestHelpers
{
	/** Under Saved/, so a stale run is cleaned by the usual means and never lands in source control. */
	inline FString StableRoot()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FlockCppEmitTest"), TEXT("Source"),
			TEXT("FlockGenerated"));
	}

	inline FString ScratchProjectDir()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FlockCppEmitTest"));
	}

	/**
	 * A throwaway project that looks like a C++ one: a .uproject plus a *.Target.cs, which is what the
	 * emitter actually tests for. The real project's file is never touched by a test.
	 */
	inline FString MakeScratchProject(const FString& ProjectDir)
	{
		IFileManager::Get().DeleteDirectory(*ProjectDir, /*RequireExists*/ false, /*Tree*/ true);
		IFileManager::Get().MakeDirectory(*FPaths::Combine(ProjectDir, TEXT("Source")), /*Tree*/ true);

		FFileHelper::SaveStringToFile(
			TEXT("using UnrealBuildTool;\npublic class ScratchTarget : TargetRules {}\n"),
			*FPaths::Combine(ProjectDir, TEXT("Source"), TEXT("Scratch.Target.cs")));

		const FString ProjectFile = FPaths::Combine(ProjectDir, TEXT("Scratch.uproject"));
		FFileHelper::SaveStringToFile(
			TEXT("{\n\t\"FileVersion\": 3,\n\t\"EngineAssociation\": \"5.5\",\n\t\"Modules\": [\n")
			TEXT("\t\t{\n\t\t\t\"Name\": \"Scratch\",\n\t\t\t\"Type\": \"Runtime\",\n")
			TEXT("\t\t\t\"LoadingPhase\": \"Default\"\n\t\t}\n\t]\n}\n"),
			*ProjectFile);
		return ProjectFile;
	}

	inline FFlockCodegenManifest Manifest()
	{
		FFlockCodegenManifest Result;
		Result.GameVersionId = TEXT("ver-1");
		Result.ContentHash = TEXT("abc123");
		Result.GeneratedAtUtc = TEXT("2026-07-28T00:00:00Z");
		Result.SdkVersion = TEXT("0.14.0");
		Result.PlayerTemplateCount = 4;
		Result.GameConfigCount = 5;
		Result.ShopCount = 1;
		return Result;
	}

	inline FString Read(const FString& Path)
	{
		FString Contents;
		FFileHelper::LoadFileToString(Contents, *Path);
		return Contents;
	}
}

using namespace FlockCppModuleEmitterTestHelpers;

// ── The skeleton: the three files a module needs, and the project entry that builds it ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCppModuleEmitTest, "Flock.Editor.CppModule.EmitsACompilableModule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCppModuleEmitTest::RunTest(const FString& Parameters)
{
	const FString ProjectDir = ScratchProjectDir();
	const FString ProjectFile = MakeScratchProject(ProjectDir);
	const FString Root = StableRoot();

	const FFlockCppModuleEmitter::FEmitResult Result =
		FFlockCppModuleEmitter::Emit(Root, Manifest(), ProjectFile);
	for (const FString& Warning : Result.Warnings)
	{
		AddInfo(Warning);
	}
	if (!TestTrue(FString::Printf(TEXT("emitted (%s)"), *Result.Error), Result.bSucceeded))
	{
		return false;
	}

	// The module's name is its .Build.cs basename, and UnrealBuildTool matches the two — a mismatch is a
	// module that silently never builds.
	TestEqual(TEXT("module named after its folder"), Result.ModuleName, FString(TEXT("FlockGenerated")));

	const FString BuildCs = Read(FPaths::Combine(Root, TEXT("FlockGenerated.Build.cs")));
	TestTrue(TEXT("the Build.cs declares the module class"),
		BuildCs.Contains(TEXT("public class FlockGenerated : ModuleRules"), ESearchCase::CaseSensitive));
	// Without this the generated accessors cannot call the SDK at all.
	TestTrue(TEXT("and depends on the SDK"), BuildCs.Contains(TEXT("\"Flock\""), ESearchCase::CaseSensitive));

	// A module with no IMPLEMENT_MODULE links but never loads, which presents as its types not existing.
	const FString ModuleCpp = Read(FPaths::Combine(Root, TEXT("Private"), TEXT("FlockGeneratedModule.cpp")));
	TestTrue(TEXT("the module implements itself"),
		ModuleCpp.Contains(TEXT("IMPLEMENT_MODULE(FFlockGeneratedModule, FlockGenerated)"), ESearchCase::CaseSensitive));
	// It installs the generated wire names on load. Without this a write goes out under C++ spellings.
	TestTrue(TEXT("and registers the wire names"),
		ModuleCpp.Contains(TEXT("FlockGeneratedWireNames::Register()"), ESearchCase::CaseSensitive));

	// The skeleton must stand alone: the module includes the wire-name header unconditionally, so a stub
	// has to exist before any type is generated or a half-finished sync leaves an uncompilable project.
	TestTrue(TEXT("a wire-name stub ships with the skeleton"),
		IFileManager::Get().FileExists(*FPaths::Combine(Root, TEXT("Public"), TEXT("FlockGeneratedWireNames.h"))));
	TestTrue(TEXT("including its implementation"),
		IFileManager::Get().FileExists(*FPaths::Combine(Root, TEXT("Private"), TEXT("FlockGeneratedWireNames.cpp"))));

	// The compiled half of the manifest. The JSON sidecar stays the source of truth for tooling, because
	// the editor must answer "is this stale?" before this header has ever compiled.
	const FString ManifestHeader = Read(FPaths::Combine(Root, TEXT("Public"), TEXT("FlockGeneratedManifest.h")));
	TestTrue(TEXT("the manifest header carries the version"),
		ManifestHeader.Contains(TEXT("GameVersionId = TEXT(\"ver-1\")"), ESearchCase::CaseSensitive));
	TestTrue(TEXT("and the content hash"),
		ManifestHeader.Contains(TEXT("ContentHash = TEXT(\"abc123\")"), ESearchCase::CaseSensitive));

	TestTrue(TEXT("the module was registered in the project"), Result.bProjectFilePatched);
	const FString ProjectJson = Read(ProjectFile);
	TestTrue(TEXT("the .uproject names it"),
		ProjectJson.Contains(TEXT("\"FlockGenerated\""), ESearchCase::CaseSensitive));
	// Additive: the project's own module must survive being patched.
	TestTrue(TEXT("and still names the project's own module"),
		ProjectJson.Contains(TEXT("\"Scratch\""), ESearchCase::CaseSensitive));

	return true;
}

// ── Re-syncing must not rewrite the project file or churn the sources ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCppModuleIdempotentTest, "Flock.Editor.CppModule.ReSyncIsIdempotent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCppModuleIdempotentTest::RunTest(const FString& Parameters)
{
	const FString ProjectDir = ScratchProjectDir();
	const FString ProjectFile = MakeScratchProject(ProjectDir);
	const FString Root = StableRoot();

	const FFlockCppModuleEmitter::FEmitResult First =
		FFlockCppModuleEmitter::Emit(Root, Manifest(), ProjectFile);
	if (!TestTrue(TEXT("first emit"), First.bSucceeded))
	{
		return false;
	}
	TestTrue(TEXT("the first sync registers the module"), First.bProjectFilePatched);
	const FString AfterFirst = Read(ProjectFile);

	const FFlockCppModuleEmitter::FEmitResult Second =
		FFlockCppModuleEmitter::Emit(Root, Manifest(), ProjectFile);
	TestTrue(TEXT("second emit"), Second.bSucceeded);
	// Reported as unpatched, which is what a caller uses to decide whether a rebuild is newly required.
	TestFalse(TEXT("the second does not patch again"), Second.bProjectFilePatched);
	TestEqual(TEXT("and leaves the project file byte-identical"), Read(ProjectFile), AfterFirst);

	// Unregistering is the switch-back-to-Blueprint path, and must be equally idempotent.
	bool bRemoved = false;
	FString Error;
	TestTrue(TEXT("unregister succeeds"),
		FFlockCppModuleEmitter::UnregisterModule(ProjectFile, TEXT("FlockGenerated"), bRemoved, Error));
	TestTrue(TEXT("and reports the removal"), bRemoved);
	TestFalse(TEXT("the module is gone"),
		Read(ProjectFile).Contains(TEXT("\"FlockGenerated\""), ESearchCase::CaseSensitive));

	bool bRemovedAgain = false;
	TestTrue(TEXT("unregistering twice is fine"),
		FFlockCppModuleEmitter::UnregisterModule(ProjectFile, TEXT("FlockGenerated"), bRemovedAgain, Error));
	TestFalse(TEXT("and reports nothing changed"), bRemovedAgain);

	return true;
}

// ── The two projects a C++ sync must refuse rather than half-convert ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCppModuleRefusalTest, "Flock.Editor.CppModule.RefusesBlueprintOnlyProjects",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCppModuleRefusalTest::RunTest(const FString& Parameters)
{
	const FString ProjectDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FlockCppEmitTest_BpOnly"));
	IFileManager::Get().DeleteDirectory(*ProjectDir, /*RequireExists*/ false, /*Tree*/ true);
	IFileManager::Get().MakeDirectory(*ProjectDir, /*Tree*/ true);
	const FString ProjectFile = FPaths::Combine(ProjectDir, TEXT("BpOnly.uproject"));
	FFileHelper::SaveStringToFile(TEXT("{\n\t\"FileVersion\": 3,\n\t\"EngineAssociation\": \"5.5\"\n}\n"), *ProjectFile);

	// No Source/ and no *.Target.cs. Converting this would mean generating a whole C++ skeleton and
	// requiring a compiler — a change of project kind, made silently, on someone who asked for typed data.
	const FFlockCppModuleEmitter::FEmitResult BpOnly = FFlockCppModuleEmitter::Emit(
		FPaths::Combine(ProjectDir, TEXT("Source"), TEXT("FlockGenerated")), Manifest(), ProjectFile);
	TestFalse(TEXT("a Blueprint-only project is refused"), BpOnly.bSucceeded);
	TestTrue(TEXT("and is told how to proceed"), BpOnly.Error.Contains(TEXT("Blueprint")));

	// A Source/ folder alone is not a C++ project — the generated module's own folder would create one,
	// so the check looks for a *.Target.cs instead.
	IFileManager::Get().MakeDirectory(*FPaths::Combine(ProjectDir, TEXT("Source")), /*Tree*/ true);
	TestFalse(TEXT("a bare Source folder does not count as C++"),
		FFlockCppModuleEmitter::ProjectHasCppTargets(ProjectDir));

	// Outside Source/, UnrealBuildTool never discovers the module, so it would silently never compile.
	const FString CppProjectDir = ScratchProjectDir();
	const FString CppProjectFile = MakeScratchProject(CppProjectDir);
	const FFlockCppModuleEmitter::FEmitResult Outside = FFlockCppModuleEmitter::Emit(
		FPaths::Combine(CppProjectDir, TEXT("NotSource"), TEXT("FlockGenerated")), Manifest(), CppProjectFile);
	TestFalse(TEXT("a root outside Source/ is refused"), Outside.bSucceeded);
	TestTrue(TEXT("and says why"), Outside.Error.Contains(TEXT("Source")));

	return true;
}

#endif // WITH_AUTOMATION_TESTS
