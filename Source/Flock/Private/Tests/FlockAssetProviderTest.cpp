// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Assets/FlockAssetCache.h"
#include "Auth/FlockAuthSession.h"
#include "FlockLogger.h"
#include "HAL/FileManager.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockSnapshotStore.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Providers/FlockAssetProvider.h"
#include "Tests/Support/FlockFakeAssetDownloader.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"

namespace FlockAssetProviderTestHelpers
{
	/** The list route is enveloped with a bare array in `result` — mirror the real wire shape. */
	inline FString ListBody(const TCHAR* Url = TEXT("https://s3/u-old"), const TCHAR* UpdatedAt = TEXT("t1"),
		int64 SizeBytes = 4)
	{
		return FString::Printf(
			TEXT("{\"result\":[{\"id\":\"a1\",\"name\":\"logo\",\"extension_type\":\"png\",\"size_bytes\":%lld,")
			TEXT("\"s3_download_url\":\"%s\",\"game_id\":\"g1\",\"created_at\":\"t0\",\"updated_at\":\"%s\"}]}"),
			SizeBytes, Url, UpdatedAt);
	}

	/** The by-id route is enveloped around a single record. */
	inline FString RecordBody(const TCHAR* Url = TEXT("https://s3/u-new"), const TCHAR* UpdatedAt = TEXT("t1"))
	{
		return FString::Printf(
			TEXT("{\"result\":{\"id\":\"a1\",\"name\":\"logo\",\"extension_type\":\"png\",\"size_bytes\":4,")
			TEXT("\"s3_download_url\":\"%s\",\"game_id\":\"g1\",\"created_at\":\"t0\",\"updated_at\":\"%s\"}}"),
			Url, UpdatedAt);
	}

	inline FFlockRetryPolicy NoRetry()
	{
		FFlockRetryPolicy Policy;
		Policy.MaxRetries = 0;
		return Policy;
	}

	inline FString TempRoot()
	{
		return FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
			FString::Printf(TEXT("asset_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	struct FFixture
	{
		FString Dir;
		FString CacheDir;
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockFakeAssetDownloader> Downloader = MakeShared<FFlockFakeAssetDownloader>();
		TSharedRef<FFlockHttpClient> Client;
		TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();
		TSharedRef<FFlockAuthSession> Session;
		TSharedPtr<FFlockSnapshotStore> Snapshot;
		TSharedRef<FFlockAssetCache> Cache;
		TSharedPtr<FFlockAssetProvider> Provider;

		explicit FFixture(const FString& ExistingDir = FString(), int32 CacheMaxMB = 0)
			: Dir(ExistingDir.IsEmpty() ? TempRoot() : ExistingDir)
			, CacheDir(FPaths::Combine(Dir, TEXT("bin")))
			, Client(MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>()))
			, Session(MakeShared<FFlockAuthSession>(Client, Store, MakeShared<FFlockNullLogger>(),
				TEXT("http://x/v1"), TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") } }))
			, Cache(MakeShared<FFlockAssetCache>(CacheDir, CacheMaxMB, MakeShared<FFlockNullLogger>()))
		{
			Snapshot = MakeShared<FFlockSnapshotStore>(FPaths::Combine(Dir, TEXT("snap")),
				MakeShared<FFlockNullLogger>(), TEXT("9.9.9"));
			Provider = MakeShared<FFlockAssetProvider>(Client, NoRetry(), MakeShared<FFlockNullLogger>(),
				Session, TEXT("http://x/v1"), Snapshot, TEXT("ver-1"), Downloader, Cache);
			Provider->Configure(/*bCacheEnabled*/ true, /*Timeout*/ 0.f, /*Retries*/ 0, /*MaxConcurrent*/ 4);
		}
	};

	inline void Cleanup(const FString& Dir)
	{
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
	}
}

using namespace FlockAssetProviderTestHelpers;

// ─────────────────────────────────── Metadata ────────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAssetIndexTest, "Flock.Assets.Provider.IndexesAndMemoizes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAssetIndexTest::RunTest(const FString&)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("/asset"), FFlockFakeTransport::Ok(ListBody()));

	int32 Count = 0;
	Fx.Provider->GetAll([&](TFlockResult<TArray<FFlockAsset>> Result)
	{
		TestTrue(TEXT("index fetch succeeds"), Result.bSuccess);
		Count = Result.Value.Num();
	});
	TestEqual(TEXT("one asset in the index"), Count, 1);

	// A second call must be served from memory, not the wire.
	Fx.Provider->GetAll([](TFlockResult<TArray<FFlockAsset>>) {});
	TestEqual(TEXT("index fetched once"), Fx.Fake->Requests.Num(), 1);

	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAssetByNameTest, "Flock.Assets.Provider.ResolvesByNameWithoutARoute",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAssetByNameTest::RunTest(const FString&)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("/asset"), FFlockFakeTransport::Ok(ListBody()));

	// There is no by-name route on the backend; the name has to resolve against the index.
	FString FoundId;
	Fx.Provider->GetByName(TEXT("logo"), [&](TFlockResult<FFlockAsset> Result)
	{
		TestTrue(TEXT("by-name succeeds"), Result.bSuccess);
		FoundId = Result.Value.Id;
	});
	TestEqual(TEXT("resolved the right record"), FoundId, FString(TEXT("a1")));

	bool bMissingFailed = false;
	Fx.Provider->GetByName(TEXT("nope"), [&](TFlockResult<FFlockAsset> Result)
	{
		bMissingFailed = !Result.bSuccess;
	});
	TestTrue(TEXT("an unknown name fails"), bMissingFailed);

	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAssetOfflineIndexTest, "Flock.Assets.Provider.ServesIndexOffline",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAssetOfflineIndexTest::RunTest(const FString&)
{
	const FString Dir = TempRoot();
	{
		FFixture Warm(Dir);
		Warm.Fake->On(TEXT("/asset"), FFlockFakeTransport::Ok(ListBody()));
		Warm.Provider->GetAll([](TFlockResult<TArray<FFlockAsset>>) {});
	}

	// Fresh provider, same snapshot directory, and nothing reachable.
	FFixture Cold(Dir);
	Cold.Provider->SetReachabilityProbe([]() { return false; });

	int32 Count = -1;
	Cold.Provider->GetAll([&](TFlockResult<TArray<FFlockAsset>> Result)
	{
		TestTrue(TEXT("cached index is served offline"), Result.bSuccess);
		Count = Result.Value.Num();
	});
	TestEqual(TEXT("index came back from the snapshot"), Count, 1);
	TestEqual(TEXT("no call was made"), Cold.Fake->Requests.Num(), 0);

	Cleanup(Dir);
	return true;
}

// ─────────────────────────────────── Downloads ───────────────────────────────────

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAssetDownloadCachesTest, "Flock.Assets.Provider.CachesDownloadedBytes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAssetDownloadCachesTest::RunTest(const FString&)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("/asset"), FFlockFakeTransport::Ok(ListBody()));
	Fx.Downloader->On(TEXT("u-old"), TEXT("PNG!"));

	FString Text;
	Fx.Provider->DownloadText(TEXT("logo"), FFlockAssetProgress(), [&](TFlockResult<FString> Result)
	{
		TestTrue(TEXT("download succeeds"), Result.bSuccess);
		Text = Result.Value;
	});
	TestEqual(TEXT("payload came through"), Text, FString(TEXT("PNG!")));
	TestEqual(TEXT("transferred once"), Fx.Downloader->Requests.Num(), 1);

	// Second read of the same version must be served from disk.
	Fx.Provider->DownloadText(TEXT("logo"), FFlockAssetProgress(), [&](TFlockResult<FString> Result)
	{
		TestTrue(TEXT("second download succeeds"), Result.bSuccess);
		TestEqual(TEXT("same payload from cache"), Result.Value, FString(TEXT("PNG!")));
	});
	TestEqual(TEXT("cache hit made no second transfer"), Fx.Downloader->Requests.Num(), 1);

	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAssetExpiredUrlTest, "Flock.Assets.Provider.RefreshesExpiredDownloadUrl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAssetExpiredUrlTest::RunTest(const FString&)
{
	FFixture Fx;
	// The index hands out a presigned URL that storage no longer honours; the by-id route issues a fresh one.
	Fx.Fake->On(TEXT("/asset/a1"), FFlockFakeTransport::Ok(RecordBody(TEXT("https://s3/u-new"))));
	Fx.Fake->On(TEXT("/asset"), FFlockFakeTransport::Ok(ListBody(TEXT("https://s3/u-old"))));
	Fx.Downloader->OnStatus(TEXT("u-old"), 403, TEXT("expired"));
	Fx.Downloader->On(TEXT("u-new"), TEXT("FRESH"));

	FString Text;
	bool bSucceeded = false;
	Fx.Provider->DownloadText(TEXT("logo"), FFlockAssetProgress(), [&](TFlockResult<FString> Result)
	{
		bSucceeded = Result.bSuccess;
		Text = Result.Value;
	});

	TestTrue(TEXT("a refused signature recovers instead of failing"), bSucceeded);
	TestEqual(TEXT("served the bytes from the re-signed URL"), Text, FString(TEXT("FRESH")));
	TestEqual(TEXT("tried the stale URL once"), Fx.Downloader->CountRequestsContaining(TEXT("u-old")), 1);
	TestEqual(TEXT("retried the fresh URL once"), Fx.Downloader->CountRequestsContaining(TEXT("u-new")), 1);

	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAssetExpiredUrlOnceTest, "Flock.Assets.Provider.RefreshesExpiredUrlOnlyOnce",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAssetExpiredUrlOnceTest::RunTest(const FString&)
{
	FFixture Fx;
	// The re-signed URL is refused too. Without the refresh being disarmed on the retry this recurses.
	Fx.Fake->On(TEXT("/asset/a1"), FFlockFakeTransport::Ok(RecordBody(TEXT("https://s3/u-new"))));
	Fx.Fake->On(TEXT("/asset"), FFlockFakeTransport::Ok(ListBody(TEXT("https://s3/u-old"))));
	Fx.Downloader->OnStatus(TEXT("u-old"), 403);
	Fx.Downloader->OnStatus(TEXT("u-new"), 403);

	bool bFailed = false;
	Fx.Provider->DownloadText(TEXT("logo"), FFlockAssetProgress(), [&](TFlockResult<FString> Result)
	{
		bFailed = !Result.bSuccess;
	});

	TestTrue(TEXT("gives up rather than looping"), bFailed);
	TestEqual(TEXT("exactly one refresh attempt"), Fx.Downloader->CountRequestsContaining(TEXT("u-new")), 1);

	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAssetFailureLeavesNoFileTest, "Flock.Assets.Provider.FailedDownloadLeavesNoCacheEntry",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAssetFailureLeavesNoFileTest::RunTest(const FString&)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("/asset"), FFlockFakeTransport::Ok(ListBody()));
	Fx.Downloader->OnStatus(TEXT("u-old"), 500, TEXT("boom"));

	bool bFailed = false;
	Fx.Provider->DownloadText(TEXT("logo"), FFlockAssetProgress(), [&](TFlockResult<FString> Result)
	{
		bFailed = !Result.bSuccess;
	});
	TestTrue(TEXT("the failure propagates"), bFailed);

	// A truncated or error-document file surviving here would later read as a valid cache hit.
	TestEqual(TEXT("nothing was committed to the cache"), Fx.Cache->GetTotalSizeBytes(), static_cast<int64>(0));

	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAssetConcurrencyCapTest, "Flock.Assets.Provider.HonoursConcurrencyCap",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAssetConcurrencyCapTest::RunTest(const FString&)
{
	FFixture Fx;
	Fx.Provider->Configure(/*bCacheEnabled*/ true, 0.f, /*Retries*/ 0, /*MaxConcurrent*/ 2);
	Fx.Downloader->bDeferred = true;

	// Five distinct assets so nothing coalesces and each needs its own slot.
	TArray<FFlockAsset> Assets;
	for (int32 Index = 0; Index < 5; ++Index)
	{
		FFlockAsset Asset;
		Asset.Id = FString::Printf(TEXT("a%d"), Index);
		Asset.Name = Asset.Id;
		Asset.UpdatedAt = TEXT("t1");
		Asset.SizeBytes = 4;
		Asset.S3DownloadUrl = FString::Printf(TEXT("https://s3/file%d"), Index);
		Assets.Add(Asset);
		Fx.Downloader->On(FString::Printf(TEXT("file%d"), Index), TEXT("DATA"));
	}

	int32 Succeeded = -1;
	Fx.Provider->Preload(Assets, nullptr, [&](TFlockResult<int32> Result)
	{
		Succeeded = Result.Value;
	});

	TestEqual(TEXT("only the cap starts immediately"), Fx.Downloader->Requests.Num(), 2);
	Fx.Downloader->FlushAll();

	TestEqual(TEXT("every asset eventually transferred"), Fx.Downloader->Requests.Num(), 5);
	TestEqual(TEXT("all five landed"), Succeeded, 5);
	TestEqual(TEXT("never exceeded the cap"), Fx.Downloader->PeakActive, 2);

	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAssetCoalesceTest, "Flock.Assets.Provider.CoalescesConcurrentDownloads",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAssetCoalesceTest::RunTest(const FString&)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("/asset"), FFlockFakeTransport::Ok(ListBody()));
	Fx.Downloader->On(TEXT("u-old"), TEXT("PNG!"));
	Fx.Provider->GetAll([](TFlockResult<TArray<FFlockAsset>>) {});
	Fx.Downloader->bDeferred = true;

	// Two callers, same asset, same version: one transfer, two results.
	int32 Delivered = 0;
	Fx.Provider->DownloadText(TEXT("logo"), FFlockAssetProgress(), [&](TFlockResult<FString> R) { if (R.bSuccess) { ++Delivered; } });
	Fx.Provider->DownloadText(TEXT("logo"), FFlockAssetProgress(), [&](TFlockResult<FString> R) { if (R.bSuccess) { ++Delivered; } });

	TestEqual(TEXT("only one transfer started"), Fx.Downloader->Requests.Num(), 1);
	Fx.Downloader->FlushAll();
	TestEqual(TEXT("both callers were served"), Delivered, 2);

	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAssetOversizeTest, "Flock.Assets.Provider.OversizeAssetSkipsCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAssetOversizeTest::RunTest(const FString&)
{
	// 1 MB budget, and an asset that claims to be larger than all of it.
	FFixture Fx(FString(), /*CacheMaxMB*/ 1);
	Fx.Fake->On(TEXT("/asset"), FFlockFakeTransport::Ok(
		ListBody(TEXT("https://s3/u-old"), TEXT("t1"), /*SizeBytes*/ 8 * 1024 * 1024)));
	Fx.Downloader->On(TEXT("u-old"), TEXT("BIG"));

	bool bSucceeded = false;
	Fx.Provider->DownloadText(TEXT("logo"), FFlockAssetProgress(), [&](TFlockResult<FString> Result)
	{
		bSucceeded = Result.bSuccess;
	});

	// It still downloads — it just must not evict the entire cache to store itself.
	TestTrue(TEXT("an oversize asset still downloads"), bSucceeded);
	TestEqual(TEXT("but nothing was cached"), Fx.Cache->GetTotalSizeBytes(), static_cast<int64>(0));

	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAssetCacheQueriesTest, "Flock.Assets.Provider.CacheQueries",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAssetCacheQueriesTest::RunTest(const FString&)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("/asset"), FFlockFakeTransport::Ok(ListBody()));
	Fx.Downloader->On(TEXT("u-old"), TEXT("PNG!"));

	FFlockAsset Asset;
	Fx.Provider->GetByName(TEXT("logo"), [&](TFlockResult<FFlockAsset> Result) { Asset = Result.Value; });

	TestFalse(TEXT("not cached before the download"), Fx.Provider->IsCached(Asset));
	TestEqual(TEXT("and listed as uncached"), Fx.Provider->GetUncached({ Asset }).Num(), 1);
	TestTrue(TEXT("no cached path yet"), Fx.Provider->GetCachedFilePath(Asset).IsEmpty());

	Fx.Provider->DownloadText(TEXT("logo"), FFlockAssetProgress(), [](TFlockResult<FString>) {});

	TestTrue(TEXT("cached after the download"), Fx.Provider->IsCached(Asset));
	TestEqual(TEXT("and no longer listed as uncached"), Fx.Provider->GetUncached({ Asset }).Num(), 0);
	TestFalse(TEXT("cached path resolves"), Fx.Provider->GetCachedFilePath(Asset).IsEmpty());

	Fx.Provider->ClearCache();
	TestFalse(TEXT("cleared"), Fx.Provider->IsCached(Asset));

	Cleanup(Fx.Dir);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
