// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Assets/FlockAssetCache.h"
#include "FlockLogger.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Models/FlockAssetModels.h"
#include "Tests/Support/FlockTemporaryFilesTestSupport.h"

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
	FString Temp;
	{
		FFlockAssetCache Cache(Dir, 0, MakeShared<FFlockNullLogger>());
		// A download that died mid-write ten minutes ago leaves this behind.
		Temp = Cache.BeginWrite(TEXT("a1"), TEXT("v"));
		FFileHelper::SaveStringToFile(TEXT("HALF"), *Temp);
		FlockMoveTestFileTimeBack(Temp);
		TestTrue(TEXT("the temp exists"), IFileManager::Get().FileExists(*Temp));
	}

	// Constructing over the same directory sweeps it, so a crash loop cannot fill the directory. The file itself is checked:
	// the cache never lists a temp as an entry, so asking the cache would pass whether or not anything was swept.
	FFlockAssetCache Fresh(Dir, 0, MakeShared<FFlockNullLogger>());
	TestFalse(TEXT("the stray temp is gone"), IFileManager::Get().FileExists(*Temp));
	TestFalse(TEXT("it never reads as a cache hit"), Fresh.Contains(TEXT("a1"), TEXT("v")));
	TestEqual(TEXT("and it counts for nothing"), Fresh.GetTotalSizeBytes(), static_cast<int64>(0));

	Cleanup(Dir);
	return true;
}

/**
 * Another launch of the game starting while this one downloads (two game clients on one machine, Play In Editor beside a
 * standalone game): its cache sweeps the folder as it is built, and the download this launch is about to commit survives.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAssetCacheKeepsADownloadAnotherLaunchIsWritingTest, "Flock.Assets.Cache.KeepsADownloadAnotherLaunchIsWriting",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockAssetCacheKeepsADownloadAnotherLaunchIsWritingTest::RunTest(const FString&)
{
	const FString Dir = TempRoot();
	{
		FFlockAssetCache Cache(Dir, 0, MakeShared<FFlockNullLogger>());
		const FString Downloading = Cache.BeginWrite(TEXT("a1"), TEXT("v"));
		TestTrue(TEXT("Precondition: the download's bytes are written"), FFileHelper::SaveStringToFile(TEXT("BYTES"), *Downloading));

		// Control: what a launch that crashed ten minutes ago left behind is still deleted.
		const FString LeftOver = Cache.BeginWrite(TEXT("a2"), TEXT("v"));
		TestTrue(TEXT("Precondition: a left-over download is written"), FFileHelper::SaveStringToFile(TEXT("HALF"), *LeftOver));
		FlockMoveTestFileTimeBack(LeftOver);

		const FFlockAssetCache AnotherLaunch(Dir, 0, MakeShared<FFlockNullLogger>());
		TestTrue(TEXT("The download in progress is left alone"), IFileManager::Get().FileExists(*Downloading));
		TestFalse(TEXT("Control: the left-over download is deleted"), IFileManager::Get().FileExists(*LeftOver));

		FString Committed;
		int32 Complaints = 0;
		const double StartedAt = FPlatformTime::Seconds();
		{
			FFlockFileManagerComplaintCounter Counter;
			Committed = Cache.Commit(TEXT("a1"), TEXT("v"), Downloading);
			Complaints = Counter.Count();
		}
		const double Seconds = FPlatformTime::Seconds() - StartedAt;

		TestEqual(TEXT("The download commits into place"), Committed, Cache.GetFinalPath(TEXT("a1"), TEXT("v")));
		FString Contents;
		FFileHelper::LoadFileToString(Contents, *Cache.GetFinalPath(TEXT("a1"), TEXT("v")));
		TestEqual(TEXT("With its bytes"), Contents, FString(TEXT("BYTES")));
		TestTrue(FString::Printf(TEXT("Without a wait (took %.2f s)"), Seconds), Seconds < FlockTestSecondsWithoutARetry);
		TestEqual(TEXT("The file manager logs no warning or error"), Complaints, 0);
	}
	Cleanup(Dir);
	return true;
}

/** Two launches downloading the same asset version at once each stream into a file of their own, and both commit. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAssetCacheTwoLaunchesOneAssetTest, "Flock.Assets.Cache.TwoLaunchesDownloadingOneAssetBothCommit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockAssetCacheTwoLaunchesOneAssetTest::RunTest(const FString&)
{
	const FString Dir = TempRoot();
	{
		FFlockAssetCache First(Dir, 0, MakeShared<FFlockNullLogger>());
		FFlockAssetCache Second(Dir, 0, MakeShared<FFlockNullLogger>());
		const FString FirstDownload = First.BeginWrite(TEXT("a1"), TEXT("v"));
		const FString SecondDownload = Second.BeginWrite(TEXT("a1"), TEXT("v"));
		TestFalse(TEXT("Each download streams into a file of its own"), FirstDownload.Equals(SecondDownload, ESearchCase::IgnoreCase));

		FString FirstCommitted;
		FString SecondCommitted;
		int32 Complaints = 0;
		{
			FFlockFileManagerComplaintCounter Counter;
			{
				// The first download is still streaming while the second one finishes and commits.
				const TUniquePtr<FArchive> Stream(IFileManager::Get().CreateFileWriter(*FirstDownload));
				TestTrue(TEXT("Precondition: the first download is streaming"), Stream.IsValid());
				if (Stream.IsValid())
				{
					uint8 Bytes[] = { 'F', 'I', 'R', 'S', 'T' };
					Stream->Serialize(Bytes, UE_ARRAY_COUNT(Bytes));
				}
				TestTrue(TEXT("Precondition: the second download's bytes are written"),
					FFileHelper::SaveStringToFile(TEXT("SECOND"), *SecondDownload));
				SecondCommitted = Second.Commit(TEXT("a1"), TEXT("v"), SecondDownload);
				if (Stream.IsValid())
				{
					Stream->Close();
				}
			}
			FirstCommitted = First.Commit(TEXT("a1"), TEXT("v"), FirstDownload);
			Complaints = Counter.Count();
		}

		TestFalse(TEXT("The second download commits"), SecondCommitted.IsEmpty());
		TestFalse(TEXT("The first download commits after it"), FirstCommitted.IsEmpty());
		FString Contents;
		FFileHelper::LoadFileToString(Contents, *First.GetFinalPath(TEXT("a1"), TEXT("v")));
		TestEqual(TEXT("The asset holds the bytes committed last"), Contents, FString(TEXT("FIRST")));
		TArray<FString> Temps;
		IFileManager::Get().FindFiles(Temps, *FPaths::Combine(Dir, TEXT("*.tmp")), /*Files*/ true, /*Directories*/ false);
		TestEqual(TEXT("No temporary file is left"), Temps.Num(), 0);
		TestEqual(TEXT("The file manager logs no warning or error"), Complaints, 0);
	}
	Cleanup(Dir);
	return true;
}

/** A commit that cannot move its download into place gives up at once and leaves the bytes where they are. */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAssetCacheCommitGivesUpAtOnceTest, "Flock.Assets.Cache.ACommitThatCannotMoveGivesUpAtOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::ClientContext | EAutomationTestFlags::EngineFilter)

bool FFlockAssetCacheCommitGivesUpAtOnceTest::RunTest(const FString&)
{
	const FString Dir = TempRoot();
	{
		FFlockAssetCache Cache(Dir, 0, MakeShared<FFlockNullLogger>());
		const FString Download = Cache.BeginWrite(TEXT("a1"), TEXT("v"));
		TestTrue(TEXT("Precondition: the download's bytes are written"), FFileHelper::SaveStringToFile(TEXT("BYTES"), *Download));
		// A folder stands where the file goes, so the download can never be moved into place.
		IFileManager::Get().MakeDirectory(*Cache.GetFinalPath(TEXT("a1"), TEXT("v")), /*Tree*/ true);

		FString Committed;
		int32 Complaints = 0;
		const double StartedAt = FPlatformTime::Seconds();
		{
			FFlockFileManagerComplaintCounter Counter;
			Committed = Cache.Commit(TEXT("a1"), TEXT("v"), Download);
			Complaints = Counter.Count();
		}
		const double Seconds = FPlatformTime::Seconds() - StartedAt;

		TestTrue(TEXT("It reports that nothing was committed"), Committed.IsEmpty());
		TestTrue(FString::Printf(TEXT("It gives up at once instead of retrying on the game thread (took %.2f s)"), Seconds), Seconds < FlockTestSecondsWithoutARetry);
		TestEqual(TEXT("The file manager logs no warning or error"), Complaints, 0);
		TestTrue(TEXT("The downloaded bytes are still there for the caller"), IFileManager::Get().FileExists(*Download));
	}
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
