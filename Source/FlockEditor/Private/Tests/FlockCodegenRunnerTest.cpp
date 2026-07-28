// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Codegen/FlockCodegenPaths.h"
#include "Codegen/FlockCodegenRunner.h"
#include "Codegen/FlockStructEmitter.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

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

	// A real clean, over a throwaway content root with one generated struct in it.
	const FString ContentRoot = FString::Printf(TEXT("/Temp/FlockCleanTest_%s"),
		*FGuid::NewGuid().ToString(EGuidFormats::Digits));

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
	if (!TestTrue(TEXT("something was generated to clean"), Structs.StructCount > 0))
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
	TestTrue(TEXT("and so is the manifest"), Cleaned.bManifestRemoved);
	TestFalse(TEXT("the manifest really is off disk"), IFileManager::Get().FileExists(*ManifestPath));

	IFileManager::Get().DeleteDirectory(*GeneratedRoot, /*bRequireExists*/ false, /*bTree*/ true);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
