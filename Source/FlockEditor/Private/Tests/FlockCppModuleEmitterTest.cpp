// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Codegen/FlockCatalogBuilder.h"
#include "Codegen/FlockCodegenRunner.h"
#include "Codegen/FlockCppModuleEmitter.h"
#include "Codegen/FlockCppTypeEmitter.h"
#include "Codegen/FlockGeneratedFiles.h"
#include "Codegen/FlockStructEmitter.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/FlockEngineCompat.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/Support/FlockCodegenTestContent.h"

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
using namespace FlockCodegenTestContent;

// ── The skeleton: the three files a module needs, and the project entry that builds it ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCppModuleEmitTest, "Flock.Editor.CppModule.EmitsACompilableModule",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCppModuleEmitTest::RunTest(const FString& Parameters)
{
	const FString ProjectDir = ScratchProjectDir();
	const FString ProjectFile = MakeScratchCppProject(ProjectDir);
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
	const FString ProjectFile = MakeScratchCppProject(ProjectDir);
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
	const FString CppProjectFile = MakeScratchCppProject(CppProjectDir);
	const FFlockCppModuleEmitter::FEmitResult Outside = FFlockCppModuleEmitter::Emit(
		FPaths::Combine(CppProjectDir, TEXT("NotSource"), TEXT("FlockGenerated")), Manifest(), CppProjectFile);
	TestFalse(TEXT("a root outside Source/ is refused"), Outside.bSucceeded);
	TestTrue(TEXT("and says why"), Outside.Error.Contains(TEXT("Source")));

	return true;
}

// ── A project with no generated module gets nothing from a Blueprint sync or a Clean ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCppModuleNoSkeletonTest, "Flock.Editor.CppModule.RemovingTypesWithNoModuleWritesNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCppModuleNoSkeletonTest::RunTest(const FString& Parameters)
{
	const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FlockCppNoModuleTest"), TEXT("FlockGenerated"));
	IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Root), /*RequireExists*/ false, /*Tree*/ true);

	FString Error;
	TestTrue(TEXT("nothing to remove is a success"), FFlockCppModuleEmitter::RemoveGeneratedTypes(Root, TEXT("FlockGenerated"),
		/*bResetManifest*/ true, Error));
	TestFalse(TEXT("and no folder or stub was written"), IFileManager::Get().DirectoryExists(*Root));
	IFileManager::Get().DeleteDirectory(*FPaths::GetPath(Root), /*RequireExists*/ false, /*Tree*/ true);
	return true;
}

namespace FlockCppModuleEmitterTestHelpers
{
	inline bool Plant(const FString& Path, const FString& Contents)
	{
		return FFileHelper::SaveStringToFile(Contents, *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}

	inline bool Exists(const FString& Path)
	{
		return IFileManager::Get().FileExists(*Path);
	}
}

// ── A generated file is known by the banner it opens with, never by its name ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockGeneratedFilesBannerTest, "Flock.Editor.GeneratedFiles.KnowsAGeneratedFileByItsBanner",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockGeneratedFilesBannerTest::RunTest(const FString& Parameters)
{
	const FString Dir = FPaths::Combine(FPaths::ProjectSavedDir(),
		FString::Printf(TEXT("FlockGeneratedFilesTest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*Dir, /*RequireExists*/ false, /*Tree*/ true); };
	auto At = [&Dir](const TCHAR* Name) { return FPaths::Combine(Dir, Name); };

	const FString Banner = FFlockGeneratedFiles::Banner;
	Plant(At(TEXT("A.h")), Banner + TEXT("#pragma once\n"));
	FFileHelper::SaveStringToFile(Banner, *At(TEXT("Bom.h")), FFileHelper::EEncodingOptions::ForceUTF8);
	Plant(At(TEXT("Crlf.h")), Banner.Replace(TEXT("\n"), TEXT("\r\n")));
	Plant(At(TEXT("C.h")), TEXT("// <auto-generated>\n// by another tool\n"));
	Plant(At(TEXT("D.h")), FString());
	Plant(At(TEXT("Late.h")), TEXT("#pragma once\n\n\n// Generated by Flock. Do not edit by hand\n"));

	TestTrue(TEXT("a file that opens with the banner is generated"), FFlockGeneratedFiles::IsGenerated(At(TEXT("A.h"))));
	TestTrue(TEXT("a byte order mark does not hide the banner"), FFlockGeneratedFiles::IsGenerated(At(TEXT("Bom.h"))));
	TestTrue(TEXT("nor do Windows line endings"), FFlockGeneratedFiles::IsGenerated(At(TEXT("Crlf.h"))));
	TestFalse(TEXT("another generator's file is not"), FFlockGeneratedFiles::IsGenerated(At(TEXT("C.h"))));
	TestFalse(TEXT("an empty file is not"), FFlockGeneratedFiles::IsGenerated(At(TEXT("D.h"))));
	TestFalse(TEXT("the words further down are not the banner"), FFlockGeneratedFiles::IsGenerated(At(TEXT("Late.h"))));
	TestFalse(TEXT("a missing file is not"), FFlockGeneratedFiles::IsGenerated(At(TEXT("Missing.h"))));

	TestFalse(TEXT("a file codegen did not write is not deleted"), FFlockGeneratedFiles::DeleteIfGenerated(At(TEXT("C.h"))));
	TestTrue(TEXT("and is still there"), Exists(At(TEXT("C.h"))));
	TestTrue(TEXT("a generated one is"), FFlockGeneratedFiles::DeleteIfGenerated(At(TEXT("A.h"))));
	TestFalse(TEXT("and is gone"), Exists(At(TEXT("A.h"))));

	FString Error;
	TestFalse(TEXT("a file codegen did not write is not overwritten"),
		FFlockGeneratedFiles::WriteIfChanged(At(TEXT("C.h")), Banner, Error));
	TestTrue(FString::Printf(TEXT("the refusal names the file and the fix (%s)"), *Error),
		Error.Contains(TEXT("C.h")) && Error.Contains(TEXT("Source/FlockGenerated")));
	FString Kept;
	FFileHelper::LoadFileToString(Kept, *At(TEXT("C.h")));
	TestEqual(TEXT("and it is unchanged"), Kept, FString(TEXT("// <auto-generated>\n// by another tool\n")));
	TestTrue(TEXT("a generated file is rewritten"), FFlockGeneratedFiles::WriteIfChanged(At(TEXT("Crlf.h")), Banner, Error));
	TestTrue(TEXT("and a new one is written"), FFlockGeneratedFiles::WriteIfChanged(At(TEXT("New.h")), Banner, Error));
	return true;
}

// ── Removing the generated types leaves the game's own files in the module's folders ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCppModuleOwnFilesTest, "Flock.Editor.CppModule.RemovesOnlyFilesCodegenWrote",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCppModuleOwnFilesTest::RunTest(const FString& Parameters)
{
	const FString ProjectDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FlockCppOwnFilesTest"));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*ProjectDir, /*RequireExists*/ false, /*Tree*/ true); };
	const FString ProjectFile = MakeScratchCppProject(ProjectDir);
	const FString Root = FPaths::Combine(ProjectDir, TEXT("Source"), TEXT("FlockGenerated"));
	if (!TestTrue(TEXT("the module was emitted"), FFlockCppModuleEmitter::Emit(Root, Manifest(), ProjectFile).bSucceeded))
	{
		return false;
	}

	// A type and an accessor file a sync wrote, and the game's own files beside them.
	const FString OldType = FPaths::Combine(Root, TEXT("Public"), TEXT("OldTypeTemplate.h"));
	const FString OldAccessors = FPaths::Combine(Root, TEXT("Private"), TEXT("FlockGeneratedOldAccessors.cpp"));
	const FString GameHeader = FPaths::Combine(Root, TEXT("Public"), TEXT("MyHelpers.h"));
	const FString GameSource = FPaths::Combine(Root, TEXT("Private"), TEXT("MyHelpers.cpp"));
	Plant(OldType, FString(FFlockGeneratedFiles::Banner) + TEXT("#pragma once\n"));
	Plant(OldAccessors, FString(FFlockGeneratedFiles::Banner) + TEXT("// accessors\n"));
	Plant(GameHeader, TEXT("#pragma once\n// the game's\n"));
	Plant(GameSource, TEXT("// the game's\n"));

	FString Error;
	TestTrue(TEXT("the generated types are removed"),
		FFlockCppModuleEmitter::RemoveGeneratedTypes(Root, TEXT("FlockGenerated"), /*bResetManifest*/ true, Error));
	TestFalse(TEXT("the generated header went"), Exists(OldType));
	TestFalse(TEXT("and the generated source"), Exists(OldAccessors));
	TestTrue(TEXT("the game's header stays"), Exists(GameHeader));
	TestTrue(TEXT("and the game's source"), Exists(GameSource));

	// The type emitter's sweep of the headers a sync no longer writes keeps the game's header too.
	const FFlockCppTypeEmitter::FEmitResult Types =
		FFlockCppTypeEmitter::Emit(EverythingSnapshot(), Root, TEXT("FlockGenerated"));
	TestTrue(FString::Printf(TEXT("the types were emitted (%s)"), *Types.Error), Types.bSucceeded);
	TestTrue(TEXT("the game's header survives the sweep"), Exists(GameHeader));
	return true;
}

// ── A Generated Code Path at the game's own module changes nothing in it ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCppModuleGameModuleTest, "Flock.Editor.CppModule.LeavesTheGamesOwnModuleAlone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCppModuleGameModuleTest::RunTest(const FString& Parameters)
{
	const FString ProjectDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FlockCppGameModuleTest"));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*ProjectDir, /*RequireExists*/ false, /*Tree*/ true); };
	const FString ProjectFile = MakeScratchCppProject(ProjectDir);
	const FString GameRoot = FPaths::Combine(ProjectDir, TEXT("Source"), TEXT("MyGame"));

	// The game's module, with files under the very names the skeleton writes.
	const TMap<FString, FString> GameFiles = {
		{ FPaths::Combine(GameRoot, TEXT("MyGame.Build.cs")), TEXT("using UnrealBuildTool;\npublic class MyGame : ModuleRules {}\n") },
		{ FPaths::Combine(GameRoot, TEXT("Public"), TEXT("MyGame.h")), TEXT("#pragma once\n") },
		{ FPaths::Combine(GameRoot, TEXT("Public"), TEXT("Hero.h")), TEXT("#pragma once\nstruct FHero {};\n") },
		{ FPaths::Combine(GameRoot, TEXT("Private"), TEXT("MyGame.cpp")), TEXT("#include \"MyGame.h\"\n") },
		{ FPaths::Combine(GameRoot, TEXT("Private"), TEXT("MyGameModule.cpp")), TEXT("// the game's module\n") },
	};
	for (const TPair<FString, FString>& File : GameFiles)
	{
		Plant(File.Key, File.Value);
	}
	const FString ProjectBefore = Read(ProjectFile);

	auto FilesInModule = [&GameRoot]()
	{
		TArray<FString> Files;
		IFileManager::Get().FindFilesRecursive(Files, *GameRoot, TEXT("*"), /*Files*/ true, /*Directories*/ false);
		Files.Sort();
		return FString::Join(Files, TEXT("\n"));
	};
	const FString FilesBefore = FilesInModule();

	// What a Blueprint sync and a Clean run against the code path: it neither deletes nor writes anything there.
	FString Error;
	TestTrue(TEXT("removing types from the game's module succeeds"),
		FFlockCppModuleEmitter::RemoveGeneratedTypes(GameRoot, TEXT("MyGame"), /*bResetManifest*/ true, Error));
	TestEqualSensitive(TEXT("and leaves exactly the game's files there, adding no stub"), FilesInModule(), FilesBefore);

	// What a C++ sync runs first.
	const FFlockCppModuleEmitter::FEmitResult Emitted = FFlockCppModuleEmitter::Emit(GameRoot, Manifest(), ProjectFile);
	TestFalse(TEXT("a C++ sync into the game's module is refused"), Emitted.bSucceeded);
	TestTrue(FString::Printf(TEXT("at the game's .Build.cs, with the fix (%s)"), *Emitted.Error),
		Emitted.Error.Contains(TEXT("MyGame.Build.cs")) && Emitted.Error.Contains(TEXT("Source/FlockGenerated")));

	for (const TPair<FString, FString>& File : GameFiles)
	{
		TestEqual(FString::Printf(TEXT("'%s' is unchanged"), *FPaths::GetCleanFilename(File.Key)), Read(File.Key), File.Value);
	}
	TestEqualSensitive(TEXT("nothing of codegen's was added"), FilesInModule(), FilesBefore);
	TestEqual(TEXT("and the project file is untouched"), Read(ProjectFile), ProjectBefore);
	return true;
}

// ── Switching to the C++ target deletes the Blueprint target's assets, and only those ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockCppSwitchKeepsGameAssetsTest,
	"Flock.Editor.CppModule.SwitchingToCppKeepsAssetsCodegenDidNotMake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockCppSwitchKeepsGameAssetsTest::RunTest(const FString& Parameters)
{
	const FCodegenTargetForThisTest OnCpp(EFlockCodegenTarget::Cpp);
	const FString ProjectDir = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("FlockCppSwitchTest"));
	const FScratchContentFolder Content(TEXT("FlockCppSwitchTest"));
	const FString& Root = Content.Root;
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*ProjectDir, /*RequireExists*/ false, /*Tree*/ true); };
	const FString ProjectFile = MakeScratchCppProject(ProjectDir);
	const FString CodeRoot = FPaths::Combine(ProjectDir, TEXT("Source"), TEXT("FlockGenerated"));

	// What the Blueprint target left, made by its real emitters (a struct and the catalog both targets share), and the
	// game's own assets beside and below it.
	FString StructError;
	const FFlockStructEmitter::FEmitResult Left = FFlockStructEmitter::Emit(ConfigOnlySnapshot(), Root, StructError);
	FString CatalogError;
	const bool bCatalogSaved = FFlockCatalogBuilder::Save(ConfigOnlySnapshot(), Root, CatalogError);
	const FString CatalogPath = Root + TEXT("/") + FFlockCatalogBuilder::AssetName;
	const TWeakObjectPtr<UObject> CatalogBefore =
		FindObject<UObject>(nullptr, *(CatalogPath + TEXT(".") + FFlockCatalogBuilder::AssetName));
	const FString LeftStruct = Root + TEXT("/GameplayConfig");
	const FString GameStruct = Root + TEXT("/HeroNotes");
	const FString GameEnumBelow = Root + TEXT("/Levels/LevelNames");
	const FString GameActor = Root + TEXT("/Levels/DoorActor");
	if (!TestTrue(TEXT("the Blueprint target's assets and the game's were made"),
		Left.StructCount == 1 && IsOnDisk(LeftStruct) && bCatalogSaved && CatalogBefore.IsValid()
			&& PlantGameStruct(GameStruct) && PlantGameEnum(GameEnumBelow) && PlantGameActorBlueprint(GameActor)))
	{
		return false;
	}

	const FFlockCodegenRunner::FRunResult Switched =
		FFlockCodegenRunner::EmitAll(EverythingSnapshot(), Root, CodeRoot, ProjectFile);
	if (!TestTrue(FString::Printf(TEXT("the C++ sync succeeded (%s)"), *Switched.Error), Switched.bSucceeded && Switched.bCppTarget))
	{
		return false;
	}

	TestFalse(TEXT("the Blueprint target's struct went"), IsOnDisk(LeftStruct));
	TestTrue(TEXT("the game's struct beside it stays"), IsOnDisk(GameStruct));
	TestTrue(TEXT("and the game's enum in the folder below"), IsOnDisk(GameEnumBelow));
	TestTrue(TEXT("and the game's Actor Blueprint"), IsOnDisk(GameActor));
	auto AnyWarningNames = [&Switched](const FString& What)
	{
		return Switched.Warnings.ContainsByPredicate([&What](const FString& Warning) { return Warning.Contains(What); });
	};
	TestTrue(TEXT("a warning names the kept struct, with what to do"), AnyWarningNames(GameStruct) && AnyWarningNames(TEXT("by hand")));
	TestFalse(TEXT("but not the Actor Blueprint, which can never list a type twice"), AnyWarningNames(GameActor));
	TestFalse(TEXT("and no generated asset is reported left behind"), AnyWarningNames(TEXT("are still in")));

	// The catalog both targets share is kept, the same asset, so whatever refers to it still does, and marked.
	TestTrue(TEXT("the catalog was not deleted and made again"),
		CatalogBefore.IsValid() && CatalogBefore.Get() == FindObject<UObject>(nullptr,
			*(CatalogPath + TEXT(".") + FFlockCatalogBuilder::AssetName)));
	TestTrue(TEXT("and it is still marked"), AssetsUnder(Root).ContainsByPredicate([&CatalogPath](const FAssetData& Asset)
		{
			return Asset.PackageName.ToString().Equals(CatalogPath, ESearchCase::CaseSensitive)
				&& FFlockGeneratedAssets::IsMarked(Asset);
		}));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
