// Copyright 2022, Qwacks. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_AUTOMATION_TESTS

#include "Auth/FlockAuthSession.h"
#include "FlockLogger.h"
#include "HAL/FileManager.h"
#include "Http/FlockHttpClient.h"
#include "Http/FlockSnapshotStore.h"
#include "Misc/Paths.h"
#include "Providers/FlockGameProvider.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"

namespace FlockGameProviderTestHelpers
{
	const TCHAR* const GameBody =
		TEXT("{\"result\":{\"id\":\"game-1\",\"name\":\"Ducks\",\"read_me\":\"hi\",\"stage\":\"launch\",")
		TEXT("\"studio_id\":\"studio-1\",\"created_at\":\"\",\"updated_at\":\"\",\"deleted_at\":null}}");

	const TCHAR* const VersionBody =
		TEXT("{\"result\":{\"id\":\"ver-1\",\"name\":\"1.0\",\"release_type\":\"prod\",\"env\":\"production\",")
		TEXT("\"created_at\":\"\",\"updated_at\":\"\"}}");

	inline FFlockRetryPolicy NoRetry()
	{
		FFlockRetryPolicy Policy;
		Policy.MaxRetries = 0;
		return Policy;
	}

	inline FString TempRoot()
	{
		return FPaths::Combine(FPaths::ProjectIntermediateDir(), TEXT("FlockTests"),
			FString::Printf(TEXT("game_%s"), *FGuid::NewGuid().ToString(EGuidFormats::Digits)));
	}

	struct FFixture
	{
		FString Dir;
		FString ApiUrl;
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockHttpClient> Client;
		TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();
		TSharedRef<FFlockAuthSession> Session;
		TSharedPtr<FFlockSnapshotStore> Snapshot;
		TSharedPtr<FFlockGameProvider> Provider;

		explicit FFixture(const FString& ExistingDir = FString(), const FString& InApiUrl = TEXT("http://x/v1"))
			: Dir(ExistingDir.IsEmpty() ? TempRoot() : ExistingDir)
			, ApiUrl(InApiUrl)
			, Client(MakeShared<FFlockHttpClient>(Fake, MakeShared<FFlockNullLogger>()))
			, Session(MakeShared<FFlockAuthSession>(Client, Store, MakeShared<FFlockNullLogger>(),
				InApiUrl, TMap<FString, FString>{ { TEXT("X-Flock-API-Key"), TEXT("k") } }))
		{
			Snapshot = MakeShared<FFlockSnapshotStore>(Dir, MakeShared<FFlockNullLogger>(), TEXT("9.9.9"));
			Provider = MakeShared<FFlockGameProvider>(Client, NoRetry(), MakeShared<FFlockNullLogger>(),
				Session, InApiUrl, Snapshot, TEXT("ver-1"));
		}
	};

	inline void Cleanup(const FString& Dir)
	{
		IFileManager::Get().DeleteDirectory(*Dir, false, true);
	}
}

using namespace FlockGameProviderTestHelpers;

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockGameProviderGameTest, "Flock.Game.Provider.GameAndCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockGameProviderGameTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("/game"), FFlockFakeTransport::Ok(GameBody));

	FFlockGameSchema Game;
	bool bDone = false;
	Fx.Provider->GetGame([&](TFlockResult<FFlockGameSchema> R) { bDone = R.bSuccess; Game = R.Value; });
	TestTrue(TEXT("game succeeds"), bDone);
	TestEqual(TEXT("id parsed"), Game.Id, FString(TEXT("game-1")));
	TestEqual(TEXT("read_me -> ReadMe"), Game.ReadMe, FString(TEXT("hi")));
	TestEqual(TEXT("studio_id -> StudioId"), Game.StudioId, FString(TEXT("studio-1")));

	Fx.Provider->GetGame([&](TFlockResult<FFlockGameSchema> R) {});
	TestEqual(TEXT("cache hit, one request"), Fx.Fake->CountTo(TEXT("/game")), 1);

	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockGameProviderVersionTest, "Flock.Game.Provider.VersionAndCache",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockGameProviderVersionTest::RunTest(const FString& Parameters)
{
	FFixture Fx;
	Fx.Fake->On(TEXT("game_version"), FFlockFakeTransport::Ok(VersionBody));

	FFlockGameVersionSchema Version;
	bool bDone = false;
	Fx.Provider->GetGameVersion([&](TFlockResult<FFlockGameVersionSchema> R) { bDone = R.bSuccess; Version = R.Value; });
	TestTrue(TEXT("version succeeds"), bDone);
	TestEqual(TEXT("id parsed"), Version.Id, FString(TEXT("ver-1")));
	TestEqual(TEXT("release_type -> ReleaseType"), Version.ReleaseType, FString(TEXT("prod")));

	Fx.Provider->GetGameVersion([&](TFlockResult<FFlockGameVersionSchema> R) {});
	TestEqual(TEXT("cache hit, one request"), Fx.Fake->CountTo(TEXT("game_version")), 1);

	Cleanup(Fx.Dir);
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockGameProviderByNameTest, "Flock.Game.Provider.VersionByNameBootstrapScope",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockGameProviderByNameTest::RunTest(const FString& Parameters)
{
	const FString Dir = TempRoot();
	{
		// Warm the by-name lookup under BootstrapScope.
		FFixture Warm(Dir, TEXT("http://a/v1"));
		Warm.Fake->On(TEXT("by-name"), FFlockFakeTransport::Ok(VersionBody));
		bool bDone = false;
		Warm.Provider->GetGameVersionByName(TEXT("release"), [&](TFlockResult<FFlockGameVersionSchema> R) { bDone = R.bSuccess; });
		TestTrue(TEXT("by-name succeeds"), bDone);

		// The snapshot landed under the reserved bootstrap scope, not under a version folder.
		const FString BootstrapDir = FPaths::Combine(Dir, FFlockSnapshotStore::BootstrapScope);
		TArray<FString> Files;
		IFileManager::Get().FindFiles(Files, *FPaths::Combine(BootstrapDir, TEXT("*.json")), true, false);
		TestTrue(TEXT("a bootstrap snapshot was written"), Files.Num() >= 1);
	}
	{
		// Same API URL + same store, forced offline: the bootstrap-scoped snapshot is served.
		FFixture Same(Dir, TEXT("http://a/v1"));
		Same.Provider->SetReachabilityProbe([]() { return false; });
		Same.Fake->On(TEXT("by-name"), FFlockFakeTransport::Ok(VersionBody));
		bool bServed = false;
		Same.Provider->GetGameVersionByName(TEXT("release"), [&](TFlockResult<FFlockGameVersionSchema> R) { bServed = R.bSuccess; });
		TestTrue(TEXT("served from bootstrap snapshot offline"), bServed);
		TestEqual(TEXT("no network call"), Same.Fake->CountTo(TEXT("by-name")), 0);
	}
	{
		// A DIFFERENT API URL over the same store must not share the entry (key includes the URL).
		FFixture Other(Dir, TEXT("http://b/v1"));
		Other.Provider->SetReachabilityProbe([]() { return false; });
		Other.Fake->On(TEXT("by-name"), FFlockFakeTransport::Offline());
		bool bFailed = false;
		Other.Provider->GetGameVersionByName(TEXT("release"), [&](TFlockResult<FFlockGameVersionSchema> R) { bFailed = !R.bSuccess; });
		TestTrue(TEXT("distinct API URL does not reuse the by-name entry"), bFailed);
	}
	Cleanup(Dir);
	return true;
}

#endif // WITH_AUTOMATION_TESTS
