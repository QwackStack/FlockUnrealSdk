// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Codegen/FlockCatalogBuilder.h"
#include "Codegen/FlockCodegenPaths.h"
#include "Codegen/FlockCodegenRunner.h"
#include "Codegen/FlockEnumEmitter.h"
#include "Codegen/FlockFunctionLibraryEmitter.h"
#include "Codegen/FlockMacroLibraryEmitter.h"
#include "Codegen/FlockStructEmitter.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Misc/ScopeExit.h"
#include "Tests/Support/FlockCodegenTestContent.h"

using namespace FlockCodegenTestContent;

namespace FlockCodegenRunnerTestHelpers
{
	inline FFlockCodegenManifest Manifest(const FString& GameVersionId)
	{
		FFlockCodegenManifest Result;
		Result.GameVersionId = GameVersionId;
		Result.ContentHash = TEXT("abc123");
		return Result;
	}
}

using namespace FlockCodegenRunnerTestHelpers;

// ── The startup drift check, which runs without touching the network ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRunnerBakedDriftTest, "Flock.Editor.CodegenRunner.ComparesBakedVersionOffline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockRunnerBakedDriftTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("matching ids are current"),
		FFlockCodegenRunner::CompareBakedVersion(TEXT("ver-1"), Manifest(TEXT("ver-1")), true),
		EFlockCodegenDrift::Current);

	// Someone re-baked the version, or pulled a branch whose generated code is for a different one.
	TestEqual(TEXT("a different baked id is drift"),
		FFlockCodegenRunner::CompareBakedVersion(TEXT("ver-2"), Manifest(TEXT("ver-1")), true),
		EFlockCodegenDrift::VersionChanged);

	// No manifest means codegen has never run here — not that anything is wrong.
	TestEqual(TEXT("no manifest"),
		FFlockCodegenRunner::CompareBakedVersion(TEXT("ver-1"), FFlockCodegenManifest(), false),
		EFlockCodegenDrift::NeverGenerated);
	TestEqual(TEXT("a manifest with no version is no manifest"),
		FFlockCodegenRunner::CompareBakedVersion(TEXT("ver-1"), FFlockCodegenManifest(), true),
		EFlockCodegenDrift::NeverGenerated);

	// An unbaked project is the play-mode guard's problem; reporting drift on top of it is just noise.
	TestEqual(TEXT("nothing baked yet"),
		FFlockCodegenRunner::CompareBakedVersion(FString(), Manifest(TEXT("ver-1")), true),
		EFlockCodegenDrift::Current);

	return true;
}

// ── The summary line distinguishes a clean sync from a partial one ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRunnerDescribeTest, "Flock.Editor.CodegenRunner.SummarisesTheRun",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockRunnerDescribeTest::RunTest(const FString& Parameters)
{
	FFlockCodegenRunner::FRunResult Success;
	Success.bSucceeded = true;
	Success.GameVersionId = TEXT("ver-1");
	Success.TemplateCount = 2;
	Success.ConfigCount = 1;
	Success.StructCount = 3;
	Success.EnumCount = 2;
	Success.FunctionCount = 9;

	const FString Clean = Success.Describe();
	TestTrue(TEXT("names the version"), Clean.Contains(TEXT("ver-1")));
	TestTrue(TEXT("counts what was written"), Clean.Contains(TEXT("3 struct(s)")));
	TestFalse(TEXT("a clean run mentions no warnings"), Clean.Contains(TEXT("warning")));

	// A sync that half worked must not read like one that fully worked — the toast is all most people see.
	FFlockCodegenRunner::FRunResult Partial = Success;
	Partial.Warnings.Add(TEXT("a field could not be typed"));
	TestTrue(TEXT("warnings are surfaced in the summary"), Partial.Describe().Contains(TEXT("1 warning(s)")));

	FFlockCodegenRunner::FRunResult Failed;
	Failed.Error = TEXT("could not reach the backend");
	const FString FailureLine = Failed.Describe();
	TestTrue(TEXT("failure says so"), FailureLine.Contains(TEXT("failed")));
	TestTrue(TEXT("failure carries the reason"), FailureLine.Contains(TEXT("could not reach the backend")));

	return true;
}

// ── Clean removes what a sync wrote, and refuses a path that is not a package path ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRunnerCleanTest, "Flock.Editor.CodegenRunner.CleanRemovesGeneratedOutput",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockRunnerCleanTest::RunTest(const FString& Parameters)
{
	// A content path is a package path, never a file path. Getting this wrong is how a clean would reach
	// outside the folder it owns, so it is rejected before anything is enumerated.
	const FFlockCodegenRunner::FCleanResult BadPath =
		FFlockCodegenRunner::CleanAt(TEXT("C:/Somewhere"), FString(), /*bShowConfirmation*/ false);
	TestFalse(TEXT("a non-package path is refused"), BadPath.bSucceeded);
	TestTrue(TEXT("and says why"), BadPath.Error.Contains(TEXT("package path")));
	TestEqual(TEXT("and deletes nothing"), BadPath.AssetsDeleted, 0);

	// A real clean, over a throwaway content root with one generated struct in it, saved to disk.
	const FScratchContentFolder Content(TEXT("FlockCleanTest"));
	const FString& ContentRoot = Content.Root;

	FFlockSchemaSnapshot Snapshot;
	Snapshot.GameVersionId = TEXT("ver-1");
	FFlockGameConfigSchema Config;
	Config.Id = TEXT("cfg-1");
	Config.Name = TEXT("Gameplay");
	Config.SchemaJson = TEXT("[{\"type\":\"int\",\"field_name\":\"move_speed\"}]");
	Snapshot.GameConfigs.Add(Config);

	FString StructError;
	const FFlockStructEmitter::FEmitResult Structs =
		FFlockStructEmitter::Emit(Snapshot, ContentRoot, StructError);
	if (!TestTrue(FString::Printf(TEXT("something was generated to clean and saved (%s)"), *StructError),
		Structs.StructCount > 0 && StructError.IsEmpty() && IsOnDisk(ContentRoot + TEXT("/GameplayConfig"))))
	{
		return false;
	}

	// The manifest travels with the assets: it is the claim that generated output exists, so a clean that
	// left it behind would have the drift check vouch for an empty folder.
	const FString GeneratedRoot = FPaths::Combine(FPaths::ProjectSavedDir(),
		FString::Printf(TEXT("FlockCleanTest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	FString ManifestError;
	FFlockCodegenManifest::Write(GeneratedRoot, FFlockCodegenManifest::FromSnapshot(Snapshot), ManifestError);
	const FString ManifestPath = FFlockCodegenPaths::ManifestPath(GeneratedRoot);
	if (!TestTrue(TEXT("a manifest was written"), IFileManager::Get().FileExists(*ManifestPath)))
	{
		return false;
	}

	const FFlockCodegenRunner::FCleanResult Cleaned =
		FFlockCodegenRunner::CleanAt(ContentRoot, GeneratedRoot, /*bShowConfirmation*/ false);
	TestTrue(TEXT("clean succeeded"), Cleaned.bSucceeded);
	TestTrue(TEXT("the generated assets are gone"), Cleaned.AssetsDeleted > 0);
	TestFalse(TEXT("from disk too"), IsOnDisk(ContentRoot + TEXT("/GameplayConfig")));
	TestTrue(TEXT("and so is the manifest"), Cleaned.bManifestRemoved);
	TestFalse(TEXT("the manifest really is off disk"), IFileManager::Get().FileExists(*ManifestPath));

	IFileManager::Get().DeleteDirectory(*GeneratedRoot, /*bRequireExists*/ false, /*bTree*/ true);
	return true;
}

// ── A Clean deletes what codegen made and nothing else, so a game asset in the folder survives it ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRunnerCleanKeepsGameAssetsTest,
	"Flock.Editor.CodegenRunner.CleanKeepsAssetsCodegenDidNotMake",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockRunnerCleanKeepsGameAssetsTest::RunTest(const FString& Parameters)
{
	const FCodegenTargetForThisTest OnBlueprint(EFlockCodegenTarget::Blueprint);
	const FScratchContentFolder Content(TEXT("FlockCleanKeepsTest"));
	const FString& Root = Content.Root;
	const FString CodeFolder = FPaths::Combine(FPaths::ProjectSavedDir(), Content.MountRoot.RightChop(1) + TEXT("_Code"));
	const FString CodeRoot = FPaths::Combine(CodeFolder, TEXT("FlockGenerated"));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*CodeFolder, /*RequireExists*/ false, /*Tree*/ true); };

	// The game's own: a struct beside what codegen makes, of a kind codegen makes too, and an enum a folder below.
	const FString GameStruct = Root + TEXT("/HeroNotes");
	const FString GameEnumBelow = Root + TEXT("/Levels/LevelNames");
	if (!TestTrue(TEXT("the game's assets were made"), PlantGameStruct(GameStruct) && PlantGameEnum(GameEnumBelow)))
	{
		return false;
	}

	// A real sync on the Blueprint target, so every emitter writes what it writes in a project.
	const FFlockCodegenRunner::FRunResult Synced = FFlockCodegenRunner::EmitAll(EverythingSnapshot(), Root, CodeRoot);
	if (!TestTrue(FString::Printf(TEXT("the sync succeeded (%s)"), *Synced.Error), Synced.bSucceeded))
	{
		return false;
	}

	// Every asset the sync wrote carries the mark, whichever emitter wrote it, and the game's carry none.
	const TArray<FAssetData> AfterSync = AssetsUnder(Root);
	for (const FAssetData& Asset : AfterSync)
	{
		const FString Name = Asset.PackageName.ToString();
		const bool bTheGames = Name.Equals(GameStruct, ESearchCase::CaseSensitive)
			|| Name.Equals(GameEnumBelow, ESearchCase::CaseSensitive);
		TestEqual(FString::Printf(TEXT("'%s' is marked exactly when codegen made it"), *Name),
			FFlockGeneratedAssets::IsMarked(Asset), !bTheGames);
	}
	for (const TCHAR* Made : { FFlockCatalogBuilder::AssetName, FFlockFunctionLibraryEmitter::LibraryAssetName,
		FFlockMacroLibraryEmitter::LibraryAssetName, FFlockEnumEmitter::ShopItemEnumName,
		FFlockEnumEmitter::CurrencyEnumName, FFlockEnumEmitter::AchievementEnumName, TEXT("GameplayConfig"),
		TEXT("WalletTemplate"), TEXT("TrophiesTemplate") })
	{
		TestTrue(FString::Printf(TEXT("the sync made %s"), Made), AnyNamed(AfterSync, Root + TEXT("/") + Made));
	}
	const int32 MadeBySync = CountMarked(AfterSync);

	// And the mark is in what was saved, which is all the asset registry has in a fresh editor session.
	const TArray<FAssetData> SavedAfterSync = AssetsOnDiskUnder(Root);
	TestEqual(TEXT("every asset was saved"), SavedAfterSync.Num(), AfterSync.Num());
	TestEqual(TEXT("and the saved ones carry the same marks"), CountMarked(SavedAfterSync), MadeBySync);

	const FFlockCodegenRunner::FCleanResult Cleaned = FFlockCodegenRunner::CleanAt(Root, CodeRoot, /*bShowConfirmation*/ false);
	if (!TestTrue(FString::Printf(TEXT("clean succeeded (%s)"), *Cleaned.Error), Cleaned.bSucceeded))
	{
		return false;
	}
	TestEqual(TEXT("everything the sync made was deleted"), Cleaned.AssetsDeleted, MadeBySync);
	TestEqual(TEXT("nothing codegen made is left"), CountMarked(AssetsUnder(Root)), 0);
	TestEqual(TEXT("and none is reported left"), Cleaned.GeneratedAssetsLeft, 0);
	TestTrue(TEXT("the game's struct beside them stays"), IsOnDisk(GameStruct));
	TestTrue(TEXT("and the game's enum in the folder below"), IsOnDisk(GameEnumBelow));
	TestEqual(TEXT("both are named as kept"), Cleaned.KeptAssets.Num(), 2);
	TestTrue(TEXT("and the summary says so, with what to do"),
		Cleaned.Describe().Contains(TEXT("Kept 2 asset(s)")) && Cleaned.Describe().Contains(TEXT("by hand")));
	return true;
}

// ── A content path that is a whole content folder is refused before anything is deleted or written ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRunnerRefusesWholeContentFolderTest,
	"Flock.Editor.CodegenRunner.RefusesAWholeContentFolder",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockRunnerRefusesWholeContentFolderTest::RunTest(const FString& Parameters)
{
	const FFlockCodegenRunner::FCleanResult Clean =
		FFlockCodegenRunner::CleanAt(TEXT("/Game"), FString(), /*bShowConfirmation*/ false);
	TestFalse(TEXT("a clean of /Game is refused"), Clean.bSucceeded);
	TestEqual(TEXT("and deletes nothing"), Clean.AssetsDeleted, 0);
	TestTrue(FString::Printf(TEXT("and says what is wrong (%s)"), *Clean.Error), Clean.Error.Contains(TEXT("whole content folder")));
	TestTrue(TEXT("and how to fix it"), Clean.Describe().Contains(TEXT("/Game/Flock/Generated")));

	// The code path is a setting of its own, so the C++ side is still cleaned.
	const FString CleanedCodeRoot = FPaths::Combine(FPaths::ProjectSavedDir(),
		FString::Printf(TEXT("FlockRefuseCleanTest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*CleanedCodeRoot, /*RequireExists*/ false, /*Tree*/ true); };
	FString ManifestError;
	FFlockCodegenManifest::Write(CleanedCodeRoot, FFlockCodegenManifest::FromSnapshot(EverythingSnapshot()), ManifestError);
	const FFlockCodegenRunner::FCleanResult CleanBoth =
		FFlockCodegenRunner::CleanAt(TEXT("/Game"), CleanedCodeRoot, /*bShowConfirmation*/ false);
	TestFalse(TEXT("a clean of /Game with a code path is still refused"), CleanBoth.bSucceeded);
	TestTrue(TEXT("but the manifest went"), CleanBoth.bManifestRemoved
		&& !IFileManager::Get().FileExists(*FFlockCodegenPaths::ManifestPath(CleanedCodeRoot)));

	// A sync is refused too, on either target, before it writes a file or an asset.
	for (const EFlockCodegenTarget Target : { EFlockCodegenTarget::Blueprint, EFlockCodegenTarget::Cpp })
	{
		const FCodegenTargetForThisTest ThisTarget(Target);
		const FString Folder = FPaths::Combine(FPaths::ProjectSavedDir(),
			FString::Printf(TEXT("FlockRefuseTest_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
		const FString CodeRoot = FPaths::Combine(Folder, TEXT("Source"), TEXT("FlockGenerated"));
		const FFlockCodegenRunner::FRunResult Synced = FFlockCodegenRunner::EmitAll(EverythingSnapshot(), TEXT("/Game"),
			CodeRoot, FPaths::Combine(Folder, TEXT("Scratch.uproject")));
		const FString Which = Target == EFlockCodegenTarget::Cpp ? TEXT("C++") : TEXT("Blueprint");
		TestFalse(FString::Printf(TEXT("a %s sync into /Game is refused"), *Which), Synced.bSucceeded);
		TestTrue(FString::Printf(TEXT("and says why (%s)"), *Synced.Error), Synced.Error.Contains(TEXT("whole content folder")));
		TestFalse(FString::Printf(TEXT("and the %s sync wrote nothing"), *Which), IFileManager::Get().DirectoryExists(*Folder));
		IFileManager::Get().DeleteDirectory(*Folder, /*RequireExists*/ false, /*Tree*/ true);
	}
	return true;
}

// ── A sync leaves a game asset of another kind alone, where the engine would otherwise stop the editor ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockRunnerLeavesOtherKindsAloneTest,
	"Flock.Editor.CodegenRunner.SyncLeavesAnAssetOfAnotherKindAlone",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockRunnerLeavesOtherKindsAloneTest::RunTest(const FString& Parameters)
{
	const FCodegenTargetForThisTest OnBlueprint(EFlockCodegenTarget::Blueprint);
	const FScratchContentFolder Content(TEXT("FlockOtherKindTest"));
	const FString& Root = Content.Root;
	const FString CodeFolder = FPaths::Combine(FPaths::ProjectSavedDir(), Content.MountRoot.RightChop(1) + TEXT("_Code"));
	ON_SCOPE_EXIT { IFileManager::Get().DeleteDirectory(*CodeFolder, /*RequireExists*/ false, /*Tree*/ true); };

	// The game's assets under the names each emitter writes, each of a kind that emitter does not make.
	const FString EnumWhereTheStructGoes = Root + TEXT("/GameplayConfig");
	const TArray<FString> StructsWhereOthersGo = {
		Root + TEXT("/") + FFlockEnumEmitter::ShopItemEnumName,
		Root + TEXT("/") + FFlockFunctionLibraryEmitter::LibraryAssetName,
		Root + TEXT("/") + FFlockMacroLibraryEmitter::LibraryAssetName,
		Root + TEXT("/") + FFlockCatalogBuilder::AssetName,
	};
	bool bPlanted = PlantGameEnum(EnumWhereTheStructGoes);
	for (const FString& Package : StructsWhereOthersGo)
	{
		bPlanted &= PlantGameStruct(Package);
	}
	if (!TestTrue(TEXT("the game's assets were made"), bPlanted))
	{
		return false;
	}

	const FFlockCodegenRunner::FRunResult Synced = FFlockCodegenRunner::EmitAll(EverythingSnapshot(), Root,
		FPaths::Combine(CodeFolder, TEXT("FlockGenerated")));

	// Still the game's, and still what they were.
	TestNotNull(TEXT("the game's enum is still an enum"), LoadObject<UUserDefinedEnum>(nullptr,
		*(EnumWhereTheStructGoes + TEXT(".") + FPackageName::GetShortName(EnumWhereTheStructGoes))));
	for (const FString& Package : StructsWhereOthersGo)
	{
		const FString ShortName = FPackageName::GetShortName(Package);
		TestNotNull(FString::Printf(TEXT("the game's %s is still a struct"), *ShortName),
			LoadObject<UUserDefinedStruct>(nullptr, *(Package + TEXT(".") + ShortName)));
		const bool bNamed = Synced.Warnings.ContainsByPredicate([&Package](const FString& Warning)
			{
				return Warning.Contains(Package) && Warning.Contains(TEXT("folder of its own"));
			});
		TestTrue(FString::Printf(TEXT("a warning names %s and the fix"), *ShortName), bNamed);
	}
	TestTrue(TEXT("and so does one for the struct's place"), Synced.Warnings.ContainsByPredicate(
		[&EnumWhereTheStructGoes](const FString& Warning) { return Warning.Contains(EnumWhereTheStructGoes); }));
	// What had room was still made.
	TestTrue(TEXT("the other structs were generated"), Synced.StructCount >= 2);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
