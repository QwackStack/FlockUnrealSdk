// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Assets/FlockAssetCache.h"
#include "FlockLogger.h"
#include "HAL/FileManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Models/FlockAssetModels.h"

namespace FlockAssetCacheTestHelpers
{
	inline FString TempRoot()
	{
		return FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
			FString::Printf(TEXT("assetcache_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	/** Writes Payload through the cache's own temp-then-commit path, as a download would. */
	inline FString Store(FFlockAssetCache& Cache, const FString& Id, const FString& Token, const FString& Payload)
	{
		const FString Temp = Cache.BeginWrite(Id, Token);
		FFileHelper::SaveStringToFile(Payload, *Temp);
		return Cache.Commit(Id, Token, Temp);
	}

	inline void Cleanup(const FString& Dir)
	{
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
	}
}

using namespace FlockAssetCacheTestHelpers;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAssetCacheVersionTest, "Flock.Assets.Cache.SupersedesOldVersions",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockAssetCacheVersionTest::RunTest(const FString&)
{
	const FString Dir = TempRoot();
	FFlockAssetCache Cache(Dir, /*MaxMB*/ 0, MakeShared<FFlockNullLogger>());

	Store(Cache, TEXT("a1"), TEXT("v1"), TEXT("OLD"));
	TestTrue(TEXT("v1 is present"), Cache.Contains(TEXT("a1"), TEXT("v1")));

	// A re-uploaded asset gets a new token; the previous copy must not linger and consume budget.
	Store(Cache, TEXT("a1"), TEXT("v2"), TEXT("NEW"));
	TestTrue(TEXT("v2 is present"), Cache.Contains(TEXT("a1"), TEXT("v2")));
	TestFalse(TEXT("v1 was dropped"), Cache.Contains(TEXT("a1"), TEXT("v1")));

	FString Path;
	TestTrue(TEXT("v2 resolves"), Cache.TryGetCachedPath(TEXT("a1"), TEXT("v2"), Path));
	FString Contents;
	FFileHelper::LoadFileToString(Contents, *Path);
	TestEqual(TEXT("and holds the new bytes"), Contents, FString(TEXT("NEW")));

	Cleanup(Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAssetCacheLruTest, "Flock.Assets.Cache.EvictsLeastRecentlyUsed",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockAssetCacheLruTest::RunTest(const FString&)
{
	const FString Dir = TempRoot();
	// 1 MB budget and ~400 KB entries: two fit, the third pushes to ~1.2 MB and forces exactly one
	// eviction. Sizing it so only one entry has to go is what makes "which one" a real assertion.
	FFlockAssetCache Cache(Dir, /*MaxMB*/ 1, MakeShared<FFlockNullLogger>());
	const FString Big = FString::ChrN(400 * 1024, TEXT('x'));

	Store(Cache, TEXT("a1"), TEXT("v"), Big);
	Store(Cache, TEXT("a2"), TEXT("v"), Big);

	// Order the two by hand rather than by wall clock. Writes microseconds apart can land on the same
	// filesystem timestamp, which would leave the sort — and so this test — deciding nothing.
	const FDateTime Now = FDateTime::UtcNow();
	IFileManager::Get().SetTimeStamp(*Cache.GetFinalPath(TEXT("a2"), TEXT("v")), Now - FTimespan::FromHours(2));
	IFileManager::Get().SetTimeStamp(*Cache.GetFinalPath(TEXT("a1"), TEXT("v")), Now - FTimespan::FromHours(1));

	// Reading a1 stamps it as used, making it newer than a2 despite a2 being written later. Without the
	// read-stamp this test would pass on a plain FIFO and prove nothing about LRU.
	FString Path;
	Cache.TryGetCachedPath(TEXT("a1"), TEXT("v"), Path);

	Store(Cache, TEXT("a3"), TEXT("v"), Big);

	TestTrue(TEXT("the budget is respected"), Cache.GetTotalSizeBytes() <= 1024 * 1024);
	TestTrue(TEXT("the freshly written entry survives"), Cache.Contains(TEXT("a3"), TEXT("v")));
	TestTrue(TEXT("the recently read entry survives"), Cache.Contains(TEXT("a1"), TEXT("v")));
	TestFalse(TEXT("the untouched entry was evicted"), Cache.Contains(TEXT("a2"), TEXT("v")));

	Cleanup(Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAssetCacheSweepTest, "Flock.Assets.Cache.SweepsStrayTemps",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockAssetCacheSweepTest::RunTest(const FString&)
{
	const FString Dir = TempRoot();
	{
		FFlockAssetCache Cache(Dir, 0, MakeShared<FFlockNullLogger>());
		// A download that died mid-write leaves this behind.
		const FString Temp = Cache.BeginWrite(TEXT("a1"), TEXT("v"));
		FFileHelper::SaveStringToFile(TEXT("HALF"), *Temp);
		TestTrue(TEXT("the temp exists"), IFileManager::Get().FileExists(*Temp));
	}

	// Constructing over the same directory sweeps it, so a truncated file can never read as a cache hit.
	FFlockAssetCache Fresh(Dir, 0, MakeShared<FFlockNullLogger>());
	TestFalse(TEXT("the stray temp is gone"), Fresh.Contains(TEXT("a1"), TEXT("v")));
	TestEqual(TEXT("and it counts for nothing"), Fresh.GetTotalSizeBytes(), static_cast<int64>(0));

	Cleanup(Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAssetCacheEscapeTest, "Flock.Assets.Cache.CannotEscapeItsDirectory",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockAssetCacheEscapeTest::RunTest(const FString&)
{
	const FString Dir = TempRoot();
	FFlockAssetCache Cache(Dir, 0, MakeShared<FFlockNullLogger>());

	// An id is server data. Traversal characters must be neutralised, not trusted.
	const FString Path = Cache.GetFinalPath(TEXT("../../evil"), TEXT("v"));
	TestTrue(TEXT("the path stays inside the cache directory"), Path.StartsWith(Dir));

	// Assert on the filename, not the whole path: FPaths::ProjectIntermediateDir() is project-relative
	// and legitimately contains "..", so testing the full string would fail on the cache root itself.
	const FString FileName = FPaths::GetCleanFilename(Path);
	TestFalse(TEXT("no traversal survives in the filename"), FileName.Contains(TEXT("..")));
	TestFalse(TEXT("and no separator survives"), FileName.Contains(TEXT("/")) || FileName.Contains(TEXT("\\")));

	Cleanup(Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAssetVersionTokenTest, "Flock.Assets.Model.VersionTokenTracksUpdatedAt",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockAssetVersionTokenTest::RunTest(const FString&)
{
	FFlockAsset Asset;
	Asset.Id = TEXT("a1");
	Asset.UpdatedAt = TEXT("2026-07-29T10:00:00Z");
	const FString First = Asset.VersionToken();

	Asset.UpdatedAt = TEXT("2026-07-29T10:00:01Z");
	TestNotEqual(TEXT("a re-upload changes the token"), Asset.VersionToken(), First);

	Asset.UpdatedAt = TEXT("2026-07-29T10:00:00Z");
	TestEqual(TEXT("and the same timestamp reproduces it"), Asset.VersionToken(), First);

	// An empty timestamp must still key stably, or every fetch would look like a new version.
	FFlockAsset Empty;
	TestEqual(TEXT("an empty timestamp is stable"), Empty.VersionToken(), FFlockAsset().VersionToken());
	TestFalse(TEXT("and produces a usable token"), Empty.VersionToken().IsEmpty());

	return true;
}

#endif // WITH_AUTOMATION_TESTS
