// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockAuthSession.h"
#include "FlockLogger.h"
#include "HAL/FileManager.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockSnapshotStore.h"
#include "Misc/Paths.h"
#include "Providers/FlockConfigProvider.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"

namespace FlockConfigProviderTestHelpers
{
	// Enveloped fixtures — `result` is an OBJECT for a single config/patch and an ARRAY for a list. An
	// object-shaped list fixture would pass while the live wire (a bare array) failed, so these mirror it.
	const TCHAR* const ConfigBody =
		TEXT("{\"result\":{\"id\":\"cfg-1\",\"name\":\"Balance\",\"game_id\":\"g\",\"game_version_id\":\"ver-1\",")
		TEXT("\"tag\":\"gameplay\",\"created_at\":\"\",\"updated_at\":\"\",")
		TEXT("\"data\":[{\"type\":\"int\",\"field_name\":\"max_health\",\"value\":100}]}}");

	const TCHAR* const ConfigListBody =
		TEXT("{\"result\":[{\"id\":\"cfg-1\",\"name\":\"Balance\",\"game_id\":\"g\",\"tag\":\"gameplay\",")
		TEXT("\"created_at\":\"\",\"updated_at\":\"\",\"data\":[{\"type\":\"int\",\"field_name\":\"max_health\",\"value\":100}]}]}");

	// A patch overriding max_health to 999 (so resolve-with-patch is distinguishable from the base 100).
	const TCHAR* const PatchListBody =
		TEXT("{\"result\":[{\"id\":\"p1\",\"name\":\"P\",\"game_config_id\":\"cfg-1\",\"created_at\":\"\",\"updated_at\":\"\",")
		TEXT("\"data\":[{\"type\":\"int\",\"field_name\":\"max_health\",\"value\":999}]}]}");

	const TCHAR* const EmptyListBody = TEXT("{\"result\":[]}");

	inline FFlockRetryPolicy NoRetry()
	{
		FFlockRetryPolicy Policy;
		Policy.MaxRetries = 0;
		return Policy;
	}

	inline FString TempRoot()
	{
		return FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
			FString::Printf(TEXT("cfg_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	struct FFixture
	{
		FString Dir;
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockHttpClient> Client;
		TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();
		TSharedRef<FFlockAuthSession> Session;
		TSharedPtr<FFlockSnapshotStore> Snapshot;
		TSharedPtr<FFlockConfigProvider> Provider;

		explicit FFixture(bool bWithSnapshot = true, const FString& ExistingDir = FString())
			: Dir(ExistingDir.IsEmpty() ? TempRoot() : ExistingDir)
			, Client(MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>()))
			, Session(MakeShared<FFlockAuthSession>(Client, Store, MakeShared<FFlockNullLogger>(),
				TEXT("http://x/v1"), TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") } }))
		{
			if (bWithSnapshot)
			{
				Snapshot = MakeShared<FFlockSnapshotStore>(Dir, MakeShared<FFlockNullLogger>(), TEXT("9.9.9"));
			}
			Provider = MakeShared<FFlockConfigProvider>(Client, NoRetry(), MakeShared<FFlockNullLogger>(),
				Session, TEXT("http://x/v1"), Snapshot, TEXT("ver-1"));
		}
	};

	inline void Cleanup(const FString& Dir)
	{
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
	}
}

using namespace FlockConfigProviderTestHelpers;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigProviderByIdTest, "Flock.Config.Provider.ConfigByIdAndCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigProviderByIdTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("game_config/cfg-1"), FFlockFakeTransport::Ok(ConfigBody));

	FFlockGameConfigSchema First;
	bool bFirstDone = false;
	Fx.Provider->GetConfigById(TEXT("cfg-1"), [&](TFlockResult<FFlockGameConfigSchema> R)
	{
		bFirstDone = R.bSuccess;
		First = R.Value;
	});
	TestTrue(TEXT("first call succeeds"), bFirstDone);
	TestEqual(TEXT("id parsed"), First.Id, FString(TEXT("cfg-1")));
	int32 Health = 0;
	TestTrue(TEXT("data flattened"), First.Data.TryGetInt(TEXT("MaxHealth"), Health));
	TestEqual(TEXT("data value"), Health, 100);

	// A second ask hits the in-process cache — no second request.
	bool bSecondDone = false;
	Fx.Provider->GetConfigById(TEXT("cfg-1"), [&](TFlockResult<FFlockGameConfigSchema> R) { bSecondDone = R.bSuccess; });
	TestTrue(TEXT("second call succeeds"), bSecondDone);
	TestEqual(TEXT("only one request issued"), Fx.Fake->CountTo(TEXT("game_config/cfg-1")), 1);

	// Empty id is a validation failure with no request.
	bool bFailed = false;
	Fx.Provider->GetConfigById(TEXT(""), [&](TFlockResult<FFlockGameConfigSchema> R) { bFailed = !R.bSuccess; });
	TestTrue(TEXT("empty id fails"), bFailed);

	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigProviderByNameTest, "Flock.Config.Provider.ConfigByName",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigProviderByNameTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("by-name"), FFlockFakeTransport::Ok(ConfigBody));

	bool bDone = false;
	Fx.Provider->GetConfigByName(TEXT("Balance"), [&](TFlockResult<FFlockGameConfigSchema> R) { bDone = R.bSuccess; });
	TestTrue(TEXT("by-name succeeds"), bDone);

	// Second ask resolves through the name->id index into the cached config — no second request.
	Fx.Provider->GetConfigByName(TEXT("Balance"), [&](TFlockResult<FFlockGameConfigSchema> R) {});
	TestEqual(TEXT("one request"), Fx.Fake->CountTo(TEXT("by-name")), 1);

	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigProviderByTagTest, "Flock.Config.Provider.ConfigsByTag",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigProviderByTagTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("game_config"), FFlockFakeTransport::Ok(ConfigListBody));

	TArray<FFlockGameConfigSchema> Configs;
	bool bDone = false;
	Fx.Provider->GetConfigsByTag(EFlockConfigTag::Gameplay, [&](TFlockResult<TArray<FFlockGameConfigSchema>> R)
	{
		bDone = R.bSuccess;
		Configs = R.Value;
	});
	TestTrue(TEXT("by-tag succeeds"), bDone);
	TestEqual(TEXT("one config in list"), Configs.Num(), 1);
	TestTrue(TEXT("tag query present"), Fx.Fake->Requests.Num() > 0 && Fx.Fake->Requests[0].Url.Contains(TEXT("tag=gameplay")));

	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigProviderTagAnyTest, "Flock.Config.Provider.TagAnyOmitsQuery",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigProviderTagAnyTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("game_config"), FFlockFakeTransport::Ok(ConfigListBody));

	Fx.Provider->GetConfigsByTag(EFlockConfigTag::Any, [&](TFlockResult<TArray<FFlockGameConfigSchema>> R) {});
	TestTrue(TEXT("a request was made"), Fx.Fake->Requests.Num() > 0);
	if (Fx.Fake->Requests.Num() > 0)
	{
		// Any means no filter: the query parameter is omitted entirely.
		TestFalse(TEXT("no tag query for Any"), Fx.Fake->Requests[0].Url.Contains(TEXT("tag=")));
	}

	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigProviderResolvePatchTest, "Flock.Config.Provider.ResolveWithPatch",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigProviderResolvePatchTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("game_patch/config"), FFlockFakeTransport::Ok(PatchListBody));

	int32 Health = 0;
	bool bDone = false;
	Fx.Provider->ResolveConfigData(TEXT("cfg-1"), [&](TFlockResult<FFlockGameConfigData> R)
	{
		bDone = R.bSuccess;
		R.Value.TryGetInt(TEXT("MaxHealth"), Health);
	});
	TestTrue(TEXT("resolve succeeds"), bDone);
	TestEqual(TEXT("patch value wins"), Health, 999);
	// The config route is never touched when a patch exists.
	TestEqual(TEXT("no config-by-id call"), Fx.Fake->CountTo(TEXT("game_config/cfg-1")), 0);

	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigProviderResolveFallbackTest, "Flock.Config.Provider.ResolveFallbackToConfig",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigProviderResolveFallbackTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	// No patch on this version -> empty list; resolve falls back to the config's own base data.
	Fx.Fake->On(TEXT("game_patch/config"), FFlockFakeTransport::Ok(EmptyListBody));
	Fx.Fake->On(TEXT("game_config/cfg-1"), FFlockFakeTransport::Ok(ConfigBody));

	int32 Health = 0;
	bool bDone = false;
	Fx.Provider->ResolveConfigData(TEXT("cfg-1"), [&](TFlockResult<FFlockGameConfigData> R)
	{
		bDone = R.bSuccess;
		R.Value.TryGetInt(TEXT("MaxHealth"), Health);
	});
	TestTrue(TEXT("resolve succeeds"), bDone);
	TestEqual(TEXT("base config value used"), Health, 100);

	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigProviderResolveFailTest, "Flock.Config.Provider.ResolveFailsWithNeither",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigProviderResolveFailTest::RunTest(const FString& Parameters)
{
	FFixture Fx(/*bWithSnapshot*/ false); // no cache to fall back on
	Fx.Fake->On(TEXT("game_patch/config"), FFlockFakeTransport::Offline());

	bool bFailed = false;
	Fx.Provider->ResolveConfigData(TEXT("cfg-1"), [&](TFlockResult<FFlockGameConfigData> R) { bFailed = !R.bSuccess; });
	TestTrue(TEXT("resolve fails when the patch fetch fails and nothing is cached"), bFailed);

	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigProviderClearCacheTest, "Flock.Config.Provider.ClearCacheRefetches",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigProviderClearCacheTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("game_config/cfg-1"), FFlockFakeTransport::Ok(ConfigBody));

	Fx.Provider->GetConfigById(TEXT("cfg-1"), [&](TFlockResult<FFlockGameConfigSchema> R) {});
	Fx.Provider->ClearCache();
	Fx.Provider->GetConfigById(TEXT("cfg-1"), [&](TFlockResult<FFlockGameConfigSchema> R) {});
	TestEqual(TEXT("cleared cache forces a second request"), Fx.Fake->CountTo(TEXT("game_config/cfg-1")), 2);

	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigProviderCoalesceTest, "Flock.Config.Provider.CoalescesConcurrent",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigProviderCoalesceTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("game_config/cfg-1"), FFlockFakeTransport::Ok(ConfigBody));
	Fx.Fake->bDeferred = true; // hold completions so both asks are in flight at once

	int32 Completions = 0;
	Fx.Provider->GetConfigById(TEXT("cfg-1"), [&](TFlockResult<FFlockGameConfigSchema> R) { ++Completions; });
	Fx.Provider->GetConfigById(TEXT("cfg-1"), [&](TFlockResult<FFlockGameConfigSchema> R) { ++Completions; });

	// Two concurrent asks, one request in flight.
	TestEqual(TEXT("one request for two concurrent asks"), Fx.Fake->CountTo(TEXT("game_config/cfg-1")), 1);
	TestEqual(TEXT("neither has completed yet"), Completions, 0);

	Fx.Fake->FlushPending();
	TestEqual(TEXT("both callers get the result"), Completions, 2);

	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigProviderSnapshotFallbackTest, "Flock.Config.Provider.SnapshotFallbackOnFailure",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigProviderSnapshotFallbackTest::RunTest(const FString& Parameters)
{
	const FString Dir = TempRoot();
	{
		// Warm the snapshot with a success.
		FFixture Warm(true, Dir);
		Warm.Fake->On(TEXT("game_config/cfg-1"), FFlockFakeTransport::Ok(ConfigBody));
		Warm.Provider->GetConfigById(TEXT("cfg-1"), [&](TFlockResult<FFlockGameConfigSchema> R) {});
	}
	{
		// A fresh provider over the same store, transport failing: one attempt, then serve the snapshot.
		FFixture Cold(true, Dir);
		Cold.Fake->On(TEXT("game_config/cfg-1"), FFlockFakeTransport::Offline());
		int32 Health = 0;
		bool bDone = false;
		Cold.Provider->GetConfigById(TEXT("cfg-1"), [&](TFlockResult<FFlockGameConfigSchema> R)
		{
			bDone = R.bSuccess;
			R.Value.Data.TryGetInt(TEXT("MaxHealth"), Health);
		});
		TestTrue(TEXT("served from snapshot despite the failure"), bDone);
		TestEqual(TEXT("snapshot value intact"), Health, 100);
		TestEqual(TEXT("one attempt was made"), Cold.Fake->CountTo(TEXT("game_config/cfg-1")), 1);
	}
	Cleanup(Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigProviderUnreachableTest, "Flock.Config.Provider.UnreachableServesCacheNoCall",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigProviderUnreachableTest::RunTest(const FString& Parameters)
{
	const FString Dir = TempRoot();
	{
		FFixture Warm(true, Dir);
		Warm.Fake->On(TEXT("game_config/cfg-1"), FFlockFakeTransport::Ok(ConfigBody));
		Warm.Provider->GetConfigById(TEXT("cfg-1"), [&](TFlockResult<FFlockGameConfigSchema> R) {});
	}
	{
		FFixture Cold(true, Dir);
		Cold.Provider->SetReachabilityProbe([]() { return false; }); // forced offline
		Cold.Fake->On(TEXT("game_config/cfg-1"), FFlockFakeTransport::Ok(ConfigBody));
		bool bDone = false;
		Cold.Provider->GetConfigById(TEXT("cfg-1"), [&](TFlockResult<FFlockGameConfigSchema> R) { bDone = R.bSuccess; });
		TestTrue(TEXT("served from cache while offline"), bDone);
		TestEqual(TEXT("no network call at all"), Cold.Fake->CountTo(TEXT("game_config/cfg-1")), 0);
	}
	Cleanup(Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigProviderPermanentTest, "Flock.Config.Provider.PermanentPropagatesWithCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigProviderPermanentTest::RunTest(const FString& Parameters)
{
	const FString Dir = TempRoot();
	{
		FFixture Warm(true, Dir);
		Warm.Fake->On(TEXT("game_config/cfg-1"), FFlockFakeTransport::Ok(ConfigBody));
		Warm.Provider->GetConfigById(TEXT("cfg-1"), [&](TFlockResult<FFlockGameConfigSchema> R) {});
	}
	{
		// A 404 is an authoritative answer (config deleted) — it propagates even though a snapshot exists.
		FFixture Cold(true, Dir);
		Cold.Fake->On(TEXT("game_config/cfg-1"), FFlockFakeTransport::Status(404, TEXT("{}")));
		bool bFailed = false;
		Cold.Provider->GetConfigById(TEXT("cfg-1"), [&](TFlockResult<FFlockGameConfigSchema> R) { bFailed = !R.bSuccess; });
		TestTrue(TEXT("permanent failure propagates past the cache"), bFailed);
	}
	Cleanup(Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockConfigProviderPlayerFeaturesNoDiskTest, "Flock.Config.Provider.PlayerFeaturesNotSnapshotted",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockConfigProviderPlayerFeaturesNoDiskTest::RunTest(const FString& Parameters)
{
	const FString Dir = TempRoot();
	{
		FFixture Warm(true, Dir);
		Warm.Fake->On(TEXT("player/p1/features"), FFlockFakeTransport::Ok(ConfigBody));
		bool bDone = false;
		Warm.Provider->GetPlayerFeatures(TEXT("p1"), [&](TFlockResult<FFlockGameConfigSchema> R) { bDone = R.bSuccess; });
		TestTrue(TEXT("player features fetched"), bDone);
	}
	{
		// A fresh provider over the same store, forced offline: nothing was snapshotted, so it must fail
		// rather than serve stale features.
		FFixture Cold(true, Dir);
		Cold.Provider->SetReachabilityProbe([]() { return false; });
		Cold.Fake->On(TEXT("player/p1/features"), FFlockFakeTransport::Offline());
		bool bFailed = false;
		Cold.Provider->GetPlayerFeatures(TEXT("p1"), [&](TFlockResult<FFlockGameConfigSchema> R) { bFailed = !R.bSuccess; });
		TestTrue(TEXT("player features were never written to disk"), bFailed);
	}
	Cleanup(Dir);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
