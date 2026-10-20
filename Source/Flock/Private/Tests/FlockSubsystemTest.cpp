// Copyright 2022, Qwacks. Licensed under the MIT License - see LICENSE.md.

#include "Misc/AutomationTest.h"
#include "Misc/FlockEngineCompat.h"

#if WITH_AUTOMATION_TESTS

#include "FlockSubsystem.h"
#include "FlockEvents.h"
#include "FlockInitConfig.h"
#include "Engine/GameInstance.h"
#include "Misc/Base64.h"
#include "Tests/Support/FlockEventTestListener.h"
#include "Tests/Support/FlockFakeTransport.h"
#include "Tests/Support/FlockMemoryTokenStore.h"
#include "Tests/Support/FlockTestSdk.h"
#include "Config/FlockConfig.h"
#include "Misc/ScopeExit.h"
#include "Providers/FlockAnalyticsProvider.h"
#include "Providers/FlockAuthProvider.h"
#include "Http/FlockSnapshotStore.h"
#include "UObject/Package.h"

namespace
{
	FFlockInitConfig MakeValidConfig()
	{
		FFlockInitConfig Config;
		Config.ApiUrl = TEXT("https://api-flock.qwacks.com");
		Config.ApiKey = TEXT("secret");
		Config.GameId = TEXT("my-game");
		Config.GameVersion = TEXT("1.2.3");
		Config.GameVersionId = TEXT("ver-abc");
		return Config;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSubsystemInitGateTest, "Flock.Runtime.Subsystem.InitGate",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSubsystemInitGateTest::RunTest(const FString& Parameters)
{
	// The clean-failure path logs an Error on purpose; tell the framework to expect it.
	AddExpectedError(TEXT("Initialize failed"), EAutomationExpectedErrorFlags::Contains, 1);

	const FFlockTestSdk Test;
	UFlockSubsystem* Sdk = Test.Sdk;

	// Missing baked version ID -> clean failure, stays uninitialized.
	FFlockInitConfig NoVersion = MakeValidConfig();
	NoVersion.GameVersionId = TEXT("");
	Sdk->InitializeWithConfig(NoVersion);
	TestFalse(TEXT("Not initialized without a baked version ID"), Sdk->IsInitialized());
	TestFalse(TEXT("Error is recorded on failed init"), Sdk->GetInitializationError().IsEmpty());

	// Valid config -> initialized, error cleared, getters populated.
	Sdk->InitializeWithConfig(MakeValidConfig());
	TestTrue(TEXT("Initialized with a baked version ID"), Sdk->IsInitialized());
	TestTrue(TEXT("Error is cleared on success"), Sdk->GetInitializationError().IsEmpty());
	TestEqual(TEXT("GameVersionId is exposed"), Sdk->GetGameVersionId(), FString(TEXT("ver-abc")));
	TestEqual(TEXT("Versioned URL appends /v1"), Sdk->GetVersionedApiUrl(),
		FString(TEXT("https://api-flock.qwacks.com/v1")));

	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSubsystemReinitTest, "Flock.Runtime.Subsystem.ReinitAndShutdown",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSubsystemReinitTest::RunTest(const FString& Parameters)
{
	const FFlockTestSdk Test;
	UFlockSubsystem* Sdk = Test.Sdk;
	Sdk->InitializeWithConfig(MakeValidConfig());
	TestTrue(TEXT("Initialized"), Sdk->IsInitialized());

	// Double-init is a no-op guard; the first config is kept.
	FFlockInitConfig Other = MakeValidConfig();
	Other.GameVersionId = TEXT("ver-xyz");
	Sdk->InitializeWithConfig(Other);
	TestEqual(TEXT("Double-init is ignored (keeps first config)"), Sdk->GetGameVersionId(),
		FString(TEXT("ver-abc")));

	// Shutdown allows re-init with the new config.
	Sdk->ShutdownSdk();
	TestFalse(TEXT("Not initialized after shutdown"), Sdk->IsInitialized());
	Sdk->InitializeWithConfig(Other);
	TestTrue(TEXT("Re-initialized after shutdown"), Sdk->IsInitialized());
	TestEqual(TEXT("New config adopted after shutdown"), Sdk->GetGameVersionId(),
		FString(TEXT("ver-xyz")));

	return true;
}

namespace FlockSubsystemAuthTestHelpers
{
	inline FString Base64Url(const FString& In)
	{
		FString Encoded = FBase64::Encode(In);
		Encoded.ReplaceInline(TEXT("+"), TEXT("-"));
		Encoded.ReplaceInline(TEXT("/"), TEXT("_"));
		Encoded.ReplaceInline(TEXT("="), TEXT(""));
		return Encoded;
	}

	inline FString MakeJwt(const FString& PlayerId, int64 ExpiryOffsetSeconds = 3600)
	{
		const int64 Exp = FDateTime::UtcNow().ToUnixTimestamp() + ExpiryOffsetSeconds;
		const FString Payload = FString::Printf(TEXT("{\"sub\":\"%s\",\"exp\":%lld}"), *PlayerId, Exp);
		return FString::Printf(TEXT("h.%s.s"), *Base64Url(Payload));
	}

	struct FSubsystemAuthFixture
	{
		FFlockTestSdk Test;
		UFlockSubsystem* Sdk = Test.Sdk;
		TSharedRef<FFlockFakeTransport> Fake = MakeShared<FFlockFakeTransport>();
		TSharedRef<FFlockMemoryTokenStore> Store = MakeShared<FFlockMemoryTokenStore>();
		UFlockEventTestListener* Listener = nullptr;

		FSubsystemAuthFixture()
		{
			Sdk->SetHttpAdapterForTesting(Fake);
			Sdk->SetTokenStoreForTesting(Store);
			Listener = NewObject<UFlockEventTestListener>();
			Sdk->GetEvents()->OnAuthenticated.AddDynamic(Listener, &UFlockEventTestListener::HandleAuthenticated);
			Sdk->GetEvents()->OnSessionRestored.AddDynamic(Listener, &UFlockEventTestListener::HandleSessionRestored);
		}
	};
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSubsystemAuthWiringTest, "Flock.Runtime.Subsystem.AuthWiring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSubsystemAuthWiringTest::RunTest(const FString& Parameters)
{
	using namespace FlockSubsystemAuthTestHelpers;

	// Before init: no provider, safe no-op Logout, signed-out getters.
	{
		FSubsystemAuthFixture F;
		TestNull(TEXT("no provider before init"), F.Sdk->GetAuthProvider());
		TestFalse(TEXT("not authenticated"), F.Sdk->IsAuthenticated());
		TestTrue(TEXT("player id empty"), F.Sdk->GetPlayerId().IsEmpty());
		F.Sdk->Logout(); // must not crash
	}
	// Init wires the auth stack; shutdown drops it.
	{
		FSubsystemAuthFixture F;
		F.Sdk->InitializeWithConfig(MakeValidConfig());
		TestTrue(TEXT("initialized"), F.Sdk->IsInitialized());
		TestNotNull(TEXT("provider wired"), F.Sdk->GetAuthProvider());
		TestFalse(TEXT("no session -> not authenticated"), F.Sdk->IsAuthenticated());
		TestEqual(TEXT("restore attempted (event fired false)"), F.Listener->SessionRestoredCount, 1);
		TestFalse(TEXT("nothing restored"), F.Listener->bLastSessionRestored);

		F.Sdk->ShutdownSdk();
		TestNull(TEXT("provider dropped"), F.Sdk->GetAuthProvider());
		TestFalse(TEXT("signed-out getters safe"), F.Sdk->IsAuthenticated());
	}
	// A persisted session is auto-restored on init and surfaces through the subsystem getters.
	{
		FSubsystemAuthFixture F;
		F.Store->bHasTokens = true;
		F.Store->Stored.AccessToken = MakeJwt(TEXT("p-42"));
		F.Store->Stored.RefreshToken = TEXT("r-42");
		F.Store->Stored.AuthMethod = EFlockAuthMethod::Device;

		F.Sdk->InitializeWithConfig(MakeValidConfig());

		TestTrue(TEXT("restored"), F.Sdk->IsAuthenticated());
		TestEqual(TEXT("player id"), F.Sdk->GetPlayerId(), FString(TEXT("p-42")));
		TestFalse(TEXT("restore finished"), F.Sdk->IsRestoringSession());
		TestEqual(TEXT("authenticated event"), F.Listener->AuthenticatedCount, 1);
		TestEqual(TEXT("via session-restore"), static_cast<int32>(F.Listener->LastAuthInfo.Method),
			static_cast<int32>(EFlockAuthMethod::SessionRestore));

		// Logout through the subsystem clears the restored session.
		F.Sdk->Logout();
		TestFalse(TEXT("logged out"), F.Sdk->IsAuthenticated());
		TestFalse(TEXT("store cleared"), F.Store->bHasTokens);

		F.Sdk->ShutdownSdk();
	}
	return true;
}

/**
 * A sign-in hands analytics its player whatever the auto-start setting: events recorded before sign-in belong to that
 * player either way. Only opening a session waits on auto-start.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockAnalyticsSubsystemWiringTest, "Flock.Analytics.Subsystem.AuthWiring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockAnalyticsSubsystemWiringTest::RunTest(const FString& Parameters)
{
	using namespace FlockSubsystemAuthTestHelpers;

	UFlockConfig* Settings = GetMutableDefault<UFlockConfig>();
	const bool bAutoStartWas = Settings->bAnalyticsAutoStartSession;
	Settings->bAnalyticsAutoStartSession = false;
	ON_SCOPE_EXIT
	{
		Settings->bAnalyticsAutoStartSession = bAutoStartWas;
	};

	FSubsystemAuthFixture F;
	F.Sdk->InitializeWithConfig(MakeValidConfig());
	FFlockAnalyticsProvider* Analytics = F.Sdk->GetAnalyticsProvider();
	TestNotNull(TEXT("analytics wired"), Analytics);
	if (Analytics == nullptr)
	{
		F.Sdk->ShutdownSdk();
		return false;
	}
	// The spool is in the test SDK's own folder, so it starts empty.
	TestTrue(TEXT("accepted while signed out"), F.Sdk->TrackAnalyticsEvent(TEXT("before_sign_in"), FFlockCommandData(), FString()));
	TestEqual(TEXT("held"), Analytics->GetPendingAnalyticsEventCount(), 1);

	// A real sign-in rather than a raised event: gameplay events are delivered only while a player is signed in. A
	// stored session restored now raises OnAuthenticated the way a login does.
	F.Store->bHasTokens = true;
	F.Store->Stored.AccessToken = MakeJwt(TEXT("p-wired"));
	F.Store->Stored.RefreshToken = TEXT("r-wired");
	F.Store->Stored.AuthMethod = EFlockAuthMethod::Device;
	FFlockAuthProvider* Auth = F.Sdk->GetAuthProvider();
	TestNotNull(TEXT("auth wired"), Auth);
	if (Auth != nullptr)
	{
		Auth->TryRestoreSession([](bool) {});
	}
	TestTrue(TEXT("precondition: signed in"), F.Sdk->IsAuthenticated());

	TestFalse(TEXT("auto-start off: no session opened"), Analytics->HasActiveSession());
	TestEqual(TEXT("but the held event was sent"), F.Fake->CountTo(TEXT("analytics/events")), 1);
	const FFlockHttpRequest* Sent = F.Fake->Requests.FindByPredicate(
		[](const FFlockHttpRequest& Request) { return Request.Url.Contains(TEXT("analytics/events")); });
	TestTrue(TEXT("attributed to the player who signed in"),
		Sent != nullptr && Sent->JsonBody.Contains(TEXT("\"player_id\":\"p-wired\""), ESearchCase::CaseSensitive));
	TestEqual(TEXT("nothing left held"), Analytics->GetPendingAnalyticsEventCount(), 0);

	TestFalse(TEXT("the subsystem refuses the reserved name too"),
		F.Sdk->TrackAnalyticsEvent(TEXT("session_started"), FFlockCommandData(), FString()));

	F.Sdk->ShutdownSdk();
	return true;
}

/**
 * A sign-in the server refuses to refresh ends as a sign-out does: the analytics session closes and the player's cached
 * rows go, before the tokens are cleared, so nothing carries on in the name of a player who is no longer signed in.
 */
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSubsystemExpiredSignInEndsAsASignOutDoesTest, "Flock.Runtime.Subsystem.ExpiredSignInEndsAsASignOutDoes",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSubsystemExpiredSignInEndsAsASignOutDoesTest::RunTest(const FString& Parameters)
{
	using namespace FlockSubsystemAuthTestHelpers;

	UFlockConfig* Settings = GetMutableDefault<UFlockConfig>();
	const bool bAutoStartWas = Settings->bAnalyticsAutoStartSession;
	Settings->bAnalyticsAutoStartSession = true;
	ON_SCOPE_EXIT
	{
		Settings->bAnalyticsAutoStartSession = bAutoStartWas;
	};

	FSubsystemAuthFixture F;
	F.Store->bHasTokens = true;
	F.Store->Stored.AccessToken = MakeJwt(TEXT("p-exp"));
	F.Store->Stored.RefreshToken = TEXT("r-exp");
	F.Store->Stored.AuthMethod = EFlockAuthMethod::Device;
	F.Fake->On(TEXT("v1/player_data"), FFlockFakeTransport::Ok(TEXT(
		"{\"items\":[{\"id\":\"pd-1\",\"player_template_id\":\"tmpl-1\",\"game_id\":\"g\",\"player_id\":\"p-exp\","
		"\"data\":[{\"type\":\"int\",\"field_name\":\"coins\",\"value\":5}],\"created_at\":\"\",\"updated_at\":\"\"}],"
		"\"total\":1,\"page\":1,\"limit\":100}")));
	F.Sdk->InitializeWithConfig(MakeValidConfig());

	FFlockAnalyticsProvider* Analytics = F.Sdk->GetAnalyticsProvider();
	FFlockPlayerProvider* Players = F.Sdk->GetPlayerProvider();
	FFlockAuthProvider* Auth = F.Sdk->GetAuthProvider();
	if (!TestNotNull(TEXT("analytics wired"), Analytics) || !TestNotNull(TEXT("players wired"), Players) || !TestNotNull(TEXT("auth wired"), Auth))
	{
		F.Sdk->ShutdownSdk();
		return false;
	}
	TestTrue(TEXT("precondition: signed in"), F.Sdk->IsAuthenticated());
	TestTrue(TEXT("precondition: a session is running"), Analytics->HasActiveSession());
	bool bRead = false;
	Players->GetMyDataByTemplate(TEXT("tmpl-1"), [&bRead](TFlockResult<FFlockPlayerData> Result) { bRead = Result.bSuccess; });
	FFlockPlayerData Row;
	TestTrue(TEXT("precondition: the player's row is cached"), bRead && Players->TryGetCachedRow(TEXT("pd-1"), Row));

	UFlockEventTestListener* Expired = NewObject<UFlockEventTestListener>();
	F.Sdk->GetEvents()->OnAuthExpired.AddDynamic(Expired, &UFlockEventTestListener::HandleAuthExpired);
	F.Fake->On(TEXT("token/refresh"), FFlockFakeTransport::Status(401, TEXT("")));
	bool bRefreshed = true;
	Auth->RefreshToken([&bRefreshed](bool bOk) { bRefreshed = bOk; });

	TestFalse(TEXT("the refresh was refused"), bRefreshed);
	TestEqual(TEXT("the game hears the sign-in expired"), Expired->AuthExpiredCount, 1);
	TestFalse(TEXT("signed out"), F.Sdk->IsAuthenticated());
	TestFalse(TEXT("the analytics session ended with it"), Analytics->HasActiveSession());
	TestFalse(TEXT("and the player's cached row is gone"), Players->TryGetCachedRow(TEXT("pd-1"), Row));

	F.Sdk->ShutdownSdk();
	return true;
}

// ── A pasted API URL with a space or a trailing slash reaches the same routes as the editor's tools ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSubsystemTrimsTheApiUrlTest, "Flock.Runtime.Subsystem.TrimsTheApiUrl",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSubsystemTrimsTheApiUrlTest::RunTest(const FString& Parameters)
{
	FFlockTestSdk Test;
	FFlockInitConfig Config = MakeValidConfig();
	Config.ApiUrl = TEXT(" https://api.example.test// ");
	Test->InitializeWithConfig(Config);
	TestEqualSensitive(TEXT("the base has no space or trailing slash"), Test->GetApiUrl(), FString(TEXT("https://api.example.test")));
	TestEqualSensitive(TEXT("so the version joins it with one slash"), Test->GetVersionedApiUrl(), FString(TEXT("https://api.example.test/v1")));
	TestEqualSensitive(TEXT("a clean URL is kept as it is"), FFlockInitConfig::TrimApiUrl(TEXT("http://127.0.0.1:8001")), FString(TEXT("http://127.0.0.1:8001")));
	Test->ShutdownSdk();
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockLeaderboardSubsystemWiringTest, "Flock.Leaderboard.Subsystem.Wiring",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockLeaderboardSubsystemWiringTest::RunTest(const FString& Parameters)
{
	const FFlockTestSdk Test;
	UFlockSubsystem* Sdk = Test.Sdk;

	// Safe to ask for before init — a graph that resolves the SDK early must get null, not a crash.
	TestNull(TEXT("no provider before init"), Sdk->GetLeaderboardProvider());

	Sdk->InitializeWithConfig(MakeValidConfig());
	FFlockLeaderboardProvider* Provider = Sdk->GetLeaderboardProvider();
	TestNotNull(TEXT("provider built at init"), Provider);

	if (Provider != nullptr)
	{
		// Without this the offline branch degrades to always-reachable, so the "serve cache without a
		// call" path could never fire in a shipped game — and no offline test would notice, because the
		// provider tests install their own probe.
		const bool bProbeWired = static_cast<bool>(Provider->GetReachabilityProbe());
		TestTrue(TEXT("reachability probe wired to the client's offline latch"), bProbeWired);
	}

	// Logout drops the player-scoped rank snapshots; it must tolerate being called with no player signed in.
	Sdk->Logout();
	TestNotNull(TEXT("provider survives logout"), Sdk->GetLeaderboardProvider());

	Sdk->ShutdownSdk();
	TestNull(TEXT("provider gone after shutdown"), Sdk->GetLeaderboardProvider());
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSubsystemRequestHeadersTest, "Flock.Runtime.Subsystem.RequestHeadersFollowInitialization",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSubsystemRequestHeadersTest::RunTest(const FString& Parameters)
{
	const FFlockTestSdk Test;
	UFlockSubsystem* Sdk = Test.Sdk;
	TestEqual(TEXT("No headers before initialization"), Sdk->GetRequestHeaders().Num(), 0);

	Sdk->InitializeWithConfig(MakeValidConfig());
	const TMap<FString, FString> Headers = Sdk->GetRequestHeaders();
	TestEqual(TEXT("The API key the SDK initialized with"), Headers.FindRef(TEXT("X-Flock-API-Key")), FString(TEXT("secret")));
	TestEqual(TEXT("The version id the SDK initialized with"), Headers.FindRef(TEXT("X-Game-Version-ID")), FString(TEXT("ver-abc")));

	Sdk->ShutdownSdk();
	TestEqual(TEXT("No headers after shutdown"), Sdk->GetRequestHeaders().Num(), 0);
	return true;
}

// ── Start-up rescues notification state an earlier build kept under its game version, before the prune deletes it ──
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSubsystemRescuesNotificationStateTest, "Flock.Runtime.Subsystem.RescuesNotificationStateBeforeTheVersionPrune",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSubsystemRescuesNotificationStateTest::RunTest(const FString& Parameters)
{
	UFlockConfig* Settings = GetMutableDefault<UFlockConfig>();
	const bool bCacheWas = Settings->bEnableOfflineCache;
	Settings->bEnableOfflineCache = true;
	ON_SCOPE_EXIT
	{
		Settings->bEnableOfflineCache = bCacheWas;
	};

	FFlockTestSdk Test;
	const FString Root = FPaths::Combine(Test.Folder, TEXT("snapshots"));
	{
		FFlockSnapshotStore Earlier(Root, MakeShared<FFlockNullLogger>(), TEXT("1.27.0"));
		Earlier.Write(TEXT("old-version/notification_state/player-a"), TEXT("pending_schedules"), TEXT("[{\"Id\":\"sch-1\"}]"));
	}
	Test->InitializeWithConfig(MakeValidConfig());
	Test->ShutdownSdk();

	FFlockSnapshotStore After(Root, MakeShared<FFlockNullLogger>(), TEXT("1.28.0"));
	FString Payload;
	TestTrue(TEXT("The reminder list is kept outside every version's tree"),
		After.TryRead(TEXT("_state/notification_state/player-a"), TEXT("pending_schedules"), Payload));
	TestFalse(TEXT("Not left where the prune deletes it"),
		IFileManager::Get().DirectoryExists(*FPaths::Combine(Root, TEXT("old-version"))));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FFlockSubsystemRefusesAnEmptyApiUrlTest, "Flock.Runtime.Subsystem.RefusesAnApiUrlOfNothing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FFlockSubsystemRefusesAnEmptyApiUrlTest::RunTest(const FString& Parameters)
{
	AddExpectedError(TEXT("Initialize failed"), EAutomationExpectedErrorFlags::Contains, 1);
	FFlockTestSdk Test;
	FFlockInitConfig Config = MakeValidConfig();
	// Spaces and slashes alone trim to nothing, which would send every request to "/v1/...".
	Config.ApiUrl = TEXT("  // ");
	Test->InitializeWithConfig(Config);
	TestFalse(TEXT("The SDK does not initialize on a URL of nothing"), Test->IsInitialized());
	TestTrue(TEXT("And says so"), Test->GetInitializationError().Contains(TEXT("API URL is empty"), ESearchCase::CaseSensitive));
	return true;
}

#endif // WITH_AUTOMATION_TESTS
